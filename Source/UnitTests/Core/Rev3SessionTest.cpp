// Unit tests for Core/Slippi/Rev3Session (rollback netplay session, milestone M2): HELLO / START_CHECK / dump codecs and the
// RollbackSession state machine, driven by a deterministic fake game (a few u32 words evolving from both players' pads), a fake snapshot
// store and either a scripted transport (one rule per test) or a simulated lossy / delayed / jittery network connecting two sessions in
// one process (1000 seeds). No Dolphin core, no game data. Run: build target `tests`, then `tests --gtest_filter=Rev3Session*`.
//
// THE SIMULATION (section "two machines"): a discrete-event scheduler over a virtual microsecond clock. Each machine calls
// RollbackSession::Step() once per game frame (its own period, so the two clocks drift against each other) and retries every 1 ms while
// the session says it must wait. The network delivers PAD packets with delay + jitter (so they reorder), drops PAD packets and acks with a
// probability, and implements Slippi's unacked-queue scheme: every packet repeats all pads the peer has not acked, plus a resend timer
// (see Transport::Pump). Reliable messages (HELLO, START_CHECK, BYE, dumps) arrive in order without loss. The reference is the same game
// run without a network: frame f gets script[f - D] of both players. Both machines must (a) report, through the final-frame observer,
// the reference checksum of every frame they ever call final, (b) end in exactly the reference state, (c) never roll back deeper than
// MAXR, (d) never log DESYNC / ERROR / LEAVE.

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Hash.h"
#include "Core/Slippi/Rev3Session.h"

namespace
{
using namespace Rev3;
using namespace Rev3::Net;

constexpr u32 SP = 0x804B1C40;  // the hook's r1 in every fake command (same call chain at every hook call)

// ---- small utilities -------------------------------------------------------------------------------------------------------------

struct Rng  // splitmix64: portable, so a failing seed means the same run on every compiler
{
  explicit Rng(u64 seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
  u64 Next()
  {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  u32 Below(u32 n) { return n == 0 ? 0 : static_cast<u32>(Next() % n); }
  u64 state;
};

Pad8 P(u8 a, u8 b = 0, u8 c = 0)
{
  return Pad8{a, b, c, 0, 0, 0, 0, 0};
}

Pad8 RandomPad(Rng& rng)
{
  Pad8 p{};
  for (u8& b : p)
    b = static_cast<u8>(rng.Below(256));
  return p;
}

// What the local controller does at game frame f in the scripted unit tests / what the remote player pressed.
Pad8 LocalPad(s32 f)
{
  return P(static_cast<u8>(0x20 + f % 11), static_cast<u8>(f));
}
Pad8 RemotePadScript(s32 f)
{
  return P(static_cast<u8>(0x40 + (f / 3) % 5), static_cast<u8>(f / 3));  // changes every 3 frames
}

class TestSink final : public Sink
{
public:
  void Line(const std::string& text) override { lines.push_back(text); }
  void Osd(const std::string& text) override { osd.push_back(text); }
  bool Has(const std::string& part) const { return Count(part) != 0; }
  size_t Count(const std::string& part) const
  {
    size_t n = 0;
    for (const std::string& l : lines)
      if (l.find(part) != std::string::npos)
        ++n;
    return n;
  }
  std::string Dump() const
  {
    std::string out;
    for (const std::string& l : lines)
      out += l + "\n";
    return out;
  }
  std::vector<std::string> lines;
  std::vector<std::string> osd;
};

// ---- the fake game and its snapshot store ----------------------------------------------------------------------------------------

u32 Pack(const Pad8& p)
{
  const u32 lo = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
  const u32 hi = (static_cast<u32>(p[4]) << 24) | (static_cast<u32>(p[5]) << 16) | (static_cast<u32>(p[6]) << 8) | p[7];
  return lo ^ ((hi << 13) | (hi >> 19));
}

// The "game memory": four words that evolve from both players' pads, order dependent, so any wrong input at any frame changes every
// later state. `bug_frame` is a persistent rules difference (the same on every re-run), used to provoke a desync.
struct FakeGame
{
  std::array<u32, 4> s{0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu, 1u};
  s32 bug_frame = -1;
  u32 stable = 0xC0FFEE00u;  // the "stable ranges" CRC

  void Apply(s32 frame, const Pad8& host, const Pad8& guest)
  {
    const u32 a = Pack(host);
    const u32 b = Pack(guest);
    s[0] = (s[0] ^ a) * 16777619u + b;
    s[1] = ((s[1] << 7) | (s[1] >> 25)) ^ (s[0] + b * 3u);
    s[2] += (a ^ (b << 1)) + (s[1] >> 3);
    s[3] += 1;
    if (frame == bug_frame)
      s[2] ^= 0x40;
  }

  static u32 Fnv(u32 h, u32 v)
  {
    for (int i = 0; i < 4; ++i)
    {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 16777619u;
    }
    return h;
  }
  u32 Checksum(s32 frame) const
  {
    u32 h = 2166136261u;
    h = Fnv(h, static_cast<u32>(frame));
    for (u32 w : s)
      h = Fnv(h, w);
    return h;
  }
  FieldSet Fields() const
  {
    FieldSet f;
    f.present = 3;
    for (size_t i = 0; i < 4; ++i)
    {
      f.value[0][i] = s[i];
      f.value[1][i] = s[i] ^ 0xFFu;
    }
    return f;
  }
};

class FakeStore final : public SnapshotStore
{
public:
  explicit FakeStore(FakeGame* game_, size_t cap = 7) : game(game_), capacity(cap) {}

  bool Capture(s32 frame, u32 sp, CaptureInfo* info) override
  {
    ++captures;
    if (fail_capture)
      return false;
    Slot& slot = slots[frame];
    slot.state = game->s;
    slot.sp = sp;
    info->checksum = game->Checksum(frame);
    info->fields = game->Fields();
    trace[frame] = info->fields;
    while (slots.size() > capacity)
      slots.erase(slots.begin());
    while (trace.size() > 40)
      trace.erase(trace.begin());
    return true;
  }
  bool Has(s32 frame) const override { return slots.count(frame) != 0; }
  u32 SlotSp(s32 frame) const override
  {
    const auto it = slots.find(frame);
    return it == slots.end() ? 0 : it->second.sp;
  }
  bool Restore(s32 frame) override
  {
    const auto it = slots.find(frame);
    if (it == slots.end())
      return false;
    game->s = it->second.state;
    restored.push_back(frame);
    return true;
  }
  u32 LiveChecksum(s32 frame, FieldSet* fields) override
  {
    if (fields != nullptr)
      *fields = game->Fields();
    return game->Checksum(frame);
  }
  u32 StableCrc() override { return game->stable; }
  bool Fields(s32 frame, FieldSet* out) const override
  {
    const auto it = trace.find(frame);
    if (it == trace.end())
      return false;
    *out = it->second;
    return true;
  }
  void Clear() override
  {
    slots.clear();
    ++clears;
  }
  bool RangeCrcs(s32 /*frame*/, bool /*live*/, std::vector<RangeCrc>* out) override
  {
    if (!ranges_ok)
      return false;
    *out = ranges;
    return true;
  }
  bool Relayout() override
  {
    slots.clear();
    ++relayouts;
    return relayout_ok;
  }

  struct Slot
  {
    std::array<u32, 4> state{};
    u32 sp = 0;
  };
  FakeGame* game;
  size_t capacity;
  std::map<s32, Slot> slots;
  std::map<s32, FieldSet> trace;
  std::vector<s32> restored;  // every Restore() in order
  u32 captures = 0;
  u32 clears = 0;
  u32 relayouts = 0;
  bool relayout_ok = true;
  bool ranges_ok = false;
  std::vector<RangeCrc> ranges;  // what RangeCrcs reports (REV3_NET_RANGE_CRC tests)
  bool fail_capture = false;
};

// ---- the scripted transport (unit tests) -------------------------------------------------------------------------------------------

class ScriptedTransport final : public Transport
{
public:
  struct Sent
  {
    s32 frame;
    Pad8 pad;
    s32 chk_frame;
    u32 chk;
  };

  void SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum) override
  {
    sent.push_back({frame, pad, checksum_frame, checksum});
  }
  s32 RemoteConfirmed() const override { return r; }
  bool RemotePad(s32 frame, Pad8* out) const override
  {
    if (frame > r)
      return false;
    const auto it = remote.find(frame);
    if (it == remote.end())
      return false;
    *out = it->second;
    return true;
  }
  std::vector<PeerChecksum> TakePeerChecksums() override
  {
    std::vector<PeerChecksum> out;
    out.swap(chk_in);
    return out;
  }
  void SendReliable(MsgType type, const std::vector<u8>& bytes) override { reliable_out.push_back({type, bytes}); }
  bool PollReliable(Message* out) override
  {
    if (inbox.empty())
      return false;
    *out = inbox.front();
    inbox.pop_front();
    return true;
  }
  u64 Now() const override { return now_us; }
  ConnState State() const override { return state; }
  void Pump() override { ++pumps; }

  void Deliver(s32 frame, const Pad8& pad)
  {
    remote[frame] = pad;
    while (remote.count(r + 1) != 0)
      ++r;
  }
  size_t CountOut(MsgType type) const
  {
    size_t n = 0;
    for (const Message& m : reliable_out)
      if (m.type == type)
        ++n;
    return n;
  }
  const Message* LastOut(MsgType type) const
  {
    for (size_t i = reliable_out.size(); i > 0; --i)
      if (reliable_out[i - 1].type == type)
        return &reliable_out[i - 1];
    return nullptr;
  }

  std::vector<Sent> sent;
  std::vector<Message> reliable_out;
  std::deque<Message> inbox;
  std::vector<PeerChecksum> chk_in;
  std::map<s32, Pad8> remote;
  s32 r = -1;
  u64 now_us = 0;
  ConnState state = ConnState::Connected;
  int pumps = 0;
};

Gate Open()
{
  Gate g;
  g.open = true;
  g.hardware_quiet = true;
  g.why = "test";
  return g;
}
Gate Closed()
{
  Gate g;
  g.open = false;
  g.hardware_quiet = true;
  g.why = "test";
  return g;
}
Gate Busy()
{
  Gate g = Open();
  g.hardware_quiet = false;
  return g;
}
// A fighter at health 0 in the live state (the KO that ends the fight). In ARMED this closes nothing by itself: the state is recorded with
// the capture of the frame and the close is scheduled when that frame is FINAL (THE GATE RULE).
Gate Lost()
{
  Gate g = Open();
  g.health_zero = true;
  g.why = "test";
  return g;
}
// The fighters cannot be found: nothing can be snapshotted, so the frame is never predicted.
Gate NoFighters()
{
  Gate g = Open();
  g.fighters_found = false;
  return g;
}

// One machine of the unit tests: session + scripted transport + fake game, with a tiny "game loop" (the hook counter, applying the reply).
struct Rig
{
  explicit Rig(Role role = Role::Host, int delay = 0, int maxr = 5, bool range_crc = false) : store(&game, static_cast<size_t>(maxr) + 2)
  {
    cfg.range_crc = range_crc;
    cfg.role = role;
    cfg.delay = delay;
    cfg.max_rollback = maxr;
    cfg.hello.game_id = "RNEEDA";
    cfg.hello.items = {{"cpu_core", "JIT64"}, {"dsp", "HLE"}};
    wait = [](const std::function<bool()>& pred, u32) { return pred(); };
    session = std::make_unique<RollbackSession>(
        cfg, net, store, sink, [this](const std::function<bool()>& pred, u32 timeout) { return wait(pred, timeout); });
  }

  Hello PeerHello() const
  {
    Hello h = cfg.hello;
    h.role = cfg.role == Role::Host ? Role::Guest : Role::Host;
    h.delay = static_cast<u32>(cfg.delay);
    return h;
  }
  void Handshake()
  {
    net.state = ConnState::Connected;
    net.inbox.push_back({MsgType::Hello, SerializeHello(PeerHello())});
  }
  Command Cmd(s32 frame, const Pad8& local) const
  {
    Command c;
    c.frame = static_cast<u32>(frame);
    c.pads[0] = ToPadBytes(local);
    c.pads[1] = PadBytes{0xEE, 0xEE, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0xAA, 0xBB, 0xCC, 0xDD};  // the second port: junk
    c.pad_return = 0;
    c.sp = SP;
    return c;
  }
  // One hook call at the current counter. Returns false (and changes nothing) when the session must wait.
  bool TryFrame(const Pad8& local, const Gate& gate, Reply* out = nullptr)
  {
    Reply r;
    if (!session->Step(Cmd(counter, local), gate, &r))
      return false;
    if (out != nullptr)
      *out = r;
    Apply(r);
    return true;
  }
  Reply Frame(const Pad8& local, const Gate& gate)
  {
    Reply r;
    EXPECT_TRUE(TryFrame(local, gate, &r)) << "the session blocked (block timeout " << session->BlockTimeoutMs() << " ms)";
    return r;
  }
  void Apply(const Reply& r)
  {
    if ((r.flags & REPLY_FLAG_RESTORED) != 0)
      counter = static_cast<s32>(r.frame);  // the hook takes the frame of the reply as its counter (memory was restored by the store)
    check_before[counter] = game.Checksum(counter);
    game.Apply(counter, SanitizePad(r.pads[0]), SanitizePad(r.pads[1]));
    ++counter;
  }
  // `n` lockstep frames: the remote pad is delivered first, the gate is closed.
  void Lockstep(s32 n)
  {
    for (s32 i = 0; i < n; ++i)
    {
      net.Deliver(counter, RemotePadScript(counter));
      Frame(LocalPad(counter), Closed());
    }
  }
  // The peer's START_CHECK for the current counter, equal to ours (the game state is exact in lockstep).
  void PeerStartCheck(bool equal = true)
  {
    StartCheck sc;
    sc.frame = counter;
    sc.checksum = game.Checksum(counter) ^ (equal ? 0u : 0x1u);
    sc.stable_crc = game.stable;
    sc.fields = game.Fields();
    if (!equal)
      sc.fields.value[1][2] ^= 0x1;
    net.inbox.push_back({MsgType::StartCheck, SerializeStartCheck(sc)});
  }
  // Lockstep `n` frames (counter = n), handshake and a matching START_CHECK queued: the next Frame(.., Open()) arms.
  void ReadyToArm(s32 n)
  {
    Handshake();
    Lockstep(n);
    PeerStartCheck();
  }

  Config cfg;
  FakeGame game;
  FakeStore store;
  ScriptedTransport net;
  TestSink sink;
  std::function<bool(const std::function<bool()>&, u32)> wait;
  std::unique_ptr<RollbackSession> session;
  s32 counter = 0;
  std::map<s32, u32> check_before;  // game.Checksum of each frame at its (latest) call
};

// ---- messages ---------------------------------------------------------------------------------------------------------------------

Hello SampleHello()
{
  Hello h;
  h.role = Role::Guest;
  h.game_id = "RNEEDA";
  h.delay = 3;
  h.items = {{"build", "rev3-abc123"}, {"cpu_core", "JIT64"}, {"rtc", "1262304000"}};
  return h;
}

TEST(Rev3Session, FieldsDigestMatchesTheReferenceChecksumModule)
{
  EXPECT_EQ(FieldsDigest(), 0xD4076EF2u);  // rbchecksum.fields_digest() for FIELDS_VERSION 1
  EXPECT_EQ(FieldsDigest(), FIELDS_DIGEST_V1);
}

TEST(Rev3Session, HelloRoundTripAndMalformed)
{
  const Hello h = SampleHello();
  const std::vector<u8> bytes = SerializeHello(h);
  Hello back;
  ASSERT_TRUE(ParseHello(bytes, &back));
  EXPECT_TRUE(back.role == Role::Guest);
  EXPECT_EQ(back.game_id, "RNEEDA");
  EXPECT_EQ(back.protocol, 2u);
  EXPECT_EQ(back.fields_version, 1u);
  EXPECT_EQ(back.fields_digest, 0xD4076EF2u);
  EXPECT_EQ(back.delay, 3u);
  ASSERT_EQ(back.items.size(), 3u);
  EXPECT_EQ(back.items[1].first, "cpu_core");
  EXPECT_EQ(back.items[1].second, "JIT64");
  EXPECT_TRUE(HelloMismatches(h, [&] {
                Hello other = h;
                other.role = Role::Host;
                return other;
              }()).empty());

  Hello unused;
  for (size_t cut = 0; cut < bytes.size(); ++cut)  // every truncation is rejected
  {
    const std::vector<u8> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
    EXPECT_FALSE(ParseHello(shorter, &unused)) << "cut at " << cut;
  }
  std::vector<u8> longer = bytes;
  longer.push_back(0);
  EXPECT_FALSE(ParseHello(longer, &unused));
  std::vector<u8> bad_magic = bytes;
  bad_magic[0] ^= 0xFF;
  EXPECT_FALSE(ParseHello(bad_magic, &unused));
}

TEST(Rev3Session, HelloMismatchNamesTheItems)
{
  Hello a = SampleHello();
  Hello b = SampleHello();
  b.role = Role::Host;
  EXPECT_TRUE(HelloMismatches(a, b).empty());
  b.delay = 9;  // the host's delay wins: not a mismatch
  EXPECT_TRUE(HelloMismatches(a, b).empty());

  b.game_id = "RNEEDB";
  b.protocol = 3;
  b.fields_digest = 0x1234;
  b.items[1].second = "CachedInterpreter";
  b.items.erase(b.items.begin());  // "build" missing on the peer
  b.items.emplace_back("save_hash", "0xDEAD");
  const std::vector<std::string> m = HelloMismatches(a, b);
  const auto has = [&](const std::string& part) {
    for (const std::string& s : m)
      if (s.find(part) != std::string::npos)
        return true;
    return false;
  };
  EXPECT_TRUE(has("game_id: own=RNEEDA peer=RNEEDB"));
  EXPECT_TRUE(has("protocol: own=2 peer=3"));
  EXPECT_TRUE(has("fields_digest"));
  EXPECT_TRUE(has("item cpu_core: own=JIT64 peer=CachedInterpreter"));
  EXPECT_TRUE(has("item build: own=rev3-abc123 peer=(absent)"));
  EXPECT_TRUE(has("item save_hash: own=(absent) peer=0xDEAD"));
  EXPECT_FALSE(has("rtc"));
  EXPECT_EQ(m.size(), 6u);

  Hello c = SampleHello();  // both guests
  EXPECT_TRUE(HelloMismatches(a, c)[0].find("role: both sides are guest") != std::string::npos);
}

TEST(Rev3Session, StartCheckRoundTripAndMismatchNamesTheField)
{
  FakeGame g;
  StartCheck a;
  a.frame = 77;
  a.checksum = g.Checksum(77);
  a.stable_crc = 0xABCD;
  a.fields = g.Fields();
  StartCheck back;
  ASSERT_TRUE(ParseStartCheck(SerializeStartCheck(a), &back));
  EXPECT_EQ(back.frame, 77);
  EXPECT_EQ(back.checksum, a.checksum);
  EXPECT_TRUE(back.fields == a.fields);
  EXPECT_TRUE(StartCheckMismatches(a, back).empty());

  std::vector<u8> bytes = SerializeStartCheck(a);
  bytes.pop_back();
  EXPECT_FALSE(ParseStartCheck(bytes, &back));

  StartCheck b = a;
  b.checksum ^= 1;
  b.stable_crc = 0x1;
  b.fields.value[1][33] = 0x600D;  // field 33 = "health" in FIELDS order
  const std::vector<std::string> m = StartCheckMismatches(a, b);
  bool saw_field = false;
  for (const std::string& s : m)
    if (s.find("p2 health own=0x00000000 peer=0x0000600D") != std::string::npos)
      saw_field = true;
  EXPECT_TRUE(saw_field) << "mismatches: " << m.size();
  EXPECT_GE(m.size(), 3u);  // checksum, stable crc, the field
}

TEST(Rev3Session, DesyncDumpRoundTrip)
{
  FakeGame g;
  DesyncDump d;
  d.frame = 90;
  d.trace.emplace_back(60, g.Fields());
  g.s[0] = 5;
  d.trace.emplace_back(61, g.Fields());
  DesyncDump back;
  ASSERT_TRUE(ParseDesyncDump(SerializeDesyncDump(d), &back));
  EXPECT_EQ(back.frame, 90);
  ASSERT_EQ(back.trace.size(), 2u);
  EXPECT_EQ(back.trace[1].first, 61);
  EXPECT_TRUE(back.trace[1].second == d.trace[1].second);
  EXPECT_FALSE(back.trace[0].second == back.trace[1].second);
  std::vector<u8> bytes = SerializeDesyncDump(d);
  bytes.resize(bytes.size() - 3);
  EXPECT_FALSE(ParseDesyncDump(bytes, &back));
}

// ---- the barrier -----------------------------------------------------------------------------------------------------------------

TEST(Rev3Session, BarrierBlocksUntilConnectedAndHelloMatched)
{
  Rig rig(Role::Host, 2);
  rig.net.state = ConnState::Connecting;
  EXPECT_TRUE(rig.session->mode() == Mode::CONNECTING);
  EXPECT_FALSE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 120000u);  // the barrier timeout
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 0u);  // nothing is sent before the connection is up
  EXPECT_TRUE(rig.net.sent.empty());

  rig.net.state = ConnState::Connected;  // connected, but the peer's HELLO has not arrived
  EXPECT_FALSE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 1u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_FALSE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 1u);  // sent once

  Hello sent;
  ASSERT_TRUE(ParseHello(rig.net.LastOut(MsgType::Hello)->bytes, &sent));
  EXPECT_TRUE(sent.role == Role::Host);
  EXPECT_EQ(sent.delay, 2u);
  EXPECT_EQ(sent.game_id, "RNEEDA");

  rig.net.inbox.push_back({MsgType::Hello, SerializeHello(rig.PeerHello())});
  EXPECT_TRUE(rig.session->BlockSatisfied());
  // D = 2: frames 0 and 1 are neutral on both sides, so the first two calls complete without any remote pad.
  EXPECT_TRUE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_TRUE(rig.sink.Has("HELLO ok"));
}

TEST(Rev3Session, BarrierTimeoutLeavesAndEchoesTheLocalPads)
{
  Rig rig(Role::Host, 2);
  rig.net.state = ConnState::Connecting;
  u32 asked = 0;
  rig.wait = [&](const std::function<bool()>&, u32 timeout) {
    asked = timeout;
    return false;  // the wait expires
  };
  const Command cmd = rig.Cmd(0, LocalPad(0));
  const Reply r = rig.session->OnCommand(cmd, Closed());
  EXPECT_EQ(asked, 120000u);
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(r.pads[0] == cmd.pads[0]);
  EXPECT_TRUE(r.pads[1] == cmd.pads[1]);  // the second port is echoed as the game read it
  EXPECT_TRUE(rig.sink.Has("LEAVE"));
  EXPECT_TRUE(rig.net.sent.empty());  // no pad ever went out
  EXPECT_FALSE(rig.sink.osd.empty());
}

TEST(Rev3Session, HelloMismatchRefusesTheSessionAndNamesTheItem)
{
  Rig rig(Role::Host, 2);
  Hello peer = rig.PeerHello();
  peer.items[0].second = "CachedInterpreter";
  rig.net.inbox.push_back({MsgType::Hello, SerializeHello(peer)});
  const Command cmd = rig.Cmd(0, LocalPad(0));
  Reply r;
  EXPECT_TRUE(rig.session->Step(cmd, Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(r.pads[0] == cmd.pads[0]);
  EXPECT_TRUE(rig.sink.Has("HELLO item cpu_core: own=JIT64 peer=CachedInterpreter"));
  ASSERT_EQ(rig.sink.osd.size(), 2u);  // the mismatch and the leave
  EXPECT_TRUE(rig.sink.osd[0].find("cpu_core") != std::string::npos);
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
  EXPECT_TRUE(rig.net.sent.empty());  // no pad ever went out
}

TEST(Rev3Session, TheSameRoleOnBothSidesIsAHelloMismatch)
{
  Rig rig(Role::Guest, 2);
  Hello peer = rig.PeerHello();
  peer.role = Role::Guest;
  rig.net.inbox.push_back({MsgType::Hello, SerializeHello(peer)});
  Reply r;
  EXPECT_TRUE(rig.session->Step(rig.Cmd(0, LocalPad(0)), Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("role: both sides are guest"));
}

TEST(Rev3Session, FirstCommandMustBeFrameZero)
{
  Rig rig(Role::Host, 2);
  rig.Handshake();
  Reply r;
  EXPECT_TRUE(rig.session->Step(rig.Cmd(7, LocalPad(7)), Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("not 0"));
}

TEST(Rev3Session, GuestAdoptsTheHostsDelay)
{
  Rig rig(Role::Guest, 1);  // the guest asked for 1
  Hello host = rig.PeerHello();
  host.delay = 4;
  rig.net.inbox.push_back({MsgType::Hello, SerializeHello(host)});
  EXPECT_TRUE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_EQ(rig.session->delay(), 4);
  ASSERT_GE(rig.net.sent.size(), 5u);
  for (s32 f = 0; f < 4; ++f)  // four neutral frames, then the first real pad is for frame 4
  {
    EXPECT_EQ(rig.net.sent[static_cast<size_t>(f)].frame, f);
    EXPECT_TRUE(rig.net.sent[static_cast<size_t>(f)].pad == Pad8{});
  }
  EXPECT_EQ(rig.net.sent[4].frame, 4);
  EXPECT_TRUE(rig.net.sent[4].pad == LocalPad(0));
}

// ---- input delay, ports, lockstep ------------------------------------------------------------------------------------------------

TEST(Rev3Session, InputDelayPlaysLocalPadsDFramesLater)
{
  Rig rig(Role::Host, 3);
  rig.Handshake();
  std::vector<Reply> replies;
  for (s32 f = 0; f < 3; ++f)  // frames 0..2 need no remote pad at all
    replies.push_back(rig.Frame(LocalPad(f), Closed()));
  for (s32 f = 0; f < 3; ++f)
  {
    EXPECT_TRUE(SanitizePad(replies[static_cast<size_t>(f)].pads[0]) == Pad8{}) << "local neutral at frame " << f;
    EXPECT_TRUE(SanitizePad(replies[static_cast<size_t>(f)].pads[1]) == Pad8{});
  }
  // Frame 3 plays the pad read at call 0, and needs the remote pad of frame 3.
  EXPECT_FALSE(rig.TryFrame(LocalPad(3), Closed()));
  rig.net.Deliver(0, Pad8{});
  rig.net.Deliver(1, Pad8{});
  rig.net.Deliver(2, Pad8{});
  rig.net.Deliver(3, P(0x77));
  const Reply r3 = rig.Frame(LocalPad(3), Closed());
  EXPECT_TRUE(SanitizePad(r3.pads[0]) == LocalPad(0));
  EXPECT_TRUE(SanitizePad(r3.pads[1]) == P(0x77));
  // The wire: neutral frames 0..2, then call 0's pad for frame 3, call 1's for 4 ... (the retried call 3 did not send twice).
  ASSERT_EQ(rig.net.sent.size(), 3u + 4u);
  EXPECT_EQ(rig.net.sent[3].frame, 3);
  EXPECT_TRUE(rig.net.sent[3].pad == LocalPad(0));
  EXPECT_EQ(rig.net.sent[6].frame, 6);
  EXPECT_TRUE(rig.net.sent[6].pad == LocalPad(3));
}

TEST(Rev3Session, GuestPlaysGamePortOneAndHostPortZero)
{
  Rig host(Role::Host, 0);
  host.Handshake();
  host.net.Deliver(0, P(0x55));
  const Reply rh = host.Frame(P(0x66), Closed());
  EXPECT_TRUE(SanitizePad(rh.pads[0]) == P(0x66));  // host: local on game port 0
  EXPECT_TRUE(SanitizePad(rh.pads[1]) == P(0x55));

  Rig guest(Role::Guest, 0);
  guest.Handshake();
  guest.net.Deliver(0, P(0x55));
  const Reply rg = guest.Frame(P(0x66), Closed());
  EXPECT_TRUE(SanitizePad(rg.pads[1]) == P(0x66));  // guest: local on game port 1
  EXPECT_TRUE(SanitizePad(rg.pads[0]) == P(0x55));
}

TEST(Rev3Session, ReplyPadsAreEightBytesWithErrZero)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.net.Deliver(0, P(1, 2, 3));
  Command cmd = rig.Cmd(0, P(9, 8, 7));
  cmd.pads[0][8] = 0xFF;  // analogA/B and err of the local block are not forwarded
  cmd.pads[0][9] = 0xFF;
  cmd.pads[0][10] = 0xFF;
  cmd.pads[0][11] = 0xFF;
  Reply r;
  ASSERT_TRUE(rig.session->Step(cmd, Closed(), &r));
  for (size_t port = 0; port < 2; ++port)
    for (size_t i = 8; i < 12; ++i)
      EXPECT_EQ(r.pads[port][i], 0) << "port " << port << " byte " << i;
  ASSERT_EQ(rig.net.sent.size(), 1u);
  EXPECT_TRUE(rig.net.sent[0].pad == P(9, 8, 7));
}

TEST(Rev3Session, StalePadReadReusesThePreviousLocalPad)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.net.Deliver(0, P(1));
  rig.net.Deliver(1, P(2));
  rig.Frame(P(0x31), Closed());
  Command cmd = rig.Cmd(1, P(0x99));
  cmd.pad_return = -3;  // PADRead failed: the block holds the previous poll
  Reply r;
  ASSERT_TRUE(rig.session->Step(cmd, Closed(), &r));
  EXPECT_TRUE(SanitizePad(r.pads[0]) == P(0x31));
  EXPECT_TRUE(rig.net.sent.back().pad == P(0x31));
}

TEST(Rev3Session, LockstepWaitsForTheRemotePadOfEveryFrame)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  EXPECT_FALSE(rig.TryFrame(LocalPad(0), Closed()));
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 10000u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  const int pumps = rig.net.pumps;
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_GT(rig.net.pumps, pumps);  // every look at the transport while waiting pumps its resend timer
  rig.net.Deliver(0, P(5));
  EXPECT_TRUE(rig.session->BlockSatisfied());
  const Reply r = rig.Frame(LocalPad(0), Closed());
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(r.frame, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(5));
  // never predicted, never captured: no snapshots in lockstep
  EXPECT_EQ(rig.store.captures, 0u);
  EXPECT_FALSE(rig.TryFrame(LocalPad(1), Closed()));
  rig.net.Deliver(2, P(6));  // a gap: frame 1 is missing, R stays 0
  EXPECT_FALSE(rig.TryFrame(LocalPad(1), Closed()));
}

TEST(Rev3Session, LockstepTimeoutLeavesTheSession)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.wait = [](const std::function<bool()>&, u32) { return false; };
  const Command cmd = rig.Cmd(0, LocalPad(0));
  const Reply r = rig.session->OnCommand(cmd, Closed());
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(r.pads[1] == cmd.pads[1]);
  EXPECT_TRUE(rig.sink.Has("no remote pad for 10000 ms"));
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
  // And it stays offline: later calls just echo.
  Reply r2;
  EXPECT_TRUE(rig.session->Step(rig.Cmd(1, LocalPad(1)), Open(), &r2));
  EXPECT_TRUE(r2.pads[1] == rig.Cmd(1, LocalPad(1)).pads[1]);
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
}

TEST(Rev3Session, OnCommandLoopsStepWithTheInjectedWait)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  int waits = 0;
  rig.wait = [&](const std::function<bool()>& pred, u32 timeout) {
    ++waits;
    EXPECT_EQ(timeout, 10000u);
    EXPECT_FALSE(pred());                  // nothing there yet
    rig.net.Deliver(0, P(0x42));           // the packet arrives while we sleep
    return pred();                         // the wait ends because the predicate became true
  };
  const Reply r = rig.session->OnCommand(rig.Cmd(0, LocalPad(0)), Closed());
  EXPECT_EQ(waits, 1);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0x42));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
}

TEST(Rev3Session, FrameCounterJumpLeavesTheSession)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(3);
  Reply r;
  EXPECT_TRUE(rig.session->Step(rig.Cmd(9, LocalPad(9)), Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("frame jump"));
}

TEST(Rev3Session, PeerByeAndLostConnectionLeave)
{
  Rig a(Role::Host, 0);
  a.Handshake();
  a.Lockstep(2);
  a.net.inbox.push_back({MsgType::Bye, std::vector<u8>{'h', 'i'}});
  Reply r;
  EXPECT_TRUE(a.session->Step(a.Cmd(2, LocalPad(2)), Closed(), &r));
  EXPECT_TRUE(a.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(a.sink.Has("BYE from the peer: hi"));
  EXPECT_EQ(a.net.CountOut(MsgType::Bye), 0u);  // no echo of the goodbye

  Rig b(Role::Host, 0);
  b.Handshake();
  b.Lockstep(2);
  b.net.state = ConnState::Failed;
  EXPECT_TRUE(b.session->Step(b.Cmd(2, LocalPad(2)), Closed(), &r));
  EXPECT_TRUE(b.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(b.sink.Has("connection lost"));
}

TEST(Rev3Session, AbortLeavesWithTheGivenReasonAndTheLateTimeoutIsSilent)
{
  // The glue calls Abort() when the emulator is stopping while a wait runs; the wait then ends and BlockTimedOut() must not log a timeout.
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(2);
  Reply r;
  EXPECT_FALSE(rig.session->Step(rig.Cmd(2, LocalPad(2)), Closed(), &r));  // no remote pad yet: the session waits
  rig.session->Abort("emulator stopping");
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("emulator stopping"));
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
  const size_t lines = rig.sink.lines.size();
  rig.session->BlockTimedOut();
  EXPECT_EQ(rig.sink.lines.size(), lines);
  EXPECT_TRUE(rig.session->Step(rig.Cmd(2, LocalPad(2)), Closed(), &r));  // offline: plain pass-through
}

TEST(Rev3Session, HelloGoesOutWhenTheConnectionComesUpDuringTheWait)
{
  // Found by the loopback run over real UDP: the CPU thread waits at the frame-0 barrier and calls Step() again only once the wait is
  // satisfied, so the HELLO must be sent from the wait itself when the connection comes up there, or both sides wait for each other.
  Rig rig(Role::Host, 0);
  rig.net.state = ConnState::Connecting;
  Reply r;
  EXPECT_FALSE(rig.session->Step(rig.Cmd(0, LocalPad(0)), Closed(), &r));  // not connected yet
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 0u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  rig.net.state = ConnState::Connected;
  EXPECT_FALSE(rig.session->BlockSatisfied());  // our HELLO goes out here; the peer's has not arrived
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 1u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 1u);  // only once
  rig.net.inbox.push_back({MsgType::Hello, SerializeHello(rig.PeerHello())});
  EXPECT_TRUE(rig.session->BlockSatisfied());
  rig.net.Deliver(0, RemotePadScript(0));
  EXPECT_TRUE(rig.session->Step(rig.Cmd(0, LocalPad(0)), Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_EQ(rig.net.CountOut(MsgType::Hello), 1u);
}

TEST(Rev3Session, FieldsFromRawAndChecksumOfFields)
{
  // Raw big-endian bytes -> FieldSet -> rbchecksum.py's CRC stream (u32 BE frame, per present player u32 BE id then the words).
  std::array<std::array<u8, FIELD_BYTES>, 2> raw{};
  for (size_t p = 0; p < 2; ++p)
    for (size_t i = 0; i < FIELD_BYTES; ++i)
      raw[p][i] = static_cast<u8>(i * 3 + p * 100 + 1);
  const FieldSet both = FieldsFromRaw(raw, 3);
  EXPECT_EQ(both.present, 3);
  EXPECT_EQ(both.value[0][0], 0x0104070Au);
  EXPECT_EQ(both.value[1][0], 0x65686B6Eu);
  const FieldSet one = FieldsFromRaw(raw, 2 | 4);  // bits above 1 are ignored; player 1 absent
  EXPECT_EQ(one.present, 2);
  EXPECT_EQ(one.value[0][0], 0u);
  EXPECT_EQ(one.value[1][5], both.value[1][5]);

  std::vector<u8> stream;
  const auto put = [&stream](u32 v) {
    stream.insert(stream.end(), {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)});
  };
  put(77);
  put(1);
  for (u32 v : both.value[0])
    put(v);
  put(2);
  for (u32 v : both.value[1])
    put(v);
  EXPECT_EQ(ChecksumOfFields(77, both), Common::ComputeCRC32(stream.data(), stream.size()));
  EXPECT_NE(ChecksumOfFields(78, both), ChecksumOfFields(77, both));
  EXPECT_NE(ChecksumOfFields(77, one), ChecksumOfFields(77, both));
  FieldSet none;
  std::vector<u8> frame_only;
  frame_only.insert(frame_only.end(), {0, 0, 0, 7});
  EXPECT_EQ(ChecksumOfFields(7, none), Common::ComputeCRC32(frame_only.data(), frame_only.size()));
}

// ---- arming ----------------------------------------------------------------------------------------------------------------------

TEST(Rev3Session, ArmingNeedsThePeersMatchingStartCheck)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  // Gate open, our START_CHECK goes out, the peer's is not here yet: wait.
  EXPECT_FALSE(rig.TryFrame(LocalPad(10), Open()));
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 10000u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  StartCheck mine;
  ASSERT_TRUE(ParseStartCheck(rig.net.LastOut(MsgType::StartCheck)->bytes, &mine));
  EXPECT_EQ(mine.frame, 10);
  EXPECT_EQ(mine.checksum, rig.game.Checksum(10));
  rig.PeerStartCheck();
  EXPECT_TRUE(rig.session->BlockSatisfied());
  rig.net.Deliver(10, RemotePadScript(10));
  EXPECT_TRUE(rig.TryFrame(LocalPad(10), Open()));
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);  // retries do not send again
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_TRUE(rig.sink.Has("GATE open frame=10 why=test"));
  EXPECT_EQ(rig.store.captures, 1u);
  EXPECT_EQ(rig.session->stats().gate_opens, 1u);
}

TEST(Rev3Session, StartCheckMismatchRefusesToArmAndNamesTheDifference)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.PeerStartCheck(/*equal=*/false);
  rig.net.Deliver(10, RemotePadScript(10));
  EXPECT_TRUE(rig.TryFrame(LocalPad(10), Open()));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);  // still in lockstep, the frame was answered exactly
  EXPECT_EQ(rig.session->stats().arm_refused, 1u);
  EXPECT_TRUE(rig.sink.Has("refused: START_CHECK mismatch"));
  EXPECT_TRUE(rig.sink.Has("START_CHECK checksum:"));
  EXPECT_TRUE(rig.sink.Has("START_CHECK field p2"));
  EXPECT_FALSE(rig.sink.osd.empty());
  EXPECT_EQ(rig.store.captures, 0u);

  // The gate stays open: no new attempt (no second START_CHECK) until it has closed once.
  rig.net.Deliver(11, RemotePadScript(11));
  EXPECT_TRUE(rig.TryFrame(LocalPad(11), Open()));
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  rig.net.Deliver(12, RemotePadScript(12));
  rig.Frame(LocalPad(12), Closed());
  rig.PeerStartCheck();  // for frame 13
  rig.net.Deliver(13, RemotePadScript(13));
  rig.Frame(LocalPad(13), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 2u);
}

TEST(Rev3Session, StartCheckForAnotherFrameRefusesToArm)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.counter = 10;
  StartCheck sc;
  sc.frame = 12;  // the peer's gate opened two frames later than ours
  sc.checksum = rig.game.Checksum(12);
  sc.stable_crc = rig.game.stable;
  sc.fields = rig.game.Fields();
  rig.net.inbox.push_back({MsgType::StartCheck, SerializeStartCheck(sc)});
  rig.net.Deliver(10, RemotePadScript(10));
  EXPECT_TRUE(rig.TryFrame(LocalPad(10), Open()));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_TRUE(rig.sink.Has("START_CHECK frame mismatch (own=10 peer=12)"));
}

TEST(Rev3Session, StartCheckTimeoutOnlyRefusesToArm)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.wait = [&](const std::function<bool()>&, u32 timeout) {
    EXPECT_EQ(timeout, 10000u);
    return false;
  };
  const Reply r = rig.session->OnCommand(rig.Cmd(10, LocalPad(10)), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);  // not left
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == RemotePadScript(10));
  EXPECT_TRUE(rig.sink.Has("no START_CHECK from the peer"));
  EXPECT_EQ(rig.session->stats().arm_refused, 1u);
}

// ---- prediction, misprediction, rollback, resimulation ---------------------------------------------------------------------------

TEST(Rev3Session, PredictsTheLastConfirmedRemotePadAndKeepsItWhenRight)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  const Pad8 last = RemotePadScript(9);
  for (s32 f = 10; f <= 12; ++f)  // no remote pad since frame 9
  {
    const Reply r = rig.Frame(LocalPad(f), Open());
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.frame, static_cast<u32>(f));
    EXPECT_TRUE(SanitizePad(r.pads[1]) == last) << "frame " << f;
    EXPECT_TRUE(SanitizePad(r.pads[0]) == LocalPad(f));
  }
  EXPECT_EQ(rig.session->stats().predicted_frames, 3u);
  // The real pads turn out to be what was predicted: nothing is rolled back.
  rig.net.Deliver(10, last);
  rig.net.Deliver(11, last);
  rig.net.Deliver(12, last);
  rig.Frame(LocalPad(13), Open());
  EXPECT_EQ(rig.session->stats().rollbacks, 0u);
  EXPECT_TRUE(rig.store.restored.empty());
  EXPECT_EQ(rig.session->verified_frame(), 12);
}

TEST(Rev3Session, MispredictionRollsBackToTheFirstWrongFrameAndResimulates)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  const Pad8 predicted = RemotePadScript(9);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  const Pad8 real10 = P(0xA1, 0xA2);
  ASSERT_FALSE(real10 == predicted);
  rig.net.Deliver(10, real10);

  // Call 13: the scan finds frame 10 wrong -> restore its snapshot, RESTORED reply naming 10 with the pads FOR 10.
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));  // note: Apply() has already moved the rig's counter to 11
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
  EXPECT_TRUE(SanitizePad(r.pads[0]) == LocalPad(10));
  EXPECT_TRUE(SanitizePad(r.pads[1]) == real10);
  ASSERT_EQ(rig.store.restored.size(), 1u);
  EXPECT_EQ(rig.store.restored[0], 10);
  EXPECT_TRUE(rig.session->Resimulating());
  EXPECT_EQ(rig.counter, 11);
  EXPECT_TRUE(rig.sink.Has("ROLLBACK depth=3 frame=10 live=13"));
  EXPECT_EQ(rig.session->stats().rollbacks, 1u);
  EXPECT_EQ(rig.session->stats().max_depth, 3u);
  EXPECT_EQ(rig.session->stats().depth_hist[3], 1u);

  // The game re-runs 11, 12, 13 as ordinary hook calls. The junk pad (live controller) must be ignored: stored values are replayed.
  for (s32 m = 11; m <= 13; ++m)
  {
    EXPECT_TRUE(rig.session->Resimulating());
    const Reply rr = rig.Frame(P(0xFF, 0xFF), Open());
    EXPECT_EQ(rr.flags, 0u);
    EXPECT_EQ(rr.frame, static_cast<u32>(m));
    EXPECT_TRUE(SanitizePad(rr.pads[0]) == LocalPad(m)) << "frame " << m;      // the pad read at call m, not the junk
    EXPECT_TRUE(SanitizePad(rr.pads[1]) == real10) << "frame " << m;          // predicted again: the last confirmed pad is now frame 10's
  }
  EXPECT_FALSE(rig.session->Resimulating());
  EXPECT_EQ(rig.counter, 14);
  EXPECT_EQ(rig.session->stats().resim_calls, 3u);
  // Nothing was sent twice: one pad per fresh call only (frames 10..13 -> wire frames 10..13, plus the 10 lockstep ones)
  EXPECT_EQ(rig.net.sent.size(), 14u);

  // Next normal call: frame 10..13 verified against what is known (R = 10), no new rollback.
  rig.Frame(LocalPad(14), Open());
  EXPECT_EQ(rig.session->stats().rollbacks, 1u);
  EXPECT_EQ(rig.session->verified_frame(), 10);
}

TEST(Rev3Session, ResimulationEndsInTheStateOfARunThatHadTheRemotePadsInTime)
{
  // Same inputs through two sessions: one sees the remote pads late (prediction, rollback, resimulation), one has them in time.
  const auto run = [](bool late) {
    Rig rig(Role::Host, 0, 5);
    rig.ReadyToArm(10);
    for (s32 f = 10; f <= 12; ++f)
    {
      if (!late)
        rig.net.Deliver(f, P(static_cast<u8>(0xB0 + f)));
      rig.Frame(LocalPad(f), Open());
    }
    if (late)
      for (s32 f = 10; f <= 12; ++f)
        rig.net.Deliver(f, P(static_cast<u8>(0xB0 + f)));
    for (s32 f = 13; f <= 20; ++f)
    {
      rig.net.Deliver(f, P(static_cast<u8>(0xB0 + f)));
      rig.Frame(LocalPad(rig.counter), Open());  // the counter jumps back on a rollback: drive until the live frame is done again
      while (rig.session->Resimulating())
        rig.Frame(LocalPad(rig.counter), Open());
    }
    return std::make_pair(rig.game.s, rig.session->stats().rollbacks);
  };
  const auto timely = run(false);
  const auto late = run(true);
  EXPECT_EQ(timely.second, 0u);
  EXPECT_GE(late.second, 1u);
  EXPECT_TRUE(timely.first == late.first);
}

TEST(Rev3Session, ResimWithAWrongFrameFallsBackToLockstepLoudly)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, P(0xA1));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_TRUE(rig.session->Resimulating());
  // The game did not honour the restore and sends frame 14 instead of 11.
  rig.counter = 14;
  rig.net.Deliver(11, P(1));
  rig.net.Deliver(12, P(1));
  rig.net.Deliver(13, P(1));
  rig.net.Deliver(14, P(0x14));
  const u32 clears = rig.store.clears;
  EXPECT_TRUE(rig.TryFrame(LocalPad(14), Closed()));
  EXPECT_TRUE(rig.sink.Has("ERROR RESIM expected frame 11, the game sent 14"));
  EXPECT_FALSE(rig.session->Resimulating());
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_GT(rig.store.clears, clears);
}

TEST(Rev3Session, RollbackSpMismatchAndMissingSnapshotAreDesyncClassErrors)
{
  {
    Rig rig(Role::Host, 0, 5);
    rig.ReadyToArm(10);
    for (s32 f = 10; f <= 11; ++f)
      rig.Frame(LocalPad(f), Open());
    rig.net.Deliver(10, P(0xA1));
    Command cmd = rig.Cmd(12, LocalPad(12));
    cmd.sp = SP - 0x50;  // a different call chain
    Reply r;
    EXPECT_TRUE(rig.session->Step(cmd, Open(), &r));
    EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
    EXPECT_TRUE(rig.sink.Has("ERROR ROLLBACK frame=10"));
    EXPECT_TRUE(rig.store.restored.empty());
  }
  {
    Rig rig(Role::Host, 0, 5);
    rig.ReadyToArm(10);
    for (s32 f = 10; f <= 11; ++f)
      rig.Frame(LocalPad(f), Open());
    rig.store.slots.clear();  // the store lost the slot
    rig.net.Deliver(10, P(0xA1));
    Reply r;
    EXPECT_TRUE(rig.session->Step(rig.Cmd(12, LocalPad(12)), Open(), &r));
    EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
    EXPECT_TRUE(rig.sink.Has("no snapshot for the target"));
  }
  {
    Rig rig(Role::Host, 0, 5);
    rig.ReadyToArm(10);
    rig.store.fail_capture = true;
    Reply r;
    EXPECT_TRUE(rig.session->Step(rig.Cmd(10, LocalPad(10)), Open(), &r));
    EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
    EXPECT_TRUE(rig.sink.Has("snapshot capture failed"));
  }
}

TEST(Rev3Session, ALostStateInAResimulatedCallDrainsNothingAndRecordsTheNewTimeline)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, P(0xA1));
  rig.net.Deliver(11, P(0xA2));
  rig.net.Deliver(12, P(0xA3));
  rig.Frame(LocalPad(13), Open());  // RESTORED
  ASSERT_TRUE(rig.session->Resimulating());
  // Re-running frames: the live state may look different from the old timeline (here: health 0). That only changes what is recorded for the
  // frame; nothing drains because of a resimulated call.
  for (s32 m = 11; m <= 13; ++m)
    rig.Frame(LocalPad(m), Lost());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().gate_closes, 0u);
  // A normal call right after it carries on ARMED.
  EXPECT_TRUE(rig.TryFrame(LocalPad(14), Open()));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().gate_closes, 0u);
}

// ---- the stall rule --------------------------------------------------------------------------------------------------------------

TEST(Rev3Session, MoreThanMaxrPredictedFramesStall)
{
  Rig rig(Role::Host, 0, 3);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)  // 1, 2, 3 predicted frames: allowed
    rig.Frame(LocalPad(f), Open());
  EXPECT_FALSE(rig.TryFrame(LocalPad(13), Open()));  // N - R = 13 - 9 = 4 > 3
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 7000u);  // 420 frames of 1/60 s
  EXPECT_EQ(rig.session->stats().stalls, 1u);
  EXPECT_FALSE(rig.TryFrame(LocalPad(13), Open()));
  EXPECT_EQ(rig.session->stats().stalls, 1u);  // a retry is the same stall
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_TRUE(rig.sink.Has("STALL start frame=13 R=9 predicted=4 maxr=3"));
  rig.net.now_us = 5000;
  rig.net.Deliver(10, RemotePadScript(9));  // R = 10 -> N - R = 3
  EXPECT_TRUE(rig.session->BlockSatisfied());
  rig.net.now_us = 12000;
  EXPECT_TRUE(rig.TryFrame(LocalPad(13), Open()));
  EXPECT_TRUE(rig.sink.Has("STALL end frame=13 ms=12.0"));
  EXPECT_EQ(rig.session->stats().stall_us_max, 12000u);
}

TEST(Rev3Session, AStallOf420FramesEndsTheSession)
{
  Rig rig(Role::Host, 0, 2);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  u32 asked = 0;
  rig.wait = [&](const std::function<bool()>&, u32 timeout) {
    asked = timeout;
    rig.net.now_us += static_cast<u64>(timeout) * 1000;
    return false;
  };
  const Command cmd = rig.Cmd(12, LocalPad(12));
  const Reply r = rig.session->OnCommand(cmd, Open());
  EXPECT_EQ(asked, 7000u);
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("STALL timeout"));
  EXPECT_TRUE(rig.sink.Has("stall: no remote pad for 420 frames"));
  EXPECT_TRUE(r.pads[1] == cmd.pads[1]);
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
}

TEST(Rev3Session, RollbackDepthNeverExceedsMaxrEvenWhenPadsArriveDuringAStall)
{
  // MAXR 3. Frames 10..12 predicted; at call 13 the session stalls; pads for 10..12 arrive (11 wrong) during the stall. Because the stall
  // ends before the scan, the scan sees them and rolls back to 11 (depth 2); it never needs to go further back than N - MAXR.
  Rig rig(Role::Host, 0, 3);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  EXPECT_FALSE(rig.TryFrame(LocalPad(13), Open()));
  const Pad8 predicted = RemotePadScript(9);
  rig.net.Deliver(10, predicted);
  rig.net.Deliver(11, P(0xCC));
  rig.net.Deliver(12, predicted);
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 11u);
  EXPECT_LE(rig.session->stats().max_depth, 3u);
  EXPECT_EQ(rig.session->stats().max_depth, 2u);
}

// ---- hardware busy ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Session, BusyHardwarePostponesARollbackUntilItWouldExceedMaxr)
{
  Rig rig(Role::Host, 0, 4);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 11; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, P(0xA1));  // frame 10 was mispredicted
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(12), Busy(), &r));  // depth 2 < 4: postponed, normal reply
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(rig.session->stats().postponed, 1u);
  EXPECT_TRUE(rig.store.restored.empty());
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Busy(), &r));  // depth 3 < 4
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(rig.session->stats().postponed, 2u);
  ASSERT_TRUE(rig.TryFrame(LocalPad(14), Busy(), &r));  // depth 4 == MAXR: forced
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
  EXPECT_EQ(rig.session->stats().forced_hw, 1u);
  EXPECT_EQ(rig.session->stats().max_depth, 4u);
  EXPECT_TRUE(rig.sink.Has("forced=hardware_busy"));
}

TEST(Rev3Session, QuietHardwareRollsBackAtOnce)
{
  Rig rig(Role::Host, 0, 4);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(10, P(0xA1));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(11), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  EXPECT_EQ(rig.session->stats().postponed, 0u);
}

// ---- drain -----------------------------------------------------------------------------------------------------------------------

// Plays an armed session whose frame 10 (final at once: everything below it was played in lockstep) is recorded as lost, so the close is
// scheduled for frame 16 = 10 + MAXR + 1. Every remote pad equals its prediction. Ends in LOCKSTEP with the counter at 17.
void PlayALostFrameToTheDrain(Rig& rig)
{
  rig.ReadyToArm(10);
  const Pad8 predicted = RemotePadScript(9);
  rig.Frame(LocalPad(10), Lost());
  for (s32 f = 11; f <= 14; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, predicted);
  rig.Frame(LocalPad(15), Open());
  for (s32 f = 11; f <= 15; ++f)
    rig.net.Deliver(f, predicted);
  rig.net.Deliver(16, P(0x16));
  rig.Frame(LocalPad(16), Open());
}

TEST(Rev3Session, ALostFinalFrameClosesTheGateMaxrPlusOneFramesLaterDrainsRollsBackAndContinuesInLockstep)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  const Pad8 predicted = RemotePadScript(9);
  // Frame 10 is final when it is played (all remote pads below it are confirmed): the close is scheduled at once, for the same frame the peer
  // will pick, 10 + MAXR + 1 = 16.
  rig.Frame(LocalPad(10), Lost());
  EXPECT_TRUE(rig.sink.Has("GATE close scheduled final_frame=10 effective_frame=16"));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  rig.Frame(LocalPad(11), Open());
  rig.Frame(LocalPad(12), Open());
  rig.net.Deliver(10, predicted);
  rig.net.Deliver(11, predicted);
  const Pad8 real12 = P(0xA1);
  rig.net.Deliver(12, real12);  // frame 12 was mispredicted; busy hardware postpones the rollback until the close
  for (s32 f = 13; f <= 15; ++f)
    rig.Frame(LocalPad(f), Busy());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().gate_closes, 0u);
  EXPECT_TRUE(rig.store.restored.empty());

  // Call 16: both machines enter DRAIN here. Wait for every remote input below 16 (R >= 15).
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Open()));
  EXPECT_TRUE(rig.sink.Has("GATE close frame=16 why=final frame 10: a fighter at health 0"));
  EXPECT_TRUE(rig.session->mode() == Mode::DRAIN);
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 10000u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  for (s32 f = 13; f <= 15; ++f)
    rig.net.Deliver(f, predicted);
  EXPECT_TRUE(rig.session->BlockSatisfied());

  // The rollback that is still due (frame 12) is done, whatever the hardware says: a drain cannot wait.
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(16), Busy(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 12u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == real12);
  rig.Frame(LocalPad(13), Open());  // resim 13, 14, 15 with confirmed pads (the gate is not looked at)
  rig.Frame(LocalPad(14), Open());
  const Reply r15 = rig.Frame(LocalPad(15), Open());
  EXPECT_TRUE(SanitizePad(r15.pads[1]) == predicted);
  // The second run of 16 needs the pad of 16 itself: lockstep.
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Open()));
  EXPECT_FALSE(rig.session->BlockSatisfied());
  rig.net.Deliver(16, P(0xD3));
  const u32 clears = rig.store.clears;
  const Reply r16 = rig.Frame(LocalPad(16), Open());
  EXPECT_EQ(r16.flags, 0u);
  EXPECT_TRUE(SanitizePad(r16.pads[1]) == P(0xD3));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_FALSE(rig.session->Resimulating());
  EXPECT_GT(rig.store.clears, clears);  // the ring is cleared
  EXPECT_TRUE(rig.sink.Has("DRAIN done frame=16"));
  EXPECT_EQ(rig.session->stats().gate_closes, 1u);
  EXPECT_EQ(rig.session->stats().gate_late, 0u);

  // From here on: plain lockstep, nothing predicted.
  EXPECT_FALSE(rig.TryFrame(LocalPad(17), Closed()));
  rig.net.Deliver(17, P(1));
  rig.Frame(LocalPad(17), Closed());
  EXPECT_EQ(rig.session->stats().predicted_frames, 6u);  // frames 10..15 before the rollback, none since
}

TEST(Rev3Session, ADrainWithNothingWrongJustWaitsAndSwitchesToLockstep)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  const Pad8 predicted = RemotePadScript(9);
  rig.Frame(LocalPad(10), Lost());
  for (s32 f = 11; f <= 14; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, predicted);
  rig.Frame(LocalPad(15), Open());
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Open()));  // the close: needs R >= 15
  rig.net.Deliver(11, predicted);
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Open()));
  for (s32 f = 12; f <= 15; ++f)
    rig.net.Deliver(f, predicted);
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Open()));  // and then R >= 16 for the frame itself
  rig.net.Deliver(16, P(0x16));
  const Reply r = rig.Frame(LocalPad(16), Open());
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0x16));
  EXPECT_EQ(rig.session->stats().rollbacks, 0u);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
}

TEST(Rev3Session, TheGateCanOpenAgainAfterADrain)
{
  Rig rig(Role::Host, 0, 5);
  PlayALostFrameToTheDrain(rig);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  rig.Lockstep(3);       // frames 17..19
  rig.PeerStartCheck();  // frame 20
  rig.net.Deliver(20, RemotePadScript(20));
  rig.Frame(LocalPad(20), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().gate_opens, 2u);
}

TEST(Rev3Session, TheCloseOfALostFrameIsScheduledOnlyOnce)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Lost());
  rig.Frame(LocalPad(11), Lost());  // a second lost frame before the close: no second schedule
  rig.Frame(LocalPad(12), Lost());
  EXPECT_EQ(rig.sink.Count("GATE close scheduled"), 1u);
}

// ---- the deterministic gate (M4) ---------------------------------------------------------------------------------------------------

TEST(Rev3Session, AMispredictedLiveStateThatLooksLostNeverClosesTheGate)
{
  // Machine A predicted frame 10 wrongly and its live state at call 11 shows a fighter at health 0 (a state the real game never reaches).
  // Machine B predicted right and never saw it. A gate that follows the live state would close on A alone; the final-frame rule closes on
  // neither: the frame is re-recorded by the resimulation before it is ever final.
  Rig a(Role::Host, 0, 5);
  Rig b(Role::Guest, 0, 5);
  a.ReadyToArm(10);
  b.ReadyToArm(10);
  a.Frame(LocalPad(10), Open());
  a.Frame(LocalPad(11), Lost());  // the live (wrong) timeline
  a.Frame(LocalPad(12), Open());
  b.Frame(LocalPad(10), Open());
  b.Frame(LocalPad(11), Open());
  b.Frame(LocalPad(12), Open());
  EXPECT_TRUE(a.session->mode() == Mode::ARMED);  // the live flag alone changed nothing
  a.net.Deliver(10, P(0xA1));                     // the misprediction
  a.net.Deliver(11, P(0xA1));
  a.net.Deliver(12, P(0xA1));
  Reply r;
  ASSERT_TRUE(a.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  for (s32 f = 11; f <= 13; ++f)  // the true timeline has no lost fighter
    a.Frame(LocalPad(f), Open());
  for (s32 f = 14; f <= 16; ++f)
  {
    a.net.Deliver(f - 1, P(0xA1));
    a.Frame(LocalPad(f), Open());
    b.net.Deliver(f - 4, P(0xA1));
  }
  EXPECT_FALSE(a.sink.Has("GATE close"));
  EXPECT_EQ(a.session->stats().gate_closes, 0u);
  EXPECT_TRUE(a.session->mode() == Mode::ARMED);
  EXPECT_TRUE(b.session->mode() == Mode::ARMED);
}

TEST(Rev3Session, ALostFrameThatIsMispredictedAtFirstClosesWhenTheTrueTimelineMakesItFinal)
{
  // The other direction: the live prediction showed nothing, the true timeline has a fighter at health 0 at frame 11. The close is
  // scheduled from the FINAL record (after the resimulation), for 11 + MAXR + 1 = 17, on this machine and on its peer alike.
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  EXPECT_FALSE(rig.sink.Has("GATE close scheduled"));
  rig.net.Deliver(10, P(0xA1));  // frame 10 was mispredicted
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(12), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  rig.Frame(LocalPad(11), Lost());  // resim: the true state of frame 11
  rig.net.Deliver(11, P(0xA1));
  rig.Frame(LocalPad(12), Open());  // second run of 12
  EXPECT_FALSE(rig.sink.Has("GATE close scheduled"));  // 11 is not final yet: pad 11 only just arrived and frame 12 was predicted
  rig.net.Deliver(12, P(0xA1));
  rig.Frame(LocalPad(13), Open());
  EXPECT_TRUE(rig.sink.Has("GATE close scheduled final_frame=11 effective_frame=17"));
}

TEST(Rev3Session, NoFightersMeansNoPredictionAndAScheduledClose)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  // No fighters at call 11: nothing can be snapshotted, so the frame cannot be predicted; it waits for the real pad.
  EXPECT_FALSE(rig.TryFrame(LocalPad(11), NoFighters()));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  rig.net.Deliver(10, RemotePadScript(9));
  EXPECT_FALSE(rig.TryFrame(LocalPad(11), NoFighters()));
  rig.net.Deliver(11, P(0x11));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(11), NoFighters(), &r));
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0x11));
  EXPECT_EQ(rig.session->stats().predicted_frames, 1u);  // only frame 10
  EXPECT_TRUE(rig.sink.Has("GATE close scheduled final_frame=11 effective_frame=17 why=final frame 11: fighters not found"));
}

TEST(Rev3Session, ASustainedBusyHardwareHoldsPredictionsInsteadOfClosingTheGate)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  Gate hold = Busy();
  hold.hardware_hold = true;
  // HOLD: the call waits for the real pad of its own frame; nothing is predicted.
  EXPECT_FALSE(rig.TryFrame(LocalPad(11), hold));
  EXPECT_TRUE(rig.sink.Has("GATE hold frame=11"));
  rig.net.Deliver(10, RemotePadScript(9));
  EXPECT_FALSE(rig.TryFrame(LocalPad(11), hold));
  rig.net.Deliver(11, P(0x11));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(11), hold, &r));
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0x11));
  EXPECT_EQ(rig.session->stats().hw_holds, 1u);
  EXPECT_EQ(rig.session->stats().hold_calls, 1u);
  EXPECT_EQ(rig.session->stats().predicted_frames, 1u);
  EXPECT_EQ(rig.session->stats().gate_closes, 0u);
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  // The hardware is quiet again: predictions resume, the gate never closed.
  rig.Frame(LocalPad(12), Open());
  EXPECT_TRUE(rig.sink.Has("GATE hold end frame=12 held_calls=1"));
  EXPECT_EQ(rig.session->stats().predicted_frames, 2u);
  EXPECT_FALSE(rig.sink.Has("GATE close"));
}

TEST(Rev3Session, ARollbackNeverRunsWhileTheHardwareIsHeldBusyUnlessItIsForced)
{
  // The point of HOLD: with the hardware busy for a while, no frame is predicted, so there is nothing to roll back while a disc read is in
  // flight. A misprediction made BEFORE the hold is still postponed as long as the depth allows.
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(10, P(0xA1));  // frame 10 was mispredicted
  Gate hold = Busy();
  hold.hardware_hold = true;
  rig.net.Deliver(11, P(0xA1));
  rig.Frame(LocalPad(11), hold);
  rig.net.Deliver(12, P(0xA1));
  rig.Frame(LocalPad(12), hold);
  EXPECT_TRUE(rig.store.restored.empty());
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  EXPECT_EQ(rig.session->stats().postponed, 2u);  // counted once per call
  EXPECT_EQ(rig.session->stats().predicted_frames, 1u);  // only frame 10: the held calls played real pads
  // The hardware quiets down: the postponed rollback runs.
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
}

TEST(Rev3Session, ALocalRelayoutDrainsRollsBackAndCarriesOnArmed)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  Gate relayout = Open();
  relayout.layout_pending = true;
  EXPECT_FALSE(rig.TryFrame(LocalPad(12), relayout));  // wait for every remote pad below 12
  EXPECT_TRUE(rig.session->mode() == Mode::DRAIN);
  EXPECT_TRUE(rig.sink.Has("GATE relayout frame=12"));
  rig.net.Deliver(10, RemotePadScript(9));
  rig.net.Deliver(11, RemotePadScript(9));
  EXPECT_FALSE(rig.TryFrame(LocalPad(12), relayout));  // and for the frame itself
  rig.net.Deliver(12, P(0x12));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(12), relayout, &r));  // the flag is still set in this call: it must not start another drain
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0x12));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.store.relayouts, 1u);
  EXPECT_EQ(rig.session->stats().relayouts, 1u);
  EXPECT_EQ(rig.session->stats().gate_closes, 0u);
  EXPECT_TRUE(rig.sink.Has("GATE relayout done frame=12"));
  // ARMED goes on: the next frame is predicted again.
  rig.Frame(LocalPad(13), Open());
  EXPECT_EQ(rig.session->stats().predicted_frames, 3u);  // 10, 11 and 13
}

TEST(Rev3Session, ARelayoutThatCannotBeInstalledLeavesTheSession)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(10, RemotePadScript(9));
  rig.net.Deliver(11, P(0x11));
  rig.store.relayout_ok = false;
  Gate relayout = Open();
  relayout.layout_pending = true;
  rig.TryFrame(LocalPad(11), relayout);
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("ERROR frame 11: the new snapshot set is not usable"));
}

// ---- hiding the resimulated frames (M4) --------------------------------------------------------------------------------------------

class FakeHider final : public ResimHider
{
public:
  void SetHidden(bool hidden) override { calls.push_back(hidden); }
  std::vector<bool> calls;
};

TEST(Rev3Session, ResimHidingFollowsTheResimulationFlagAndLogsOnce)
{
  FakeHider hider;
  TestSink log;
  ResimHiding hiding(&hider, log);
  hiding.Update(false);
  EXPECT_TRUE(hider.calls.empty());  // nothing to undo
  hiding.Update(true);
  hiding.Update(true);  // already hidden: no second call
  ASSERT_EQ(hider.calls.size(), 1u);
  EXPECT_TRUE(hider.calls[0]);
  EXPECT_TRUE(hiding.hidden());
  hiding.Update(false);
  ASSERT_EQ(hider.calls.size(), 2u);
  EXPECT_FALSE(hider.calls[1]);
  EXPECT_FALSE(hiding.hidden());
  hiding.Update(true);
  hiding.Update(false);
  EXPECT_EQ(hider.calls.size(), 4u);
  EXPECT_EQ(hiding.hides(), 2u);
  EXPECT_EQ(log.Count("NOTE resim hiding v1: throttle off + mute"), 1u);  // once, however many resimulations
}

TEST(Rev3Session, ResimHidingWithoutAHiderDoesNothing)
{
  TestSink log;
  ResimHiding hiding(nullptr, log);
  hiding.Update(true);
  hiding.Update(false);
  EXPECT_FALSE(hiding.hidden());
  EXPECT_TRUE(log.lines.empty());
}

TEST(Rev3Session, ResimHidingTracksARealSessionFromTheRestoredReplyToTheLastResimulatedCall)
{
  Rig rig(Role::Host, 0, 5);
  FakeHider hider;
  ResimHiding hiding(&hider, rig.sink);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  hiding.Update(rig.session->Resimulating());
  EXPECT_TRUE(hider.calls.empty());
  rig.net.Deliver(10, P(0xA1));
  rig.net.Deliver(11, P(0xA2));
  rig.net.Deliver(12, P(0xA3));
  rig.Frame(LocalPad(13), Open());  // RESTORED
  hiding.Update(rig.session->Resimulating());
  EXPECT_TRUE(hiding.hidden());
  rig.Frame(LocalPad(11), Open());
  hiding.Update(rig.session->Resimulating());
  EXPECT_TRUE(hiding.hidden());
  rig.Frame(LocalPad(12), Open());
  hiding.Update(rig.session->Resimulating());
  EXPECT_TRUE(hiding.hidden());
  rig.Frame(LocalPad(13), Open());  // the last resimulated call
  hiding.Update(rig.session->Resimulating());
  EXPECT_FALSE(hiding.hidden());
  ASSERT_EQ(hider.calls.size(), 2u);
  EXPECT_TRUE(hider.calls[0]);
  EXPECT_FALSE(hider.calls[1]);
}

TEST(Rev3Session, ResimHidingIsRestoredWhenTheSessionLeavesInTheMiddleOfAResimulation)
{
  Rig rig(Role::Host, 0, 5);
  FakeHider hider;
  ResimHiding hiding(&hider, rig.sink);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, P(0xA1));
  rig.Frame(LocalPad(13), Open());  // RESTORED
  hiding.Update(rig.session->Resimulating());
  ASSERT_TRUE(hiding.hidden());
  rig.session->Abort("test: the peer is gone");
  EXPECT_FALSE(rig.session->Resimulating());
  hiding.Update(rig.session->Resimulating());
  EXPECT_FALSE(hiding.hidden());
  EXPECT_FALSE(hider.calls.back());
}

// ---- the log volume (M4) -----------------------------------------------------------------------------------------------------------

// Plays `n` consecutive one-frame mispredictions: frame f is predicted, the real pad differs, the next call rolls back (RESTORED) and the
// resimulation ends at the second run of f + 1, whose own pad is delivered equal to the prediction.
void PlayRollbacks(Rig& rig, s32 first, u32 n)
{
  s32 f = first;
  for (u32 i = 0; i < n; ++i, f += 2)
  {
    const u8 v = (i % 2 == 0) ? 0xA1 : 0xA2;
    rig.Frame(LocalPad(f), Open());
    rig.net.Deliver(f, P(v));
    rig.Frame(LocalPad(f + 1), Open());  // RESTORED: frame f
    rig.Frame(LocalPad(f + 1), Open());  // the second run of f + 1 (predicted with P(v))
    rig.net.Deliver(f + 1, P(v));
  }
}

TEST(Rev3Session, TheFirst200RollbackLinesAreWrittenInFullThenOneSummaryPerWindow)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  PlayRollbacks(rig, 10, 330);  // frames 10..669: 330 rollbacks, one window boundary at frame 610
  EXPECT_EQ(rig.session->stats().rollbacks, 330u);
  EXPECT_EQ(rig.sink.Count("ROLLBACK depth="), 200u);
  EXPECT_EQ(rig.sink.Count("RESIM done"), 200u);
  EXPECT_EQ(rig.session->stats().log_suppressed, 2u * 130u);  // the ROLLBACK line and the RESIM line of each of the 130 others
  EXPECT_EQ(rig.sink.Count("ROLLBACK depth_hist="), 1u);      // the window that was closed by now
  rig.session->Finish();
  EXPECT_EQ(rig.sink.Count("ROLLBACK depth_hist="), 2u);
  // The counts of the two summaries add up to the rollbacks that were left out, and the depth histogram is exact.
  u32 left_out = 0;
  for (const std::string& line : rig.sink.lines)
  {
    const size_t at = line.find("ROLLBACK depth_hist=");
    if (at == std::string::npos)
      continue;
    EXPECT_NE(line.find("1:"), std::string::npos) << line;  // all rollbacks have depth 1
    unsigned rollbacks = 0;
    ASSERT_EQ(sscanf(line.c_str() + line.find("rollbacks="), "rollbacks=%u", &rollbacks), 1) << line;
    left_out += rollbacks;
  }
  EXPECT_GE(left_out, 130u);  // the first window also holds rollbacks that were written in full
  EXPECT_TRUE(rig.sink.Has("SUMMARY final"));
  EXPECT_TRUE(rig.sink.Has("log_suppressed=260"));
}

TEST(Rev3Session, ThereIsNoSummaryLineWhenNothingWasLeftOut)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  PlayRollbacks(rig, 10, 50);
  rig.session->Finish();
  EXPECT_EQ(rig.sink.Count("ROLLBACK depth_hist="), 0u);
  EXPECT_EQ(rig.sink.Count("ROLLBACK depth="), 50u);
  EXPECT_TRUE(rig.sink.Has("log_suppressed=0"));
}

TEST(Rev3Session, ChkDesyncErrorAndGateLinesAreNeverRateLimited)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  PlayRollbacks(rig, 10, 250);  // past the budget
  EXPECT_GT(rig.session->stats().log_suppressed, 0u);
  rig.Frame(LocalPad(510), Lost());
  EXPECT_TRUE(rig.sink.Has("GATE close scheduled"));  // still written in full
}

TEST(Rev3Session, StallLinesHaveTheirOwnBudgetOf200AndAnEndLineFollowsItsStartLine)
{
  Rig rig(Role::Host, 0, 2);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  // Call 12 stalls (3 predicted frames > MAXR 2); every pad that arrives ends the stall and the next call stalls again: 260 stalls.
  for (s32 i = 0; i < 260; ++i)
  {
    EXPECT_FALSE(rig.TryFrame(LocalPad(12 + i), Open()));
    rig.net.Deliver(10 + i, RemotePadScript(9));
    rig.Frame(LocalPad(12 + i), Open());
  }
  EXPECT_EQ(rig.sink.Count("STALL start"), 200u);
  EXPECT_EQ(rig.sink.Count("STALL end"), 200u);
  EXPECT_EQ(rig.session->stats().stalls, 260u);
  EXPECT_EQ(rig.session->stats().log_suppressed, 120u);  // the start and the end line of each of the 60 left out
}

// ---- REV3_NET_RANGE_CRC (testing) --------------------------------------------------------------------------------------------------

TEST(Rev3Session, RangeCrcsAreExchangedAtEachCheckFrameAndDifferencesAreLoggedWithoutADesync)
{
  Rig rig(Role::Host, 0, 5, true);
  rig.store.ranges_ok = true;
  rig.store.ranges = {{0x80000000u, 0x80001000u, 0x11111111u}, {0x80002000u, 0x80003000u, 0x22222222u}, {0x90000000u, 0x90000100u, 0x33333333u}};
  rig.Handshake();
  rig.Lockstep(31);  // frame 30 is a check frame: our CRCs go out
  const Message* sent = rig.net.LastOut(MsgType::RangeCrc);
  ASSERT_NE(sent, nullptr);
  RangeCrcMsg ours;
  ASSERT_TRUE(ParseRangeCrc(sent->bytes, &ours));
  EXPECT_EQ(ours.frame, 30);
  EXPECT_EQ(ours.ranges.size(), 3u);
  // The peer's: one range differs, one is missing on its side.
  RangeCrcMsg theirs;
  theirs.frame = 30;
  theirs.ranges = {{0x80000000u, 0x80001000u, 0x11111111u}, {0x80002000u, 0x80003000u, 0xDEADBEEFu}};
  rig.net.inbox.push_back({MsgType::RangeCrc, SerializeRangeCrc(theirs)});
  rig.net.Deliver(31, RemotePadScript(31));
  rig.Frame(LocalPad(31), Closed());
  EXPECT_TRUE(rig.sink.Has("NOTE range_crc frame=30 ranges=3 differing=2"));
  EXPECT_TRUE(rig.sink.Has("range 0x80002000-0x80003000 own=0x22222222 peer=0xDEADBEEF"));
  EXPECT_TRUE(rig.sink.Has("range 0x90000000-0x90000100 only on the own side"));
  EXPECT_FALSE(rig.sink.Has("DESYNC"));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_EQ(rig.session->stats().range_checks, 1u);
  EXPECT_EQ(rig.session->stats().range_diff_frames, 1u);
}

TEST(Rev3Session, RangeCrcsThatMatchLogNothingAndWithoutTheOptionNothingIsSent)
{
  {
    Rig rig(Role::Host, 0, 5, true);
    rig.store.ranges_ok = true;
    rig.store.ranges = {{0x80000000u, 0x80001000u, 0x11111111u}};
    rig.Handshake();
    rig.Lockstep(31);
    RangeCrcMsg theirs;
    theirs.frame = 30;
    theirs.ranges = rig.store.ranges;
    rig.net.inbox.push_back({MsgType::RangeCrc, SerializeRangeCrc(theirs)});
    rig.net.Deliver(31, RemotePadScript(31));
    rig.Frame(LocalPad(31), Closed());
    EXPECT_FALSE(rig.sink.Has("range_crc"));
    EXPECT_EQ(rig.session->stats().range_checks, 1u);
    EXPECT_EQ(rig.session->stats().range_diff_frames, 0u);
  }
  {
    Rig rig(Role::Host, 0, 5, false);
    rig.store.ranges_ok = true;
    rig.store.ranges = {{0x80000000u, 0x80001000u, 0x11111111u}};
    rig.Handshake();
    rig.Lockstep(31);
    EXPECT_EQ(rig.net.CountOut(MsgType::RangeCrc), 0u);
    RangeCrcMsg theirs;
    theirs.frame = 30;
    theirs.ranges = {{0x80000000u, 0x80001000u, 0x1u}};
    rig.net.inbox.push_back({MsgType::RangeCrc, SerializeRangeCrc(theirs)});  // the peer has the option, we do not: ignored, no error
    rig.net.Deliver(31, RemotePadScript(31));
    rig.Frame(LocalPad(31), Closed());
    EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
    EXPECT_FALSE(rig.sink.Has("ERROR"));
  }
}

TEST(Rev3Session, SummaryCarriesTheM4Counters)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.session->Finish();
  for (const char* key : {"hw_holds=", "hold_calls=", "relayouts=", "forced_hw=", "gate_late=", "log_suppressed=", "depth_hist=["})
    EXPECT_TRUE(rig.sink.Has(key)) << key;
}

// ---- checksums -------------------------------------------------------------------------------------------------------------------

TEST(Rev3Session, LockstepChecksumsGoOutEvery30FramesAndMatchingOnesLogChk)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(31);  // frames 0..30
  // The checksums of frames 0 and 30 were recorded; the one of frame 30 rides in the next pad packet.
  rig.net.Deliver(31, RemotePadScript(31));
  rig.Frame(LocalPad(31), Closed());
  EXPECT_EQ(rig.net.sent.back().chk_frame, 30);
  EXPECT_EQ(rig.net.sent.back().chk, rig.check_before[30]);
  // The peer's equal value arrives: CHK line, no desync.
  rig.net.chk_in.push_back({30, rig.check_before[30]});
  rig.net.Deliver(32, RemotePadScript(32));
  rig.Frame(LocalPad(32), Closed());
  EXPECT_TRUE(rig.sink.Has(std::string("CHK frame=30 value=0x")));
  EXPECT_EQ(rig.session->stats().checks_ok, 1u);
  EXPECT_EQ(rig.session->stats().checks_bad, 0u);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
}

TEST(Rev3Session, ChecksumMismatchLogsDesyncSendsTheDumpAndLeaves)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(31);
  rig.net.chk_in.push_back({30, rig.check_before[30] ^ 0x10u});
  const Command cmd = rig.Cmd(31, LocalPad(31));
  rig.net.Deliver(31, RemotePadScript(31));
  Reply r;
  ASSERT_TRUE(rig.session->Step(cmd, Closed(), &r));
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(r.pads[0] == cmd.pads[0] && r.pads[1] == cmd.pads[1]);  // continues offline with the local pads
  EXPECT_TRUE(rig.sink.Has("DESYNC frame=30"));
  EXPECT_FALSE(rig.sink.osd.empty());
  EXPECT_EQ(rig.net.CountOut(MsgType::DesyncDump), 1u);
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
  EXPECT_EQ(rig.session->stats().checks_bad, 1u);
  DesyncDump dump;
  ASSERT_TRUE(ParseDesyncDump(rig.net.LastOut(MsgType::DesyncDump)->bytes, &dump));
  EXPECT_EQ(dump.frame, 30);
  EXPECT_FALSE(dump.trace.empty());
  EXPECT_EQ(dump.trace.back().first, 30);

  // The peer's dump arrives later (its state differs in a field): the log names the first differing field.
  DesyncDump theirs = dump;
  theirs.trace.back().second.value[0][2] ^= 0x4;
  rig.net.inbox.push_back({MsgType::DesyncDump, SerializeDesyncDump(theirs)});
  EXPECT_TRUE(rig.session->Step(rig.Cmd(32, LocalPad(32)), Closed(), &r));
  EXPECT_TRUE(rig.sink.Has("DESYNC first_diff frame=30"));
  EXPECT_TRUE(rig.sink.Has("p1 move_id"));  // value[0][2] is field 2 of the table (FIELDS[2] = move_id)
}

TEST(Rev3Session, OnlyTheLastEightChecksumsAreKept)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(301);  // check frames 0, 30, ..., 300 = 11 values
  rig.net.Deliver(301, RemotePadScript(301));
  rig.net.chk_in.push_back({0, 0xBAD0BAD0u});  // older than the 8 kept: dropped without a verdict
  rig.Frame(LocalPad(301), Closed());
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_EQ(rig.session->stats().checks_bad, 0u);
  rig.net.Deliver(302, RemotePadScript(302));
  rig.net.chk_in.push_back({270, rig.check_before[270] ^ 1u});  // within the last 8: compared
  rig.Frame(LocalPad(302), Closed());
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("DESYNC frame=270"));
}

TEST(Rev3Session, ArmedChecksumsAreExchangedOnlyOnceTheFrameIsFinal)
{
  Rig rig(Role::Host, 0, 7);
  rig.ReadyToArm(25);
  const Pad8 predicted = RemotePadScript(24);
  const auto last_chk_frame = [&] {
    s32 f = -1;
    for (const auto& s : rig.net.sent)
      f = std::max(f, s.chk_frame);
    return f;
  };
  EXPECT_EQ(last_chk_frame(), 0);  // lockstep frame 0 only
  for (s32 f = 25; f <= 31; ++f)  // 7 predicted frames; frame 30 passes without the remote pads below it being known
    rig.Frame(LocalPad(f), Open());
  EXPECT_EQ(last_chk_frame(), 0);
  // All remote pads below 30 arrive and equal the prediction. The next call verifies them: frame 30 is final.
  for (s32 f = 25; f <= 31; ++f)
    rig.net.Deliver(f, predicted);
  rig.Frame(LocalPad(32), Open());
  EXPECT_EQ(rig.session->verified_frame(), 31);
  rig.Frame(LocalPad(33), Open());  // the checksum rides in this call's packet
  EXPECT_EQ(last_chk_frame(), 30);
  EXPECT_EQ(rig.net.sent.back().chk_frame, 30);
  EXPECT_EQ(rig.net.sent.back().chk, rig.check_before[30]);
}

TEST(Rev3Session, APendingMispredictionKeepsAFrameFromBeingFinal)
{
  Rig rig(Role::Host, 0, 7);
  rig.ReadyToArm(25);
  for (s32 f = 25; f <= 31; ++f)
    rig.Frame(LocalPad(f), Open());
  const Pad8 predicted = RemotePadScript(24);
  for (s32 f = 25; f <= 31; ++f)
    rig.net.Deliver(f, f == 27 ? P(0x99) : predicted);  // frame 27 was wrong: the state at 30 is not what the real game has
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(32), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 27u);
  for (const auto& s : rig.net.sent)
    EXPECT_LE(s.chk_frame, 0) << "checksum of a frame that is not final went out";
  // Resimulate to 32; the next normal call verifies and releases frame 30 with the CORRECTED state.
  while (rig.session->Resimulating())
    rig.Frame(LocalPad(rig.counter), Open());
  const u32 corrected_30 = rig.check_before[30];  // check_before holds the latest (resimulated) value
  rig.Frame(LocalPad(rig.counter), Open());
  rig.Frame(LocalPad(rig.counter), Open());
  s32 f = -1;
  u32 v = 0;
  for (const auto& s : rig.net.sent)
    if (s.chk_frame > f)
    {
      f = s.chk_frame;
      v = s.chk;
    }
  EXPECT_EQ(f, 30);
  EXPECT_EQ(v, corrected_30);
}

TEST(Rev3Session, ArmedDesyncIsFoundWhenTheFrameBecomesFinal)
{
  Rig rig(Role::Host, 0, 7);
  rig.ReadyToArm(25);
  const Pad8 predicted = RemotePadScript(24);
  for (s32 f = 25; f <= 31; ++f)
  {
    rig.Frame(LocalPad(f), Open());
    rig.net.Deliver(f, predicted);
  }
  rig.net.chk_in.push_back({30, 0xDEADBEEFu});  // the peer's state at 30 is different
  rig.Frame(LocalPad(32), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("DESYNC frame=30"));
}

TEST(Rev3Session, FinalObserverSeesEveryVerifiedFrameInOrderOnce)
{
  Rig rig(Role::Host, 0, 5);
  std::vector<std::pair<s32, u32>> seen;
  rig.session->SetFinalObserver([&](s32 f, u32 c) { seen.emplace_back(f, c); });
  rig.ReadyToArm(10);
  const Pad8 predicted = RemotePadScript(9);
  for (s32 f = 10; f <= 14; ++f)
    rig.Frame(LocalPad(f), Open());
  for (s32 f = 10; f <= 14; ++f)
    rig.net.Deliver(f, f == 12 ? P(0x66) : predicted);
  while (true)
  {
    rig.Frame(LocalPad(rig.counter), Open());
    if (!rig.session->Resimulating() && rig.counter > 18)
      break;
  }
  for (size_t i = 1; i < seen.size(); ++i)
    EXPECT_EQ(seen[i].first, seen[i - 1].first + 1) << "frames are reported in order, once";
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front().first, 10);
}

// ---- two machines through a simulated network ---------------------------------------------------------------------------------------

struct PadPacket
{
  std::vector<std::pair<s32, Pad8>> pads;  // newest first
  s32 chk_frame = -1;
  u32 chk = 0;
};

struct NetParams
{
  u64 delay_us = 0;       // one way
  u64 jitter_us = 0;      // + uniform 0..jitter
  u32 loss_pct = 0;       // of PAD packets and acks
  u64 resend_us = 25000;  // unacked pads are re-sent after this long without a packet
  u64 connect_at_us = 0;  // the transports report Connected from here on
};

class SimNet;

class SimEndpoint final : public Transport
{
public:
  SimEndpoint(SimNet* net_, int me_) : net(net_), me(me_) {}

  void SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum) override;
  s32 RemoteConfirmed() const override { return m_contig; }
  bool RemotePad(s32 frame, Pad8* out) const override
  {
    const auto it = m_got.find(frame);
    if (it == m_got.end() || frame > m_contig)
      return false;
    *out = it->second;
    return true;
  }
  std::vector<PeerChecksum> TakePeerChecksums() override
  {
    std::vector<PeerChecksum> out;
    out.swap(m_chk_in);
    return out;
  }
  void SendReliable(MsgType type, const std::vector<u8>& bytes) override;
  bool PollReliable(Message* out) override
  {
    if (m_inbox.empty())
      return false;
    *out = m_inbox.front();
    m_inbox.pop_front();
    return true;
  }
  u64 Now() const override;
  ConnState State() const override;
  void Pump() override { Tick(); }

  void Tick();
  void OnPadPacket(const PadPacket& packet);
  void OnAck(s32 frame)
  {
    while (!m_unacked.empty() && m_unacked.front().first <= frame)
      m_unacked.pop_front();
  }
  void OnReliable(const Message& m) { m_inbox.push_back(m); }

private:
  void Transmit();

  SimNet* net;
  int me;
  std::deque<std::pair<s32, Pad8>> m_unacked;  // ascending
  s32 m_chk_frame = -1;
  u32 m_chk = 0;
  u64 m_last_send_us = 0;
  std::map<s32, Pad8> m_got;
  s32 m_contig = -1;
  std::map<s32, u32> m_chk_seen;
  std::vector<PeerChecksum> m_chk_in;
  std::deque<Message> m_inbox;
};

class SimNet
{
public:
  SimNet(u64 seed, const NetParams& params_) : params(params_), rng(seed ^ 0xABCDEF), ep{SimEndpoint(this, 0), SimEndpoint(this, 1)} {}

  u64 Delay() { return params.delay_us + (params.jitter_us != 0 ? rng.Below(static_cast<u32>(params.jitter_us) + 1) : 0) + 1; }
  bool Lost() { return params.loss_pct != 0 && rng.Below(100) < params.loss_pct; }

  void PostPad(int from, const PadPacket& p)
  {
    if (Lost())
      return;
    Push(now + Delay(), Event{1 - from, Event::Pad, p, 0, {}});
  }
  void PostAck(int from, s32 frame)
  {
    if (Lost())
      return;
    Push(now + Delay(), Event{1 - from, Event::Ack, {}, frame, {}});
  }
  void PostReliable(int from, const Message& m)
  {
    u64 at = now + Delay();
    at = std::max(at, last_reliable[from] + 1);  // in order, no loss
    last_reliable[from] = at;
    Push(at, Event{1 - from, Event::Reliable, {}, 0, m});
  }
  u64 NextEvent() const { return events.empty() ? ~0ull : events.begin()->first.first; }
  void DeliverUpTo(u64 t)
  {
    while (!events.empty() && events.begin()->first.first <= t)
    {
      const Event e = events.begin()->second;
      events.erase(events.begin());
      switch (e.kind)
      {
      case Event::Pad:
        ep[e.to].OnPadPacket(e.pad);
        break;
      case Event::Ack:
        ep[e.to].OnAck(e.ack);
        break;
      case Event::Reliable:
        ep[e.to].OnReliable(e.msg);
        break;
      }
    }
  }

  NetParams params;
  Rng rng;
  u64 now = 0;
  SimEndpoint ep[2];

private:
  struct Event
  {
    int to;
    enum Kind
    {
      Pad,
      Ack,
      Reliable
    } kind;
    PadPacket pad;
    s32 ack;
    Message msg;
  };
  void Push(u64 at, const Event& e) { events.emplace(std::make_pair(at, seq++), e); }
  std::map<std::pair<u64, u64>, Event> events;
  u64 seq = 0;
  u64 last_reliable[2] = {0, 0};
};

u64 SimEndpoint::Now() const
{
  return net->now;
}
ConnState SimEndpoint::State() const
{
  return net->now >= net->params.connect_at_us ? ConnState::Connected : ConnState::Connecting;
}
void SimEndpoint::SendReliable(MsgType type, const std::vector<u8>& bytes)
{
  net->PostReliable(me, Message{type, bytes});
}
void SimEndpoint::SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum)
{
  m_unacked.emplace_back(frame, pad);
  while (m_unacked.size() > 128)
    m_unacked.pop_front();
  m_chk_frame = checksum_frame;
  m_chk = checksum;
  Transmit();
}
void SimEndpoint::Transmit()
{
  PadPacket p;
  for (size_t i = m_unacked.size(); i > 0; --i)
    p.pads.push_back(m_unacked[i - 1]);
  p.chk_frame = m_chk_frame;
  p.chk = m_chk;
  m_last_send_us = net->now;
  net->PostPad(me, p);
}
void SimEndpoint::Tick()
{
  if (!m_unacked.empty() && net->now >= m_last_send_us + net->params.resend_us)
    Transmit();
}
void SimEndpoint::OnPadPacket(const PadPacket& packet)
{
  if (packet.pads.empty())
    return;
  for (const auto& fp : packet.pads)
    m_got.emplace(fp.first, fp.second);
  while (m_got.count(m_contig + 1) != 0)
    ++m_contig;
  if (packet.chk_frame >= 0 && m_chk_seen.emplace(packet.chk_frame, packet.chk).second)
    m_chk_in.push_back({packet.chk_frame, packet.chk});
  net->PostAck(me, packet.pads.front().first);  // ack the newest frame of the packet (Slippi)
}

struct SimParams
{
  u64 seed = 1;
  int delay = 2;
  int maxr = 5;
  NetParams net;
  s32 frames = 300;
  s32 open_at = 40;                    // the gate (a function of the frame: it is a function of game state in lockstep)
  s32 close_at = 270;                  // closes for good: the last frames run in lockstep so the end states are exact
  s32 mid_close = -1, mid_reopen = -1; // optional close / reopen in the middle
  u32 change_pct = 15;                 // chance per frame that a player's pad changes (otherwise a prediction is right)
  bool busy_hardware = false;          // each machine independently reports "hardware busy" on ~10% of its calls, and in bursts (HOLD)
  u32 lost_mod = 0;                    // != 0: a fighter is "at health 0" when (state word 0 >> 5) % lost_mod == 0, a function of the LIVE state:
                                       // a mispredicted timeline can show it where the real one does not (0 = only the frame windows below)
  u32 relayout_pct = 0;                // chance per armed call that THIS machine wants a new snapshot set (local: the peer never knows)
  u64 period_us[2] = {16667, 16700};   // each machine's frame period (clock drift)
  u64 start_us[2] = {0, 0};
  s32 bug_frame = -1;                  // machine 1's game has a rules difference at this frame (a desync)
};

struct SimMachineResult
{
  std::array<u32, 4> state{};
  SessionStats stats;
  Mode mode = Mode::OFFLINE;
  std::map<s32, u32> finals;  // frame -> checksum the session called final
  u32 phantom_lost = 0;       // normal ARMED calls whose live state said "lost" where the true timeline does not (a live-following gate would close here)
  std::string log;
  size_t chk_lines = 0;
  s32 counter = 0;
  bool left_session = false;
};

struct SimResult
{
  SimMachineResult m[2];
  std::array<u32, 4> reference_state{};
  std::vector<std::array<u32, 4>> reference_states;  // the state at the start of each frame
  std::vector<u32> reference_checksum;
  bool finished = false;
  u64 end_us = 0;
  u64 log_hash = 0;
};

// The fight is over (frame windows: the end of the run, the optional middle window) or a fighter is at health 0 in the live state.
bool LostAt(const SimParams& p, const FakeGame& game, s32 frame)
{
  if (frame >= p.close_at)
    return true;
  if (p.mid_close >= 0 && frame >= p.mid_close && frame < p.mid_reopen)
    return true;
  return p.lost_mod != 0 && frame >= p.open_at && ((game.s[0] >> 5) % p.lost_mod) == 0;
}

// The window in which the gate may open (a function of the frame: in lockstep it is a function of game state that both machines share).
bool GateAt(const SimParams& p, s32 frame)
{
  if (frame < p.open_at || frame >= p.close_at)
    return false;
  if (p.mid_close >= 0 && frame >= p.mid_close && frame < p.mid_reopen)
    return false;
  return true;
}

// The no-network run: frame f gets script[f - D] of both players.
void Reference(const SimParams& p, const std::vector<Pad8> script[2], SimResult* out)
{
  FakeGame g;
  for (s32 f = 0; f < p.frames; ++f)
  {
    out->reference_checksum.push_back(g.Checksum(f));
    out->reference_states.push_back(g.s);
    const Pad8 host = f >= p.delay ? script[0][static_cast<size_t>(f - p.delay)] : Pad8{};
    const Pad8 guest = f >= p.delay ? script[1][static_cast<size_t>(f - p.delay)] : Pad8{};
    g.Apply(f, host, guest);
  }
  out->reference_state = g.s;
}

u64 HashString(u64 h, const std::string& s)
{
  for (char c : s)
    h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
  return h;
}

struct SimMachine
{
  SimMachine(int idx_, const SimParams& p, SimNet* net, const std::vector<Pad8>& script_)
      : idx(idx_), store(&game, static_cast<size_t>(p.maxr) + 2), script(script_), rng(p.seed * 7 + static_cast<u64>(idx_))
  {
    if (idx == 1)
      game.bug_frame = p.bug_frame;
    Config cfg;
    cfg.role = idx == 0 ? Role::Host : Role::Guest;
    cfg.delay = idx == 0 ? p.delay : (p.delay + 1) % 5;  // the guest asks for something else; the host's value must win
    cfg.max_rollback = p.maxr;
    cfg.hello.game_id = "RNEEDA";
    cfg.hello.items = {{"cpu_core", "JIT64"}};
    session = std::make_unique<RollbackSession>(cfg, net->ep[idx], store, sink, nullptr);
    session->SetFinalObserver([this](s32 f, u32 c) { finals[f] = c; });
    ep = &net->ep[idx];
    next_at = p.start_us[idx];
    period = p.period_us[idx];
    total = p.frames;
  }

  int idx;
  FakeGame game;
  FakeStore store;
  TestSink sink;
  std::vector<Pad8> script;
  Rng rng;
  SimEndpoint* ep = nullptr;
  std::unique_ptr<RollbackSession> session;
  std::map<s32, u32> finals;
  s32 counter = 0;
  s32 total = 0;
  u64 next_at = 0;
  u64 period = 16667;
  bool done = false;
  bool blocking = false;
  u64 block_start = 0;
  u32 phantom_lost = 0;
  u32 busy_left = 0;     // calls left in the current hardware-busy burst
  u32 busy_streak = 0;   // consecutive busy calls so far
  bool layout_pending = false;
  u32 relayouts_seen = 0;
};

// Runs the two machines to completion. False: the scheduler gave up (a deadlock: a bug).
bool RunSim(const SimParams& p, SimResult* out)
{
  SimNet net(p.seed, p.net);
  std::vector<Pad8> script[2];
  Rng srng(p.seed + 99);
  for (int i = 0; i < 2; ++i)
  {
    Pad8 cur = RandomPad(srng);
    for (s32 f = 0; f < p.frames + 8; ++f)
    {
      if (srng.Below(100) < p.change_pct)
        cur = RandomPad(srng);
      script[i].push_back(cur);
    }
  }
  Reference(p, script, out);

  SimMachine machines[2] = {SimMachine(0, p, &net, script[0]), SimMachine(1, p, &net, script[1])};
  Rng junk(p.seed + 5);
  u64 guard = 0;
  while (!(machines[0].done && machines[1].done))
  {
    if (++guard > 20000000)
      return false;
    u64 t = net.NextEvent();
    for (const SimMachine& m : machines)
      if (!m.done)
        t = std::min(t, m.next_at);
    if (t == ~0ull)
      return false;
    net.now = std::max(net.now, t);
    net.DeliverUpTo(net.now);
    net.ep[0].Tick();
    net.ep[1].Tick();
    for (SimMachine& m : machines)
    {
      if (m.done || m.next_at > net.now)
        continue;
      Command c;
      c.frame = static_cast<u32>(m.counter);
      c.sp = SP;
      c.pad_return = 0;
      // The live controller; during a resimulation the hook still reads a pad, which the session must ignore.
      c.pads[0] = ToPadBytes(m.session->Resimulating() ? RandomPad(junk) : m.script[static_cast<size_t>(m.counter)]);
      c.pads[1] = ToPadBytes(RandomPad(junk));
      Gate gate;
      gate.health_zero = LostAt(p, m.game, m.counter);
      gate.open = GateAt(p, m.counter) && !gate.health_zero;
      gate.hardware_quiet = !(p.busy_hardware && (m.busy_left > 0 || m.rng.Below(10) == 0));
      gate.hardware_hold = m.busy_left > 0 && m.busy_streak >= 3;  // GateTracker: busy for 3 calls in a row
      gate.layout_pending = m.layout_pending && m.session->mode() == Mode::ARMED;
      gate.why = "sim";
      const bool armed_call = m.session->mode() == Mode::ARMED && !m.session->Resimulating();
      Reply r;
      if (!m.session->Step(c, gate, &r))
      {
        if (!m.blocking)
        {
          m.blocking = true;
          m.block_start = net.now;
        }
        if ((net.now - m.block_start) / 1000 >= m.session->BlockTimeoutMs())
        {
          m.session->BlockTimedOut();
          m.blocking = false;
        }
        m.next_at = net.now + 1000;  // the real wait polls every millisecond
        continue;
      }
      m.blocking = false;
      if (armed_call && gate.health_zero && m.counter >= 0 && static_cast<size_t>(m.counter) < out->reference_states.size())
      {
        FakeGame truth;
        truth.s = out->reference_states[static_cast<size_t>(m.counter)];
        if (!LostAt(p, truth, m.counter))
          ++m.phantom_lost;
      }
      if (p.busy_hardware)
      {
        if (m.busy_left > 0)
        {
          --m.busy_left;
          ++m.busy_streak;
        }
        else
        {
          m.busy_streak = 0;
          if (m.rng.Below(40) == 0)
            m.busy_left = 3 + static_cast<u32>(m.rng.Below(6));
        }
      }
      if (m.session->stats().relayouts != m.relayouts_seen)
      {
        m.relayouts_seen = m.session->stats().relayouts;
        m.layout_pending = false;
      }
      else if (p.relayout_pct != 0 && m.session->mode() == Mode::ARMED && m.rng.Below(100) < p.relayout_pct)
      {
        m.layout_pending = true;
      }
      if ((r.flags & REPLY_FLAG_RESTORED) != 0)
        m.counter = static_cast<s32>(r.frame);
      m.game.Apply(m.counter, SanitizePad(r.pads[0]), SanitizePad(r.pads[1]));
      ++m.counter;
      if (m.counter >= m.total && !m.session->Resimulating())
        m.done = true;
      m.next_at = net.now + m.period;
    }
  }
  out->finished = true;
  out->end_us = net.now;
  for (int i = 0; i < 2; ++i)
  {
    SimMachine& m = machines[i];
    m.session->Finish();
    SimMachineResult& r = out->m[i];
    r.state = m.game.s;
    r.stats = m.session->stats();
    r.mode = m.session->mode();
    r.finals = m.finals;
    r.phantom_lost = m.phantom_lost;
    r.log = m.sink.Dump();
    r.chk_lines = m.sink.Count("CHK frame=");
    r.counter = m.counter;
    r.left_session = m.sink.Has("LEAVE");
    out->log_hash = HashString(out->log_hash, r.log);
  }
  return true;
}

SimParams ParamsFromSeed(u64 seed)
{
  Rng r(seed);
  SimParams p;
  p.seed = seed;
  p.delay = static_cast<int>(r.Below(5));
  p.maxr = 1 + static_cast<int>(r.Below(7));
  p.net.delay_us = r.Below(5) == 0 ? 0 : r.Below(120000);
  p.net.jitter_us = r.Below(60000);
  p.net.loss_pct = r.Below(4) == 0 ? r.Below(31) : r.Below(8);
  p.net.connect_at_us = r.Below(60000);
  p.net.resend_us = 15000 + r.Below(20000);
  p.frames = 200 + static_cast<s32>(r.Below(200));
  p.open_at = 20 + static_cast<s32>(r.Below(40));
  p.close_at = p.frames - 25 - static_cast<s32>(r.Below(15));
  if (r.Below(2) == 0)
  {
    p.mid_close = p.open_at + 25 + static_cast<s32>(r.Below(60));
    p.mid_reopen = p.mid_close + 5 + static_cast<s32>(r.Below(40));
    if (p.mid_reopen >= p.close_at - 15)
      p.mid_close = p.mid_reopen = -1;
  }
  p.change_pct = 5 + r.Below(30);
  p.busy_hardware = r.Below(3) == 0;
  p.lost_mod = r.Below(2) == 0 ? 0 : 20 + r.Below(200);
  p.relayout_pct = r.Below(4) == 0 ? 1 + r.Below(3) : 0;
  p.period_us[0] = 15500 + r.Below(2500);
  p.period_us[1] = 15500 + r.Below(2500);
  p.start_us[0] = r.Below(80000);
  p.start_us[1] = r.Below(80000);
  return p;
}

// The "GATE close frame=.. why=.." lines of a log, in order: both machines must have the same ones (THE GATE RULE).
std::vector<std::string> CloseLines(const std::string& log)
{
  std::vector<std::string> out;
  size_t pos = 0;
  while ((pos = log.find("GATE close frame=", pos)) != std::string::npos)
  {
    const size_t end = log.find('\n', pos);
    out.push_back(log.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    pos = end == std::string::npos ? log.size() : end;
  }
  return out;
}

// The checks every healthy run must pass; returns a description of the first failure ("" = fine).
std::string CheckHealthy(const SimParams& p, const SimResult& res)
{
  if (!res.finished)
    return "the scheduler gave up (deadlock)";
  if (CloseLines(res.m[0].log) != CloseLines(res.m[1].log))
    return "the two machines closed the gate at different frames or for different reasons";
  if (res.m[0].stats.gate_closes != res.m[1].stats.gate_closes || res.m[0].stats.gate_opens != res.m[1].stats.gate_opens)
    return "the two machines opened / closed a different number of times";
  for (int i = 0; i < 2; ++i)
    if (res.m[i].stats.gate_late != 0)
      return "machine " + std::to_string(i) + " applied a scheduled close late";
  for (int i = 0; i < 2; ++i)
  {
    const SimMachineResult& m = res.m[i];
    if (m.left_session || m.log.find("DESYNC") != std::string::npos || m.log.find("ERROR") != std::string::npos)
      return "machine " + std::to_string(i) + " logged LEAVE/DESYNC/ERROR:\n" + m.log;
    if (m.stats.max_depth > static_cast<u32>(p.maxr))
      return "machine " + std::to_string(i) + " rolled back " + std::to_string(m.stats.max_depth) + " deep, MAXR " + std::to_string(p.maxr);
    for (const auto& f : m.finals)
    {
      if (f.first < 0 || static_cast<size_t>(f.first) >= res.reference_checksum.size())
        return "final frame out of range";
      if (res.reference_checksum[static_cast<size_t>(f.first)] != f.second)
        return "machine " + std::to_string(i) + ": frame " + std::to_string(f.first) + " was called final with a state that differs from the reference";
    }
    if (m.state != res.reference_state)
      return "machine " + std::to_string(i) + " ended in a different state than the reference";
    if (m.mode != Mode::LOCKSTEP)
      return "machine " + std::to_string(i) + " did not end in LOCKSTEP";
    if (m.counter != p.frames)
      return "machine did not play all frames";
  }
  return {};
}

TEST(Rev3Session, TwoMachinesReachTheReferenceStateWithoutLatency)
{
  SimParams p;
  p.seed = 1;
  p.delay = 2;
  p.maxr = 5;
  p.net.delay_us = 0;
  SimResult res;
  RunSim(p, &res);
  EXPECT_EQ(CheckHealthy(p, res), "");
  EXPECT_FALSE(res.m[0].finals.empty());
  EXPECT_GE(res.m[0].stats.gate_opens, 1u);
  EXPECT_GE(res.m[0].stats.checks_ok, 3u);  // CHK lines were exchanged and matched
}

TEST(Rev3Session, TwoMachinesWithLatencyRollBackAndStillAgree)
{
  SimParams p;
  p.seed = 2;
  p.delay = 1;
  p.maxr = 6;
  p.net.delay_us = 60000;  // ~3.6 frames one way: beyond D, so predictions happen
  p.net.jitter_us = 20000;
  p.net.loss_pct = 5;
  p.change_pct = 25;
  SimResult res;
  RunSim(p, &res);
  EXPECT_EQ(CheckHealthy(p, res), "");
  EXPECT_GT(res.m[0].stats.rollbacks + res.m[1].stats.rollbacks, 0u);
  EXPECT_GT(res.m[0].stats.predicted_frames + res.m[1].stats.predicted_frames, 0u);
  EXPECT_GE(res.m[0].stats.checks_ok, 3u);
}

TEST(Rev3Session, ExactLatencyWithinTheInputDelayNeverRollsBack)
{
  // One way latency below D frames: every remote pad is here in time, so nothing is predicted wrongly and nothing rolls back.
  SimParams p;
  p.seed = 3;
  p.delay = 4;
  p.maxr = 5;
  p.net.delay_us = 2 * 16667;
  p.period_us[0] = p.period_us[1] = 16667;
  SimResult res;
  RunSim(p, &res);
  EXPECT_EQ(CheckHealthy(p, res), "");
  EXPECT_EQ(res.m[0].stats.rollbacks, 0u);
  EXPECT_EQ(res.m[1].stats.rollbacks, 0u);
}

TEST(Rev3Session, VeryLateInputsStallInsteadOfRollingBackDeeperThanMaxr)
{
  SimParams p;
  p.seed = 4;
  p.delay = 0;
  p.maxr = 2;
  p.net.delay_us = 150000;  // ~9 frames: far beyond MAXR + D
  p.net.jitter_us = 40000;
  p.net.loss_pct = 10;
  p.change_pct = 30;
  SimResult res;
  RunSim(p, &res);
  EXPECT_EQ(CheckHealthy(p, res), "");
  EXPECT_GT(res.m[0].stats.stalls + res.m[1].stats.stalls, 0u);
  EXPECT_LE(res.m[0].stats.max_depth, 2u);
  EXPECT_LE(res.m[1].stats.max_depth, 2u);
}

TEST(Rev3Session, GateClosingAndReopeningInTheMiddleStaysInAgreement)
{
  SimParams p;
  p.seed = 5;
  p.delay = 2;
  p.maxr = 5;
  p.net.delay_us = 50000;
  p.net.jitter_us = 30000;
  p.net.loss_pct = 8;
  p.frames = 400;
  p.open_at = 30;
  p.mid_close = 120;
  p.mid_reopen = 150;
  p.close_at = 360;
  p.change_pct = 25;
  p.busy_hardware = true;
  SimResult res;
  RunSim(p, &res);
  EXPECT_EQ(CheckHealthy(p, res), "");
  EXPECT_EQ(res.m[0].stats.gate_opens, 2u);
  EXPECT_EQ(res.m[0].stats.gate_closes, 2u);
  EXPECT_EQ(res.m[1].stats.gate_opens, 2u);
}

TEST(Rev3Session, ASeedReproducesTheSameLogs)
{
  const SimParams p = ParamsFromSeed(77);
  SimResult a, b;
  RunSim(p, &a);
  RunSim(p, &b);
  EXPECT_EQ(CheckHealthy(p, a), "");
  EXPECT_EQ(a.log_hash, b.log_hash);
  EXPECT_EQ(a.m[0].log, b.m[0].log);
  EXPECT_EQ(a.m[1].log, b.m[1].log);
}

TEST(Rev3Session, ThousandSeedsWithDelayJitterAndLossAgreeWithTheReference)
{
  u64 rollbacks = 0, stalls = 0, postponed = 0, forced = 0, checks = 0, reopens = 0, deep = 0, predicted = 0, finals = 0;
  u32 failures = 0;
  for (u64 seed = 1; seed <= 1000; ++seed)
  {
    const SimParams p = ParamsFromSeed(seed);
    SimResult res;
    RunSim(p, &res);
    const std::string problem = CheckHealthy(p, res);
    if (!problem.empty())
    {
      EXPECT_EQ(problem, "") << "seed " << seed << " (delay " << p.delay << " maxr " << p.maxr << " net " << p.net.delay_us << "/"
                             << p.net.jitter_us << "/" << p.net.loss_pct << "%)";
      if (++failures >= 5)  // do not flood the output
        break;
      continue;
    }
    for (const SimMachineResult& m : res.m)
    {
      rollbacks += m.stats.rollbacks;
      stalls += m.stats.stalls;
      postponed += m.stats.postponed;
      forced += m.stats.forced_hw;
      checks += m.stats.checks_ok;
      predicted += m.stats.predicted_frames;
      finals += m.finals.size();
      deep = std::max<u64>(deep, m.stats.max_depth);
      if (m.stats.gate_opens > 1)
        ++reopens;
    }
  }
  // The seeds must actually exercise the machinery, or the test proves nothing.
  EXPECT_GT(rollbacks, 5000u);
  EXPECT_GT(stalls, 500u);
  EXPECT_GT(postponed, 50u);
  EXPECT_GT(checks, 5000u);
  EXPECT_GT(reopens, 100u);
  EXPECT_GT(predicted, 50000u);
  EXPECT_GT(finals, 200000u);
  EXPECT_GT(forced, 20u);
  EXPECT_GE(deep, 5u);
  EXPECT_LE(deep, 7u);
}

TEST(Rev3Session, LiveStatesThatLookLostOnOneSideOnlyNeverCloseTheGateOnOneSideOnly)
{
  // Many mispredictions + a "health 0" test that is a function of the LIVE state: a mispredicted timeline shows it where the true one does
  // not (phantom_lost counts those calls). A gate that followed the live state would close one machine and not the other on them; the
  // final-frame rule keeps both closes at the same frames, for the same reason, every time.
  u64 phantom = 0, closes = 0, relayouts = 0, holds = 0;
  for (u64 seed = 1; seed <= 150; ++seed)
  {
    SimParams p;
    p.seed = seed;
    p.delay = static_cast<int>(seed % 3);
    p.maxr = 3 + static_cast<int>(seed % 5);
    p.net.delay_us = 40000 + (seed % 7) * 15000;
    p.net.jitter_us = 25000;
    p.net.loss_pct = seed % 4 == 0 ? 10 : 3;
    p.frames = 420;
    p.open_at = 25;
    p.close_at = 390;
    p.change_pct = 35;
    p.lost_mod = 12 + static_cast<u32>(seed % 40);
    p.busy_hardware = seed % 3 == 0;
    p.relayout_pct = seed % 5 == 0 ? 2 : 0;
    SimResult res;
    RunSim(p, &res);
    const std::string problem = CheckHealthy(p, res);
    ASSERT_EQ(problem, "") << "seed " << seed;
    for (const SimMachineResult& m : res.m)
    {
      phantom += m.phantom_lost;
      closes += m.stats.gate_closes;
      relayouts += m.stats.relayouts;
      holds += m.stats.hw_holds;
    }
  }
  EXPECT_GT(phantom, 100u);   // the situation did occur
  EXPECT_GT(closes, 300u);
  EXPECT_GT(relayouts, 10u);
  EXPECT_GT(holds, 10u);
}

TEST(Rev3Session, AnInjectedRulesDifferenceIsCaughtAtTheNextCheckFrameByBothSides)
{
  u32 caught = 0;
  for (u64 seed = 1; seed <= 40; ++seed)
  {
    SimParams p = ParamsFromSeed(seed);
    p.mid_close = p.mid_reopen = -1;
    p.frames = 300;
    p.close_at = 280;
    p.open_at = 20 + static_cast<s32>(seed % 20);
    p.bug_frame = 100 + static_cast<s32>(seed % 17);  // the guest's game differs from frame 100.. on
    SimResult res;
    RunSim(p, &res);
    ASSERT_TRUE(res.finished) << "seed " << seed;
    const s32 expected_check = ((p.bug_frame + 1 + 29) / 30) * 30;  // the first check frame whose start state contains the difference
    const std::string want = "DESYNC frame=" + std::to_string(expected_check) + " ";
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_TRUE(res.m[i].log.find(want) != std::string::npos) << "seed " << seed << " machine " << i << " wanted '" << want << "'\n"
                                                                << res.m[i].log;
      EXPECT_TRUE(res.m[i].mode == Mode::OFFLINE) << "seed " << seed << " machine " << i;
      EXPECT_EQ(res.m[i].stats.checks_bad, 1u);
      EXPECT_EQ(res.m[i].counter, p.frames);  // the game goes on offline
    }
    if (res.m[0].log.find(want) != std::string::npos && res.m[1].log.find(want) != std::string::npos)
      ++caught;
  }
  EXPECT_EQ(caught, 40u);
}

TEST(Rev3Session, AnInjectedDifferenceNamesTheFirstDifferingFieldOnBothSides)
{
  SimParams p = ParamsFromSeed(9);
  p.mid_close = p.mid_reopen = -1;
  p.frames = 300;
  p.close_at = 280;
  p.open_at = 30;
  p.bug_frame = 105;
  SimResult res;
  RunSim(p, &res);
  ASSERT_TRUE(res.finished);
  for (int i = 0; i < 2; ++i)
  {
    // The bug flips bit 6 of state word 2 at frame 105: the dump shows the first frame whose fields differ, and the field.
    EXPECT_TRUE(res.m[i].log.find("DESYNC first_diff frame=106 field=p1.") != std::string::npos) << res.m[i].log;
    EXPECT_TRUE(res.m[i].log.find("p1 ") != std::string::npos);
    // Both sides' arrays of that frame, both players, are in the log (the 61 fields, as hex words).
    for (const char* side : {"own", "peer"})
      for (int player = 1; player <= 2; ++player)
        EXPECT_TRUE(res.m[i].log.find(std::string("DESYNC fields frame=106 side=") + side + " player=" + std::to_string(player) + " ") != std::string::npos)
            << side << " " << player << "\n"
            << res.m[i].log;
  }
}

TEST(Rev3Session, ADifferenceBeforeArmingIsCaughtInLockstep)
{
  SimParams p;
  p.seed = 12;
  p.delay = 2;
  p.maxr = 5;
  p.net.delay_us = 20000;
  p.open_at = 200;  // never armed in this run before the bug
  p.close_at = 250;
  p.frames = 300;
  p.bug_frame = 40;
  SimResult res;
  RunSim(p, &res);
  ASSERT_TRUE(res.finished);
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_TRUE(res.m[i].log.find("DESYNC frame=60 ") != std::string::npos) << res.m[i].log;
    EXPECT_TRUE(res.m[i].mode == Mode::OFFLINE);
  }
}

TEST(Rev3Session, MismatchingHellosEndBothSessionsBeforeFrameZero)
{
  SimNet net(1, NetParams{});
  FakeGame ga, gb;
  FakeStore sa(&ga), sb(&gb);
  TestSink ka, kb;
  Config ca, cb;
  ca.role = Role::Host;
  cb.role = Role::Guest;
  ca.hello.game_id = cb.hello.game_id = "RNEEDA";
  ca.hello.items = {{"cpu_core", "JIT64"}};
  cb.hello.items = {{"cpu_core", "CachedInterpreter"}};
  RollbackSession a(ca, net.ep[0], sa, ka, nullptr), b(cb, net.ep[1], sb, kb, nullptr);
  Command c;
  c.frame = 0;
  c.sp = SP;
  Reply r;
  for (int i = 0; i < 100 && (a.mode() != Mode::OFFLINE || b.mode() != Mode::OFFLINE); ++i)
  {
    net.now += 1000;
    net.DeliverUpTo(net.now);
    if (a.mode() != Mode::OFFLINE)
      a.Step(c, Closed(), &r);
    if (b.mode() != Mode::OFFLINE)
      b.Step(c, Closed(), &r);
  }
  EXPECT_TRUE(a.mode() == Mode::OFFLINE);
  EXPECT_TRUE(b.mode() == Mode::OFFLINE);
  EXPECT_TRUE(ka.Has("HELLO item cpu_core: own=JIT64 peer=CachedInterpreter"));
  EXPECT_TRUE(kb.Has("HELLO item cpu_core: own=CachedInterpreter peer=JIT64"));
}
}  // namespace
