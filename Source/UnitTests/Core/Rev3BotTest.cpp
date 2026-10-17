// Unit tests for Core/Slippi/Rev3Bot (unattended soak testing): the seeded bot pad, the menu presser, the takeover rule, the automatic exit and the
// option parsing. All pure: no Dolphin core, no clock, no game data. Run: build target `tests`, then `tests --gtest_filter=Rev3Bot*`.
//
// What matters here: the bot's pad is a function of (seed, port, frame) only (asked twice or in another order, it is the same), everything it
// plays lies inside the 8 bytes Rev3 reads with err = 0, it looks like a person (direction holds of 8-40 frames, bursts, idle, supers), outside a
// fight it only presses A / Start, and the exit controller ends a match on the results screen but not on a loading screen or a mispredicted KO.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Slippi/Rev3Bot.h"

namespace
{
using namespace Rev3::Bot;

constexpr u16 ALLOWED_FIGHT_BUTTONS = BTN_A | BTN_B | BTN_X | BTN_Y | BTN_Z | BTN_L | BTN_R;  // never Start in a fight: it pauses

u16 Buttons(const BotPad& p)
{
  return static_cast<u16>((p[0] << 8) | p[1]);
}

std::vector<BotPad> Pads(u64 seed, u32 port, u32 from, u32 count)
{
  PadGenerator gen(seed, port);
  std::vector<BotPad> out;
  for (u32 i = 0; i < count; ++i)
    out.push_back(gen.FightPad(from + i));
  return out;
}

struct Lines
{
  std::vector<std::string> lines;
  LogFn Fn() { return [this](const std::string& s) { lines.push_back(s); }; }
  size_t Count(const std::string& needle) const
  {
    size_t n = 0;
    for (const std::string& l : lines)
      n += l.find(needle) != std::string::npos ? 1 : 0;
    return n;
  }
};
}  // namespace

// ---- the generator ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Bot, SameSeedPortAndFrameGiveTheSamePad)
{
  EXPECT_EQ(Pads(7, 0, 0, 5000), Pads(7, 0, 0, 5000));
  EXPECT_NE(Pads(7, 0, 0, 5000), Pads(8, 0, 0, 5000));  // another seed
  EXPECT_NE(Pads(7, 0, 0, 5000), Pads(7, 1, 0, 5000));  // the other side of the same run
}

TEST(Rev3Bot, AFrameAskedTwiceOrOutOfOrderIsTheSame)
{
  const std::vector<BotPad> in_order = Pads(42, 1, 0, 3000);
  PadGenerator gen(42, 1);
  // backwards, then a stride: a resimulated frame is asked again long after later frames were
  for (u32 f = 3000; f-- > 0;)
    ASSERT_EQ(gen.FightPad(f), in_order[f]) << f;
  for (u32 f = 0; f < 3000; f += 37)
    ASSERT_EQ(gen.FightPad(f), in_order[f]) << f;
  PadGenerator late(42, 1);  // the first question is a late frame
  EXPECT_EQ(late.FightPad(2999), in_order[2999]);
  EXPECT_EQ(late.FightPad(0), in_order[0]);
}

TEST(Rev3Bot, PadsStayInsideWhatRev3ReadsAndErrIsZero)
{
  std::set<u16> seen;
  for (const u64 seed : {1ull, 2ull, 99ull})
  {
    for (const BotPad& p : Pads(seed, seed % 2, 0, 20000))
    {
      const u16 b = Buttons(p);
      seen.insert(b);
      EXPECT_EQ(b & ~ALLOWED_FIGHT_BUTTONS, 0) << "button bits outside the allowed set: " << std::hex << b;
      const int sx = static_cast<std::int8_t>(p[2]);
      const int sy = static_cast<std::int8_t>(p[3]);
      EXPECT_LE(std::abs(sx), STICK_FULL);
      EXPECT_LE(std::abs(sy), STICK_FULL);
      EXPECT_EQ(p[4], 0);  // c-stick unused
      EXPECT_EQ(p[5], 0);
      EXPECT_TRUE(p[6] == 0 || p[6] == TRIGGER_FULL);
      EXPECT_TRUE(p[7] == 0 || p[7] == TRIGGER_FULL);
      EXPECT_EQ(p[6] != 0, (b & BTN_L) != 0);  // the analog trigger goes with its digital click
      EXPECT_EQ(p[7] != 0, (b & BTN_R) != 0);
      EXPECT_EQ(p[8], 0);
      EXPECT_EQ(p[9], 0);
      EXPECT_EQ(p[10], 0);  // err = 0: a good read
      EXPECT_EQ(p[11], 0);
    }
  }
  // every button the bot is meant to press shows up in a long run
  u16 all = 0;
  for (const u16 b : seen)
    all |= b;
  for (const u16 want : {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_Z, BTN_L, BTN_R})
    EXPECT_NE(all & want, 0) << "never pressed " << std::hex << want;
}

TEST(Rev3Bot, HeldDirectionsLastEightToFortyFrames)
{
  int moves = 0;
  for (u32 index = 0; index < 4000; ++index)
  {
    const Segment seg = BuildSegment(5, 0, index);
    if (seg.kind != SegmentKind::Move)
      continue;
    ++moves;
    ASSERT_EQ(seg.steps.size(), 1u);
    EXPECT_GE(seg.steps[0].frames, MOVE_MIN_FRAMES);
    EXPECT_LE(seg.steps[0].frames, MOVE_MAX_FRAMES);
    EXPECT_TRUE(seg.steps[0].stick_x != 0 || seg.steps[0].stick_y != 0);
    EXPECT_EQ(seg.steps[0].buttons, 0);
  }
  EXPECT_GT(moves, 1000);  // the most common kind (30 %)
}

TEST(Rev3Bot, EverySegmentKindOccursWithRoughlyItsWeight)
{
  std::array<int, static_cast<size_t>(SegmentKind::Count)> count{};
  const int n = 20000;
  for (int index = 0; index < n; ++index)
    ++count[static_cast<size_t>(BuildSegment(11, 1, static_cast<u32>(index)).kind)];
  u32 total = 0;
  for (size_t k = 0; k < count.size(); ++k)
  {
    total += SEGMENT_WEIGHTS[k];
    const double want = SEGMENT_WEIGHTS[k] / 100.0 * n;
    EXPECT_GT(count[k], want * 0.8) << SegmentKindName(static_cast<SegmentKind>(k));
    EXPECT_LT(count[k], want * 1.2) << SegmentKindName(static_cast<SegmentKind>(k));
  }
  EXPECT_EQ(total, 100u);
}

TEST(Rev3Bot, FightPadLooksLikeAPerson)
{
  const std::vector<BotPad> pads = Pads(3, 0, 0, 12000);  // 200 s
  int idle_frames = 0;
  int press_starts = 0;
  int direction_changes = 0;
  for (size_t i = 0; i < pads.size(); ++i)
  {
    const BotPad& p = pads[i];
    if (Buttons(p) == 0 && p[2] == 0 && p[3] == 0)
      ++idle_frames;
    if (i > 0)
    {
      if (Buttons(p) != 0 && Buttons(pads[i - 1]) == 0)
        ++press_starts;
      if (p[2] != pads[i - 1][2] || p[3] != pads[i - 1][3])
        ++direction_changes;
    }
  }
  EXPECT_GT(idle_frames, 600);                     // it stands still now and then ...
  EXPECT_LT(idle_frames, static_cast<int>(pads.size()) / 2);  // ... but is mostly doing something
  EXPECT_GT(press_starts, 300);                    // presses come in bursts all along
  EXPECT_GT(direction_changes, 500);
  // no run of more than a few seconds with the same pad: a held direction ends within MOVE_MAX_FRAMES (a move can follow another with the same stick)
  size_t run = 1;
  size_t longest = 1;
  for (size_t i = 1; i < pads.size(); ++i)
  {
    run = pads[i] == pads[i - 1] ? run + 1 : 1;
    longest = std::max(longest, run);
  }
  EXPECT_LE(longest, 3u * MOVE_MAX_FRAMES);
}

TEST(Rev3Bot, SupersAreMotionsIntoX)
{
  bool found_quarter = false;
  bool found_down_x = false;
  for (u32 index = 0; index < 20000 && !(found_quarter && found_down_x); ++index)
  {
    const Segment seg = BuildSegment(9, 0, index);
    if (seg.kind != SegmentKind::Super)
      continue;
    if (seg.steps.size() == 5)  // down, down-forward, forward, forward + X, rest
    {
      found_quarter = true;
      EXPECT_EQ(seg.steps[0].stick_x, 0);
      EXPECT_LT(seg.steps[0].stick_y, 0);
      EXPECT_NE(seg.steps[1].stick_x, 0);
      EXPECT_LT(seg.steps[1].stick_y, 0);
      EXPECT_NE(seg.steps[2].stick_x, 0);
      EXPECT_EQ(seg.steps[2].stick_y, 0);
      EXPECT_EQ(seg.steps[3].buttons, BTN_X);
      EXPECT_EQ(seg.steps[3].stick_x, seg.steps[2].stick_x);
    }
    else  // 2X
    {
      found_down_x = true;
      ASSERT_EQ(seg.steps.size(), 2u);
      EXPECT_EQ(seg.steps[0].buttons, BTN_X);
      EXPECT_LT(seg.steps[0].stick_y, 0);
    }
  }
  EXPECT_TRUE(found_quarter);
  EXPECT_TRUE(found_down_x);
}

TEST(Rev3Bot, KindAtMatchesThePadItProduces)
{
  PadGenerator gen(21, 0);
  int chakra_frames = 0;
  for (u32 f = 0; f < 30000; ++f)
  {
    if (gen.KindAt(f) == SegmentKind::Chakra)
    {
      ++chakra_frames;
      EXPECT_EQ(Buttons(gen.FightPad(f)), BTN_Z);
    }
  }
  EXPECT_GT(chakra_frames, 100);
}

TEST(Rev3Bot, FramesPastTheTableLimitGetANeutralPad)
{
  PadGenerator gen(1, 0);
  EXPECT_EQ(gen.FightPad(GENERATOR_MAX_FRAME), BotPad{});
  EXPECT_EQ(gen.FightPad(0xFFFFFFFFu), BotPad{});
}

// ---- menus -----------------------------------------------------------------------------------------------------------------------

TEST(Rev3Bot, MenuPadPressesOnlyAOrStartInShortPulses)
{
  PadGenerator gen(4, 0);
  std::map<u16, int> frames;
  int pulses = 0;
  int start_pulses = 0;
  int run = 0;
  for (u32 f = 0; f < MENU_PERIOD_FRAMES * 60; ++f)
  {
    const BotPad p = gen.MenuPad(f);
    const u16 b = Buttons(p);
    ++frames[b];
    EXPECT_TRUE(b == 0 || b == BTN_A || b == BTN_START) << std::hex << b;
    EXPECT_EQ(p[2], 0);  // the stick stays at rest
    EXPECT_EQ(p[3], 0);
    EXPECT_EQ(p[10], 0);
    if (b != 0)
    {
      ++run;
    }
    else if (run != 0)
    {
      EXPECT_EQ(run, static_cast<int>(MENU_PRESS_FRAMES));
      ++pulses;
      run = 0;
    }
    if (b == BTN_START && (f == 0 || Buttons(gen.MenuPad(f - 1)) != BTN_START))
      ++start_pulses;
  }
  EXPECT_GE(pulses, 58);
  EXPECT_EQ(start_pulses, 60 / static_cast<int>(MENU_START_EVERY));
  EXPECT_GT(frames[BTN_A], frames[BTN_START]);
  EXPECT_EQ(frames[BTN_A] + frames[BTN_START] + frames[0], static_cast<int>(MENU_PERIOD_FRAMES * 60));
}

TEST(Rev3Bot, MenuPadIsAPureFunctionOfSeedPortFrame)
{
  PadGenerator a(4, 0);
  PadGenerator b(4, 0);
  PadGenerator other_port(4, 1);
  bool differs = false;
  for (u32 f = 0; f < 5000; ++f)
  {
    ASSERT_EQ(a.MenuPad(f), b.MenuPad(f));
    differs = differs || a.MenuPad(f) != other_port.MenuPad(f);
  }
  EXPECT_TRUE(differs);
}

// ---- the controller --------------------------------------------------------------------------------------------------------------

TEST(Rev3Bot, ScriptDrivesMenusUntilFightersAreFound)
{
  Lines log;
  PadBotController bot(1, 0, log.Fn());
  GameObs menu;
  BotPad pad{};
  for (u32 f = 0; f < 100; ++f)
    EXPECT_FALSE(bot.Choose(f, /*script_active=*/true, menu, &pad));  // the script's pad stays
  EXPECT_FALSE(bot.FightFound());
  EXPECT_EQ(log.Count("BOT takeover"), 0u);
}

TEST(Rev3Bot, FightersFoundTakeOverEvenIfTheScriptIsStillRunning)
{
  Lines log;
  PadBotController bot(1, 0, log.Fn());
  GameObs fight;
  fight.fighters_found = true;
  BotPad pad{};
  ASSERT_TRUE(bot.Choose(500, true, fight, &pad));
  EXPECT_TRUE(bot.FightFound());
  EXPECT_EQ(bot.FightFrame(), 500u);
  EXPECT_EQ(pad, bot.generator().FightPad(500));
  EXPECT_EQ(log.Count("BOT fight found frame=500"), 1u);
  EXPECT_EQ(log.Count("BOT takeover frame=500 why=fighters found"), 1u);
  // and it keeps the pad although the script has frames left
  GameObs menu;
  ASSERT_TRUE(bot.Choose(501, true, menu, &pad));
  EXPECT_EQ(pad, bot.generator().MenuPad(501));
}

TEST(Rev3Bot, PastTheScriptWithoutFightersItPressesMenuButtons)
{
  Lines log;
  PadBotController bot(2, 1, log.Fn());
  GameObs menu;
  BotPad pad{};
  EXPECT_FALSE(bot.Choose(99, true, menu, &pad));
  ASSERT_TRUE(bot.Choose(100, false, menu, &pad));  // the script ended
  EXPECT_EQ(pad, bot.generator().MenuPad(100));
  EXPECT_EQ(log.Count("BOT takeover frame=100 why=script ended or no script"), 1u);
  EXPECT_EQ(log.Count("BOT mode=menu frame=100"), 1u);
  ASSERT_TRUE(bot.Choose(101, false, menu, &pad));
  EXPECT_EQ(log.Count("BOT mode=menu"), 1u);  // logged on a change only
}

TEST(Rev3Bot, WithoutAScriptTheBotStartsAtOnceAndFollowsTheScreen)
{
  Lines log;
  PadBotController bot(2, 0, log.Fn());
  GameObs menu;
  GameObs fight;
  fight.fighters_found = true;
  BotPad pad{};
  ASSERT_TRUE(bot.Choose(0, false, menu, &pad));
  EXPECT_EQ(pad, bot.generator().MenuPad(0));
  ASSERT_TRUE(bot.Choose(1000, false, fight, &pad));
  EXPECT_EQ(pad, bot.generator().FightPad(1000));
  EXPECT_TRUE(bot.InFight());
  ASSERT_TRUE(bot.Choose(5000, false, menu, &pad));  // results screen: back to A / Start
  EXPECT_EQ(pad, bot.generator().MenuPad(5000));
  EXPECT_FALSE(bot.InFight());
  EXPECT_EQ(log.Count("BOT mode=fight"), 1u);
  EXPECT_EQ(log.Count("BOT mode=menu"), 2u);
}

// ---- the exit --------------------------------------------------------------------------------------------------------------------

TEST(Rev3Bot, ExitSpecParsing)
{
  ExitSpec s;
  std::string error;
  ASSERT_TRUE(ParseExitSpec("", &s, &error));
  EXPECT_FALSE(s.enabled);
  ASSERT_TRUE(ParseExitSpec(" Match ", &s, &error));
  EXPECT_TRUE(s.enabled);
  EXPECT_EQ(s.kind, ExitKind::Match);
  ASSERT_TRUE(ParseExitSpec("frames:3600", &s, &error));
  EXPECT_EQ(s.kind, ExitKind::Frames);
  EXPECT_EQ(s.n, 3600u);
  ASSERT_TRUE(ParseExitSpec("Rounds: 3", &s, &error));
  EXPECT_EQ(s.kind, ExitKind::Rounds);
  EXPECT_EQ(s.n, 3u);
  EXPECT_EQ(ExitSpecText(s), "rounds:3");
  for (const char* bad : {"frames", "frames:", "frames:0", "frames:x", "rounds:100", "rounds:-1", "minutes:5", "mach", "match:2"})
  {
    EXPECT_FALSE(ParseExitSpec(bad, &s, &error)) << bad;
    EXPECT_NE(error.find("REV3_NET_EXIT="), std::string::npos) << bad;
    EXPECT_FALSE(s.enabled) << bad;
  }
}

namespace
{
// A pretend match: feeds the controller one observation per call and returns the call number (1-based) at which it asked to stop (0 = never).
struct Match
{
  Match(const ExitSpec& spec, ExitTuning tuning) : controller(spec, tuning, log.Fn()) {}
  void Feed(u32 calls, const GameObs& obs)
  {
    for (u32 i = 0; i < calls && stop_call == 0; ++i)
    {
      ++call;
      const ExitStep step = controller.OnCall(call, obs);
      if (step.stop)
      {
        stop_call = call;
        reason = step.reason;
      }
    }
  }
  Lines log;
  ExitController controller;
  u32 call = 0;
  u32 stop_call = 0;
  std::string reason;
};

ExitTuning SmallTuning()
{
  ExitTuning t;
  t.wait_calls = 100;
  t.ko_debounce_calls = 10;
  t.scene_away_calls = 20;
  t.lost_calls = 50;
  t.idle_after_ko_calls = 400;
  t.min_rounds_for_scene_rule = 2;
  return t;
}

GameObs Obs(bool fighters, bool zero, u32 scene = 2)
{
  GameObs o;
  o.fighters_found = fighters;
  o.health_zero = zero;
  o.scene_valid = true;
  o.scene_type = scene;
  return o;
}

ExitSpec Spec(ExitKind kind, u32 n = 0)
{
  ExitSpec s;
  s.enabled = true;
  s.kind = kind;
  s.n = n;
  return s;
}
}  // namespace

TEST(Rev3Bot, MatchEndsOnTheResultsScreenAfterTwoKos)
{
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(30, Obs(false, false, 3));    // menus: nothing watched yet
  m.Feed(200, Obs(true, false));       // round 1
  m.Feed(30, Obs(true, true));         // KO 1
  EXPECT_EQ(m.controller.Kos(), 1u);
  m.Feed(60, Obs(true, false));        // round 2 starts
  m.Feed(30, Obs(true, true));         // KO 2
  EXPECT_EQ(m.controller.Kos(), 2u);
  EXPECT_FALSE(m.controller.Decided());
  const u32 ko2 = m.call;
  m.Feed(15, Obs(true, true));          // the KO animation: same scene, not over
  EXPECT_FALSE(m.controller.Decided());
  m.Feed(400, Obs(false, true, 3));    // results screen: scene 3, no fighters
  ASSERT_TRUE(m.controller.Decided());
  EXPECT_GT(m.stop_call, ko2);
  EXPECT_NE(m.reason.find("match: the scene changed (2 -> 3) after 2 KOs"), std::string::npos) << m.reason;
  // decision after scene_away_calls (20), stop wait_calls (100) later
  EXPECT_EQ(m.stop_call, ko2 + 15 + 20 + 100);
  EXPECT_EQ(m.log.Count("EXIT planned"), 1u);
  EXPECT_EQ(m.log.Count("EXIT ko="), 2u);
}

TEST(Rev3Bot, OneKoDoesNotEndAMatchBySceneOrLostFighters)
{
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(100, Obs(true, false));
  m.Feed(20, Obs(true, true));  // KO 1
  m.Feed(100, Obs(false, true, 3));  // a transition screen between rounds (scene differs for a while)
  EXPECT_FALSE(m.controller.Decided());
  m.Feed(100, Obs(true, false));  // round 2 comes
  EXPECT_FALSE(m.controller.Decided());
  EXPECT_EQ(m.stop_call, 0u);
}

TEST(Rev3Bot, FightersLostAfterTwoKosEndTheMatchEvenWithoutASceneRead)
{
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(50, Obs(true, false));
  m.Feed(20, Obs(true, true));
  m.Feed(50, Obs(true, false));
  m.Feed(20, Obs(true, true));
  GameObs gone = Obs(false, false);
  gone.scene_valid = false;
  m.Feed(500, gone);
  ASSERT_TRUE(m.controller.Decided());
  EXPECT_NE(m.reason.find("match: no fighters for 50 calls after 2 KOs"), std::string::npos) << m.reason;
}

TEST(Rev3Bot, NoNewRoundLongAfterAKoEndsAMatchWhoseScreenIsNotRecognised)
{
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(50, Obs(true, false));
  m.Feed(20, Obs(true, true));  // KO 1, and then nothing but zeros: a stale struct on a results screen of the same scene type
  m.Feed(1000, Obs(true, true));
  ASSERT_TRUE(m.controller.Decided());
  EXPECT_NE(m.reason.find("match: no new round"), std::string::npos) << m.reason;
}

TEST(Rev3Bot, ZeroHealthWhileLoadingAndMispredictedKosAreNotKos)
{
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(100, Obs(true, true));   // fighters exist at health 0 while the stage loads: both never healthy yet
  EXPECT_EQ(m.controller.Kos(), 0u);
  m.Feed(50, Obs(true, false));
  for (int i = 0; i < 5; ++i)     // a predicted hit that is rolled back again: a few frames of zero, then full health
  {
    m.Feed(6, Obs(true, true));
    m.Feed(20, Obs(true, false));
  }
  EXPECT_EQ(m.controller.Kos(), 0u);
  m.Feed(10, Obs(true, true));    // a real one
  EXPECT_EQ(m.controller.Kos(), 1u);
}

TEST(Rev3Bot, RoundsAndFramesModes)
{
  {
    Match m(Spec(ExitKind::Rounds, 2), SmallTuning());
    m.Feed(40, Obs(true, false));
    m.Feed(10, Obs(true, true));
    EXPECT_FALSE(m.controller.Decided());
    m.Feed(40, Obs(true, false));
    m.Feed(10, Obs(true, true));
    ASSERT_TRUE(m.controller.Decided());
    EXPECT_EQ(m.log.Count("EXIT planned reason=\"rounds:2\""), 1u);
    m.Feed(100, Obs(true, true));
    EXPECT_EQ(m.reason, "rounds:2");
    EXPECT_NE(m.stop_call, 0u);
    EXPECT_EQ(m.stop_call, 100u + 100u);  // 100 calls of play, then the 100-call wait
  }
  {
    Match m(Spec(ExitKind::Frames, 300), SmallTuning());
    m.Feed(50, Obs(false, false, 3));  // menus do not count
    m.Feed(299, Obs(true, false));
    EXPECT_FALSE(m.controller.Decided());
    m.Feed(1, Obs(true, false));
    EXPECT_TRUE(m.controller.Decided());
    EXPECT_EQ(m.log.Count("EXIT planned reason=\"frames:300\""), 1u);
    m.Feed(200, Obs(true, false));
    EXPECT_EQ(m.reason, "frames:300");
    EXPECT_EQ(m.stop_call, 50u + 300u + 100u);
    EXPECT_EQ(m.controller.FightCalls(), 400u);
  }
}

TEST(Rev3Bot, StopIsRequestedOnceAndADisabledSpecNeverStops)
{
  Match m(Spec(ExitKind::Frames, 5), SmallTuning());
  m.Feed(10, Obs(true, false));
  m.Feed(200, Obs(true, false));
  EXPECT_EQ(m.stop_call, 105u);
  // after the request the controller is quiet (a stop that takes seconds must not request again)
  for (int i = 0; i < 500; ++i)
    EXPECT_FALSE(m.controller.OnCall(1000u + static_cast<u32>(i), Obs(true, false)).stop);
  Match off(ExitSpec(), SmallTuning());
  off.Feed(5000, Obs(true, true));
  EXPECT_EQ(off.stop_call, 0u);
  EXPECT_FALSE(off.controller.FightSeen());
}

TEST(Rev3Bot, ZeroWaitStopsOnTheCallThatDecides)
{
  ExitTuning t = SmallTuning();
  t.wait_calls = 0;
  Match m(Spec(ExitKind::Frames, 10), t);
  m.Feed(100, Obs(true, false));
  EXPECT_EQ(m.stop_call, 10u);
}

TEST(Rev3Bot, AnUnplannedLeaveStopsTheRunAfterTheWaitAndSaysSo)
{
  // D's soak, Oct 16 (run_2 / run_6): the session left at the start of round 2 and the run carried on offline until the runner killed it.
  Match m(Spec(ExitKind::Match), SmallTuning());
  m.Feed(200, Obs(true, false));
  m.Feed(30, Obs(true, true));  // KO 1
  m.Feed(40, Obs(true, false));
  const u32 leave_call = m.call;
  EXPECT_TRUE(m.controller.NoteLeave(m.call, "no remote pad for 10000 ms (frame 8851, R=8850)"));
  EXPECT_TRUE(m.controller.Decided());
  EXPECT_EQ(m.log.Count("EXIT unplanned leave"), 1u);
  EXPECT_EQ(m.log.Count("EXIT planned"), 0u);
  bool left = false;
  for (u32 i = 0; i < 300 && m.stop_call == 0; ++i)
  {
    ++m.call;
    const ExitStep step = m.controller.OnCall(m.call, Obs(true, false));
    if (step.stop)
    {
      m.stop_call = m.call;
      m.reason = step.reason;
      left = step.left;
    }
  }
  EXPECT_EQ(m.stop_call, leave_call + 100);  // wait_calls later
  EXPECT_TRUE(left);
  EXPECT_NE(m.reason.find("net session left: no remote pad for 10000 ms"), std::string::npos) << m.reason;
}

TEST(Rev3Bot, ALeaveAfterAPlannedDecisionOrWithTheExitOffChangesNothing)
{
  Match m(Spec(ExitKind::Frames, 5), SmallTuning());
  m.Feed(5, Obs(true, false));
  ASSERT_TRUE(m.controller.Decided());
  EXPECT_FALSE(m.controller.NoteLeave(m.call, "connection lost"));  // the planned exit is under way: the same stop, not a PROBLEM
  m.Feed(200, Obs(true, false));
  EXPECT_EQ(m.stop_call, 5u + 100u);
  Lines log;
  ExitSpec off;
  ExitController disabled(off, SmallTuning(), log.Fn());
  EXPECT_FALSE(disabled.NoteLeave(1, "connection lost"));
  EXPECT_EQ(log.Count("EXIT"), 0u);
}

TEST(Rev3Bot, OnlyThePeersPlannedExitOrItsClosedEmulatorIsAPlannedLeave)
{
  EXPECT_TRUE(IsPlannedLeaveReason("the peer left (planned exit: match: the scene changed (2 -> 3) after 2 KOs)"));
  EXPECT_TRUE(IsPlannedLeaveReason("the peer left (the emulator was closed)"));
  EXPECT_FALSE(IsPlannedLeaveReason("the peer left (no remote pad for 10000 ms (frame 8851, R=8850))"));
  EXPECT_FALSE(IsPlannedLeaveReason("no remote pad for 10000 ms (frame 8851, R=8850)"));
  EXPECT_FALSE(IsPlannedLeaveReason("connection lost"));
  EXPECT_FALSE(IsPlannedLeaveReason("desync (START_CHECK mismatch)"));
  EXPECT_FALSE(IsPlannedLeaveReason(""));
  EXPECT_EQ(EXIT_CODE_LEFT, 73);
  for (const int code : {EXIT_CODE_FREEZE, EXIT_CODE_MAX_S, EXIT_CODE_STOP_HANG})
    EXPECT_NE(EXIT_CODE_LEFT, code);
}

// ---- host-side exits -------------------------------------------------------------------------------------------------------------

TEST(Rev3Bot, FreezeExitWaitsForTheStallLimitPlusItsSecondsAndIgnoresDeliberateWaits)
{
  EXPECT_FALSE(FreezeExitDue(1000000, true, false, 2000, 0));          // off
  EXPECT_FALSE(FreezeExitDue(5000, true, false, 2000, 10));            // 5 s of silence: the dump fired at 2 s, 10 s more are wanted
  EXPECT_FALSE(FreezeExitDue(11999, true, false, 2000, 10));
  EXPECT_TRUE(FreezeExitDue(12000, true, false, 2000, 10));
  EXPECT_FALSE(FreezeExitDue(60000, true, true, 2000, 10));            // the net session waits for the peer on purpose
  EXPECT_FALSE(FreezeExitDue(60000, false, false, 2000, 10));          // no hook command ever arrived (the game is still booting)
}

TEST(Rev3Bot, MaxSecondsAndStopHang)
{
  EXPECT_FALSE(MaxSecondsDue(10000000, 0));
  EXPECT_FALSE(MaxSecondsDue(59999, 60));
  EXPECT_TRUE(MaxSecondsDue(60000, 60));
  EXPECT_FALSE(StopHangDue(STOP_GRACE_MS - 1));
  EXPECT_TRUE(StopHangDue(STOP_GRACE_MS));
  EXPECT_NE(EXIT_CODE_FREEZE, EXIT_CODE_MAX_S);
  EXPECT_NE(EXIT_CODE_FREEZE, EXIT_CODE_STOP_HANG);
  EXPECT_NE(EXIT_CODE_MAX_S, EXIT_CODE_STOP_HANG);
  for (const int code : {EXIT_CODE_FREEZE, EXIT_CODE_MAX_S, EXIT_CODE_STOP_HANG, EXIT_CODE_LEFT})
    EXPECT_GT(code, 1);  // not 0 (clean) and not 1 (Dolphin's own generic failure)
}

// ---- options ---------------------------------------------------------------------------------------------------------------------

namespace
{
BotOptions Parse(const std::map<std::string, std::string>& env)
{
  return ParseBotOptions([&env](const char* name) {
    const auto it = env.find(name);
    return it == env.end() ? std::string() : it->second;
  });
}
}  // namespace

TEST(Rev3Bot, OptionsDefaultToEverythingOff)
{
  const BotOptions o = Parse({});
  EXPECT_FALSE(o.Any());
  EXPECT_FALSE(o.bot);
  EXPECT_FALSE(o.exit.enabled);
  EXPECT_EQ(o.freeze_exit_s, 0u);
  EXPECT_EQ(o.max_s, 0u);
  EXPECT_EQ(o.tuning.wait_calls, EXIT_WAIT_DEFAULT_S * EXIT_CALLS_PER_SECOND);
  EXPECT_TRUE(o.warnings.empty());
}

TEST(Rev3Bot, OptionsAreReadFromTheEnvironment)
{
  const BotOptions o = Parse({{"REV3_PAD_BOT", "0x10"},
                              {"REV3_NET_EXIT", "rounds:3"},
                              {"REV3_NET_EXIT_WAIT_S", "2"},
                              {"REV3_NET_FREEZE_EXIT_S", "30"},
                              {"REV3_NET_MAX_S", "900"}});
  EXPECT_TRUE(o.Any());
  EXPECT_TRUE(o.bot);
  EXPECT_EQ(o.seed, 16u);
  EXPECT_EQ(o.exit.kind, ExitKind::Rounds);
  EXPECT_EQ(o.exit.n, 3u);
  EXPECT_EQ(o.tuning.wait_calls, 120u);
  EXPECT_EQ(o.freeze_exit_s, 30u);
  EXPECT_EQ(o.max_s, 900u);
  EXPECT_TRUE(o.warnings.empty());
  EXPECT_EQ(Parse({{"REV3_PAD_BOT", "123456789012"}}).seed, 123456789012ull);
  EXPECT_EQ(Parse({{"REV3_PAD_BOT", "0"}}).bot, true);  // seed 0 is a seed
}

TEST(Rev3Bot, BadValuesAreWarningsAndThatPartStaysOff)
{
  const BotOptions o = Parse({{"REV3_PAD_BOT", "abc"},
                              {"REV3_NET_EXIT", "soon"},
                              {"REV3_NET_EXIT_WAIT_S", "-1"},
                              {"REV3_NET_FREEZE_EXIT_S", "1e3"},
                              {"REV3_NET_MAX_S", "99999999"}});
  EXPECT_FALSE(o.bot);
  EXPECT_FALSE(o.exit.enabled);
  EXPECT_EQ(o.freeze_exit_s, 0u);
  EXPECT_EQ(o.max_s, 0u);
  EXPECT_EQ(o.tuning.wait_calls, EXIT_WAIT_DEFAULT_S * EXIT_CALLS_PER_SECOND);
  EXPECT_EQ(o.warnings.size(), 5u);
  EXPECT_FALSE(o.Any());
}
