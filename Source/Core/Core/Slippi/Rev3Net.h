// Rev3 netplay (milestone M3): the ENet transport, the SyncTest-backed snapshot store and the 0xA0 command handler of a netplay run.
// Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT DOES
//   EnetTransport    Rev3::Net::Transport on ENet (the library Slippi's SlippiNetplayClient uses), without any of Slippi's online stack: no
//                    matchmaking, no user / login, no Rust objects, no spectator, no replay writing, no reporting. Direct connect only: both
//                    emulators connect to each other at the same time from their fixed UDP ports (hole-punching style; the two connections that
//                    result are both used, duplicates are dropped by the sequence numbers of Rev3NetLink.h). A connection from any other
//                    address is refused, so a packet capture shows only the peer. PAD / ACK packets: Rev3NetLink.h PadLink. Reliable messages
//                    0x90..0x93 on channel 0, PAD on channel 1 (unsequenced), ACK on channel 2 (unsequenced), like Slippi. One network thread owns
//                    the ENet host; the CPU thread talks to it through a mutex-guarded PadLink / inbox / outbox, and a condition variable wakes
//                    a waiting CPU thread the moment something arrives (WaitActivity).
//   SyncTestStore    Rev3::Net::SnapshotStore on Rev3::SyncTest's network mode (SyncTest::Net* in Rev3Rollback.h): the same capture, restore,
//                    adaptive set, hardware cuts, JIT invalidation and audio hardware rewind as the self test, plus a field trace of the last 64
//                    frames for desync dumps. Without a SyncTest (lockstep only) it still answers the live checksums.
//   NetModule        everything a netplay run needs per 0xA0 command: starts the transport and the RollbackSession at the first command (which
//                    must be frame 0), evaluates the arm gate and the per-call fighter flags, blocks while the session waits (WaitFn:
//                    condition variable + stop check, so closing the window never hangs), hides resimulated frames (ResimHiding: throttle
//                    off + mute, v1), writes the net log.
//   Pacing (M5)      EnetTransport answers TimeOffsetUs() from PadLink's TimeSync (arrival of every newer remote pad against our newest send, RTT/2
//                    corrected); NetModule hands the session a PaceActuator (Rev3Rollback.cpp's DolphinPacer) and the pacing / adaptive MAXR options.
//   REV3_NETSIM      (M4) EnetTransport holds every received message through a NetSim (Rev3NetSim.h) before it is processed: delay, jitter,
//                    loss of PAD / ACK packets; the ENet connect handshake is not touched.
//
// HOW IT FITS IN
//   Rev3Rollback.cpp (HandleCommand) creates a NetModule when REV3_NET_ROLE is set and hands it every command after the determinism kit has
//   substituted the scripted pads. Rev3Session.h is the state machine, Rev3NetLink.h the pure pieces. Threads: the CPU thread (inside the EXI
//   DMA handler) calls everything except EnetTransport's network thread, which only touches ENet and the mutex-guarded link state.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Slippi/Rev3NetLink.h"
#include "Core/Slippi/Rev3Rollback.h"
#include "Core/Slippi/Rev3Session.h"

struct _ENetHost;
struct _ENetPeer;

namespace Rev3::Net
{
// ---- transport -------------------------------------------------------------------------------------------------------------
class EnetTransport final : public Transport
{
public:
  struct Config
  {
    Role role = Role::Host;
    u16 local_port = 2626;
    std::string peer_host;
    u16 peer_port = 2627;
    u32 connect_timeout_ms = 120000;
    NetSimConfig netsim;  // REV3_NETSIM; inactive = messages are processed as they arrive
  };

  // Starts connecting at once (the network thread runs from here on). `sink` is only used on the calling (CPU) thread: network events are
  // queued and written to it from Pump().
  EnetTransport(const Config& config, Sink& sink);
  ~EnetTransport() override;
  EnetTransport(const EnetTransport&) = delete;
  EnetTransport& operator=(const EnetTransport&) = delete;

  // False when the UDP port could not be opened or the peer name did not resolve (State() is Failed then).
  bool Usable() const { return m_host != nullptr && m_state.load() != ConnState::Failed; }
  std::string PeerText() const;
  // CPU thread: block up to `ms` for the network thread to receive something (or a connection event); returns at once when something
  // arrived since the last call.
  void WaitActivity(u32 ms);
  u64 RttUs() const override;
  u32 BadPackets() const;
  bool TimeOffsetUs(s32* out, u32* samples) const override;
  void ResetTimeOffsets() override;
  NetSimStats NetSimCounts() const override;

  // Transport
  void SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum) override;
  s32 RemoteConfirmed() const override;
  bool RemotePad(s32 frame, Pad8* out) const override;
  std::vector<PeerChecksum> TakePeerChecksums() override;
  void SendReliable(MsgType type, const std::vector<u8>& bytes) override;
  bool PollReliable(Message* out) override;
  u64 Now() const override;
  ConnState State() const override { return m_state.load(); }
  void Pump() override;

private:
  struct Outgoing
  {
    bool reliable = false;
    std::vector<u8> bytes;
  };

  void ThreadMain();
  // Processes one received datagram payload (PAD / ACK / reliable), from the network thread; `peer` is where an ACK goes back to.
  void HandleReceived(_ENetPeer* peer, const u8* data, size_t size);
  void Wake();
  void PushEvent(const std::string& text);  // network thread or CPU thread; written to the sink by Pump()
  void FlushEvents();

  Config m_cfg;
  Sink& m_sink;
  _ENetHost* m_host = nullptr;
  bool m_enet_initialized = false;
  std::thread m_thread;
  std::atomic<ConnState> m_state{ConnState::Connecting};

  mutable std::mutex m_mutex;  // guards everything below
  std::condition_variable m_cv;
  bool m_activity = false;
  bool m_stop = false;
  PadLink m_link;
  ReliableInbox m_inbox;
  u32 m_next_sequence = 0;
  std::vector<Outgoing> m_out;
  std::vector<std::string> m_events;
  NetSimStats m_sim_stats;  // copy of the network thread's NetSim counters (under m_mutex)
  // The resolved peer address (ENet's, network byte order host); set before the thread starts.
  u32 m_peer_host_net = 0;
  u16 m_peer_port_net = 0;
};

// ---- snapshots -------------------------------------------------------------------------------------------------------------
class SyncTestStore final : public SnapshotStore
{
public:
  // `synctest` may be null (lockstep only: nothing is captured, the live checksums still work). `stable` are the ranges whose CRC START_CHECK
  // compares (REV3_NET_STABLE); empty = StableCrc() is 0. Not owned.
  SyncTestStore(const GuestMemory& memory, SyncTest* synctest, std::vector<Range> stable);

  bool Capture(s32 frame, u32 sp, CaptureInfo* info) override;
  bool Has(s32 frame) const override;
  u32 SlotSp(s32 frame) const override;
  bool Restore(s32 frame) override;
  u32 LiveChecksum(s32 frame, FieldSet* fields) override;
  u32 StableCrc() override;
  bool Fields(s32 frame, FieldSet* out) const override;
  void Clear() override;
  bool Relayout() override;
  bool RangeCrcs(s32 frame, bool live, std::vector<RangeCrc>* out) override;
  std::string SummaryNote() override;

  // Adds the live fighters' fields to the trace without a snapshot (lockstep calls: a desync dump then has every frame).
  void RecordLive(s32 frame);

  static constexpr size_t TRACE_FRAMES = 64;  // >= 31 (the dump covers CHECK_INTERVAL + 1 frames)

private:
  struct Trace
  {
    s32 frame = -1;
    FieldSet fields;
  };
  void Record(s32 frame, const FieldSet& fields);

  const GuestMemory& m_memory;
  SyncTest* m_sync;
  std::vector<Range> m_stable;
  std::array<Trace, TRACE_FRAMES> m_trace{};
};

// ---- the module ------------------------------------------------------------------------------------------------------------
struct NetEnvironment
{
  std::function<bool()> stopping;  // true once the emulator is shutting down (a blocked wait must end)
  HelloInputs hello;               // everything the Dolphin side knows; the module adds the hook CRC at the first command
  ResimHider* hider = nullptr;     // v1 hiding of resimulated frames (throttle off + mute); null = none. Not owned.
  FreezeDump* dump = nullptr;      // freeze dump: marked blocked while a wait loop waits for the peer (not a freeze); null = none. Not owned.
  PaceActuator* pacer = nullptr;   // M5: carries out the pacing delays (sleep + restart of Dolphin's throttle clock); null = pacing only measures. Not owned.
};

class NetModule
{
public:
  // `synctest` is the network-mode snapshot machinery (null: lockstep only, options.rollback is then treated as 0); `hardware` may be null.
  // `net_log` receives every session line and the NET lines. None of them is owned; all must outlive the module.
  NetModule(const NetOptions& options, const NetEnvironment& env, GuestMemory& memory, HardwareView* hardware, SyncTest* synctest,
            std::vector<Range> stable_ranges, Sink& net_log);
  ~NetModule();

  // True while the session is re-running frames after a RESTORED reply (the determinism kit neither records nor traces those calls).
  bool Resimulating() const;
  bool ResimHidden() const { return m_hiding.hidden(); }
  // One 0xA0 command in (the pads already substituted by the kit), the reply out. Blocks while the session waits for the peer.
  Reply OnCommand(const Command& command);
  // Leaves the session (BYE to the peer), writes the final lines, stops the network thread. Idempotent.
  void Shutdown();

private:
  void Start(const Command& command);
  Gate EvaluateGate(Mode mode, bool resimulating);
  bool HealthZero() const;
  bool Wait(const std::function<bool()>& pred, u32 timeout_ms);

  NetOptions m_options;
  NetEnvironment m_env;
  GuestMemory& m_memory;
  HardwareView* m_hardware;
  SyncTest* m_sync;
  std::vector<Range> m_stable;
  Sink& m_log;

  bool m_started = false;
  bool m_shut_down = false;
  GateTracker m_gate_tracker;
  ResimHiding m_hiding;
  Gate m_last_gate;
  std::unique_ptr<EnetTransport> m_transport;
  std::unique_ptr<SyncTestStore> m_store;
  std::unique_ptr<RollbackSession> m_session;
};
}  // namespace Rev3::Net
