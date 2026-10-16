// Rev3 netplay (milestone M4): REV3_NETSIM, a seeded network simulator for the real transport. Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT DOES
//   Holds every message the transport receives until  now + delay +- jitter  (a seeded random draw per message), and drops a percentage of
//   the PAD / PAD_ACK packets (the unsequenced ones; reliable messages are delivered late but never dropped, the way ENet's own
//   retransmission would eventually deliver them). Both instances of a loopback run apply it to what THEY receive, so the round trip is
//   2 x delay. The ENet connect handshake is not a message of this layer and is never touched.
//   REV3_NETSIM=delay_ms=80,jitter_ms=30,loss_pct=5,seed=1   (any subset, any order; unset keys are 0, seed defaults to 1)
//
// HOW IT FITS IN
//   Pure logic, no sockets and no clock of its own (the caller passes `now_us`), so Rev3NetSimTest.cpp checks the determinism per seed.
//   Rev3Net.cpp's EnetTransport owns one NetSim on its network thread: Submit() for every received PAD / ACK / reliable packet, PopDue()
//   in the service loop, and NextDueUs() to shorten the ENet service wait. Rev3Net.cpp logs `NET netsim delay_ms=.. jitter_ms=.. loss_pct=..
//   seed=..` at start and the counters go into SUMMARY (Transport::NetSimCounts).
//
// DETERMINISM
//   The jitter and loss draws come from two splitmix64 streams seeded from (seed, stream); `stream` is 0 for the host and 1 for the guest so
//   the two instances of one run do not mirror each other's pattern. The same seed and the same sequence of Submit() calls give the same
//   drops and release times on every platform (no std:: distributions: their output is implementation defined).
//   Messages leave in order of release time (ties: arrival order), so jitter reorders them like a real network.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Rev3::Net
{
struct NetSimConfig
{
  u32 delay_ms = 0;     // one way hold time of every received message (milliseconds)
  u32 jitter_ms = 0;    // each message's hold time is delay +- a uniform draw in [-jitter, +jitter] (never below 0)
  double loss_pct = 0;  // percent (0..100) of PAD / PAD_ACK packets dropped on receipt
  u64 seed = 1;         // seeds both random streams

  bool Active() const { return delay_ms != 0 || jitter_ms != 0 || loss_pct > 0; }
};

constexpr u32 NETSIM_MAX_MS = 10000;  // largest delay / jitter accepted

// "delay_ms=80,jitter_ms=30,loss_pct=5,seed=1": keys in any order and case, whitespace ignored. An empty text is an inactive config.
// False with *error on an unknown key, a malformed or out-of-range value.
bool ParseNetSim(const std::string& text, NetSimConfig* out, std::string* error);
// "delay_ms=80 jitter_ms=30 loss_pct=5 seed=1" (the text after `NET netsim ` in the log).
std::string NetSimText(const NetSimConfig& config);

struct NetSimStats
{
  bool active = false;
  u32 submitted = 0;  // messages seen by Submit()
  u32 dropped = 0;    // PAD / ACK packets lost
  u32 held = 0;       // messages held back for a non-zero time
  u32 released = 0;   // messages handed out by PopDue()
};

class NetSim
{
public:
  // `stream` separates the random sequences of the two instances of a run (0 host, 1 guest).
  NetSim(const NetSimConfig& config, u32 stream);

  // A message arrived at `now_us`. `droppable`: a PAD / PAD_ACK packet (subject to loss_pct). `tag` is handed back by PopDue (the transport
  // keeps the peer there). False: dropped, nothing will come out.
  bool Submit(u64 now_us, bool droppable, u64 tag, std::vector<u8> bytes);
  // The next message whose release time is <= now_us (earliest first). False: none due.
  bool PopDue(u64 now_us, u64* tag, std::vector<u8>* bytes);
  bool Empty() const { return m_queue.empty(); }
  size_t Pending() const { return m_queue.size(); }
  // Release time (microseconds, caller's clock) of the earliest held message; 0 when nothing is held.
  u64 NextDueUs() const { return m_queue.empty() ? 0 : m_queue.begin()->first.first; }
  const NetSimStats& stats() const { return m_stats; }
  const NetSimConfig& config() const { return m_cfg; }

private:
  struct Entry
  {
    u64 tag = 0;
    std::vector<u8> bytes;
  };
  NetSimConfig m_cfg;
  u64 m_jitter_state;
  u64 m_loss_state;
  u64 m_order = 0;
  std::map<std::pair<u64, u64>, Entry> m_queue;  // (release time, arrival order) -> message
  NetSimStats m_stats;
};
}  // namespace Rev3::Net
