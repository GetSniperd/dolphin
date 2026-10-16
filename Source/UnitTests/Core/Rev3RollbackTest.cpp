// Unit tests for Core/Slippi/Rev3Rollback: the wire codec, the whitelist parser and the SyncTest state machine, driven by a fake
// guest memory and a tiny deterministic "game" (no Dolphin core, no game data). Run: build target `tests`, then `tests --gtest_filter=Rev3*`.

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

  // Direct pointer for the page tracker; switch off to exercise its Read() fallback.
  const u8* Peek(u32 addr, size_t n) const override
  {
    if (!peek_enabled || !Inside(addr, n))
      return nullptr;
    return &m_mem1[addr - 0x80000000];
  }
  bool peek_enabled = true;

  void InvalidateCode(u32 addr, size_t n) override { invalidated.emplace_back(addr, n); }
  std::vector<std::pair<u32, size_t>> invalidated;  // every InvalidateCode call, in order

  bool ReadTimeBase(u64* out) const override
  {
    *out = time_base;
    return true;
  }
  bool WriteTimeBase(u64 value) override
  {
    time_base = value;
    ++time_base_writes;
    return true;
  }
  u64 time_base = 5000000;  // the emulated time base; FakeGame advances it by TB_PER_FRAME every frame
  int time_base_writes = 0;

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

// Hardware that uses memory on its own: Query returns whatever `list` holds right now.
class FakeHardware final : public HardwareView
{
public:
  void Query(std::vector<Exclusion>* out) override
  {
    ++queries;
    out->insert(out->end(), list.begin(), list.end());
  }
  std::vector<Exclusion> list;
  int queries = 0;
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

constexpr u64 TB_PER_FRAME = 1000;

constexpr u32 HW_WORD = STATE_A + 0x44;  // a word "hardware" writes by itself every frame (inside the excluded 0x40..0x80)
constexpr u32 FIELD_X = 0x84;
constexpr u32 FIELD_HEALTH = 0x28;
constexpr u32 FIELD_ANGLEA0 = 0xA0;
constexpr u32 FIELD_Y = 0x88;
constexpr u32 FIELD_COMBO_COUNT = 0x9AC;
constexpr u32 LATE_PAGE = 0x80A00000;  // a page nothing seeds; only SetLatePage makes it change
constexpr u32 LATE_WALK = 0x80C00000;  // SetWalker's first page

// A deterministic game: every step adds the pads to counters in two whitelisted ranges and mirrors them into the fighters. With
// `leak` set, a hidden counter outside the whitelist also feeds the state (what an incomplete whitelist looks like).
class FakeGame
{
public:
  explicit FakeGame(bool leak, u32 late_leak_from = 0) : m_leak(leak), m_late_from(late_leak_from)
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
    // The time base the hook call of this frame sees: a frame visited again (after a restore) must see the same value again.
    const auto seen = m_tb_seen.emplace(m_counter, m_mem.time_base);
    if (!seen.second && seen.first->second != m_mem.time_base)
      m_tb_consistent = false;
    Command c;
    c.frame = m_counter;
    c.pads = PadsFor(m_counter);
    if (m_z.has_value())
    {
      c.pads[0][0] = 0;
      c.pads[0][1] = *m_z ? 0x10 : 0x00;
    }
    c.sp = sp;
    const Reply r = test.OnCommand(c);
    if (r.flags & REPLY_FLAG_RESTORED)
    {
      ++m_restores_seen;
      m_hw_word_intact = m_hw_word_intact && m_mem.U32(HW_WORD) == m_hw_ticks;
      EXPECT_EQ(m_mem.U32(sp), r.frame) << "stack frame not restored";
      EXPECT_EQ(m_mem.U32(sp + 4), 0xC0FFEE00 + r.frame);
      m_counter = r.frame;
    }
    Step(r.pads);
    if (m_hw_writes)
      m_mem.SetU32(HW_WORD, ++m_hw_ticks);  // the hardware's own write, after the frame
    m_mem.time_base += TB_PER_FRAME;
    ++m_counter;
    return r;
  }

  bool TimeBaseConsistent() const { return m_tb_consistent; }  // every revisited frame saw the time base it saw the first time

  void EnableHardwareWord() { m_hw_writes = true; }  // "hardware" starts writing HW_WORD every frame
  void SetZ(bool down) { m_z = down; }  // from now on port 0's buttons carry Z (0x0010) or nothing
  bool HwWordIntact() const { return m_hw_word_intact; }  // every RESTORED reply left the hardware word alone
  int RestoresSeen() const { return m_restores_seen; }

  void SetQuiet() { m_quiet = true; }  // Step changes nothing: only the hook's stack frame (written by Frame) changes
  // From this frame on a counter in the page LATE_PAGE (outside every seed) counts up and feeds p1's combo_count.
  void SetLatePage(u32 from_frame) { m_late_page_from = from_frame; }
  // From this frame on every frame writes one constant word into a new page: LATE_WALK + (frame - from) * stride.
  void SetWalker(u32 from_frame, u32 stride) { m_walk_from = from_frame, m_walk_stride = stride; }

  void Step(const std::array<PadBytes, 2>& pads)
  {
    if (m_quiet)
      return;
    if (m_late_page_from != 0 && m_counter >= m_late_page_from)
    {
      const u32 v = m_mem.U32(LATE_PAGE);
      m_mem.SetU32(LATE_PAGE, v + 1);
      m_mem.SetU32(FIGHTER[0] + FIELD_COMBO_COUNT, v);
    }
    if (m_walk_from != 0 && m_counter >= m_walk_from)
      m_mem.SetU32(LATE_WALK + (m_counter - m_walk_from) * m_walk_stride, 0x1234);
    const u32 hidden = m_mem.U32(HIDDEN);
    m_mem.SetU32(HIDDEN, hidden + 1);
    const u32 leak = m_leak ? hidden : 0;
    const u32 a = m_mem.U32(STATE_A) + pads[0][0] + pads[1][0] + leak;
    const u32 b = m_mem.U32(STATE_B + 4) + pads[0][3] * 2 + pads[1][5];
    m_mem.SetU32(STATE_A, a);
    m_mem.SetU32(STATE_A + 0x80, a ^ 0x5A5A5A5A);
    m_mem.SetU32(STATE_B + 4, b);
    m_mem.SetU32(FIGHTER[0] + FIELD_X, a);
    if (m_late_from != 0 && m_counter >= m_late_from)
    {
      m_mem.SetU32(FIGHTER[0] + FIELD_ANGLEA0, hidden);  // a second, later leak into other fighter fields:
      m_mem.SetU32(FIGHTER[1] + FIELD_Y, hidden);        // p1 anglea0 (in the test's snapshot) and p2 y (not in it)
    }
    m_mem.SetU32(FIGHTER[1] + FIELD_HEALTH, b);
  }

  FakeMemory& Mem() { return m_mem; }
  u32 Counter() const { return m_counter; }

private:
  FakeMemory m_mem;
  bool m_leak;
  u32 m_late_from;
  bool m_quiet = false;
  u32 m_late_page_from = 0;
  u32 m_walk_from = 0;
  u32 m_walk_stride = 0;
  std::map<u32, u64> m_tb_seen;
  bool m_tb_consistent = true;
  u32 m_counter = 1000;
  std::optional<bool> m_z;
  bool m_hw_writes = false;
  u32 m_hw_ticks = 0;
  bool m_hw_word_intact = true;
  int m_restores_seen = 0;
};

SyncTestOptions Options(int k, int every = 1)
{
  SyncTestOptions o;
  o.k = k;
  o.every = every;
  o.stack_window = WINDOW;
  o.whitelist_path = "test.json";
  o.arm_commands = 60;  // the product default is 600; most tests do not need to wait that long
  o.adaptive = false;   // the classic tests use a fixed whitelist; the adaptive snapshot set has its own tests at the end
  return o;
}

// The adaptive snapshot set: no whitelist needed, the set is learned. `slices` pages are hashed per call in 1/slices parts.
SyncTestOptions AdaptiveOptions(int k, u32 slices = 16, u32 learn = 20)
{
  SyncTestOptions o = Options(k);
  o.adaptive = true;
  o.scan_slices = slices;
  o.learn_cycles = learn;
  return o;
}

bool Overlaps(const Range& r, u32 lo, u32 hi)
{
  return r.start < hi && lo < r.end;
}

bool SetContains(const std::vector<Range>& set, u32 addr, size_t n = 4)
{
  for (const Range& r : set)
  {
    if (addr >= r.start && u64(addr) + n <= r.end)
      return true;
  }
  return false;
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
  EXPECT_TRUE(sink.Contains("arm=60 noop=0"));
  EXPECT_TRUE(sink.Contains("NOTE armed at frame 1059 (60 hook calls with both fighters)"));
  EXPECT_EQ(s.icache_us_max, 7u);
  EXPECT_EQ(s.noop_restores, 0u);
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

// ---- arm delay, noop mode, hardware exclusions, pad toggle, JIT invalidation ------------------------------------------------

TEST(Rev3Rollback, ArmsOnlyAfterTheConfiguredDelay)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.arm_commands = 600;  // the product default
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 599; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_FALSE(sink.Contains("NOTE armed"));

  game.Frame(test);  // the 600th call arms
  EXPECT_TRUE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE armed at frame 1599"));
  for (int i = 0; i < 30; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // Losing the fighters disarms and the delay starts over.
  game.Mem().SetU32(0x8038FE50 + 4, 0);
  game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("fighters lost"));
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 599; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_FALSE(test.Active());
}

TEST(Rev3Rollback, HeartbeatEvery60ThenEvery300CallsWhileArmed)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 60 + 899; ++i)  // armed on call 60; 899 armed calls after it plus the arming call = 900
    game.Frame(test);
  int hb = 0;
  for (const std::string& l : sink.lines)
    hb += l.rfind("HB frame=", 0) == 0;
  // armed calls 60, 120 .. 600 (10 lines), then 900
  EXPECT_EQ(hb, 11);
  EXPECT_TRUE(sink.Contains(" calls=60 rollbacks="));
  EXPECT_TRUE(sink.Contains(" calls=600 rollbacks="));
  EXPECT_FALSE(sink.Contains("calls=660 "));
  EXPECT_TRUE(sink.Contains("calls=900 "));
}

TEST(Rev3Rollback, DspStateIsLoggedAtArmAndInEveryHeartbeat)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  int calls = 0;
  test.SetDspState([&calls]() { return "dsp csr=0x0804 left=" + std::to_string(++calls); });
  for (int i = 0; i < 60 + 130; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE dsp at arm: dsp csr=0x0804 left=1"));
  int hb_with_dsp = 0;
  for (const std::string& l : sink.lines)
    hb_with_dsp += l.rfind("HB frame=", 0) == 0 && l.find(" dsp csr=0x0804 left=") != std::string::npos;
  EXPECT_EQ(hb_with_dsp, 2);  // armed calls 60 and 120
}

TEST(Rev3Rollback, FirstTenRestoresAreLogged)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  int done = 0, begin = 0;
  for (const std::string& l : sink.lines)
  {
    done += l.rfind("RESTORE ", 0) == 0 && l.find(" bytes=") != std::string::npos;
    begin += l.rfind("RESTORE ", 0) == 0 && l.find(" begin") != std::string::npos;
  }
  EXPECT_EQ(done, 10);
  EXPECT_EQ(begin, 10);
  EXPECT_TRUE(sink.Contains("RESTORE 1061 -> 1059 bytes=" + std::to_string(320 + WINDOW) + " us=7 icache_us=7"));
}

TEST(Rev3Rollback, RestoresElevenToThirtyAreOneShortLine)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_GT(test.Stats().restores, 40u);
  int full = 0, shortline = 0, begin = 0;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("RESTORE ", 0) != 0)
      continue;
    begin += l.find(" begin") != std::string::npos;
    full += l.find(" bytes=") != std::string::npos;
    shortline += l.find(" bytes=") == std::string::npos && l.find(" begin") == std::string::npos && l.find(" us=7") != std::string::npos;
  }
  EXPECT_EQ(begin, 10);
  EXPECT_EQ(full, 10);
  EXPECT_EQ(shortline, 20);  // restores 11..30; none after
}

TEST(Rev3Rollback, NoopModeRestoresTheSameFrameEveryFrame)
{
  FakeGame game(true);  // even a leaky game cannot mismatch: nothing is compared
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.noop = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);

  for (int i = 0; i < 700; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);  // checks that the stack frame came back
    EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
    EXPECT_EQ(r.frame, frame);                       // same frame ...
    EXPECT_EQ(r.pads, FakeGame::PadsFor(frame));     // ... same inputs
    EXPECT_EQ(game.Counter(), frame + 1);            // the game moves on normally
  }
  const SyncTestStats& s = test.Stats();
  EXPECT_EQ(s.noop_restores, 700u);
  EXPECT_EQ(s.rollbacks, 0u);
  EXPECT_EQ(s.frames_compared, 0u);
  EXPECT_EQ(s.mismatching_frames, 0u);
  EXPECT_FALSE(test.Resimulating());
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_FALSE(sink.Contains("ERROR"));
  EXPECT_TRUE(sink.Contains("noop=1"));
  EXPECT_TRUE(sink.Contains("SUMMARY frame="));  // every 600 noop restores
  test.Finish();
  EXPECT_TRUE(sink.Contains("noop_restores=700"));
}

TEST(Rev3Rollback, HardwareRegionsAreSubtractedFromTheSnapshot)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  game.EnableHardwareWord();
  hw.list = {{"gx_fifo", STATE_A + 0x40, STATE_A + 0x80},  // inside a whitelist range: 0x40 bytes go
             {"xfb_top", 0x80900000, 0x80980000}};          // not in the whitelist at all: logged, nothing removed
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 300; ++i)
    game.Frame(test);

  EXPECT_GT(hw.queries, 0);
  EXPECT_TRUE(sink.Contains("NOTE exclude gx_fifo 0x80500040-0x80500080 (64 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE exclude xfb_top 0x80900000-0x80980000 (0 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE snapshot now ranges=3"));
  size_t bytes = 0;
  for (const Range& r : test.Ranges())
  {
    bytes += r.end - r.start;
    EXPECT_TRUE(r.end <= STATE_A + 0x40 || r.start >= STATE_A + 0x80);
  }
  EXPECT_EQ(bytes, 320u - 0x40u);
  EXPECT_EQ(test.SnapshotBytes(), bytes + WINDOW);

  // The hardware's own word was never rewound, and since it is not in the snapshot the replay does not differ because of it.
  EXPECT_GT(game.RestoresSeen(), 20);
  EXPECT_TRUE(game.HwWordIntact());
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_EQ(test.Stats().sp_mismatches, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));

  // Control: without the exclusion the same word is rewound and the replay differs.
  FakeGame game2(false);
  game2.EnableHardwareWord();
  FakeSink sink2;
  SyncTest test2(Options(2), game2.Ranges(), game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 300; ++i)
    game2.Frame(test2);
  EXPECT_FALSE(game2.HwWordIntact());
  EXPECT_GT(test2.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, NewHardwareRegionsClearTheHistoryAndOnlyGrow)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  hw.list = {{"audio_dma", STATE_B + 0x08, STATE_B + 0x10}};  // the game's own state at STATE_B + 4 stays in
  SyncTestOptions o = Options(2);
  o.hardware_check_every = 50;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Ranges().size(), 3u);  // STATE_A, STATE_B + 0 .. 8, STATE_B + 0x10 .. 0x40
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_GT(rollbacks, 0u);

  // A second buffer shows up (double buffering); the first stays excluded even when the hardware no longer reports it.
  hw.list = {{"audio_dma", STATE_B + 0x20, STATE_B + 0x30}};
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE exclude audio_dma 0x80600020-0x80600030"));
  EXPECT_TRUE(sink.Contains("history cleared, frame"));
  const std::vector<Range> want = {{STATE_A, STATE_A + 0x100}, {STATE_B, STATE_B + 0x08}, {STATE_B + 0x10, STATE_B + 0x20}, {STATE_B + 0x30, STATE_B + 0x40}};
  ASSERT_EQ(test.Ranges().size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    EXPECT_EQ(test.Ranges()[i].start, want[i].start) << i;
    EXPECT_EQ(test.Ranges()[i].end, want[i].end) << i;
  }
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  // Seeing the same region again is not news.
  int lines = 0;
  for (const std::string& l : sink.lines)
    lines += l.rfind("NOTE exclude", 0) == 0;
  EXPECT_EQ(lines, 2);
}

TEST(Rev3Rollback, PadZArmsAndDisarms)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.arm_by_pad = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  game.SetZ(false);
  for (int i = 0; i < 300; ++i)  // fighters present for a long time, no press: never armed
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("arm=pad"));

  game.SetZ(true);  // press: armed
  game.Frame(test);
  EXPECT_TRUE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE armed by Z at frame 1300"));
  for (int i = 0; i < 20; ++i)  // holding Z does nothing
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  game.SetZ(false);
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // A press in the middle of a resimulation waits until it is over.
  while (!test.Resimulating())
    game.Frame(test);
  game.SetZ(true);
  game.Frame(test);
  EXPECT_TRUE(test.Active());
  game.SetZ(false);
  for (int i = 0; i < 6; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE disarmed by Z"));
  EXPECT_FALSE(test.Resimulating());
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 50; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // And a new press arms again.
  game.SetZ(true);
  game.Frame(test);
  EXPECT_TRUE(test.Active());
}

TEST(Rev3Rollback, RestoredRangesAreInvalidatedInTheJit)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  ASSERT_GT(rollbacks, 0u);
  // Two ranges per restore, each invalidated once with its own start and length.
  ASSERT_EQ(game.Mem().invalidated.size(), rollbacks * 2);
  EXPECT_EQ(game.Mem().invalidated[0], std::make_pair(STATE_A, size_t(0x100)));
  EXPECT_EQ(game.Mem().invalidated[1], std::make_pair(STATE_B, size_t(0x40)));
  EXPECT_EQ(test.Stats().icache_us_total, test.Stats().restore_us_total);  // the fake clock: 7 us each

  FakeGame game2(false);
  FakeSink sink2;
  SyncTestOptions o = Options(2);
  o.invalidate_code = false;
  SyncTest test2(o, game2.Ranges(), game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 70; ++i)
    game2.Frame(test2);
  EXPECT_GT(test2.Stats().rollbacks, 0u);
  EXPECT_TRUE(game2.Mem().invalidated.empty());
}

// ---- field coverage diagnostics ---------------------------------------------------------------------------------------------

namespace
{
size_t CountInFieldRange(u32 range_bytes)  // fields of a fighter whose 4 bytes lie inside [0, range_bytes)
{
  size_t n = 0;
  for (const FieldDef& f : FIELDS)
    n += (u32(f.offset) + 4 <= range_bytes);
  return n;
}
}  // namespace

TEST(Rev3Rollback, ArmLogsFighterAddressesAndFieldCoverage)
{
  FakeGame game(false);
  FakeSink sink;
  // Only the first 0x100 bytes of fighter 1 are whitelisted; fighter 2 not at all.
  std::vector<Range> ranges = game.Ranges();
  ranges.push_back({FIGHTER[0], FIGHTER[0] + 0x100});
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 61; ++i)
    game.Frame(test);

  EXPECT_TRUE(sink.Contains("NOTE fighters p1=0x80420000 p2=0x80430000"));
  const size_t in1 = CountInFieldRange(0x100);
  ASSERT_GT(in1, 0u);
  ASSERT_LT(in1, FIELD_COUNT);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=" + std::to_string(in1) + "/61 NOT IN SNAPSHOT:"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=0/61 NOT IN SNAPSHOT: action@0x80430140"));
  // p1's list names a field past 0x100 (action is at 0x140) but not one inside (x at 0x84).
  std::string p1_line;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("NOTE field coverage p1", 0) == 0)
      p1_line = l;
  }
  EXPECT_NE(p1_line.find(" action@0x80420140"), std::string::npos);
  EXPECT_EQ(p1_line.find(" x@0x80420084"), std::string::npos);

  // Everything in: no list.
  FakeGame game2(false);
  FakeSink sink2;
  std::vector<Range> all = game2.Ranges();
  all.push_back({FIGHTER[0], FIGHTER[0] + 0x1700});
  all.push_back({FIGHTER[1], FIGHTER[1] + 0x1700});
  SyncTest test2(Options(2), all, game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 61; ++i)
    game2.Frame(test2);
  EXPECT_TRUE(sink2.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_FALSE(sink2.Contains("NOT IN SNAPSHOT"));
}

TEST(Rev3Rollback, MismatchNamesTheAddressAndTheSnapshotRangeOfEachNewField)
{
  FakeGame game(true, 1100);  // leak into p1 x from the start, into p1 anglea0 and p2 y from frame 1100
  FakeSink sink;
  std::vector<Range> ranges = game.Ranges();
  ranges.push_back({FIGHTER[0], FIGHTER[0] + 0x100});  // p1 x (0x84) and anglea0 (0xA0) are inside; p2 y is not
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);

  EXPECT_TRUE(sink.Contains("  p1 x ref=0x"));
  EXPECT_TRUE(sink.Contains(" addr=0x80420084 in 0x80420000-0x80420100"));

  // The address is added the first time a field name differs, not on every line.
  int x_lines = 0, x_annotated = 0, y_lines = 0, y_annotated = 0;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("  p1 x ref=", 0) == 0)
    {
      ++x_lines;
      x_annotated += l.find(" addr=") != std::string::npos;
    }
    if (l.rfind("  p2 y ref=", 0) == 0)
    {
      ++y_lines;
      y_annotated += l.find(" addr=") != std::string::npos;
    }
  }
  EXPECT_GT(x_lines, 5);
  EXPECT_EQ(x_annotated, 1);

  // Fields that differ for the first time after the first 20 detailed frames still get a block, with their address.
  EXPECT_TRUE(sink.Contains("  p1 anglea0 ref=0x"));
  EXPECT_TRUE(sink.Contains(" addr=0x804200A0 in 0x80420000-0x80420100"));
  EXPECT_GE(y_lines, 1);
  EXPECT_EQ(y_annotated, 1);
  EXPECT_TRUE(sink.Contains(" addr=0x80430088 NOT IN SNAPSHOT"));
  int detailed = 0;
  for (const std::string& l : sink.lines)
    detailed += l.rfind("MISMATCH frame=", 0) == 0;
  EXPECT_GT(detailed, 20);
  EXPECT_LT(detailed, 40);
  EXPECT_GT(test.Stats().mismatching_frames, 40u);
}

// ---- time base ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, TimeBaseIsRestoredWhenEnabled)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.restore_timebase = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_TRUE(game.TimeBaseConsistent());  // a replayed frame saw the same time base as the first run
  EXPECT_EQ(game.Mem().time_base_writes, static_cast<int>(test.Stats().rollbacks));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains("NOTE timebase restore on"));
  EXPECT_TRUE(sink.Contains(" tb=1"));
  // K = 2 frames of 1000 ticks moved back; shown in the full lines and in the short ones (restores 11..30).
  EXPECT_TRUE(sink.Contains("RESTORE 1061 -> 1059 bytes=" + std::to_string(320 + WINDOW) + " us=7 icache_us=7 tb_delta=2000"));
  int short_with_tb = 0;
  for (const std::string& l : sink.lines)
    short_with_tb += l.rfind("RESTORE ", 0) == 0 && l.find(" bytes=") == std::string::npos && l.find(" tb_delta=2000") != std::string::npos;
  EXPECT_EQ(short_with_tb, 20);
}

TEST(Rev3Rollback, TimeBaseIsLeftAloneByDefault)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(game.Mem().time_base_writes, 0);
  EXPECT_FALSE(game.TimeBaseConsistent());  // control: the replayed frames see a later time base
  EXPECT_FALSE(sink.Contains("tb_delta="));
  EXPECT_FALSE(sink.Contains("NOTE timebase"));
  EXPECT_TRUE(sink.Contains(" tb=0"));
}

TEST(Rev3Rollback, TimeBaseRestoreWorksInNoopMode)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.noop = true;
  o.restore_timebase = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().noop_restores, 30u);
  EXPECT_TRUE(game.TimeBaseConsistent());
  EXPECT_TRUE(sink.Contains("tb_delta=0"));  // same frame: nothing moved
}

// ---- user exclusions (REV3_SYNCTEST_EXCLUDE) ---------------------------------------------------------------------------------

TEST(Rev3Rollback, UserExclusionsAreSubtractedAndLogged)
{
  FakeGame game(false);
  FakeSink sink;
  std::vector<Range> ranges = game.Ranges();
  std::string error;
  ASSERT_TRUE(ApplyExclusions(R"({"ranges":[{"start":"0x80500040","end":"0x80500080"},{"start":"0x80900000","end":"0x80900100"}]})",
                              &ranges, sink, &error))
      << error;
  EXPECT_TRUE(sink.Contains("NOTE exclude user 0x80500040-0x80500080 (64 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE exclude user 0x80900000-0x80900100 (0 bytes were in the whitelist)"));
  size_t bytes = 0;
  for (const Range& r : ranges)
    bytes += r.end - r.start;
  EXPECT_EQ(bytes, 320u - 0x40u);
  EXPECT_EQ(ranges.size(), 3u);

  // Used for a SyncTest: the word the "hardware" writes inside the excluded part is never rewound.
  game.EnableHardwareWord();
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(game.RestoresSeen(), 20);
  EXPECT_TRUE(game.HwWordIntact());
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains(" bytes=256 "));  // the start line: 320 - 64 whitelisted bytes remain
}

TEST(Rev3Rollback, UserExclusionsRejectBadInput)
{
  FakeGame game(false);
  FakeSink sink;
  const std::vector<Range> original = game.Ranges();
  std::vector<Range> ranges = original;
  std::string error;
  EXPECT_FALSE(ApplyExclusions("not json", &ranges, sink, &error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(ApplyExclusions(R"({"ranges":[{"start":"0x80500000","end":"0x80600040"}]})", &ranges, sink, &error));  // removes all
  EXPECT_NE(error.find("every range"), std::string::npos);
  ASSERT_EQ(ranges.size(), original.size());  // untouched on failure
  EXPECT_EQ(ranges[0].start, original[0].start);
  EXPECT_EQ(ranges[1].end, original[1].end);
  EXPECT_FALSE(sink.Contains("NOTE exclude user"));
}

// ---- the adaptive snapshot set ------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, MergeRangesSortsAndJoins)
{
  const std::vector<Range> merged = MergeRanges({{0x300, 0x400}, {0x100, 0x200}, {0x200, 0x250}, {0x380, 0x500}, {0x700, 0x700}});
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].start, 0x100u);
  EXPECT_EQ(merged[0].end, 0x250u);
  EXPECT_EQ(merged[1].start, 0x300u);
  EXPECT_EQ(merged[1].end, 0x500u);
}

TEST(Rev3Rollback, PageTrackerLearnsPagesThatChange)
{
  for (const bool peek : {true, false})  // direct pointers and the Read() fallback must agree
  {
    FakeMemory mem;
    mem.peek_enabled = peek;
    PageTracker tracker(mem, 4);
    const u32 busy = 0x80100000, busy2 = 0x80101000, quiet = 0x80200000, once = 0x80300000;
    mem.SetU32(quiet, 0x1234);

    // Cycle 1 only takes baselines (22528 pages in 4 slices); nothing can be dynamic yet.
    for (int i = 0; i < 4; ++i)
      EXPECT_EQ(tracker.Scan(), 0u);
    EXPECT_EQ(tracker.Cycles(), 1u);
    EXPECT_EQ(tracker.DynamicPages(), 0u);

    for (u32 cycle = 0; cycle < 3; ++cycle)
    {
      mem.SetU32(busy, 100 + cycle);        // changes every cycle
      mem.SetU32(busy2 + 0xFFC, 7 + cycle);  // the last word of the next page
      if (cycle == 0)
        mem.SetU32(once, 5);                // changes once, then never again
      for (int i = 0; i < 4; ++i)
        tracker.Scan();
    }
    EXPECT_EQ(tracker.Cycles(), 4u);
    EXPECT_TRUE(tracker.IsDynamic(busy));
    EXPECT_TRUE(tracker.IsDynamic(busy + 0xFFF));
    EXPECT_TRUE(tracker.IsDynamic(busy2));
    EXPECT_TRUE(tracker.IsDynamic(once));  // sticky
    EXPECT_FALSE(tracker.IsDynamic(quiet));
    EXPECT_FALSE(tracker.IsDynamic(0x80400000));
    EXPECT_EQ(tracker.DynamicPages(), 3u);

    std::vector<Range> ranges;
    tracker.AppendDynamicRanges(&ranges);
    ASSERT_EQ(ranges.size(), 2u);  // the two neighbouring pages are one range
    EXPECT_EQ(ranges[0].start, busy);
    EXPECT_EQ(ranges[0].end, busy + 0x2000);
    EXPECT_EQ(ranges[1].start, once);
    EXPECT_EQ(ranges[1].end, once + 0x1000);

    // A restore rewrites pages: their old hashes are forgotten, so the rewrite is not taken for a change by the game.
    tracker.Reset();
    EXPECT_EQ(tracker.DynamicPages(), 0u);
    EXPECT_EQ(tracker.Cycles(), 0u);
    EXPECT_FALSE(tracker.IsDynamic(busy));
    for (int i = 0; i < 4; ++i)
      tracker.Scan();                       // baselines again
    mem.SetU32(quiet, 0x9999);              // "restored" to other content ...
    tracker.InvalidateBaseline({{quiet, quiet + 4}});
    mem.SetU32(0x80500000, 1);              // ... while a page nobody told the tracker about really changed
    for (int i = 0; i < 4; ++i)
      tracker.Scan();
    EXPECT_FALSE(tracker.IsDynamic(quiet));
    EXPECT_TRUE(tracker.IsDynamic(0x80500000));
    EXPECT_EQ(tracker.DynamicPages(), 1u);
  }
}

TEST(Rev3Rollback, AdaptiveSetAlwaysHoldsBothFightersAndTheStackWindow)
{
  FakeGame game(false);
  game.SetQuiet();  // nothing in memory changes except the hook's stack frame
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());  // no whitelist at all
  EXPECT_TRUE(sink.Contains("adaptive=1 slices=16 learn=20"));
  for (int i = 0; i < 400; ++i)
    game.Frame(test);

  ASSERT_TRUE(test.Active());
  EXPECT_EQ(test.DynamicPages(), 1u);  // only the page of the stack frame
  const std::vector<Range> want = {{FIGHTER[0], FIGHTER[0] + 0x1800}, {FIGHTER[1], FIGHTER[1] + 0x1800}, {SP, SP + 0x1000}};
  ASSERT_EQ(test.Ranges().size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    EXPECT_EQ(test.Ranges()[i].start, want[i].start) << i;
    EXPECT_EQ(test.Ranges()[i].end, want[i].end) << i;
  }
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=61/61"));
  EXPECT_FALSE(sink.Contains("NOT IN SNAPSHOT"));
  EXPECT_TRUE(sink.Contains("NOTE adaptive set at arm: dynamic_pages=1 (4096 bytes)"));
  EXPECT_TRUE(sink.Contains("NOTE snapshot now ranges=3 bytes=" + std::to_string(2 * 0x1800 + 0x1000)));
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, AdaptiveSetLearnsStateThatNoWhitelistNamed)
{
  for (const bool peek : {true, false})
  {
    FakeGame game(true);  // the hidden counter outside any whitelist feeds the state (the classic failure of a fixed whitelist)
    game.Mem().peek_enabled = peek;
    FakeSink sink;
    SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 320 + 400; ++i)
      game.Frame(test);
    while (test.Resimulating())
      game.Frame(test);

    EXPECT_TRUE(test.Active());
    EXPECT_GT(test.Stats().rollbacks, 100u);
    EXPECT_EQ(test.Stats().mismatching_frames, 0u);
    EXPECT_FALSE(sink.Contains("MISMATCH"));
    EXPECT_FALSE(sink.Contains("ERROR"));
    EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN));
    EXPECT_TRUE(SetContains(test.Ranges(), STATE_A));
    EXPECT_TRUE(SetContains(test.Ranges(), STATE_B + 4));
    EXPECT_FALSE(SetContains(test.Ranges(), 0x80900000));  // memory that never changes stays out
    EXPECT_EQ(test.Stats().pages_learned_armed, 0u);       // all of it was learned before arming
    EXPECT_TRUE(sink.Contains("NOTE learning complete: 20 scan cycles (slices=16)"));
    EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=61/61"));
    // Scan cost is in the heartbeat and the summary (the fake clock makes every timed call 7 us).
    EXPECT_TRUE(sink.Contains(" dyn="));
    EXPECT_TRUE(sink.Contains(" scan_us=7.0 scan_us_max=7"));
    test.Finish();
    EXPECT_TRUE(sink.Contains("SUMMARY final "));
    EXPECT_TRUE(sink.Contains(" dynamic_pages="));
    EXPECT_TRUE(sink.Contains(" scan_us_mean=7.0 scan_us_max=7"));
  }
}

TEST(Rev3Rollback, WhitelistStaysAnOptionalSeed)
{
  FakeGame game(true);
  FakeSink sink;
  // The seed names STATE_A / STATE_B only; the hidden counter is learned on top, the seed is kept even where nothing changes.
  std::vector<Range> seed = game.Ranges();
  seed.push_back({0x80900000, 0x80900100});  // never changes
  SyncTest test(AdaptiveOptions(2), seed, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 320 + 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80900000));
  EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_GT(test.Stats().rollbacks, 20u);
}

TEST(Rev3Rollback, ArmWaitsForTheLearningWindow)
{
  // Count mode: the 60 calls of REV3_SYNCTEST_ARM are not enough; 20 cycles of 16 scans = 320 calls are.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 319; ++i)
      EXPECT_EQ(game.Frame(test).flags, 0u);
    EXPECT_FALSE(test.Active());
    EXPECT_FALSE(test.LearningDone());
    EXPECT_EQ(test.Stats().rollbacks, 0u);
    EXPECT_TRUE(sink.Contains("NOTE arm deferred: learning (3/20 cycles)"));  // first asked for at call 60
    EXPECT_FALSE(sink.Contains("NOTE armed"));

    game.Frame(test);  // call 320 completes the 20th cycle: learned and armed together
    EXPECT_TRUE(test.LearningDone());
    EXPECT_TRUE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE learning complete: 20 scan cycles"));
    EXPECT_TRUE(sink.Contains("NOTE armed at frame 1319 (320 hook calls with both fighters)"));
    int deferred = 0;
    for (const std::string& l : sink.lines)
      deferred += l.rfind("NOTE arm deferred", 0) == 0;
    EXPECT_EQ(deferred, 1);  // logged once, not on every call
  }
  // A long N later than the learning window: N decides.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_commands = 600;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 599; ++i)
      game.Frame(test);
    EXPECT_TRUE(test.LearningDone());
    EXPECT_FALSE(test.Active());
    EXPECT_FALSE(sink.Contains("arm deferred"));
    game.Frame(test);
    EXPECT_TRUE(test.Active());
  }
  // Pad mode: a Z press during learning is remembered and arms by itself when learning completes.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_by_pad = true;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    game.SetZ(false);
    for (int i = 0; i < 49; ++i)
      game.Frame(test);
    game.SetZ(true);  // call 50
    game.Frame(test);
    game.SetZ(false);
    EXPECT_TRUE(sink.Contains("NOTE arm deferred: learning (3/20 cycles)"));
    EXPECT_FALSE(test.Active());
    for (int i = 0; i < 269; ++i)  // calls 51..319
      game.Frame(test);
    EXPECT_FALSE(test.Active());
    EXPECT_EQ(test.Stats().rollbacks, 0u);
    game.Frame(test);  // call 320
    EXPECT_TRUE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE armed by Z at frame 1319"));
    for (int i = 0; i < 50; ++i)
      game.Frame(test);
    EXPECT_GT(test.Stats().rollbacks, 5u);
    EXPECT_EQ(test.Stats().mismatching_frames, 0u);

    // Z again disarms as before; pressing before learning is done and again cancels the wish.
    game.SetZ(true);
    while (test.Resimulating())
      game.Frame(test);
    game.Frame(test);
    game.SetZ(false);
    EXPECT_FALSE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE disarmed by Z"));
  }
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_by_pad = true;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    game.SetZ(false);
    for (int i = 0; i < 10; ++i)
      game.Frame(test);
    game.SetZ(true);
    game.Frame(test);
    game.SetZ(false);
    game.Frame(test);
    game.SetZ(true);  // second press: cancel
    game.Frame(test);
    game.SetZ(false);
    EXPECT_TRUE(sink.Contains("NOTE arm cancelled by Z"));
    for (int i = 0; i < 400; ++i)
      game.Frame(test);
    EXPECT_FALSE(test.Active());  // learning completed, but nobody wants it armed
    EXPECT_TRUE(test.LearningDone());
  }
}

TEST(Rev3Rollback, ExclusionsWinOverLearnedAndSeededPages)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  hw.list = {{"audio_dma", STATE_B + 0x10, STATE_B + 0x20}, {"xfb_top", 0x80700000, 0x80701000}};  // cuts the hidden counter's page
  std::vector<Range> seed = game.Ranges();
  seed.push_back({0x80002000, 0x80003000});  // seeded across the hook region
  SyncTest test(AdaptiveOptions(2), seed, game.Mem(), sink, FakeClock(), &hw);
  test.SetUserExclusions({{STATE_A + 0x200, STATE_A + 0x300}, {FIGHTER[1] + 0x1700, FIGHTER[1] + 0x1800}});  // fighter fields end at 0x1648
  for (int i = 0; i < 320 + 300; ++i)
  {
    game.Mem().SetU32(0x80002500, i);  // the hook's own counter changes every frame: its page is dynamic, its region still cut
    game.Frame(test);
  }
  ASSERT_TRUE(test.Active());
  EXPECT_TRUE(test.LearningDone());
  const std::vector<std::pair<u32, u32>> cuts = {{HOOK_REGION_START, HOOK_REGION_END},
                                                 {STATE_A + 0x200, STATE_A + 0x300},
                                                 {FIGHTER[1] + 0x1700, FIGHTER[1] + 0x1800},
                                                 {STATE_B + 0x10, STATE_B + 0x20},
                                                 {0x80700000, 0x80701000}};
  for (const Range& r : test.Ranges())
  {
    for (const auto& c : cuts)
      EXPECT_FALSE(Overlaps(r, c.first, c.second)) << std::hex << r.start << "-" << r.end << " vs " << c.first;
  }
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80002000));  // the rest of that page is in
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80002A00));
  EXPECT_TRUE(SetContains(test.Ranges(), STATE_A));
  EXPECT_TRUE(SetContains(test.Ranges(), FIGHTER[1] + 0x1000));
  EXPECT_GT(test.DynamicPages(), 3u);  // the cut pages were learned as dynamic, then cut
  EXPECT_TRUE(sink.Contains("NOTE exclude xfb_top 0x80700000-0x80701000"));
  EXPECT_TRUE(sink.Contains("in_snapshot=61/61"));
  EXPECT_FALSE(sink.Contains("ERROR"));
}

TEST(Rev3Rollback, AFighterFieldThatIsCutRefusesToArm)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  test.SetUserExclusions({{FIGHTER[1] + 0x100, FIGHTER[1] + 0x200}});  // takes the fields at 0x140, 0x150, 0x158 ... away
  for (int i = 0; i < 1000; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot="));
  EXPECT_TRUE(sink.Contains(" NOT IN SNAPSHOT: action@0x80430140"));
  EXPECT_TRUE(sink.Contains("ERROR frame 1319: not all fighter fields are inside the snapshot set"));
  int errors = 0;
  for (const std::string& l : sink.lines)
    errors += l.rfind("ERROR", 0) == 0;
  EXPECT_EQ(errors, 1);  // once, not on every call
  EXPECT_FALSE(sink.Contains("NOTE armed"));
}

TEST(Rev3Rollback, ResimulatedFramesAreNotScanned)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(3), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 320; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  int resim_calls = 0;
  for (int i = 0; i < 200; ++i)
  {
    const bool resim = test.Resimulating();  // this call replays a frame
    const u64 scans = test.Stats().scans;
    game.Frame(test);
    EXPECT_EQ(test.Stats().scans, scans + (resim ? 0 : 1));
    resim_calls += resim;
  }
  EXPECT_GT(resim_calls, 100);  // K = 3: three of every four calls replay
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, PagesThatChangeLaterAreAddedAndTheHistoryIsCleared)
{
  FakeGame game(false);
  game.SetLatePage(1500);  // armed at frame 1319; from 1500 a counter in an unseen page feeds p1's combo_count
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 1500)  // with a rollback every frame the game advances one frame per K + 1 hook calls
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_FALSE(SetContains(test.Ranges(), LATE_PAGE));
  const size_t bytes_before = test.SnapshotBytes();
  const u32 rollbacks_before = test.Stats().rollbacks;
  EXPECT_GT(rollbacks_before, 50u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // Until the tracker comes round to that page the replay differs; then the page is added and the set rebuilt.
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  EXPECT_TRUE(SetContains(test.Ranges(), LATE_PAGE));
  EXPECT_GE(test.Stats().pages_learned_armed, 1u);
  EXPECT_GT(test.SnapshotBytes(), bytes_before);
  EXPECT_TRUE(sink.Contains("NOTE learned +1 pages (now ranges="));
  EXPECT_TRUE(sink.Contains("history cleared"));
  EXPECT_GT(test.Stats().mismatching_frames, 0u);  // the missing page was noticed before it was learned

  // From here on the replays agree again.
  while (test.Resimulating())
    game.Frame(test);
  const u32 mismatches = test.Stats().mismatching_frames;
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks + 30);
  EXPECT_EQ(test.Stats().mismatching_frames, mismatches);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
}

TEST(Rev3Rollback, LearningWhileArmedIsRateLimitedInTheLog)
{
  FakeGame game(false);
  // One new page per frame from frame 4400, each in the next scan slice (1024 slices of 22 pages): 60 growths in a row. The
  // first scan cycle only records baselines and the second finds the game's own pages, so learning 3 cycles (3072 calls) is
  // enough to arm with a complete set.
  game.SetWalker(4400, 22 * 0x1000);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2, 1024, 3), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 4400 + 1100)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GE(test.Stats().pages_learned_armed, 50u);
  int learned = 0;
  for (const std::string& l : sink.lines)
    learned += l.rfind("NOTE learned +", 0) == 0;
  EXPECT_GE(learned, 10);
  EXPECT_LE(learned, 14);
  EXPECT_TRUE(sink.Contains("NOTE further learning is logged at most every 300 hook calls"));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, LosingTheFightForgetsEverythingLearned)
{
  FakeGame game(true);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GT(test.DynamicPages(), 3u);

  game.Mem().SetU32(0x8038FE50 + 4, 0);  // loading screen
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.DynamicPages(), 0u);
  EXPECT_EQ(test.LearnCycles(), 0u);
  EXPECT_FALSE(test.LearningDone());

  // A new match learns from scratch: no rollback for 320 calls even though 60 would do for the count.
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 319; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  for (int i = 0; i < 40; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  int complete = 0;
  for (const std::string& l : sink.lines)
    complete += l.rfind("NOTE learning complete", 0) == 0;
  EXPECT_EQ(complete, 2);
}

TEST(Rev3Rollback, ExclusionsMayEmptyTheSeedWhenTheSetIsAdaptive)
{
  FakeSink sink;
  std::vector<Range> ranges = {{STATE_A, STATE_A + 0x100}};
  std::vector<Range> parsed;
  std::string error;
  const char* const json = R"({"ranges":[{"start":"0x80500000","end":"0x80500100"}]})";
  EXPECT_FALSE(ApplyExclusions(json, &ranges, sink, &error, &parsed, true));  // classic: nothing would be left
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_TRUE(ApplyExclusions(json, &ranges, sink, &error, &parsed, false));
  EXPECT_TRUE(ranges.empty());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].start, STATE_A);
  EXPECT_EQ(parsed[0].end, STATE_A + 0x100);
}
