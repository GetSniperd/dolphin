// Unit tests for Core/Slippi/Rev3NetLink (rollback netplay, milestone M3): the PAD / PAD_ACK link, the reliable message envelope and inbox,
// peer address and environment parsing, the HELLO items and the arm gate. All pure: no sockets, no clock (every call takes the time), no
// Dolphin core, no game data. Run: build target `tests`, then `tests --gtest_filter=Rev3NetLink*`.
//
// The PadLink tests run two links against each other through a small in-memory "wire" (a list of datagrams the test delivers, drops or
// reorders by hand), which is what Rev3Net.cpp's ENet threads do with real packets.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Slippi/Rev3NetLink.h"

namespace
{
using namespace Rev3;
using namespace Rev3::Net;

Pad8 P(u8 a)
{
  return Pad8{a, static_cast<u8>(a + 1), static_cast<u8>(a + 2), 0, 0, 0, 0, 0};
}

// Delivers `packet` to `to` at `now`; returns the reply datagram (possibly empty).
std::vector<u8> Deliver(PadLink& to, const std::vector<u8>& packet, u64 now)
{
  std::vector<u8> reply;
  EXPECT_TRUE(to.OnPacket(packet.data(), packet.size(), now, &reply));
  return reply;
}

u32 BE32(const std::vector<u8>& v, size_t at)
{
  return (u32(v[at]) << 24) | (u32(v[at + 1]) << 16) | (u32(v[at + 2]) << 8) | v[at + 3];
}

GetEnvFn EnvOf(std::map<std::string, std::string> vars)
{
  return [vars](const char* name) {
    const auto it = vars.find(name);
    return it == vars.end() ? std::string() : it->second;
  };
}

HelloInputs SampleInputs()
{
  HelloInputs in;
  in.build = "rev3-net-test";
  in.game_id = "RNEEDA";
  in.cpu_core = "JIT64";
  in.rtc_enabled = true;
  in.rtc_value = 1577836800;
  in.wiimote_sources = {1, 0, 0, 0, 0};
  in.hook_crc_valid = true;
  in.hook_crc = 0xA1B2C3D4;
  return in;
}
}  // namespace

// ---- PadLink: wire layout ----------------------------------------------------------------------------------------------------------

TEST(Rev3NetLink, PadPacketLayoutIsSlippis)
{
  PadLink host(0);
  // Hook frame 0 is wire frame 1. Header: id 0x80, s32 newest wire frame, u8 port, s32 checksum frame, u32 checksum; then 8-byte pads
  // newest first.
  const std::vector<u8> first = host.QueueLocal(0, P(10), -1, 0, 1000);
  ASSERT_EQ(first.size(), PAD_HEADER_BYTES + PAD_BYTES);
  EXPECT_EQ(first[0], 0x80);
  EXPECT_EQ(BE32(first, 1), 1u);
  EXPECT_EQ(first[5], 0);
  EXPECT_EQ(BE32(first, 6), 0xFFFFFFFFu);  // no checksum yet
  EXPECT_EQ(BE32(first, 10), 0u);
  EXPECT_EQ(first[14], 10);

  const std::vector<u8> second = host.QueueLocal(1, P(20), 30, 0xCAFEF00D, 2000);
  ASSERT_EQ(second.size(), PAD_HEADER_BYTES + 2 * PAD_BYTES);
  EXPECT_EQ(BE32(second, 1), 2u);
  EXPECT_EQ(BE32(second, 6), 30u);
  EXPECT_EQ(BE32(second, 10), 0xCAFEF00Du);
  EXPECT_EQ(second[14], 20);  // newest first
  EXPECT_EQ(second[14 + 8], 10);

  PadLink guest(1);
  std::vector<u8> ack = Deliver(guest, second, 2500);
  ASSERT_EQ(ack.size(), ACK_BYTES);
  EXPECT_EQ(ack[0], 0x81);
  EXPECT_EQ(BE32(ack, 1), 2u);  // acks wire frame 2 = hook frame 1
  EXPECT_EQ(ack[5], 1);         // the sender's (guest's) own port
  EXPECT_EQ(guest.RemoteConfirmed(), 1);
  Pad8 pad{};
  ASSERT_TRUE(guest.RemotePad(0, &pad));
  EXPECT_EQ(pad[0], 10);
  ASSERT_TRUE(guest.RemotePad(1, &pad));
  EXPECT_EQ(pad[0], 20);
  EXPECT_FALSE(guest.RemotePad(2, &pad));
}

TEST(Rev3NetLink, AckRetiresPadsAndMeasuresTheRoundTrip)
{
  PadLink host(0), guest(1);
  const std::vector<u8> p0 = host.QueueLocal(0, P(1), -1, 0, 10000);
  EXPECT_EQ(host.Unacked(), 1u);
  EXPECT_EQ(host.AckedFrame(), -1);
  const std::vector<u8> ack = Deliver(guest, p0, 14000);
  Deliver(host, ack, 30000);
  EXPECT_EQ(host.Unacked(), 0u);
  EXPECT_EQ(host.AckedFrame(), 0);
  EXPECT_EQ(host.RttUs(), 20000u);  // 30000 - 10000: the first sample is taken as is
  // An old ack changes nothing.
  std::vector<u8> reply;
  EXPECT_TRUE(host.OnPacket(ack.data(), ack.size(), 40000, &reply));
  EXPECT_EQ(host.AckedFrame(), 0);
}

TEST(Rev3NetLink, ARepeatedPadIsNotASecondSample)
{
  PadLink host(0), guest(1);
  const std::vector<u8> p0 = host.QueueLocal(0, P(1), -1, 0, 0);
  const std::vector<u8> again = host.PollResend(20000);
  ASSERT_FALSE(again.empty());
  const std::vector<u8> ack = Deliver(guest, again, 21000);
  Deliver(host, ack, 40000);
  EXPECT_EQ(host.RttUs(), 0u);  // the pad was sent twice: the ack cannot be matched to one send (Karn)
  EXPECT_EQ(host.Unacked(), 0u);
}

// ---- PadLink: M5 round trip while streaming, and the time offsets -------------------------------------------------------------------

TEST(Rev3NetLink, TheRoundTripIsSampledWhileFramesStreamEvenThoughEveryPacketRepeatsTheUnackedPads)
{
  // ARMED: a packet per frame carries every pad that is not acked yet, so with a 20 ms round trip (more than a frame time) every pad goes out
  // twice before its ack is back. The ack of a pad is still a clean sample of the FIRST send (only timer re-sends are ambiguous, Karn).
  PadLink host(0), guest(1);
  std::vector<std::pair<u64, std::vector<u8>>> acks;  // (arrival at the host, datagram)
  for (s32 f = 0; f < 12; ++f)
  {
    const u64 t = static_cast<u64>(f) * 16683;
    for (auto it = acks.begin(); it != acks.end();)
    {
      if (it->first <= t)
      {
        Deliver(host, it->second, it->first);
        it = acks.erase(it);
      }
      else
        ++it;
    }
    const std::vector<u8> packet = host.QueueLocal(f, P(static_cast<u8>(f)), -1, 0, t);
    acks.emplace_back(t + 20000, Deliver(guest, packet, t + 10000));
  }
  EXPECT_EQ(host.RttUs(), 20000u);  // 20 ms every time
  EXPECT_GE(host.AckedFrame(), 9);
}

TEST(Rev3NetLink, ATimerResendOfAPadIsNoSampleButTheNextFrameIsOne)
{
  PadLink host(0), guest(1);
  const std::vector<u8> p0 = host.QueueLocal(0, P(1), -1, 0, 0);
  const std::vector<u8> again = host.PollResend(30000);  // the packet (or its ack) was lost: the timer sends frame 0 again
  ASSERT_FALSE(again.empty());
  Deliver(host, Deliver(guest, again, 40000), 70000);
  EXPECT_EQ(host.RttUs(), 0u);
  const std::vector<u8> p1 = host.QueueLocal(1, P(2), -1, 0, 80000);
  Deliver(host, Deliver(guest, p1, 90000), 100000);
  EXPECT_EQ(host.RttUs(), 20000u);
}

namespace
{
// Two PadLinks and a wire with a fixed one-way delay, driven in time order.
struct TwoLinks
{
  explicit TwoLinks(u64 one_way) : one_way_us(one_way) {}
  void Send(int from, const std::vector<u8>& bytes, u64 now) { q.emplace(now + one_way_us, std::make_pair(1 - from, bytes)); }
  void Run(u64 until)
  {
    while (!q.empty() && q.begin()->first <= until)
    {
      const u64 at = q.begin()->first;
      const int to = q.begin()->second.first;
      const std::vector<u8> bytes = q.begin()->second.second;
      q.erase(q.begin());
      std::vector<u8> reply;
      EXPECT_TRUE(link[to].OnPacket(bytes.data(), bytes.size(), at, &reply));
      if (!reply.empty())
        Send(to, reply, at);
    }
  }
  PadLink link[2] = {PadLink(0), PadLink(1)};
  u64 one_way_us;
  std::multimap<u64, std::pair<int, std::vector<u8>>> q;
};
}  // namespace

TEST(Rev3NetLink, TimeOffsetsSayWhichSideSendsEarlier)
{
  // The host sends every frame 20 ms before the guest does (so it is 20 ms ahead); one way takes 30 ms, the round trip 60 ms.
  TwoLinks net(30000);
  s32 off = 0;
  u32 n = 0;
  for (s32 f = 0; f < 80; ++f)
  {
    const u64 host_t = static_cast<u64>(f) * 16683;
    const u64 guest_t = host_t + 20000;
    net.Run(host_t);
    net.Send(0, net.link[0].QueueLocal(f, P(static_cast<u8>(f)), -1, 0, host_t), host_t);
    if (f == 0)
    {
      EXPECT_FALSE(net.link[1].TimeOffsetUs(&off, &n));  // nothing received yet
    }
    net.Run(guest_t);
    net.Send(1, net.link[1].QueueLocal(f, P(static_cast<u8>(f + 100)), -1, 0, guest_t), guest_t);
  }
  net.Run(80 * 16683 + 200000);
  EXPECT_EQ(net.link[0].RttUs(), 60000u);
  EXPECT_EQ(net.link[1].RttUs(), 60000u);
  ASSERT_TRUE(net.link[0].TimeOffsetUs(&off, &n));
  EXPECT_EQ(off, 20000);  // the host is ahead
  EXPECT_EQ(n, 30u);      // the window holds the last 30 samples
  ASSERT_TRUE(net.link[1].TimeOffsetUs(&off, &n));
  EXPECT_EQ(off, -20000);  // the guest is behind
  net.link[0].ResetTimeOffsets();
  EXPECT_FALSE(net.link[0].TimeOffsetUs(&off, &n));
}

// ---- PadLink: loss, reordering, resend --------------------------------------------------------------------------------------------

TEST(Rev3NetLink, ALostPacketIsFilledByTheNextOne)
{
  PadLink host(0), guest(1);
  std::vector<std::vector<u8>> sent;
  for (s32 f = 0; f < 5; ++f)
    sent.push_back(host.QueueLocal(f, P(static_cast<u8>(f)), -1, 0, 1000 * (f + 1)));
  // Packets for frames 0, 1 and 3 are lost; 2 and 4 arrive. Packet 4 holds pads 0..4 (nothing was acked), packet 2 holds 0..2.
  Deliver(guest, sent[2], 5000);
  EXPECT_EQ(guest.RemoteConfirmed(), 2);
  const std::vector<u8> ack = Deliver(guest, sent[4], 6000);
  EXPECT_EQ(guest.RemoteConfirmed(), 4);
  EXPECT_EQ(BE32(ack, 1), 5u);
}

TEST(Rev3NetLink, AckNeverClaimsAFrameThatIsNotContiguous)
{
  // The sender had already dropped frames 0..9 from its queue (acked earlier in another life of the link): the receiver holds only
  // 10..11 and R stays -1, so it sends no ack at all rather than a lying one.
  PadLink host(0), guest(1);
  for (s32 f = 0; f < 10; ++f)
    host.QueueLocal(f, P(1), -1, 0, 0);
  std::vector<u8> fake_ack = {WIRE_PAD_ACK, 0, 0, 0, 10, 1};
  std::vector<u8> reply;
  EXPECT_TRUE(host.OnPacket(fake_ack.data(), fake_ack.size(), 100, &reply));
  EXPECT_EQ(host.Unacked(), 0u);
  const std::vector<u8> p10 = host.QueueLocal(10, P(2), -1, 0, 200);
  const std::vector<u8> p11 = host.QueueLocal(11, P(3), -1, 0, 300);
  ASSERT_EQ(p11.size(), PAD_HEADER_BYTES + 2 * PAD_BYTES);
  EXPECT_TRUE(guest.OnPacket(p11.data(), p11.size(), 400, &reply));
  EXPECT_TRUE(reply.empty());
  EXPECT_EQ(guest.RemoteConfirmed(), -1);
  Pad8 pad{};
  EXPECT_FALSE(guest.RemotePad(10, &pad));  // beyond R: not handed out
  (void)p10;
}

TEST(Rev3NetLink, ReorderedAndDuplicatedPacketsAreHarmless)
{
  PadLink host(0), guest(1);
  std::vector<std::vector<u8>> sent;
  for (s32 f = 0; f < 4; ++f)
    sent.push_back(host.QueueLocal(f, P(static_cast<u8>(100 + f)), -1, 0, 100 * f));
  Deliver(guest, sent[3], 1000);
  EXPECT_EQ(guest.RemoteConfirmed(), 3);
  Deliver(guest, sent[0], 1100);
  Deliver(guest, sent[3], 1200);
  EXPECT_EQ(guest.RemoteConfirmed(), 3);
  for (s32 f = 0; f < 4; ++f)
  {
    Pad8 pad{};
    ASSERT_TRUE(guest.RemotePad(f, &pad));
    EXPECT_EQ(pad[0], 100 + f);
  }
}

TEST(Rev3NetLink, ResendWaitsAboutOneRoundTripAndAtLeast15Ms)
{
  PadLink host(0), guest(1);
  // First a clean round trip of 40 ms to learn the RTT.
  const std::vector<u8> p0 = host.QueueLocal(0, P(1), -1, 0, 0);
  Deliver(host, Deliver(guest, p0, 20000), 40000);
  ASSERT_EQ(host.RttUs(), 40000u);
  host.QueueLocal(1, P(2), -1, 0, 50000);
  EXPECT_TRUE(host.PollResend(60000).empty());   // 10 ms < max(15 ms, 40 ms)
  EXPECT_TRUE(host.PollResend(89999).empty());
  const std::vector<u8> r = host.PollResend(90000);
  ASSERT_FALSE(r.empty());
  EXPECT_EQ(r.size(), PAD_HEADER_BYTES + PAD_BYTES);
  EXPECT_TRUE(host.PollResend(95000).empty());  // the timer restarts at each send
}

TEST(Rev3NetLink, ResendFloorIs15MsWhenTheRoundTripIsShort)
{
  PadLink host(0);
  host.QueueLocal(0, P(1), -1, 0, 0);
  EXPECT_TRUE(host.PollResend(14999).empty());
  EXPECT_FALSE(host.PollResend(15000).empty());
}

TEST(Rev3NetLink, NothingToResendWhenEverythingIsAcked)
{
  PadLink host(0), guest(1);
  const std::vector<u8> p0 = host.QueueLocal(0, P(1), -1, 0, 0);
  Deliver(host, Deliver(guest, p0, 100), 200);
  EXPECT_TRUE(host.PollResend(1000000).empty());
}

TEST(Rev3NetLink, ASilentPeerOverflowsTheQueue)
{
  PadLink host(0);
  for (s32 f = 0; f < static_cast<s32>(MAX_UNACKED) + 1; ++f)
    host.QueueLocal(f, P(1), -1, 0, static_cast<u64>(f));
  EXPECT_TRUE(host.Overflowed());
  // A packet never carries more than MAX_PADS_PER_PACKET pads: the OLDEST are sent, the receiver needs them contiguous.
  const std::vector<u8> p = host.PollResend(100000000);
  ASSERT_FALSE(p.empty());
  EXPECT_EQ(p.size(), PAD_HEADER_BYTES + MAX_PADS_PER_PACKET * PAD_BYTES);
  EXPECT_EQ(BE32(p, 1), static_cast<u32>(MAX_PADS_PER_PACKET));  // newest pad in the packet = hook frame 127 = wire frame 128
}

TEST(Rev3NetLink, FramesNotAboveTheLastQueuedAreIgnored)
{
  PadLink host(0);
  EXPECT_FALSE(host.QueueLocal(5, P(1), -1, 0, 0).empty());
  EXPECT_TRUE(host.QueueLocal(5, P(2), -1, 0, 1).empty());
  EXPECT_TRUE(host.QueueLocal(3, P(2), -1, 0, 1).empty());
  EXPECT_EQ(host.Unacked(), 1u);
}

TEST(Rev3NetLink, PeerChecksumsAreHandedOutOnceEach)
{
  PadLink host(0), guest(1);
  Deliver(guest, host.QueueLocal(0, P(1), -1, 0, 0), 10);       // no checksum
  Deliver(guest, host.QueueLocal(1, P(1), 30, 0x11111111, 20), 30);
  Deliver(guest, host.QueueLocal(2, P(1), 30, 0x11111111, 40), 50);  // the same one again
  Deliver(guest, host.QueueLocal(3, P(1), 60, 0x22222222, 60), 70);
  const std::vector<PeerChecksum> got = guest.TakePeerChecksums();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].frame, 30);
  EXPECT_EQ(got[0].value, 0x11111111u);
  EXPECT_EQ(got[1].frame, 60);
  EXPECT_TRUE(guest.TakePeerChecksums().empty());
}

TEST(Rev3NetLink, MalformedPacketsAreIgnoredAndCounted)
{
  PadLink guest(1);
  std::vector<u8> reply;
  const std::vector<u8> truncated = {WIRE_PAD, 0, 0, 0, 1, 0};
  EXPECT_FALSE(guest.OnPacket(truncated.data(), truncated.size(), 0, &reply));
  std::vector<u8> own_port(PAD_HEADER_BYTES + PAD_BYTES, 0);  // a PAD that names OUR port is not from the peer
  own_port[0] = WIRE_PAD;
  own_port[4] = 1;
  own_port[5] = 1;
  EXPECT_FALSE(guest.OnPacket(own_port.data(), own_port.size(), 0, &reply));
  std::vector<u8> bad_port = own_port;
  bad_port[5] = 7;
  EXPECT_FALSE(guest.OnPacket(bad_port.data(), bad_port.size(), 0, &reply));
  std::vector<u8> zero_frame = own_port;
  zero_frame[5] = 0;
  zero_frame[4] = 0;  // wire frame 0 does not exist (Slippi starts at 1)
  EXPECT_FALSE(guest.OnPacket(zero_frame.data(), zero_frame.size(), 0, &reply));
  const std::vector<u8> unknown = {0x55, 1, 2, 3};
  EXPECT_FALSE(guest.OnPacket(unknown.data(), unknown.size(), 0, &reply));
  EXPECT_FALSE(guest.OnPacket(unknown.data(), 0, 0, &reply));
  EXPECT_EQ(guest.BadPackets(), 4u);
  EXPECT_EQ(guest.RemoteConfirmed(), -1);
}

TEST(Rev3NetLink, ARemotePadFarAheadIsIgnoredUntilTheGapIsFilled)
{
  PadLink host(0), guest(1);
  // A hand-built PAD holding only hook frame `far` (the sender's queue is never trimmed in a real run, so a real packet always starts at
  // the oldest unacked pad; this is what a corrupt or hostile peer could send).
  const s32 far = REMOTE_AHEAD + 40;
  std::vector<u8> packet(PAD_HEADER_BYTES + PAD_BYTES, 0);
  packet[0] = WIRE_PAD;
  const u32 wire = static_cast<u32>(far + WIRE_FRAME_OFFSET);
  packet[1] = static_cast<u8>(wire >> 24);
  packet[2] = static_cast<u8>(wire >> 16);
  packet[3] = static_cast<u8>(wire >> 8);
  packet[4] = static_cast<u8>(wire);
  packet[5] = 0;
  for (size_t i = 6; i < 10; ++i)
    packet[i] = 0xFF;  // no checksum
  std::vector<u8> reply;
  EXPECT_TRUE(guest.OnPacket(packet.data(), packet.size(), 0, &reply));
  EXPECT_EQ(guest.RemoteConfirmed(), -1);
  EXPECT_TRUE(reply.empty());
  Pad8 pad{};
  EXPECT_FALSE(guest.RemotePad(far, &pad));
  // The real stream still gets through afterwards.
  for (s32 f = 0; f < 5; ++f)
    Deliver(guest, host.QueueLocal(f, P(static_cast<u8>(f)), -1, 0, f), f);
  EXPECT_EQ(guest.RemoteConfirmed(), 4);
}

TEST(Rev3NetLink, RemotePadsOlderThanTheKeepWindowAreGone)
{
  PadLink host(0), guest(1);
  for (s32 f = 0; f < 400; ++f)
  {
    const std::vector<u8> p = host.QueueLocal(f, P(static_cast<u8>(f)), -1, 0, f);
    Deliver(host, Deliver(guest, p, f), f);
  }
  EXPECT_EQ(guest.RemoteConfirmed(), 399);
  Pad8 pad{};
  EXPECT_TRUE(guest.RemotePad(399, &pad));
  EXPECT_TRUE(guest.RemotePad(399 - REMOTE_KEEP, &pad));
  EXPECT_FALSE(guest.RemotePad(399 - REMOTE_KEEP - 1, &pad));
  EXPECT_FALSE(guest.RemotePad(400, &pad));
  EXPECT_FALSE(guest.RemotePad(-1, &pad));
}

TEST(Rev3NetLink, RemotePadsAreContiguousFromFrameZeroThroughLoss)
{
  // 300 frames, every third packet dropped, every pad must arrive and be exactly the one sent.
  PadLink host(0), guest(1);
  for (s32 f = 0; f < 300; ++f)
  {
    const std::vector<u8> p = host.QueueLocal(f, P(static_cast<u8>(f * 7)), -1, 0, 1000 * static_cast<u64>(f));
    if (f % 3 != 0)
      Deliver(host, Deliver(guest, p, 1000 * static_cast<u64>(f)), 1000 * static_cast<u64>(f) + 500);
    Pad8 pad{};
    if (guest.RemoteConfirmed() >= 0)
    {
      for (s32 g = std::max(0, guest.RemoteConfirmed() - 60); g <= guest.RemoteConfirmed(); ++g)
      {
        ASSERT_TRUE(guest.RemotePad(g, &pad));
        ASSERT_EQ(pad[0], static_cast<u8>(g * 7));
      }
    }
  }
  EXPECT_EQ(guest.RemoteConfirmed(), 299);  // 299 % 3 == 2: the last packet arrived and it carries every pad not yet acked
}

// ---- reliable messages ------------------------------------------------------------------------------------------------------------

TEST(Rev3NetLink, ReliableEnvelopeRoundTrips)
{
  const std::vector<u8> payload = {1, 2, 3, 250};
  const std::vector<u8> wire = EncodeReliable(MsgType::StartCheck, 0x01020304, payload);
  ASSERT_EQ(wire.size(), RELIABLE_HEADER_BYTES + payload.size());
  EXPECT_EQ(wire[0], 0x91);
  EXPECT_EQ(BE32(wire, 1), 0x01020304u);
  MsgType type = MsgType::Bye;
  u32 sequence = 0;
  std::vector<u8> back;
  ASSERT_TRUE(DecodeReliable(wire.data(), wire.size(), &type, &sequence, &back));
  EXPECT_EQ(type, MsgType::StartCheck);
  EXPECT_EQ(sequence, 0x01020304u);
  EXPECT_EQ(back, payload);
  // An empty payload is fine; ids outside 0x90..0x94 and short buffers are not.
  const std::vector<u8> bye = EncodeReliable(MsgType::Bye, 9, {});
  EXPECT_TRUE(DecodeReliable(bye.data(), bye.size(), &type, &sequence, &back));
  EXPECT_TRUE(back.empty());
  std::vector<u8> bad = wire;
  bad[0] = 0x80;
  EXPECT_FALSE(DecodeReliable(bad.data(), bad.size(), &type, &sequence, &back));
  bad[0] = 0x94;  // RANGE_CRC (M4)
  EXPECT_TRUE(DecodeReliable(bad.data(), bad.size(), &type, &sequence, &back));
  EXPECT_EQ(type, MsgType::RangeCrc);
  bad[0] = 0x95;
  EXPECT_FALSE(DecodeReliable(bad.data(), bad.size(), &type, &sequence, &back));
  EXPECT_FALSE(DecodeReliable(wire.data(), RELIABLE_HEADER_BYTES - 1, &type, &sequence, &back));
}

TEST(Rev3NetLink, MessageIdsAre0x90To0x93)
{
  EXPECT_EQ(static_cast<u8>(MsgType::Hello), 0x90);
  EXPECT_EQ(static_cast<u8>(MsgType::Bye), 0x93);
}

TEST(Rev3NetLink, InboxDeliversOnceAndInOrder)
{
  ReliableInbox inbox;
  Message m;
  EXPECT_FALSE(inbox.Poll(&m));
  inbox.Add(MsgType::StartCheck, 1, {11});
  EXPECT_FALSE(inbox.Poll(&m));  // sequence 0 has not arrived: nothing is delivered out of order
  inbox.Add(MsgType::Hello, 0, {10});
  inbox.Add(MsgType::Hello, 0, {99});  // the same message over the second connection
  ASSERT_TRUE(inbox.Poll(&m));
  EXPECT_EQ(m.type, MsgType::Hello);
  EXPECT_EQ(m.bytes, std::vector<u8>({10}));
  ASSERT_TRUE(inbox.Poll(&m));
  EXPECT_EQ(m.type, MsgType::StartCheck);
  EXPECT_FALSE(inbox.Poll(&m));
  inbox.Add(MsgType::Hello, 0, {10});  // a late copy of a delivered one
  inbox.Add(MsgType::StartCheck, 1, {11});
  EXPECT_FALSE(inbox.Poll(&m));
  EXPECT_EQ(inbox.Held(), 0u);
}

TEST(Rev3NetLink, InboxRefusesAFlood)
{
  ReliableInbox inbox;
  for (u32 s = 1; s < 1000; ++s)
    inbox.Add(MsgType::DesyncDump, s, {1});
  EXPECT_LE(inbox.Held(), 256u);
}

// ---- options ----------------------------------------------------------------------------------------------------------------------

TEST(Rev3NetLink, PeerAddressParsing)
{
  PeerAddress a;
  std::string error;
  ASSERT_TRUE(ParsePeerAddress("192.168.1.20:2627", &a, &error));
  EXPECT_EQ(a.host, "192.168.1.20");
  EXPECT_EQ(a.port, 2627);
  ASSERT_TRUE(ParsePeerAddress("rig-b.lan:1", &a, &error));
  EXPECT_EQ(a.host, "rig-b.lan");
  EXPECT_EQ(a.port, 1);
  for (const char* bad : {"", "10.0.0.1", "10.0.0.1:", ":2627", "10.0.0.1:0", "10.0.0.1:65536", "10.0.0.1:12ab", "::1:2627",
                          "10.0.0 1:2627", "10.0.0.1:99999999999"})
  {
    EXPECT_FALSE(ParsePeerAddress(bad, &a, &error)) << bad;
    EXPECT_FALSE(error.empty()) << bad;
  }
  ASSERT_TRUE(ParsePeerAddress("10.0.0.1:65535", &a, &error));
  EXPECT_EQ(a.port, 65535);
}

TEST(Rev3NetLink, UnsetRoleMeansNoNetplay)
{
  const NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_PEER", "1.2.3.4:5"}}));
  EXPECT_FALSE(o.requested);
  EXPECT_FALSE(o.ok);
}

TEST(Rev3NetLink, DefaultsAreThePlansNumbers)
{
  const NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}, {"REV3_NET_PEER", "127.0.0.1:2627"}}));
  ASSERT_TRUE(o.ok) << o.error;
  EXPECT_TRUE(o.requested);
  EXPECT_EQ(o.role, Role::Host);
  EXPECT_EQ(o.listen_port, 2626);
  EXPECT_EQ(o.peer.host, "127.0.0.1");
  EXPECT_EQ(o.peer.port, 2627);
  EXPECT_EQ(o.delay, 2);
  EXPECT_FALSE(o.rollback);  // REV3_NET_ROLLBACK defaults to 0: lockstep
  EXPECT_EQ(o.max_rollback, 5);
  EXPECT_EQ(o.arm_calls, 600u);
  EXPECT_EQ(o.quiet_calls, 60u);
  EXPECT_EQ(o.connect_timeout_ms, 120000u);
  EXPECT_TRUE(o.log_path.empty());
  EXPECT_TRUE(o.warnings.empty());
}

TEST(Rev3NetLink, EverythingCanBeSet)
{
  const NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "Guest"},
                                              {"REV3_NET_PEER", "10.1.2.3:3000"},
                                              {"REV3_NET_PORT", "3001"},
                                              {"REV3_NET_DELAY", "4"},
                                              {"REV3_NET_ROLLBACK", "1"},
                                              {"REV3_NET_ROLLBACK_MAX", "7"},
                                              {"REV3_NET_ARM", "10"},
                                              {"REV3_NET_QUIET", "0"},
                                              {"REV3_NET_TIMEOUT_S", "30"},
                                              {"REV3_NET_WAIT_MS", "2500"},
                                              {"REV3_NET_LOG", "C:\\x\\net.log"},
                                              {"REV3_NET_STABLE", "stable.json"}}));
  ASSERT_TRUE(o.ok) << o.error;
  EXPECT_EQ(o.role, Role::Guest);
  EXPECT_EQ(o.listen_port, 3001);
  EXPECT_EQ(o.delay, 4);
  EXPECT_TRUE(o.rollback);
  EXPECT_EQ(o.max_rollback, 7);
  EXPECT_EQ(o.arm_calls, 10u);
  EXPECT_EQ(o.quiet_calls, 0u);
  EXPECT_EQ(o.connect_timeout_ms, 30000u);
  EXPECT_EQ(o.wait_timeout_ms, 2500u);
  EXPECT_EQ(o.log_path, "C:\\x\\net.log");
  EXPECT_EQ(o.stable_path, "stable.json");
  EXPECT_TRUE(o.warnings.empty());
}

TEST(Rev3NetLink, M4OptionsHaveDefaultsAndCanBeSet)
{
  NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}, {"REV3_NET_PEER", "1.2.3.4:5"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_EQ(o.busy_calls, 3u);
  EXPECT_FALSE(o.range_crc);
  EXPECT_TRUE(o.netsim.empty());
  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"},
                             {"REV3_NET_PEER", "1.2.3.4:5"},
                             {"REV3_NET_BUSY", "0"},
                             {"REV3_NET_RANGE_CRC", "1"},
                             {"REV3_NETSIM", "delay_ms=80,jitter_ms=30,loss_pct=5,seed=1"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_EQ(o.busy_calls, 0u);
  EXPECT_TRUE(o.range_crc);
  EXPECT_EQ(o.netsim, "delay_ms=80,jitter_ms=30,loss_pct=5,seed=1");
  EXPECT_TRUE(o.warnings.empty());
  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}, {"REV3_NET_PEER", "1.2.3.4:5"}, {"REV3_NET_BUSY", "5000"}, {"REV3_NET_RANGE_CRC", "maybe"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_EQ(o.busy_calls, 3u);
  EXPECT_FALSE(o.range_crc);
  EXPECT_EQ(o.warnings.size(), 2u);
}

TEST(Rev3NetLink, M5PacingOptionsHaveDefaultsAndCanBeSet)
{
  NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}, {"REV3_NET_PEER", "1.2.3.4:5"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_TRUE(o.pace.enabled);  // pacing is on unless switched off
  EXPECT_EQ(o.pace.t1_us, 10000);
  EXPECT_EQ(o.pace.hard_max_us, 5 * PACE_FRAME_US);
  EXPECT_EQ(o.pace.slice_us, 4000);
  EXPECT_EQ(o.pace.gentle_max_permille, 20);
  EXPECT_FALSE(o.maxr_adapt);  // the adaptive MAXR is off unless switched on
  EXPECT_EQ(o.maxr_budget_us, 14000u);
  EXPECT_TRUE(o.warnings.empty());

  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "guest"},
                             {"REV3_NET_PEER", "1.2.3.4:5"},
                             {"REV3_NET_PACE", "0"},
                             {"REV3_NET_PACE_T1_US", "12000"},
                             {"REV3_NET_PACE_HARD_MAX_US", "50000"},
                             {"REV3_NET_PACE_SLICE_US", "2000"},
                             {"REV3_NET_PACE_GENTLE_PERMILLE", "5"},
                             {"REV3_NET_MAXR_ADAPT", "1"},
                             {"REV3_NET_MAXR_BUDGET_US", "9000"},
                             {"REV3_NET_ROLLBACK_MAX", "7"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_FALSE(o.pace.enabled);
  EXPECT_EQ(o.pace.t1_us, 12000);
  EXPECT_EQ(o.pace.hard_max_us, 50000);
  EXPECT_EQ(o.pace.slice_us, 2000);
  EXPECT_EQ(o.pace.gentle_max_permille, 5);
  EXPECT_TRUE(o.maxr_adapt);
  EXPECT_EQ(o.maxr_budget_us, 9000u);
  EXPECT_EQ(o.max_rollback, 7);  // MAXR up to 7 (Slippi's ROLLBACK_MAX_FRAMES)
  EXPECT_TRUE(o.warnings.empty());

  // Unusable values keep the default and say so; MAXR 8 is above the ceiling.
  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"},
                             {"REV3_NET_PEER", "1.2.3.4:5"},
                             {"REV3_NET_PACE", "yes"},
                             {"REV3_NET_PACE_SLICE_US", "0"},
                             {"REV3_NET_MAXR_ADAPT", "2"},
                             {"REV3_NET_MAXR_BUDGET_US", "5"},
                             {"REV3_NET_ROLLBACK_MAX", "8"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_TRUE(o.pace.enabled);
  EXPECT_EQ(o.pace.slice_us, 4000);
  EXPECT_FALSE(o.maxr_adapt);
  EXPECT_EQ(o.maxr_budget_us, 14000u);
  EXPECT_EQ(o.max_rollback, 5);
  EXPECT_EQ(o.warnings.size(), 5u);
}

TEST(Rev3NetLink, BadRoleOrPeerIsRefusedWithAReason)
{
  NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "spectator"}, {"REV3_NET_PEER", "1.2.3.4:5"}}));
  EXPECT_TRUE(o.requested);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.error.find("neither host nor guest"), std::string::npos);
  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}}));
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.error.find("REV3_NET_PEER"), std::string::npos);
  o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"}, {"REV3_NET_PEER", "nonsense"}}));
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.error.find("nonsense"), std::string::npos);
}

TEST(Rev3NetLink, UnusableNumbersKeepTheDefaultAndWarn)
{
  const NetOptions o = ParseNetOptions(EnvOf({{"REV3_NET_ROLE", "host"},
                                              {"REV3_NET_PEER", "1.2.3.4:5"},
                                              {"REV3_NET_DELAY", "99"},
                                              {"REV3_NET_ROLLBACK_MAX", "8"},
                                              {"REV3_NET_PORT", "abc"},
                                              {"REV3_NET_ROLLBACK", "yes"}}));
  ASSERT_TRUE(o.ok);
  EXPECT_EQ(o.delay, 2);
  EXPECT_EQ(o.max_rollback, 5);
  EXPECT_EQ(o.listen_port, 2626);
  EXPECT_FALSE(o.rollback);
  EXPECT_EQ(o.warnings.size(), 4u);
}

// ---- HELLO ------------------------------------------------------------------------------------------------------------------------

TEST(Rev3NetLink, HelloCarriesTheListedItems)
{
  const Hello h = BuildHello(SampleInputs(), Role::Host, 2);
  EXPECT_EQ(h.game_id, "RNEEDA");
  EXPECT_EQ(h.protocol, 2u);
  EXPECT_EQ(h.fields_digest, FIELDS_DIGEST_V1);
  EXPECT_EQ(h.delay, 2u);
  std::map<std::string, std::string> items(h.items.begin(), h.items.end());
  for (const char* key : {"build", "cpu_core", "cpu_thread", "sync_gpu", "dsp", "rtc", "overclock", "mmu", "fast_disc_speed", "wiimotes",
                          "hook_code_crc", "rollback", "gate", "maxr", "stable_ranges"})
    EXPECT_EQ(items.count(key), 1u) << key;
  EXPECT_EQ(items["cpu_core"], "JIT64");
  EXPECT_EQ(items["cpu_thread"], "single");
  EXPECT_EQ(items["dsp"], "HLE");
  EXPECT_EQ(items["rtc"], "on:1577836800");
  EXPECT_EQ(items["overclock"], "off");
  EXPECT_EQ(items["wiimotes"], "1,0,0,0,0");
  EXPECT_EQ(items["hook_code_crc"], "0xA1B2C3D4");
  EXPECT_EQ(items["rollback"], "0");
  EXPECT_EQ(items["gate"], "arm=600,quiet=60");
  EXPECT_EQ(items["maxr"], "5");
  EXPECT_EQ(items["stable_ranges"], "none");
}

TEST(Rev3NetLink, EachSettingThatCouldDivergeMakesTheHellosDisagree)
{
  const HelloInputs base = SampleInputs();
  const Hello host = BuildHello(base, Role::Host, 2);
  EXPECT_TRUE(HelloMismatches(host, BuildHello(base, Role::Guest, 2)).empty());
  // The input delay is the host's to choose: not a mismatch.
  EXPECT_TRUE(HelloMismatches(host, BuildHello(base, Role::Guest, 5)).empty());
  // Two equal roles are.
  EXPECT_FALSE(HelloMismatches(host, BuildHello(base, Role::Host, 2)).empty());

  struct Case
  {
    const char* key;
    void (*change)(HelloInputs*);
  };
  const Case cases[] = {
      {"build", [](HelloInputs* i) { i->build = "other"; }},
      {"cpu_core", [](HelloInputs* i) { i->cpu_core = "CachedInterpreter"; }},
      {"cpu_thread", [](HelloInputs* i) { i->cpu_thread = true; }},
      {"sync_gpu", [](HelloInputs* i) { i->sync_gpu = true; }},
      {"dsp", [](HelloInputs* i) { i->dsp_hle = false; }},
      {"rtc", [](HelloInputs* i) { i->rtc_value += 1; }},
      {"rtc", [](HelloInputs* i) { i->rtc_enabled = false; }},
      {"overclock", [](HelloInputs* i) { i->overclock_enabled = true; i->overclock = 1.5f; }},
      {"mmu", [](HelloInputs* i) { i->mmu = true; }},
      {"fast_disc_speed", [](HelloInputs* i) { i->fast_disc_speed = true; }},
      {"wiimotes", [](HelloInputs* i) { i->wiimote_sources[1] = 2; }},
      {"hook_code_crc", [](HelloInputs* i) { i->hook_crc ^= 1; }},
      {"rollback", [](HelloInputs* i) { i->rollback = true; }},
      {"gate", [](HelloInputs* i) { i->arm_calls = 601; }},
      {"maxr", [](HelloInputs* i) { i->max_rollback = 6; }},  // M5: the gate close is scheduled MAXR + 1 frames on, so both sides must use the same MAXR
      {"stable_ranges", [](HelloInputs* i) { i->stable_crc = 0x1234; }},
  };
  for (const Case& c : cases)
  {
    HelloInputs changed = base;
    c.change(&changed);
    const std::vector<std::string> diffs = HelloMismatches(host, BuildHello(changed, Role::Guest, 2));
    ASSERT_FALSE(diffs.empty()) << c.key;
    bool named = false;
    for (const std::string& d : diffs)
      named = named || d.find(c.key) != std::string::npos;
    EXPECT_TRUE(named) << c.key;
  }
  HelloInputs other_game = base;
  other_game.game_id = "GALE01";
  EXPECT_FALSE(HelloMismatches(host, BuildHello(other_game, Role::Guest, 2)).empty());
}

TEST(Rev3NetLink, HelloCarriesTheHookProtocolAndAMismatchRefuses)
{
  HelloInputs v2 = SampleInputs();
  v2.hook_protocol = 2;
  HelloInputs v3 = SampleInputs();
  v3.hook_protocol = 3;
  const Hello host = BuildHello(v3, Role::Host, 2);
  EXPECT_EQ(host.protocol, 3u);
  EXPECT_EQ(BuildHello(v2, Role::Guest, 2).protocol, 2u);
  EXPECT_TRUE(HelloMismatches(host, BuildHello(v3, Role::Guest, 2)).empty());
  const std::vector<std::string> diffs = HelloMismatches(host, BuildHello(v2, Role::Guest, 2));
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(diffs[0], "protocol: own=3 peer=2");
  Hello back;
  ASSERT_TRUE(ParseHello(SerializeHello(host), &back));
  EXPECT_EQ(back.protocol, 3u);
}

TEST(Rev3NetLink, HelloSurvivesTheWire)
{
  const Hello h = BuildHello(SampleInputs(), Role::Guest, 3);
  Hello back;
  ASSERT_TRUE(ParseHello(SerializeHello(h), &back));
  EXPECT_TRUE(HelloMismatches(back, BuildHello(SampleInputs(), Role::Host, 3)).empty());
  EXPECT_EQ(back.delay, 3u);
}

// ---- the arm gate -----------------------------------------------------------------------------------------------------------------

namespace
{
GateInputs Good()
{
  GateInputs in;
  in.fighters_found = true;
  in.learning_done = true;
  in.may_prepare = true;
  return in;
}

GateConfig Cfg(u32 arm, u32 quiet)
{
  GateConfig c;
  c.rollback = true;
  c.arm_calls = arm;
  c.quiet_calls = quiet;
  return c;
}
}  // namespace

TEST(Rev3NetLink, GateStaysShutWhenRollbackIsOff)
{
  GateConfig c = Cfg(0, 0);
  c.rollback = false;
  GateTracker t(c);
  int prepared = 0;
  for (int i = 0; i < 100; ++i)
    EXPECT_FALSE(t.Update(Good(), [&] { ++prepared; return true; }).open);
  EXPECT_EQ(prepared, 0);
}

TEST(Rev3NetLink, GateOpensAfterTheArmCallsAndOnlyThen)
{
  GateTracker t(Cfg(600, 60));
  int prepared = 0;
  const auto prepare = [&] { ++prepared; return true; };
  for (int i = 1; i < 600; ++i)
  {
    const GateDecision d = t.Update(Good(), prepare);
    ASSERT_FALSE(d.open) << i;
    EXPECT_STREQ(d.why, "fighters not found long enough");
  }
  EXPECT_EQ(prepared, 0);
  EXPECT_TRUE(t.Update(Good(), prepare).open);  // the 600th call
  EXPECT_EQ(prepared, 1);
  for (int i = 0; i < 50; ++i)
    EXPECT_TRUE(t.Update(Good(), prepare).open);
  EXPECT_EQ(prepared, 1);  // the snapshot set is installed once
}

TEST(Rev3NetLink, GateWaitsForQuietHardware)
{
  GateTracker t(Cfg(5, 60));
  GateInputs busy = Good();
  busy.hardware_busy = true;
  for (int i = 0; i < 100; ++i)
  {
    const GateDecision d = t.Update(busy, nullptr);
    EXPECT_FALSE(d.open);
    EXPECT_FALSE(d.hardware_quiet);
  }
  for (int i = 1; i < 60; ++i)
    EXPECT_FALSE(t.Update(Good(), nullptr).open) << i;  // 59 quiet calls
  EXPECT_TRUE(t.Update(Good(), nullptr).open);          // the 60th
}

TEST(Rev3NetLink, GateNeedsTheSnapshotSetAndALockstepSession)
{
  GateTracker t(Cfg(1, 0));
  GateInputs in = Good();
  in.learning_done = false;
  EXPECT_STREQ(t.Update(in, nullptr).why, "snapshot set still learning");
  in.learning_done = true;
  in.may_prepare = false;
  EXPECT_STREQ(t.Update(in, nullptr).why, "session not in lockstep");
  in.may_prepare = true;
  in.health_zero = true;
  EXPECT_STREQ(t.Update(in, nullptr).why, "health 0");
  in.health_zero = false;
  EXPECT_TRUE(t.Update(in, nullptr).open);
}

TEST(Rev3NetLink, TheOpenDecisionNeedsFightersAndNobodyAtHealthZero)
{
  GateTracker t(Cfg(1, 0));
  GateInputs dead = Good();
  dead.health_zero = true;
  GateDecision d = t.Update(dead, nullptr);
  EXPECT_FALSE(d.open);
  EXPECT_STREQ(d.why, "health 0");
  ASSERT_TRUE(t.Update(Good(), nullptr).open);
  t.Close();  // the session closed (a final frame said so): shut until it opens again
  EXPECT_FALSE(t.open());
  d = t.Update(dead, nullptr);
  EXPECT_FALSE(d.open);
  EXPECT_STREQ(d.why, "health 0");
  GateInputs lost;
  EXPECT_STREQ(t.Update(lost, nullptr).why, "no fighters");
  EXPECT_TRUE(t.Update(Good(), nullptr).open);  // the streaks restarted: arm_calls is 1 here
}

TEST(Rev3NetLink, WhileArmedNothingLiveEverClosesTheGate)
{
  // THE GATE RULE: the close is the session's, from final frames. The tracker reports the local conditions only, and "open" stays true.
  GateTracker t(Cfg(1, 0));
  ASSERT_TRUE(t.Update(Good(), nullptr).open);
  GateInputs dead = Good();
  dead.health_zero = true;
  GateDecision d = t.UpdateArmed(dead);
  EXPECT_TRUE(d.open);
  EXPECT_TRUE(t.open());
  GateInputs lost;  // fighters not found
  d = t.UpdateArmed(lost);
  EXPECT_TRUE(d.open);
  GateInputs pending = Good();
  pending.set_pending = true;
  d = t.UpdateArmed(pending);
  EXPECT_TRUE(d.open);
  EXPECT_TRUE(d.layout_pending);  // a local relayout, not a close
  EXPECT_FALSE(t.UpdateArmed(Good()).layout_pending);
}

TEST(Rev3NetLink, ADiscReadWhileArmedOnlyPostponesRollbacksUntilItIsSustained)
{
  GateConfig cfg = Cfg(1, 0);
  cfg.hold_calls = 3;
  GateTracker t(cfg);
  ASSERT_TRUE(t.Update(Good(), nullptr).open);
  GateInputs busy = Good();
  busy.hardware_busy = true;
  GateDecision d = t.UpdateArmed(busy);
  EXPECT_TRUE(d.open);
  EXPECT_FALSE(d.hardware_quiet);  // rollbacks merely wait
  EXPECT_FALSE(d.hold);
  EXPECT_FALSE(t.UpdateArmed(busy).hold);
  d = t.UpdateArmed(busy);  // the third call in a row: HOLD (predict nothing); still open for the peer
  EXPECT_TRUE(d.hold);
  EXPECT_TRUE(d.open);
  // It ends only after HOLD_RELEASE_CALLS quiet calls, so a flickering read does not toggle it every frame.
  EXPECT_TRUE(t.UpdateArmed(Good()).hold);
  EXPECT_TRUE(t.UpdateArmed(busy).hold);
  EXPECT_TRUE(t.UpdateArmed(Good()).hold);
  EXPECT_FALSE(t.UpdateArmed(Good()).hold);
  // ... and it needs a sustained read again.
  EXPECT_FALSE(t.UpdateArmed(busy).hold);
}

TEST(Rev3NetLink, HoldCanBeSwitchedOff)
{
  GateConfig cfg = Cfg(1, 0);
  cfg.hold_calls = 0;
  GateTracker t(cfg);
  ASSERT_TRUE(t.Update(Good(), nullptr).open);
  GateInputs busy = Good();
  busy.hardware_busy = true;
  for (int i = 0; i < 20; ++i)
    EXPECT_FALSE(t.UpdateArmed(busy).hold);
}

TEST(Rev3NetLink, ASustainedDiscReadBeforeOpeningStillKeepsTheGateShut)
{
  GateTracker t(Cfg(1, 5));
  GateInputs busy = Good();
  busy.hardware_busy = true;
  EXPECT_STREQ(t.Update(busy, nullptr).why, "hardware busy");
  for (int i = 0; i < 4; ++i)
    EXPECT_FALSE(t.Update(Good(), nullptr).open);
  EXPECT_TRUE(t.Update(Good(), nullptr).open);
}

TEST(Rev3NetLink, AnUnusableSnapshotSetKeepsTheGateShutUntilTheFightersAreLostAndFoundAgain)
{
  GateTracker t(Cfg(1, 0));
  int prepared = 0;
  EXPECT_FALSE(t.Update(Good(), [&] { ++prepared; return false; }).open);
  EXPECT_STREQ(t.Update(Good(), [&] { ++prepared; return true; }).why, "snapshot set unusable");
  EXPECT_EQ(prepared, 1);
  t.Update(GateInputs(), nullptr);  // fighters lost
  EXPECT_TRUE(t.Update(Good(), [&] { ++prepared; return true; }).open);
  EXPECT_EQ(prepared, 2);
}

TEST(Rev3NetLink, RoleText)
{
  EXPECT_STREQ(RoleText(Role::Host), "host");
  EXPECT_STREQ(RoleText(Role::Guest), "guest");
}
