// Rev3 online rollback session (milestone M2): implementation of Rev3Session.h. Pure logic: no Dolphin core, no sockets, no clock of its
// own (Transport::Now and the injected WaitFn), so Source/UnitTests/Core/Rev3SessionTest.cpp runs two sessions against each other in one
// process. See Rev3Session.h for what the modes mean and docs/rollback_netplay_plan.md (rev3-mod) section 3.3 for the per-call rules.
//
// ONE CALL, in the order this file implements it (N = the hook frame of the command):
//   1. local pad: not on a resim call: local[N + D] = pad (or the previous one when PADRead failed), SendLocal(N + D)
//   2. LOCKSTEP: wait for R >= N, reply {local[N], remote[N]}.
//   3. ARMED: (a) gate closed -> DRAIN. (b) stall: wait while N - R > MAXR. (c) misprediction scan over the unverified frames
//      V+1..min(R, N-1); the first frame whose played remote pad differs from the real one is the rollback target g: Restore(g), reply
//      RESTORED naming g with the pads for g. (d) otherwise Capture(N), predict the remote pad (last confirmed) if it is not here, reply.
//   4. resim calls (after RESTORED): the frame must be next_expected; replies the best-known pads, re-captures the slot.
//   Differences from the plan's text: the stall wait runs BEFORE the scan (plan: after) so a rollback is never deeper than MAXR even when
//   packets arrive while waiting; the stall disconnect is "a stall that lasts 420 frames of time" (7 s), not 420 consecutive stalled calls
//   (with a permanently slow link every call stalls briefly and that must not end the session).
//
// FINAL FRAMES. Frame f is final when V >= f - 1: every remote pad below f is known and was the pad the game played, so the start state
// of f cannot change any more. Only final frames are reported to the observer and, for f % 30 == 0, exchanged as checksums. The gate's
// CLOSE decision is taken on final frames too (THE GATE RULE in Rev3Session.h): the "lost" flag recorded with the capture of the first
// final frame F that has one schedules the DRAIN for frame F + MAXR + 1 on both machines.

#include "Core/Slippi/Rev3Session.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Common/Hash.h"

namespace Rev3::Net
{
namespace
{
std::string Fmt(const char* format, ...)
{
  char buf[2048];  // the SUMMARY line is the longest
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (n < 0)
    return {};
  return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
}

// What the live state of a call says about the fight: 0 nothing, 1 a fighter at health 0, 2 no fighters (nothing to snapshot).
u8 LostKind(const Gate& gate)
{
  return !gate.fighters_found ? 2 : (gate.health_zero ? 1 : 0);
}

const Pad8 NEUTRAL_PAD{};  // all zero: buttons up, sticks and triggers at rest (PADStatus raw, sticks are signed and centred on 0)

// ---- little big-endian byte writer / reader for the messages -------------------------------------------------------------------
class Writer
{
public:
  void U8(u8 v) { m_out.push_back(v); }
  void U16(u16 v)
  {
    U8(static_cast<u8>(v >> 8));
    U8(static_cast<u8>(v));
  }
  void U32(u32 v)
  {
    U16(static_cast<u16>(v >> 16));
    U16(static_cast<u16>(v));
  }
  void S32(s32 v) { U32(static_cast<u32>(v)); }
  void Str(const std::string& s)
  {
    const size_t n = std::min<size_t>(s.size(), 0xFFFF);
    U16(static_cast<u16>(n));
    m_out.insert(m_out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
  }
  void Fields(const FieldSet& f)
  {
    U8(f.present);
    for (const auto& player : f.value)
      for (u32 v : player)
        U32(v);
  }
  std::vector<u8> Take() { return std::move(m_out); }

private:
  std::vector<u8> m_out;
};

class Reader
{
public:
  explicit Reader(const std::vector<u8>& bytes) : m_bytes(bytes) {}
  bool ok() const { return m_ok; }
  bool done() const { return m_pos == m_bytes.size(); }
  u8 U8()
  {
    if (m_pos + 1 > m_bytes.size())
    {
      m_ok = false;
      return 0;
    }
    return m_bytes[m_pos++];
  }
  u16 U16()
  {
    const u16 hi = U8();
    const u16 lo = U8();
    return static_cast<u16>((hi << 8) | lo);
  }
  u32 U32()
  {
    const u32 hi = U16();
    const u32 lo = U16();
    return (hi << 16) | lo;
  }
  s32 S32() { return static_cast<s32>(U32()); }
  std::string Str()
  {
    const size_t n = U16();
    if (m_pos + n > m_bytes.size())
    {
      m_ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(m_bytes.data() + m_pos), n);
    m_pos += n;
    return s;
  }
  void Fields(FieldSet* f)
  {
    f->present = U8();
    for (auto& player : f->value)
      for (u32& v : player)
        v = U32();
  }

private:
  const std::vector<u8>& m_bytes;
  size_t m_pos = 0;
  bool m_ok = true;
};

constexpr u32 HELLO_MAGIC = 0x52334831;        // "R3H1"
constexpr u32 START_CHECK_MAGIC = 0x52335331;  // "R3S1"
constexpr u32 DUMP_MAGIC = 0x52334431;         // "R3D1"
constexpr size_t MAX_DUMP_FRAMES = 64;
constexpr size_t MAX_HELLO_ITEMS = 256;
constexpr size_t MAX_FIELD_DIFFS_LOGGED = 20;
constexpr u32 RANGE_MAGIC = 0x52335231;        // "R3R1"
constexpr size_t MAX_RANGE_ENTRIES = 20000;
constexpr size_t MAX_RANGE_DIFFS_LOGGED = 40;
constexpr size_t RANGE_KEEP = 8;               // frames of range CRCs kept per side

const char* RoleName(Role role)
{
  return role == Role::Host ? "host" : "guest";
}

// "p1 action own=0x.. peer=0x.." for the first differing fields of two sets (limit lines); returns how many differ.
size_t DiffFields(const FieldSet& own, const FieldSet& peer, size_t limit, std::vector<std::string>* lines)
{
  size_t count = 0;
  if (own.present != peer.present)
  {
    if (lines != nullptr)
      lines->push_back(Fmt("fighters found: own=0x%X peer=0x%X", own.present, peer.present));
    ++count;
  }
  for (size_t p = 0; p < 2; ++p)
  {
    if (((own.present & peer.present) & (1u << p)) == 0)
      continue;
    for (size_t i = 0; i < FIELD_COUNT; ++i)
    {
      if (own.value[p][i] == peer.value[p][i])
        continue;
      if (lines != nullptr && count < limit)
        lines->push_back(Fmt("p%zu %s own=0x%08X peer=0x%08X", p + 1, FIELDS[i].name, own.value[p][i], peer.value[p][i]));
      ++count;
    }
  }
  return count;
}

// All FIELD_COUNT fields of one player as "name=0xVALUE name=0xVALUE ..." (the desync log writes both sides' arrays for the frame).
std::string FieldsText(const FieldSet& set, size_t player)
{
  std::string out;
  out.reserve(FIELD_COUNT * 24);
  char piece[64];
  for (size_t i = 0; i < FIELD_COUNT; ++i)
  {
    snprintf(piece, sizeof(piece), "%s%s=0x%08X", i == 0 ? "" : " ", FIELDS[i].name, set.value[player][i]);
    out += piece;
  }
  return out;
}
}  // namespace

// ---- helpers ---------------------------------------------------------------------------------------------------------------

Pad8 SanitizePad(const PadBytes& pad)
{
  Pad8 out;
  std::copy_n(pad.begin(), out.size(), out.begin());
  return out;
}

PadBytes ToPadBytes(const Pad8& pad)
{
  PadBytes out{};  // analogA/B (+8/+9), +11 and err (+10) = 0: Rev3 never reads them and the hook wants err 0 for a connected port
  std::copy(pad.begin(), pad.end(), out.begin());
  return out;
}

u32 FieldsDigest()
{
  std::string text;
  for (const FieldDef& f : FIELDS)
    text += Fmt("%s:%u:%u;", f.name, static_cast<unsigned>(f.offset), 4u);
  text += Fmt("v%u", FIELDS_VERSION);
  return Common::ComputeCRC32(reinterpret_cast<const u8*>(text.data()), text.size());
}

FieldSet FieldsFromRaw(const std::array<std::array<u8, FIELD_BYTES>, 2>& raw, u8 present)
{
  FieldSet out;
  out.present = static_cast<u8>(present & 3u);
  for (size_t p = 0; p < 2; ++p)
  {
    if ((out.present & (1u << p)) == 0)
      continue;
    for (size_t i = 0; i < FIELD_COUNT; ++i)
    {
      const u8* b = raw[p].data() + 4 * i;
      out.value[p][i] = (static_cast<u32>(b[0]) << 24) | (static_cast<u32>(b[1]) << 16) | (static_cast<u32>(b[2]) << 8) | b[3];
    }
  }
  return out;
}

u32 ChecksumOfFields(s32 frame, const FieldSet& fields)
{
  std::vector<u8> stream;
  stream.reserve(4 + 2 * (4 + FIELD_BYTES));
  const auto append = [&stream](u32 v) {
    stream.push_back(static_cast<u8>(v >> 24));
    stream.push_back(static_cast<u8>(v >> 16));
    stream.push_back(static_cast<u8>(v >> 8));
    stream.push_back(static_cast<u8>(v));
  };
  append(static_cast<u32>(frame));
  for (size_t p = 0; p < 2; ++p)
  {
    if ((fields.present & (1u << p)) == 0)
      continue;
    append(static_cast<u32>(p + 1));
    for (u32 v : fields.value[p])
      append(v);
  }
  return Common::ComputeCRC32(stream.data(), stream.size());
}

const char* ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::OFFLINE:
    return "OFFLINE";
  case Mode::CONNECTING:
    return "CONNECTING";
  case Mode::LOCKSTEP:
    return "LOCKSTEP";
  case Mode::ARMED:
    return "ARMED";
  case Mode::DRAIN:
    return "DRAIN";
  }
  return "?";
}

// ---- messages --------------------------------------------------------------------------------------------------------------

std::vector<u8> SerializeHello(const Hello& hello)
{
  Writer w;
  w.U32(HELLO_MAGIC);
  w.U8(static_cast<u8>(hello.role));
  w.Str(hello.game_id);
  w.U32(hello.protocol);
  w.U32(hello.fields_version);
  w.U32(hello.fields_digest);
  w.U32(hello.delay);
  w.U16(static_cast<u16>(std::min(hello.items.size(), MAX_HELLO_ITEMS)));
  for (size_t i = 0; i < hello.items.size() && i < MAX_HELLO_ITEMS; ++i)
  {
    w.Str(hello.items[i].first);
    w.Str(hello.items[i].second);
  }
  return w.Take();
}

bool ParseHello(const std::vector<u8>& bytes, Hello* out)
{
  Reader r(bytes);
  if (r.U32() != HELLO_MAGIC)
    return false;
  Hello h;
  const u8 role = r.U8();
  if (role > 1)
    return false;
  h.role = static_cast<Role>(role);
  h.game_id = r.Str();
  h.protocol = r.U32();
  h.fields_version = r.U32();
  h.fields_digest = r.U32();
  h.delay = r.U32();
  const size_t count = r.U16();
  if (count > MAX_HELLO_ITEMS)
    return false;
  for (size_t i = 0; i < count && r.ok(); ++i)
  {
    std::string key = r.Str();
    std::string value = r.Str();
    h.items.emplace_back(std::move(key), std::move(value));
  }
  if (!r.ok() || !r.done())
    return false;
  *out = std::move(h);
  return true;
}

std::vector<std::string> HelloMismatches(const Hello& mine, const Hello& theirs)
{
  std::vector<std::string> out;
  if (mine.role == theirs.role)
    out.push_back(Fmt("role: both sides are %s (one must be host, one guest)", RoleName(mine.role)));
  if (mine.game_id != theirs.game_id)
    out.push_back("game_id: own=" + mine.game_id + " peer=" + theirs.game_id);
  if (mine.protocol != theirs.protocol)
    out.push_back(Fmt("protocol: own=%u peer=%u", mine.protocol, theirs.protocol));
  if (mine.fields_version != theirs.fields_version)
    out.push_back(Fmt("fields_version: own=%u peer=%u", mine.fields_version, theirs.fields_version));
  if (mine.fields_digest != theirs.fields_digest)
    out.push_back(Fmt("fields_digest: own=0x%08X peer=0x%08X", mine.fields_digest, theirs.fields_digest));
  // The items: every key of either side. A key only one side has is a difference too.
  std::map<std::string, std::pair<const std::string*, const std::string*>> keys;
  for (const auto& item : mine.items)
    keys[item.first].first = &item.second;
  for (const auto& item : theirs.items)
    keys[item.first].second = &item.second;
  for (const auto& entry : keys)
  {
    const std::string* a = entry.second.first;
    const std::string* b = entry.second.second;
    if (a != nullptr && b != nullptr)
    {
      if (*a != *b)
        out.push_back("item " + entry.first + ": own=" + *a + " peer=" + *b);
    }
    else if (a != nullptr)
      out.push_back("item " + entry.first + ": own=" + *a + " peer=(absent)");
    else
      out.push_back("item " + entry.first + ": own=(absent) peer=" + *b);
  }
  return out;
}

std::vector<u8> SerializeStartCheck(const StartCheck& check)
{
  Writer w;
  w.U32(START_CHECK_MAGIC);
  w.S32(check.frame);
  w.U32(check.checksum);
  w.U32(check.stable_crc);
  w.Fields(check.fields);
  return w.Take();
}

bool ParseStartCheck(const std::vector<u8>& bytes, StartCheck* out)
{
  Reader r(bytes);
  if (r.U32() != START_CHECK_MAGIC)
    return false;
  StartCheck c;
  c.frame = r.S32();
  c.checksum = r.U32();
  c.stable_crc = r.U32();
  r.Fields(&c.fields);
  if (!r.ok() || !r.done())
    return false;
  *out = c;
  return true;
}

std::vector<std::string> StartCheckMismatches(const StartCheck& mine, const StartCheck& theirs)
{
  std::vector<std::string> out;
  if (mine.frame != theirs.frame)
    out.push_back(Fmt("frame: own=%d peer=%d (the arm gate opened on different frames)", mine.frame, theirs.frame));
  if (mine.checksum != theirs.checksum)
    out.push_back(Fmt("checksum: own=0x%08X peer=0x%08X", mine.checksum, theirs.checksum));
  if (mine.stable_crc != theirs.stable_crc)
    out.push_back(Fmt("stable_ranges_crc: own=0x%08X peer=0x%08X", mine.stable_crc, theirs.stable_crc));
  if (mine.checksum != theirs.checksum || !(mine.fields == theirs.fields))
  {
    std::vector<std::string> lines;
    const size_t n = DiffFields(mine.fields, theirs.fields, MAX_FIELD_DIFFS_LOGGED, &lines);
    for (std::string& line : lines)
      out.push_back("field " + line);
    if (n > lines.size())
      out.push_back(Fmt("... and %zu more differing fields", n - lines.size()));
  }
  return out;
}

std::vector<u8> SerializeDesyncDump(const DesyncDump& dump)
{
  Writer w;
  w.U32(DUMP_MAGIC);
  w.S32(dump.frame);
  w.U16(static_cast<u16>(std::min(dump.trace.size(), MAX_DUMP_FRAMES)));
  for (size_t i = 0; i < dump.trace.size() && i < MAX_DUMP_FRAMES; ++i)
  {
    w.S32(dump.trace[i].first);
    w.Fields(dump.trace[i].second);
  }
  return w.Take();
}

bool ParseDesyncDump(const std::vector<u8>& bytes, DesyncDump* out)
{
  Reader r(bytes);
  if (r.U32() != DUMP_MAGIC)
    return false;
  DesyncDump d;
  d.frame = r.S32();
  const size_t count = r.U16();
  if (count > MAX_DUMP_FRAMES)
    return false;
  for (size_t i = 0; i < count && r.ok(); ++i)
  {
    const s32 frame = r.S32();
    FieldSet fields;
    r.Fields(&fields);
    d.trace.emplace_back(frame, fields);
  }
  if (!r.ok() || !r.done())
    return false;
  *out = std::move(d);
  return true;
}

std::vector<u8> SerializeRangeCrc(const RangeCrcMsg& msg)
{
  Writer w;
  w.U32(RANGE_MAGIC);
  w.S32(msg.frame);
  const size_t count = std::min(msg.ranges.size(), MAX_RANGE_ENTRIES);
  w.U32(static_cast<u32>(count));
  for (size_t i = 0; i < count; ++i)
  {
    w.U32(msg.ranges[i].start);
    w.U32(msg.ranges[i].end);
    w.U32(msg.ranges[i].crc);
  }
  return w.Take();
}

bool ParseRangeCrc(const std::vector<u8>& bytes, RangeCrcMsg* out)
{
  Reader r(bytes);
  if (r.U32() != RANGE_MAGIC)
    return false;
  RangeCrcMsg m;
  m.frame = r.S32();
  const u32 count = r.U32();
  if (count > MAX_RANGE_ENTRIES)
    return false;
  for (u32 i = 0; i < count && r.ok(); ++i)
  {
    RangeCrc e;
    e.start = r.U32();
    e.end = r.U32();
    e.crc = r.U32();
    m.ranges.push_back(e);
  }
  if (!r.ok() || !r.done())
    return false;
  *out = std::move(m);
  return true;
}

// ---- pacing: the time offset estimate and the policy (M5) -----------------------------------------------------------------------

void TimeSync::OnLocalSend(s32 frame, u64 now_us)
{
  if (frame <= m_local_frame)
    return;  // only a newer frame counts (a re-send is not a new frame)
  m_local_frame = frame;
  Sent& slot = m_sent[static_cast<size_t>(frame) % RING];
  slot.frame = frame;
  slot.us = now_us;
}

void TimeSync::OnRemote(s32 frame, u64 now_us, u64 rtt_us)
{
  if (frame <= m_newest_remote)
    return;  // a re-sent or reordered packet: not the first arrival of this frame
  m_newest_remote = frame;
  if (frame < m_min_frame)
    return;  // sent by the peer before our last correction took effect: our send time of the same frame is the old timing
  if (m_local_frame < 0 || rtt_us == 0)
    return;  // nothing to compare with yet (Slippi's has_game_started), or no RTT estimate: the arrival time would be wrong by RTT/2
  // Slippi (SlippiNetplay.cpp:295-316): opponent_send_time = now - ping/2; offset = opponent_send_time - our_send_time + frame_time * (our_frame - their_frame).
  const s64 opponent_send_us = static_cast<s64>(now_us) - static_cast<s64>(rtt_us / 2);
  const Sent& same = m_sent[static_cast<size_t>(std::max(frame, 0)) % RING];
  s64 offset;
  if (frame >= 0 && same.frame == frame)
    offset = opponent_send_us - static_cast<s64>(same.us);
  else
  {
    const Sent& newest = m_sent[static_cast<size_t>(m_local_frame) % RING];
    offset = opponent_send_us - static_cast<s64>(newest.us) + static_cast<s64>(PACE_FRAME_US) * (static_cast<s64>(m_local_frame) - static_cast<s64>(frame));
  }
  constexpr s64 LIMIT = 60000000;  // one minute: a sample beyond this is a broken clock, not a skew; keeps the sums far from overflow
  offset = std::max(-LIMIT, std::min(LIMIT, offset));
  m_buf[m_next] = static_cast<s32>(offset);
  m_next = (m_next + 1) % m_buf.size();
  m_count = std::min(m_count + 1, m_buf.size());
}

bool TimeSync::OffsetUs(s32* out, u32* samples) const
{
  if (m_count == 0)
    return false;
  // CalcTimeOffsetUs (SlippiNetplay.cpp:1698-1717): sort, drop the lowest and highest third, average the rest.
  std::array<s32, PACE_INTERVAL_FRAMES> sorted = m_buf;
  std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(m_count));
  const size_t third = m_count / 3;
  const size_t end = m_count - third;
  s64 sum = 0;
  for (size_t i = third; i < end; ++i)
    sum += sorted[i];
  *out = static_cast<s32>(sum / static_cast<s64>(end - third));
  if (samples != nullptr)
    *samples = static_cast<u32>(m_count);
  return true;
}

void TimeSync::Reset()
{
  m_count = 0;
  m_next = 0;
  m_min_frame = m_local_frame + 1;
}

PaceDecision DecidePace(const PaceConfig& c, s32 frames_since_gate, s32 offset_us)
{
  PaceDecision d;
  if (frames_since_gate <= c.hard_frames)
  {
    // Hard sync (Slippi EXI_DeviceSlippi.cpp:1437-1470): ahead by more than t1 -> lose the excess at once, up to hard_max_us.
    if (offset_us > c.t1_us)
    {
      const s32 lose = std::min(offset_us - c.hard_target_us, c.hard_max_us);
      if (lose > 0)
      {
        d.action = "hard";
        d.delay_us = lose;
      }
    }
    else if (offset_us < -c.t1_us)
    {
      d.action = "behind";
    }
    return d;
  }
  // Gentle (Slippi shouldAdvanceOnlineFrame, EXI_DeviceSlippi.cpp:1503-1530): a slowdown of up to gentle_max_permille of the interval,
  // proportional to the lead up to gentle_window_frames frames, nothing inside the dead band.
  if (offset_us > c.gentle_deadband_us)
  {
    const s64 window_us = static_cast<s64>(c.gentle_window_frames) * PACE_FRAME_US;
    const s64 lead_us = std::min<s64>(offset_us, window_us);
    const s64 interval_us = static_cast<s64>(c.interval_frames) * PACE_FRAME_US;
    const s64 lose = window_us > 0 ? interval_us * c.gentle_max_permille * lead_us / (1000 * window_us) : 0;
    if (lose > 0)
    {
      d.action = "gentle";
      d.delay_us = static_cast<s32>(lose);
    }
  }
  else if (offset_us < -c.gentle_deadband_us)
  {
    d.action = "behind";
  }
  return d;
}

void ResimHiding::Update(bool resimulating)
{
  if (m_hider == nullptr || resimulating == m_hidden)
    return;
  m_hidden = resimulating;
  m_hider->SetHidden(resimulating);
  if (resimulating)
  {
    ++m_hides;
    if (!m_noted)
    {
      m_noted = true;
      m_log.Line("NOTE resim hiding v1: throttle off + mute");
    }
  }
}

// ---- construction and small helpers ----------------------------------------------------------------------------------------------

RollbackSession::RollbackSession(const Config& config, Transport& transport, SnapshotStore& store, Sink& sink, WaitFn wait)
    : m_cfg(config), m_net(transport), m_store(store), m_sink(sink), m_wait(std::move(wait))
{
  m_cfg.max_rollback = std::max(1, std::min(m_cfg.max_rollback, MAX_ROLLBACK_CEILING));
  m_cfg.delay = std::max(0, std::min(m_cfg.delay, MAX_DELAY));
  m_delay = m_cfg.delay;
  m_cfg.hello.role = m_cfg.role;
  m_cfg.hello.delay = static_cast<u32>(m_delay);
  m_maxr_eff = m_cfg.max_rollback;
  m_stats.maxr_eff = m_maxr_eff;
  m_stats.maxr_eff_min = m_maxr_eff;
  Line(Fmt("SESSION start role=%s delay=%d maxr=%d pace=%s maxr_adapt=%s", RoleName(m_cfg.role), m_delay, m_cfg.max_rollback,
           m_cfg.pace.enabled ? "on" : "off", m_cfg.maxr_adapt ? "on" : "off"));
}

void RollbackSession::Line(const std::string& text)
{
  m_sink.Line(text);
}

s32 RollbackSession::Confirmed() const
{
  // The frames below D carry the neutral pad on both sides by construction, so they count as known even before the packets arrive.
  return std::max(m_net.RemoteConfirmed(), m_delay - 1);
}

Pad8 RollbackSession::RemoteAt(s32 frame) const
{
  if (frame < 0 || frame < m_delay)
    return NEUTRAL_PAD;
  Pad8 pad{};
  if (!m_net.RemotePad(frame, &pad))
    return NEUTRAL_PAD;  // only for a frame above R: callers check Confirmed() first
  return pad;
}

Pad8 RollbackSession::Predict() const
{
  const s32 r = Confirmed();
  return r >= 0 ? RemoteAt(r) : NEUTRAL_PAD;
}

Pad8 RollbackSession::LocalAt(s32 frame) const
{
  if (frame < 0)
    return NEUTRAL_PAD;
  const PadSlot& slot = m_local[static_cast<size_t>(frame) % RING];
  return slot.frame == frame ? slot.pad : NEUTRAL_PAD;
}

void RollbackSession::StoreLocal(const Command& command, s32 frame)
{
  // PADRead's return value is a bit mask of the ports with data (Rev3 normally returns 0x80000000 = port 0; D's pad record, Oct 16),
  // not an error code, so it cannot tell a failed read. The port's PADStatus err byte (+10) does: PAD_ERR_TRANSFER (-3) means the block
  // holds the previous poll, so the previous local pad is the best statement of the player's input; PAD_ERR_NO_CONTROLLER (-1) and
  // PAD_ERR_NOT_READY (-2) mean no input (neutral). (Until Oct 16 any non-zero return was taken as a failure, so every local pad was
  // neutral and neither loopback window reacted to the controller.)
  const int err = static_cast<std::int8_t>(command.pads[0][10]);
  Pad8 pad;
  if (err == -3 || command.pad_return == -3)
    pad = m_last_local;
  else if (err == -1 || err == -2)
    pad = NEUTRAL_PAD;
  else
    pad = SanitizePad(command.pads[0]);
  m_last_local = pad;
  const s32 target = frame + m_delay;
  PadSlot& slot = m_local[static_cast<size_t>(target) % RING];
  slot.frame = target;
  slot.pad = pad;
  m_input_frame = frame;
  m_net.SendLocal(target, pad, m_send_chk_frame, m_send_chk_value);
}

Reply RollbackSession::MakeReply(const Pad8& local, const Pad8& remote, u32 flags, s32 frame) const
{
  Reply r;
  r.flags = flags;
  r.frame = static_cast<u32>(frame);
  const size_t local_port = m_cfg.role == Role::Host ? 0 : 1;  // host = game port 0, guest = game port 1
  r.pads[local_port] = ToPadBytes(local);
  r.pads[1 - local_port] = ToPadBytes(remote);
  return r;
}

Reply RollbackSession::EchoReply(const Command& command) const
{
  Reply r;
  r.flags = 0;
  r.frame = command.frame;
  r.pads = command.pads;
  return r;
}

bool RollbackSession::Blocked(Block block)
{
  m_block = block;
  return false;
}

bool RollbackSession::EndCall(s32 frame, const Reply& reply, Reply* out)
{
  *out = reply;
  m_cur_frame = -1;
  m_expected_next = frame + 1;
  m_block = Block::None;
  m_stalled_this_call = false;
  m_postpone_counted = false;
  return true;
}

// `info` null: the call had no fighters, so no snapshot and no checksum exist for the frame (only what it was played with and its flag).
void RollbackSession::CaptureUsed(s32 frame, const Pad8& local, const Pad8& remote, bool predicted, const CaptureInfo* info, u8 lost)
{
  Used& u = UsedAt(frame);
  u.frame = frame;
  u.local = local;
  u.remote = remote;
  u.predicted = predicted;
  u.lost = lost;
  u.has_checksum = info != nullptr;
  u.checksum = info != nullptr ? info->checksum : 0;
  u.has_fields = info != nullptr && frame % CHECK_INTERVAL == 0;
  if (u.has_fields)
    u.fields = info->fields;
}

// ---- the blocking wrapper ------------------------------------------------------------------------------------------------------

Reply RollbackSession::OnCommand(const Command& command, const Gate& gate)
{
  Reply reply;
  while (!Step(command, gate, &reply))
  {
    const bool satisfied = m_wait ? m_wait([this] { return BlockSatisfied(); }, BlockTimeoutMs()) : false;
    if (!satisfied)
      BlockTimedOut();
  }
  return reply;
}

u32 RollbackSession::BlockTimeoutMs() const
{
  switch (m_block)
  {
  case Block::Barrier:
    return m_cfg.barrier_timeout_ms;
  case Block::Stall:
    return m_cfg.stall_disconnect_frames * 1000 / 60;  // 420 frames of 1/60 s = 7000 ms
  case Block::StartCheck:
    return m_cfg.start_check_timeout_ms;
  case Block::None:
  case Block::Lockstep:
  case Block::DrainA:
  case Block::DrainB:
    break;
  }
  return m_cfg.wait_timeout_ms;
}

bool RollbackSession::BlockSatisfied()
{
  m_net.Pump();
  PumpMessages();
  if (m_mode == Mode::OFFLINE || m_net.State() == ConnState::Failed)
    return true;  // Step() sees it and answers offline
  const s32 n = m_cur_frame;
  switch (m_block)
  {
  case Block::None:
    return true;
  case Block::Barrier:
    // The connection may come up while the CPU thread waits here, and the wait only calls Step() again once this is true: our HELLO has
    // to go out from here, or the two sides would wait for each other's HELLO until the barrier times out.
    if (!m_hello_sent && m_mode == Mode::CONNECTING && m_net.State() == ConnState::Connected)
      SendHello();
    return m_net.State() == ConnState::Connected && m_peer_hello_ok;
  case Block::Lockstep:
    return Confirmed() >= n;
  case Block::Stall:
    return n - Confirmed() <= m_maxr_eff;
  case Block::DrainA:
    return Confirmed() >= n - 1;
  case Block::DrainB:
    return Confirmed() >= n;
  case Block::StartCheck:
    return !m_peer_start.empty();
  }
  return true;
}

void RollbackSession::Abort(const std::string& why)
{
  LeaveSession(why, true);
}

void RollbackSession::BlockTimedOut()
{
  if (m_mode == Mode::OFFLINE)
    return;  // the wait was ended by Abort() (emulator stopping) or a LeaveSession inside it: nothing timed out
  switch (m_block)
  {
  case Block::Barrier:
    LeaveSession(Fmt("no connection / HELLO within %u ms (frame-0 barrier)", m_cfg.barrier_timeout_ms), true);
    break;
  case Block::Stall:
    EndStall(true);
    LeaveSession(Fmt("stall: no remote pad for %u frames (%u ms)", m_cfg.stall_disconnect_frames, BlockTimeoutMs()), true);
    break;
  case Block::StartCheck:
    // The peer is not asking for the same arm (see "Known gap" in Rev3Session.h): stay in lockstep, correct but slower.
    Line(Fmt("ARM frame=%d refused: no START_CHECK from the peer within %u ms; staying in lockstep", m_cur_frame,
             m_cfg.start_check_timeout_ms));
    m_arm_blocked = true;
    ++m_stats.arm_refused;
    break;
  case Block::None:
  case Block::Lockstep:
  case Block::DrainA:
  case Block::DrainB:
    LeaveSession(Fmt("no remote pad for %u ms (frame %d, R=%d)", m_cfg.wait_timeout_ms, m_cur_frame, Confirmed()), true);
    break;
  }
}

// ---- the call --------------------------------------------------------------------------------------------------------------------

bool RollbackSession::Step(const Command& command, const Gate& gate, Reply* reply)
{
  const bool was_offline = m_mode == Mode::OFFLINE;
  if (!StepInner(command, gate, reply))
    return false;
  if (!was_offline)
    ++m_stats.calls;
  // A desync / error that left the session in the middle of a call answers with the local pads, unless the memory was already rewound
  // (a RESTORED reply is never replaced).
  if (m_mode == Mode::OFFLINE && (reply->flags & REPLY_FLAG_RESTORED) == 0)
    *reply = EchoReply(command);
  return true;
}

bool RollbackSession::StepInner(const Command& command, const Gate& gate, Reply* reply)
{
  m_block = Block::None;
  if (m_mode == Mode::OFFLINE)
  {
    PollOffline();
    m_cur_frame = -1;
    *reply = EchoReply(command);
    return true;
  }
  if (m_net.State() == ConnState::Failed)
  {
    LeaveSession("connection lost", false);
    *reply = EchoReply(command);
    return true;
  }
  PumpMessages();
  if (m_mode == Mode::OFFLINE)
  {
    m_cur_frame = -1;
    *reply = EchoReply(command);
    return true;
  }

  const s32 n = static_cast<s32>(command.frame);

  // ---- frame-0 barrier
  if (m_mode == Mode::CONNECTING)
  {
    if (!m_hello_sent && m_net.State() == ConnState::Connected)
      SendHello();
    if (m_mode == Mode::OFFLINE)
    {
      *reply = EchoReply(command);
      return true;
    }
    if (m_net.State() != ConnState::Connected || !m_peer_hello_ok)
      return Blocked(Block::Barrier);
    if (n != 0)
    {
      LeaveSession(Fmt("the first command is frame %d, not 0 (a session cannot start mid-game)", n), true);
      *reply = EchoReply(command);
      return true;
    }
    StartLockstep();
  }

  // ---- a fresh call (not a retry after a wait)?
  if (m_cur_frame != n)
  {
    if (!m_resim && n != m_expected_next)
    {
      Line(Fmt("ERROR frame jump: expected %d, the game sent %d", m_expected_next, n));
      LeaveSession(Fmt("frame counter jumped (expected %d, got %d)", m_expected_next, n), true);
      *reply = EchoReply(command);
      return true;
    }
    m_cur_frame = n;
    m_stalled_this_call = false;
    m_postpone_counted = false;
    if (!m_resim)
      WindowTick(n);
  }

  // Pacing (M5) only delays a call that began as a normal ARMED call: not a resimulation (the throttle is off then), not a DRAIN or the
  // call that finishes one (it may end a resimulation), not LOCKSTEP.
  m_pace_allowed = !m_resim && m_mode == Mode::ARMED;

  if (m_resim)
    return StepResim(command, gate, reply);

  if (m_input_frame != n)
    StoreLocal(command, n);

  switch (m_mode)
  {
  case Mode::LOCKSTEP:
    return StepLockstep(command, gate, reply);
  case Mode::ARMED:
    return StepArmed(command, gate, reply);
  case Mode::DRAIN:
    return StepDrain(command, gate, reply);
  case Mode::CONNECTING:
  case Mode::OFFLINE:
    break;
  }
  *reply = EchoReply(command);
  return true;
}

void RollbackSession::StartLockstep()
{
  m_mode = Mode::LOCKSTEP;
  // Neutral pads for the frames the input delay covers: the transport's contiguous queues start at frame 0 on both sides.
  for (s32 f = 0; f < m_delay; ++f)
  {
    PadSlot& slot = m_local[static_cast<size_t>(f) % RING];
    slot.frame = f;
    slot.pad = NEUTRAL_PAD;
    m_net.SendLocal(f, NEUTRAL_PAD, -1, 0);
  }
  Line(Fmt("SESSION lockstep from frame 0 role=%s delay=%d maxr=%d", RoleName(m_cfg.role), m_delay, m_cfg.max_rollback));
}

bool RollbackSession::StepLockstep(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 n = m_cur_frame;
  if (!gate.open)
  {
    m_arm_blocked = false;  // a refused gate opening is over once the gate has closed
  }
  else if (!m_arm_blocked)
  {
    const int arm = TryArm(gate);
    if (arm == 0)
      return false;
    if (arm == 1)
      return StepArmed(command, gate, reply);
  }
  if (Confirmed() < n)
    return Blocked(Block::Lockstep);
  LockstepCheck(n);
  return EndCall(n, MakeReply(LocalAt(n), RemoteAt(n), 0, n), reply);
}

int RollbackSession::TryArm(const Gate& gate)
{
  const s32 n = m_cur_frame;
  if (m_arm_sent_frame != n)
  {
    StartCheck mine;
    mine.frame = n;
    mine.checksum = m_store.LiveChecksum(n, &mine.fields);
    mine.stable_crc = m_store.StableCrc();
    m_arm_mine = mine;
    m_arm_sent_frame = n;
    m_net.SendReliable(MsgType::StartCheck, SerializeStartCheck(mine));
  }
  const auto it = m_peer_start.find(n);
  if (it == m_peer_start.end())
  {
    if (m_peer_start.empty())
    {
      Blocked(Block::StartCheck);
      return 0;
    }
    // The peer asked for an arm at another frame.
    const s32 other = m_peer_start.begin()->first;
    Line(Fmt("ARM frame=%d refused: START_CHECK frame mismatch (own=%d peer=%d)", n, n, other));
    m_sink.Osd(Fmt("Rev3 netplay: not arming (gate opened on frame %d here, %d at the peer); staying in lockstep", n, other));
    m_peer_start.clear();
    m_arm_blocked = true;
    ++m_stats.arm_refused;
    return 2;
  }
  const StartCheck theirs = it->second;
  m_peer_start.erase(m_peer_start.begin(), m_peer_start.upper_bound(n));
  const std::vector<std::string> mismatches = StartCheckMismatches(m_arm_mine, theirs);
  if (!mismatches.empty())
  {
    Line(Fmt("ARM frame=%d refused: START_CHECK mismatch (%zu items)", n, mismatches.size()));
    for (const std::string& m : mismatches)
      Line("  START_CHECK " + m);
    m_sink.Osd(Fmt("Rev3 netplay: not arming, the machines differ at frame %d (%s); staying in lockstep", n, mismatches.front().c_str()));
    m_arm_blocked = true;
    ++m_stats.arm_refused;
    return 2;
  }

  m_mode = Mode::ARMED;
  m_armed_frame = n;
  m_verified = n - 1;
  m_finalized = n - 1;
  m_close_at = -1;
  m_close_why.clear();
  m_relayout = false;
  m_relayout_frame = -1;
  m_hold = false;
  m_pace_next = n + m_cfg.pace.interval_frames;  // the first check after a full interval of ARMED samples
  m_pace_pending_us = 0;
  m_stall_run = 0;
  m_stall_run_us = 0;
  m_net.ResetTimeOffsets();  // the samples so far are lockstep timing (every frame waited for the peer)
  m_store.Clear();
  for (Used& u : m_used)
    u = Used{};
  ++m_stats.gate_opens;
  Line(Fmt("GATE open frame=%d why=%s", n, gate.why != nullptr ? gate.why : ""));
  Line(Fmt("CHK start frame=%d value=0x%08X stable_crc=0x%08X (both machines agree)", n, m_arm_mine.checksum, m_arm_mine.stable_crc));
  return 1;
}

// ---- ARMED -----------------------------------------------------------------------------------------------------------------------

bool RollbackSession::StepArmed(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 n = m_cur_frame;
  const int maxr = m_maxr_eff;  // the stall rule's limit (<= the configured MAXR; only the stall rule and the depth of a postponed rollback use it)

  // The agreed close: scheduled from a FINAL frame F for frame F + MAXR + 1, the same on both machines (THE GATE RULE). A live state, which
  // may be a misprediction's, never closes anything: gate.fighters_found / gate.health_zero are only recorded, below.
  if (m_close_at >= 0 && n >= m_close_at)
  {
    if (n > m_close_at)
    {
      ++m_stats.gate_late;
      Line(Fmt("GATE close late: scheduled for frame %d, applied at frame %d (the peer may have closed at another frame)", m_close_at, n));
    }
    EnterDrain(n, m_close_why, false);
    return StepDrain(command, gate, reply);
  }

  // A local relayout: the snapshot set would change on this machine. Drain, install it, carry on ARMED (the peer notices a short stall).
  if (gate.layout_pending && gate.fighters_found && n != m_relayout_frame)
  {
    EnterDrain(n, "snapshot set changed (local: the peer is not affected)", true);
    return StepDrain(command, gate, reply);
  }

  // A local HOLD: the hardware has been busy for several calls. No frame is predicted while it lasts, so no rollback can become due while a
  // disc read is in flight (a restore cannot rewind it); every call waits for the real pad of its own frame.
  if (gate.hardware_hold)
  {
    if (!m_hold)
    {
      m_hold = true;
      m_hold_run = 0;
      ++m_stats.hw_holds;
      Line(Fmt("GATE hold frame=%d why=hardware busy: no frame is predicted while it lasts (the gate stays open)", n));
    }
    if (Confirmed() < n)
      return Blocked(Block::Lockstep);
    ++m_hold_run;
    ++m_stats.hold_calls;
  }
  else if (m_hold)
  {
    m_hold = false;
    Line(Fmt("GATE hold end frame=%d held_calls=%u", n, m_hold_run));
  }

  // Stall rule: the frames R+1..N are predicted; never more than MAXR of them. (Before the scan, so that a pad that arrives while we
  // wait is seen by it and no rollback can reach further back than MAXR frames.)
  if (n - Confirmed() > maxr)
  {
    if (!m_stalled_this_call)
    {
      m_stalled_this_call = true;
      m_in_stall = true;
      m_stall_start_us = m_net.Now();
      ++m_stats.stalls;
      ++m_win.stalls;
      ++m_stall_run;
      m_stats.stall_run_max = std::max(m_stats.stall_run_max, m_stall_run);
      m_stall_logged = DetailLine(Detail::Stall, Fmt("STALL start frame=%d R=%d predicted=%d maxr=%d", n, Confirmed(), n - Confirmed(), maxr));
    }
    return Blocked(Block::Stall);
  }
  if (m_in_stall)
  {
    EndStall(false);
    if (StallRunTooLong())
      return EndCall(n, EchoReply(command), reply);
  }

  const s32 g = FindMisprediction(n);
  if (g >= 0)
  {
    // The frames up to g are final now (everything below g is verified): say so BEFORE a rollback may run, or a run of consecutive
    // rollbacks could keep a final frame from being declared and the scheduled close from reaching both machines in time.
    FinalizeChecks(g);
    const bool quiet = gate.hardware_quiet;
    const bool forced = !quiet && n - g >= maxr;  // postponing once more would make the rollback deeper than MAXR
    if (quiet || forced)
      return DoRollback(command, g, forced, reply);
    if (!m_postpone_counted)
    {
      m_postpone_counted = true;
      ++m_stats.postponed;
      ++m_win.postponed;
      DetailLine(Detail::Rollback, Fmt("ROLLBACK postponed frame=%d live=%d depth=%d (hardware busy)", g, n, n - g));
    }
  }

  const Pad8 local = LocalAt(n);
  const u8 lost = LostKind(gate);
  if (!gate.fighters_found)
  {
    // No fighters: nothing can be snapshotted, so a prediction for this frame could never be rolled back to. Play it with the real pad.
    if (Confirmed() < n)
      return Blocked(Block::Lockstep);
    const Pad8 remote = RemoteAt(n);
    CaptureUsed(n, local, remote, false, nullptr, lost);
    FinalizeChecks(n);
    if (!m_stalled_this_call)
      NoteCallWithoutStall();
    Pace(n);
    return EndCall(n, MakeReply(local, remote, 0, n), reply);
  }

  CaptureInfo info;
  if (!m_store.Capture(n, command.sp, &info))
  {
    Line(Fmt("ERROR frame %d: snapshot capture failed", n));
    LeaveSession("snapshot capture failed", true);
    return EndCall(n, EchoReply(command), reply);
  }
  const bool known = n <= Confirmed();
  const Pad8 remote = known ? RemoteAt(n) : Predict();
  if (!known)
    ++m_stats.predicted_frames;
  CaptureUsed(n, local, remote, !known, &info, lost);
  FinalizeChecks(n);
  if (!m_stalled_this_call)
    NoteCallWithoutStall();
  Pace(n);
  return EndCall(n, MakeReply(local, remote, 0, n), reply);
}

s32 RollbackSession::FindMisprediction(s32 frame)
{
  const s32 limit = std::min(Confirmed(), frame - 1);
  for (s32 f = m_verified + 1; f <= limit; ++f)
  {
    const Used& u = UsedAt(f);
    if (u.frame != f)  // cannot happen while the ring is larger than the longest unverified run; do not rollback to a frame we cannot name
    {
      Line(Fmt("ERROR frame %d: no record of what frame %d was played with", frame, f));
      m_verified = f;
      continue;
    }
    if (u.remote != RemoteAt(f))
      return f;
    m_verified = f;
  }
  return -1;
}

bool RollbackSession::DoRollback(const Command& command, s32 target, bool forced, Reply* reply)
{
  const s32 n = m_cur_frame;
  const s32 depth = n - target;
  m_resim_start_us = m_net.Now();  // the cost of a rollback = restore + K re-run frames, measured from here to the end of the resimulation
  m_resim_depth = depth;
  m_resim_blocked = false;
  if (!m_store.Has(target))
  {
    Line(Fmt("ERROR ROLLBACK frame=%d live=%d: no snapshot for the target", target, n));
    LeaveSession(Fmt("no snapshot for frame %d (rollback from %d)", target, n), true);
    *reply = EchoReply(command);
    return true;
  }
  if (m_store.SlotSp(target) != command.sp)
  {
    Line(Fmt("ERROR ROLLBACK frame=%d live=%d: sp 0x%08X differs from the slot's 0x%08X (desync class)", target, n, command.sp,
             m_store.SlotSp(target)));
    LeaveSession(Fmt("hook sp differs at the rollback target %d", target), true);
    *reply = EchoReply(command);
    return true;
  }
  if (!m_store.Restore(target))
  {
    Line(Fmt("ERROR ROLLBACK frame=%d live=%d: restore failed", target, n));
    LeaveSession(Fmt("restore of frame %d failed", target), true);
    *reply = EchoReply(command);
    return true;
  }
  ++m_stats.rollbacks;
  ++m_win.rollbacks;
  m_stats.max_depth = std::max(m_stats.max_depth, static_cast<u32>(depth));
  const size_t bucket = std::min<size_t>(static_cast<size_t>(depth), m_stats.depth_hist.size() - 1);
  ++m_stats.depth_hist[bucket];
  ++m_win.depth[bucket];
  if (forced)
  {
    ++m_stats.forced_hw;
    ++m_win.forced;
  }
  m_resim_logged = DetailLine(
      Detail::Rollback, Fmt("ROLLBACK depth=%d frame=%d live=%d R=%d%s", depth, target, n, Confirmed(), forced ? " forced=hardware_busy" : ""));

  // The pads FOR the target frame: the remote one is known (that is how the misprediction was found).
  const Pad8 local = LocalAt(target);
  const Pad8 remote = RemoteAt(target);
  Used& u = UsedAt(target);
  u.local = local;
  u.remote = remote;
  u.predicted = false;

  m_resim = true;
  m_resim_target = n;
  m_next_expected = target + 1;
  m_cur_frame = -1;
  m_block = Block::None;
  m_stalled_this_call = false;
  m_postpone_counted = false;
  *reply = MakeReply(local, remote, REPLY_FLAG_RESTORED, target);
  return true;
}

bool RollbackSession::StepResim(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 m = m_cur_frame;
  if (m != m_next_expected)
  {
    Line(Fmt("ERROR RESIM expected frame %d, the game sent %d (restore not honoured?); back to lockstep", m_next_expected, m));
    m_resim = false;
    m_mode = Mode::LOCKSTEP;
    m_relayout = false;
    m_close_at = -1;
    m_store.Clear();
    m_expected_next = m;
    m_input_frame = -1;
    m_cur_frame = -1;
    return StepInner(command, gate, reply);
  }
  const bool drain = m_mode == Mode::DRAIN;
  const bool last = m == m_resim_target;
  if (drain && last && Confirmed() < m)
  {
    m_resim_blocked = true;
    return Blocked(Block::DrainB);
  }

  const Pad8 local = LocalAt(m);
  const u8 lost = LostKind(gate);
  Pad8 remote{};
  bool known = m <= Confirmed();
  CaptureInfo info;
  const bool captured = gate.fighters_found;
  if (!captured)
  {
    // The corrected timeline has no fighters at this frame: nothing to snapshot, so it is played with the real pad only.
    if (!known)
    {
      m_resim_blocked = true;
      return Blocked(Block::Lockstep);
    }
  }
  remote = known ? RemoteAt(m) : Predict();
  if (captured && !m_store.Capture(m, command.sp, &info))
  {
    Line(Fmt("ERROR RESIM frame %d: snapshot capture failed", m));
    LeaveSession("snapshot capture failed during the resimulation", true);
    m_cur_frame = -1;
    *reply = EchoReply(command);
    return true;
  }
  ++m_stats.resim_calls;
  if (!known)
    ++m_stats.predicted_frames;
  CaptureUsed(m, local, remote, !known, captured ? &info : nullptr, lost);
  m_next_expected = m + 1;
  if (!last)
    return EndCall(m, MakeReply(local, remote, 0, m), reply);

  // The second run of the call that was live when the rollback began: the resimulation is over.
  m_resim = false;
  if (m_resim_blocked)
    ++m_stats.resim_cost_skipped;
  else
    NoteResimCost(m_resim_depth, m_net.Now() - m_resim_start_us);
  // The RESIM line goes with its ROLLBACK line (written or left out together: the budget counts rollbacks, not lines).
  if (m_resim_logged)
    Line(Fmt("RESIM done frame=%d", m));
  else
  {
    ++m_win.suppressed;
    ++m_stats.log_suppressed;
  }
  if (drain)
    return FinishDrain(command, gate, reply);
  return EndCall(m, MakeReply(local, remote, 0, m), reply);
}

// ---- DRAIN -----------------------------------------------------------------------------------------------------------------------

void RollbackSession::EnterDrain(s32 frame, const std::string& why, bool relayout)
{
  m_mode = Mode::DRAIN;
  m_relayout = relayout;
  m_hold = false;
  m_pace_pending_us = 0;  // pacing is for ARMED calls only
  if (relayout)
  {
    ++m_stats.relayouts;
    Line(Fmt("GATE relayout frame=%d why=%s", frame, why.c_str()));
    return;
  }
  m_arm_blocked = false;
  ++m_stats.gate_closes;
  Line(Fmt("GATE close frame=%d why=%s", frame, why.c_str()));
}

bool RollbackSession::StepDrain(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 n = m_cur_frame;
  if (Confirmed() < n - 1)
    return Blocked(Block::DrainA);
  const s32 g = FindMisprediction(n);
  if (g >= 0)
    return DoRollback(command, g, !gate.hardware_quiet, reply);  // draining cannot wait for the hardware: a rollback is due now
  return FinishDrain(command, gate, reply);
}

bool RollbackSession::FinishDrain(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 n = m_cur_frame;
  if (Confirmed() < n)
    return Blocked(Block::DrainB);
  m_verified = n - 1;  // every remote pad below n is confirmed, and the resimulation (if any) played them as confirmed
  FinalizeChecks(n - 1);
  if (m_mode == Mode::OFFLINE)
    return EndCall(n, EchoReply(command), reply);
  if (m_relayout)
    return FinishRelayout(command, gate, reply);
  Line(Fmt("DRAIN done frame=%d", n));
  m_mode = Mode::LOCKSTEP;
  m_close_at = -1;
  m_close_why.clear();
  m_store.Clear();
  LockstepCheck(n);
  return EndCall(n, MakeReply(LocalAt(n), RemoteAt(n), 0, n), reply);
}

// The local drain is over: every remote pad up to this frame is confirmed and nothing is due. Install the new snapshot set and go on ARMED
// with fresh slots. The peer never learns of it (it is the same stream of frames played with the same pads).
bool RollbackSession::FinishRelayout(const Command& command, const Gate& gate, Reply* reply)
{
  const s32 n = m_cur_frame;
  m_relayout = false;
  m_relayout_frame = n;
  if (gate.fighters_found)
  {
    if (!m_store.Relayout())
    {
      Line(Fmt("ERROR frame %d: the new snapshot set is not usable", n));
      LeaveSession("snapshot set unusable after a relayout", true);
      return EndCall(n, EchoReply(command), reply);
    }
    Line(Fmt("GATE relayout done frame=%d", n));
  }
  else
  {
    m_store.Clear();  // the fighters are gone: the close is on its way; the set is installed at the next arm
  }
  m_mode = Mode::ARMED;
  for (Used& u : m_used)
    u = Used{};
  m_finalized = n - 1;
  return StepArmed(command, gate, reply);
}

// ---- stall bookkeeping -----------------------------------------------------------------------------------------------------------

void RollbackSession::EndStall(bool hit_timeout)
{
  if (!m_in_stall)
    return;
  m_in_stall = false;
  const u64 now = m_net.Now();
  const u64 us = now >= m_stall_start_us ? now - m_stall_start_us : 0;
  m_stats.stall_us_total += us;
  m_stats.stall_us_max = std::max(m_stats.stall_us_max, us);
  m_win.stall_us += us;
  m_stall_run_us += us;
  const std::string text =
      Fmt("STALL %s frame=%d ms=%.1f R=%d", hit_timeout ? "timeout" : "end", m_cur_frame, static_cast<double>(us) / 1000.0, Confirmed());
  if (hit_timeout || m_stall_logged)
    Line(text);  // the end of a session: never limited; the end of a stall goes with its start line (written or left out together)
  else
  {
    ++m_win.suppressed;
    ++m_stats.log_suppressed;
  }
}

// ---- M5: stall run, pacing, resimulation cost, adaptive MAXR ---------------------------------------------------------------------

void RollbackSession::NoteCallWithoutStall()
{
  m_stall_run = 0;
  m_stall_run_us = 0;
}

// Slippi (EXI_DeviceSlippi.cpp:1380-1400) force-disconnects after 420 frames (7 s) in which a player kept needing inputs it did not have. Its
// halted frames each take one frame time, so "420 halted frames" is "7 s without progress". Here a stalled call is one blocking wait: a single
// wait of 420 frame times is the BlockTimeoutMs timeout (BlockTimedOut); this is the other form, many short stalls in a row. A run of
// stalled calls counts only when its waits add up to as many frame times as calls (the game then runs at 30 fps or less the whole time): a
// link that stalls every call by a millisecond or two is slow but playable and must not end the session.
bool RollbackSession::StallRunTooLong()
{
  const u32 limit = m_cfg.stall_disconnect_frames;
  if (limit == 0 || m_stall_run < limit || m_stall_run_us < static_cast<u64>(limit) * static_cast<u64>(PACE_FRAME_US))
    return false;
  LeaveSession(Fmt("stall: %u consecutive stalled frames, %.1f s of waiting for the peer (the link cannot keep up)", m_stall_run,
                   static_cast<double>(m_stall_run_us) / 1e6),
               true);
  return true;
}

// ARMED, normal calls only (m_pace_allowed; the callers are the two successful ends of StepArmed): a check when one is due, then the next slice of an accepted
// delay. Everything here only moves host wall time; the reply of the call is already decided.
void RollbackSession::Pace(s32 frame)
{
  if (!m_pace_allowed)
    return;  // a pending delay waits for the next normal ARMED call
  const PaceConfig& pc = m_cfg.pace;
  if (m_pace_pending_us == 0 && frame >= m_pace_next && !m_hold)
  {
    s32 offset_us = 0;
    u32 samples = 0;
    const u64 rtt_us = m_net.RttUs();
    // Not ready (too few samples since the last correction, or no RTT yet): try again on the next call, nothing is logged.
    if (m_net.TimeOffsetUs(&offset_us, &samples) && samples >= pc.min_samples && rtt_us >= pc.min_rtt_us)
    {
      const s32 since = frame - m_armed_frame;
      PaceDecision d = DecidePace(pc, since, offset_us);
      m_pace_next = frame + pc.interval_frames;
      ++m_stats.pace_checks;
      m_stats.pace_offset_sum_us += offset_us;
      if (m_stats.pace_checks == 1)
        m_stats.pace_offset_max_us = m_stats.pace_offset_min_us = offset_us;
      m_stats.pace_offset_max_us = std::max(m_stats.pace_offset_max_us, offset_us);
      m_stats.pace_offset_min_us = std::min(m_stats.pace_offset_min_us, offset_us);

      std::string action = d.action;
      s32 sleep_us = 0;
      std::string would;
      if (d.delay_us > 0)
      {
        if (pc.enabled && m_pacer != nullptr)
        {
          m_pace_pending_us = d.delay_us;
          sleep_us = d.delay_us;
          ++m_stats.pace_slowdowns;
        }
        else
        {
          would = Fmt(" would=%s:%d", d.action, d.delay_us);
          action = "off";
          if (pc.enabled && !m_pace_noted)
          {
            m_pace_noted = true;
            Line("NOTE pacing has no actuator in this run: the offset is measured and logged, nothing is delayed");
          }
        }
      }
      const bool hard = d.delay_us > 0 && std::string(d.action) == "hard";
      const std::string text =
          Fmt("PACE frame=%d since_gate=%d offset_us=%d adv=%.2f rtt_ms=%.1f samples=%u action=%s sleep_us=%d%s", frame, since, offset_us,
              static_cast<double>(offset_us) / PACE_FRAME_US, static_cast<double>(rtt_us) / 1000.0, samples, action.c_str(), sleep_us, would.c_str());
      // The first PACE_LOG_FULL_LINES checks are written in full, later ones only when they are a hard correction or one in PACE_LOG_EVERY.
      if (m_pace_check_lines < PACE_LOG_FULL_LINES || hard || m_stats.pace_checks % PACE_LOG_EVERY == 0)
      {
        ++m_pace_check_lines;
        Line(text);
      }
      else
        ++m_stats.pace_lines_left_out;
    }
  }
  if (m_pace_pending_us > 0)
  {
    const s32 slice = std::min(m_pace_pending_us, std::max(1, pc.slice_us));
    m_pace_pending_us -= slice;
    if (m_pacer != nullptr)
      m_pacer->Delay(static_cast<u32>(slice));
    m_stats.pace_sleep_us_total += static_cast<u64>(slice);
    if (m_pace_pending_us == 0)
      m_net.ResetTimeOffsets();  // the samples taken so far describe the timing before this correction
  }
}

void RollbackSession::NoteResimCost(s32 depth, u64 cost_us)
{
  const u32 cost = static_cast<u32>(std::min<u64>(cost_us, 0xFFFFFFFFull));
  ++m_stats.resim_costs;
  if (m_resim_cost_us.size() < RESIM_COST_KEEP)
    m_resim_cost_us.push_back(cost);
  else
  {
    m_resim_cost_us[m_resim_cost_next] = cost;
    m_resim_cost_next = (m_resim_cost_next + 1) % RESIM_COST_KEEP;
  }
  m_resim_unit_us[m_resim_unit_count % m_resim_unit_us.size()] = cost / static_cast<u32>(std::max<s32>(depth, 1));
  ++m_resim_unit_count;
  AdaptMaxr();
}

bool RollbackSession::ResimCostPercentileUs(int percent, u32* out) const
{
  if (m_resim_cost_us.empty())
    return false;
  std::vector<u32> sorted = m_resim_cost_us;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  // Nearest rank: the smallest value with at least `percent` % of the samples at or below it.
  const size_t rank = (static_cast<size_t>(std::max(0, std::min(100, percent))) * n + 99) / 100;
  *out = sorted[std::max<size_t>(rank, 1) - 1];
  return true;
}

// Adaptive MAXR (docs/rollback_netplay_plan.md 3.5): MAXR_eff is the largest K for which the resimulation of K frames costs less than the
// budget at its p95. The cost of a rollback is not linear in its depth (the restore and the live frame are fixed parts) but the per-frame
// cost cost/depth over-estimates for shallow rollbacks, which are the common ones, so the estimate errs on the side of a smaller K. Local: the stall
// rule is the only thing that uses MAXR_eff; the gate close and the slot count keep using the configured MAXR, which both machines share.
// Down at once, up by one per ADAPT_RAISE_WAIT measured rollbacks, never above the configured MAXR, never below 1.
void RollbackSession::AdaptMaxr()
{
  constexpr size_t MIN_SAMPLES = 8;       // measured rollbacks before the first change
  constexpr u32 ADAPT_RAISE_WAIT = 16;    // measured rollbacks between two increases
  if (!m_cfg.maxr_adapt)
    return;
  const size_t n = std::min(m_resim_unit_count, m_resim_unit_us.size());
  if (n < MIN_SAMPLES)
    return;
  std::vector<u32> units(m_resim_unit_us.begin(), m_resim_unit_us.begin() + static_cast<std::ptrdiff_t>(n));
  std::sort(units.begin(), units.end());
  const u32 p95 = units[std::max<size_t>((95 * n + 99) / 100, 1) - 1];
  const u32 k_raw = p95 == 0 ? static_cast<u32>(MAX_ROLLBACK_CEILING) : m_cfg.maxr_budget_us / p95;
  const int k = static_cast<int>(std::max<u32>(1, std::min<u32>(k_raw, static_cast<u32>(m_cfg.max_rollback))));
  int next = m_maxr_eff;
  if (k < m_maxr_eff)
  {
    next = k;
    m_maxr_raise_wait = ADAPT_RAISE_WAIT;
  }
  else if (k > m_maxr_eff)
  {
    if (m_maxr_raise_wait > 0)
      --m_maxr_raise_wait;
    else
    {
      next = m_maxr_eff + 1;
      m_maxr_raise_wait = ADAPT_RAISE_WAIT;
    }
  }
  if (next == m_maxr_eff)
    return;
  Line(Fmt("MAXR eff %d -> %d (p95 resimulation cost %.2f ms per frame, budget %.1f ms, configured %d, %zu rollbacks measured)", m_maxr_eff, next,
           static_cast<double>(p95) / 1000.0, static_cast<double>(m_cfg.maxr_budget_us) / 1000.0, m_cfg.max_rollback, n));
  m_maxr_eff = next;
  m_stats.maxr_eff = next;
  m_stats.maxr_eff_min = std::min(m_stats.maxr_eff_min, next);
  ++m_stats.maxr_changes;
}

// ---- checksums -------------------------------------------------------------------------------------------------------------------

void RollbackSession::FinalizeChecks(s32 limit)
{
  const s32 vlimit = std::min(limit, m_verified + 1);
  for (s32 f = m_finalized + 1; f <= vlimit && m_mode != Mode::OFFLINE; ++f)
  {
    const Used& u = UsedAt(f);
    if (u.frame == f)
    {
      if (u.has_checksum)
      {
        if (m_final_observer)
          m_final_observer(f, u.checksum);
        if (f % CHECK_INTERVAL == 0)
          RecordFinal(f, u.checksum, u.has_fields ? &u.fields : nullptr, false);
      }
      // THE GATE RULE: the first FINAL frame whose recorded state says "lost" schedules the close, for the same frame on both machines.
      if (u.lost != 0 && m_close_at < 0 && (m_mode == Mode::ARMED || m_relayout))
        ScheduleClose(f, u.lost);
    }
    m_finalized = f;
  }
}

void RollbackSession::ScheduleClose(s32 final_frame, u8 lost)
{
  m_close_at = final_frame + m_cfg.max_rollback + 1;
  m_close_why = Fmt("final frame %d: %s", final_frame, lost == 2 ? "fighters not found" : "a fighter at health 0");
  Line(Fmt("GATE close scheduled final_frame=%d effective_frame=%d why=%s", final_frame, m_close_at, m_close_why.c_str()));
}

void RollbackSession::LockstepCheck(s32 frame)
{
  if (frame < 0 || frame % CHECK_INTERVAL != 0)
    return;
  FieldSet fields;
  const u32 value = m_store.LiveChecksum(frame, &fields);
  RecordFinal(frame, value, &fields, true);
}

// `live`: the caller is at the hook call of `frame` itself (lockstep), so the live memory is that frame's start state.
void RollbackSession::RecordFinal(s32 frame, u32 value, const FieldSet* fields, bool live)
{
  m_own[frame] = OwnCheck{value, false};
  if (fields != nullptr)
    m_own_fields[frame] = *fields;
  while (m_own.size() > CHECK_KEEP)
    m_own.erase(m_own.begin());
  while (m_own_fields.size() > CHECK_KEEP)
    m_own_fields.erase(m_own_fields.begin());
  m_send_chk_frame = frame;  // rides in every PAD packet from the next SendLocal on
  m_send_chk_value = value;
  if (m_cfg.range_crc)
    ExchangeRanges(frame, live);
  CompareChecksums();
}

void RollbackSession::CompareChecksums()
{
  if (m_mode == Mode::OFFLINE)
    return;
  for (auto& entry : m_own)
  {
    if (entry.second.compared)
      continue;
    const auto peer = m_peer.find(entry.first);
    if (peer == m_peer.end())
      continue;
    entry.second.compared = true;
    if (peer->second == entry.second.value)
    {
      ++m_stats.checks_ok;
      Line(Fmt("CHK frame=%d value=0x%08X", entry.first, entry.second.value));
      m_peer.erase(peer);
    }
    else
    {
      Desync(entry.first, entry.second.value, peer->second);
      return;
    }
  }
  // A peer value older than our oldest kept one can never be compared any more.
  if (!m_own.empty())
    m_peer.erase(m_peer.begin(), m_peer.lower_bound(m_own.begin()->first));
}

DesyncDump RollbackSession::BuildDump(s32 frame)
{
  DesyncDump dump;
  dump.frame = frame;
  for (s32 f = std::max(0, frame - CHECK_INTERVAL); f <= frame; ++f)
  {
    FieldSet fields;
    if (m_store.Fields(f, &fields))
      dump.trace.emplace_back(f, fields);
    else
    {
      const auto own = m_own_fields.find(f);
      if (own != m_own_fields.end())
        dump.trace.emplace_back(f, own->second);
    }
  }
  return dump;
}

void RollbackSession::Desync(s32 frame, u32 own, u32 peer)
{
  ++m_stats.checks_bad;
  Line(Fmt("DESYNC frame=%d own=0x%08X peer=0x%08X (the previous check frame matched: the divergence lies in (%d, %d])", frame, own, peer,
           frame - CHECK_INTERVAL, frame));
  m_sink.Osd(Fmt("Rev3 netplay: desync at frame %d, leaving the session", frame));
  m_own_dump = BuildDump(frame);
  m_dump_pending = true;
  m_dump_polls = 0;
  m_net.SendReliable(MsgType::DesyncDump, SerializeDesyncDump(m_own_dump));
  LeaveSession("desync", true);
}

// Both sides' dumps are here: name the first frame whose 61-field arrays differ and the first field in it, and write the arrays of both sides
// for that frame (all fields, both players), so the log alone is a complete bug report.
void RollbackSession::CompareDumps(const DesyncDump& peer)
{
  m_dump_pending = false;
  s32 last_equal = -1;
  for (const auto& ours : m_own_dump.trace)  // ascending: the first difference found is the earliest
  {
    const FieldSet* theirs = nullptr;
    for (const auto& candidate : peer.trace)
      if (candidate.first == ours.first)
      {
        theirs = &candidate.second;
        break;
      }
    if (theirs == nullptr)
      continue;
    if (ours.second == *theirs)
    {
      last_equal = ours.first;
      continue;
    }
    std::vector<std::string> lines;
    const size_t n = DiffFields(ours.second, *theirs, 8, &lines);
    std::string first_field = "(fighters found)";
    u32 own_value = 0;
    u32 peer_value = 0;
    for (size_t p = 0; p < 2 && n != 0; ++p)
    {
      bool done = false;
      for (size_t i = 0; i < FIELD_COUNT && ((ours.second.present & theirs->present) & (1u << p)) != 0; ++i)
      {
        if (ours.second.value[p][i] == theirs->value[p][i])
          continue;
        first_field = Fmt("p%zu.%s", p + 1, FIELDS[i].name);
        own_value = ours.second.value[p][i];
        peer_value = theirs->value[p][i];
        done = true;
        break;
      }
      if (done)
        break;
    }
    Line(Fmt("DESYNC first_diff frame=%d field=%s own=0x%08X peer=0x%08X fields_differing=%zu last_equal_frame=%d", ours.first, first_field.c_str(),
             own_value, peer_value, n, last_equal));
    for (const std::string& line : lines)
      Line("  " + line);
    for (size_t p = 0; p < 2; ++p)
    {
      const unsigned player = static_cast<unsigned>(p + 1);
      for (int side = 0; side < 2; ++side)
      {
        const FieldSet& set = side == 0 ? ours.second : *theirs;
        const char* name = side == 0 ? "own" : "peer";
        if ((set.present & (1u << p)) == 0)
          Line(Fmt("DESYNC fields frame=%d side=%s player=%u not found", ours.first, name, player));
        else
          Line(Fmt("DESYNC fields frame=%d side=%s player=%u ", ours.first, name, player) + FieldsText(set, p));
      }
    }
    return;
  }
  Line(Fmt("DESYNC dump: no differing frame in the common trace (own %zu frames, peer %zu frames, last equal frame %d)", m_own_dump.trace.size(),
           peer.trace.size(), last_equal));
}

// ---- messages and lifecycle ----------------------------------------------------------------------------------------------------

void RollbackSession::PollOffline()
{
  if (!m_dump_pending)
    return;
  PumpMessages();
  if (++m_dump_polls > 600)  // about 10 s of game frames: the peer's dump is not coming
    m_dump_pending = false;
}

void RollbackSession::PumpMessages()
{
  const std::vector<PeerChecksum> seen = m_net.TakePeerChecksums();
  if (m_mode != Mode::OFFLINE)
  {
    for (const PeerChecksum& e : seen)
      m_peer[e.frame] = e.value;
    while (m_peer.size() > 4 * CHECK_KEEP)
      m_peer.erase(m_peer.begin());
  }
  Message message;
  while (m_net.PollReliable(&message))
    HandleMessage(message);
  if (m_mode != Mode::OFFLINE)
    CompareChecksums();
}

void RollbackSession::HandleMessage(const Message& message)
{
  switch (message.type)
  {
  case MsgType::Hello:
  {
    if (m_mode != Mode::CONNECTING || m_peer_hello_ok)
      return;
    Hello theirs;
    if (!ParseHello(message.bytes, &theirs))
    {
      Line("ERROR bad HELLO message");
      LeaveSession("malformed HELLO from the peer", true);
      return;
    }
    const std::vector<std::string> mismatches = HelloMismatches(m_cfg.hello, theirs);
    if (!mismatches.empty())
    {
      Line(Fmt("HELLO mismatch (%zu items), refusing the session:", mismatches.size()));
      for (const std::string& item : mismatches)
        Line("  HELLO " + item);
      m_sink.Osd("Rev3 netplay: the two machines differ (" + mismatches.front() + "); playing offline");
      LeaveSession("HELLO mismatch: " + mismatches.front(), true);
      return;
    }
    if (m_cfg.role == Role::Guest)
    {
      const int host_delay = static_cast<int>(std::min<u32>(theirs.delay, static_cast<u32>(MAX_DELAY)));
      if (host_delay != m_delay)
        Line(Fmt("HELLO input delay: the host's %d wins (own %d)", host_delay, m_delay));
      m_delay = host_delay;
    }
    m_peer_hello_ok = true;
    Line(Fmt("HELLO ok peer=%s game=%s protocol=%u delay=%d items=%zu", RoleName(theirs.role), theirs.game_id.c_str(), theirs.protocol,
             m_delay, theirs.items.size()));
    return;
  }
  case MsgType::StartCheck:
  {
    StartCheck check;
    if (m_mode == Mode::OFFLINE || !ParseStartCheck(message.bytes, &check))
      return;
    m_peer_start[check.frame] = check;
    while (m_peer_start.size() > 8)
      m_peer_start.erase(m_peer_start.begin());
    return;
  }
  case MsgType::DesyncDump:
  {
    DesyncDump dump;
    if (!ParseDesyncDump(message.bytes, &dump))
      return;
    if (m_mode != Mode::OFFLINE)
    {
      // The peer saw the mismatch first: record what we have, answer with our dump, leave.
      ++m_stats.checks_bad;
      Line(Fmt("DESYNC frame=%d reported by the peer", dump.frame));
      m_sink.Osd(Fmt("Rev3 netplay: desync at frame %d, leaving the session", dump.frame));
      m_own_dump = BuildDump(dump.frame);
      m_net.SendReliable(MsgType::DesyncDump, SerializeDesyncDump(m_own_dump));
      m_dump_pending = true;
      CompareDumps(dump);
      LeaveSession("desync (reported by the peer)", true);
      return;
    }
    if (m_dump_pending)
      CompareDumps(dump);
    return;
  }
  case MsgType::RangeCrc:
  {
    RangeCrcMsg msg;
    if (!m_cfg.range_crc || m_mode == Mode::OFFLINE || !ParseRangeCrc(message.bytes, &msg))
      return;  // only a run with REV3_NET_RANGE_CRC=1 listens; the other side's setting is no mismatch
    m_peer_ranges[msg.frame] = std::move(msg.ranges);
    while (m_peer_ranges.size() > RANGE_KEEP)
      m_peer_ranges.erase(m_peer_ranges.begin());
    CompareRanges(msg.frame);
    return;
  }
  case MsgType::Bye:
  {
    if (m_mode == Mode::OFFLINE)
      return;
    const std::string reason(message.bytes.begin(), message.bytes.end());
    Line("BYE from the peer: " + reason);
    LeaveSession("the peer left (" + reason + ")", false);
    return;
  }
  }
}

void RollbackSession::SendHello()
{
  m_cfg.hello.role = m_cfg.role;
  m_cfg.hello.delay = static_cast<u32>(m_delay);
  m_net.SendReliable(MsgType::Hello, SerializeHello(m_cfg.hello));
  m_hello_sent = true;
  Line(Fmt("HELLO sent role=%s game=%s protocol=%u fields_digest=0x%08X delay=%d items=%zu", RoleName(m_cfg.role),
           m_cfg.hello.game_id.c_str(), m_cfg.hello.protocol, m_cfg.hello.fields_digest, m_delay, m_cfg.hello.items.size()));
}

void RollbackSession::LeaveSession(const std::string& why, bool tell_peer)
{
  if (m_mode == Mode::OFFLINE)
    return;
  Line(Fmt("LEAVE frame=%d from=%s reason=%s", m_cur_frame, ModeName(m_mode), why.c_str()));
  m_sink.Osd("Rev3 netplay: " + why + " (continuing offline)");
  if (tell_peer && m_net.State() != ConnState::Failed)
    m_net.SendReliable(MsgType::Bye, std::vector<u8>(why.begin(), why.end()));
  if (!m_finished)
  {
    m_finished = true;
    FlushWindow();
    SummaryLine("SUMMARY");
  }
  m_mode = Mode::OFFLINE;
  m_resim = false;
  m_relayout = false;
  m_hold = false;
  m_close_at = -1;
  m_cur_frame = -1;
  m_block = Block::None;
  m_store.Clear();
}

void RollbackSession::SummaryLine(const char* tag)
{
  std::string hist;
  for (size_t d = 1; d < m_stats.depth_hist.size(); ++d)
    if (m_stats.depth_hist[d] != 0)
      hist += Fmt("%s%zu:%u", hist.empty() ? "" : ",", d, m_stats.depth_hist[d]);
  std::string sim;
  const NetSimStats ns = m_net.NetSimCounts();
  if (ns.active)
    sim = Fmt(" netsim_dropped=%u netsim_held=%u", ns.dropped, ns.held);
  std::string ranges;
  if (m_cfg.range_crc)
    ranges = Fmt(" range_checks=%u range_diff_frames=%u", m_stats.range_checks, m_stats.range_diff_frames);
  // M5: pacing (adv_* in frames of 16.683 ms, positive = ahead of the peer), the stall run, the cost of a rollback, the stall rule's limit.
  const double checks = static_cast<double>(std::max<u32>(m_stats.pace_checks, 1));
  const auto adv = [](double us) { return us / PACE_FRAME_US; };
  u32 p50 = 0, p95 = 0, pmax = 0;
  ResimCostPercentileUs(50, &p50);
  ResimCostPercentileUs(95, &p95);
  ResimCostPercentileUs(100, &pmax);
  const std::string m5 = Fmt(
      " pace=%s pace_checks=%u pace_slowdowns=%u pace_sleep_ms_total=%.1f adv_mean=%.2f adv_max=%.2f adv_min=%.2f pace_lines_left_out=%u "
      "stall_run_max=%u resim_n=%u resim_skipped=%u resim_ms_p50=%.2f resim_ms_p95=%.2f resim_ms_max=%.2f maxr_eff=%d maxr_eff_min=%d maxr_changes=%u",
      m_cfg.pace.enabled ? "on" : "off", m_stats.pace_checks, m_stats.pace_slowdowns, static_cast<double>(m_stats.pace_sleep_us_total) / 1000.0,
      adv(static_cast<double>(m_stats.pace_offset_sum_us) / checks), adv(m_stats.pace_offset_max_us), adv(m_stats.pace_offset_min_us),
      m_stats.pace_lines_left_out, m_stats.stall_run_max, m_stats.resim_costs, m_stats.resim_cost_skipped, p50 / 1000.0, p95 / 1000.0,
      pmax / 1000.0, m_stats.maxr_eff, m_stats.maxr_eff_min, m_stats.maxr_changes);
  const std::string store_note = m_store.SummaryNote();  // may write the `NOTE learned summary` line, so before the SUMMARY line below
  Line(Fmt("%s mode=%s frame=%d calls=%u resim_calls=%u rollbacks=%u max_depth=%u depth_hist=[%s] predicted=%u stalls=%u stall_ms_max=%.1f "
           "postponed=%u forced_hw=%u hw_holds=%u hold_calls=%u relayouts=%u chk_ok=%u chk_bad=%u gate_opens=%u gate_closes=%u gate_late=%u "
           "arm_refused=%u log_suppressed=%u%s%s%s",
           tag, ModeName(m_mode), m_expected_next, m_stats.calls, m_stats.resim_calls, m_stats.rollbacks, m_stats.max_depth, hist.c_str(),
           m_stats.predicted_frames, m_stats.stalls, static_cast<double>(m_stats.stall_us_max) / 1000.0, m_stats.postponed,
           m_stats.forced_hw, m_stats.hw_holds, m_stats.hold_calls, m_stats.relayouts, m_stats.checks_ok, m_stats.checks_bad,
           m_stats.gate_opens, m_stats.gate_closes, m_stats.gate_late, m_stats.arm_refused, m_stats.log_suppressed, sim.c_str(),
           ranges.c_str(), m5.c_str()) +
       store_note);
}

void RollbackSession::Finish()
{
  if (m_finished)
    return;
  m_finished = true;
  FlushWindow();
  SummaryLine("SUMMARY final");
}

// ---- rate-limited detail lines -----------------------------------------------------------------------------------------------------
// The counters of every event are kept exactly (SessionStats and the window); only the text of the lines past the first LOG_FULL_LINES of a
// kind is replaced by one summary line per window of LOG_WINDOW_FRAMES frames.

// True: the line was written. False: left out (past the first LOG_FULL_LINES of its kind), counted in the window.
bool RollbackSession::DetailLine(Detail kind, const std::string& text)
{
  u32& written = kind == Detail::Stall ? m_detail_stall : m_detail_rollback;
  if (written < LOG_FULL_LINES)
  {
    ++written;
    Line(text);
    return true;
  }
  ++m_win.suppressed;
  ++m_stats.log_suppressed;
  return false;
}

void RollbackSession::WindowTick(s32 frame)
{
  if (!m_win.begun)
  {
    m_win.begun = true;
    m_win.start = frame;
  }
  else if (frame >= m_win.start + LOG_WINDOW_FRAMES)
  {
    FlushWindow();
    m_win.begun = true;
    m_win.start = frame;
  }
  m_win.last = frame;
}

void RollbackSession::FlushWindow()
{
  if (m_win.suppressed != 0)
  {
    std::string hist;
    for (size_t d = 1; d < m_win.depth.size(); ++d)
      if (m_win.depth[d] != 0)
        hist += Fmt("%s%zu:%u", hist.empty() ? "" : ",", d, m_win.depth[d]);
    Line(Fmt("ROLLBACK depth_hist=%s window=%d-%d rollbacks=%u stalls=%u stall_ms=%.1f postponed=%u forced_hw=%u lines_left_out=%u",
             hist.empty() ? "none" : hist.c_str(), m_win.start, m_win.last, m_win.rollbacks, m_win.stalls,
             static_cast<double>(m_win.stall_us) / 1000.0, m_win.postponed, m_win.forced, m_win.suppressed));
  }
  m_win = Window{};
}

// ---- REV3_NET_RANGE_CRC ------------------------------------------------------------------------------------------------------------

void RollbackSession::ExchangeRanges(s32 frame, bool live)
{
  std::vector<RangeCrc> ranges;
  if (!m_store.RangeCrcs(frame, live, &ranges))
  {
    if (++m_range_misses <= 3)
      Line(Fmt("NOTE range_crc frame=%d: the snapshot ranges of that frame are not available (no slot, no ranges)", frame));
    return;
  }
  RangeCrcMsg msg;
  msg.frame = frame;
  msg.ranges = ranges;
  m_net.SendReliable(MsgType::RangeCrc, SerializeRangeCrc(msg));
  m_own_ranges[frame] = std::move(ranges);
  while (m_own_ranges.size() > RANGE_KEEP)
    m_own_ranges.erase(m_own_ranges.begin());
  CompareRanges(frame);
}

// Both sides' CRCs of the ranges at `frame` are here: name the ranges that differ. Never a desync (some ranges differ between two healthy
// runs: volatile audio / picture memory); it is what tells which memory to look at when the field checksum fires.
void RollbackSession::CompareRanges(s32 frame)
{
  const auto own_it = m_own_ranges.find(frame);
  const auto peer_it = m_peer_ranges.find(frame);
  if (own_it == m_own_ranges.end() || peer_it == m_peer_ranges.end())
    return;
  ++m_stats.range_checks;
  std::map<std::pair<u32, u32>, std::pair<const RangeCrc*, const RangeCrc*>> merged;
  for (const RangeCrc& r : own_it->second)
    merged[{r.start, r.end}].first = &r;
  for (const RangeCrc& r : peer_it->second)
    merged[{r.start, r.end}].second = &r;
  std::vector<std::string> lines;
  size_t differing = 0;
  for (const auto& entry : merged)
  {
    const RangeCrc* a = entry.second.first;
    const RangeCrc* b = entry.second.second;
    if (a != nullptr && b != nullptr && a->crc == b->crc)
      continue;
    ++differing;
    if (lines.size() >= MAX_RANGE_DIFFS_LOGGED)
      continue;
    if (a != nullptr && b != nullptr)
      lines.push_back(Fmt("  range 0x%08X-0x%08X own=0x%08X peer=0x%08X", entry.first.first, entry.first.second, a->crc, b->crc));
    else
      lines.push_back(Fmt("  range 0x%08X-0x%08X only on the %s side", entry.first.first, entry.first.second, a != nullptr ? "own" : "peer"));
  }
  if (differing != 0)
  {
    ++m_stats.range_diff_frames;
    Line(Fmt("NOTE range_crc frame=%d ranges=%zu differing=%zu (no desync: information only)", frame, merged.size(), differing));
    for (const std::string& line : lines)
      Line(line);
    if (differing > lines.size())
      Line(Fmt("  ... and %zu more differing ranges", differing - lines.size()));
  }
  m_own_ranges.erase(own_it);
  m_peer_ranges.erase(peer_it);
}
}  // namespace Rev3::Net
