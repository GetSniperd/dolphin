// Rev3 online rollback session (milestone M2): the pure-logic state machine that answers the game's per-frame EXI hook command
// when two machines play each other. Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT DOES
//   RollbackSession::OnCommand() takes one hook command (Rev3::Command, protocol v2, Rev3Rollback.h) and returns the reply
//   (Rev3::Reply). It implements docs/rollback_netplay_plan.md sections 2, 3 and 5 of GetSniperd/rev3-mod:
//     OFFLINE     not in a session: the reply echoes the pads of the command (the game behaves as without the fork)
//     CONNECTING  before frame 0: the first command blocks until the transport is connected and both HELLOs matched ("frame-0
//                 barrier": the hook counter is the session clock, both machines start it together)
//     LOCKSTEP    every call blocks until the peer's pad for that frame is there; nothing is ever predicted (menus, loading,
//                 character select, the result screen)
//     ARMED       inside a live fight (the arm gate is open): the remote pad is predicted (= the last confirmed remote pad), the
//                 game runs on, and when the real pad turns out different the session restores the snapshot of the first wrong
//                 frame and has the game re-run the frames (RESTORED reply, then ordinary hook calls: the SyncTest path with real
//                 inputs). At most MAXR frames are ever predicted (stall rule), so no rollback is deeper than MAXR.
//     DRAIN       the gate closed (KO, loading ahead): block until every remote input below the current frame is confirmed, do the
//                 rollback that is still due, then continue in LOCKSTEP so the KO cinematic and the load run exact.
//   Input delay D: the pad read at call N is played at frame N + D; frames 0..D-1 are neutral on both sides. Host = game port 0,
//   guest = game port 1; the local pad always arrives as the command's port-0 pad.
//   Desync detection: a checksum of both fighters (rev3-mod rbchecksum.py, computed by the SnapshotStore) of every 30th FINAL frame
//   (all real inputs below it known and verified) rides in every PAD packet; the last 8 of both sides are kept and compared. A
//   mismatch logs DESYNC with the first differing field, exchanges DESYNC_DUMPs and leaves the session (OFFLINE; the game goes on
//   with the local pads; there is no resync).
//
// HOW IT FITS IN
//   Everything outside this class is an interface, so a gtest drives two sessions through a fake lossy transport and a fake game
//   (Source/UnitTests/Core/Rev3SessionTest.cpp) with no Dolphin core:
//     Transport      what SlippiNetplayClient will provide in M3 (PAD/PAD_ACK with 8-byte pads, reliable channel, clock)
//     SnapshotStore  what Rev3Rollback.cpp's snapshot slots will provide in M3/M4 (capture, restore, checksum, 61-field arrays)
//     Sink           Rev3Rollback.h's log sink (one line per event: ROLLBACK, STALL, GATE, CHK, DESYNC, SUMMARY ...)
//     WaitFn         "block the CPU thread until pred() or timeout": 1 ms sleeps that also check the core state, in the real glue
//   The wire types (Command, Reply, PadBytes, Sink, FIELD_COUNT) come from Rev3Rollback.h; nothing else of it is used.
//   Blocking is modelled twice: OnCommand() loops Step() with the injected WaitFn; Step() itself never blocks (it returns false and
//   names what it waits for), which is what the two-machine tests use (a discrete-event scheduler needs no threads).
//
// THREADING
//   One thread (the CPU thread, inside the EXI DMA handler) calls OnCommand()/Step(). The Transport implementation must make
//   its own state safe against the network thread; the session never takes a lock.
//
// THE GATE RULE (M4: both machines always agree on WHEN the shared mode changes)
//   The shared modes are LOCKSTEP and ARMED. Whatever changes them must be a function of state both machines have, applied at a frame both
//   know. Three kinds of decisions exist; each is handled in the one way that keeps the machines in agreement:
//   OPEN   decided in LOCKSTEP, where every frame is final (all inputs confirmed). Applied at the frame where both sides' START_CHECKs
//          name the same frame and carry the same state; a side whose local conditions (quiet hardware, snapshot set learned) differ
//          simply sends no matching START_CHECK, and both stay in lockstep (a mismatch of frames is refused on both sides).
//   CLOSE  (a fighter at health 0, fighters not found) decided ONLY on FINAL frames: the state recorded at the capture of frame F is
//          identical on both machines once F is final (every remote pad below F was played as confirmed). The first final frame F whose
//          record says "lost" schedules the close for frame F + MAXR + 1, on both machines alike. F is always declared final by call
//          F + MAXR (stall rule: R >= N - MAXR; the pre-rollback finalisation below keeps rollbacks from delaying it), so neither side
//          has passed F + MAXR + 1 when it learns the decision. Until then the machines keep predicting; at that frame both enter DRAIN
//          together. A live (possibly mispredicted) state never closes anything.
//   LOCAL  reasons that exist on one machine only never change the shared mode, because the peer cannot know about them:
//            hardware busy for a while  -> HOLD: this side predicts nothing while it lasts (it waits for the real pad of every frame),
//                                          so a rollback cannot become due while the hardware is busy; the gate stays open for the peer.
//            snapshot set would change  -> RELAYOUT: this side drains (waits for every remote pad, rolls back what is due), installs the
//                                          new set and carries on ARMED. The peer sees a short stall, nothing else.
//          Both are invisible on the wire, and the frames played are the same, so the final states cannot differ.
//   A frame without fighters cannot be snapshotted, so such a frame is never predicted (the call waits for the real pad), and a live state
//   without fighters is recorded as "lost" like a health-0 fighter.
//
// PACING (M5): keeping the two machines in step so that neither one stalls
//   Rollback bounds how far a machine may run ahead of what it knows (the stall rule, N - R > MAXR), but it does not say who is ahead. Two
//   machines that start a few frames apart settle with one of them always at the stall limit (it waits for the other, then catches up at
//   full speed, because Dolphin's throttle makes up lost time) and the other never predicting at all. TimeSync measures the skew the way
//   Slippi does (SlippiNetplay.cpp CalcTimeOffsetUs): every time a newer remote pad arrives, "when did the opponent send frame f, by our
//   clock" (arrival - RTT/2) minus "when did we send our newest frame h" plus the frame difference; a window of the last 30 samples is
//   sorted and the middle third averaged. Positive = this machine is AHEAD. RollbackSession::Pace() (ARMED, normal calls only) looks at it
//   every PACE_INTERVAL_FRAMES; an ahead machine delays itself through the PaceActuator, a machine that is behind does nothing (the other
//   side slows). The first PaceConfig::hard_frames after the gate opens correct hard (this is where the lockstep leaves a skew behind), later only
//   gently (Slippi's emulation-speed step, as a delay). The delay only moves host wall time: it never touches the game, the replies or the
//   pads. Never in LOCKSTEP (the wait for the pad IS the pacing there) and never in a resimulation. The numbers are in PaceConfig.
//   Skew at the gate: in lockstep every call waits for the peer's pad and then runs unthrottled until it is back on its schedule, so neither
//   machine's throttle clock says anything about the other's. When the gate opens (both at the same frame, THE GATE RULE) the two are
//   some frames apart, in the order of the one-way latency plus jitter plus the START_CHECK exchange (the test network starts them 0 to 8
//   frames apart; D's loopback with 137 ms RTT settled with the host pinned at the stall limit). Nothing in the protocol removes it, so the
//   first PaceConfig::hard_frames (120) frames are the hard window: the machine that is ahead gives the difference back at once (at most 5
//   frames per check, in slices of 4 ms), the one that is behind does nothing. Every correction clears the offset window, so the next check
//   waits for min_samples (15) fresh samples measured with the new timing. Pacing needs an RTT estimate (no decision without one: a
//   missing RTT would make both machines believe they are ahead and both slow down). The throttle keeps its schedule through a hidden
//   resimulation (CoreTimingManager::BeginResim / EndResim: the re-run frames do not count as schedule time). Until M5 fix 2 the clock was
//   restarted at the end of every resimulation, which lost the re-run time for good: D's guest rolled back 120 times at ~7 ms, about 2 % of
//   its speed, drifted later and later and left the host pinned at the stall limit, more than a 1 % pacing could take back.
//
// MAXR (M5)
//   Config::max_rollback (REV3_NET_ROLLBACK_MAX, 1..7 = Slippi's ROLLBACK_MAX_FRAMES) is shared by both machines (HELLO checks it): the
//   scheduled gate close and the snapshot slot count depend on it. The stall rule uses MAXR_eff <= MAXR, which one machine may lower on its
//   own (stalls are local): with Config::maxr_adapt the cost of every rollback (RESTORED reply to the end of the resimulation) is measured and
//   MAXR_eff is the largest K for which K frames of resimulation fit the budget at the p95 per-frame cost. See AdaptMaxr().
//
// NOT DONE YET (later milestones)
//   M6: the game-side render / SFX skip. See also the header notes at the declarations marked "M3".

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Slippi/Rev3NetSim.h"
#include "Core/Slippi/Rev3Rollback.h"

namespace Rev3::Net
{
// ---- constants -------------------------------------------------------------------------------------------------------------
constexpr u32 NET_PROTOCOL_VERSION = 2;       // HELLO: the default hook protocol version; the module sends the version byte of the first command (2 or 3)
constexpr s32 CHECK_INTERVAL = 30;            // frames between exchanged checksums (rbchecksum.CHECK_INTERVAL)
constexpr size_t CHECK_KEEP = 8;              // own and peer checksums kept
constexpr int MAX_ROLLBACK_CEILING = 7;       // SlippiNetplay.h ROLLBACK_MAX_FRAMES: the largest MAXR a config may ask for
constexpr int MAX_DELAY = 15;                 // largest input delay accepted (frames)
constexpr u32 FIELDS_VERSION = 1;             // rbchecksum.FIELDS_VERSION
constexpr u32 FIELDS_DIGEST_V1 = 0xD4076EF2;  // rbchecksum.fields_digest() for version 1 (a test keeps FIELDS and this equal)

using Pad8 = std::array<u8, 8>;  // bytes 0..7 of a PADStatus: buttons u16, stick x/y, c-stick x/y, triggers L/R. All that Rev3 reads.

Pad8 SanitizePad(const PadBytes& pad);  // first 8 bytes
PadBytes ToPadBytes(const Pad8& pad);   // 8 bytes + analogA/B = 0, err = 0

// crc32 of "name:offset:4;" per field in Rev3::FIELDS order + "v1" (rbchecksum.fields_digest). Equal on both machines or the checksums
// of the two builds mean different things.
u32 FieldsDigest();

enum class Role : u8
{
  Host = 0,   // game port 0
  Guest = 1,  // game port 1
};

enum class Mode : u8
{
  OFFLINE,
  CONNECTING,
  LOCKSTEP,
  ARMED,
  DRAIN,
};
const char* ModeName(Mode mode);

// ---- pacing (M5) -----------------------------------------------------------------------------------------------------------
// Every number below is a tunable; the unit is in the comment. REV3_NET_PACE* in Rev3NetLink.h override some of them.
// "ahead" = this machine runs earlier than the peer (it sends the same frame earlier); the offset is positive then.
constexpr s32 PACE_FRAME_US = 16683;  // microseconds per game frame at 60 Hz (Slippi's frame_time, EXI_DeviceSlippi.cpp:1424)
constexpr s32 PACE_INTERVAL_FRAMES = 30;  // frames between two pacing checks and samples in the offset window (Slippi SLIPPI_ONLINE_LOCKSTEP_INTERVAL, SlippiNetplay.h:35)

struct PaceConfig
{
  bool enabled = true;               // false (REV3_NET_PACE=0): measure and log, never delay (the A/B baseline)
  s32 interval_frames = PACE_INTERVAL_FRAMES;  // frames between two checks (after a correction the next one waits for fresh samples, see min_samples)
  s32 hard_frames = 120;             // frames after the gate opened in which corrections are hard (Slippi: "frame <= 120", EXI_DeviceSlippi.cpp:1437)
  s32 t1_us = 10000;                 // hard sync: ahead by this much or less = nothing to do (microseconds; Slippi t1 = 10 ms, EXI:1426)
  s32 hard_target_us = 5000;         // hard sync: a correction aims to leave this much lead (microseconds); Slippi leaves up to t1
  s32 hard_max_us = 5 * PACE_FRAME_US;  // hard sync: most delay one check may ask for (microseconds; Slippi: 5 frames at most early on, EXI:1451)
  s32 gentle_deadband_us = 8000;     // later: ahead by this much or less = nothing (microseconds; Slippi: 8000, EXI:1521)
  s32 gentle_window_frames = 2;      // later: the lead (in frames) at which the correction reaches its maximum (Slippi slow_down_frame_window = 3, EXI:1514; 2 here:
                                     // the lead cannot grow past the stall limit, about 3 frames, so a 3 frame window never reached its maximum)
  s32 gentle_max_permille = 20;      // later: largest slowdown, thousandths of the interval (20 = 2.0 %; Slippi uses 0.5 % = 5, EXI:1513). 2.0 % of a
                                     // 30 frame interval is 10 ms: the gentle regime can never cost more than that per check. (D's first loopback with 1.0 %: the
                                     // lead stayed at the stall limit; a machine that rolls back loses up to ~2 % of its speed unless the throttle keeps its schedule)
  s32 slice_us = 4000;               // the most one hook call is delayed (microseconds). A correction is spread over calls of this size, so a hard
                                     // one (83 ms) takes 21 calls, inside one interval, and a frame is never stretched by more than 4 ms
  u32 min_samples = 15;              // offsets needed in the window before a check may decide (a correction clears the window)
  u64 min_rtt_us = 1;                // no decision without an RTT estimate (the offset subtracts RTT/2): microseconds
};

// What one check decided.
struct PaceDecision
{
  const char* action = "none";  // "none" (not ahead), "hard", "gentle", "behind" (the peer is ahead: nothing to do here), "off" (pacing disabled, ahead)
  s32 delay_us = 0;             // microseconds this machine delays itself in total (spread over calls of at most slice_us)
};
// The pure policy: `frames_since_gate` = frames since the arm gate opened, `offset_us` = the robust average (positive = ahead).
PaceDecision DecidePace(const PaceConfig& config, s32 frames_since_gate, s32 offset_us);

// Slippi's time offset estimate (SlippiNetplay.cpp:295-316 per received pad, CalcTimeOffsetUs:1664-1733), for one peer.
//   OnLocalSend  a local pad for `frame` went out at now_us.
//   OnRemote     a PAD packet whose newest pad is for `frame` arrived at now_us; only the FIRST arrival of a newer frame is a sample (a re-sent
//                packet says nothing about when the peer sent it). The peer sent that frame about rtt/2 ago, so
//                   sample = (now - rtt/2) - our_send_time(frame)
//                is how much LATER than us the peer sent the same frame: positive = we are ahead. Slippi compares with our NEWEST send h instead
//                and adds PACE_FRAME_US * (h - frame), which assumes our frames go out 16.7 ms apart; Dolphin's throttle makes a machine that waited
//                for the peer run its next frames in a burst, so here the send time of the very same frame is used whenever it is still known
//                (the last PACE_RING frames) and Slippi's formula only when the peer is so far ahead that we have not sent that frame yet.
//                Not sampled until an RTT is known.
//   OffsetUs     sorts the last PACE_INTERVAL_FRAMES samples and averages the middle third (outliers: jitter spikes, late re-sends).
// Pure and clock-free (the caller passes microseconds); used by PadLink (the real transport) and the test network.
class TimeSync
{
public:
  static constexpr size_t RING = 64;  // local send times kept (frames)
  void OnLocalSend(s32 frame, u64 now_us);
  void OnRemote(s32 frame, u64 now_us, u64 rtt_us);
  bool OffsetUs(s32* out, u32* samples = nullptr) const;  // false: no sample yet
  void Reset();                                           // forget the samples (after a correction: they describe the old timing), and ignore remote
                                                          // frames up to our newest sent one: our send time of those is from before the correction
  u32 Samples() const { return static_cast<u32>(m_count); }

private:
  struct Sent
  {
    s32 frame = -1;
    u64 us = 0;
  };
  std::array<s32, PACE_INTERVAL_FRAMES> m_buf{};
  size_t m_count = 0;
  size_t m_next = 0;
  std::array<Sent, RING> m_sent{};
  s32 m_local_frame = -1;  // newest local frame sent
  s32 m_min_frame = 0;     // remote frames below this are not sampled (Reset)
  s32 m_newest_remote = -1;
};

// What carries out a delay. Real: sleeps and restarts Dolphin's throttle clock (CoreTimingManager::SleepAndResetThrottle), so the lost time is
// not made up by running the next frames faster. CPU thread only. Never touches game state.
class PaceActuator
{
public:
  virtual ~PaceActuator() = default;
  virtual void Delay(u32 microseconds) = 0;
};

// ---- the transport ---------------------------------------------------------------------------------------------------------
enum class ConnState : u8
{
  Connecting,
  Connected,
  Failed,  // never recovers; the session leaves
};

// Reliable message ids (channel 0 of the real transport; ids after NetPlayProto.h:166, rollback_netplay_plan.md section 4.4).
enum class MsgType : u8
{
  Hello = 0x90,
  StartCheck = 0x91,
  DesyncDump = 0x92,
  Bye = 0x93,
  RangeCrc = 0x94,  // M4, REV3_NET_RANGE_CRC=1 only: the CRCs of the snapshot ranges at a checksum frame (testing)
};

struct Message
{
  MsgType type = MsgType::Bye;
  std::vector<u8> bytes;
};

struct PeerChecksum
{
  s32 frame = -1;
  u32 value = 0;
};

// What the session needs from the network. Frames are HOOK frames (the real implementation adds Slippi's +1 on the wire).
class Transport
{
public:
  virtual ~Transport() = default;
  // The pad FOR `frame` (= call N + D), in ascending frame order without gaps, plus the newest final checksum (checksum_frame -1 = none)
  // that rides in the packet. The real implementation queues the pad, sends one PAD packet with every unacked pad (newest first), and
  // the receiver acks; this is Slippi's SendSlippiPad.
  virtual void SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum) = 0;
  // R: the highest frame such that the remote pads of ALL frames <= R are here (contiguous from frame 0); -1 = none.
  virtual s32 RemoteConfirmed() const = 0;
  // The remote pad of a frame <= R (kept for at least the last 64 frames). False for a frame above R.
  virtual bool RemotePad(s32 frame, Pad8* out) const = 0;
  // Peer checksums seen since the last call (each distinct (frame, value) once).
  virtual std::vector<PeerChecksum> TakePeerChecksums() = 0;
  virtual void SendReliable(MsgType type, const std::vector<u8>& bytes) = 0;
  virtual bool PollReliable(Message* out) = 0;  // next received reliable message, in order; false = none
  virtual u64 Now() const = 0;                  // microseconds, monotonic
  virtual ConnState State() const = 0;
  // Called each time the session looks at the transport while it WAITS (and by BlockSatisfied): a real transport re-sends the unacked local
  // pads that have not been acked for about one round trip. Slippi only re-sends when a new pad is queued (its game keeps asking while it
  // stalls); a blocked session queues nothing, so without this a lost packet at the wrong moment would deadlock both sides.
  virtual void Pump() {}
  // REV3_NETSIM counters (M4) for the SUMMARY line; `active` false = no simulation.
  virtual NetSimStats NetSimCounts() const { return {}; }
  // M5, pacing. TimeSync's robust offset (positive = this machine is ahead) and the number of samples behind it; false = none yet.
  virtual bool TimeOffsetUs(s32* /*out*/, u32* /*samples*/) const { return false; }
  virtual void ResetTimeOffsets() {}  // forget the samples (the session just changed its own timing)
  virtual u64 RttUs() const { return 0; }  // smoothed round trip of the PAD / ACK exchange, microseconds; 0 = no sample yet
};

// ---- snapshots -------------------------------------------------------------------------------------------------------------
// The 61 fighter fields (Rev3::FIELDS order) of both players as 32-bit words; present bit 0 = player 1 found, bit 1 = player 2.
struct FieldSet
{
  u8 present = 0;
  std::array<std::array<u32, FIELD_COUNT>, 2> value{};
  bool operator==(const FieldSet& other) const { return present == other.present && value == other.value; }
};

struct CaptureInfo
{
  u32 checksum = 0;  // Rev3Checksum(frame) of the live state (rbchecksum.py), taken in the same breath as the capture
  FieldSet fields;
};

// One memory range and the CRC-32 of its bytes (REV3_NET_RANGE_CRC).
using RangeCrc = RangeHash;  // Rev3Rollback.h

// Snapshot slots keyed by hook frame; M3 implements this on Rev3Rollback's capture/restore. The session needs MAXR + 2 slots.
class SnapshotStore
{
public:
  virtual ~SnapshotStore() = default;
  // Captures the live state as the start of `frame` (whitelist/adaptive ranges + the stack window at sp), replacing any slot for that
  // frame, and fills *info. False: the capture failed (the session leaves).
  virtual bool Capture(s32 frame, u32 sp, CaptureInfo* info) = 0;
  virtual bool Has(s32 frame) const = 0;
  virtual u32 SlotSp(s32 frame) const = 0;  // the sp the slot was captured with (0 when absent)
  // Writes the slot of `frame` back (memory only: the game's hook reloads the registers). False: failed.
  virtual bool Restore(s32 frame) = 0;
  // Checksum (+ fields) of the LIVE state as the start of `frame`, without a snapshot (lockstep check frames, START_CHECK).
  virtual u32 LiveChecksum(s32 frame, FieldSet* fields) = 0;
  // CRC over the stable whitelist ranges (START_CHECK); 0 when not available.
  virtual u32 StableCrc() = 0;
  // The fields recorded at the capture of `frame` (the desync field trace; at least the last 31 frames are kept). False: not kept.
  virtual bool Fields(s32 frame, FieldSet* out) const = 0;
  virtual void Clear() = 0;  // forget every slot (the gate closed)
  // M4. The snapshot set changed on this machine only (learned pages, a new hardware cut): forget every slot, install the new set and verify
  // that the fighters are inside it. False: not usable (the session leaves). The default is Clear().
  virtual bool Relayout()
  {
    Clear();
    return true;
  }
  // M4, REV3_NET_RANGE_CRC=1 (testing only). CRC-32 of each snapshot range as it was at the start of `frame`: from the frame's slot, or, when
  // `live` (the caller is at that frame's own hook call), from live memory. False: not available.
  virtual bool RangeCrcs(s32 /*frame*/, bool /*live*/, std::vector<RangeCrc>* /*out*/) { return false; }
  // False when the slot of `frame` was captured with a disc read in flight (hardware / IOS or the game's own DVD library): restoring it would leave
  // the game waiting for a read that already finished, so the session never rolls back to such a slot while an earlier clean one exists.
  virtual bool Clean(s32 /*frame*/) const { return true; }
  // Called before every SUMMARY line (also the final one): the store may write its own lines first (the `NOTE learned summary` line of the
  // snapshot machinery) and returns fields appended to the SUMMARY line (restore verify counters, resimulation hiding counters), with a
  // leading space, or "".
  virtual std::string SummaryNote() { return {}; }
  // Called right after every SUMMARY line: one more line for the log (the `SNAPSHOT kind=..` line of the snapshot machinery), or "".
  virtual std::string AfterSummaryLine() { return {}; }
};

// The FieldSet of raw big-endian field bytes (Rev3::ReadFighterFields / SyncTest::NetSnapshotInfo), `present` as in FieldSet.
FieldSet FieldsFromRaw(const std::array<std::array<u8, FIELD_BYTES>, 2>& raw, u8 present);
// rbchecksum.py checksum(frame, fighters): CRC-32 (zlib, init 0) over u32 BE frame, then for each present player u32 BE id (1, 2) and the 61
// words big-endian. Equals Rev3::FighterChecksum on the memory the fields were read from.
u32 ChecksumOfFields(s32 frame, const FieldSet& fields);

// ---- HELLO, START_CHECK, DESYNC_DUMP ---------------------------------------------------------------------------------------
// Sent once after connecting. Everything that makes two machines play differently goes in; a mismatch names the items.
struct Hello
{
  Role role = Role::Host;
  std::string game_id;                         // "RNEEDA"
  u32 protocol = NET_PROTOCOL_VERSION;         // the hook protocol (2 or 3, Rev3::Command::protocol): both machines must run the same patch set
  u32 fields_version = FIELDS_VERSION;
  u32 fields_digest = FIELDS_DIGEST_V1;
  u32 delay = 2;                               // input delay D in frames; the HOST's value wins (not a mismatch)
  std::vector<std::pair<std::string, std::string>> items;  // settings: fork build id, CPU core, DSP engine, RTC, save hash, code hash ...
};
std::vector<u8> SerializeHello(const Hello& hello);
bool ParseHello(const std::vector<u8>& bytes, Hello* out);
// Human readable differences, one per entry ("item cpu_core: own=JIT64 peer=CachedInterpreter"); empty = the two may play. `mine`/`theirs`
// are each side's own HELLO; a delay difference is not listed, two equal roles are.
std::vector<std::string> HelloMismatches(const Hello& mine, const Hello& theirs);

// Sent by both sides at the frame the arm gate opens (lockstep, so the same frame): the exact state must agree before prediction starts.
struct StartCheck
{
  s32 frame = 0;
  u32 checksum = 0;
  u32 stable_crc = 0;
  FieldSet fields;
};
std::vector<u8> SerializeStartCheck(const StartCheck& check);
bool ParseStartCheck(const std::vector<u8>& bytes, StartCheck* out);
std::vector<std::string> StartCheckMismatches(const StartCheck& mine, const StartCheck& theirs);  // empty = equal

// REV3_NET_RANGE_CRC=1 (testing): at each checksum frame both sides send the CRC of every snapshot range; a difference is logged (the
// names of the memory ranges that differ) and never ends the session. It finds the whitelist range that is missing when the field
// checksum fires.
struct RangeCrcMsg
{
  s32 frame = 0;
  std::vector<RangeCrc> ranges;
};
std::vector<u8> SerializeRangeCrc(const RangeCrcMsg& msg);
bool ParseRangeCrc(const std::vector<u8>& bytes, RangeCrcMsg* out);

// The field trace both sides exchange after a checksum mismatch: (frame, fields) for the frames around it.
struct DesyncDump
{
  s32 frame = 0;  // the mismatching check frame
  std::vector<std::pair<s32, FieldSet>> trace;
};
std::vector<u8> SerializeDesyncDump(const DesyncDump& dump);
bool ParseDesyncDump(const std::vector<u8>& bytes, DesyncDump* out);

// ---- configuration and per-call input --------------------------------------------------------------------------------------
struct Config
{
  Role role = Role::Host;
  int delay = 2;                      // D, input delay in frames (REV3_NET_DELAY); the guest adopts the host's value from its HELLO
  int max_rollback = 5;               // MAXR: most frames ever predicted = deepest rollback (1..MAX_ROLLBACK_CEILING)
  u32 barrier_timeout_ms = 120000;    // frame-0 barrier: connect + HELLO (D starts both emulators by hand)
  u32 wait_timeout_ms = 10000;        // lockstep / drain / ARMED waits for a peer that sends nothing
  u32 stall_disconnect_frames = 420;  // Slippi's number (EXI_DeviceSlippi.cpp:1396, 7 s): a stall (N - R > MAXR) that lasts this many frames (x 1/60 s)
                                      // ends the session, and so does this many CONSECUTIVE stalled calls whose waits add up to as many frame times
  u32 start_check_timeout_ms = 10000; // waiting for the peer's START_CHECK at arm time (a timeout only refuses to arm)
  bool range_crc = false;             // REV3_NET_RANGE_CRC=1: exchange the CRCs of the snapshot ranges at every checksum frame (testing only)
  PaceConfig pace;                    // M5: pacing (REV3_NET_PACE and friends)
  bool maxr_adapt = false;            // M5, REV3_NET_MAXR_ADAPT=1: the stall rule uses MAXR_eff <= max_rollback, lowered when resimulation gets slow
  u32 maxr_budget_us = 14000;         // M5, adaptive MAXR: the resimulation of K frames should cost less than this at its p95 (microseconds; ~ one 60 Hz
                                      // frame of 16.7 ms minus the live frame's own work)
  Hello hello;                        // this side's HELLO (role and delay are filled in from this config when the session starts)
};

// What the caller evaluates outside the session (needs the game state; the session stays pure). EVERY call carries the live-state flags
// (fighters_found, health_zero), resimulated calls included: the session records them with the capture of the frame and decides on the
// FINAL frames only (see "THE GATE RULE" above). The remaining fields are read on NORMAL calls only.
struct Gate
{
  // ---- the live state of THIS call (start of the frame being run)
  bool fighters_found = true;   // both fighters found through TEAMS; false = nothing can be snapshotted, the frame is never predicted
  bool health_zero = false;     // a fighter's health (+0x28) is 0 (the KO that ends the fight)
  // ---- LOCKSTEP: may the session arm now
  bool open = false;            // fighters found for N consecutive calls AND hardware quiet for M calls AND nobody at health 0 AND set ready
  // ---- local conditions (see the rule above: never a shared decision)
  bool hardware_quiet = true;   // no DVD/ARAM DMA in flight right now (HardwareView): a rollback may run
  bool hardware_hold = false;   // ARMED: the hardware has been busy for several calls in a row: predict nothing while it lasts
  bool layout_pending = false;  // ARMED: the snapshot set would change (learned pages / a hardware cut): re-install it at a drain
  const char* why = "";         // logged with GATE open
};

struct SessionStats
{
  u32 calls = 0;                      // hook commands answered by the session (every mode, resimulated frames included)
  u32 resim_calls = 0;                // of which re-runs after a RESTORED reply
  u32 rollbacks = 0;
  u32 max_depth = 0;
  std::array<u32, MAX_ROLLBACK_CEILING + 2> depth_hist{};  // [d] = rollbacks of depth d
  u32 predicted_frames = 0;           // frames whose remote pad was a prediction when replied
  u32 stalls = 0;                     // calls that had to wait because N - R > MAXR
  u64 stall_us_total = 0;
  u64 stall_us_max = 0;
  u32 postponed = 0;                  // rollbacks postponed because hardware was busy
  u32 forced_hw = 0;                  // rollbacks done while busy because postponing would have exceeded MAXR (or in DRAIN)
  u32 io_dirty_targets = 0;           // rollbacks whose target slot had been captured with a disc read in flight
  u32 io_redirected = 0;              // ... of which went to the newest earlier clean slot instead (more frames resimulated)
  u32 io_no_clean_slot = 0;           // ... and of which found no clean slot in the ring: restored anyway (a desync risk, logged loudly)
  u32 hw_holds = 0;                   // times a sustained busy hardware made this side predict nothing (HOLD)
  u32 hold_calls = 0;                 // calls answered with the real remote pad because of a HOLD
  u32 relayouts = 0;                  // local drains that installed a changed snapshot set and carried on ARMED
  u32 gate_late = 0;                  // a scheduled close that was applied after its frame (the machines may have closed apart: must stay 0)
  u32 log_suppressed = 0;             // ROLLBACK / STALL / RESIM detail lines left out (rate limit); summarised per window
  u32 range_checks = 0;               // REV3_NET_RANGE_CRC: checksum frames whose range CRCs were compared
  u32 range_diff_frames = 0;          // ... of which differing
  u32 checks_ok = 0;
  u32 checks_bad = 0;
  u32 gate_opens = 0;
  u32 gate_closes = 0;
  u32 arm_refused = 0;
  // M5 pacing (checks = decisions taken; the offset sums are microseconds, positive = ahead)
  u32 pace_checks = 0;
  u32 pace_slowdowns = 0;             // checks that asked for a delay
  u64 pace_sleep_us_total = 0;        // delay actually handed to the PaceActuator
  s64 pace_offset_sum_us = 0;
  s32 pace_offset_max_us = 0;
  s32 pace_offset_min_us = 0;
  u32 pace_lines_left_out = 0;        // PACE lines not written (rate limit)
  // M5 stall run and resimulation cost
  u32 stall_run_max = 0;              // longest run of consecutive stalled calls
  u32 resim_costs = 0;                // rollbacks whose cost was measured (RESTORED reply to the end of the resimulation)
  u32 resim_cost_skipped = 0;         // rollbacks not measured (the resimulation had to wait for the peer)
  int maxr_eff = 0;                   // the stall rule's current limit (= max_rollback unless adaptive)
  int maxr_eff_min = 0;
  u32 maxr_changes = 0;
};

// ---- hiding the resimulated frames, v1 (M4) ---------------------------------------------------------------------------------
// While the game re-runs frames after a RESTORED reply the player should neither wait for them nor hear them twice: v1 turns the
// throttle off and mutes the audio OUTPUT (never the DSP). The real implementation is in Rev3Rollback.cpp (Dolphin's throttler flag and
// sound stream volume); tests inject a fake.
class ResimHider
{
public:
  virtual ~ResimHider() = default;
  virtual void SetHidden(bool hidden) = 0;  // true: throttle off + mute; false: back to the user's settings. CPU thread.
};

// Follows RollbackSession::Resimulating() after every command (and at shutdown, with false): hides when it becomes true, restores when it
// becomes false, i.e. at the reply of the last resimulated frame, when the session leaves, and when the module shuts down.
class ResimHiding
{
public:
  ResimHiding(ResimHider* hider, Sink& log) : m_hider(hider), m_log(log) {}
  void Update(bool resimulating);
  bool hidden() const { return m_hidden; }
  u32 hides() const { return m_hides; }

private:
  ResimHider* m_hider;
  Sink& m_log;
  bool m_hidden = false;
  u32 m_hides = 0;
  bool m_noted = false;
};

// Rate limit of the per-event detail lines (M4): ROLLBACK / STALL / RESIM lines of the session are written in full for the first
// LOG_FULL_LINES events of each kind; after that a window of LOG_WINDOW_FRAMES frames writes one `ROLLBACK depth_hist=..` summary line (with
// the counts of ALL events of the window) instead, and only when it had left lines out. CHK, DESYNC, ERROR, GATE, LEAVE, SUMMARY are never
// limited. The same numbers rate-limit SyncTest's RESTORE lines in a netplay run (Rev3Rollback.h).
constexpr size_t RESIM_COST_KEEP = 4096;  // rollback costs kept for the SUMMARY percentiles (a ring; a match has far fewer rollbacks)
constexpr u32 PACE_LOG_FULL_LINES = 120;  // PACE lines written in full; afterwards only hard corrections and one in PACE_LOG_EVERY checks
constexpr u32 PACE_LOG_EVERY = 20;
constexpr u32 LOG_FULL_LINES = 200;
constexpr s32 LOG_WINDOW_FRAMES = 600;

// ---- the session -----------------------------------------------------------------------------------------------------------
class RollbackSession
{
public:
  // "Block until pred() is true or timeout_ms passed; return pred()". The real one sleeps 1 ms at a time, checks the core state and
  // calls Transport::Pump(); tests inject a fake clock.
  using WaitFn = std::function<bool(const std::function<bool()>& pred, u32 timeout_ms)>;

  RollbackSession(const Config& config, Transport& transport, SnapshotStore& store, Sink& sink, WaitFn wait);

  // The blocking form: loops Step() with the WaitFn. Always returns a reply.
  Reply OnCommand(const Command& command, const Gate& gate);

  // The non-blocking core. Returns true with *reply filled, or false = "I must wait": call BlockSatisfied() (or wait on it with the
  // timeout BlockTimeoutMs()), and call Step() again with the SAME command and gate; on a timeout call BlockTimedOut() first.
  bool Step(const Command& command, const Gate& gate, Reply* reply);
  bool BlockSatisfied();    // pumps the transport and messages; true when Step() may be retried
  u32 BlockTimeoutMs() const;
  void BlockTimedOut();     // logs, and leaves the session (or, for the START_CHECK wait, refuses to arm)

  // Writes SUMMARY (once; also written when the session is left).
  void Finish();

  // Leaves the session now with this reason (BYE to the peer, SUMMARY, OFFLINE): the glue calls it when the emulator is stopping while a
  // wait is running, so the log says why instead of "timed out".
  void Abort(const std::string& why);

  Mode mode() const { return m_mode; }
  bool Resimulating() const { return m_resim; }  // the game is re-running frames after a RESTORED reply (v1 hiding: throttle off + mute)
  const SessionStats& stats() const { return m_stats; }
  int delay() const { return m_delay; }
  s32 verified_frame() const { return m_verified; }  // ARMED: every remote input <= this is confirmed and was played as it was confirmed

  // Called for every frame that becomes final while ARMED (all inputs below it verified): (frame, Rev3Checksum of its start state).
  // Used by tests to compare every frame with a no-latency reference run; M4 may use it for logging.
  void SetFinalObserver(std::function<void(s32 frame, u32 checksum)> observer) { m_final_observer = std::move(observer); }

  // M5. Where pacing delays go (null = pacing only measures and logs). Not owned; must outlive the session.
  void SetPaceActuator(PaceActuator* actuator) { m_pacer = actuator; }
  int maxr_eff() const { return m_maxr_eff; }  // the stall rule's limit now (<= Config::max_rollback)
  // Percentile (0..100) of the measured rollback costs in microseconds (RESTORED reply to the end of the resimulation); false = no sample.
  bool ResimCostPercentileUs(int percent, u32* out) const;

private:
  enum class Block : u8
  {
    None,
    Barrier,
    Lockstep,
    Stall,
    DrainA,       // R >= N - 1
    DrainB,       // R >= N
    StartCheck,
  };

  struct Used  // what the game was given for a frame, and its start-state checksum
  {
    s32 frame = -1;
    Pad8 local{};
    Pad8 remote{};
    bool predicted = false;
    bool has_checksum = false;
    u32 checksum = 0;
    bool has_fields = false;  // only check frames keep their fields
    FieldSet fields;
    u8 lost = 0;              // the live state of the capturing call: 0 fine, 1 a fighter at health 0, 2 fighters not found (no snapshot)
  };

  struct OwnCheck
  {
    u32 value = 0;
    bool compared = false;
  };

  struct PadSlot
  {
    s32 frame = -1;
    Pad8 pad{};
  };

  static constexpr size_t RING = 64;

  // ---- inputs
  s32 Confirmed() const;  // R, with the neutral frames below D counted as known
  Pad8 RemoteAt(s32 frame) const;
  Pad8 Predict() const;
  Pad8 LocalAt(s32 frame) const;
  void StoreLocal(const Command& command, s32 frame);
  Reply MakeReply(const Pad8& local, const Pad8& remote, u32 flags, s32 frame) const;
  Reply EchoReply(const Command& command) const;
  Used& UsedAt(s32 frame) { return m_used[static_cast<size_t>(frame) % RING]; }

  // ---- per-mode steps
  bool StepInner(const Command& command, const Gate& gate, Reply* reply);
  bool StepLockstep(const Command& command, const Gate& gate, Reply* reply);
  bool StepArmed(const Command& command, const Gate& gate, Reply* reply);
  bool StepDrain(const Command& command, const Gate& gate, Reply* reply);
  bool StepResim(const Command& command, const Gate& gate, Reply* reply);
  bool DoRollback(const Command& command, s32 target, bool forced, Reply* reply);
  // The frame to restore for a misprediction at `g`: g itself, or the newest earlier frame whose slot was captured with no disc read in flight when
  // g's own slot was not (SnapshotStore::Clean). Counts and logs both cases.
  s32 ChooseCleanTarget(s32 g);
  bool FinishDrain(const Command& command, const Gate& gate, Reply* reply);
  bool FinishRelayout(const Command& command, const Gate& gate, Reply* reply);
  bool EndCall(s32 frame, const Reply& reply, Reply* out);
  bool Blocked(Block block);
  int TryArm(const Gate& gate);  // 0 = waiting, 1 = armed, 2 = refused
  void StartLockstep();
  void EnterDrain(s32 frame, const std::string& why, bool relayout);
  void ScheduleClose(s32 final_frame, u8 lost);
  void CaptureUsed(s32 frame, const Pad8& local, const Pad8& remote, bool predicted, const CaptureInfo* info, u8 lost);
  s32 FindMisprediction(s32 frame);
  void EndStall(bool hit_timeout);

  // ---- M5: pacing, stall run, resimulation cost
  void Pace(s32 frame);                // ARMED normal calls only: take a check when due, hand out the next slice of an accepted delay
  void NoteCallWithoutStall();         // a normal call completed without waiting: the stall run is over
  bool StallRunTooLong();              // true (after leaving the session): too many consecutive stalled calls
  void NoteResimCost(s32 depth, u64 cost_us);
  void AdaptMaxr();

  // ---- checksums
  void FinalizeChecks(s32 limit);
  void RecordFinal(s32 frame, u32 value, const FieldSet* fields, bool live);
  void LockstepCheck(s32 frame);
  void CompareChecksums();
  void Desync(s32 frame, u32 own, u32 peer);
  DesyncDump BuildDump(s32 frame);
  void CompareDumps(const DesyncDump& peer);
  void ExchangeRanges(s32 frame, bool live);
  void CompareRanges(s32 frame);

  // ---- rate-limited detail lines (LOG_FULL_LINES / LOG_WINDOW_FRAMES)
  enum class Detail : u8
  {
    Rollback,
    Stall,
  };
  bool DetailLine(Detail kind, const std::string& text);
  void WindowTick(s32 frame);
  void FlushWindow();

  // ---- messages and lifecycle
  void PumpMessages();
  void PollOffline();
  void HandleMessage(const Message& message);
  void SendHello();
  void LeaveSession(const std::string& why, bool tell_peer);
  void Line(const std::string& text);
  void SummaryLine(const char* tag);

  Config m_cfg;
  Transport& m_net;
  SnapshotStore& m_store;
  Sink& m_sink;
  WaitFn m_wait;

  Mode m_mode = Mode::CONNECTING;
  int m_delay = 2;
  Block m_block = Block::None;
  s32 m_cur_frame = -1;       // the frame of the call in progress (kept across Step() retries); -1 = none
  s32 m_expected_next = 0;    // the next fresh normal call must carry this frame
  s32 m_input_frame = -1;     // the frame whose local pad was already stored and sent
  Pad8 m_last_local{};
  std::array<PadSlot, RING> m_local{};

  // hello
  bool m_hello_sent = false;
  bool m_peer_hello_ok = false;

  // arm
  bool m_arm_blocked = false;      // refused (or timed out) at this gate opening; cleared when the gate closes
  s32 m_arm_sent_frame = -1;       // the frame our START_CHECK was sent for
  StartCheck m_arm_mine;
  std::map<s32, StartCheck> m_peer_start;
  s32 m_armed_frame = -1;
  s32 m_verified = -1;
  s32 m_finalized = -1;            // highest frame already handed to the observer / check bookkeeping
  std::array<Used, RING> m_used{};
  bool m_postpone_counted = false; // the postponed counter was bumped for this call

  // agreed close, local relayout / hold (see THE GATE RULE)
  s32 m_close_at = -1;             // the frame both machines enter DRAIN at (scheduled from a final frame); -1 = none
  std::string m_close_why;
  bool m_relayout = false;         // the DRAIN in progress is a local relayout: it ends ARMED, not in LOCKSTEP
  s32 m_relayout_frame = -1;       // the frame the latest relayout finished at (the same call must not start another)
  bool m_hold = false;             // HOLD in progress (hardware busy)
  u32 m_hold_run = 0;              // calls answered in this HOLD

  // resim
  bool m_resim = false;
  s32 m_resim_target = 0;
  s32 m_next_expected = 0;

  // stall
  bool m_stalled_this_call = false;  // this call was counted as a stall
  bool m_pace_allowed = false;       // M5: this call began as a normal ARMED call (not LOCKSTEP, DRAIN or a resimulation): the only kind pacing may delay
  bool m_in_stall = false;           // a stall wait is running (EndStall not yet called)
  bool m_stall_logged = false;       // the STALL start line of the stall in progress was written in full
  u64 m_stall_start_us = 0;
  u32 m_stall_run = 0;               // consecutive normal calls that stalled (reset by a call that did not)
  u64 m_stall_run_us = 0;            // their waits added up

  // checksums
  std::map<s32, OwnCheck> m_own;
  std::map<s32, FieldSet> m_own_fields;
  std::map<s32, u32> m_peer;
  s32 m_send_chk_frame = -1;
  u32 m_send_chk_value = 0;
  bool m_dump_pending = false;     // desync seen: waiting for the peer's dump to name the field
  DesyncDump m_own_dump;
  u32 m_dump_polls = 0;

  // REV3_NET_RANGE_CRC
  std::map<s32, std::vector<RangeCrc>> m_own_ranges;
  std::map<s32, std::vector<RangeCrc>> m_peer_ranges;
  u32 m_range_misses = 0;

  // log rate limit
  u32 m_detail_rollback = 0;       // ROLLBACK lines (and the RESIM lines that go with them) written in full
  bool m_resim_logged = false;     // the ROLLBACK line of the resimulation in progress was written in full
  u32 m_detail_stall = 0;
  struct Window
  {
    bool begun = false;
    s32 start = 0;
    s32 last = 0;
    u32 rollbacks = 0;
    u32 stalls = 0;
    u32 postponed = 0;
    u32 forced = 0;
    u32 suppressed = 0;
    u64 stall_us = 0;
    std::array<u32, MAX_ROLLBACK_CEILING + 2> depth{};
  } m_win;

  // M5 pacing
  PaceActuator* m_pacer = nullptr;
  s32 m_pace_next = 0;               // the frame from which the next check may run
  s32 m_pace_pending_us = 0;         // accepted delay not yet handed out
  u32 m_pace_check_lines = 0;        // PACE lines written in full so far
  bool m_pace_noted = false;         // the "no actuator" note was written

  // M5 resimulation cost and the adaptive MAXR
  int m_maxr_eff = 5;
  u64 m_resim_start_us = 0;
  bool m_resim_blocked = false;      // the resimulation in progress had to wait for the peer: its cost says nothing about the machine
  s32 m_resim_depth = 0;
  std::vector<u32> m_resim_cost_us;  // ring of the last RESIM_COST_KEEP costs
  size_t m_resim_cost_next = 0;
  std::array<u32, 64> m_resim_unit_us{};  // ring of the last 64 per-frame costs (cost / depth), for AdaptMaxr
  size_t m_resim_unit_count = 0;
  u32 m_maxr_raise_wait = 0;         // measured rollbacks left before the limit may go up again

  std::function<void(s32, u32)> m_final_observer;
  SessionStats m_stats;
  bool m_finished = false;
};
}  // namespace Rev3::Net
