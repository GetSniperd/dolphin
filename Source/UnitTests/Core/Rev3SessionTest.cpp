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
#include <cmath>
#include <array>
#include <cstdlib>
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
  bool Clean(s32 frame) const override { return dirty.count(frame) == 0; }
  size_t SlotCount() const override { return capacity; }
  std::string AfterSummaryLine() override { return after_summary; }

  struct Slot
  {
    std::array<u32, 4> state{};
    u32 sp = 0;
  };
  FakeGame* game;
  size_t capacity;
  std::map<s32, Slot> slots;
  std::map<s32, FieldSet> trace;
  std::set<s32> dirty;        // frames whose slot was "captured with a disc read in flight" (Clean() is false)
  std::vector<s32> restored;  // every Restore() in order
  u32 captures = 0;
  u32 clears = 0;
  u32 relayouts = 0;
  bool relayout_ok = true;
  bool ranges_ok = false;
  std::vector<RangeCrc> ranges;  // what RangeCrcs reports (REV3_NET_RANGE_CRC tests)
  bool fail_capture = false;
  std::string after_summary;  // what AfterSummaryLine returns (the SNAPSHOT line of the real store)
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
  // M5: what the real transport's TimeSync would report (off until has_offset), and how often the session asked to forget the samples.
  bool TimeOffsetUs(s32* out, u32* samples) const override
  {
    if (!has_offset)
      return false;
    *out = offset_us;
    *samples = offset_samples;
    return true;
  }
  void ResetTimeOffsets() override { ++offset_resets; }
  u64 RttUs() const override { return rtt_us; }

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
  bool has_offset = false;
  s32 offset_us = 0;         // microseconds; positive = the local machine is ahead of the peer
  u32 offset_samples = 30;
  u64 rtt_us = 0;            // 0 = no RTT estimate yet
  int offset_resets = 0;
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
  // `io_extra`: Config::io_extra, and the FakeStore holds that many more slots than MAXR + 2.
  explicit Rig(Role role = Role::Host, int delay = 0, int maxr = 5, bool range_crc = false, int io_extra = 0)
      : store(&game, static_cast<size_t>(maxr + io_extra) + 2)
  {
    cfg.range_crc = range_crc;
    cfg.io_extra = io_extra;
    cfg.role = role;
    cfg.delay = delay;
    cfg.max_rollback = maxr;
    cfg.hello.game_id = "RNEEDA";
    cfg.hello.items = {{"cpu_core", "JIT64"}, {"dsp", "HLE"}};
    wait = [](const std::function<bool()>& pred, u32) { return pred(); };
    session = std::make_unique<RollbackSession>(
        cfg, net, store, sink, [this](const std::function<bool()>& pred, u32 timeout) { return wait(pred, timeout); });
  }

  // A new session with the (changed) cfg; the old one is dropped.
  void Rebuild()
  {
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
  // The peer proposes to arm at `frame` (it sent the proposal at its call `at`).
  void PeerPropose(s32 frame, u32 id = 1, s32 at = -1)
  {
    ArmPropose m;
    m.id = id;
    m.frame = frame;
    m.at_frame = at >= 0 ? at : counter;
    m.gate = "peer gate (test)";
    net.inbox.push_back({MsgType::ArmPropose, SerializeArmPropose(m)});
  }
  void PeerAccept(u32 id, s32 frame)
  {
    ArmAccept m;
    m.id = id;
    m.frame = frame;
    m.at_frame = counter;
    net.inbox.push_back({MsgType::ArmAccept, SerializeArmAccept(m)});
  }
  void PeerReject(u32 id, s32 frame, ArmReason reason)
  {
    ArmReject m;
    m.id = id;
    m.frame = frame;
    m.at_frame = counter;
    m.reason = static_cast<u8>(reason);
    m.gate = "peer gate (test)";
    net.inbox.push_back({MsgType::ArmReject, SerializeArmReject(m)});
  }
  // Lockstep until counter = n - 1, then one call with the gate open at which the peer's proposal "arm at frame n" arrives and is accepted, and the
  // peer's equal START_CHECK queued (counter = n): the next Frame(.., Open()) arms at frame n.
  void ReadyToArm(s32 n)
  {
    Handshake();
    Lockstep(n - 1);
    PeerPropose(n);
    net.Deliver(counter, RemotePadScript(counter));
    Frame(LocalPad(counter), Open());
    PeerStartCheck();
  }
  // ReadyToArm(n - 1) and the arming call itself, with its remote pad delivered: ARMED at counter n, the newest confirmed remote pad is frame n - 1's.
  // For the tests whose first ARMED call must already be a lost one: the arm agreement itself cancels an arm at a frame with a fighter at health 0.
  void ArmedAt(s32 n)
  {
    ReadyToArm(n - 1);
    net.Deliver(counter, RemotePadScript(counter));
    Frame(LocalPad(counter), Open());
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
  cmd.pads[0][8] = 0xFF;  // analogA/B and byte 11 of the local block are not forwarded (err stays 0: a connected pad)
  cmd.pads[0][9] = 0xFF;
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

TEST(Rev3Session, PadReadReturnIsAPortMaskNotAnError)
{
  // Rev3's PADRead returns 0x80000000 (port 0 has data) on every good read (D's pad record, Oct 16); the pad must be used.
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.net.Deliver(0, P(1));
  Command cmd = rig.Cmd(0, P(0x42, 0x43));
  cmd.pad_return = static_cast<s32>(0x80000000u);
  Reply r;
  ASSERT_TRUE(rig.session->Step(cmd, Closed(), &r));
  EXPECT_TRUE(SanitizePad(r.pads[0]) == P(0x42, 0x43));
  EXPECT_TRUE(rig.net.sent.back().pad == P(0x42, 0x43));
}

TEST(Rev3Session, PadErrTransferReusesAndNoControllerIsNeutral)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.net.Deliver(0, P(1));
  rig.net.Deliver(1, P(2));
  rig.net.Deliver(2, P(3));
  rig.Frame(P(0x31), Closed());
  Command stale = rig.Cmd(1, P(0x99));
  stale.pads[0][10] = 0xFD;  // PAD_ERR_TRANSFER: the block holds the previous poll
  Reply r;
  ASSERT_TRUE(rig.session->Step(stale, Closed(), &r));
  EXPECT_TRUE(SanitizePad(r.pads[0]) == P(0x31));
  Command none = rig.Cmd(2, P(0x77));
  none.pads[0][10] = 0xFF;  // PAD_ERR_NO_CONTROLLER
  ASSERT_TRUE(rig.session->Step(none, Closed(), &r));
  EXPECT_TRUE(rig.net.sent.back().pad == Pad8{});  // neutral
}

// ---- local pad diagnostics and the guest's pad mirror (D's first record-menu, Oct 16: the guest's pad was neutral for 7972 frames) ----------

TEST(Rev3Session, MirrorGuestPlaysTheNewestConfirmedHostPadAndSendsItToTheHost)
{
  Rig rig(Role::Guest, 2);
  rig.cfg.pad_mirror = true;
  rig.Rebuild();
  EXPECT_TRUE(rig.sink.Has("NOTE pad mirror on"));
  rig.Handshake();
  // The host's pads for frames 0..4 (frames 0 and 1 are the neutral delay frames).
  rig.net.Deliver(0, Pad8{});
  rig.net.Deliver(1, Pad8{});
  rig.net.Deliver(2, P(0x01, 0x11));
  Reply r = rig.Frame(P(0x66, 0x77), Closed());  // call 0: the guest's own controller (0x66..) is NOT used
  ASSERT_EQ(rig.net.sent.size(), 2u + 1u);       // the two neutral delay frames, then frame 2
  EXPECT_EQ(rig.net.sent[2].frame, 2);
  EXPECT_TRUE(rig.net.sent[2].pad == P(0x01, 0x11)) << "newest confirmed host pad (frame 2)";
  EXPECT_TRUE(SanitizePad(r.pads[0]) == Pad8{});  // frame 0: host neutral, guest neutral (the delay)
  rig.net.Deliver(3, P(0x02));
  rig.net.Deliver(4, P(0x03, 0x33));
  rig.Frame(P(0x66), Closed());  // call 1 sends frame 3 with the newest confirmed host pad = frame 4
  EXPECT_EQ(rig.net.sent[3].frame, 3);
  EXPECT_TRUE(rig.net.sent[3].pad == P(0x03, 0x33));
  EXPECT_EQ(rig.session->stats().local_mirrored, 2u);
  EXPECT_EQ(rig.session->stats().local_active, 2u);
  // The reply of frame 2 carries the mirrored pad on game port 1 and the host's own pad on port 0.
  const Reply r2 = rig.Frame(P(0x66), Closed());
  EXPECT_TRUE(SanitizePad(r2.pads[0]) == P(0x01, 0x11));
  EXPECT_TRUE(SanitizePad(r2.pads[1]) == P(0x01, 0x11));  // the same pad: both players' cursors move together
  EXPECT_EQ(rig.sink.Count("LOCAL first input"), 1u);
  EXPECT_TRUE(rig.sink.Has("LOCAL first input frame=0 pad=0111000000000000 err=0 (mirrored from the host)")) << rig.sink.Dump();
}

TEST(Rev3Session, MirrorBeforeAnyHostPadIsNeutralAndTheHostIgnoresTheMirror)
{
  Rig guest(Role::Guest, 2);
  guest.cfg.pad_mirror = true;
  guest.Rebuild();
  guest.Handshake();
  guest.net.Deliver(0, Pad8{});
  guest.net.Deliver(1, Pad8{});
  guest.Frame(P(0x66), Closed());
  EXPECT_TRUE(guest.net.sent.back().pad == Pad8{});  // nothing from the host yet beyond the delay frames
  EXPECT_EQ(guest.session->stats().local_active, 0u);
  EXPECT_FALSE(guest.sink.Has("LOCAL first input"));

  Rig host(Role::Host, 0);
  host.cfg.pad_mirror = true;
  host.Rebuild();
  EXPECT_TRUE(host.sink.Has("ignored on the host"));
  EXPECT_FALSE(host.sink.Has("NOTE pad mirror on"));
  host.Handshake();
  host.net.Deliver(0, P(0x55));
  const Reply rh = host.Frame(P(0x66), Closed());
  EXPECT_TRUE(SanitizePad(rh.pads[0]) == P(0x66));  // the host's pad is its own controller
  EXPECT_EQ(host.session->stats().local_mirrored, 0u);
}

TEST(Rev3Session, LocalPadDiagnosticsCountActiveFramesAndTheErrBytes)
{
  Rig rig(Role::Guest, 0);
  rig.Handshake();
  const auto call = [&rig](s32 f, const Pad8& pad, u8 err) {
    rig.net.Deliver(f, Pad8{});
    Command c = rig.Cmd(f, pad);
    c.pads[0][10] = err;
    Reply r;
    ASSERT_TRUE(rig.session->Step(c, Closed(), &r));
  };
  call(0, Pad8{}, 0x00);       // connected, nothing pressed
  call(1, Pad8{}, 0x00);
  call(2, P(0x01), 0x00);      // first input
  call(3, P(0x01), 0xFF);      // no controller: neutral whatever the block holds
  call(4, P(0x02), 0xFE);      // not ready: neutral
  call(5, P(0x09), 0xFD);      // transfer error: the previous local pad (neutral, from frame 4)
  call(6, P(0x04), 0x05);      // any other value: the pad as it is
  const SessionStats& st = rig.session->stats();
  EXPECT_EQ(st.local_err[0], 3u);
  EXPECT_EQ(st.local_err[1], 1u);
  EXPECT_EQ(st.local_err[2], 1u);
  EXPECT_EQ(st.local_err[3], 1u);
  EXPECT_EQ(st.local_err[4], 1u);
  EXPECT_EQ(st.local_active, 2u);  // frames 2 and 6
  EXPECT_EQ(st.local_mirrored, 0u);
  EXPECT_EQ(rig.sink.Count("LOCAL first input"), 1u);
  EXPECT_TRUE(rig.sink.Has("LOCAL first input frame=2 pad=0100000000000000 err=0")) << rig.sink.Dump();
  rig.session->Finish();
  EXPECT_TRUE(rig.sink.Has("local_active=2 local_mirrored=0 err_hist=[3,1,1,1,1]")) << rig.sink.Dump();
}

TEST(Rev3Session, ANeutralLocalPadInTheMenusIsNotedOnceEveryTenSeconds)
{
  Rig rig(Role::Guest, 0);
  rig.Handshake();
  Gate menu = Closed();
  menu.fighters_found = false;
  const auto call = [&](s32 f, const Pad8& pad, const Gate& gate) {
    rig.net.Deliver(f, Pad8{});
    Reply r;
    ASSERT_TRUE(rig.session->Step(rig.Cmd(f, pad), gate, &r));
  };
  rig.net.now_us = 1000000;
  call(0, Pad8{}, menu);
  rig.net.now_us = 10900000;  // 9.9 s after the first call
  call(1, Pad8{}, menu);
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 0u);
  rig.net.now_us = 11000000;
  call(2, Pad8{}, menu);
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 1u);
  EXPECT_TRUE(rig.sink.Has("(err=0; err_hist=[3,0,0,0,0]")) << rig.sink.Dump();
  EXPECT_TRUE(rig.sink.Has("keyboard focus"));  // the explanation is on the first note
  rig.net.now_us = 15000000;
  call(3, Pad8{}, menu);
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 1u);  // once per 10 s
  rig.net.now_us = 21000000;
  call(4, Pad8{}, menu);
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 2u);
  EXPECT_EQ(rig.sink.Count("keyboard focus"), 1u);
  // Input resets the timer; a neutral pad in a fight (fighters found) is not noted.
  call(5, P(0x01), menu);
  rig.net.now_us = 30900000;
  call(6, Pad8{}, menu);
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 2u);
  rig.net.now_us = 60000000;
  call(7, Pad8{}, Closed());
  rig.net.now_us = 71000000;
  call(8, Pad8{}, Closed());
  EXPECT_EQ(rig.sink.Count("NOTE local pad neutral for 10 s"), 2u);
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

// Parses the newest message of a type our side sent.
ArmPropose LastPropose(const Rig& rig)
{
  ArmPropose m;
  const Message* msg = rig.net.LastOut(MsgType::ArmPropose);
  EXPECT_TRUE(msg != nullptr);
  EXPECT_TRUE(msg != nullptr && ParseArmPropose(msg->bytes, &m));
  return m;
}
ArmReject LastReject(const Rig& rig)
{
  ArmReject m;
  const Message* msg = rig.net.LastOut(MsgType::ArmReject);
  EXPECT_TRUE(msg != nullptr);
  EXPECT_TRUE(msg != nullptr && ParseArmReject(msg->bytes, &m));
  return m;
}

TEST(Rev3Session, ArmMessagesRoundTripAndRejectMalformedBytes)
{
  ArmPropose p;
  p.id = 7;
  p.frame = 1234;
  p.at_frame = 1220;
  p.gate = "open=1 why=\"x\" found_streak=3/3";
  ArmPropose p2;
  ASSERT_TRUE(ParseArmPropose(SerializeArmPropose(p), &p2));
  EXPECT_EQ(p2.id, 7u);
  EXPECT_EQ(p2.frame, 1234);
  EXPECT_EQ(p2.at_frame, 1220);
  EXPECT_EQ(p2.gate, p.gate);
  ArmAccept a{9, 55, 50};
  ArmAccept a2;
  ASSERT_TRUE(ParseArmAccept(SerializeArmAccept(a), &a2));
  EXPECT_EQ(a2.id, 9u);
  EXPECT_EQ(a2.frame, 55);
  EXPECT_EQ(a2.at_frame, 50);
  ArmReject r;
  r.id = 3;
  r.frame = -4;
  r.at_frame = 8;
  r.reason = static_cast<u8>(ArmReason::NotReady);
  r.gate = "hardware busy";
  ArmReject r2;
  ASSERT_TRUE(ParseArmReject(SerializeArmReject(r), &r2));
  EXPECT_EQ(r2.id, 3u);
  EXPECT_EQ(r2.frame, -4);
  EXPECT_EQ(r2.reason, static_cast<u8>(ArmReason::NotReady));
  EXPECT_EQ(r2.gate, "hardware busy");
  EXPECT_STREQ(ArmReasonName(r2.reason), "not ready");
  // Every truncation, an extra byte and another message's magic are refused.
  const std::vector<u8> bytes = SerializeArmReject(r);
  ArmReject unused;
  for (size_t cut = 0; cut < bytes.size(); ++cut)
    EXPECT_FALSE(ParseArmReject(std::vector<u8>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut)), &unused)) << cut;
  std::vector<u8> longer = bytes;
  longer.push_back(0);
  EXPECT_FALSE(ParseArmReject(longer, &unused));
  ArmPropose up;
  ArmAccept ua;
  EXPECT_FALSE(ParseArmPropose(SerializeArmAccept(a), &up));
  EXPECT_FALSE(ParseArmAccept(SerializeArmPropose(p), &ua));
  EXPECT_FALSE(ParseArmReject(SerializeArmPropose(p), &unused));
}

TEST(Rev3Session, AReadyGateProposesAndKeepsPlayingLockstepWhileItWaitsForTheAnswer)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  // The gate is open at call 10: the proposal goes out and the call is answered at once with the exact lockstep pads.
  rig.net.Deliver(10, RemotePadScript(10));
  const Reply r = rig.Frame(LocalPad(10), Open());
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == RemotePadScript(10));
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmPropose), 1u);
  const ArmPropose p = LastPropose(rig);
  EXPECT_EQ(p.id, 1u);
  EXPECT_EQ(p.at_frame, 10);
  EXPECT_EQ(p.frame, 10 + 14);  // no RTT estimate yet: 8 frames + 2 * D (0) + 6, above the 12 frame floor
  EXPECT_NE(p.gate.find("open=1"), std::string::npos);
  EXPECT_TRUE(rig.sink.Has("ARM propose id=1 frame=24"));
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 0u);  // nothing is sent about the state until the arm itself

  // The pad stream goes on, one pad per call, for as long as the answer takes; no second proposal is sent.
  const size_t sent_before = rig.net.sent.size();
  for (s32 f = 11; f < 24; ++f)
  {
    rig.net.Deliver(f, RemotePadScript(f));
    EXPECT_TRUE(rig.TryFrame(LocalPad(f), Open())) << "frame " << f << " must not block";
  }
  EXPECT_EQ(rig.net.sent.size(), sent_before + 13u);
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmPropose), 1u);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);

  // ACCEPT arrives; call 24 arms (and sends the START_CHECK, which is compared whenever the peer's arrives).
  rig.PeerAccept(1, 24);
  rig.net.Deliver(24, RemotePadScript(24));
  rig.Frame(LocalPad(24), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_TRUE(rig.sink.Has("ARM agreed id=1 frame=24"));
  EXPECT_TRUE(rig.sink.Has("ARM armed id=1 frame=24 (own proposal)"));
  EXPECT_TRUE(rig.sink.Has("GATE open frame=24 why=test"));
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);
  EXPECT_EQ(rig.session->stats().gate_opens, 1u);
  EXPECT_EQ(rig.session->stats().arm_proposed, 1u);
  EXPECT_EQ(rig.session->stats().arm_waits, 0u);  // the answer was here before call F: no wait at all
  EXPECT_EQ(rig.store.captures, 1u);
  EXPECT_FALSE(rig.sink.Has("CHK start"));  // the peer's START_CHECK is not here yet
  rig.PeerStartCheck();                     // for frame 25: not the arm frame, ignored
  rig.net.Deliver(25, RemotePadScript(25));
  rig.Frame(LocalPad(25), Open());
  EXPECT_FALSE(rig.sink.Has("CHK start"));
}

TEST(Rev3Session, TheMarginFollowsTheRoundTripAndTheInputDelay)
{
  Rig rig(Role::Host, 3);
  rig.net.rtt_us = 150000;  // 9 frames of 16.683 ms
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  EXPECT_EQ(LastPropose(rig).frame, 10 + 9 + 2 * 3 + 6);
}

TEST(Rev3Session, TheProposerWaitsAtTheArmFrameOnlyForTheAnswerAndSendsItsPadFirst)
{
  Rig rig(Role::Host, 2);
  rig.net.now_us = 1000;
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  const s32 f = LastPropose(rig).frame;  // 10 + 8 + 4 + 6 = 28
  EXPECT_EQ(f, 28);
  for (s32 c = 11; c < f; ++c)
  {
    rig.net.Deliver(c, RemotePadScript(c));
    rig.Frame(LocalPad(c), Open());
  }
  // Call F, the answer is not here: the call blocks, but its own pad (for F + D) is already out, so the peer can go on to call F + D.
  rig.net.Deliver(f, RemotePadScript(f));
  EXPECT_FALSE(rig.TryFrame(LocalPad(f), Open()));
  EXPECT_EQ(rig.net.sent.back().frame, f + 2);
  EXPECT_EQ(rig.session->BlockTimeoutMs(), 10000u);
  EXPECT_FALSE(rig.session->BlockSatisfied());
  EXPECT_EQ(rig.session->stats().arm_waits, 1u);
  rig.net.now_us += 40000;
  rig.PeerAccept(1, f);
  EXPECT_TRUE(rig.session->BlockSatisfied());
  EXPECT_TRUE(rig.TryFrame(LocalPad(f), Open()));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.net.sent.back().frame, f + 2);  // the retry did not send the pad twice
  EXPECT_EQ(rig.session->stats().arm_waits, 1u);
  EXPECT_GE(rig.session->stats().arm_wait_us_max, 40000u);
}

TEST(Rev3Session, ARejectionAtTheArmFrameEndsTheWaitAndLockstepGoesOn)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  const s32 f = LastPropose(rig).frame;
  for (s32 c = 11; c < f; ++c)
  {
    rig.net.Deliver(c, RemotePadScript(c));
    rig.Frame(LocalPad(c), Open());
  }
  rig.net.Deliver(f, RemotePadScript(f));
  EXPECT_FALSE(rig.TryFrame(LocalPad(f), Open()));
  rig.PeerReject(1, f, ArmReason::NotReady);
  EXPECT_TRUE(rig.session->BlockSatisfied());
  const Reply r = rig.Frame(LocalPad(f), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(rig.session->stats().arm_rejected, 1u);
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 0u);
  EXPECT_TRUE(rig.sink.Has("rejected by the peer: not ready"));
  EXPECT_TRUE(rig.sink.Has("peer gate: peer gate (test)"));
  EXPECT_TRUE(rig.sink.Has("own gate: open=1"));
}

TEST(Rev3Session, NoAnswerWithinTheWaitLimitLeavesTheSession)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  const s32 f = LastPropose(rig).frame;
  for (s32 c = 11; c < f; ++c)
  {
    rig.net.Deliver(c, RemotePadScript(c));
    rig.Frame(LocalPad(c), Open());
  }
  rig.net.Deliver(f, RemotePadScript(f));
  rig.wait = [&](const std::function<bool()>&, u32 timeout) {
    EXPECT_EQ(timeout, 10000u);
    return false;
  };
  const Reply r = rig.session->OnCommand(rig.Cmd(f, LocalPad(f)), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(rig.sink.Has("LEAVE"));
  EXPECT_TRUE(rig.sink.Has("no answer to ARM_PROPOSE id=1 frame=" + std::to_string(f)));
  EXPECT_EQ(rig.session->leave_reason().find("no answer to ARM_PROPOSE"), 0u);
}

TEST(Rev3Session, APeerProposalIsAcceptedWhenTheGateIsReadyAndBothArmAtItsFrame)
{
  Rig rig(Role::Guest, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.PeerPropose(25, 5, 9);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmAccept), 1u);
  ArmAccept a;
  ASSERT_TRUE(ParseArmAccept(rig.net.LastOut(MsgType::ArmAccept)->bytes, &a));
  EXPECT_EQ(a.id, 5u);
  EXPECT_EQ(a.frame, 25);
  EXPECT_EQ(a.at_frame, 10);
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmPropose), 0u);  // an agreed arm is not proposed again
  EXPECT_EQ(rig.session->stats().arm_accepted, 1u);
  EXPECT_TRUE(rig.sink.Has("ARM accept id=5 frame=25"));
  // Committed: it arms at 25 whatever its own gate does in between (hardware busy, the set not ready).
  for (s32 f = 11; f < 25; ++f)
  {
    rig.net.Deliver(f, RemotePadScript(f));
    Gate g = Closed();
    rig.Frame(LocalPad(f), g);
    EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  }
  rig.net.Deliver(25, RemotePadScript(25));
  rig.Frame(LocalPad(25), Closed());  // the gate is shut here; the shared state (fighters, health) is fine: arm
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_TRUE(rig.sink.Has("ARM armed id=5 frame=25 (the peer's proposal)"));
  EXPECT_TRUE(rig.sink.Has("installing the snapshot set now"));
  EXPECT_EQ(rig.store.relayouts, 1u);
  EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);
}

TEST(Rev3Session, APeerProposalIsRejectedWithBothGatesWhenTheGateIsNotReady)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.PeerPropose(25, 5, 9);
  rig.net.Deliver(10, RemotePadScript(10));
  Gate g = Closed();
  g.why = "hardware busy";
  g.detail = "found_streak=60/60 quiet_streak=2/30";
  const Reply r = rig.Frame(LocalPad(10), g);
  EXPECT_EQ(r.flags, 0u);  // the call itself is plain lockstep
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmReject), 1u);
  const ArmReject rej = LastReject(rig);
  EXPECT_EQ(rej.id, 5u);
  EXPECT_EQ(rej.frame, 25);
  EXPECT_EQ(rej.reason, static_cast<u8>(ArmReason::NotReady));
  EXPECT_EQ(rej.at_frame, 10);
  EXPECT_NE(rej.gate.find("why=\"hardware busy\""), std::string::npos);
  EXPECT_NE(rej.gate.find("quiet_streak=2/30"), std::string::npos);  // the inputs the log is for
  EXPECT_TRUE(rig.sink.Has("ARM reject id=5 frame=25 reason=not ready"));
  EXPECT_TRUE(rig.sink.Has("peer gate: peer gate (test)"));
  EXPECT_EQ(rig.session->stats().arm_refused, 1u);
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  // Its own gate becomes ready two frames later: it proposes itself (and the peer was not wrongly counted as agreed).
  rig.net.Deliver(11, RemotePadScript(11));
  rig.Frame(LocalPad(11), Closed());
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmPropose), 0u);
  rig.net.Deliver(12, RemotePadScript(12));
  rig.Frame(LocalPad(12), Open());
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmPropose), 1u);
  EXPECT_EQ(LastPropose(rig).frame, 12 + 14);
}

TEST(Rev3Session, AProposalThatIsAlreadyPastIsRejectedTooLate)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.PeerPropose(10, 2, 3);  // F = our current call: not ahead of us any more
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Closed());
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmAccept), 0u);
  EXPECT_EQ(LastReject(rig).reason, static_cast<u8>(ArmReason::TooLate));
  // ... while one for the next frame is still accepted.
  rig.PeerPropose(12, 3, 9);
  rig.net.Deliver(11, RemotePadScript(11));
  rig.Frame(LocalPad(11), Open());
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmAccept), 1u);
  ArmAccept a;
  ASSERT_TRUE(ParseArmAccept(rig.net.LastOut(MsgType::ArmAccept)->bytes, &a));
  EXPECT_EQ(a.id, 3u);
}

TEST(Rev3Session, AProposalThatArrivesWhileTheCallWaitsIsAnsweredInTheWaitNotAfterIt)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  // Call 10 waits for the remote pad of frame 10. The peer, itself at the arm frame, is waiting for our answer: it must get one now.
  EXPECT_FALSE(rig.TryFrame(LocalPad(10), Open()));
  const size_t proposals = rig.net.CountOut(MsgType::ArmPropose);
  rig.PeerPropose(20, 4, 8);
  EXPECT_FALSE(rig.session->BlockSatisfied());                     // still no pad
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmAccept) + rig.net.CountOut(MsgType::ArmReject), 1u);  // but the answer is out
  EXPECT_EQ(proposals, rig.net.CountOut(MsgType::ArmPropose));
}

TEST(Rev3Session, CrossedProposalsTheHostsWinsAndTheGuestAnswersIt)
{
  Rig host(Role::Host, 0);
  Rig guest(Role::Guest, 0);
  for (Rig* r : {&host, &guest})
  {
    r->Handshake();
    r->Lockstep(10);
    r->net.Deliver(10, RemotePadScript(10));
    r->Frame(LocalPad(10), Open());
    ASSERT_EQ(r->net.CountOut(MsgType::ArmPropose), 1u);
  }
  const ArmPropose from_host = LastPropose(host);
  const ArmPropose from_guest = LastPropose(guest);
  // Each receives the other's proposal at its next call.
  host.net.inbox.push_back(*guest.net.LastOut(MsgType::ArmPropose));
  guest.net.inbox.push_back(*host.net.LastOut(MsgType::ArmPropose));
  host.net.Deliver(11, RemotePadScript(11));
  guest.net.Deliver(11, RemotePadScript(11));
  host.Frame(LocalPad(11), Open());
  guest.Frame(LocalPad(11), Open());
  // The host refuses the guest's (crossed); the guest drops its own and accepts the host's.
  EXPECT_EQ(host.net.CountOut(MsgType::ArmAccept), 0u);
  EXPECT_EQ(LastReject(host).reason, static_cast<u8>(ArmReason::Crossed));
  EXPECT_EQ(guest.net.CountOut(MsgType::ArmReject), 0u);
  ASSERT_EQ(guest.net.CountOut(MsgType::ArmAccept), 1u);
  ArmAccept a;
  ASSERT_TRUE(ParseArmAccept(guest.net.LastOut(MsgType::ArmAccept)->bytes, &a));
  EXPECT_EQ(a.id, from_host.id);
  EXPECT_EQ(a.frame, from_host.frame);
  EXPECT_TRUE(guest.sink.Has("ARM crossed: dropping own proposal"));
  // The host gets the accept, the guest the host's rejection of its dropped proposal (ignored): both arm at the host's frame.
  host.net.inbox.push_back(*guest.net.LastOut(MsgType::ArmAccept));
  guest.net.inbox.push_back(*host.net.LastOut(MsgType::ArmReject));
  s32 hc = 12;
  s32 gc = 12;
  for (; hc <= from_host.frame; ++hc)
  {
    host.net.Deliver(hc, RemotePadScript(hc));
    host.Frame(LocalPad(hc), Open());
  }
  for (; gc <= from_host.frame; ++gc)
  {
    guest.net.Deliver(gc, RemotePadScript(gc));
    guest.Frame(LocalPad(gc), Open());
  }
  EXPECT_TRUE(host.session->mode() == Mode::ARMED);
  EXPECT_TRUE(guest.session->mode() == Mode::ARMED);
  EXPECT_TRUE(host.sink.Has("GATE open frame=" + std::to_string(from_host.frame)));
  EXPECT_TRUE(guest.sink.Has("GATE open frame=" + std::to_string(from_host.frame)));
  EXPECT_NE(from_guest.id + from_guest.frame, 0u);
}

TEST(Rev3Session, ARejectedSideBacksOffAndTooLateGrowsTheMargin)
{
  Rig rig(Role::Host, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  EXPECT_EQ(LastPropose(rig).frame, 24);
  rig.PeerReject(1, 24, ArmReason::NotReady);
  s32 c = 11;
  // First rejection: the next proposal comes 30 frames after the call that saw the rejection (call 11).
  for (; c < 41; ++c)
  {
    rig.net.Deliver(c, RemotePadScript(c));
    rig.Frame(LocalPad(c), Open());
    EXPECT_EQ(rig.net.CountOut(MsgType::ArmPropose), 1u) << "call " << c;
  }
  rig.net.Deliver(c, RemotePadScript(c));
  rig.Frame(LocalPad(c), Open());
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmPropose), 2u);
  EXPECT_EQ(LastPropose(rig).id, 2u);
  EXPECT_EQ(LastPropose(rig).frame, 41 + 14);
  // TooLate: try again at once with a 16 frame longer margin.
  rig.PeerReject(2, 55, ArmReason::TooLate);
  ++c;
  rig.net.Deliver(c, RemotePadScript(c));
  rig.Frame(LocalPad(c), Open());  // sees the rejection
  ++c;
  rig.net.Deliver(c, RemotePadScript(c));
  rig.Frame(LocalPad(c), Open());
  ASSERT_EQ(rig.net.CountOut(MsgType::ArmPropose), 3u);
  EXPECT_EQ(LastPropose(rig).frame, c + 14 + 16);
}

TEST(Rev3Session, AnAgreedFrameWithoutFightersOrWithAFighterAtHealthZeroIsCancelledAlikeOnBothSides)
{
  for (int kind = 0; kind < 2; ++kind)
  {
    Rig rig(Role::Guest, 0);
    rig.Handshake();
    rig.Lockstep(10);
    rig.PeerPropose(14, 1, 9);
    rig.net.Deliver(10, RemotePadScript(10));
    rig.Frame(LocalPad(10), Open());
    ASSERT_EQ(rig.net.CountOut(MsgType::ArmAccept), 1u);
    for (s32 f = 11; f < 14; ++f)
    {
      rig.net.Deliver(f, RemotePadScript(f));
      rig.Frame(LocalPad(f), Open());
    }
    rig.net.Deliver(14, RemotePadScript(14));
    rig.Frame(LocalPad(14), kind == 0 ? NoFighters() : Lost());
    EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP) << kind;
    EXPECT_EQ(rig.session->stats().arm_cancelled, 1u);
    EXPECT_TRUE(rig.sink.Has("ARM id=1 frame=14 cancelled"));
    EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 0u);
    EXPECT_EQ(rig.session->stats().gate_opens, 0u);
  }
}

TEST(Rev3Session, ArmingWithTheHardwareBusyPlaysTheFirstArmedCallWithTheRealPad)
{
  Rig rig(Role::Guest, 0);
  rig.Handshake();
  rig.Lockstep(10);
  rig.PeerPropose(12, 1, 9);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(11, RemotePadScript(11));
  rig.Frame(LocalPad(11), Open());
  // Call 12 arms with a disc read in flight and its remote pad not here: a HOLD, so the call waits instead of predicting.
  EXPECT_FALSE(rig.TryFrame(LocalPad(12), Busy()));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().hw_holds, 1u);
  EXPECT_EQ(rig.session->stats().predicted_frames, 0u);
  rig.net.Deliver(12, RemotePadScript(12));
  EXPECT_TRUE(rig.session->BlockSatisfied());
  const Reply r = rig.Frame(LocalPad(12), Busy());
  EXPECT_TRUE(SanitizePad(r.pads[0]) == RemotePadScript(12));  // the guest plays game port 1; the host's pad is port 0
}

TEST(Rev3Session, TheStartCheckNeverBlocksAndAMismatchIsADesync)
{
  // Equal state, the peer's START_CHECK already here when we arm.
  {
    Rig rig(Role::Host, 0);
    rig.ReadyToArm(10);
    rig.net.Deliver(10, RemotePadScript(10));
    rig.Frame(LocalPad(10), Open());
    EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
    EXPECT_TRUE(rig.sink.Has("CHK start frame=10"));
    EXPECT_EQ(rig.net.CountOut(MsgType::StartCheck), 1u);
  }
  // The peer's START_CHECK arrives after the arm: compared then.
  {
    Rig rig(Role::Host, 0);
    rig.Handshake();
    rig.Lockstep(9);
    rig.PeerPropose(10);
    rig.net.Deliver(9, RemotePadScript(9));
    rig.Frame(LocalPad(9), Open());
    rig.net.Deliver(10, RemotePadScript(10));
    rig.Frame(LocalPad(10), Open());
    EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
    EXPECT_FALSE(rig.sink.Has("CHK start"));
    const s32 mine_frame = 10;
    StartCheck sc;
    ASSERT_TRUE(ParseStartCheck(rig.net.LastOut(MsgType::StartCheck)->bytes, &sc));
    EXPECT_EQ(sc.frame, mine_frame);
    rig.net.inbox.push_back({MsgType::StartCheck, rig.net.LastOut(MsgType::StartCheck)->bytes});
    rig.net.Deliver(11, RemotePadScript(11));
    rig.Frame(LocalPad(11), Open());
    EXPECT_TRUE(rig.sink.Has("CHK start frame=10"));
    EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  }
  // A different state at the arm frame: DESYNC naming the items, and the session leaves.
  {
    Rig rig(Role::Host, 0);
    rig.Handshake();
    rig.Lockstep(9);
    rig.PeerPropose(10);
    rig.net.Deliver(9, RemotePadScript(9));
    rig.Frame(LocalPad(9), Open());
    rig.counter = 10;
    rig.PeerStartCheck(/*equal=*/false);
    rig.net.Deliver(10, RemotePadScript(10));
    rig.Frame(LocalPad(10), Open());
    EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
    EXPECT_TRUE(rig.sink.Has("DESYNC start frame=10"));
    EXPECT_TRUE(rig.sink.Has("START_CHECK checksum:"));
    EXPECT_TRUE(rig.sink.Has("START_CHECK field p2"));
    EXPECT_EQ(rig.session->stats().checks_bad, 1u);
    EXPECT_FALSE(rig.sink.osd.empty());
  }
}

TEST(Rev3Session, ProposalsAreAnsweredNotInLockstepWhenArmedAndNeverHang)
{
  Rig rig(Role::Host, 0);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  ASSERT_TRUE(rig.session->mode() == Mode::ARMED);
  rig.PeerPropose(40, 9, 12);
  rig.net.Deliver(10, RemotePadScript(10));
  rig.Frame(LocalPad(11), Open());
  EXPECT_EQ(LastReject(rig).reason, static_cast<u8>(ArmReason::NotLockstep));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
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

TEST(Rev3Session, BusyHardwareDefersARollbackPastMaxrAndItRunsAtTheGreaterDepthOnceTheReadEnds)
{
  // D's soak 20261017_113023: the old rule restored anyway when postponing once more would have made the rollback deeper than MAXR ("forced",
  // forced_hw 36 / 34) and the nw4r stream thread froze afterwards. Now the restore waits for the read, however deep that makes it, as long as the
  // ring (MAXR + 2 + io_extra slots) still holds the target.
  Rig rig(Role::Host, 0, 4, false, 6);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 11; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, P(0xA1));  // frame 10 was mispredicted
  Reply r;
  for (s32 f = 12; f <= 17; ++f)  // depth 2 .. 7 at the call: past MAXR (4) from frame 14 on
  {
    rig.net.Deliver(f - 1, RemotePadScript(f - 1));  // R follows the calls: what limits the wait is the disc, not the stall rule (predicted <= MAXR)
    ASSERT_TRUE(rig.TryFrame(LocalPad(f), Busy(), &r)) << f;
    EXPECT_EQ(r.flags, 0u) << "a restore under a disc read at frame " << f;
  }
  EXPECT_TRUE(rig.store.restored.empty());
  EXPECT_EQ(rig.session->stats().postponed, 6u);
  EXPECT_EQ(rig.session->stats().io_postponed_rollbacks, 1u);
  EXPECT_EQ(rig.session->stats().io_postponed_max_calls, 6u);
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  EXPECT_EQ(rig.session->stats().io_overflow, 0u);
  // The read ends: the rollback runs at once, to the same target, 8 frames deep (call 18 - frame 10), and is counted and logged as a deep one.
  rig.net.Deliver(17, RemotePadScript(17));
  ASSERT_TRUE(rig.TryFrame(LocalPad(18), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
  EXPECT_EQ(rig.session->stats().max_depth, 8u);
  EXPECT_EQ(rig.session->stats().deep_rollbacks, 1u);
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  EXPECT_TRUE(rig.sink.Has("ROLLBACK deep depth=8 (MAXR 4) frame=10 live=18"));
  EXPECT_TRUE(rig.sink.Has("waited=6"));
  EXPECT_FALSE(rig.sink.Has("forced"));
  // The resimulation of the 8 frames works and the session carries on armed.
  for (s32 f = 11; f <= 18; ++f)
    rig.Frame(LocalPad(f), Open());
  EXPECT_FALSE(rig.sink.Has("ERROR"));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_FALSE(rig.session->Resimulating());
  EXPECT_EQ(rig.store.restored.size(), 1u);
}

TEST(Rev3Session, ADeferredRollbackKeepsItsTargetInTheRingByNotCapturingWhenTheRingIsFull)
{
  // MAXR 2 + io_extra 3 = 7 slots. The read lasts longer: the target (frame 10) would be overwritten by the capture of frame 17, so from there on
  // the frames are played with real pads and no snapshot (io_overflow), and the restore still goes to frame 10 when the read ends.
  Rig rig(Role::Host, 0, 2, false, 3);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(10, P(0xA1));  // frame 10 mispredicted (D = 0: it was predicted from the neutral pad before)
  Reply r;
  for (s32 f = 11; f <= 16; ++f)
  {
    if (f > 11)
      rig.net.Deliver(f - 1, RemotePadScript(f - 1));  // R follows, so the stall rule (predicted <= MAXR) is not what limits the wait
    ASSERT_TRUE(rig.TryFrame(LocalPad(f), Busy(), &r)) << f;
    EXPECT_EQ(r.flags, 0u);
  }
  EXPECT_EQ(rig.session->stats().io_overflow, 0u) << "frames 11..16 still fit the 7 slots with frame 10";
  EXPECT_TRUE(rig.store.Has(10));
  const u32 captures = rig.store.captures;
  rig.net.Deliver(16, RemotePadScript(16));
  // Frame 17: capturing it would evict frame 10. The remote pad of 17 is not here (the stall rule's business is predictions only, and none is made).
  EXPECT_FALSE(rig.TryFrame(LocalPad(17), Busy(), &r)) << "no capture, no prediction: the call waits for the real pad of its own frame";
  rig.net.Deliver(17, RemotePadScript(17));
  ASSERT_TRUE(rig.TryFrame(LocalPad(17), Busy(), &r));
  EXPECT_EQ(r.flags, 0u);
  EXPECT_TRUE(SanitizePad(r.pads[1]) == RemotePadScript(17));
  EXPECT_EQ(rig.store.captures, captures) << "frame 17 was not captured";
  EXPECT_TRUE(rig.store.Has(10));
  EXPECT_EQ(rig.session->stats().io_overflow, 1u);
  EXPECT_EQ(rig.session->stats().io_overflow_calls, 1u);
  EXPECT_TRUE(rig.sink.Has("WARNING ROLLBACK frame=10 target=10 live=17"));
  for (s32 f = 18; f <= 20; ++f)
  {
    rig.net.Deliver(f, RemotePadScript(f));
    ASSERT_TRUE(rig.TryFrame(LocalPad(f), Busy(), &r)) << f;
    EXPECT_EQ(r.flags, 0u) << "never a restore while the read is in flight";
  }
  EXPECT_EQ(rig.session->stats().io_overflow, 1u) << "one wait is counted once";
  EXPECT_EQ(rig.session->stats().io_overflow_calls, 4u);
  EXPECT_TRUE(rig.store.restored.empty());
  rig.net.Deliver(21, RemotePadScript(21));
  ASSERT_TRUE(rig.TryFrame(LocalPad(21), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
  EXPECT_EQ(rig.session->stats().max_depth, 11u);
  EXPECT_EQ(rig.session->stats().deep_rollbacks, 1u);
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  // 11 frames of resimulation with the confirmed pads; the game state is the one a game that never mispredicted would have.
  for (s32 f = 11; f <= 21; ++f)
    rig.Frame(LocalPad(f), Open());
  EXPECT_FALSE(rig.sink.Has("ERROR"));
  EXPECT_FALSE(rig.session->Resimulating());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
}

TEST(Rev3Session, TheCleanTargetRedirectCountsForTheRingRoomOfAWaitingRollback)
{
  // Frame 11's slot is dirty (captured under a read): the rollback for a misprediction at 11 goes back to 10, and the ring has to keep 10.
  Rig rig(Role::Host, 0, 2, false, 2);  // 6 slots
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  rig.store.dirty = {11};
  rig.net.Deliver(10, RemotePadScript(9));
  rig.net.Deliver(11, P(0xA1));
  Reply r;
  for (s32 f = 12; f <= 14; ++f)
  {
    if (f > 12)
      rig.net.Deliver(f - 1, RemotePadScript(f - 1));
    ASSERT_TRUE(rig.TryFrame(LocalPad(f), Busy(), &r)) << f;
  }
  EXPECT_EQ(rig.session->stats().io_overflow, 0u);
  // Capturing frame 16 would evict frame 10 (6 slots: 11..16): the call at 15 is the last that captures (10..15).
  rig.net.Deliver(14, RemotePadScript(14));
  ASSERT_TRUE(rig.TryFrame(LocalPad(15), Busy(), &r));
  EXPECT_EQ(rig.session->stats().io_overflow, 0u);
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Busy(), &r));  // overflow: waits for its real pad, no capture
  EXPECT_EQ(rig.session->stats().io_overflow, 1u);
  EXPECT_TRUE(rig.sink.Has("WARNING ROLLBACK frame=11 target=10 live=16"));
  EXPECT_TRUE(rig.store.Has(10));
}

TEST(Rev3Session, ADrainDoesNotRestoreUnderAReadItPlaysItsFramesWithRealPadsUntilTheReadEnds)
{
  // The scheduled close arrives while a rollback waits for a disc read: the drain captures nothing, so it keeps playing (real pads) until the
  // read has ended and only then restores.
  Rig rig(Role::Host, 0, 5, false, 4);
  rig.ArmedAt(10);
  const Pad8 predicted = RemotePadScript(9);
  rig.Frame(LocalPad(10), Lost());
  for (s32 f = 11; f <= 12; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.net.Deliver(10, predicted);
  rig.net.Deliver(11, predicted);
  rig.net.Deliver(12, P(0xA1));  // frame 12 mispredicted
  for (s32 f = 13; f <= 15; ++f)
    rig.Frame(LocalPad(f), Busy());
  EXPECT_TRUE(rig.store.restored.empty());
  // Call 16 = the agreed close: DRAIN. Everything below 16 must be confirmed, and the hardware is still busy.
  EXPECT_FALSE(rig.TryFrame(LocalPad(16), Busy()));
  EXPECT_TRUE(rig.session->mode() == Mode::DRAIN);
  for (s32 f = 13; f <= 15; ++f)
    rig.net.Deliver(f, predicted);
  rig.net.Deliver(16, P(0xD3));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(16), Busy(), &r));
  EXPECT_EQ(r.flags, 0u) << "no restore under the read, not even in a drain";
  EXPECT_TRUE(SanitizePad(r.pads[1]) == P(0xD3));
  EXPECT_TRUE(rig.store.restored.empty());
  EXPECT_TRUE(rig.session->mode() == Mode::DRAIN);
  rig.net.Deliver(17, P(0x17));
  ASSERT_TRUE(rig.TryFrame(LocalPad(17), Busy(), &r));
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
  // The read ends at call 18: the rollback to frame 12 runs, 6 frames deep, and the drain finishes after the resimulation.
  rig.net.Deliver(18, P(0x18));
  ASSERT_TRUE(rig.TryFrame(LocalPad(18), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 12u);
  EXPECT_EQ(rig.session->stats().max_depth, 6u);
  EXPECT_EQ(rig.session->stats().deep_rollbacks, 1u);
  for (s32 f = 13; f <= 18; ++f)
    rig.Frame(LocalPad(f), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  EXPECT_FALSE(rig.sink.Has("ERROR"));
  EXPECT_EQ(rig.session->stats().gate_late, 0u);
}

TEST(Rev3Session, AWaitThatOutlastsTheSessionsRecordsLeavesInsteadOfRestoringUnderTheRead)
{
  Rig rig(Role::Host, 0, 2, false, 0);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.net.Deliver(10, P(0xA1));
  Reply r;
  s32 f = 11;
  for (; f < 80 && rig.session->mode() == Mode::ARMED; ++f)
  {
    rig.net.Deliver(f, RemotePadScript(f));
    if (!rig.TryFrame(LocalPad(f), Busy(), &r))
      break;
    EXPECT_EQ(r.flags, 0u);
  }
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(rig.sink.Has("ERROR ROLLBACK frame=10"));
  EXPECT_TRUE(rig.store.restored.empty());
  EXPECT_EQ(rig.session->stats().forced_hw, 0u);
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
  rig.ArmedAt(10);
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
  rig.ArmedAt(10);
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

  // The rollback that is still due (frame 12) is done now that the hardware is quiet (with a read in flight the drain would wait for it: see
  // ADrainDoesNotRestoreUnderAReadItPlaysItsFramesWithRealPadsUntilTheReadEnds).
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(16), Open(), &r));
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
  rig.ArmedAt(10);
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
  rig.Lockstep(2);  // frames 17, 18
  rig.PeerPropose(20, 2, 18);  // the second arming is another agreement
  rig.net.Deliver(19, RemotePadScript(19));
  rig.Frame(LocalPad(19), Open());
  EXPECT_EQ(rig.net.CountOut(MsgType::ArmAccept), 2u);  // (the first one was the arm at frame 9 of ArmedAt)
  rig.PeerStartCheck();  // frame 20
  rig.net.Deliver(20, RemotePadScript(20));
  rig.Frame(LocalPad(20), Open());
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().gate_opens, 2u);
}

TEST(Rev3Session, TheCloseOfALostFrameIsScheduledOnlyOnce)
{
  Rig rig(Role::Host, 0, 5);
  rig.ArmedAt(10);
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
  rig.Frame(LocalPad(12), Open());  // second run of 12: the end of the resimulation verifies and finalizes frame 11 (its pad arrived just before)
  EXPECT_TRUE(rig.sink.Has("GATE close scheduled final_frame=11 effective_frame=17"));
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

TEST(Rev3Session, ARollbackWhoseSlotWasCapturedDuringADiscReadGoesToTheNewestEarlierCleanSlot)
{
  // D's freeze at the match end: a restore to a frame captured while a stream read was in flight leaves the game's DVD library waiting for a
  // completion that already happened. The session restores the newest earlier clean slot instead and resimulates the frames in between.
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  rig.Frame(LocalPad(12), Open());
  rig.store.dirty = {11};
  rig.net.Deliver(10, RemotePadScript(9));  // frame 10 was predicted right
  rig.net.Deliver(11, P(0xA1));             // frame 11 was not
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u) << "frame 11's slot is dirty: back to frame 10";
  ASSERT_EQ(rig.store.restored.size(), 1u);
  EXPECT_EQ(rig.store.restored[0], 10);
  EXPECT_EQ(rig.session->stats().io_dirty_targets, 1u);
  EXPECT_EQ(rig.session->stats().io_redirected, 1u);
  EXPECT_EQ(rig.session->stats().io_no_clean_slot, 0u);
  EXPECT_EQ(rig.session->stats().max_depth, 3u) << "13 - 10: one frame more than the misprediction needed";
  EXPECT_TRUE(rig.sink.Has("ROLLBACK redirected frame=11 -> 10"));
  for (s32 f = 11; f <= 13; ++f)
    rig.Frame(LocalPad(f), Open());
  EXPECT_FALSE(rig.sink.Has("ERROR"));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_FALSE(rig.session->Resimulating());
}

TEST(Rev3Session, TheRedirectSkipsEveryDirtySlotInARow)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  for (s32 f = 10; f <= 13; ++f)
    rig.Frame(LocalPad(f), Open());
  rig.store.dirty = {11, 12};
  rig.net.Deliver(10, RemotePadScript(9));
  rig.net.Deliver(11, RemotePadScript(9));
  rig.net.Deliver(12, P(0xA1));  // frame 12 mispredicted; its slot and frame 11's are dirty
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(14), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 10u);
  EXPECT_EQ(rig.session->stats().io_redirected, 1u);
  EXPECT_EQ(rig.session->stats().io_dirty_targets, 1u);
}

TEST(Rev3Session, WithNoCleanSlotInTheRingTheRollbackStillRunsAndIsCountedAndWarnedAbout)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  rig.Frame(LocalPad(12), Open());
  rig.store.dirty = {10, 11, 12};
  rig.net.Deliver(10, RemotePadScript(9));
  rig.net.Deliver(11, P(0xA1));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(13), Open(), &r));
  EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
  EXPECT_EQ(r.frame, 11u) << "nothing clean to go back to: the misprediction's own slot, as before";
  EXPECT_EQ(rig.session->stats().io_dirty_targets, 1u);
  EXPECT_EQ(rig.session->stats().io_redirected, 0u);
  EXPECT_EQ(rig.session->stats().io_no_clean_slot, 1u);
  EXPECT_TRUE(rig.sink.Has("WARNING ROLLBACK frame=11: its slot was captured with a disc read in flight and no earlier slot in the ring is clean"));
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
}

TEST(Rev3Session, CleanSlotsRollBackExactlyWhereTheyDidAndTheSummaryCarriesTheCounters)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  rig.net.Deliver(10, P(0xA1));
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(12), Open(), &r));
  EXPECT_EQ(r.frame, 10u);
  EXPECT_EQ(rig.session->stats().io_dirty_targets, 0u);
  EXPECT_FALSE(rig.sink.Has("redirected"));
  rig.session->Finish();
  EXPECT_TRUE(rig.sink.Has("io_redirected=0 io_no_clean_slot=0"));
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

TEST(Rev3Session, TheStoresSnapshotLineFollowsEverySummaryLine)
{
  Rig rig(Role::Host, 0, 5);
  rig.store.after_summary = "SNAPSHOT kind=fake pages=1";
  rig.ReadyToArm(10);
  PlayRollbacks(rig, 10, 3);
  rig.session->Finish();
  ASSERT_TRUE(rig.sink.Has("SUMMARY final"));
  size_t summary = rig.sink.lines.size();
  for (size_t i = 0; i < rig.sink.lines.size(); ++i)
    if (rig.sink.lines[i].find("SUMMARY final") != std::string::npos)
      summary = i;
  ASSERT_LT(summary + 1, rig.sink.lines.size());
  EXPECT_EQ(rig.sink.lines[summary + 1], "SNAPSHOT kind=fake pages=1");
  EXPECT_EQ(rig.sink.Count("SNAPSHOT kind=fake"), 1u);
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

// ---- M5: pacing policy, time offsets, stall run, resimulation cost, adaptive MAXR ----------------------------------------------

class RecordingPacer final : public PaceActuator
{
public:
  void Delay(u32 microseconds) override
  {
    delays.push_back(microseconds);
    total += microseconds;
  }
  std::vector<u32> delays;
  u64 total = 0;
};

// `n` ARMED calls with every remote pad in time and equal to the one the lockstep ended with: no prediction, no rollback, no stall.
void ArmedFrames(Rig& rig, s32 n)
{
  const Pad8 same = RemotePadScript(9);
  for (s32 i = 0; i < n; ++i)
  {
    rig.net.Deliver(rig.counter, same);
    rig.Frame(LocalPad(rig.counter), Open());
    rig.net.now_us += 16683;
  }
}

// One rollback of `depth` frames whose resimulation takes `cost_us` of (test) time; afterwards every pad is in and the next call is a fresh one.
void RollbackOf(Rig& rig, s32 depth, u64 cost_us, int tag)
{
  const s32 c = rig.counter;
  for (s32 i = 0; i < depth; ++i)
    rig.Frame(LocalPad(rig.counter), Open());  // c .. c+depth-1, predicted
  const Pad8 wrong = P(static_cast<u8>(0x90 + tag % 100), static_cast<u8>(tag), static_cast<u8>(tag >> 8));
  for (s32 f = c; f < c + depth; ++f)
    rig.net.Deliver(f, wrong);
  Reply r;
  ASSERT_TRUE(rig.TryFrame(LocalPad(rig.counter), Open(), &r));  // the call that finds frame c wrong
  ASSERT_EQ(r.flags, REPLY_FLAG_RESTORED);
  for (s32 i = 0; i < depth; ++i)
  {
    if (i == depth - 1)
      rig.net.now_us += cost_us;
    rig.Frame(P(0xFF, 0xFF), Open());
  }
  rig.net.Deliver(c + depth, wrong);  // the frame the rollback call re-ran was predicted with `wrong` already
}

TEST(Rev3Session, DecidePaceHardInTheFirst120FramesThenGentle)
{
  const PaceConfig c;
  // Hard: ahead by more than t1 (10 ms) loses the excess over hard_target (5 ms), at most hard_max (5 frames).
  EXPECT_STREQ(DecidePace(c, 30, 9000).action, "none");
  EXPECT_STREQ(DecidePace(c, 30, 10000).action, "none");
  EXPECT_STREQ(DecidePace(c, 30, 10001).action, "hard");
  EXPECT_EQ(DecidePace(c, 30, 10001).delay_us, 5001);
  EXPECT_EQ(DecidePace(c, 30, 40000).delay_us, 35000);
  EXPECT_EQ(DecidePace(c, 30, 1000000).delay_us, 5 * PACE_FRAME_US);
  EXPECT_STREQ(DecidePace(c, 30, -10001).action, "behind");
  EXPECT_EQ(DecidePace(c, 30, -1000000).delay_us, 0);  // behind: nothing here, the peer slows
  EXPECT_STREQ(DecidePace(c, 30, -10000).action, "none");
  // The 120th frame after the gate is still hard, the 121st is gentle.
  EXPECT_STREQ(DecidePace(c, 120, 40000).action, "hard");
  EXPECT_STREQ(DecidePace(c, 121, 40000).action, "gentle");
  // Gentle: nothing inside the dead band (8 ms), then proportional up to 2 % of the interval at a lead of 2 frames and beyond.
  EXPECT_STREQ(DecidePace(c, 121, 8000).action, "none");
  EXPECT_STREQ(DecidePace(c, 121, 8001).action, "gentle");
  EXPECT_GE(DecidePace(c, 121, 8001).delay_us, 2399);
  EXPECT_LE(DecidePace(c, 121, 8001).delay_us, 2401);
  EXPECT_EQ(DecidePace(c, 500, 16683).delay_us, 5004);  // half the window: half the maximum
  const s32 most = DecidePace(c, 500, 2 * PACE_FRAME_US).delay_us;
  EXPECT_EQ(most, 10009);
  EXPECT_EQ(DecidePace(c, 500, 50000).delay_us, most);
  EXPECT_EQ(DecidePace(c, 500, 1000000).delay_us, most);
  EXPECT_LE(most, c.interval_frames * PACE_FRAME_US * 2 / 100);  // never more than 2 % of an interval
  EXPECT_STREQ(DecidePace(c, 500, -8001).action, "behind");
  EXPECT_STREQ(DecidePace(c, 500, -8000).action, "none");
  // A hard correction is bounded by one hard_max however large the offset is.
  for (s32 off : {20000, 60000, 200000, 60000000})
    EXPECT_LE(DecidePace(c, 10, off).delay_us, c.hard_max_us);
}

TEST(Rev3Session, TimeSyncUsesTheSendTimeOfTheSameFrameAndAveragesTheMiddleThird)
{
  TimeSync t;
  s32 off = 0;
  EXPECT_FALSE(t.OffsetUs(&off));
  constexpr u64 RTT = 40000;
  // The peer sends every frame 20 ms later than we do; its pad arrives half an RTT after it was sent.
  for (s32 f = 0; f < 30; ++f)
  {
    const u64 sent = 100000 + static_cast<u64>(f) * 16683;
    t.OnLocalSend(f, sent);
    t.OnRemote(f, sent + 20000 + RTT / 2, RTT);
  }
  u32 n = 0;
  ASSERT_TRUE(t.OffsetUs(&off, &n));
  EXPECT_EQ(off, 20000);
  EXPECT_EQ(n, 30u);
  // The same frame arriving again (a re-send) or an older one is no sample.
  t.OnRemote(29, 900000, RTT);
  t.OnRemote(3, 900000, RTT);
  EXPECT_EQ(t.Samples(), 30u);
  t.Reset();
  EXPECT_EQ(t.Samples(), 0u);
  EXPECT_FALSE(t.OffsetUs(&off));
}

TEST(Rev3Session, TimeSyncDropsJitterSpikesAtBothEnds)
{
  TimeSync t;
  constexpr u64 RTT = 40000;
  // 10 samples at +5 ms, 10 spikes of +1 s (a late re-send) and 10 of -1 s: the middle third is the 5 ms ones.
  for (s32 f = 0; f < 30; ++f)
  {
    const u64 sent = 5000000 + static_cast<u64>(f) * 16683;
    const s64 delta = f % 3 == 0 ? 5000 : (f % 3 == 1 ? 1000000 : -1000000);
    t.OnLocalSend(f, sent);
    t.OnRemote(f, static_cast<u64>(static_cast<s64>(sent) + delta + static_cast<s64>(RTT / 2)), RTT);
  }
  s32 off = 0;
  ASSERT_TRUE(t.OffsetUs(&off));
  EXPECT_EQ(off, 5000);
}

TEST(Rev3Session, TimeSyncAfterAResetIgnoresRemoteFramesWeSentBeforeThePacingDelay)
{
  // Frames the peer sent before our correction arrive for a while after it (half an RTT in flight); our send time of THOSE frames is the old
  // timing, so they would describe the skew we just removed. After Reset() only remote frames above our newest sent one are sampled.
  TimeSync t;
  constexpr u64 RTT = 40000;
  for (s32 f = 0; f < 10; ++f)
  {
    t.OnLocalSend(f, 100000 + static_cast<u64>(f) * PACE_FRAME_US);
    t.OnRemote(f, 100000 + static_cast<u64>(f) * PACE_FRAME_US + 20000 + RTT / 2, RTT);
  }
  EXPECT_EQ(t.Samples(), 10u);
  t.Reset();  // we delayed ourselves; our newest sent frame is 9
  for (s32 f = 10; f < 14; ++f)  // our frames 10..13 go out after the correction
    t.OnLocalSend(f, 500000 + static_cast<u64>(f) * PACE_FRAME_US);
  t.OnRemote(5, 900000, RTT);   // older than the newest remote seen: ignored as before
  t.OnRemote(10, 900000, RTT);  // not older, and above our newest frame at the Reset: a sample
  EXPECT_EQ(t.Samples(), 1u);
  TimeSync u;
  for (s32 f = 0; f < 10; ++f)
    u.OnLocalSend(f, 100000 + static_cast<u64>(f) * PACE_FRAME_US);
  u.OnRemote(4, 400000, RTT);
  u.Reset();                    // newest sent frame: 9
  u.OnRemote(7, 500000, RTT);   // sent by the peer before our correction: skipped
  u.OnRemote(9, 500000, RTT);   // skipped (our frame 9 went out before the Reset)
  EXPECT_EQ(u.Samples(), 0u);
  u.OnLocalSend(10, 600000);
  u.OnRemote(10, 700000, RTT);  // a frame we sent after the Reset: counts
  EXPECT_EQ(u.Samples(), 1u);
}

TEST(Rev3Session, TimeSyncNeedsAnRttAndALocalSendAndFallsBackToSlippisFormulaForFramesNotSentYet)
{
  TimeSync t;
  s32 off = 0;
  t.OnRemote(0, 50000, 20000);  // we have not sent anything yet
  EXPECT_FALSE(t.OffsetUs(&off));
  t.OnLocalSend(0, 1000);
  t.OnRemote(1, 60000, 0);  // no RTT estimate: the arrival time would be wrong by RTT/2, so it is not a sample
  EXPECT_FALSE(t.OffsetUs(&off));
  // Frames 0..2 went out at 0, 1 and 2 frame times after 100 ms; the peer's frame 5 arrives at 200 ms (RTT 20 ms): we have not sent frame 5.
  TimeSync u;
  for (s32 f = 0; f <= 2; ++f)
    u.OnLocalSend(f, 100000 + static_cast<u64>(f) * PACE_FRAME_US);
  u.OnRemote(5, 200000, 20000);
  ASSERT_TRUE(u.OffsetUs(&off));
  // Slippi: (now - rtt/2) - newest_send + frame_time * (our newest - their frame) = 190000 - 133366 - 3 * 16683 = 6585.
  EXPECT_EQ(off, 190000 - (100000 + 2 * PACE_FRAME_US) - 3 * PACE_FRAME_US);
}

TEST(Rev3Session, AnAheadMachineDelaysItselfInSlicesAfterEveryThirtiethArmedFrameAndForgetsTheSamples)
{
  Rig rig(Role::Host, 0, 5);
  RecordingPacer pacer;
  rig.session->SetPaceActuator(&pacer);
  rig.net.has_offset = true;
  rig.net.offset_us = 40000;  // 2.4 frames ahead
  rig.net.rtt_us = 100000;
  rig.ReadyToArm(10);
  ArmedFrames(rig, 30);  // calls 10..39: no check yet (the first one is a full interval after arming)
  EXPECT_TRUE(pacer.delays.empty());
  EXPECT_EQ(rig.session->stats().pace_checks, 0u);
  EXPECT_EQ(rig.net.offset_resets, 1);  // arming forgot the lockstep samples
  ArmedFrames(rig, 1);  // call 40: the check; its first slice goes out with it
  EXPECT_EQ(rig.session->stats().pace_checks, 1u);
  ASSERT_EQ(pacer.delays.size(), 1u);
  EXPECT_EQ(pacer.delays[0], 4000u);
  EXPECT_TRUE(rig.sink.Has("PACE frame=40 since_gate=30 offset_us=40000 adv=2.40 rtt_ms=100.0 samples=30 action=hard sleep_us=35000"));
  ArmedFrames(rig, 8);  // the rest: 7 more slices of 4000 and a last one of 3000
  ASSERT_EQ(pacer.delays.size(), 9u);
  for (size_t i = 0; i + 1 < pacer.delays.size(); ++i)
    EXPECT_EQ(pacer.delays[i], 4000u);
  EXPECT_EQ(pacer.delays.back(), 3000u);
  EXPECT_EQ(pacer.total, 35000u);
  EXPECT_EQ(rig.net.offset_resets, 2);  // after the last slice
  ArmedFrames(rig, 20);                 // calls 49..68: next check is at 70
  EXPECT_EQ(pacer.delays.size(), 9u);
  EXPECT_EQ(rig.session->stats().pace_slowdowns, 1u);
  EXPECT_EQ(rig.session->stats().pace_sleep_us_total, 35000u);
}

TEST(Rev3Session, PacingTurnsGentleAfter120FramesAndNeverAsksForMoreThanASlicePerCall)
{
  Rig rig(Role::Host, 0, 5);
  RecordingPacer pacer;
  rig.session->SetPaceActuator(&pacer);
  rig.net.has_offset = true;
  rig.net.offset_us = 50000;
  rig.net.rtt_us = 100000;
  rig.ReadyToArm(10);
  ArmedFrames(rig, 160 - 10 + 12);  // calls 10..161: checks at 40, 70, 100, 130 (since_gate 120: still hard) and 160 (gentle)
  EXPECT_EQ(rig.session->stats().pace_checks, 5u);
  EXPECT_TRUE(rig.sink.Has("PACE frame=130 since_gate=120 offset_us=50000 adv=3.00 rtt_ms=100.0 samples=30 action=hard sleep_us=45000"));
  EXPECT_TRUE(rig.sink.Has("PACE frame=160 since_gate=150 offset_us=50000 adv=3.00 rtt_ms=100.0 samples=30 action=gentle sleep_us=10009"));
  EXPECT_EQ(pacer.total, 4u * 45000u + 10009u);
  for (u32 d : pacer.delays)
    EXPECT_LE(d, 4000u);
}

TEST(Rev3Session, AMachineThatIsBehindOrOnTimeIsNeverDelayed)
{
  for (s32 offset : {-40000, -9000, 0, 6000, 8000})
  {
    Rig rig(Role::Host, 0, 5);
    RecordingPacer pacer;
    rig.session->SetPaceActuator(&pacer);
    rig.net.has_offset = true;
    rig.net.offset_us = offset;
    rig.net.rtt_us = 100000;
    rig.ReadyToArm(10);
    ArmedFrames(rig, 200);
    EXPECT_GE(rig.session->stats().pace_checks, 5u) << offset;
    EXPECT_TRUE(pacer.delays.empty()) << offset;
    EXPECT_EQ(rig.net.offset_resets, 1) << offset;  // only the arming one: no correction, so the samples are kept
    EXPECT_TRUE(rig.sink.Has(offset < -10000 ? "action=behind sleep_us=0" : "action=none sleep_us=0")) << offset;
  }
}

TEST(Rev3Session, PacingOffMeasuresAndLogsWhatItWouldHaveDone)
{
  Rig rig(Role::Host, 0, 5);
  rig.cfg.pace.enabled = false;
  rig.Rebuild();
  RecordingPacer pacer;
  rig.session->SetPaceActuator(&pacer);
  rig.net.has_offset = true;
  rig.net.offset_us = 40000;
  rig.net.rtt_us = 100000;
  rig.ReadyToArm(10);
  ArmedFrames(rig, 100);
  EXPECT_TRUE(pacer.delays.empty());
  EXPECT_EQ(rig.session->stats().pace_slowdowns, 0u);
  EXPECT_EQ(rig.session->stats().pace_sleep_us_total, 0u);
  EXPECT_GE(rig.session->stats().pace_checks, 3u);
  EXPECT_TRUE(rig.sink.Has("action=off sleep_us=0 would=hard:35000"));
  EXPECT_EQ(rig.net.offset_resets, 1);
  rig.session->Finish();
  EXPECT_TRUE(rig.sink.Has("pace=off pace_checks="));
}

TEST(Rev3Session, NoCheckWithoutAnRttOrEnoughSamplesAndNothingInLockstep)
{
  Rig rig(Role::Host, 0, 5);
  RecordingPacer pacer;
  rig.session->SetPaceActuator(&pacer);
  rig.net.has_offset = true;
  rig.net.offset_us = 40000;
  rig.Handshake();
  rig.Lockstep(100);  // LOCKSTEP: pacing never runs, whatever the transport says
  EXPECT_TRUE(rig.session->mode() == Mode::LOCKSTEP);
  rig.net.rtt_us = 100000;
  rig.Lockstep(100);
  EXPECT_EQ(rig.session->stats().pace_checks, 0u);
  EXPECT_TRUE(pacer.delays.empty());
  EXPECT_EQ(rig.sink.Count("PACE "), 0u);

  rig.PeerPropose(rig.counter + 1);  // arm at the next frame
  rig.net.Deliver(rig.counter, RemotePadScript(rig.counter));
  rig.Frame(LocalPad(rig.counter), Open());
  rig.PeerStartCheck();
  rig.net.rtt_us = 0;  // armed, but no RTT estimate: no decision (the offset subtracts RTT/2)
  ArmedFrames(rig, 80);
  EXPECT_EQ(rig.session->stats().pace_checks, 0u);
  rig.net.rtt_us = 100000;
  rig.net.offset_samples = 14;  // too few samples
  ArmedFrames(rig, 40);
  EXPECT_EQ(rig.session->stats().pace_checks, 0u);
  rig.net.offset_samples = 15;
  ArmedFrames(rig, 1);  // due since long: decided on the next call
  EXPECT_EQ(rig.session->stats().pace_checks, 1u);
  EXPECT_EQ(pacer.delays.size(), 1u);
}

TEST(Rev3Session, WithoutAnActuatorPacingOnlyMeasuresAndSaysSoOnce)
{
  Rig rig(Role::Host, 0, 5);  // no actuator set
  rig.net.has_offset = true;
  rig.net.offset_us = 40000;
  rig.net.rtt_us = 100000;
  rig.ReadyToArm(10);
  ArmedFrames(rig, 100);
  EXPECT_EQ(rig.session->stats().pace_slowdowns, 0u);
  EXPECT_TRUE(rig.sink.Has("NOTE pacing has no actuator"));
  EXPECT_EQ(rig.sink.Count("NOTE pacing has no actuator"), 1u);
}

TEST(Rev3Session, PaceLogLinesAreBoundedAndSummaryCarriesThePaceCounters)
{
  Rig rig(Role::Host, 0, 5);
  rig.net.has_offset = true;
  rig.net.offset_us = 1000;
  rig.net.rtt_us = 100000;
  RecordingPacer pacer;
  rig.session->SetPaceActuator(&pacer);
  rig.ReadyToArm(10);
  ArmedFrames(rig, 6000);  // ~199 checks
  const SessionStats& st = rig.session->stats();
  EXPECT_GT(st.pace_checks, 150u);
  const size_t lines = rig.sink.Count("PACE frame=");
  EXPECT_LE(lines, PACE_LOG_FULL_LINES + st.pace_checks / PACE_LOG_EVERY + 1);
  EXPECT_EQ(lines + st.pace_lines_left_out, st.pace_checks);
  rig.session->Finish();
  for (const char* key : {"pace=on", "pace_checks=", "pace_slowdowns=0", "pace_sleep_ms_total=0.0", "adv_mean=0.06", "adv_max=0.06", "adv_min=0.06",
                          "stall_run_max=0", "resim_n=0", "resim_ms_p50=0.00", "resim_ms_p95=0.00", "maxr_eff=5", "maxr_changes=0"})
    EXPECT_TRUE(rig.sink.Has(key)) << key;
}

TEST(Rev3Session, ARunOf420StalledCallsThatTakeAWholeFrameEachEndsTheSession)
{
  Rig rig(Role::Host, 0, 2);
  rig.ReadyToArm(10);
  const Pad8 same = RemotePadScript(9);
  rig.Frame(LocalPad(10), Open());  // predicted 1 and 2 frames: allowed
  rig.Frame(LocalPad(11), Open());
  s32 next_pad = 10;
  u32 stalls = 0;
  while (rig.session->mode() == Mode::ARMED && stalls < 600)
  {
    if (rig.TryFrame(LocalPad(rig.counter), Open()))
      continue;
    ++stalls;
    rig.net.now_us += 20000;  // a 20 ms wait: more than a frame time
    rig.net.Deliver(next_pad++, same);
    EXPECT_TRUE(rig.session->BlockSatisfied());
  }
  EXPECT_TRUE(rig.session->mode() == Mode::OFFLINE);
  EXPECT_EQ(stalls, 420u);
  EXPECT_TRUE(rig.sink.Has("stall: 420 consecutive stalled frames, 8.4 s of waiting for the peer"));
  EXPECT_EQ(rig.net.CountOut(MsgType::Bye), 1u);
}

TEST(Rev3Session, ShortStallsAndInterruptedRunsDoNotEndTheSession)
{
  Rig rig(Role::Host, 0, 2);
  rig.ReadyToArm(10);
  const Pad8 same = RemotePadScript(9);
  rig.Frame(LocalPad(10), Open());
  rig.Frame(LocalPad(11), Open());
  s32 next_pad = 10;
  const auto stall_once = [&](u64 wait_us) {
    for (int guard = 0; rig.TryFrame(LocalPad(rig.counter), Open()); ++guard)  // calls that do not stall (pads ahead) reset the run
      ASSERT_LT(guard, 100) << "the session never stalled (mode " << ModeName(rig.session->mode()) << ")";
    rig.net.now_us += wait_us;
    rig.net.Deliver(next_pad++, same);
    ASSERT_TRUE(rig.session->BlockSatisfied());
    ASSERT_TRUE(rig.TryFrame(LocalPad(rig.counter), Open()));
  };
  // 500 stalls of 5 ms: a slow link, but 2.5 s is not 7 s of waiting.
  for (int i = 0; i < 500; ++i)
    stall_once(5000);
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_EQ(rig.session->stats().stalls, 500u);
  // 300 stalls of 20 ms, calls that do not stall (the peer's pads are ahead), 300 more: never 420 in a row.
  for (int i = 0; i < 6; ++i)
    rig.net.Deliver(next_pad++, same);
  for (int i = 0; i < 300; ++i)
    stall_once(20000);
  for (int i = 0; i < 6; ++i)
    rig.net.Deliver(next_pad++, same);
  for (int i = 0; i < 300; ++i)
    stall_once(20000);
  EXPECT_TRUE(rig.session->mode() == Mode::ARMED);
  EXPECT_FALSE(rig.sink.Has("consecutive stalled frames"));
  EXPECT_GE(rig.session->stats().stall_run_max, 300u);
}

TEST(Rev3Session, ClosingTheWindowWhileArmedSendsAClearByeAndThePeersLogNamesTheReason)
{
  Rig a(Role::Host, 0, 5);
  a.ReadyToArm(10);
  ArmedFrames(a, 5);
  EXPECT_TRUE(a.session->mode() == Mode::ARMED);
  a.session->Abort("the emulator was closed");  // what the glue calls when the window closes while a wait (or a frame) is running
  EXPECT_TRUE(a.session->mode() == Mode::OFFLINE);
  ASSERT_EQ(a.net.CountOut(MsgType::Bye), 1u);
  const Message* bye = a.net.LastOut(MsgType::Bye);
  ASSERT_TRUE(bye != nullptr);
  EXPECT_EQ(std::string(bye->bytes.begin(), bye->bytes.end()), "the emulator was closed");
  EXPECT_TRUE(a.sink.Has("LEAVE frame="));
  EXPECT_TRUE(a.sink.Has("reason=the emulator was closed"));
  EXPECT_TRUE(a.sink.Has("SUMMARY "));  // the SUMMARY is written on the way out

  // The peer: it gets the BYE while it waits for a pad that will never come, logs why, leaves, and the game goes on offline.
  Rig b(Role::Guest, 0, 5);
  b.ReadyToArm(10);
  ArmedFrames(b, 3);
  b.net.inbox.push_back(*bye);
  Reply r;
  EXPECT_TRUE(b.session->Step(b.Cmd(b.counter, LocalPad(b.counter)), Open(), &r));
  EXPECT_TRUE(b.session->mode() == Mode::OFFLINE);
  EXPECT_TRUE(b.sink.Has("BYE from the peer: the emulator was closed"));
  EXPECT_TRUE(b.sink.Has("reason=the peer left (the emulator was closed)"));
  EXPECT_EQ(b.net.CountOut(MsgType::Bye), 0u);  // no echo
  EXPECT_TRUE(b.session->Step(b.Cmd(b.counter + 1, LocalPad(b.counter + 1)), Open(), &r));  // offline: pass-through, never blocks
}

TEST(Rev3Session, ResimulationCostIsMeasuredPerRollbackAndSummarisedAsPercentiles)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  for (int k = 1; k <= 20; ++k)
    RollbackOf(rig, 1, static_cast<u64>(k) * 1000, k);
  EXPECT_EQ(rig.session->stats().rollbacks, 20u);
  EXPECT_EQ(rig.session->stats().resim_costs, 20u);
  u32 v = 0;
  ASSERT_TRUE(rig.session->ResimCostPercentileUs(50, &v));
  EXPECT_EQ(v, 10000u);
  ASSERT_TRUE(rig.session->ResimCostPercentileUs(95, &v));
  EXPECT_EQ(v, 19000u);
  ASSERT_TRUE(rig.session->ResimCostPercentileUs(100, &v));
  EXPECT_EQ(v, 20000u);
  rig.session->Finish();
  EXPECT_TRUE(rig.sink.Has("resim_n=20 resim_skipped=0 resim_ms_p50=10.00 resim_ms_p95=19.00 resim_ms_max=20.00"));
}

TEST(Rev3Session, WithoutTheSwitchMaxrNeverAdaptsHoweverSlowTheResimulationIs)
{
  Rig rig(Role::Host, 0, 5);
  rig.ReadyToArm(10);
  for (int k = 1; k <= 30; ++k)
    RollbackOf(rig, 2, 60000, k);
  EXPECT_EQ(rig.session->maxr_eff(), 5);
  EXPECT_EQ(rig.session->stats().maxr_changes, 0u);
  EXPECT_FALSE(rig.sink.Has("MAXR eff"));
}

TEST(Rev3Session, AdaptiveMaxrDropsToTheLargestKWhoseP95CostFitsTheBudgetAndOnlyChangesTheStallRule)
{
  Rig rig(Role::Host, 0, 5);
  rig.cfg.maxr_adapt = true;
  rig.cfg.maxr_budget_us = 14000;
  rig.Rebuild();
  rig.ReadyToArm(10);
  // Depth 2 rollbacks that cost 20 ms: 10 ms per frame, so one frame fits the 14 ms budget and two do not. The first change comes with the
  // 8th measured rollback.
  for (int k = 1; k <= 7; ++k)
    RollbackOf(rig, 2, 20000, k);
  EXPECT_EQ(rig.session->maxr_eff(), 5);
  RollbackOf(rig, 2, 20000, 8);
  EXPECT_EQ(rig.session->maxr_eff(), 1);
  EXPECT_EQ(rig.session->stats().maxr_changes, 1u);
  EXPECT_EQ(rig.session->stats().maxr_eff_min, 1);
  EXPECT_TRUE(rig.sink.Has("MAXR eff 5 -> 1 (p95 resimulation cost 10.00 ms per frame, budget 14.0 ms, configured 5, 8 rollbacks measured)"));
  // The stall rule now allows one predicted frame; the configured MAXR (which the gate close and the HELLO use) is untouched: only the stall rule moved.
  EXPECT_TRUE(rig.TryFrame(LocalPad(rig.counter), Open()));   // predicted 1
  EXPECT_FALSE(rig.TryFrame(LocalPad(rig.counter), Open()));  // predicted 2 > 1
  EXPECT_TRUE(rig.sink.Has("maxr=1"));
}

TEST(Rev3Session, AdaptiveMaxrComesBackOneStepAtATimeWhenResimulationGetsCheapAgainAndNeverPassesTheConfiguredValue)
{
  Rig rig(Role::Host, 0, 5);
  rig.cfg.maxr_adapt = true;
  rig.Rebuild();
  rig.ReadyToArm(10);
  for (int k = 1; k <= 8; ++k)
    RollbackOf(rig, 1, 40000, k);  // 40 ms per frame: K = 1 (never below 1)
  ASSERT_EQ(rig.session->maxr_eff(), 1);
  int last = 1;
  int steps = 0;
  for (int k = 9; k < 400 && rig.session->maxr_eff() < 5; ++k)
  {
    RollbackOf(rig, 1, 2000, k);  // 2 ms per frame: K = 7 -> capped at the configured 5
    const int now = rig.session->maxr_eff();
    EXPECT_LE(now - last, 1);
    EXPECT_GE(now, last);
    steps += now - last;
    last = now;
  }
  EXPECT_EQ(rig.session->maxr_eff(), 5);
  EXPECT_EQ(steps, 4);
  for (int k = 500; k < 540; ++k)
    RollbackOf(rig, 1, 1000, k);
  EXPECT_EQ(rig.session->maxr_eff(), 5);
  EXPECT_EQ(rig.session->stats().maxr_changes, 5u);
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
  // M5: the same TimeSync as the real transport (PadLink), and an ack based RTT like PadLink's (first send of the acked frame, EWMA 7/8).
  bool TimeOffsetUs(s32* out, u32* samples) const override { return m_time.OffsetUs(out, samples); }
  void ResetTimeOffsets() override { m_time.Reset(); }
  u64 RttUs() const override { return m_rtt_us; }
  std::map<s32, u64> send_log;  // exploration: when each local frame went out

  void Tick();
  void OnPadPacket(const PadPacket& packet);
  void OnAck(s32 frame);
  void OnReliable(const Message& m) { m_inbox.push_back(m); }

private:
  void Transmit(bool timer_resend = false);

  SimNet* net;
  int me;
  std::deque<std::pair<s32, Pad8>> m_unacked;  // ascending
  struct SendInfo
  {
    u64 first_us = 0;
    u32 sent = 0;
    bool timer_resent = false;  // re-sent by the timer: the ack cannot be matched to one send
  };
  std::map<s32, SendInfo> m_send_info;  // unacked frames
  TimeSync m_time;
  u64 m_rtt_us = 0;
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
void SimEndpoint::OnAck(s32 frame)
{
  const auto info = m_send_info.find(frame);
  if (info != m_send_info.end() && !info->second.timer_resent && net->now >= info->second.first_us)
  {
    const u64 sample = net->now - info->second.first_us;
    m_rtt_us = m_rtt_us == 0 ? sample : (7 * m_rtt_us + sample) / 8;
  }
  while (!m_unacked.empty() && m_unacked.front().first <= frame)
  {
    m_send_info.erase(m_unacked.front().first);
    m_unacked.pop_front();
  }
}
void SimEndpoint::SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum)
{
  m_time.OnLocalSend(frame, net->now);
  send_log[frame] = net->now;
  m_unacked.emplace_back(frame, pad);
  while (m_unacked.size() > 128)
    m_unacked.pop_front();
  m_chk_frame = checksum_frame;
  m_chk = checksum;
  Transmit();
}
void SimEndpoint::Transmit(bool timer_resend)
{
  PadPacket p;
  for (size_t i = m_unacked.size(); i > 0; --i)
  {
    p.pads.push_back(m_unacked[i - 1]);
    SendInfo& info = m_send_info[m_unacked[i - 1].first];
    if (info.sent == 0)
      info.first_us = net->now;
    ++info.sent;
    if (timer_resend)
      info.timer_resent = true;
  }
  p.chk_frame = m_chk_frame;
  p.chk = m_chk;
  m_last_send_us = net->now;
  net->PostPad(me, p);
}
void SimEndpoint::Tick()
{
  if (!m_unacked.empty() && net->now >= m_last_send_us + net->params.resend_us)
    Transmit(true);
}
void SimEndpoint::OnPadPacket(const PadPacket& packet)
{
  if (packet.pads.empty())
    return;
  m_time.OnRemote(packet.pads.front().first, net->now, m_rtt_us);
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
  s32 guest_change_pct = -1;           // >= 0: the guest's pad changes at this rate instead (0: it never moves, so the HOST never rolls back: D's loopback)
  bool busy_hardware = false;          // each machine independently reports "hardware busy" on ~10% of its calls, and in bursts (HOLD)
  bool busy_long = false;              // busy_hardware: the bursts last 3..42 calls instead of 3..8, so a rollback waits longer than the ring is deep (io_overflow)
  int io_extra = 0;                    // Config::io_extra of both machines; their FakeStores hold maxr + 2 + io_extra slots
  u32 lost_mod = 0;                    // != 0: a fighter is "at health 0" when (state word 0 >> 5) % lost_mod == 0, a function of the LIVE state:
                                       // a mispredicted timeline can show it where the real one does not (0 = only the frame windows below)
  u32 relayout_pct = 0;                // chance per armed call that THIS machine wants a new snapshot set (local: the peer never knows)
  u64 period_us[2] = {16667, 16700};   // each machine's frame period (clock drift)
  u64 start_us[2] = {0, 0};
  s32 bug_frame = -1;                  // machine 1's game has a rules difference at this frame (a desync)
  // M5. Dolphin's throttle: a machine that is late (it waited for the peer) catches up by running the next frames without sleeping, until it is
  // back on its schedule or MAIN_MAX_FALLBACK behind it. Without this the simulated stalls shift the machine's phase for free and the skew that
  // pacing is about cannot exist (the older tests keep the simple model: next call = this call + period).
  bool catch_up = false;
  u64 compute_us[2] = {2000, 2000};    // what one frame of work costs on each host when nothing throttles it (catch_up only)
  u64 resim_compute_us[2] = {2000, 2000};  // what one hidden resimulated frame costs (catch_up only)
  bool resim_restarts_clock = false;   // catch_up only. false: Dolphin as built since M5 fix 2 (CoreTimingManager::BeginResim / EndResim): the re-run frames
                                       // do not count as schedule time, so a machine that rolled back pays for the re-run out of the next frames and stays
                                       // on its schedule. true: the M4/M5 behaviour (every Throttle during the resimulation resets the clock): the re-run
                                       // time is lost for good, a machine that rolls back often drifts later (~2 % in D's first loopback)
  u64 max_fallback_us = 100000;        // MAIN_MAX_FALLBACK: a lag beyond this is forgiven (catch_up only)
  bool pace = false;                   // pacing on (Config::pace.enabled) with a simulated actuator; false: the sessions only measure
  s32 ready_extra[2] = {0, 0};         // machine i's gate turns READY this many frames after open_at: its local inputs (hardware quiet streak, learned
                                       // set, write tracking) differ from the peer's by some frames (D's soak run_2 / run_6, Oct 16). A huge value = never ready.
  u64 gate_skew_us[2] = {0, 0};        // catch_up only: when machine i first turns ARMED its throttle schedule restarts at "now + this", i.e. it is that
                                       // far BEHIND a machine that restarts on time. Two greedy machines (each runs as early as the peer's pads allow)
                                       // come out of the lockstep with some skew like this; the one in front then sits at the stall limit, which is what
                                       // D's loopback showed (host: 1902 stalls, guest: 5)
};

struct SimMachineResult
{
  std::array<u32, 4> state{};
  SessionStats stats;
  Mode mode = Mode::OFFLINE;
  std::map<s32, u32> finals;  // frame -> checksum the session called final
  u32 restores_while_busy = 0;  // RESTORED replies given in a call whose gate said "hardware busy": must stay 0 (A RESTORE NEVER RUNS UNDER A READ)
  u32 phantom_lost = 0;       // normal ARMED calls whose live state said "lost" where the true timeline does not (a live-following gate would close here)
  std::string log;
  size_t chk_lines = 0;
  s32 counter = 0;
  bool left_session = false;
  u64 slept_us = 0;           // pacing delay actually handed out by the actuator
  u32 delay_calls = 0;
  u32 bad_delay_calls = 0;    // pacing delays asked in LOCKSTEP / DRAIN / a resimulation (a bug if not 0)
  u64 largest_delay_us = 0;
  u64 end_us = 0;
  u64 max_block_us = 0;         // the longest time one call waited (a deadlock shows here as the 10 s timeout)
  std::map<s32, u64> send_log;  // when each local frame went out (the true timing, to judge the offset estimate)
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

// The simulated PaceActuator: "sleep, then restart the throttle clock". The machine applies it when the call completes.
class SimPacer final : public PaceActuator
{
public:
  void Delay(u32 microseconds) override
  {
    pending_us += microseconds;
    ++calls;
    largest_us = std::max<u64>(largest_us, microseconds);
    if (!armed_normal_call)
      ++bad_calls;
  }
  bool armed_normal_call = false;  // set by the machine before each Step: the call is ARMED and not a resimulation (the only place pacing may delay)
  u32 bad_calls = 0;               // delays requested anywhere else (must stay 0)
  u64 pending_us = 0;
  u64 total_us = 0;
  u32 calls = 0;
  u64 largest_us = 0;
};

struct SimMachine
{
  SimMachine(int idx_, const SimParams& p, SimNet* net, const std::vector<Pad8>& script_)
      : idx(idx_), store(&game, static_cast<size_t>(p.maxr + p.io_extra) + 2), script(script_), rng(p.seed * 7 + static_cast<u64>(idx_))
  {
    if (idx == 1)
      game.bug_frame = p.bug_frame;
    Config cfg;
    cfg.pace.enabled = p.pace;
    cfg.role = idx == 0 ? Role::Host : Role::Guest;
    cfg.delay = idx == 0 ? p.delay : (p.delay + 1) % 5;  // the guest asks for something else; the host's value must win
    cfg.max_rollback = p.maxr;
    cfg.io_extra = p.io_extra;
    cfg.hello.game_id = "RNEEDA";
    cfg.hello.items = {{"cpu_core", "JIT64"}};
    session = std::make_unique<RollbackSession>(cfg, net->ep[idx], store, sink, nullptr);
    session->SetFinalObserver([this](s32 f, u32 c) { finals[f] = c; });
    if (p.pace)
      session->SetPaceActuator(&pacer);
    ep = &net->ep[idx];
    next_at = p.start_us[idx];
    target_us = p.start_us[idx];
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
  u64 max_block_us = 0;
  u32 phantom_lost = 0;
  u32 restores_while_busy = 0;
  SimPacer pacer;
  u64 target_us = 0;     // catch_up: when the throttle wants the next call
  bool skewed = false;
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
      if (srng.Below(100) < (i == 1 && p.guest_change_pct >= 0 ? static_cast<u32>(p.guest_change_pct) : p.change_pct))
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
      gate.open = GateAt(p, m.counter) && !gate.health_zero && m.counter >= p.open_at + p.ready_extra[m.idx];
      // The disc is quiet for the last frames of a run: a rollback that waits for a read has to be able to run before the run's end (a real game does
      // not stop in the middle of a read).
      const bool disc_may_be_busy = p.busy_hardware && m.counter < p.frames - 10;
      gate.hardware_quiet = !(disc_may_be_busy && (m.busy_left > 0 || m.rng.Below(10) == 0));
      gate.hardware_hold = disc_may_be_busy && m.busy_left > 0 && m.busy_streak >= 3;  // GateTracker: busy for 3 calls in a row
      gate.layout_pending = m.layout_pending && m.session->mode() == Mode::ARMED;
      gate.why = "sim";
      const bool armed_call = m.session->mode() == Mode::ARMED && !m.session->Resimulating();
      Reply r;
      m.pacer.armed_normal_call = armed_call;
      const bool resim_call = m.session->Resimulating();  // this call re-runs a frame after a RESTORED reply
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
      if (m.blocking)
        m.max_block_us = std::max(m.max_block_us, net.now - m.block_start);
      m.blocking = false;
      if ((r.flags & REPLY_FLAG_RESTORED) != 0 && !gate.hardware_quiet)
        ++m.restores_while_busy;
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
            m.busy_left = 3 + static_cast<u32>(m.rng.Below(p.busy_long ? 40 : 6));
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
      const u64 shift = m.pacer.pending_us;  // a pacing delay of this call: sleep, then the throttle clock restarts
      m.pacer.total_us += shift;
      m.pacer.pending_us = 0;
      if (p.catch_up)
      {
        if (!m.skewed && m.session->mode() == Mode::ARMED)
        {
          m.skewed = true;
          m.target_us = net.now + p.gate_skew_us[m.idx];
        }
        if (m.session->Resimulating())
        {
          // Hidden resimulation (v1): the throttle is off, so the frames run back to back. Either every Throttle call restarts the clock (legacy)
          // or the schedule is left alone and put back at the end (BeginResim / EndResim): the target does not move here.
          if (p.resim_restarts_clock)
            m.target_us = net.now;
          m.next_at = net.now + p.resim_compute_us[m.idx];
        }
        else
        {
          if (shift != 0 || (resim_call && p.resim_restarts_clock))
            m.target_us = net.now + shift;  // a pacing delay (sleep, then the throttle clock restarts) or, legacy, the end of a resimulation
          m.target_us += m.period;
          if (m.target_us + p.max_fallback_us < net.now)
            m.target_us = net.now - p.max_fallback_us;  // lagging by more than MAIN_MAX_FALLBACK: the lag is forgiven
          m.next_at = std::max(m.target_us, net.now + p.compute_us[m.idx]);
        }
      }
      else
        m.next_at = net.now + shift + m.period;
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
    r.restores_while_busy = m.restores_while_busy;
    r.log = m.sink.Dump();
    r.chk_lines = m.sink.Count("CHK frame=");
    r.counter = m.counter;
    r.left_session = m.sink.Has("LEAVE");
    r.send_log = net.ep[i].send_log;
    r.max_block_us = m.max_block_us;
    r.slept_us = m.pacer.total_us;
    r.delay_calls = m.pacer.calls;
    r.bad_delay_calls = m.pacer.bad_calls;
    r.largest_delay_us = m.pacer.largest_us;
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
  // (Drawn last so every older parameter of a seed is unchanged.) The two gates become ready at different frames in most runs.
  p.ready_extra[0] = r.Below(3) == 0 ? 0 : static_cast<s32>(r.Below(30));
  p.ready_extra[1] = r.Below(3) == 0 ? 0 : static_cast<s32>(r.Below(30));
  // (Drawn after those, again so no older parameter changes.) The slots a rollback may wait in and how long a disc read lasts.
  p.io_extra = r.Below(3) == 0 ? 0 : static_cast<int>(r.Below(13));
  p.busy_long = r.Below(4) == 0;
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
    // A restore never runs under a disc read; a rollback that has to wait for one may go deeper than MAXR: up to the ring (MAXR + 2 + io_extra slots)
    // while it captures, up to the session's records (RING - 4) once frames are played without a capture or in a drain.
    const u32 depth_limit = p.busy_hardware ? 59u : static_cast<u32>(p.maxr);  // (a DRAIN that waits captures nothing, so it has no ring limit either)
    if (m.restores_while_busy != 0 || m.stats.forced_hw != 0)
      return "machine " + std::to_string(i) + " restored under a disc read " + std::to_string(m.restores_while_busy) + " times, forced_hw " +
             std::to_string(m.stats.forced_hw);
    if (m.stats.max_depth > depth_limit)
      return "machine " + std::to_string(i) + " rolled back " + std::to_string(m.stats.max_depth) + " deep, MAXR " + std::to_string(p.maxr) +
             " io_extra " + std::to_string(p.io_extra);
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

// The frames of the "GATE open frame=N" lines of a log, in order.
std::vector<s32> OpenFrames(const std::string& log)
{
  std::vector<s32> out;
  size_t pos = 0;
  while ((pos = log.find("GATE open frame=", pos)) != std::string::npos)
  {
    pos += std::strlen("GATE open frame=");
    out.push_back(std::atoi(log.c_str() + pos));
  }
  return out;
}

// D's soak, Oct 16 (run_2 and run_6 of 20261016_212543): at the start of round 2 one machine's gate was ready and the other's was not yet (the
// hardware quiet streak, the learned set and write tracking are local, so they differ by a few frames). The ready one blocked for a START_CHECK
// the other never sent, the other blocked for the first one's pads: 10 s, then both left. With the arm agreed over the wire a gate that is ready
// proposes and goes on playing; the other accepts when it gets ready or proposes itself.
TEST(Rev3Session, OneGateReadyFramesBeforeTheOtherNeverDeadlocksAndBothArmAtTheSameFrame)
{
  u32 armed_runs = 0;
  for (int lead_machine = 0; lead_machine < 2; ++lead_machine)
    for (s32 n : {1, 2, 3, 5, 8, 13, 21, 34, 55, 89})
    {
      SimParams p;
      p.seed = static_cast<u64>(100 + n);
      p.delay = 2;
      p.maxr = 5;
      p.net.delay_us = 70000;
      p.net.jitter_us = 30000;
      p.net.loss_pct = 5;
      p.frames = 400;
      p.open_at = 30;
      p.close_at = 380;
      p.ready_extra[lead_machine] = 0;
      p.ready_extra[1 - lead_machine] = n;  // the other gate is ready n frames later
      SimResult res;
      ASSERT_TRUE(RunSim(p, &res)) << "n " << n << ": the scheduler gave up (deadlock)";
      const std::string what = "lead " + std::to_string(lead_machine) + " n " + std::to_string(n);
      EXPECT_EQ(CheckHealthy(p, res), "") << what;
      const std::vector<s32> a = OpenFrames(res.m[0].log);
      const std::vector<s32> b = OpenFrames(res.m[1].log);
      ASSERT_EQ(a.size(), 1u) << what << "\n" << res.m[0].log;
      EXPECT_EQ(a, b) << what;  // the same arm frame on both machines
      EXPECT_GE(a[0], p.open_at + n) << what;
      for (const SimMachineResult& m : res.m)
      {
        EXPECT_LT(m.max_block_us, 1000000u) << what << ": a wait of a second or more (the deadlock waited 10 s)";
        EXPECT_FALSE(m.left_session) << what;
        EXPECT_EQ(m.stats.checks_bad, 0u) << what;
        EXPECT_NE(m.log.find("CHK start frame="), std::string::npos) << what << ": the START_CHECK was compared";
      }
      ++armed_runs;
    }
  EXPECT_EQ(armed_runs, 20u);
}

TEST(Rev3Session, ABothSidesProposalRaceIsSettledByTheHost)
{
  // Both gates are ready at the same frame: both propose; the host's proposal wins, the guest's is dropped. One arm, the same frame, no wait.
  for (u64 seed = 1; seed <= 30; ++seed)
  {
    SimParams p;
    p.seed = seed;
    p.delay = static_cast<int>(seed % 4);
    p.maxr = 5;
    p.net.delay_us = 20000 + (seed % 5) * 20000;
    p.net.jitter_us = 15000;
    p.net.loss_pct = 3;
    p.frames = 300;
    p.open_at = 30;
    p.close_at = 280;
    SimResult res;
    ASSERT_TRUE(RunSim(p, &res)) << "seed " << seed;
    EXPECT_EQ(CheckHealthy(p, res), "") << "seed " << seed;
    const std::vector<s32> a = OpenFrames(res.m[0].log);
    ASSERT_EQ(a.size(), 1u) << "seed " << seed;
    EXPECT_EQ(a, OpenFrames(res.m[1].log)) << "seed " << seed;
    EXPECT_EQ(res.m[1].stats.arm_accepted, 1u) << "seed " << seed << ": the guest accepted the host's proposal\n" << res.m[1].log;
    EXPECT_EQ(res.m[0].stats.arm_accepted, 0u) << "seed " << seed;
    EXPECT_EQ(res.m[0].stats.arm_proposed, 1u) << "seed " << seed;
  }
}

TEST(Rev3Session, WhenTheOtherGateNeverBecomesReadyBothStayInLockstepAndThePadsKeepFlowing)
{
  for (int never = 0; never < 2; ++never)
  {
    SimParams p;
    p.seed = 31;
    p.delay = 2;
    p.maxr = 5;
    p.net.delay_us = 60000;
    p.net.jitter_us = 20000;
    p.net.loss_pct = 4;
    p.frames = 600;
    p.open_at = 30;
    p.close_at = 580;
    p.ready_extra[never] = 1000000;  // this machine's gate is never ready
    SimResult res;
    ASSERT_TRUE(RunSim(p, &res)) << never;
    EXPECT_EQ(CheckHealthy(p, res), "") << never;
    // The same run with nobody ready at all is the baseline of how long the lockstep takes.
    SimParams q = p;
    q.ready_extra[0] = q.ready_extra[1] = 1000000;
    SimResult base;
    ASSERT_TRUE(RunSim(q, &base));
    for (const SimMachineResult& m : res.m)
    {
      EXPECT_EQ(m.stats.gate_opens, 0u) << never;
      EXPECT_EQ(m.stats.predicted_frames, 0u) << never;  // lockstep all the way
      EXPECT_EQ(m.counter, p.frames) << never;
      EXPECT_FALSE(m.left_session) << never;
      EXPECT_LT(m.max_block_us, 1000000u) << never;
    }
    const SimMachineResult& ready = res.m[1 - never];
    EXPECT_GE(ready.stats.arm_proposed, 3u) << never;       // it keeps trying
    EXPECT_LE(ready.stats.arm_proposed, 20u) << never;      // ... with a back-off, not every frame
    EXPECT_EQ(ready.stats.arm_proposed, ready.stats.arm_rejected) << never;
    EXPECT_EQ(res.m[never].stats.arm_refused, ready.stats.arm_proposed) << never;
    EXPECT_TRUE(res.m[never].log.find("reason=not ready") != std::string::npos) << res.m[never].log;
    EXPECT_TRUE(ready.log.find("rejected by the peer: not ready") != std::string::npos) << ready.log;
    // The proposals and rejections cost the run nothing worth measuring.
    EXPECT_LE(res.end_us, base.end_us + 100000) << never << ": " << res.end_us << " vs " << base.end_us;
  }
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
  u64 deep_rollbacks = 0, overflows = 0, overflow_calls = 0, waited_rollbacks = 0, busy_restores = 0, beyond_maxr = 0;
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
                             << p.net.jitter_us << "/" << p.net.loss_pct << "%): " << problem.substr(0, 600);
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
      deep_rollbacks += m.stats.deep_rollbacks;
      overflows += m.stats.io_overflow;
      overflow_calls += m.stats.io_overflow_calls;
      waited_rollbacks += m.stats.io_postponed_rollbacks;
      busy_restores += m.restores_while_busy;
      if (m.stats.max_depth > static_cast<u32>(p.maxr))
        ++beyond_maxr;
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
  // A restore never runs under a disc read (the sim's busy windows include long ones): none did, none was forced, and the rollbacks that had to
  // wait for one went deeper than MAXR, some of them past the ring (io_overflow: real pads, no capture) and still agreed with the reference.
  EXPECT_EQ(forced, 0u);
  EXPECT_EQ(busy_restores, 0u);
  EXPECT_GT(waited_rollbacks, 100u);
  EXPECT_GT(deep_rollbacks, 20u);
  EXPECT_GT(beyond_maxr, 5u);
  EXPECT_GT(overflows, 3u);
  EXPECT_GT(overflow_calls, overflows);
  EXPECT_GE(deep, 7u);
  EXPECT_LE(deep, 59u);
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
    ASSERT_EQ(problem, "") << "seed " << seed << ": " << problem.substr(0, 600);
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
    // A gate that opens again after the bug (the machines already differ) is caught by the START_CHECK of that arm, earlier than the check frame:
    // "DESYNC start frame=<arm frame>" with the arm frame in (bug_frame, expected_check].
    const auto caught_by = [&](const std::string& log) {
      if (log.find(want) != std::string::npos)
        return true;
      const size_t at = log.find("DESYNC start frame=");
      if (at == std::string::npos)
        return false;
      const s32 frame = std::atoi(log.c_str() + at + std::strlen("DESYNC start frame="));
      return frame > p.bug_frame && frame <= expected_check;
    };
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_TRUE(caught_by(res.m[i].log)) << "seed " << seed << " machine " << i << " wanted '" << want << "' or a START_CHECK desync\n" << res.m[i].log;
      EXPECT_TRUE(res.m[i].mode == Mode::OFFLINE) << "seed " << seed << " machine " << i;
      EXPECT_EQ(res.m[i].stats.checks_bad, 1u);
      EXPECT_EQ(res.m[i].counter, p.frames);  // the game goes on offline
    }
    if (caught_by(res.m[0].log) && caught_by(res.m[1].log))
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
// ---- M5: pacing in the closed loop --------------------------------------------------------------------------------------------
// D's first loopback (two Dolphins, 80 ms delay, 30 ms jitter, 5 % loss, input delay 2, MAXR 5): the host stalled 1902 of 7788 frames and never rolled
// back, the guest stalled 5 times and rolled back 42 times. The host's pads were the ones that kept changing, the guest's stayed the same. With pacing
// (1 % at most) the second run was no better: host 1691 stalls, 130 slowdowns that were all at the maximum, the host's lead (+2.5 frames, the stall limit)
// never went away. Two causes, both modelled by the test network (SimMachine with Dolphin's catch-up throttle):
//  1. Dolphin's throttle lets a machine that waited run the next frames unthrottled, so a machine that starts ahead sits at the stall limit for good (the
//     gate-open skew: `lead_frames` below).
//  2. The hidden resimulation ran with the throttle off and every Throttle call restarted the clock, so the re-run time was lost for good: a machine
//     that rolled back 120 times at ~7 ms lost about 2 % of its speed and drifted later and later, more than the 1 % pacing could take back
//     (`resim_restarts_clock = true` is that old behaviour; since the fix CoreTimingManager::BeginResim / EndResim keep the schedule).
SimParams LoopbackLikeParams(u64 seed, u64 lead_frames, u64 resim_us, bool pace)
{
  SimParams p;
  p.seed = seed;
  p.delay = 2;
  p.maxr = 5;
  p.net.delay_us = 50000;
  p.net.jitter_us = 30000;
  p.net.loss_pct = 5;
  p.net.resend_us = 25000;
  p.catch_up = true;
  p.pace = pace;
  p.frames = 2400;
  p.open_at = 60;
  p.close_at = 2300;
  p.change_pct = 8;        // the host's pad moves (the guest predicts it and rolls back)
  p.guest_change_pct = 0;  // the guest's pad never does (the host never rolls back)
  p.resim_compute_us[0] = p.resim_compute_us[1] = resim_us;
  p.period_us[0] = p.period_us[1] = 16683;
  p.gate_skew_us[1] = lead_frames * 16683;  // the guest's throttle schedule starts this late: the host starts `lead_frames` ahead
  return p;
}

TEST(Rev3Session, PacingRemovesTheLeadersPersistentStallsWhenOneSideStartsFiveFramesAhead)
{
  u32 compared = 0;
  // (A start 5 frames ahead is not always persistent: a long jitter stall can make the leader's lag exceed MAIN_MAX_FALLBACK, which shifts its schedule
  // back and ends the lead on its own. These seeds are the ones where it persists with pacing off, which is the case worth fixing. The list was
  // re-picked on Oct 17, when the arm became an agreed frame: the arm frame moved, and with it which seeds keep their lead.)
  for (u64 seed : {11ull, 12ull, 13ull, 16ull})
    for (u64 resim_us : {2000ull, 8000ull})
    {
      SimResult off, on;
      const SimParams p_off = LoopbackLikeParams(seed, 5, resim_us, false);
      const SimParams p_on = LoopbackLikeParams(seed, 5, resim_us, true);
      ASSERT_TRUE(RunSim(p_off, &off));
      ASSERT_TRUE(RunSim(p_on, &on));
      const std::string what = "seed " + std::to_string(seed) + " resim " + std::to_string(resim_us) + " us";
      EXPECT_EQ(CheckHealthy(p_off, off), "") << what;
      EXPECT_EQ(CheckHealthy(p_on, on), "") << what;
      // Both sides end in the same state, the reference one, with and without pacing.
      EXPECT_TRUE(off.m[0].state == off.m[1].state) << what;
      EXPECT_TRUE(on.m[0].state == on.m[1].state) << what;
      EXPECT_TRUE(on.m[0].state == off.m[0].state) << what;
      // The baseline reproduces D's picture: the host pinned at the stall limit, the guest hardly ever stalling.
      EXPECT_GT(off.m[0].stats.stalls, 300u) << what;
      EXPECT_LT(off.m[1].stats.stalls, 20u) << what;
      EXPECT_EQ(off.m[0].stats.rollbacks, 0u) << what;
      // Pacing: the leader's stalls drop by a large factor (and the time it spends waiting with them).
      EXPECT_LE(on.m[0].stats.stalls * 8, off.m[0].stats.stalls) << what;
      EXPECT_LE(on.m[0].stats.stall_us_total * 4, off.m[0].stats.stall_us_total) << what;
      // The lead was turned into delay on the leader's side only.
      EXPECT_GT(on.m[0].stats.pace_slowdowns, 0u) << what;
      EXPECT_LT(std::abs(static_cast<double>(on.m[0].stats.pace_offset_sum_us) / std::max<u32>(on.m[0].stats.pace_checks, 1) / PACE_FRAME_US), 1.0) << what;  // level on average
      EXPECT_GT(on.m[0].slept_us, 0u) << what;
      EXPECT_EQ(on.m[0].stats.pace_sleep_us_total, on.m[0].slept_us) << what;
      EXPECT_EQ(off.m[0].slept_us, 0u) << what;
      EXPECT_EQ(off.m[0].delay_calls, 0u) << what;
      EXPECT_GT(off.m[0].stats.pace_checks, 10u) << what;  // pacing off still measures
      EXPECT_TRUE(off.m[0].log.find("action=off") != std::string::npos) << what;
      // The sleep is bounded: slices of at most 4 ms per call, one check never asks for more than 5 frames, and the whole run loses well under
      // 2 % of its length to it (the run lasts about 40 s; D's loopback lasted 130 s).
      for (const SimMachineResult& m : on.m)
      {
        EXPECT_LE(m.largest_delay_us, 4000u) << what;
        EXPECT_LE(m.slept_us, static_cast<u64>(m.stats.pace_slowdowns) * 5 * PACE_FRAME_US) << what;
        EXPECT_LT(m.slept_us * 50, on.end_us) << what;
        EXPECT_EQ(m.bad_delay_calls, 0u) << what;
      }
      // Pacing is not allowed to make the match longer by more than the sleep it took.
      EXPECT_LE(on.end_us, off.end_us + 1000000) << what;
      // The checksums the two sides exchanged agree (no DESYNC, the final ones are the reference ones).
      EXPECT_GT(on.m[0].chk_lines, 10u) << what;
      EXPECT_EQ(on.m[0].stats.checks_bad, 0u) << what;
      EXPECT_EQ(on.m[1].stats.checks_bad, 0u) << what;
      ++compared;
    }
  EXPECT_EQ(compared, 8u);
}

TEST(Rev3Session, ARollbackThatKeepsTheThrottleScheduleLeavesTheMachineLevelWithoutAnyPacing)
{
  // The cause of the drift in D's first loopback. The guest rolls back on ~8 % of the frames (the host's pad moves), each re-run costs 2 or 8 ms per
  // frame. Old model (every Throttle call of the resimulation restarts the clock): the re-run time is lost for good, the guest falls behind by a
  // few frames and the host sits at the stall limit. Model of the fix (the schedule is left alone and put back, CoreTimingManager::BeginResim /
  // EndResim): the guest pays for the re-run out of the following frames (it runs them unthrottled until it is back on its schedule) and stays level
  // with no pacing at all.
  // (Seeds where the two machines happen to come out of the lockstep level: the question here is what the rollbacks do, not the gate skew.)
  for (u64 seed : {12ull, 16ull})  // (re-picked on Oct 17 with the agreed arm frame: seeds where the two come out level)
    for (u64 resim_us : {2000ull, 8000ull})
    {
      SimParams keep = LoopbackLikeParams(seed, 0, resim_us, false);
      SimParams restart = keep;
      restart.resim_restarts_clock = true;
      SimResult a, b;
      ASSERT_TRUE(RunSim(keep, &a));
      ASSERT_TRUE(RunSim(restart, &b));
      const std::string what = "seed " + std::to_string(seed) + " resim " + std::to_string(resim_us) + " us";
      EXPECT_EQ(CheckHealthy(keep, a), "") << what;
      EXPECT_EQ(CheckHealthy(restart, b), "") << what;
      EXPECT_GT(a.m[1].stats.rollbacks, 100u) << what;  // the guest really rolls back a lot
      EXPECT_LT(a.m[0].stats.stalls + a.m[1].stats.stalls, 60u) << what;  // nobody pinned (which side leads a little varies with the seed)
      EXPECT_LT(std::abs(static_cast<double>(a.m[0].stats.pace_offset_sum_us) / std::max<u32>(a.m[0].stats.pace_checks, 1) / PACE_FRAME_US), 1.6) << what;
      EXPECT_GT(b.m[0].stats.stalls, 500u) << what;      // the old behaviour: the host pinned, as in D's loopback
      EXPECT_EQ(b.m[0].stats.rollbacks, 0u) << what;
      EXPECT_TRUE(a.m[0].state == a.m[1].state) << what;
    }
}

TEST(Rev3Session, WhenARollbackRestartsTheClockPacingStillRemovesMostOfTheLeadersStalls)
{
  // The old throttle behaviour (resim_restarts_clock): pacing at up to 2 % takes back what a machine loses to its rollbacks.
  for (u64 resim_us : {2000ull, 8000ull})
  {
    SimParams off = LoopbackLikeParams(12, 0, resim_us, false);
    off.resim_restarts_clock = true;
    SimParams on = off;
    on.pace = true;
    SimResult a, b;
    ASSERT_TRUE(RunSim(off, &a));
    ASSERT_TRUE(RunSim(on, &b));
    EXPECT_EQ(CheckHealthy(off, a), "") << resim_us;
    EXPECT_EQ(CheckHealthy(on, b), "") << resim_us;
    EXPECT_GT(a.m[0].stats.stalls, 500u) << resim_us;
    EXPECT_LE(b.m[0].stats.stalls * 8, a.m[0].stats.stalls) << resim_us;
    EXPECT_TRUE(b.m[0].state == b.m[1].state) << resim_us;
    EXPECT_EQ(b.m[0].bad_delay_calls, 0u) << resim_us;
  }
}

TEST(Rev3Session, PacingFollowsAPeerWhoseClockRunsOnePointFivePercentSlower)
{
  // A machine that is simply slower (here the guest's frame is 1.5 % longer) pins the faster one at the stall limit for good; the leader's gentle
  // slowdown (up to 2 %) takes the difference back, a 1 % cap (the first M5 build) could not.
  SimParams off = LoopbackLikeParams(13, 0, 2000, false);
  off.period_us[1] = off.period_us[0] + off.period_us[0] * 15 / 1000;
  SimParams on = off;
  on.pace = true;
  SimResult a, b;
  ASSERT_TRUE(RunSim(off, &a));
  ASSERT_TRUE(RunSim(on, &b));
  EXPECT_EQ(CheckHealthy(off, a), "");
  EXPECT_EQ(CheckHealthy(on, b), "");
  EXPECT_GT(a.m[0].stats.stalls, 300u);
  EXPECT_LE(b.m[0].stats.stalls * 8, a.m[0].stats.stalls);
  EXPECT_TRUE(b.m[0].state == b.m[1].state);
  EXPECT_LE(b.m[0].largest_delay_us, 4000u);
  EXPECT_LT(b.m[0].slept_us * 50, b.end_us);  // under 2 % of the run
}

TEST(Rev3Session, PacingBarelyActsOnABalancedPairAndStillAgrees)
{
  for (u64 seed : {21ull, 22ull, 23ull})
  {
    SimParams p = LoopbackLikeParams(seed, 0, 2000, true);
    p.change_pct = 15;
    p.guest_change_pct = -1;  // both players move: both machines predict and roll back
    SimResult res;
    ASSERT_TRUE(RunSim(p, &res));
    EXPECT_EQ(CheckHealthy(p, res), "") << seed;
    EXPECT_TRUE(res.m[0].state == res.m[1].state) << seed;
    for (const SimMachineResult& m : res.m)
    {
      EXPECT_GT(m.stats.rollbacks, 100u) << seed;
      EXPECT_LT(m.slept_us * 100, res.end_us) << seed;  // under 1 % of the run
      EXPECT_EQ(m.bad_delay_calls, 0u) << seed;
    }
  }
}

TEST(Rev3Session, PacingRunsAreReproducible)
{
  const SimParams p = LoopbackLikeParams(31, 5, 2000, true);
  SimResult a, b;
  ASSERT_TRUE(RunSim(p, &a));
  ASSERT_TRUE(RunSim(p, &b));
  EXPECT_EQ(a.log_hash, b.log_hash);
  EXPECT_EQ(a.m[0].slept_us, b.m[0].slept_us);
}

TEST(Rev3Session, ThousandSeedsWithThrottleAndPacingAgreeWithTheReferenceAndNeverPaceOutsideNormalArmedCalls)
{
  u64 delay_calls = 0, slept = 0, rollbacks = 0, resims = 0, checks = 0, reopens = 0;
  u32 failures = 0;
  for (u64 seed = 1; seed <= 1000; ++seed)
  {
    SimParams p = ParamsFromSeed(seed);
    Rng r(seed * 31 + 7);
    p.catch_up = true;
    p.pace = true;
    p.compute_us[0] = 500 + r.Below(4000);
    p.compute_us[1] = 500 + r.Below(4000);
    p.resim_compute_us[0] = 500 + r.Below(9000);
    p.resim_compute_us[1] = 500 + r.Below(9000);
    p.max_fallback_us = 50000 + r.Below(100000);
    p.resim_restarts_clock = r.Below(2) == 0;  // the old throttle behaviour for half of the seeds
    p.gate_skew_us[0] = r.Below(3) == 0 ? r.Below(8 * 16683) : 0;
    p.gate_skew_us[1] = r.Below(3) == 0 ? r.Below(8 * 16683) : 0;
    SimResult res;
    ASSERT_TRUE(RunSim(p, &res));
    const std::string problem = CheckHealthy(p, res);
    if (!problem.empty())
    {
      EXPECT_EQ(problem, "") << "seed " << seed << ": " << problem.substr(0, 600);
      if (++failures >= 5)
        break;
      continue;
    }
    EXPECT_TRUE(res.m[0].state == res.m[1].state) << "seed " << seed;
    for (const SimMachineResult& m : res.m)
    {
      EXPECT_EQ(m.bad_delay_calls, 0u) << "seed " << seed << ": a pacing delay outside a normal ARMED call";
      EXPECT_LE(m.largest_delay_us, 4000u) << "seed " << seed;
      EXPECT_LE(m.slept_us, static_cast<u64>(m.stats.pace_slowdowns) * 5 * PACE_FRAME_US) << "seed " << seed;
      EXPECT_EQ(m.slept_us, m.stats.pace_sleep_us_total) << "seed " << seed;
      delay_calls += m.delay_calls;
      slept += m.slept_us;
      rollbacks += m.stats.rollbacks;
      resims += m.stats.resim_calls;
      checks += m.stats.pace_checks;
      if (m.stats.gate_opens > 1)
        ++reopens;
    }
  }
  // The seeds must reach pacing, rollbacks, resimulations and re-opened gates, or the test proves nothing.
  EXPECT_GT(delay_calls, 1000u);
  EXPECT_GT(slept, 1000000u);
  EXPECT_GT(checks, 2000u);
  EXPECT_GT(rollbacks, 5000u);
  EXPECT_GT(resims, 5000u);
  EXPECT_GT(reopens, 100u);
}



}  // namespace
