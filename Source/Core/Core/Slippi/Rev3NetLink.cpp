// Rev3 netplay, pure parts (milestone M3): PAD / PAD_ACK link, reliable message envelope, options, HELLO items, arm gate.
// See Rev3NetLink.h for what each piece does and how it fits in. Licensed under GPLv2+. No sockets, no clock, no Dolphin core.

#include "Core/Slippi/Rev3NetLink.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace Rev3::Net
{
namespace
{
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

void PutU32(std::vector<u8>* out, u32 v)
{
  out->push_back(static_cast<u8>(v >> 24));
  out->push_back(static_cast<u8>(v >> 16));
  out->push_back(static_cast<u8>(v >> 8));
  out->push_back(static_cast<u8>(v));
}

u32 GetU32(const u8* p)
{
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

s32 GetS32(const u8* p)
{
  return static_cast<s32>(GetU32(p));
}
}  // namespace

const char* RoleText(Role role)
{
  return role == Role::Host ? "host" : "guest";
}

// ---- PadLink ----------------------------------------------------------------------------------------------------------------

std::vector<u8> PadLink::QueueLocal(s32 frame, const Pad8& pad, s32 checksum_frame, u32 checksum, u64 now_us)
{
  m_chk_frame = checksum_frame;
  m_chk_value = checksum;
  if (frame <= m_last_queued)
    return {};
  m_last_queued = frame;
  m_time.OnLocalSend(frame, now_us);
  Local entry;
  entry.frame = frame;
  entry.pad = pad;
  m_unacked.push_back(entry);
  return BuildPad(now_us);
}

std::vector<u8> PadLink::PollResend(u64 now_us)
{
  if (m_unacked.empty())
    return {};
  const u64 interval = std::max(RESEND_MIN_US, m_rtt_us);
  if (now_us >= m_last_send_us && now_us - m_last_send_us < interval)
    return {};
  return BuildPad(now_us, true);
}

// Oldest unacked pads first (the receiver needs them contiguous), at most MAX_PADS_PER_PACKET; the header names the newest of them and the
// pads follow newest first, exactly Slippi's SendSlippiPad layout.
std::vector<u8> PadLink::BuildPad(u64 now_us, bool timer_resend)
{
  const size_t count = std::min(m_unacked.size(), MAX_PADS_PER_PACKET);
  std::vector<u8> out;
  out.reserve(PAD_HEADER_BYTES + count * PAD_BYTES);
  out.push_back(WIRE_PAD);
  PutU32(&out, static_cast<u32>(m_unacked[count - 1].frame + WIRE_FRAME_OFFSET));
  out.push_back(m_port);
  PutU32(&out, static_cast<u32>(m_chk_frame));
  PutU32(&out, m_chk_value);
  for (size_t i = count; i-- > 0;)
    out.insert(out.end(), m_unacked[i].pad.begin(), m_unacked[i].pad.end());
  for (size_t i = 0; i < count; ++i)
  {
    if (m_unacked[i].sent == 0)
      m_unacked[i].first_sent_us = now_us;
    ++m_unacked[i].sent;
    if (timer_resend)
      m_unacked[i].timer_resent = true;
  }
  m_last_send_us = now_us;
  return out;
}

bool PadLink::OnPacket(const u8* data, size_t size, u64 now_us, std::vector<u8>* reply)
{
  reply->clear();
  if (size < 1)
    return false;
  if (data[0] == WIRE_PAD)
  {
    if (size < PAD_HEADER_BYTES + PAD_BYTES)
    {
      ++m_bad;
      return false;
    }
    const s32 newest_wire = GetS32(data + 1);
    const u8 port = data[5];
    const s32 checksum_frame = GetS32(data + 6);
    const u32 checksum = GetU32(data + 10);
    if (port > 1 || port == m_port || newest_wire < WIRE_FRAME_OFFSET)
    {
      ++m_bad;
      return false;
    }
    m_time.OnRemote(newest_wire - WIRE_FRAME_OFFSET, now_us, m_rtt_us);
    const size_t count = (size - PAD_HEADER_BYTES) / PAD_BYTES;
    for (size_t i = 0; i < count; ++i)
    {
      const s64 frame = static_cast<s64>(newest_wire) - static_cast<s64>(i) - WIRE_FRAME_OFFSET;
      if (frame < 0 || frame <= m_remote_r)
        break;  // this and every older pad of the packet is already here
      if (frame > static_cast<s64>(m_remote_r) + REMOTE_AHEAD)
        continue;  // too far ahead of what we hold: it will be re-sent when the acks catch up
      Remote& slot = m_remote[static_cast<size_t>(frame) % REMOTE_RING];
      slot.frame = static_cast<s32>(frame);
      std::copy_n(data + PAD_HEADER_BYTES + i * PAD_BYTES, PAD_BYTES, slot.pad.begin());
    }
    while (m_remote[static_cast<size_t>(m_remote_r + 1) % REMOTE_RING].frame == m_remote_r + 1)
      ++m_remote_r;
    if (checksum_frame >= 0 && (checksum_frame != m_last_peer_chk_frame || checksum != m_last_peer_chk_value))
    {
      m_last_peer_chk_frame = checksum_frame;
      m_last_peer_chk_value = checksum;
      if (m_peer_checks.size() < 64)
        m_peer_checks.push_back({checksum_frame, checksum});
    }
    if (m_remote_r >= 0)
    {
      reply->push_back(WIRE_PAD_ACK);
      PutU32(reply, static_cast<u32>(m_remote_r + WIRE_FRAME_OFFSET));
      reply->push_back(m_port);
    }
    return true;
  }
  if (data[0] == WIRE_PAD_ACK)
  {
    if (size < ACK_BYTES || data[5] > 1 || data[5] == m_port)
    {
      ++m_bad;
      return false;
    }
    const s32 acked = GetS32(data + 1) - WIRE_FRAME_OFFSET;
    if (acked <= m_acked)
      return true;
    while (!m_unacked.empty() && m_unacked.front().frame <= acked)
    {
      const Local& front = m_unacked.front();
      // The receiver acks R when a packet makes it contiguous, normally the first packet that carried the pad, so the sample is the time since
      // the pad's FIRST send. A pad also travels in every later packet while it is unacked (that is the protocol, not a retransmission) and
      // must not rule the sample out: with a packet per frame and an RTT of several frames that was every pad, and the estimate stayed at its
      // last lockstep value for the whole armed stretch (found in M5; pacing subtracts RTT/2). Karn's rule applies to the TIMER resend only
      // (PollResend): after one, an ack cannot be matched to one send.
      if (front.frame == acked && !front.timer_resent && now_us >= front.first_sent_us)
      {
        const u64 sample = now_us - front.first_sent_us;
        m_rtt_us = m_rtt_us == 0 ? sample : (7 * m_rtt_us + sample) / 8;
      }
      m_unacked.pop_front();
    }
    m_acked = acked;
    return true;
  }
  return false;
}

bool PadLink::RemotePad(s32 frame, Pad8* out) const
{
  if (frame < 0 || frame > m_remote_r || m_remote_r - frame > REMOTE_KEEP)
    return false;
  const Remote& slot = m_remote[static_cast<size_t>(frame) % REMOTE_RING];
  if (slot.frame != frame)
    return false;
  *out = slot.pad;
  return true;
}

std::vector<PeerChecksum> PadLink::TakePeerChecksums()
{
  std::vector<PeerChecksum> out;
  out.swap(m_peer_checks);
  return out;
}

// ---- reliable messages --------------------------------------------------------------------------------------------------------

std::vector<u8> EncodeReliable(MsgType type, u32 sequence, const std::vector<u8>& payload)
{
  std::vector<u8> out;
  out.reserve(RELIABLE_HEADER_BYTES + payload.size());
  out.push_back(static_cast<u8>(type));
  PutU32(&out, sequence);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool DecodeReliable(const u8* data, size_t size, MsgType* type, u32* sequence, std::vector<u8>* payload)
{
  if (size < RELIABLE_HEADER_BYTES || data[0] < static_cast<u8>(MsgType::Hello) || data[0] > static_cast<u8>(MsgType::ArmReject))  // 0x90..0x97 (ARM_* 0x95..0x97 added Oct 17)
    return false;
  *type = static_cast<MsgType>(data[0]);
  *sequence = GetU32(data + 1);
  payload->assign(data + RELIABLE_HEADER_BYTES, data + size);
  return true;
}

void ReliableInbox::Add(MsgType type, u32 sequence, std::vector<u8> payload)
{
  if (sequence < m_next || m_held.size() >= 256)  // already delivered (the second copy), or a flood
    return;
  Message message;
  message.type = type;
  message.bytes = std::move(payload);
  m_held.emplace(sequence, std::move(message));  // a second copy of a held sequence number is dropped by emplace
}

bool ReliableInbox::Poll(Message* out)
{
  const auto it = m_held.find(m_next);
  if (it == m_held.end())
    return false;
  *out = std::move(it->second);
  m_held.erase(it);
  ++m_next;
  return true;
}

// ---- options ----------------------------------------------------------------------------------------------------------------

bool ParsePeerAddress(const std::string& text, PeerAddress* out, std::string* error)
{
  const size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
  {
    *error = "expected <ip>:<port>";
    return false;
  }
  const std::string host = text.substr(0, colon);
  const std::string port_text = text.substr(colon + 1);
  if (host.find(':') != std::string::npos)
  {
    *error = "IPv6 addresses are not supported";
    return false;
  }
  for (char c : host)
  {
    if (std::isspace(static_cast<unsigned char>(c)) != 0)
    {
      *error = "the host contains a space";
      return false;
    }
  }
  if (port_text == "*")  // accept only (a local tunnel's proxy connects from a port nobody knows in advance)
  {
    out->host = host;
    out->port = 0;
    return true;
  }
  u32 port = 0;
  for (char c : port_text)
  {
    if (c < '0' || c > '9' || port > 65535)
    {
      *error = "the port is not a number (or * for accept only)";
      return false;
    }
    port = port * 10 + static_cast<u32>(c - '0');
  }
  if (port < 1 || port > 65535)
  {
    *error = "the port must be 1..65535 (or * for accept only)";
    return false;
  }
  out->host = host;
  out->port = static_cast<u16>(port);
  return true;
}

namespace
{
// A variable that is set but not a decimal number in [lo, hi] is reported (warnings) and its default kept.
void ReadNumber(const GetEnvFn& get_env, const char* name, long lo, long hi, long* value, std::vector<std::string>* warnings)
{
  const std::string text = get_env(name);
  if (text.empty())
    return;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < lo || parsed > hi)
  {
    warnings->push_back(Fmt("%s=%s is not a number in %ld..%ld; using %ld", name, text.c_str(), lo, hi, *value));
    return;
  }
  *value = parsed;
}

std::string Lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
}  // namespace

NetOptions ParseNetOptions(const GetEnvFn& get_env)
{
  NetOptions o;
  const std::string role = Lower(get_env("REV3_NET_ROLE"));
  if (role.empty())
    return o;
  o.requested = true;
  if (role == "host")
    o.role = Role::Host;
  else if (role == "guest")
    o.role = Role::Guest;
  else
  {
    o.error = "REV3_NET_ROLE=" + role + " is neither host nor guest";
    return o;
  }
  const std::string peer = get_env("REV3_NET_PEER");
  if (peer.empty())
  {
    o.error = "REV3_NET_PEER is not set (expected <ip>:<port>)";
    return o;
  }
  std::string error;
  if (!ParsePeerAddress(peer, &o.peer, &error))
  {
    o.error = "REV3_NET_PEER=" + peer + ": " + error;
    return o;
  }
  long v = o.listen_port;
  ReadNumber(get_env, "REV3_NET_PORT", 1, 65535, &v, &o.warnings);
  o.listen_port = static_cast<u16>(v);
  v = o.delay;
  ReadNumber(get_env, "REV3_NET_DELAY", 0, MAX_DELAY, &v, &o.warnings);
  o.delay = static_cast<int>(v);
  v = o.max_rollback;
  ReadNumber(get_env, "REV3_NET_ROLLBACK_MAX", 1, MAX_ROLLBACK_CEILING, &v, &o.warnings);
  o.max_rollback = static_cast<int>(v);
  v = o.arm_calls;
  ReadNumber(get_env, "REV3_NET_ARM", 0, 100000000, &v, &o.warnings);
  o.arm_calls = static_cast<u32>(v);
  v = o.quiet_calls;
  ReadNumber(get_env, "REV3_NET_QUIET", 0, 100000000, &v, &o.warnings);
  o.quiet_calls = static_cast<u32>(v);
  v = o.connect_timeout_ms / 1000;
  ReadNumber(get_env, "REV3_NET_TIMEOUT_S", 1, 3600, &v, &o.warnings);
  o.connect_timeout_ms = static_cast<u32>(v) * 1000;
  v = o.wait_timeout_ms;
  ReadNumber(get_env, "REV3_NET_WAIT_MS", 100, 3600000, &v, &o.warnings);
  o.wait_timeout_ms = static_cast<u32>(v);

  const std::string rollback = get_env("REV3_NET_ROLLBACK");
  if (rollback == "1")
    o.rollback = true;
  else if (!rollback.empty() && rollback != "0")
    o.warnings.push_back("REV3_NET_ROLLBACK=" + rollback + " is neither 0 nor 1; using 0 (lockstep only)");
  v = o.busy_calls;
  ReadNumber(get_env, "REV3_NET_BUSY", 0, 1000, &v, &o.warnings);
  o.busy_calls = static_cast<u32>(v);
  const std::string range_crc = get_env("REV3_NET_RANGE_CRC");
  if (range_crc == "1")
    o.range_crc = true;
  else if (!range_crc.empty() && range_crc != "0")
    o.warnings.push_back("REV3_NET_RANGE_CRC=" + range_crc + " is neither 0 nor 1; using 0");
  // M5: pacing and the adaptive MAXR
  const std::string pace = get_env("REV3_NET_PACE");
  if (pace == "0")
    o.pace.enabled = false;
  else if (pace == "1")
    o.pace.enabled = true;
  else if (!pace.empty())
    o.warnings.push_back("REV3_NET_PACE=" + pace + " is neither 0 nor 1; using 1 (pacing on)");
  v = o.pace.t1_us;
  ReadNumber(get_env, "REV3_NET_PACE_T1_US", 0, 100000, &v, &o.warnings);
  o.pace.t1_us = static_cast<s32>(v);
  v = o.pace.hard_max_us;
  ReadNumber(get_env, "REV3_NET_PACE_HARD_MAX_US", 0, 300000, &v, &o.warnings);
  o.pace.hard_max_us = static_cast<s32>(v);
  v = o.pace.slice_us;
  ReadNumber(get_env, "REV3_NET_PACE_SLICE_US", 100, 16000, &v, &o.warnings);
  o.pace.slice_us = static_cast<s32>(v);
  v = o.pace.gentle_max_permille;
  ReadNumber(get_env, "REV3_NET_PACE_GENTLE_PERMILLE", 0, 100, &v, &o.warnings);
  o.pace.gentle_max_permille = static_cast<s32>(v);
  const std::string adapt = get_env("REV3_NET_MAXR_ADAPT");
  if (adapt == "1")
    o.maxr_adapt = true;
  else if (!adapt.empty() && adapt != "0")
    o.warnings.push_back("REV3_NET_MAXR_ADAPT=" + adapt + " is neither 0 nor 1; using 0");
  v = o.maxr_budget_us;
  ReadNumber(get_env, "REV3_NET_MAXR_BUDGET_US", 1000, 100000, &v, &o.warnings);
  o.maxr_budget_us = static_cast<u32>(v);
  v = o.io_extra;
  ReadNumber(get_env, "REV3_NET_IO_EXTRA", 0, MAX_IO_EXTRA, &v, &o.warnings);
  o.io_extra = static_cast<int>(v);
  const std::string mirror = get_env("REV3_PAD_MIRROR");
  if (mirror == "1")
    o.pad_mirror = true;
  else if (!mirror.empty() && mirror != "0")
    o.warnings.push_back("REV3_PAD_MIRROR=" + mirror + " is neither 0 nor 1; using 0");
  o.log_path = get_env("REV3_NET_LOG");
  o.stable_path = get_env("REV3_NET_STABLE");
  o.netsim = get_env("REV3_NETSIM");
  o.ok = true;
  return o;
}

// ---- HELLO --------------------------------------------------------------------------------------------------------------------

Hello BuildHello(const HelloInputs& in, Role role, int delay)
{
  Hello h;
  h.role = role;
  h.game_id = in.game_id;
  h.protocol = in.hook_protocol;
  h.fields_version = FIELDS_VERSION;
  h.fields_digest = FieldsDigest();
  h.delay = static_cast<u32>(std::max(0, delay));
  const auto add = [&h](const char* key, std::string value) { h.items.emplace_back(key, std::move(value)); };
  const auto onoff = [](bool v) { return std::string(v ? "on" : "off"); };
  add("build", in.build);
  add("cpu_core", in.cpu_core);
  add("cpu_thread", in.cpu_thread ? "dual" : "single");
  add("sync_gpu", onoff(in.sync_gpu));
  add("dsp", in.dsp_hle ? "HLE" : (in.dsp_thread ? "LLE-thread" : "LLE"));
  add("rtc", in.rtc_enabled ? Fmt("on:%u", in.rtc_value) : std::string("off"));
  add("overclock", in.overclock_enabled ? Fmt("%.3f", static_cast<double>(in.overclock)) : std::string("off"));
  add("mmu", onoff(in.mmu));
  add("fast_disc_speed", onoff(in.fast_disc_speed));
  add("cheats", onoff(in.cheats));
  add("bluetooth_passthrough", onoff(in.bluetooth_passthrough));
  add("wiimotes", Fmt("%d,%d,%d,%d,%d", in.wiimote_sources[0], in.wiimote_sources[1], in.wiimote_sources[2], in.wiimote_sources[3],
                      in.wiimote_sources[4]));
  add("hook_code_crc", in.hook_crc_valid ? Fmt("0x%08X", in.hook_crc) : std::string("(not read)"));
  add("rollback", in.rollback ? "1" : "0");
  add("maxr", Fmt("%d", in.max_rollback));
  add("gate", Fmt("arm=%u,quiet=%u", in.arm_calls, in.quiet_calls));
  add("stable_ranges", in.stable_crc != 0 ? Fmt("0x%08X", in.stable_crc) : std::string("none"));
  return h;
}

// ---- the arm gate -------------------------------------------------------------------------------------------------------------

void GateTracker::Close()
{
  m_open = false;
  m_prepared = false;
  m_quiet_streak = 0;  // the next opening needs a fresh quiet stretch
  m_busy_streak = 0;
  m_release_streak = 0;
  m_holding = false;
}

std::string GateTracker::Describe(const GateInputs& in, const char* write_track) const
{
  return Fmt("fighters_found=%d (streak %u/%u) hardware_quiet=%d (streak %u/%u) health_zero=%d learning_done=%d layout_pending=%d open=%d prepared=%d%s "
             "write_track=%s",
             in.fighters_found ? 1 : 0, m_found_streak, m_cfg.arm_calls, in.hardware_busy ? 0 : 1, m_quiet_streak, m_cfg.quiet_calls,
             in.health_zero ? 1 : 0, in.learning_done ? 1 : 0, in.set_pending ? 1 : 0, m_open ? 1 : 0, m_prepared ? 1 : 0,
             m_prepare_failed ? " prepare_failed" : "", write_track != nullptr ? write_track : "?");
}

GateDecision GateTracker::UpdateArmed(const GateInputs& in)
{
  GateDecision d;
  d.open = true;
  d.why = "armed";
  d.hardware_quiet = !in.hardware_busy;
  d.layout_pending = in.set_pending;
  if (in.hardware_busy)
  {
    ++m_busy_streak;
    m_release_streak = 0;
  }
  else
  {
    m_busy_streak = 0;
    ++m_release_streak;
  }
  if (m_cfg.hold_calls != 0 && m_busy_streak >= m_cfg.hold_calls)
    m_holding = true;
  else if (m_holding && m_release_streak >= HOLD_RELEASE_CALLS)
    m_holding = false;  // released only after a few quiet calls, so a flickering read does not toggle the hold every frame
  d.hold = m_holding;
  return d;
}

GateDecision GateTracker::Update(const GateInputs& in, const std::function<bool()>& prepare)
{
  GateDecision d;
  d.hardware_quiet = !in.hardware_busy;
  if (!m_cfg.rollback)
  {
    d.why = "rollback off";
    return d;
  }
  m_found_streak = in.fighters_found ? m_found_streak + 1 : 0;
  m_quiet_streak = in.hardware_busy ? 0 : m_quiet_streak + 1;
  if (!in.fighters_found)
  {
    const bool was_open = m_open;
    m_open = false;
    m_prepared = false;
    m_prepare_failed = false;
    d.why = was_open ? "fighters lost" : "no fighters";
    return d;
  }

  if (m_open)
  {
    // Only reached if the caller keeps feeding Update() after the gate opened: the session decides the close (final frames only, THE GATE
    // RULE in Rev3Session.h) and the caller then calls Close(); UpdateArmed() carries the local conditions meanwhile.
    d.open = true;
    d.why = "open";
    return d;
  }

  if (m_found_streak < m_cfg.arm_calls)
    d.why = "fighters not found long enough";
  else if (m_quiet_streak < m_cfg.quiet_calls)
    d.why = "hardware busy";
  else if (in.health_zero)
    d.why = "health 0";
  else if (!in.learning_done)
    d.why = "snapshot set still learning";
  else if (!in.may_prepare)
    d.why = "session not in lockstep";
  else if (m_prepare_failed)
    d.why = "snapshot set unusable";
  else
  {
    if (!m_prepared)
    {
      m_prepared = prepare ? prepare() : true;
      m_prepare_failed = !m_prepared;
    }
    if (m_prepared)
    {
      m_open = true;
      d.open = true;
      d.why = "fighters found, hardware quiet, nobody at health 0";
    }
    else
      d.why = "snapshot set unusable";
  }
  return d;
}
}  // namespace Rev3::Net
