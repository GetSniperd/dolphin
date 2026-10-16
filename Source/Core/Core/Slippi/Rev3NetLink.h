// Rev3 netplay (milestone M3): the pure parts of the wire and the session glue. Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT HOLDS (no sockets, no Dolphin core, no clock of its own: everything takes `now_us` from the caller, so
// Source/UnitTests/Core/Rev3NetLinkTest.cpp covers it completely)
//   PadLink            the PAD / PAD_ACK exchange of Slippi's netplay (SlippiNetplay.cpp OnData / SendSlippiPad), kept byte-compatible:
//                        PAD  0x80: u8 id | s32 newest wire frame | u8 sender port | s32 checksum frame | u32 checksum | 8-byte pads, newest first
//                        ACK  0x81: u8 id | s32 acked wire frame | u8 sender port        (big endian, like sf::Packet)
//                      Wire frame = hook frame + 1 (Slippi's first frame is 1; the single place is WIRE_FRAME_OFFSET). Differences from
//                      Slippi: the receiver stores every pad of a packet by its own frame number (no "head frame" arithmetic) and acks R, the
//                      highest frame such that all pads up to it are here, so one lost packet never makes an ack lie; and the unacked local
//                      pads are re-sent from PollResend() when about one round trip (min 15 ms) has passed without an ack (Slippi only re-sends
//                      when a new pad is queued; a blocked session queues nothing).
//   ReliableInbox      messages 0x90..0x93 ride in ENet reliable packets with a u32 sequence number in front, so the same message arriving over
//                      both connections of a simultaneous connect (each side connects to the other) is delivered once, in order.
//   ParsePeerAddress   "ip:port"
//   ParseNetOptions    the REV3_NET_* environment variables (REV3.md "Netplay (M3)" and "Netplay (M4)")
//   BuildHello         the HELLO items from a struct of settings (the Dolphin side fills the struct)
//   GateTracker        the arm gate: fighters found for N normal calls, hardware quiet for M calls, nobody at health 0, snapshot set learned;
//                      hysteresis once open. It decides OPENING only (in LOCKSTEP, where the state is exact). Once the session is ARMED the
//                      close is the session's own deterministic decision (Rev3Session.h, THE GATE RULE); the tracker only reports the
//                      machine-local conditions of an armed stretch: hardware busy for several calls (HOLD) and a changed snapshot set
//                      (RELAYOUT), and is told with Close() when the session went back to lockstep.
//
// HOW IT FITS IN
//   Rev3Session.h (pure session state machine; its Transport / Hello / Gate types are used here)  <-  this file  <-  Rev3Net.{h,cpp}
//   (ENet transport, SyncTest-backed SnapshotStore, the 0xA0 command handler of a netplay run)  <-  Rev3Rollback.cpp (HandleCommand).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Slippi/Rev3Session.h"

namespace Rev3::Net
{
// ---- wire constants --------------------------------------------------------------------------------------------------------
constexpr u8 WIRE_PAD = 0x80;                 // NetPlay::MessageID::SLIPPI_PAD
constexpr u8 WIRE_PAD_ACK = 0x81;             // NetPlay::MessageID::SLIPPI_PAD_ACK
constexpr size_t PAD_HEADER_BYTES = 14;       // id + frame + port + checksum frame + checksum
constexpr size_t ACK_BYTES = 6;               // id + frame + port
constexpr size_t PAD_BYTES = 8;               // SLIPPI_PAD_DATA_SIZE
constexpr size_t MAX_PADS_PER_PACKET = 128;   // Slippi caps the local queue at 128 behind the newest frame
constexpr s32 WIRE_FRAME_OFFSET = 1;          // wire frame = hook frame + 1
constexpr u64 RESEND_MIN_US = 15000;          // an unacked pad is re-sent after max(this, one round trip)
constexpr size_t REMOTE_RING = 512;           // remote pads kept (by frame % REMOTE_RING)
constexpr s32 REMOTE_AHEAD = 256;             // a remote pad more than this far above R is ignored (it will be re-sent)
constexpr s32 REMOTE_KEEP = 192;              // RemotePad() answers for frames at most this far below R (the session needs 64)
constexpr size_t MAX_UNACKED = 512;           // the peer stopped acking: the transport fails (the session times out first in practice)
constexpr size_t RELIABLE_HEADER_BYTES = 5;   // id + u32 sequence number

// ---- PAD / PAD_ACK ----------------------------------------------------------------------------------------------------------
class PadLink
{
public:
  explicit PadLink(u8 local_port) : m_port(local_port) {}

  // ---- local side (CPU thread)
  // Queues the pad FOR `frame` (ascending, one call per frame; a frame not above the last queued one is ignored) and returns the PAD packet
  // to send now: every unacked pad (oldest first up to MAX_PADS_PER_PACKET), newest first on the wire, with the checksum header.
  std::vector<u8> QueueLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum, u64 now_us);
  // The re-send due at `now_us` (empty when nothing is unacked or the timer has not run out).
  std::vector<u8> PollResend(u64 now_us);
  size_t Unacked() const { return m_unacked.size(); }
  bool Overflowed() const { return m_unacked.size() > MAX_UNACKED; }
  u64 RttUs() const { return m_rtt_us; }      // smoothed ack round trip; 0 = no sample yet
  // M5 pacing: Slippi's per-received-pad time offset (TimeSync in Rev3Session.h). Taken at the first arrival of every newer remote frame, with the
  // local send time of our newest queued frame; positive = this machine is ahead.
  bool TimeOffsetUs(s32* out, u32* samples) const { return m_time.OffsetUs(out, samples); }
  void ResetTimeOffsets() { m_time.Reset(); }
  s32 AckedFrame() const { return m_acked; }  // hook frame; -1 = none

  // ---- receiving side (network thread)
  // Handles one received datagram payload. PAD: stores the pads and puts the ACK to send back in *reply; ACK: retires pads. False: not a
  // well-formed PAD / ACK of the peer (ignored; counted in BadPackets()).
  bool OnPacket(const u8* data, size_t size, u64 now_us, std::vector<u8>* reply);
  s32 RemoteConfirmed() const { return m_remote_r; }  // R (hook frame); -1 = none
  bool RemotePad(s32 frame, Pad8* out) const;
  std::vector<PeerChecksum> TakePeerChecksums();
  u32 BadPackets() const { return m_bad; }

private:
  struct Local
  {
    s32 frame = 0;
    Pad8 pad{};
    u64 first_sent_us = 0;
    u32 sent = 0;
    bool timer_resent = false;  // sent again by PollResend (no RTT sample from its ack, Karn)
  };
  struct Remote
  {
    s32 frame = -1;
    Pad8 pad{};
  };

  std::vector<u8> BuildPad(u64 now_us, bool timer_resend = false);

  u8 m_port;
  std::deque<Local> m_unacked;
  s32 m_last_queued = -1;
  s32 m_acked = -1;
  s32 m_chk_frame = -1;
  u32 m_chk_value = 0;
  u64 m_last_send_us = 0;
  u64 m_rtt_us = 0;
  std::array<Remote, REMOTE_RING> m_remote{};
  s32 m_remote_r = -1;
  TimeSync m_time;
  std::vector<PeerChecksum> m_peer_checks;
  s32 m_last_peer_chk_frame = -1;
  u32 m_last_peer_chk_value = 0;
  u32 m_bad = 0;
};

// ---- reliable messages 0x90..0x93 -------------------------------------------------------------------------------------------
// [id][u32 BE sequence][payload]
std::vector<u8> EncodeReliable(MsgType type, u32 sequence, const std::vector<u8>& payload);
bool DecodeReliable(const u8* data, size_t size, MsgType* type, u32* sequence, std::vector<u8>* payload);

// Delivers messages once and in sequence order however many copies arrive and in whatever order.
class ReliableInbox
{
public:
  void Add(MsgType type, u32 sequence, std::vector<u8> payload);
  bool Poll(Message* out);
  size_t Held() const { return m_held.size(); }

private:
  u32 m_next = 0;
  std::map<u32, Message> m_held;
};

// ---- options (environment) ---------------------------------------------------------------------------------------------------
struct PeerAddress
{
  std::string host;  // dotted quad or a host name
  u16 port = 0;
};
// "host:port" (the port after the LAST colon; IPv4 / names only). False with *error on anything else.
bool ParsePeerAddress(const std::string& text, PeerAddress* out, std::string* error);

struct NetOptions
{
  bool requested = false;  // REV3_NET_ROLE is set
  bool ok = false;         // requested and usable (role and peer valid)
  std::string error;       // why not ok
  std::vector<std::string> warnings;  // variables that were set but unusable (their default was used)
  Role role = Role::Host;
  u16 listen_port = 2626;  // REV3_NET_PORT: UDP port this side listens on and sends from
  PeerAddress peer;        // REV3_NET_PEER
  int delay = 2;           // REV3_NET_DELAY: input delay D in frames (the host's value wins)
  bool rollback = false;   // REV3_NET_ROLLBACK=1: the arm gate may open (default 0 = pure lockstep)
  int max_rollback = 5;    // REV3_NET_ROLLBACK_MAX: MAXR, 1..7
  u32 arm_calls = 600;     // REV3_NET_ARM: normal calls with both fighters found before the gate may open (SyncTest's default)
  u32 quiet_calls = 60;    // REV3_NET_QUIET: normal calls without a disc read in flight before the gate may open
  u32 connect_timeout_ms = 120000;  // REV3_NET_TIMEOUT_S * 1000: connect + HELLO (the frame-0 barrier)
  u32 wait_timeout_ms = 10000;      // REV3_NET_WAIT_MS: no remote pad this long in lockstep = the peer is gone
  std::string log_path;    // REV3_NET_LOG ("" = User\Logs\rev3_net_<role>.log)
  std::string stable_path; // REV3_NET_STABLE: {"ranges":[...]} whose CRC is compared at arm time (START_CHECK); "" = none
  std::string netsim;      // REV3_NETSIM (Rev3NetSim.h): delay_ms=..,jitter_ms=..,loss_pct=..,seed=.. ; "" = off
  u32 busy_calls = 1;      // REV3_NET_BUSY: consecutive armed calls with a disc read in flight that start a HOLD (predict nothing); 0 = never. 1 = from the first busy call,
                           // so that no frame is predicted while a read is in flight (a slot captured with a read in flight cannot be a rollback target)
  bool range_crc = false;  // REV3_NET_RANGE_CRC=1 (testing only): exchange the CRCs of the snapshot ranges at every checksum frame
  // M5 (REV3.md "Netplay (M5)"). Local to a machine (not part of HELLO) except the MAXR itself.
  PaceConfig pace;                 // REV3_NET_PACE=0|1 (default 1), REV3_NET_PACE_T1_US, REV3_NET_PACE_HARD_MAX_US, REV3_NET_PACE_SLICE_US, REV3_NET_PACE_GENTLE_PERMILLE
  bool maxr_adapt = false;         // REV3_NET_MAXR_ADAPT=1: the stall rule's limit follows the measured cost of resimulation (off by default)
  u32 maxr_budget_us = 14000;      // REV3_NET_MAXR_BUDGET_US: resimulation of K frames should cost less than this at its p95 (microseconds)
  bool pad_mirror = false;         // REV3_PAD_MIRROR=1 (guest only; the session ignores it on the host): the local pad is the newest confirmed remote pad. Local to a machine
};
using GetEnvFn = std::function<std::string(const char*)>;  // "" for unset
NetOptions ParseNetOptions(const GetEnvFn& get_env);
const char* RoleText(Role role);  // "host" / "guest"

// ---- HELLO ------------------------------------------------------------------------------------------------------------------
// What the Dolphin side reads from its configuration and the running game; BuildHello turns it into the items both machines compare.
struct HelloInputs
{
  std::string build;        // fork build id (Common::GetScmRevGitStr)
  std::string game_id;      // the running game's id ("RNEEDA")
  std::string cpu_core;     // "JIT64", "CachedInterpreter", ...
  bool cpu_thread = false;  // dual core
  bool sync_gpu = false;
  bool dsp_hle = true;
  bool dsp_thread = false;
  bool rtc_enabled = false;
  u32 rtc_value = 0;
  bool overclock_enabled = false;
  float overclock = 1.0f;
  bool mmu = false;
  bool fast_disc_speed = false;
  bool cheats = false;
  bool bluetooth_passthrough = false;
  std::array<int, 5> wiimote_sources{};  // Wii Remote 1..4 and the balance board: 0 none, 1 emulated, 2 real
  u32 hook_protocol = 2;    // the version byte of the hook's commands (2, or 3 = with the resimulation hiding patches); HELLO carries it, a mismatch refuses
  bool hook_crc_valid = false;
  u32 hook_crc = 0;         // CRC-32 of the hook code region 0x800024A0-0x8000291F once the game has booted
  bool rollback = false;    // REV3_NET_ROLLBACK
  int max_rollback = 5;     // REV3_NET_ROLLBACK_MAX: MAXR. The gate close is scheduled MAXR + 1 frames after a final frame, so both sides must use the same
  u32 arm_calls = 600;
  u32 quiet_calls = 60;
  u32 stable_crc = 0;       // CRC of the REV3_NET_STABLE ranges' file contents (0 = none), so both sides use the same ranges
};
constexpr u32 HOOK_CODE_START = 0x800024A0;  // rbhook.py: the hook's code (the data block 0x80002400-0x8000249F changes every frame)
constexpr u32 HOOK_CODE_END = 0x80002920;    // exclusive (0x8000291F inclusive)
Hello BuildHello(const HelloInputs& in, Role role, int delay);

// ---- the arm gate -----------------------------------------------------------------------------------------------------------
struct GateConfig
{
  bool rollback = false;  // false: the gate never opens
  u32 arm_calls = 600;
  u32 quiet_calls = 60;
  u32 hold_calls = 1;     // armed calls in a row with the hardware busy before a HOLD starts (0 = never); 1 = the first busy call: no frame is predicted while a read is in flight
};

struct GateInputs
{
  bool fighters_found = false;   // both fighters found through TEAMS in this call
  bool hardware_busy = false;    // a disc read is in flight right now (HardwareView::DiscReadInFlight)
  bool health_zero = false;      // a fighter's health (+0x28) is 0
  bool learning_done = false;    // the adaptive snapshot set has finished learning
  bool set_pending = false;      // slots are in use and the snapshot set would change (learned pages / new hardware cut)
  bool may_prepare = false;      // the session is in LOCKSTEP (not armed, not draining): the set may be installed now
};

struct GateDecision
{
  bool open = false;
  bool hardware_quiet = true;
  bool hold = false;            // armed: the hardware has been busy long enough (and not quiet for HOLD_RELEASE_CALLS yet)
  bool layout_pending = false;  // armed: the snapshot set would change
  const char* why = "";         // static text
};
constexpr u32 HOLD_RELEASE_CALLS = 2;  // a HOLD ends after this many calls in a row without a disc read in flight

// Feed it once per NORMAL hook call (not a resimulated one). `prepare` is called once when everything else allows opening and must install
// the snapshot set (SyncTest::NetPrepareArm); false keeps the gate shut until the fighters are lost and found again.
class GateTracker
{
public:
  explicit GateTracker(const GateConfig& config) : m_cfg(config) {}
  // LOCKSTEP (and the frame-0 barrier): may the gate open now?
  GateDecision Update(const GateInputs& in, const std::function<bool()>& prepare);
  // ARMED: the local conditions only (the session decides the close itself).
  GateDecision UpdateArmed(const GateInputs& in);
  // The session left the armed stretch (drained into lockstep): the gate is shut until it opens again through Update().
  void Close();
  bool open() const { return m_open; }

private:
  GateConfig m_cfg;
  u32 m_found_streak = 0;
  u32 m_quiet_streak = 0;
  u32 m_busy_streak = 0;     // armed calls in a row with a disc read in flight
  u32 m_release_streak = 0;  // armed calls in a row without one (ends a HOLD)
  bool m_holding = false;
  bool m_open = false;
  bool m_prepared = false;
  bool m_prepare_failed = false;
};
}  // namespace Rev3::Net
