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
// NOT DONE YET (later milestones)
//   M3: the real Transport on SlippiNetplayClient (direct connect, wire frame = hook frame + 1, HELLO/START_CHECK/DUMP/BYE ids
//       0x90..0x93), the real SnapshotStore on Rev3Rollback's slots, the real WaitFn, EXI glue (HandleCommand third branch,
//       REV3_NET_* variables), the arm gate evaluation (fighters found N calls + hardware quiet + nobody at 0 health), the HELLO
//       contents (CPU core, DSP, RTC, save hash, ...). M4: v1 hiding of the resim (throttle off + mute, polled through
//       Resimulating()), NetSim. M5: pacing (CalcTimeOffsetUs port), MAXR measurement. See also the header notes at the
//       declarations marked "M3".
//   Known gap: the gate-close decision is made on the (possibly predicted) live state; if a misprediction makes one machine close
//   the gate that the other keeps open, the first machine drains into lockstep and its re-arm waits (START_CHECK timeout) for a
//   peer that is not asking. It stays correct (lockstep), only slower, until the gate closes and opens again.

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
#include "Core/Slippi/Rev3Rollback.h"

namespace Rev3::Net
{
// ---- constants -------------------------------------------------------------------------------------------------------------
constexpr u32 NET_PROTOCOL_VERSION = 2;       // HELLO: the hook protocol version both sides speak
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
  u32 protocol = NET_PROTOCOL_VERSION;         // the hook protocol (2)
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
  u32 stall_disconnect_frames = 420;  // a stall (N - R > MAXR) that lasts this many frames (x 1/60 s) ends the session (Slippi's number)
  u32 start_check_timeout_ms = 10000; // waiting for the peer's START_CHECK at arm time (a timeout only refuses to arm)
  Hello hello;                        // this side's HELLO (role and delay are filled in from this config when the session starts)
};

// What the caller evaluates outside the session (needs the game state; the session stays pure). Read on every NORMAL call; resimulated
// calls (Resimulating() true when the command arrives) ignore it: the fighters of an old timeline must not close the gate.
struct Gate
{
  bool open = false;            // fighters found for N consecutive calls AND hardware quiet for M calls AND nobody at health 0
  bool hardware_quiet = true;   // no DVD/ARAM DMA in flight right now (HardwareView): a rollback may run
  const char* why = "";         // logged with GATE open/close
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
  u32 checks_ok = 0;
  u32 checks_bad = 0;
  u32 gate_opens = 0;
  u32 gate_closes = 0;
  u32 arm_refused = 0;
};

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
  bool FinishDrain(const Command& command, Reply* reply);
  bool EndCall(s32 frame, const Reply& reply, Reply* out);
  bool Blocked(Block block);
  int TryArm(const Gate& gate);  // 0 = waiting, 1 = armed, 2 = refused
  void StartLockstep();
  void EnterDrain(s32 frame, const Gate& gate);
  void CaptureUsed(s32 frame, const Pad8& local, const Pad8& remote, bool predicted, const CaptureInfo& info);
  s32 FindMisprediction(s32 frame);
  void EndStall(bool hit_timeout);

  // ---- checksums
  void FinalizeChecks(s32 limit);
  void RecordFinal(s32 frame, u32 value, const FieldSet* fields);
  void LockstepCheck(s32 frame);
  void CompareChecksums();
  void Desync(s32 frame, u32 own, u32 peer);
  DesyncDump BuildDump(s32 frame);
  void CompareDumps(const DesyncDump& peer);

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

  // resim
  bool m_resim = false;
  s32 m_resim_target = 0;
  s32 m_next_expected = 0;

  // stall
  bool m_stalled_this_call = false;  // this call was counted as a stall
  bool m_in_stall = false;           // a stall wait is running (EndStall not yet called)
  u64 m_stall_start_us = 0;

  // checksums
  std::map<s32, OwnCheck> m_own;
  std::map<s32, FieldSet> m_own_fields;
  std::map<s32, u32> m_peer;
  s32 m_send_chk_frame = -1;
  u32 m_send_chk_value = 0;
  bool m_dump_pending = false;     // desync seen: waiting for the peer's dump to name the field
  DesyncDump m_own_dump;
  u32 m_dump_polls = 0;

  std::function<void(s32, u32)> m_final_observer;
  SessionStats m_stats;
  bool m_finished = false;
};
}  // namespace Rev3::Net
