// Unit tests for Core/Slippi/Rev3NetSim (rollback netplay, milestone M4): REV3_NETSIM parsing and the seeded network simulator that Rev3Net.cpp
// puts between ENet and the session. All pure: no sockets, no clock (every call takes the time), no Dolphin core, no game data.
// Run: build target `tests`, then `tests --gtest_filter=Rev3NetSim*`.
//
// What matters here: a seed fixes the whole run (drops and release times), only PAD / ACK packets are ever dropped, reliable messages are
// late but never lost, jitter reorders like a real network, and nothing ever comes out before its release time.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Slippi/Rev3NetSim.h"

namespace
{
using namespace Rev3::Net;

NetSimConfig Cfg(u32 delay, u32 jitter, double loss, u64 seed)
{
  NetSimConfig c;
  c.delay_ms = delay;
  c.jitter_ms = jitter;
  c.loss_pct = loss;
  c.seed = seed;
  return c;
}

// What one instance of the simulator does with `count` messages arriving every `step_us`: (index, release time) of every surviving message in
// the order they leave, droppable ones marked by `droppable(i)`.
struct Trace
{
  std::vector<std::pair<u64, u64>> out;  // (tag, release time in us)
  std::vector<u64> dropped;              // tags
};

Trace Simulate(const NetSimConfig& config, u32 stream, int count, u64 step_us, bool all_droppable, int reliable_every = 0)
{
  NetSim sim(config, stream);
  Trace t;
  for (int i = 0; i < count; ++i)
  {
    const u64 now = static_cast<u64>(i) * step_us;
    const bool reliable = reliable_every != 0 && i % reliable_every == 0;
    const bool droppable = all_droppable && !reliable;
    if (!sim.Submit(now, droppable, static_cast<u64>(i), {static_cast<u8>(i)}))
      t.dropped.push_back(static_cast<u64>(i));
  }
  u64 tag = 0;
  std::vector<u8> bytes;
  // Pop in order of release by advancing a far-away clock one microsecond at a time would be slow; ask for the next due time instead.
  while (!sim.Empty())
  {
    const u64 due = sim.NextDueUs();
    EXPECT_TRUE(sim.PopDue(due, &tag, &bytes));
    t.out.emplace_back(tag, due);
  }
  return t;
}

// ---- parsing -----------------------------------------------------------------------------------------------------------------------

TEST(Rev3NetSim, ParsesAllKeysInAnyOrderCaseAndSpacing)
{
  NetSimConfig c;
  std::string error;
  ASSERT_TRUE(ParseNetSim("delay_ms=80,jitter_ms=30,loss_pct=5,seed=7", &c, &error)) << error;
  EXPECT_EQ(c.delay_ms, 80u);
  EXPECT_EQ(c.jitter_ms, 30u);
  EXPECT_EQ(c.loss_pct, 5.0);
  EXPECT_EQ(c.seed, 7u);
  EXPECT_TRUE(c.Active());
  ASSERT_TRUE(ParseNetSim("  SEED=99 , Loss_Pct=2.5,  delay_ms = 10  ", &c, &error)) << error;
  EXPECT_EQ(c.delay_ms, 10u);
  EXPECT_EQ(c.jitter_ms, 0u);
  EXPECT_EQ(c.loss_pct, 2.5);
  EXPECT_EQ(c.seed, 99u);
}

TEST(Rev3NetSim, UnsetKeysAreZeroAndTheSeedDefaultsToOne)
{
  NetSimConfig c;
  std::string error;
  ASSERT_TRUE(ParseNetSim("delay_ms=5", &c, &error));
  EXPECT_EQ(c.jitter_ms, 0u);
  EXPECT_EQ(c.loss_pct, 0.0);
  EXPECT_EQ(c.seed, 1u);
  ASSERT_TRUE(ParseNetSim("", &c, &error));
  EXPECT_FALSE(c.Active());
  EXPECT_EQ(c.seed, 1u);
  ASSERT_TRUE(ParseNetSim("seed=3", &c, &error));
  EXPECT_FALSE(c.Active());  // a seed alone simulates nothing
  ASSERT_TRUE(ParseNetSim("delay_ms=0,,", &c, &error));  // empty items are skipped
  EXPECT_FALSE(c.Active());
}

TEST(Rev3NetSim, RefusesWhatItDoesNotUnderstandWithAReason)
{
  NetSimConfig c;
  std::string error;
  for (const char* bad : {"delay=80", "delay_ms", "delay_ms=", "delay_ms=abc", "delay_ms=-5", "delay_ms=10001", "jitter_ms=1.5", "loss_pct=101",
                          "loss_pct=-1", "loss_pct=x", "loss_pct=", "seed=-1", "seed=abc", "delay_ms=80;jitter_ms=3", "rtt=100"})
  {
    error.clear();
    c.delay_ms = 123;
    EXPECT_FALSE(ParseNetSim(bad, &c, &error)) << bad;
    EXPECT_FALSE(error.empty()) << bad;
    EXPECT_EQ(c.delay_ms, 123u) << bad << ": a failed parse leaves the output alone";
  }
}

TEST(Rev3NetSim, TextIsWhatTheLogLineShows)
{
  EXPECT_EQ(NetSimText(Cfg(80, 30, 5, 1)), "delay_ms=80 jitter_ms=30 loss_pct=5 seed=1");
  EXPECT_EQ(NetSimText(Cfg(0, 0, 2.5, 18446744073709551615ull)), "delay_ms=0 jitter_ms=0 loss_pct=2.5 seed=18446744073709551615");
  NetSimConfig back;
  std::string error;
  std::string text = NetSimText(Cfg(80, 30, 5, 42));
  std::replace(text.begin(), text.end(), ' ', ',');
  ASSERT_TRUE(ParseNetSim(text, &back, &error)) << error;
  EXPECT_EQ(back.delay_ms, 80u);
  EXPECT_EQ(back.seed, 42u);
}

// ---- behaviour ---------------------------------------------------------------------------------------------------------------------

TEST(Rev3NetSim, AnInactiveSimulatorPassesEverythingStraightThroughInOrder)
{
  NetSim sim(Cfg(0, 0, 0, 1), 0);
  EXPECT_FALSE(sim.stats().active);
  u64 tag = 0;
  std::vector<u8> bytes;
  for (u64 i = 0; i < 10; ++i)
  {
    ASSERT_TRUE(sim.Submit(1000 + i, true, i, {static_cast<u8>(i), 9}));
    ASSERT_TRUE(sim.PopDue(1000 + i, &tag, &bytes));
    EXPECT_EQ(tag, i);
    EXPECT_EQ(bytes, (std::vector<u8>{static_cast<u8>(i), 9}));
  }
  EXPECT_TRUE(sim.Empty());
  EXPECT_EQ(sim.stats().held, 0u);
  EXPECT_EQ(sim.stats().dropped, 0u);
  EXPECT_EQ(sim.stats().released, 10u);
}

TEST(Rev3NetSim, ADelayHoldsEveryMessageForExactlyThatLong)
{
  NetSim sim(Cfg(80, 0, 0, 1), 0);
  EXPECT_TRUE(sim.stats().active);
  ASSERT_TRUE(sim.Submit(5000, true, 1, {1}));
  ASSERT_TRUE(sim.Submit(6000, false, 2, {2}));
  EXPECT_EQ(sim.Pending(), 2u);
  EXPECT_EQ(sim.NextDueUs(), 85000u);
  u64 tag = 0;
  std::vector<u8> bytes;
  EXPECT_FALSE(sim.PopDue(84999, &tag, &bytes));  // nothing early, not even by a microsecond
  ASSERT_TRUE(sim.PopDue(85000, &tag, &bytes));
  EXPECT_EQ(tag, 1u);
  EXPECT_FALSE(sim.PopDue(85999, &tag, &bytes));
  ASSERT_TRUE(sim.PopDue(86000, &tag, &bytes));
  EXPECT_EQ(tag, 2u);
  EXPECT_TRUE(sim.Empty());
  EXPECT_EQ(sim.NextDueUs(), 0u);
  EXPECT_EQ(sim.stats().held, 2u);
  EXPECT_EQ(sim.stats().released, 2u);
}

TEST(Rev3NetSim, JitterStaysWithinTheBandAndNeverGoesBelowZero)
{
  const Trace t = Simulate(Cfg(80, 30, 0, 5), 0, 2000, 10000, true);
  ASSERT_EQ(t.out.size(), 2000u);
  u64 lo = ~0ull, hi = 0;
  for (const auto& entry : t.out)
  {
    const u64 hold = entry.second - entry.first * 10000;
    lo = std::min(lo, hold);
    hi = std::max(hi, hold);
  }
  EXPECT_GE(lo, 50000u);
  EXPECT_LE(hi, 110000u);
  EXPECT_LT(lo, 55000u);  // the band is actually used, both ends
  EXPECT_GT(hi, 105000u);

  // Jitter larger than the delay: the hold time is clamped at 0, never negative (it would be a message from the future).
  const Trace wide = Simulate(Cfg(5, 50, 0, 5), 0, 2000, 10000, true);
  for (const auto& entry : wide.out)
    EXPECT_GE(entry.second, entry.first * 10000);
}

TEST(Rev3NetSim, JitterReordersMessagesLikeARealNetworkAndTiesKeepArrivalOrder)
{
  const Trace t = Simulate(Cfg(40, 35, 0, 11), 0, 500, 5000, true);
  size_t inversions = 0;
  for (size_t i = 1; i < t.out.size(); ++i)
    if (t.out[i].first < t.out[i - 1].first)
      ++inversions;
  EXPECT_GT(inversions, 20u);
  // Release times never go backwards in the order messages leave.
  for (size_t i = 1; i < t.out.size(); ++i)
    EXPECT_GE(t.out[i].second, t.out[i - 1].second);

  // Equal release times leave in arrival order.
  NetSim sim(Cfg(10, 0, 0, 1), 0);
  for (u64 i = 0; i < 5; ++i)
    sim.Submit(0, false, i, {});
  u64 tag = 0;
  std::vector<u8> bytes;
  for (u64 i = 0; i < 5; ++i)
  {
    ASSERT_TRUE(sim.PopDue(10000, &tag, &bytes));
    EXPECT_EQ(tag, i);
  }
}

TEST(Rev3NetSim, ASeedFixesTheWholeRunAndTheStreamsAndSeedsDiffer)
{
  const NetSimConfig c = Cfg(80, 30, 5, 1234);
  const Trace a = Simulate(c, 0, 3000, 8000, true, 7);
  const Trace b = Simulate(c, 0, 3000, 8000, true, 7);
  EXPECT_EQ(a.out, b.out);
  EXPECT_EQ(a.dropped, b.dropped);
  EXPECT_FALSE(a.dropped.empty());

  const Trace other_seed = Simulate(Cfg(80, 30, 5, 1235), 0, 3000, 8000, true, 7);
  EXPECT_NE(a.out, other_seed.out);
  EXPECT_NE(a.dropped, other_seed.dropped);

  const Trace other_stream = Simulate(c, 1, 3000, 8000, true, 7);  // the guest does not mirror the host
  EXPECT_NE(a.out, other_stream.out);
  EXPECT_NE(a.dropped, other_stream.dropped);

  // Seeds near each other (1, 2, 3) are not near-copies: the first drops differ.
  std::vector<std::vector<u64>> firsts;
  for (u64 seed = 1; seed <= 3; ++seed)
  {
    std::vector<u64> d = Simulate(Cfg(0, 0, 10, seed), 0, 400, 1000, true).dropped;
    d.resize(std::min<size_t>(d.size(), 10));
    firsts.push_back(d);
  }
  EXPECT_NE(firsts[0], firsts[1]);
  EXPECT_NE(firsts[1], firsts[2]);
}

TEST(Rev3NetSim, ReliableMessagesAreLateButNeverDropped)
{
  NetSim sim(Cfg(20, 10, 100, 3), 0);  // every droppable packet is lost
  for (u64 i = 0; i < 200; ++i)
  {
    EXPECT_TRUE(sim.Submit(i * 1000, false, i, {1}));   // reliable: delivered
    EXPECT_FALSE(sim.Submit(i * 1000, true, 1000 + i, {2}));  // PAD / ACK: gone
  }
  EXPECT_EQ(sim.Pending(), 200u);
  EXPECT_EQ(sim.stats().dropped, 200u);
  EXPECT_EQ(sim.stats().submitted, 400u);
  u64 tag = 0;
  std::vector<u8> bytes;
  u32 got = 0;
  while (sim.PopDue(~0ull, &tag, &bytes))
  {
    EXPECT_LT(tag, 1000u);
    ++got;
  }
  EXPECT_EQ(got, 200u);
}

TEST(Rev3NetSim, TheLossRateIsWhatWasAskedForAndZeroLosesNothing)
{
  for (double pct : {1.0, 5.0, 20.0})
  {
    const Trace t = Simulate(Cfg(0, 0, pct, 99), 0, 40000, 100, true);
    const double got = 100.0 * static_cast<double>(t.dropped.size()) / 40000.0;
    EXPECT_NEAR(got, pct, pct * 0.2 + 0.2) << pct;
    EXPECT_EQ(t.dropped.size() + t.out.size(), 40000u);
  }
  EXPECT_TRUE(Simulate(Cfg(30, 10, 0, 99), 0, 5000, 100, true).dropped.empty());
}

TEST(Rev3NetSim, WhichPacketsAreLostDoesNotDependOnJitterOrOnTheReliableTraffic)
{
  // The loss draws come from their own stream and only droppable packets consume them, so the same seed loses the same PAD packets whether
  // or not reliable messages are mixed in and whatever the jitter is. (That makes a run comparable with and without a jitter setting.)
  const Trace plain = Simulate(Cfg(40, 0, 7, 21), 0, 4000, 4000, true);
  const Trace jittery = Simulate(Cfg(40, 30, 7, 21), 0, 4000, 4000, true);
  EXPECT_EQ(plain.dropped, jittery.dropped);
  // With every 5th message reliable the droppable ones keep their pattern shifted by the interleaving: compare by position among droppables.
  const Trace mixed = Simulate(Cfg(40, 30, 7, 21), 0, 5000, 4000, true, 5);  // 4000 droppable, 1000 reliable
  std::vector<u64> positions;
  u64 index = 0;
  for (int i = 0; i < 5000; ++i)
  {
    if (i % 5 == 0)
      continue;
    if (std::find(mixed.dropped.begin(), mixed.dropped.end(), static_cast<u64>(i)) != mixed.dropped.end())
      positions.push_back(index);
    ++index;
  }
  EXPECT_EQ(positions, plain.dropped);
}

TEST(Rev3NetSim, StatsCountSubmittedDroppedHeldAndReleased)
{
  NetSim sim(Cfg(10, 0, 50, 8), 1);
  for (u64 i = 0; i < 1000; ++i)
    sim.Submit(i, true, i, {});
  const NetSimStats mid = sim.stats();
  EXPECT_TRUE(mid.active);
  EXPECT_EQ(mid.submitted, 1000u);
  EXPECT_EQ(mid.dropped + sim.Pending(), 1000u);
  EXPECT_EQ(mid.held, sim.Pending());  // everything that survived waits 10 ms
  EXPECT_EQ(mid.released, 0u);
  u64 tag = 0;
  std::vector<u8> bytes;
  while (sim.PopDue(~0ull, &tag, &bytes))
  {
  }
  EXPECT_EQ(sim.stats().released, mid.held);
  EXPECT_GT(mid.dropped, 350u);
  EXPECT_LT(mid.dropped, 650u);
}

TEST(Rev3NetSim, ThePayloadAndTagComeBackIntact)
{
  NetSim sim(Cfg(1, 0, 0, 1), 0);
  std::vector<u8> big(5000);
  for (size_t i = 0; i < big.size(); ++i)
    big[i] = static_cast<u8>(i * 7);
  ASSERT_TRUE(sim.Submit(0, false, 0xFEEDFACEull, big));
  u64 tag = 0;
  std::vector<u8> bytes;
  ASSERT_TRUE(sim.PopDue(1000, &tag, &bytes));
  EXPECT_EQ(tag, 0xFEEDFACEull);
  EXPECT_EQ(bytes, big);
}
}  // namespace
