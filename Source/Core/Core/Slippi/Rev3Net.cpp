// Rev3 netplay (milestone M3): ENet transport, SyncTest-backed snapshot store, the netplay command handler. See Rev3Net.h for what each
// part does and how it fits in. Licensed under GPLv2+.
//
// Layout of this file: (1) EnetTransport, (2) SyncTestStore, (3) NetModule.
// To change: the channel numbers and timeouts at the top; the HELLO contents live in Rev3NetLink.cpp (BuildHello) and are filled in
// Rev3Rollback.cpp (CollectHelloInputs).

#include "Core/Slippi/Rev3Net.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <utility>

#include <enet/enet.h>

#include "Common/ENet.h"
#include "Common/Hash.h"
#include "Common/Timer.h"
#include "Core/Slippi/Rev3NetSim.h"

namespace Rev3::Net
{
namespace
{
constexpr enet_uint8 CHANNEL_RELIABLE = 0;  // HELLO / START_CHECK / DESYNC_DUMP / BYE (ordered, reliable)
constexpr enet_uint8 CHANNEL_PAD = 1;       // PAD (unsequenced), Slippi's channel for pads
constexpr enet_uint8 CHANNEL_ACK = 2;       // PAD_ACK (unsequenced), Slippi's channel for acks
constexpr size_t CHANNEL_COUNT = 3;
constexpr size_t MAX_PEERS = 8;             // ENet peer slots: the two connections to the peer plus stragglers that are refused
constexpr u32 SERVICE_MS = 20;              // longest enet_host_service wait of the network thread (wake-up datagrams end it early)
constexpr u64 RECONNECT_MS = 1000;          // a connection attempt that failed is repeated after this long until the timeout
constexpr u32 LINGER_MS = 300;              // at shutdown: how long the network thread flushes and waits for the disconnect

std::string Fmt(const char* format, ...)
{
  char buf[512];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (n < 0)
    return {};
  return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
}

std::string AddressText(const ENetAddress& address)
{
  const u32 host = ENET_NET_TO_HOST_32(address.host);
  return Fmt("%u.%u.%u.%u:%u", (host >> 24) & 0xFF, (host >> 16) & 0xFF, (host >> 8) & 0xFF, host & 0xFF, static_cast<unsigned>(address.port));
}

// enet_peer_send takes ownership of the packet on success; on failure it is ours to free.
void SendPacket(ENetPeer* peer, enet_uint8 channel, const std::vector<u8>& bytes, enet_uint32 flags)
{
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (packet == nullptr)
    return;
  if (enet_peer_send(peer, channel, packet) != 0)
    enet_packet_destroy(packet);
}
}  // namespace

// ---- (1) EnetTransport -----------------------------------------------------------------------------------------------------------

EnetTransport::EnetTransport(const Config& config, Sink& sink)
    : m_cfg(config), m_sink(sink), m_link(config.role == Role::Host ? 0 : 1)
{
  if (enet_initialize() != 0)
  {
    PushEvent("ERROR NET enet_initialize failed");
    m_state.store(ConnState::Failed);
    FlushEvents();
    return;
  }
  m_enet_initialized = true;

  ENetAddress local;
  local.host = ENET_HOST_ANY;
  local.port = m_cfg.local_port;
  m_host = enet_host_create(&local, MAX_PEERS, CHANNEL_COUNT, 0, 0);
  if (m_host == nullptr)
  {
    PushEvent(Fmt("ERROR NET cannot listen on UDP port %u (already in use?)", static_cast<unsigned>(m_cfg.local_port)));
    m_state.store(ConnState::Failed);
    FlushEvents();
    return;
  }
  // The wake-up datagram (Common::ENet::WakeupThread) ends a service wait early; the callback swallows it.
  m_host->intercept = Common::ENet::InterceptCallback;

  ENetAddress peer{};
  if (enet_address_set_host(&peer, m_cfg.peer_host.c_str()) != 0)
  {
    PushEvent("ERROR NET cannot resolve the peer address " + m_cfg.peer_host);
    m_state.store(ConnState::Failed);
    FlushEvents();
    return;
  }
  peer.port = m_cfg.peer_port;
  m_peer_host_net = peer.host;
  m_peer_port_net = peer.port;
  PushEvent(Fmt("NET listening on UDP %u, connecting to %s (timeout %u s)", static_cast<unsigned>(m_cfg.local_port), AddressText(peer).c_str(),
                m_cfg.connect_timeout_ms / 1000));
  FlushEvents();
  m_thread = std::thread(&EnetTransport::ThreadMain, this);
}

EnetTransport::~EnetTransport()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
  }
  Wake();
  if (m_thread.joinable())
    m_thread.join();
  if (m_host != nullptr)
  {
    enet_host_destroy(m_host);
    m_host = nullptr;
  }
  if (m_enet_initialized)
    enet_deinitialize();
  FlushEvents();
}

std::string EnetTransport::PeerText() const
{
  return Fmt("%s:%u", m_cfg.peer_host.c_str(), static_cast<unsigned>(m_cfg.peer_port));
}

void EnetTransport::Wake()
{
  if (m_host != nullptr)
    Common::ENet::WakeupThread(m_host);
}

void EnetTransport::PushEvent(const std::string& text)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_events.push_back(text);
}

void EnetTransport::FlushEvents()
{
  std::vector<std::string> events;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    events.swap(m_events);
  }
  for (const std::string& e : events)
    m_sink.Line(e);
}

void EnetTransport::WaitActivity(u32 ms)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  if (!m_activity)
    m_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_activity; });
  m_activity = false;
}

u64 EnetTransport::RttUs() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.RttUs();
}

bool EnetTransport::TimeOffsetUs(s32* out, u32* samples) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.TimeOffsetUs(out, samples);
}

void EnetTransport::ResetTimeOffsets()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_link.ResetTimeOffsets();
}

u32 EnetTransport::BadPackets() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.BadPackets();
}

u64 EnetTransport::Now() const
{
  return Common::Timer::NowUs();
}

NetSimStats EnetTransport::NetSimCounts() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_sim_stats;
}

// One received datagram payload, on the network thread (with REV3_NETSIM: when the simulator releases it).
void EnetTransport::HandleReceived(ENetPeer* peer, const u8* data, size_t size)
{
  if (size >= 1 && (data[0] == WIRE_PAD || data[0] == WIRE_PAD_ACK))
  {
    std::vector<u8> reply;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_link.OnPacket(data, size, Common::Timer::NowUs(), &reply);
      m_activity = true;
    }
    m_cv.notify_all();
    if (!reply.empty())
      SendPacket(peer, CHANNEL_ACK, reply, ENET_PACKET_FLAG_UNSEQUENCED);
    return;
  }
  MsgType type = MsgType::Bye;
  u32 sequence = 0;
  std::vector<u8> payload;
  if (DecodeReliable(data, size, &type, &sequence, &payload))
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_inbox.Add(type, sequence, std::move(payload));
      m_activity = true;
    }
    m_cv.notify_all();
  }
}

void EnetTransport::SendLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<u8> packet = m_link.QueueLocal(frame, pad, checksum_frame, checksum, Now());
    if (!packet.empty())
      m_out.push_back({false, std::move(packet)});
    if (m_link.Overflowed() && m_state.load() == ConnState::Connected)
    {
      m_events.push_back(Fmt("ERROR NET the peer has not acked %zu pads: connection lost", m_link.Unacked()));
      m_state.store(ConnState::Failed);
    }
  }
  Wake();
}

s32 EnetTransport::RemoteConfirmed() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.RemoteConfirmed();
}

bool EnetTransport::RemotePad(s32 frame, Pad8* out) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.RemotePad(frame, out);
}

std::vector<PeerChecksum> EnetTransport::TakePeerChecksums()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_link.TakePeerChecksums();
}

void EnetTransport::SendReliable(MsgType type, const std::vector<u8>& bytes)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_out.push_back({true, EncodeReliable(type, m_next_sequence++, bytes)});
  }
  Wake();
}

bool EnetTransport::PollReliable(Message* out)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_inbox.Poll(out);
}

// The re-send timer of the unacked local pads (a blocked session queues nothing, so nothing else would send) and the network thread's log lines.
void EnetTransport::Pump()
{
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<u8> packet = m_link.PollResend(Now());
    if (!packet.empty())
    {
      m_out.push_back({false, std::move(packet)});
      queued = true;
    }
  }
  if (queued)
    Wake();
  FlushEvents();
}

void EnetTransport::ThreadMain()
{
  ENetAddress peer_address;
  peer_address.host = m_peer_host_net;
  peer_address.port = m_peer_port_net;

  std::vector<ENetPeer*> peers;  // connected, ours alone to touch
  ENetPeer* connecting = nullptr;
  bool ever_connected = false;
  bool duplicate_noted = false;
  u32 attempts = 0;
  const u64 start_ms = Common::Timer::NowMs();
  u64 next_attempt_ms = start_ms;
  // REV3_NETSIM: every received message waits here until its release time. The ENet connect handshake is below this layer.
  std::unique_ptr<NetSim> sim;
  if (m_cfg.netsim.Active())
    sim = std::make_unique<NetSim>(m_cfg.netsim, m_cfg.role == Role::Host ? 0u : 1u);

  const auto notify = [this] {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_activity = true;
    }
    m_cv.notify_all();
  };

  for (;;)
  {
    std::vector<Outgoing> out;
    bool stop = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      out.swap(m_out);
      stop = m_stop;
    }
    for (const Outgoing& o : out)
    {
      for (ENetPeer* peer : peers)
      {
        if (o.reliable)
          SendPacket(peer, CHANNEL_RELIABLE, o.bytes, ENET_PACKET_FLAG_RELIABLE);
        else
          SendPacket(peer, CHANNEL_PAD, o.bytes, ENET_PACKET_FLAG_UNSEQUENCED);
      }
    }
    if (stop)
      break;

    const u64 now_ms = Common::Timer::NowMs();
    if (!ever_connected)
    {
      if (now_ms - start_ms >= m_cfg.connect_timeout_ms)
      {
        PushEvent(Fmt("NET connect timeout after %u s: no connection to %s", m_cfg.connect_timeout_ms / 1000, PeerText().c_str()));
        m_state.store(ConnState::Failed);
        notify();
        break;
      }
      if ((connecting == nullptr || connecting->state == ENET_PEER_STATE_DISCONNECTED) && now_ms >= next_attempt_ms)
      {
        connecting = enet_host_connect(m_host, &peer_address, CHANNEL_COUNT, 0);
        next_attempt_ms = now_ms + RECONNECT_MS;
        ++attempts;
        if (attempts == 1 || attempts % 10 == 0)
          PushEvent(Fmt("NET connecting to %s (attempt %u)", AddressText(peer_address).c_str(), attempts));
      }
    }

    // With a simulation running the wait ends when the earliest held message is due.
    u32 wait_ms = SERVICE_MS;
    if (sim != nullptr && !sim->Empty())
    {
      const u64 now_us = Common::Timer::NowUs();
      const u64 due_us = sim->NextDueUs();
      wait_ms = due_us <= now_us ? 0 : static_cast<u32>(std::min<u64>(SERVICE_MS, (due_us - now_us + 999) / 1000));
    }
    ENetEvent event;
    int result = enet_host_service(m_host, &event, wait_ms);
    while (result > 0)
    {
      switch (event.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
      {
        if (event.peer->address.host != peer_address.host)
        {
          PushEvent("NET refused a connection from " + AddressText(event.peer->address) + " (not the peer)");
          enet_peer_reset(event.peer);
          break;
        }
        if (std::find(peers.begin(), peers.end(), event.peer) == peers.end())
          peers.push_back(event.peer);
        if (!ever_connected)
        {
          ever_connected = true;
          PushEvent(Fmt("NET connected to %s (%s connection, %llu ms after start)", AddressText(event.peer->address).c_str(),
                        event.peer == connecting ? "outgoing" : "incoming", static_cast<unsigned long long>(now_ms - start_ms)));
          m_state.store(ConnState::Connected);
        }
        else if (!duplicate_noted)
        {
          duplicate_noted = true;
          PushEvent("NET second connection to the peer (both sides connected at once); both are used, duplicates are dropped");
        }
        notify();
        break;
      }
      case ENET_EVENT_TYPE_RECEIVE:
      {
        const u8* data = event.packet->data;
        const size_t size = event.packet->dataLength;
        if (sim != nullptr)
        {
          const bool droppable = size >= 1 && (data[0] == WIRE_PAD || data[0] == WIRE_PAD_ACK);
          sim->Submit(Common::Timer::NowUs(), droppable, static_cast<u64>(reinterpret_cast<uintptr_t>(event.peer)), std::vector<u8>(data, data + size));
        }
        else
          HandleReceived(event.peer, data, size);
        enet_packet_destroy(event.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT:
      {
        peers.erase(std::remove(peers.begin(), peers.end(), event.peer), peers.end());
        if (event.peer == connecting)
          connecting = nullptr;
        if (peers.empty() && ever_connected && m_state.load() != ConnState::Failed)
        {
          PushEvent("NET the peer disconnected");
          m_state.store(ConnState::Failed);
          notify();
        }
        break;
      }
      default:
        break;  // Common::ENet::SKIPPABLE_EVENT (our own wake-up datagram)
      }
      result = enet_host_service(m_host, &event, 0);
    }
    if (result < 0)
    {
      PushEvent("ERROR NET enet_host_service failed");
      m_state.store(ConnState::Failed);
      notify();
      break;
    }
    if (sim != nullptr)
    {
      u64 tag = 0;
      std::vector<u8> bytes;
      const u64 now_us = Common::Timer::NowUs();
      while (sim->PopDue(now_us, &tag, &bytes))
      {
        ENetPeer* const peer = reinterpret_cast<ENetPeer*>(static_cast<uintptr_t>(tag));
        if (std::find(peers.begin(), peers.end(), peer) != peers.end())  // the connection it came over may be gone by now
          HandleReceived(peer, bytes.data(), bytes.size());
      }
      std::lock_guard<std::mutex> lock(m_mutex);
      m_sim_stats = sim->stats();
    }
  }

  // Shutdown: whatever was queued last (BYE) goes out, then a polite disconnect, then a bounded wait for ENet to finish it.
  {
    std::vector<Outgoing> out;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      out.swap(m_out);
    }
    for (const Outgoing& o : out)
      for (ENetPeer* peer : peers)
        SendPacket(peer, o.reliable ? CHANNEL_RELIABLE : CHANNEL_PAD, o.bytes, o.reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
  }
  for (ENetPeer* peer : peers)
    enet_peer_disconnect_later(peer, 0);
  const u64 linger_start = Common::Timer::NowMs();
  ENetEvent event;
  while (!peers.empty() && Common::Timer::NowMs() - linger_start < LINGER_MS)
  {
    if (enet_host_service(m_host, &event, 20) > 0)
    {
      if (event.type == ENET_EVENT_TYPE_RECEIVE)
        enet_packet_destroy(event.packet);
      else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        peers.erase(std::remove(peers.begin(), peers.end(), event.peer), peers.end());
    }
  }
  for (ENetPeer* peer : peers)
    enet_peer_reset(peer);
}

// ---- (2) SyncTestStore -----------------------------------------------------------------------------------------------------------

SyncTestStore::SyncTestStore(const GuestMemory& memory, SyncTest* synctest, std::vector<Range> stable)
    : m_memory(memory), m_sync(synctest), m_stable(std::move(stable))
{
}

void SyncTestStore::Record(s32 frame, const FieldSet& fields)
{
  if (frame < 0)
    return;
  Trace& t = m_trace[static_cast<size_t>(frame) % TRACE_FRAMES];
  t.frame = frame;
  t.fields = fields;
}

bool SyncTestStore::Capture(s32 frame, u32 sp, CaptureInfo* info)
{
  if (m_sync == nullptr || frame < 0)
    return false;
  SyncTest::NetSnapshotInfo raw;
  if (!m_sync->NetCapture(static_cast<u32>(frame), sp, &raw))
    return false;
  info->fields = FieldsFromRaw(raw.fields, 3);  // a capture needs both fighters
  info->checksum = ChecksumOfFields(frame, info->fields);
  Record(frame, info->fields);
  return true;
}

// False: the slot of `frame` was captured with a disc read in flight (hardware / IOS view or the game's DVD library), see SnapshotStore::Clean.
bool SyncTestStore::Clean(s32 frame) const
{
  return m_sync == nullptr || frame < 0 || m_sync->NetClean(static_cast<u32>(frame));
}

bool SyncTestStore::Has(s32 frame) const
{
  return m_sync != nullptr && frame >= 0 && m_sync->NetHas(static_cast<u32>(frame));
}

u32 SyncTestStore::SlotSp(s32 frame) const
{
  return m_sync != nullptr && frame >= 0 ? m_sync->NetSlotSp(static_cast<u32>(frame)) : 0;
}

bool SyncTestStore::Restore(s32 frame)
{
  return m_sync != nullptr && frame >= 0 && m_sync->NetRestore(static_cast<u32>(frame));
}

u32 SyncTestStore::LiveChecksum(s32 frame, FieldSet* fields)
{
  std::array<std::array<u8, FIELD_BYTES>, 2> raw{};
  u8 present = 0;
  ReadFighterFields(m_memory, &raw, &present);
  const FieldSet set = FieldsFromRaw(raw, present);
  if (fields != nullptr)
    *fields = set;
  if (present != 0)
    Record(frame, set);
  return ChecksumOfFields(frame, set);
}

void SyncTestStore::RecordLive(s32 frame)
{
  std::array<std::array<u8, FIELD_BYTES>, 2> raw{};
  u8 present = 0;
  if (ReadFighterFields(m_memory, &raw, &present) == 2)
    Record(frame, FieldsFromRaw(raw, present));
}

u32 SyncTestStore::StableCrc()
{
  return m_stable.empty() ? 0 : MemoryCrc(m_memory, m_stable);
}

bool SyncTestStore::Fields(s32 frame, FieldSet* out) const
{
  if (frame < 0)
    return false;
  const Trace& t = m_trace[static_cast<size_t>(frame) % TRACE_FRAMES];
  if (t.frame != frame)
    return false;
  *out = t.fields;
  return true;
}

void SyncTestStore::Clear()
{
  if (m_sync != nullptr)
    m_sync->NetClear();
}

// The snapshot set changed on this machine only: install it exactly like an arm does (hardware cuts re-checked, all 61 fields of both
// fighters verified to be inside it).
bool SyncTestStore::Relayout()
{
  return m_sync != nullptr && m_sync->NetPrepareArm();
}

// Before every SUMMARY line: `NOTE learned summary` (when the learned pages changed) and the restore verify / hiding counters for the line.
std::string SyncTestStore::SummaryNote()
{
  return m_sync != nullptr ? m_sync->NetSummaryNote() : std::string();
}

// After every SUMMARY line: the `SNAPSHOT kind=..` line (set size, write tracking counters, restore verify), as SyncTest writes it.
std::string SyncTestStore::AfterSummaryLine()
{
  return m_sync != nullptr ? m_sync->SnapshotLine() : std::string();
}

// REV3_NET_RANGE_CRC: the snapshot ranges (SyncTest's set); before the first arm, or in a lockstep-only run, the REV3_NET_STABLE ranges.
bool SyncTestStore::RangeCrcs(s32 frame, bool live, std::vector<RangeCrc>* out)
{
  if (m_sync != nullptr && frame >= 0 && m_sync->NetRangeCrcs(static_cast<u32>(frame), live, out))
    return true;
  if (!live || m_stable.empty())
    return false;
  out->clear();
  for (const Range& r : m_stable)
  {
    RangeCrc e;
    e.start = r.start;
    e.end = r.end;
    e.crc = MemoryCrc(m_memory, {r});
    out->push_back(e);
  }
  return true;
}

// ---- (3) NetModule -----------------------------------------------------------------------------------------------------------------

NetModule::NetModule(const NetOptions& options, const NetEnvironment& env, GuestMemory& memory, HardwareView* hardware, SyncTest* synctest,
                     std::vector<Range> stable_ranges, Sink& net_log)
    : m_options(options), m_env(env), m_memory(memory), m_hardware(hardware), m_sync(synctest), m_stable(std::move(stable_ranges)),
      m_log(net_log),
      m_gate_tracker(GateConfig{options.rollback && synctest != nullptr, options.arm_calls, options.quiet_calls, options.busy_calls}),
      m_hiding(env.hider, net_log)
{
  if (m_sync == nullptr)
    m_options.rollback = false;
  m_last_gate.why = "rollback off";
}

NetModule::~NetModule()
{
  Shutdown();
}

bool NetModule::Resimulating() const
{
  return m_session != nullptr && m_session->Resimulating();
}

// The session's blocking wait: the predicate (BlockSatisfied: pumps the transport and the messages) every time the network thread reports
// something and at least every millisecond; ends early, leaving the session, when the emulator is stopping (a closing window must never
// find the CPU thread stuck here).
bool NetModule::Wait(const std::function<bool()>& pred, u32 timeout_ms)
{
  // No hook command arrives while this loop waits for the peer, on purpose: the freeze dump's watchdog must not report it.
  struct BlockedScope
  {
    explicit BlockedScope(FreezeDump* d) : dump(d)
    {
      if (dump != nullptr)
        dump->SetBlocked(true);
    }
    ~BlockedScope()
    {
      if (dump != nullptr)
        dump->SetBlocked(false);
    }
    BlockedScope(const BlockedScope&) = delete;
    BlockedScope& operator=(const BlockedScope&) = delete;
    FreezeDump* dump;
  } blocked(m_env.dump);
  const u64 start = m_transport->Now();
  for (;;)
  {
    if (pred())
      return true;
    if (m_env.stopping && m_env.stopping())
    {
      m_session->Abort("the emulator is stopping");
      return false;
    }
    if (m_transport->Now() - start >= static_cast<u64>(timeout_ms) * 1000)
      return false;
    m_transport->WaitActivity(1);
  }
}

void NetModule::Start(const Command& command)
{
  m_started = true;
  if (command.frame != 0)
  {
    m_log.Line(Fmt("ERROR NET the first command is frame %u, not 0: a session cannot start mid-game; netplay is off for this run", command.frame));
    m_log.Osd("Rev3 netplay: the game was already running when the hook started (first frame is not 0); playing offline");
    return;
  }

  HelloInputs hello_inputs = m_env.hello;
  std::vector<u8> code(HOOK_CODE_END - HOOK_CODE_START);
  if (m_memory.Read(HOOK_CODE_START, code.data(), code.size()))
  {
    hello_inputs.hook_crc = Common::ComputeCRC32(code.data(), code.size());
    hello_inputs.hook_crc_valid = true;
  }
  hello_inputs.hook_protocol = command.protocol;  // 2 or 3: both machines must run the same patch set (HELLO compares it)
  hello_inputs.rollback = m_options.rollback;
  hello_inputs.max_rollback = m_options.max_rollback;
  hello_inputs.arm_calls = m_options.arm_calls;
  hello_inputs.quiet_calls = m_options.quiet_calls;

  EnetTransport::Config net;
  net.role = m_options.role;
  net.local_port = m_options.listen_port;
  net.peer_host = m_options.peer.host;
  net.peer_port = m_options.peer.port;
  net.connect_timeout_ms = m_options.connect_timeout_ms;
  if (!m_options.netsim.empty())
  {
    std::string error;
    if (!ParseNetSim(m_options.netsim, &net.netsim, &error))
    {
      m_log.Line("ERROR NET REV3_NETSIM=" + m_options.netsim + ": " + error + "; no simulation (the run is NOT the test that was asked for)");
      net.netsim = NetSimConfig();
    }
    else if (net.netsim.Active())
    {
      m_log.Line("NET netsim " + NetSimText(net.netsim));
    }
    else
    {
      m_log.Line("NOTE REV3_NETSIM=" + m_options.netsim + " has no delay, jitter or loss: nothing is simulated");
    }
  }
  m_transport = std::make_unique<EnetTransport>(net, m_log);
  if (!m_transport->Usable())
  {
    m_log.Line("ERROR NET the transport could not start; netplay is off for this run");
    m_log.Osd("Rev3 netplay: cannot open the network connection (see the net log); playing offline");
    m_transport.reset();
    return;
  }

  m_store = std::make_unique<SyncTestStore>(m_memory, m_sync, m_stable);
  Config config;
  config.role = m_options.role;
  config.delay = m_options.delay;
  config.max_rollback = m_options.max_rollback;
  config.barrier_timeout_ms = m_options.connect_timeout_ms;
  config.wait_timeout_ms = m_options.wait_timeout_ms;
  config.range_crc = m_options.range_crc;
  config.pace = m_options.pace;
  config.maxr_adapt = m_options.maxr_adapt;
  config.maxr_budget_us = m_options.maxr_budget_us;
  config.pad_mirror = m_options.pad_mirror;  // the session ignores it on the host (and says so)
  config.hello = BuildHello(hello_inputs, m_options.role, m_options.delay);
  m_log.Line(Fmt("NET start role=%s listen=%u peer=%s delay=%d rollback=%d maxr=%d gate=arm:%u,quiet:%u game=%s build=%s",
                 RoleText(m_options.role), static_cast<unsigned>(m_options.listen_port), m_transport->PeerText().c_str(), m_options.delay,
                 m_options.rollback ? 1 : 0, m_options.max_rollback, m_options.arm_calls, m_options.quiet_calls,
                 hello_inputs.game_id.c_str(), hello_inputs.build.c_str()));
  if (m_options.range_crc)
    m_log.Line("NOTE REV3_NET_RANGE_CRC=1: the CRCs of the snapshot ranges are exchanged at every checksum frame (testing only; differences are logged, never a desync)");
  m_session = std::make_unique<RollbackSession>(
      config, *m_transport, *m_store, m_log, [this](const std::function<bool()>& pred, u32 timeout_ms) { return Wait(pred, timeout_ms); });
  m_session->SetPaceActuator(m_env.pacer);
  m_log.Line(Fmt("NET pacing %s%s (hard %d frames, t1 %d us, hard max %d us, gentle max %d permille, slice %d us)%s", config.pace.enabled ? "on" : "off (measure only)",
                 config.pace.enabled && m_env.pacer == nullptr ? ", no actuator" : "", config.pace.hard_frames, config.pace.t1_us, config.pace.hard_max_us,
                 config.pace.gentle_max_permille, config.pace.slice_us,
                 config.maxr_adapt ? Fmt("; adaptive MAXR on (budget %u us)", config.maxr_budget_us).c_str() : ""));
  m_log.Line("NET waiting for the peer: the game is held at frame 0 until both machines are connected and their HELLOs match");
}

bool NetModule::HealthZero() const
{
  if (!m_sync->NetFightersFound())
    return false;
  for (u32 fighter : m_sync->NetFighters())
  {
    u8 word[4];
    if (!m_memory.Read(fighter + 0x28, word, 4))  // rbchecksum.py "health" (u32)
      continue;
    if ((word[0] | word[1] | word[2] | word[3]) == 0)
      return true;
  }
  return false;
}

// The gate of one hook call. Every call carries the live-state flags of the fighters (the session records them with the capture of the frame and
// decides on final frames only, Rev3Session.h THE GATE RULE). The rest depends on the session's mode and, being derived from the live state or
// from this machine's hardware, is read on normal calls only; a resimulated call reuses the last one.
Gate NetModule::EvaluateGate(Mode mode, bool resimulating)
{
  Gate gate = resimulating ? m_last_gate : Gate();
  if (!resimulating)
    gate.why = "rollback off";
  if (!m_options.rollback || m_sync == nullptr)
    return gate;

  gate.fighters_found = m_sync->NetFightersFound();
  gate.health_zero = HealthZero();
  if (resimulating)
    return gate;

  GateInputs in;
  in.fighters_found = gate.fighters_found;
  // Busy = a disc read in flight as the hardware / IOS see it OR as the game's own DVD library does (a command executing): a frame played in
  // either state could later be restored into a game that waits for a completion that has already happened.
  in.hardware_busy = (m_hardware != nullptr && m_hardware->DiscReadInFlight()) || m_sync->GameDvdBusy();
  in.health_zero = gate.health_zero;
  in.learning_done = m_sync->LearningDone();
  in.set_pending = m_sync->NetLayoutPending();
  in.may_prepare = mode == Mode::LOCKSTEP;
  GateDecision decision;
  switch (mode)
  {
  case Mode::ARMED:
    decision = m_gate_tracker.UpdateArmed(in);  // the close is the session's (final frames); only the local conditions come from here
    break;
  case Mode::DRAIN:
    decision.hardware_quiet = !in.hardware_busy;
    decision.why = "drain";
    break;
  case Mode::LOCKSTEP:
  case Mode::CONNECTING:
  case Mode::OFFLINE:
    decision = m_gate_tracker.Update(in, [this] { return m_sync->NetPrepareArm(); });
    break;
  }
  gate.open = decision.open;
  gate.hardware_quiet = decision.hardware_quiet;
  gate.hardware_hold = decision.hold;
  gate.layout_pending = decision.layout_pending;
  gate.why = decision.why;
  return gate;
}

Reply NetModule::OnCommand(const Command& command)
{
  if (!m_started)
    Start(command);
  if (m_session == nullptr)
  {
    Reply echo;
    echo.frame = command.frame;
    echo.pads = command.pads;
    return echo;
  }

  const bool resim = m_session->Resimulating();
  const Mode before = m_session->mode();
  if (m_sync != nullptr)
    m_sync->NetObserve(command, resim);
  if (!resim && before == Mode::LOCKSTEP && m_store != nullptr)
    m_store->RecordLive(static_cast<s32>(command.frame));  // the start-of-frame fields, for a desync dump taken in lockstep
  const Gate gate = EvaluateGate(before, resim);
  m_last_gate = gate;
  const Reply reply = m_session->OnCommand(command, gate);
  const Mode after = m_session->mode();
  // The session ended on this call (left, desync, timeout): nobody uses the snapshots any more, so host write tracking stops for good (the guest
  // RAM is writable again at once; the SUMMARY the session wrote already carries the counters).
  if (after == Mode::OFFLINE && before != Mode::OFFLINE && m_sync != nullptr)
    m_sync->NetLeave();
  // An armed stretch that ended (DRAIN done): the tracker must see the gate shut to open it again later.
  if (after == Mode::LOCKSTEP && (before == Mode::ARMED || before == Mode::DRAIN))
    m_gate_tracker.Close();
  // v1 hiding: the reply of a rollback starts the resimulation (throttle off + mute); the reply of its last frame, or the session
  // leaving, ends it.
  m_hiding.Update(m_session->Resimulating());
  if (before == Mode::CONNECTING && after != Mode::CONNECTING && after != Mode::OFFLINE)
  {
    m_log.Line(Fmt("NET connected to %s: %s, input delay %d, %s", m_transport->PeerText().c_str(), RoleText(m_options.role), m_session->delay(),
                   m_options.rollback ? "rollback enabled (gate)" : "lockstep only"));
    m_log.OsdInfo(Fmt("Rev3 netplay: connected to %s (%s, input delay %d)", m_transport->PeerText().c_str(), RoleText(m_options.role),
                      m_session->delay()));
  }
  return reply;
}

void NetModule::Shutdown()
{
  if (m_shut_down)
    return;
  m_shut_down = true;
  m_hiding.Update(false);  // never leave the throttle off or the sound muted behind
  if (m_session != nullptr)
  {
    if (m_session->mode() != Mode::OFFLINE)
      m_session->Abort(m_exit_reason);  // BYE to the peer, SUMMARY
    m_session->Finish();
  }
  if (m_sync != nullptr && m_started && m_session != nullptr)
  {
    m_sync->NetLeave();  // host write tracking off (counters stay for the lines below)
    m_sync->NetFlushLog();
    m_sync->LearnedSummaryLine();  // the session may have left (final SUMMARY already written) before pages were learned
    m_log.Line(m_sync->NetStatsLine());
    m_log.Line(m_sync->SnapshotLine());
    if (m_sync->Verifier() != nullptr)
      m_log.Line(m_sync->VerifySummaryText());
  }
  if (m_transport != nullptr)
  {
    m_log.Line(Fmt("NET closing: rtt_us=%llu bad_packets=%u", static_cast<unsigned long long>(m_transport->RttUs()), m_transport->BadPackets()));
    m_session.reset();  // before the transport and the store it points at
    m_transport.reset();
  }
  m_session.reset();
  m_store.reset();
}
}  // namespace Rev3::Net
