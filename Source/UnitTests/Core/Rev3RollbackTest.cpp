// Unit tests for Core/Slippi/Rev3Rollback: the wire codec, the whitelist parser and the SyncTest state machine, driven by a fake
// guest memory and a tiny deterministic "game" (no Dolphin core, no game data). Run: build target `tests`, then `tests --gtest_filter=Rev3*`.

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Slippi/Rev3Rollback.h"

namespace
{
using namespace Rev3;

// ---- fakes -----------------------------------------------------------------------------------------------------------------

// MEM1 only (24 MB, zero filled); reads and writes outside it fail like the real accessor's range check.
class FakeMemory final : public GuestMemory
{
public:
  FakeMemory() : m_mem1(0x1800000, 0) {}

  bool Read(u32 addr, void* dst, size_t n) const override
  {
    if (!Inside(addr, n))
      return false;
    std::memcpy(dst, &m_mem1[addr - 0x80000000], n);
    return true;
  }
  bool Write(u32 addr, const void* src, size_t n) override
  {
    if (!Inside(addr, n))
      return false;
    std::memcpy(&m_mem1[addr - 0x80000000], src, n);
    return true;
  }

  u32 U32(u32 addr) const
  {
    u8 b[4] = {};
    Read(addr, b, 4);
    return (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | b[3];
  }
  void SetU32(u32 addr, u32 v)
  {
    const u8 b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)};
    Write(addr, b, 4);
  }

private:
  static bool Inside(u32 addr, size_t n) { return addr >= 0x80000000 && u64(addr) + n <= 0x81800000; }
  std::vector<u8> m_mem1;
};

class FakeSink final : public Sink
{
public:
  void Line(const std::string& text) override { lines.push_back(text); }
  void Osd(const std::string& text) override { osd.push_back(text); }

  bool Contains(const std::string& needle) const
  {
    for (const std::string& l : lines)
    {
      if (l.find(needle) != std::string::npos)
        return true;
    }
    return false;
  }

  std::vector<std::string> lines;
  std::vector<std::string> osd;
};

// The layout of the fake game's memory.
constexpr u32 TEAM[2] = {0x80400000, 0x80401000};
constexpr u32 NODE[2] = {0x80410000, 0x80411000};
constexpr u32 FIGHTER[2] = {0x80420000, 0x80430000};
constexpr u32 STATE_A = 0x80500000;  // whitelisted: 0x100 bytes
constexpr u32 STATE_B = 0x80600000;  // whitelisted: 0x40 bytes
constexpr u32 HIDDEN = 0x80700000;   // NOT whitelisted
constexpr u32 SP = 0x80800000;       // the hook's stack pointer
constexpr u32 WINDOW = 0x400;

constexpr u32 FIELD_X = 0x84;
constexpr u32 FIELD_HEALTH = 0x28;

// A deterministic game: every step adds the pads to counters in two whitelisted ranges and mirrors them into the fighters. With
// `leak` set, a hidden counter outside the whitelist also feeds the state (what an incomplete whitelist looks like).
class FakeGame
{
public:
  explicit FakeGame(bool leak) : m_leak(leak)
  {
    for (int p = 0; p < 2; ++p)
    {
      m_mem.SetU32(0x8038FE50 + 4 * p, TEAM[p]);
      m_mem.SetU32(TEAM[p] + 0x0C, NODE[p]);
      m_mem.SetU32(NODE[p], FIGHTER[p]);
    }
  }

  std::vector<Range> Ranges() const { return {{STATE_A, STATE_A + 0x100}, {STATE_B, STATE_B + 0x40}}; }

  static std::array<PadBytes, 2> PadsFor(u32 frame)
  {
    std::array<PadBytes, 2> pads{};
    for (size_t i = 0; i < 12; ++i)
    {
      pads[0][i] = u8(frame * 3 + i);
      pads[1][i] = u8(frame * 5 + 2 * i);
    }
    return pads;
  }

  // One hook call plus the game frame that follows it. Returns the reply.
  Reply Frame(SyncTest& test, u32 sp = SP)
  {
    // The hook's stack frame holds its frame counter; a RESTORED reply must bring the old value back.
    m_mem.SetU32(sp, m_counter);
    m_mem.SetU32(sp + 4, 0xC0FFEE00 + m_counter);
    Command c;
    c.frame = m_counter;
    c.pads = PadsFor(m_counter);
    c.sp = sp;
    const Reply r = test.OnCommand(c);
    if (r.flags & REPLY_FLAG_RESTORED)
    {
      EXPECT_EQ(m_mem.U32(sp), r.frame) << "stack frame not restored";
      EXPECT_EQ(m_mem.U32(sp + 4), 0xC0FFEE00 + r.frame);
      m_counter = r.frame;
    }
    Step(r.pads);
    ++m_counter;
    return r;
  }

  void Step(const std::array<PadBytes, 2>& pads)
  {
    const u32 hidden = m_mem.U32(HIDDEN);
    m_mem.SetU32(HIDDEN, hidden + 1);
    const u32 leak = m_leak ? hidden : 0;
    const u32 a = m_mem.U32(STATE_A) + pads[0][0] + pads[1][0] + leak;
    const u32 b = m_mem.U32(STATE_B + 4) + pads[0][3] * 2 + pads[1][5];
    m_mem.SetU32(STATE_A, a);
    m_mem.SetU32(STATE_A + 0x80, a ^ 0x5A5A5A5A);
    m_mem.SetU32(STATE_B + 4, b);
    m_mem.SetU32(FIGHTER[0] + FIELD_X, a);
    m_mem.SetU32(FIGHTER[1] + FIELD_HEALTH, b);
  }

  FakeMemory& Mem() { return m_mem; }
  u32 Counter() const { return m_counter; }

private:
  FakeMemory m_mem;
  bool m_leak;
  u32 m_counter = 1000;
};

SyncTestOptions Options(int k, int every = 1)
{
  SyncTestOptions o;
  o.k = k;
  o.every = every;
  o.stack_window = WINDOW;
  o.whitelist_path = "test.json";
  return o;
}

std::function<u64()> FakeClock()
{
  auto now = std::make_shared<u64>(0);
  return [now]() { return *now += 7; };
}
}  // namespace

// ---- codec ------------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, CommandCodec)
{
  std::array<u8, CMD_SIZE> raw{};
  raw[0] = CMD_ID;
  raw[1] = PROTOCOL_VERSION;
  raw[4] = 0x52, raw[5] = 0x42, raw[6] = 0x4B, raw[7] = 0x31;
  raw[8] = 0x00, raw[9] = 0x00, raw[10] = 0x01, raw[11] = 0x02;  // frame 258
  for (u8 i = 0; i < 12; ++i)
  {
    raw[0x0C + i] = i + 1;
    raw[0x18 + i] = 0x80 + i;
  }
  raw[0x24] = 0xFF, raw[0x25] = 0xFF, raw[0x26] = 0xFF, raw[0x27] = 0xFF;  // -1
  raw[0x28] = 0x80, raw[0x29] = 0x41, raw[0x2A] = 0x23, raw[0x2B] = 0x40;  // sp

  EXPECT_TRUE(IsCommand(raw.data(), raw.size()));
  Command c;
  ASSERT_TRUE(ParseCommand(raw.data(), raw.size(), &c));
  EXPECT_EQ(c.frame, 258u);
  EXPECT_EQ(c.pads[0][0], 1);
  EXPECT_EQ(c.pads[0][11], 12);
  EXPECT_EQ(c.pads[1][0], 0x80);
  EXPECT_EQ(c.pad_return, -1);
  EXPECT_EQ(c.sp, 0x80412340u);

  // Not ours: wrong id, wrong magic, too short, other version.
  auto bad = raw;
  bad[0] = 0xB1;
  EXPECT_FALSE(IsCommand(bad.data(), bad.size()));
  bad = raw;
  bad[7] = 0x32;
  EXPECT_FALSE(IsCommand(bad.data(), bad.size()));
  EXPECT_FALSE(IsCommand(raw.data(), 63));
  bad = raw;
  bad[1] = 1;
  EXPECT_TRUE(IsCommand(bad.data(), bad.size()));
  EXPECT_FALSE(ParseCommand(bad.data(), bad.size(), &c));

  Reply r;
  r.flags = REPLY_FLAG_RESTORED;
  r.frame = 0x01020304;
  r.pads = c.pads;
  std::array<u8, REPLY_SIZE> out;
  out.fill(0xEE);
  EncodeReply(r, out.data());
  EXPECT_EQ(out[0], 'R');
  EXPECT_EQ(out[1], 'B');
  EXPECT_EQ(out[2], 'R');
  EXPECT_EQ(out[3], '1');
  EXPECT_EQ(out[7], 1);
  EXPECT_EQ(out[8], 1);
  EXPECT_EQ(out[11], 4);
  EXPECT_EQ(out[0x0C], 0);
  EXPECT_EQ(out[0x10], 1);
  EXPECT_EQ(out[0x1C], 0x80);
  EXPECT_EQ(out[0x2B], 0x0);
  EXPECT_EQ(out[63], 0);
}

// ---- whitelist --------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, WhitelistParsing)
{
  std::vector<Range> ranges;
  std::string error;
  ASSERT_TRUE(ParseWhitelist(R"({"ranges":[{"start":"0x80005520","end":"0x80005940"},{"start":2151182400,"end":"0x804DEC00"}]})",
                             &ranges, &error))
      << error;
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].start, 0x80005520u);
  EXPECT_EQ(ranges[0].end, 0x80005940u);
  EXPECT_EQ(ranges[1].start, 2151182400u);
  EXPECT_EQ(ranges[1].end, 0x804DEC00u);

  // Extra keys are ignored; MEM2 is fine.
  ASSERT_TRUE(ParseWhitelist(R"({"x":1,"ranges":[{"start":"0x90000000","end":"0x90001000","note":"heap"}]})", &ranges, &error));
  EXPECT_EQ(ranges.size(), 1u);

  const char* const bad[] = {
      "not json",
      R"({"ranges":[]})",
      R"({"nothing":1})",
      R"({"ranges":[{"start":"0x80000100","end":"0x80000100"}]})",    // empty
      R"({"ranges":[{"start":"0x80000200","end":"0x80000100"}]})",    // backwards
      R"({"ranges":[{"start":"0x80000000"}]})",                       // no end
      R"({"ranges":[{"start":"zz","end":"0x80000100"}]})",            // not a number
      R"({"ranges":[{"start":"0x00001000","end":"0x00002000"}]})",    // outside MEM1 / MEM2
      R"({"ranges":[{"start":"0x817FFF00","end":"0x81800100"}]})",    // runs past MEM1
      R"({"ranges":[{"start":-5,"end":10}]})",
  };
  for (const char* text : bad)
  {
    ranges.clear();
    error.clear();
    EXPECT_FALSE(ParseWhitelist(text, &ranges, &error)) << text;
    EXPECT_FALSE(error.empty()) << text;
    EXPECT_TRUE(ranges.empty()) << text;
  }
}

TEST(Rev3Rollback, HookRegionIsCutOut)
{
  std::vector<Range> r = {{0x80002000, 0x80003000}, {0x80002500, 0x80002600}, {0x80002A00, 0x80002B00}, {0x80001000, 0x80002400},
                          {0x80005000, 0x80006000}};
  EXPECT_EQ(SubtractRange(&r, HOOK_REGION_START, HOOK_REGION_END), 0x600u + 0x100u);
  const std::vector<Range> want = {{0x80002000, 0x80002400}, {0x80002A00, 0x80003000}, {0x80002A00, 0x80002B00},
                                   {0x80001000, 0x80002400}, {0x80005000, 0x80006000}};
  ASSERT_EQ(r.size(), want.size());
  for (size_t i = 0; i < r.size(); ++i)
  {
    EXPECT_EQ(r[i].start, want[i].start) << i;
    EXPECT_EQ(r[i].end, want[i].end) << i;
  }
}

TEST(Rev3Rollback, FieldTableMatchesChecksumModule)
{
  // rev3-mod rbchecksum.py: 61 fields of 4 bytes, first "action" at 0x140, last "str_pending" at 0x13EC, spanning 0x24..0x1648.
  EXPECT_EQ(FIELDS.size(), 61u);
  EXPECT_EQ(FIELD_BYTES, 244u);
  EXPECT_STREQ(FIELDS.front().name, "action");
  EXPECT_EQ(FIELDS.front().offset, 0x140);
  EXPECT_STREQ(FIELDS.back().name, "str_pending");
  EXPECT_EQ(FIELDS.back().offset, 0x13EC);
  u16 lo = 0xFFFF, hi = 0;
  for (const FieldDef& f : FIELDS)
  {
    lo = std::min(lo, f.offset);
    hi = std::max<u16>(hi, f.offset);
  }
  EXPECT_EQ(lo, 0x24);
  EXPECT_EQ(hi, 0x1644);
}

// ---- SyncTest ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, NoFightersJustEchoes)
{
  FakeGame game(false);
  game.Mem().SetU32(0x8038FE50, 0);  // team 0 pointer gone: not a fight
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.frame, frame);
    EXPECT_EQ(r.pads, FakeGame::PadsFor(frame));
  }
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_FALSE(test.Active());
}

TEST(Rev3Rollback, CleanRunHasNoMismatches)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());

  // The first 59 commands are not enough to start; the 60th starts the history (first_n = frame 1059).
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());

  const u32 first_n = game.Counter() + 0;  // the command of the 60th call
  EXPECT_EQ(first_n, 1059u);
  // n = first_n, first_n + 1: building history. n = first_n + 2: restore to first_n, resimulate first_n + 1 and first_n + 2.
  std::vector<std::pair<u32, u32>> seen;  // (flags, frame) per hook call
  for (int i = 0; i < 9; ++i)
  {
    const Reply r = game.Frame(test);
    seen.emplace_back(r.flags, r.frame);
  }
  const std::vector<std::pair<u32, u32>> expected = {
      {0, 1059}, {0, 1060}, {1, 1059},  // rollback at 1061 -> back to 1059
      {0, 1060}, {0, 1061},             // resimulation (1061 is the real run: no second rollback)
      {1, 1060},                        // 1062 -> back to 1060
      {0, 1061}, {0, 1062},
      {1, 1061},
  };
  EXPECT_EQ(seen, expected);

  for (int i = 0; i < 1000; ++i)
    game.Frame(test);
  while (test.Resimulating())  // finish the replay in progress so every rollback has its K comparisons
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  const SyncTestStats& s = test.Stats();
  EXPECT_GT(s.rollbacks, 50u);
  EXPECT_EQ(s.frames_compared, s.rollbacks * 2);
  EXPECT_EQ(s.mismatching_frames, 0u);
  EXPECT_EQ(s.sp_mismatches, 0u);
  EXPECT_EQ(s.restore_us_max, 7u);  // the fake clock advances 7 us per call
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_TRUE(sink.osd.empty());
  EXPECT_TRUE(sink.Contains("SYNCTEST start K=2 EVERY=1 window=0x400 whitelist=test.json ranges=2 bytes=320"));
  EXPECT_TRUE(sink.Contains("SUMMARY frame="));  // 600 frames compared were passed

  test.Finish();
  test.Finish();  // only once
  int finals = 0;
  for (const std::string& l : sink.lines)
    finals += l.find("SUMMARY final") != std::string::npos;
  EXPECT_EQ(finals, 1);
}

TEST(Rev3Rollback, EveryNthFrame)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(3, 5), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  // A rollback needs frame % 5 == 0; each one replays 3 frames, so about 400 / 5 minus the resimulated frames.
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_LT(test.Stats().rollbacks, 80u);
  EXPECT_EQ(test.Stats().frames_compared, test.Stats().rollbacks * 3);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, IncompleteWhitelistIsReported)
{
  FakeGame game(true);  // a hidden counter outside the whitelist feeds the state
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
    game.Frame(test);

  EXPECT_GT(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains("MISMATCH frame="));
  EXPECT_TRUE(sink.Contains("K=2"));
  EXPECT_TRUE(sink.Contains("  p1 x ref=0x"));  // fighter 1's x differs
  EXPECT_TRUE(sink.Contains("  range 0x80500000-0x80500100 crc ref="));
  EXPECT_TRUE(sink.Contains("first(addr:ref>resim)= 0x80500000:"));
  ASSERT_EQ(sink.osd.size(), 1u);  // on screen only once
  EXPECT_NE(sink.osd[0].find("MISMATCH"), std::string::npos);

  // Full detail for the first 20 mismatching frames, then counts only.
  int detailed = 0;
  for (const std::string& l : sink.lines)
    detailed += l.rfind("MISMATCH frame=", 0) == 0;
  EXPECT_EQ(detailed, 20);
  EXPECT_GT(test.Stats().mismatching_frames, 20u);
  EXPECT_TRUE(sink.Contains("detail is logged for the first 20"));
}

TEST(Rev3Rollback, StackPointerMismatchSkipsTheRestore)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_GT(rollbacks, 0u);

  // Drive the state machine to a normal frame whose rollback point has another sp.
  // The game is mid-cycle at an arbitrary place, so run until a normal frame (not resimulating) comes next.
  while (test.Resimulating())
    game.Frame(test);
  const Reply r = game.Frame(test, SP + 0x40);
  EXPECT_EQ(r.flags, 0u);  // no restore under a different sp
  EXPECT_EQ(test.Stats().sp_mismatches, 1u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_TRUE(sink.Contains("ERROR"));
  EXPECT_TRUE(sink.Contains("differs from the snapshot"));

  // And it carries on afterwards.
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, LosingTheFightClearsHistory)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(test.Active());

  game.Mem().SetU32(0x8038FE50 + 4, 0);  // team 1 gone (loading screen)
  for (int i = 0; i < 20; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.frame, frame);
  }
  EXPECT_FALSE(test.Active());
  EXPECT_FALSE(test.Resimulating());
  EXPECT_TRUE(sink.Contains("fighters lost"));

  // Back in a fight: 60 commands must pass again before any rollback.
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, GameThatIgnoresTheRestoreIsDetected)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 61; ++i)
    game.Frame(test);
  // Next call is the rollback; the "hook" ignores the RESTORED frame and carries on counting up.
  Command c;
  c.frame = game.Counter();
  c.pads = FakeGame::PadsFor(c.frame);
  c.sp = SP;
  const Reply r = test.OnCommand(c);
  ASSERT_EQ(r.flags, REPLY_FLAG_RESTORED);
  c.frame = r.frame + 5;  // wrong: the restore asks for r.frame + 1 next
  const Reply r2 = test.OnCommand(c);
  EXPECT_EQ(r2.flags, 0u);
  EXPECT_FALSE(test.Resimulating());
  EXPECT_TRUE(sink.Contains("ERROR resimulation expected frame"));
}
