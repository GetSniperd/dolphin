// Rev3 rollback support: command/reply codec, whitelist parser, the SyncTest state machine and the Dolphin glue.
// See Rev3Rollback.h for what it does, how to enable it (REV3_SYNCTEST ...) and where it fits in. Licensed under GPLv2+.
//
// Layout of this file: (1) wire codec, (2) whitelist parser, (3) the fighter field table, (4) Rev3::SyncTest (pure logic, only
// GuestMemory / Sink / a clock), (5) Dolphin glue: env vars, log file, memory accessor, HandleCommand / Shutdown.
// To change which fighter fields are compared, edit FIELDS (and FIELD_COUNT in the header) to match rev3-mod rbchecksum.py.

#include "Core/Slippi/Rev3Rollback.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Rev3
{
namespace
{
// Where Rev3 keeps things in guest memory (rev3-mod rbchecksum.py _ok: MEM1 0x80000000 + 24 MB, MEM2 0x90000000 + 64 MB).
constexpr u32 MEM1_BASE = 0x80000000;
constexpr u32 MEM1_END = 0x81800000;
constexpr u32 MEM2_BASE = 0x90000000;
constexpr u32 MEM2_END = 0x94000000;

// SyncTest arms after SyncTestOptions::arm_commands consecutive hook calls (one per game frame) with both fighters found (default
// 600): the stage must be loaded and the fight running, not loading (loading fills memory by DVD DMA and runs threads, which a
// memory snapshot knows nothing about). The first real run froze at the end of a stage load with a 60 call delay.
constexpr u32 HEARTBEAT_EVERY = 300;              // "HB" log line every this many hook calls while armed (finds where a hang is) ...
constexpr u32 HEARTBEAT_EARLY_EVERY = 60;         // ... but every this many hook calls for the first HEARTBEAT_EARLY_CALLS armed calls
constexpr u32 HEARTBEAT_EARLY_CALLS = 600;        // (the first real rewinds are where the game froze)
constexpr u32 MAX_RESTORE_LOGS = 10;              // the first restores log RESTORE begin / done lines (bytes, microseconds)
constexpr u32 MAX_SHORT_RESTORE_LOGS = 30;        // restores 11..30 log one short line "RESTORE n -> m us=.." (places a later hang)
constexpr u32 MAX_EXCLUSION_LINES = 64;           // "NOTE exclude" lines (buffers that alternate would otherwise flood the log)

constexpr u32 MAX_DETAILED_MISMATCH_FRAMES = 20;  // full field / range detail for this many mismatching frames, then counts only
constexpr u32 MAX_LOGGED_ERRORS = 50;             // error lines (sp mismatch, capture failure ...) before they go silent
constexpr u32 SUMMARY_EVERY_COMPARED = 600;       // frames compared between two summary lines
constexpr size_t MAX_WORDS_LISTED = 8;            // differing words listed per differing range in a detailed mismatch
constexpr size_t MAX_RANGES_LISTED = 12;          // differing ranges listed per mismatching frame

// The fighter struct bytes the table reads: one span read per fighter (rbchecksum.fighter_bytes reads min offset .. max offset + 4).
constexpr u32 FIELD_SPAN_LO = 0x24;
constexpr u32 FIELD_SPAN_HI = 0x1648;

constexpr u32 ReadBE32(const u8* p)
{
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

void WriteBE32(u8* p, u32 v)
{
  p[0] = static_cast<u8>(v >> 24);
  p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);
  p[3] = static_cast<u8>(v);
}

std::string Format(const char* fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0)
    return {};
  return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
}

bool InMem1(u32 p)
{
  return p >= MEM1_BASE && p < MEM1_END;
}
bool InMem2(u32 p)
{
  return p >= MEM2_BASE && p < MEM2_END;
}
bool PointerOk(u32 p)
{
  return InMem1(p) || InMem2(p);
}

// One past the last address of the memory region `addr` is in, or 0 when it is in neither.
u32 RegionEnd(u32 addr)
{
  if (InMem1(addr))
    return MEM1_END;
  if (InMem2(addr))
    return MEM2_END;
  return 0;
}

u64 SteadyMicros()
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}
}  // namespace

// ---- (1) wire codec -----------------------------------------------------------------------------------------------------

bool IsCommand(const u8* data, size_t size)
{
  return data != nullptr && size >= CMD_SIZE && data[0] == CMD_ID && ReadBE32(data + 4) == CMD_MAGIC;
}

bool ParseCommand(const u8* data, size_t size, Command* out)
{
  if (!IsCommand(data, size) || data[1] != PROTOCOL_VERSION)
    return false;
  out->frame = ReadBE32(data + 0x08);
  std::copy_n(data + 0x0C, 12, out->pads[0].begin());
  std::copy_n(data + 0x18, 12, out->pads[1].begin());
  out->pad_return = static_cast<s32>(ReadBE32(data + 0x24));
  out->sp = ReadBE32(data + 0x28);
  return true;
}

void EncodeReply(const Reply& reply, u8* out64)
{
  std::fill_n(out64, REPLY_SIZE, static_cast<u8>(0));
  WriteBE32(out64 + 0x00, REPLY_MAGIC);
  WriteBE32(out64 + 0x04, reply.flags);
  WriteBE32(out64 + 0x08, reply.frame);
  // +0x0C stays 0
  std::copy(reply.pads[0].begin(), reply.pads[0].end(), out64 + 0x10);
  std::copy(reply.pads[1].begin(), reply.pads[1].end(), out64 + 0x1C);
}

// ---- (2) whitelist ------------------------------------------------------------------------------------------------------

namespace
{
bool ParseAddress(const picojson::value& v, u32* out, std::string* error, const char* what)
{
  if (v.is<std::string>())
  {
    const std::string& s = v.get<std::string>();
    if (s.empty())
    {
      *error = Format("%s is an empty string", what);
      return false;
    }
    const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    char* end = nullptr;
    const unsigned long long value = std::strtoull(s.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
    if (end == nullptr || *end != '\0' || end == s.c_str() + (hex ? 2 : 0) || value > 0xFFFFFFFFull)
    {
      *error = Format("%s is not a 32-bit address: \"%s\"", what, s.c_str());
      return false;
    }
    *out = static_cast<u32>(value);
    return true;
  }
  if (v.is<double>())
  {
    const double d = v.get<double>();
    if (!(d >= 0.0 && d <= 4294967295.0) || d != static_cast<double>(static_cast<u64>(d)))
    {
      *error = Format("%s is not a 32-bit address: %f", what, d);
      return false;
    }
    *out = static_cast<u32>(static_cast<u64>(d));
    return true;
  }
  *error = Format("%s must be a hex string or a number", what);
  return false;
}
}  // namespace

bool ParseWhitelist(const std::string& json_text, std::vector<Range>* ranges, std::string* error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  picojson::value root;
  const std::string parse_error = picojson::parse(root, json_text);
  if (!parse_error.empty())
  {
    *error = "JSON error: " + parse_error;
    return false;
  }
  if (!root.is<picojson::object>() || !root.contains("ranges") || !root.get("ranges").is<picojson::array>())
  {
    *error = "expected {\"ranges\": [...]}";
    return false;
  }
  const picojson::array& list = root.get("ranges").get<picojson::array>();
  std::vector<Range> result;
  for (size_t i = 0; i < list.size(); ++i)
  {
    const picojson::value& item = list[i];
    if (!item.is<picojson::object>() || !item.contains("start") || !item.contains("end"))
    {
      *error = Format("range %zu: expected {\"start\":..., \"end\":...}", i);
      return false;
    }
    Range r;
    const std::string label_start = Format("range %zu start", i);
    const std::string label_end = Format("range %zu end", i);
    if (!ParseAddress(item.get("start"), &r.start, error, label_start.c_str()) ||
        !ParseAddress(item.get("end"), &r.end, error, label_end.c_str()))
    {
      return false;
    }
    if (r.end <= r.start)
    {
      *error = Format("range %zu: end 0x%08X is not after start 0x%08X", i, r.end, r.start);
      return false;
    }
    // end is exclusive, so the last byte is end - 1 and it must be in the same region as start.
    const u32 region_end = RegionEnd(r.start);
    if (region_end == 0 || r.end > region_end)
    {
      *error = Format("range %zu: 0x%08X-0x%08X is not inside MEM1 (0x80000000-0x81800000) or MEM2 (0x90000000-0x94000000)", i,
                      r.start, r.end);
      return false;
    }
    result.push_back(r);
  }
  if (result.empty())
  {
    *error = "the whitelist has no ranges";
    return false;
  }
  *ranges = std::move(result);
  return true;
}

u64 SubtractRange(std::vector<Range>* ranges, u32 lo, u32 hi)
{
  std::vector<Range> result;
  u64 removed = 0;
  for (const Range& r : *ranges)
  {
    if (r.end <= lo || r.start >= hi)
    {
      result.push_back(r);
      continue;
    }
    const u32 cut_lo = std::max(r.start, lo);
    const u32 cut_hi = std::min(r.end, hi);
    removed += cut_hi - cut_lo;
    if (r.start < cut_lo)
      result.push_back({r.start, cut_lo});
    if (cut_hi < r.end)
      result.push_back({cut_hi, r.end});
  }
  *ranges = std::move(result);
  return removed;
}

bool ApplyExclusions(const std::string& json_text, std::vector<Range>* ranges, Sink& sink, std::string* error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  std::vector<Range> exclusions;
  if (!ParseWhitelist(json_text, &exclusions, error))
    return false;
  std::vector<Range> result = *ranges;
  std::vector<std::string> lines;
  for (const Range& e : exclusions)
  {
    const u64 removed = SubtractRange(&result, e.start, e.end);
    lines.push_back(Format("NOTE exclude user 0x%08X-0x%08X (%llu bytes were in the whitelist)", e.start, e.end,
                           static_cast<unsigned long long>(removed)));
  }
  if (result.empty())
  {
    *error = "the exclusions remove every range of the whitelist";
    return false;
  }
  for (const std::string& line : lines)
    sink.Line(line);
  *ranges = std::move(result);
  return true;
}

// ---- (3) the fighter field table ------------------------------------------------------------------------------------------

// rev3-mod rbchecksum.py FIELDS (FIELDS_VERSION 1, fields_digest 0xD4076EF2), same order.
const std::array<FieldDef, FIELD_COUNT> FIELDS = {{
    {"action", 0x140},       {"next_action", 0x150},     {"move_id", 0x158},      {"script_timer", 0x134},
    {"flags1", 0x50},        {"flags2", 0x54},           {"flags3", 0x58},        {"flags4", 0x5C},
    {"flags5", 0x60},        {"hit_received", 0x64},     {"move_flags2", 0x68},   {"flags7", 0x6C},
    {"flags8", 0x70},        {"x", 0x84},                {"y", 0x88},             {"z", 0x8C},
    {"angle9c", 0x9C},       {"anglea0", 0xA0},          {"move_dx", 0xC4},       {"move_dy", 0xC8},
    {"move_dz", 0xCC},       {"ax0_angle", 0xE8},        {"ax0_speed", 0xEC},     {"ax0_decay", 0xF0},
    {"vy", 0xF4},            {"gravity", 0xF8},          {"ax1_angle", 0xFC},     {"ax1_speed", 0x100},
    {"kb_angle", 0x108},     {"kb_speed", 0x10C},        {"kb_decay", 0x110},     {"time_step", 0x12C},
    {"health_max", 0x24},    {"health", 0x28},           {"chakra", 0x44},        {"guard", 0x4C},
    {"damage_in", 0x2C},     {"combo_damage", 0x38},     {"attack_mult", 0x9A4},  {"stun_a", 0x9D0},
    {"down_timer", 0x9D4},   {"stun_b", 0x9DC},          {"juggle_count", 0x9C0}, {"launch_angle", 0x9B8},
    {"combo_count", 0x9AC},  {"combo_timer", 0x9B0},     {"freeze", 0x1640},      {"freeze_next", 0x1644},
    {"hit_damage", 0x184},   {"hit_guard_flags", 0x178}, {"hit_flags", 0x17C},    {"hit_flags2", 0x180},
    {"hit_strength", 0x3D8}, {"hit_reaction", 0x3E4},    {"hit_since_start", 0x1CC}, {"hit_since_end", 0x1D0},
    {"str_queued0", 0x13CC}, {"str_queued1", 0x13D0},    {"str_done", 0x13E4},    {"str_next", 0x13E8},
    {"str_pending", 0x13EC},
}};

// ---- (4) SyncTest -------------------------------------------------------------------------------------------------------

SyncTest::SyncTest(const SyncTestOptions& options, std::vector<Range> ranges, GuestMemory& memory, Sink& sink,
                   std::function<u64()> micros, HardwareView* hardware)
    : m_options(options), m_ranges(std::move(ranges)), m_memory(memory), m_sink(sink), m_micros(std::move(micros)),
      m_hardware(hardware)
{
  if (!m_micros)
    m_micros = SteadyMicros;
  m_options.k = std::clamp(m_options.k, 1, 8);
  m_options.every = std::max(m_options.every, 1);
  m_options.hardware_check_every = std::max<u32>(m_options.hardware_check_every, 1);
  m_span.resize(FIELD_SPAN_HI - FIELD_SPAN_LO);

  // Allocated once: k + 1 slots of ~2.3 MB, plus the scratch slot used while comparing a resimulated frame. Exclusions only
  // shrink the ranges, so RebuildLayout never has to grow them.
  m_ring.resize(static_cast<size_t>(m_options.k) + 1);
  RebuildLayout();

  m_sink.Line(Format("SYNCTEST start K=%d EVERY=%d window=0x%X whitelist=%s ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu "
                     "arm=%s noop=%d hw_exclude=%d icache=%d tb=%d",
                     m_options.k, m_options.every, m_options.stack_window, m_options.whitelist_path.c_str(), m_ranges.size(),
                     m_ranges_bytes, m_snapshot_bytes, m_ring.size(),
                     m_options.arm_by_pad ? "pad" : Format("%u", m_options.arm_commands).c_str(), m_options.noop ? 1 : 0,
                     m_hardware != nullptr ? 1 : 0, m_options.invalidate_code ? 1 : 0, m_options.restore_timebase ? 1 : 0));
  if (m_options.restore_timebase)
    m_sink.Line("NOTE timebase restore on");
}

// Recomputes the offsets and buffer sizes for the current m_ranges. Every slot is invalidated: its data has the old layout.
void SyncTest::RebuildLayout()
{
  m_range_offsets.clear();
  m_ranges_bytes = 0;
  for (const Range& r : m_ranges)
  {
    m_range_offsets.push_back(m_ranges_bytes);
    m_ranges_bytes += r.end - r.start;
  }
  m_snapshot_bytes = m_ranges_bytes + m_options.stack_window;
  for (Slot& s : m_ring)
  {
    s.valid = false;
    s.data.resize(m_snapshot_bytes);
    s.crc.assign(m_ranges.size(), 0);
  }
  m_scratch.valid = false;
  m_scratch.data.resize(m_snapshot_bytes);
  m_scratch.crc.assign(m_ranges.size(), 0);
}

// Asks the emulated hardware which memory it uses on its own and cuts it out of the snapshot ranges. The set only grows (double
// buffers alternate; seeing both once is enough), and a change that touches the snapshot clears the history.
void SyncTest::RefreshExclusions(bool at_arm)
{
  if (m_hardware == nullptr)
    return;
  std::vector<Exclusion> found;
  m_hardware->Query(&found);
  bool layout_changed = false;
  for (const Exclusion& e : found)
  {
    if (e.end <= e.start)
      continue;
    bool known = false;
    for (const Exclusion& old : m_exclusions_logged)
      known = known || (old.what == e.what && old.start == e.start && old.end == e.end);
    if (known)
      continue;
    m_exclusions_logged.push_back(e);
    const u64 removed = SubtractRange(&m_ranges, e.start, e.end);
    layout_changed = layout_changed || removed != 0;
    ++m_exclusion_lines;
    if (m_exclusion_lines <= MAX_EXCLUSION_LINES)
    {
      m_sink.Line(Format("NOTE exclude %s 0x%08X-0x%08X (%llu bytes were in the whitelist)", e.what.c_str(), e.start, e.end,
                         static_cast<unsigned long long>(removed)));
      if (m_exclusion_lines == MAX_EXCLUSION_LINES)
        m_sink.Line("NOTE further exclusions are not logged");
    }
  }
  if (!layout_changed)
    return;
  RebuildLayout();
  ClearHistory();
  m_sink.Line(Format("NOTE snapshot now ranges=%zu bytes=%zu snapshot_bytes=%zu (history cleared, frame %u)", m_ranges.size(),
                     m_ranges_bytes, m_snapshot_bytes, m_last_frame));
  if (m_armed && !at_arm)
    LogFieldCoverage(m_fighters, "after exclusion change");
}

std::string SyncTest::RangeContaining(u32 addr) const
{
  for (const Range& r : m_ranges)
  {
    if (addr >= r.start && static_cast<u64>(addr) + 4 <= r.end)
      return Format("0x%08X-0x%08X", r.start, r.end);
  }
  return {};
}

std::string SyncTest::SnapshotRangeOf(const Slot& slot, u32 addr) const
{
  std::string found = RangeContaining(addr);
  if (found.empty() && slot.stack_len != 0 && addr >= slot.sp && static_cast<u64>(addr) + 4 <= static_cast<u64>(slot.sp) + slot.stack_len)
    found = Format("stack_window 0x%08X-0x%08X", slot.sp, slot.sp + slot.stack_len);
  return found;
}

// Which of the 61 compared fields of each fighter lie inside the current snapshot ranges. A field outside is never rewound, so its
// value after a restore is whatever the first run left there; the list names them.
void SyncTest::LogFieldCoverage(const std::array<u32, 2>& fighters, const char* when)
{
  for (size_t p = 0; p < 2; ++p)
  {
    size_t inside = 0;
    std::string missing;
    for (const FieldDef& f : FIELDS)
    {
      const u32 addr = fighters[p] + f.offset;
      if (!RangeContaining(addr).empty())
      {
        ++inside;
        continue;
      }
      missing += Format(" %s@0x%08X", f.name, addr);
    }
    std::string line = Format("NOTE field coverage p%zu%s%s fighter=0x%08X in_snapshot=%zu/%zu", p + 1, when != nullptr ? " " : "",
                              when != nullptr ? when : "", fighters[p], inside, FIELD_COUNT);
    if (inside != FIELD_COUNT)
      line += " NOT IN SNAPSHOT:" + missing;
    m_sink.Line(line);
  }
}

Reply SyncTest::Echo(const Command& command) const
{
  Reply r;
  r.flags = 0;
  r.frame = command.frame;
  r.pads = command.pads;
  return r;
}

void SyncTest::Error(const std::string& text)
{
  ++m_error_count;
  if (m_error_count <= MAX_LOGGED_ERRORS)
  {
    m_sink.Line("ERROR " + text);
    if (m_error_count == MAX_LOGGED_ERRORS)
      m_sink.Line("ERROR further errors are not logged");
  }
}

void SyncTest::ClearHistory()
{
  for (Slot& s : m_ring)
    s.valid = false;
  m_scratch.valid = false;
  m_resim = false;
  m_have_history = false;
}

void SyncTest::Reset(const char* why)
{
  ClearHistory();
  m_sink.Line(Format("NOTE history cleared: %s (frame %u)", why, m_last_frame));
}

bool SyncTest::FindFighters(std::array<u32, 2>* addrs) const
{
  bool all = true;
  for (u32 team = 0; team < 2; ++team)
  {
    (*addrs)[team] = 0;
    u8 word[4];
    if (!m_memory.Read(TEAMS_ADDR + 4 * team, word, 4))
    {
      all = false;
      continue;
    }
    const u32 team_ptr = ReadBE32(word);
    if (!PointerOk(team_ptr) || !m_memory.Read(team_ptr + 0x0C, word, 4))
    {
      all = false;
      continue;
    }
    const u32 node = ReadBE32(word);
    if (!PointerOk(node) || !m_memory.Read(node, word, 4))
    {
      all = false;
      continue;
    }
    const u32 fighter = ReadBE32(word);
    if (!PointerOk(fighter))
    {
      all = false;
      continue;
    }
    (*addrs)[team] = fighter;
  }
  return all;
}

bool SyncTest::Capture(Slot& slot, const Command& command, const std::array<u32, 2>& fighters)
{
  const u32 region_end = RegionEnd(command.sp);
  if (region_end == 0)
  {
    Error(Format("frame %u: the hook's sp 0x%08X is not in MEM1 / MEM2", command.frame, command.sp));
    return false;
  }
  const u32 stack_len = std::min<u32>(m_options.stack_window, region_end - command.sp);

  size_t offset = 0;
  for (size_t i = 0; i < m_ranges.size(); ++i)
  {
    const size_t len = m_ranges[i].end - m_ranges[i].start;
    if (!m_memory.Read(m_ranges[i].start, slot.data.data() + offset, len))
    {
      Error(Format("frame %u: cannot read whitelist range 0x%08X-0x%08X", command.frame, m_ranges[i].start, m_ranges[i].end));
      return false;
    }
    slot.crc[i] = Common::ComputeCRC32(slot.data.data() + offset, len);
    offset += len;
  }
  if (stack_len != 0 && !m_memory.Read(command.sp, slot.data.data() + offset, stack_len))
  {
    Error(Format("frame %u: cannot read the stack window at 0x%08X", command.frame, command.sp));
    return false;
  }

  for (size_t p = 0; p < 2; ++p)
  {
    if (!m_memory.Read(fighters[p] + FIELD_SPAN_LO, m_span.data(), m_span.size()))
    {
      Error(Format("frame %u: cannot read the fields of fighter 0x%08X", command.frame, fighters[p]));
      return false;
    }
    for (size_t f = 0; f < FIELD_COUNT; ++f)
      std::copy_n(m_span.data() + (FIELDS[f].offset - FIELD_SPAN_LO), 4, slot.fields[p].data() + 4 * f);
    slot.fighter_addr[p] = fighters[p];
  }

  slot.timebase_valid = m_options.restore_timebase && m_memory.ReadTimeBase(&slot.timebase);
  slot.valid = true;
  slot.frame = command.frame;
  slot.sp = command.sp;
  slot.stack_len = stack_len;
  slot.inputs = command.pads;
  return true;
}

// Writes the snapshot `s` (ranges, then the stack window) back, invalidates the JIT for the restored ranges, and times the two
// steps separately. `to` is the frame the game continues from (only used for the log). False: nothing was (fully) restored.
bool SyncTest::Restore(const Slot& s, const Command& command, u32 to)
{
  if (!s.valid || s.frame != to)
  {
    Error(Format("frame %u: no snapshot of frame %u to restore", command.frame, to));
    return false;
  }
  if (s.sp != command.sp)
  {
    // The hook's saved registers live at sp..sp+0x200; restoring them under a different sp would hand the hook garbage.
    ++m_stats.sp_mismatches;
    Error(Format("frame %u: sp 0x%08X differs from the snapshot of frame %u (0x%08X); not restoring", command.frame,
                 command.sp, to, s.sp));
    return false;
  }

  const bool log_it = m_restore_logs < MAX_RESTORE_LOGS;
  const bool log_short = !log_it && m_restore_logs < MAX_SHORT_RESTORE_LOGS;
  if (log_it)
    m_sink.Line(Format("RESTORE %u -> %u begin", command.frame, to));

  const u64 t0 = m_micros();
  size_t offset = 0;
  for (const Range& r : m_ranges)
  {
    const size_t len = r.end - r.start;
    if (!m_memory.Write(r.start, s.data.data() + offset, len))
    {
      Error(Format("frame %u: cannot write whitelist range 0x%08X-0x%08X", command.frame, r.start, r.end));
      return false;
    }
    offset += len;
  }
  if (s.stack_len != 0 && !m_memory.Write(s.sp, s.data.data() + offset, s.stack_len))
  {
    Error(Format("frame %u: cannot write the stack window at 0x%08X", command.frame, s.sp));
    return false;
  }
  const u64 t1 = m_micros();
  if (m_options.invalidate_code)
  {
    for (const Range& r : m_ranges)
      m_memory.InvalidateCode(r.start, r.end - r.start);
  }
  const u64 t2 = m_micros();
  const u64 us = t1 - t0;
  const u64 icache_us = t2 - t1;

  // Time base: moved back to its value in the snapshot (the decrementer and CoreTiming stay as they are).
  std::string tb_text;
  if (m_options.restore_timebase && s.timebase_valid)
  {
    u64 now = 0;
    if (m_memory.ReadTimeBase(&now) && m_memory.WriteTimeBase(s.timebase))
      tb_text = Format(" tb_delta=%lld", static_cast<long long>(now - s.timebase));
    else
      Error(Format("frame %u: cannot set the time base", command.frame));
  }

  ++m_stats.restores;
  m_stats.restore_us_total += us;
  m_stats.restore_us_max = std::max(m_stats.restore_us_max, us);
  m_stats.icache_us_total += icache_us;
  m_stats.icache_us_max = std::max(m_stats.icache_us_max, icache_us);
  if (log_it)
  {
    ++m_restore_logs;
    m_sink.Line(Format("RESTORE %u -> %u bytes=%zu us=%llu icache_us=%llu%s", command.frame, to, m_ranges_bytes + s.stack_len,
                       static_cast<unsigned long long>(us), static_cast<unsigned long long>(icache_us), tb_text.c_str()));
  }
  else if (log_short)
  {
    ++m_restore_logs;
    m_sink.Line(Format("RESTORE %u -> %u us=%llu%s", command.frame, to, static_cast<unsigned long long>(us), tb_text.c_str()));
  }
  return true;
}

// Diagnosis mode: the state just captured for this frame is written straight back and the game is told it was restored (same
// frame, same inputs). Memory is unchanged in effect, but the hook's RESTORED path (register reload from its stack frame) runs.
Reply SyncTest::Noop(const Command& command)
{
  const u32 n = command.frame;
  const Slot& slot = SlotFor(n);
  if (n % static_cast<u32>(m_options.every) != 0 || !Restore(slot, command, n))
    return Echo(command);
  ++m_stats.noop_restores;
  if (m_stats.noop_restores % SUMMARY_EVERY_COMPARED == 0)
    SummaryLine("SUMMARY");
  Reply r;
  r.flags = REPLY_FLAG_RESTORED;
  r.frame = n;
  r.pads = command.pads;
  return r;
}

Reply SyncTest::Normal(const Command& command, const std::array<u32, 2>& fighters)
{
  const u32 n = command.frame;
  Slot& slot = SlotFor(n);
  if (!Capture(slot, command, fighters))
  {
    slot.valid = false;
    Reset("capture failed");
    return Echo(command);
  }
  if (!m_have_history)
  {
    m_have_history = true;
    m_first_n = n;
  }
  m_last_frame = n;

  if (m_options.noop)
    return Noop(command);

  const u32 k = static_cast<u32>(m_options.k);
  if (n >= m_first_n + k && n % static_cast<u32>(m_options.every) == 0 && Restore(SlotFor(n - k), command, n - k))
  {
    const u32 target = n - k;
    ++m_stats.rollbacks;
    m_resim = true;
    m_resim_target = n;
    m_last_frame = target;  // the next command must be target + 1
    Reply r;
    r.flags = REPLY_FLAG_RESTORED;
    r.frame = target;
    r.pads = SlotFor(target).inputs;
    return r;
  }
  return Echo(command);
}

Reply SyncTest::Resim(const Command& command, const std::array<u32, 2>& fighters)
{
  const u32 m = command.frame;
  if (m != m_last_frame + 1 || m > m_resim_target)
  {
    Error(Format("resimulation expected frame %u, the game sent %u (restore not honoured?)", m_last_frame + 1, m));
    Reset("unexpected frame during resimulation");
    return Echo(command);
  }
  Slot& reference = SlotFor(m);
  if (!reference.valid || reference.frame != m)
  {
    Error(Format("frame %u: no reference record to compare", m));
    Reset("missing reference");
    return Echo(command);
  }
  if (!Capture(m_scratch, command, fighters))
  {
    Reset("capture failed during resimulation");
    return Echo(command);
  }

  m_scratch.inputs = reference.inputs;  // the recorded inputs, not the live ones, drive the replay
  Compare(reference, m_scratch, m);
  std::swap(reference, m_scratch);      // the resimulated state becomes the record (and snapshot) of frame m
  ++m_stats.frames_compared;
  m_last_frame = m;
  if (m == m_resim_target)
    m_resim = false;  // frame n's real run: carry on as a normal frame, but no new rollback on this frame
  if (m_stats.frames_compared % SUMMARY_EVERY_COMPARED == 0)
    SummaryLine("SUMMARY");

  Reply r;
  r.flags = 0;
  r.frame = m;
  r.pads = reference.inputs;
  return r;
}

void SyncTest::Compare(const Slot& reference, const Slot& live, u32 frame)
{
  struct FieldDiff
  {
    size_t player;
    const char* name;
    u32 ref;
    u32 resim;
    u32 offset;
  };
  std::vector<FieldDiff> field_diffs;
  for (size_t p = 0; p < 2; ++p)
  {
    for (size_t f = 0; f < FIELD_COUNT; ++f)
    {
      const u8* a = reference.fields[p].data() + 4 * f;
      const u8* b = live.fields[p].data() + 4 * f;
      if (std::memcmp(a, b, 4) != 0)
        field_diffs.push_back({p + 1, FIELDS[f].name, ReadBE32(a), ReadBE32(b), FIELDS[f].offset});
    }
  }
  std::vector<size_t> range_diffs;
  for (size_t i = 0; i < m_ranges.size(); ++i)
  {
    if (reference.crc[i] != live.crc[i])
      range_diffs.push_back(i);
  }
  const bool addr_diff = reference.fighter_addr != live.fighter_addr;
  if (field_diffs.empty() && range_diffs.empty() && !addr_diff)
    return;

  ++m_stats.mismatching_frames;
  // Full detail for the first MAX_DETAILED_MISMATCH_FRAMES mismatching frames, and for any later frame in which a field name
  // differs for the first time (so a late-appearing field is still named, with its address).
  const auto is_new = [this](const char* name) {
    return std::find(m_fields_annotated.begin(), m_fields_annotated.end(), name) == m_fields_annotated.end();
  };
  bool has_new_field = false;
  for (const FieldDiff& d : field_diffs)
    has_new_field = has_new_field || is_new(d.name);
  const bool detailed = m_logged_mismatch_frames < MAX_DETAILED_MISMATCH_FRAMES || has_new_field;
  if (!m_osd_mismatch_shown)
  {
    m_osd_mismatch_shown = true;
    m_sink.Osd(Format("Rev3 SyncTest: MISMATCH at frame %u (K=%d), see the log", frame, m_options.k));
  }
  if (!detailed)
  {
    if (!m_suppressed_noted)
    {
      m_suppressed_noted = true;
      m_sink.Line(Format("NOTE detail is logged for the first %u mismatching frames only; counts continue in the summaries",
                         MAX_DETAILED_MISMATCH_FRAMES));
    }
    return;
  }
  ++m_logged_mismatch_frames;

  m_sink.Line(Format("MISMATCH frame=%u K=%d rollback=%u fields=%zu ranges=%zu", frame, m_options.k, m_stats.rollbacks,
                     field_diffs.size(), range_diffs.size()));
  if (addr_diff)
  {
    m_sink.Line(Format("  fighter_addr p1 ref=0x%08X resim=0x%08X, p2 ref=0x%08X resim=0x%08X", reference.fighter_addr[0],
                       live.fighter_addr[0], reference.fighter_addr[1], live.fighter_addr[1]));
  }
  for (const FieldDiff& d : field_diffs)
  {
    // The first time a field name differs: also its guest address and the snapshot range holding it (the question is whether the
    // field is rewound at all; "NOT IN SNAPSHOT" = the whitelist or an exclusion leaves it out).
    std::string where;
    if (is_new(d.name))
    {
      m_fields_annotated.push_back(d.name);
      const u32 addr = reference.fighter_addr[d.player - 1] + d.offset;
      const std::string range = SnapshotRangeOf(reference, addr);
      where = Format(" addr=0x%08X ", addr) + (range.empty() ? std::string("NOT IN SNAPSHOT") : "in " + range);
    }
    m_sink.Line(Format("  p%zu %s ref=0x%08X resim=0x%08X", d.player, d.name, d.ref, d.resim) + where);
  }
  for (size_t j = 0; j < range_diffs.size(); ++j)
  {
    if (j == MAX_RANGES_LISTED)
    {
      m_sink.Line(Format("  ... and %zu more differing ranges", range_diffs.size() - j));
      break;
    }
    const size_t i = range_diffs[j];
    const Range& r = m_ranges[i];
    const size_t len = r.end - r.start;
    const u8* a = reference.data.data() + m_range_offsets[i];
    const u8* b = live.data.data() + m_range_offsets[i];
    size_t differing = 0;
    std::string words;
    for (size_t off = 0; off + 4 <= len; off += 4)
    {
      if (std::memcmp(a + off, b + off, 4) == 0)
        continue;
      if (differing < MAX_WORDS_LISTED)
        words += Format(" 0x%08X:%08X>%08X", r.start + static_cast<u32>(off), ReadBE32(a + off), ReadBE32(b + off));
      ++differing;
    }
    m_sink.Line(Format("  range 0x%08X-0x%08X crc ref=0x%08X resim=0x%08X words=%zu%s%s", r.start, r.end, reference.crc[i],
                       live.crc[i], differing, differing != 0 ? " first(addr:ref>resim)=" : "", words.c_str()));
  }
}

void SyncTest::SummaryLine(const char* tag)
{
  const double n = m_stats.restores != 0 ? static_cast<double>(m_stats.restores) : 1.0;
  m_sink.Line(Format("%s frame=%u rollbacks=%u frames_compared=%u mismatching_frames=%u restore_us_mean=%.1f restore_us_max=%llu "
                     "icache_us_mean=%.1f icache_us_max=%llu sp_mismatches=%u noop_restores=%u",
                     tag, m_last_frame, m_stats.rollbacks, m_stats.frames_compared, m_stats.mismatching_frames,
                     m_stats.restore_us_total / n, static_cast<unsigned long long>(m_stats.restore_us_max),
                     m_stats.icache_us_total / n, static_cast<unsigned long long>(m_stats.icache_us_max),
                     m_stats.sp_mismatches, m_stats.noop_restores));
}

void SyncTest::Finish()
{
  if (m_finished)
    return;
  m_finished = true;
  SummaryLine("SUMMARY final");
}

Reply SyncTest::OnCommand(const Command& command)
{
  std::array<u32, 2> fighters{};
  const bool found = FindFighters(&fighters);
  if (found)
  {
    if (m_streak < 0xFFFFFFFFu)
      ++m_streak;
  }
  else
  {
    m_streak = 0;
  }

  // P1's Z, edge triggered. A press is remembered until it can be acted on (not in the middle of a resimulation).
  if (m_options.arm_by_pad)
  {
    const u16 buttons = static_cast<u16>((command.pads[0][0] << 8) | command.pads[0][1]);
    const bool z = (buttons & PAD_BUTTON_Z) != 0;
    if (z && !m_prev_z)
      m_z_pending = true;
    m_prev_z = z;
  }

  if (found)
    m_fighters = fighters;
  if (!found)
  {
    m_z_pending = false;
    if (m_armed)
    {
      m_armed = false;
      Reset("fighters lost");
    }
    return Echo(command);
  }

  if (!m_armed)
  {
    const bool arm = m_options.arm_by_pad ? m_z_pending : m_streak >= std::max<u32>(m_options.arm_commands, 1);
    if (!arm)
      return Echo(command);
    m_z_pending = false;
    m_armed = true;
    m_armed_calls = 0;
    m_last_frame = command.frame;
    ClearHistory();
    if (m_options.arm_by_pad)
      m_sink.Line(Format("NOTE armed by Z at frame %u", command.frame));
    else
      m_sink.Line(Format("NOTE armed at frame %u (%u hook calls with both fighters)", command.frame, m_streak));
    RefreshExclusions(true);
    m_sink.Line(Format("NOTE fighters p1=0x%08X p2=0x%08X", fighters[0], fighters[1]));
    LogFieldCoverage(fighters, nullptr);
    if (m_dsp_state)
      m_sink.Line("NOTE dsp at arm: " + m_dsp_state());
  }
  else if (m_options.arm_by_pad && m_z_pending && !m_resim)
  {
    m_z_pending = false;
    m_armed = false;
    ClearHistory();
    m_sink.Line(Format("NOTE disarmed by Z at frame %u", command.frame));
    return Echo(command);
  }

  ++m_armed_calls;
  if (m_armed_calls % (m_armed_calls <= HEARTBEAT_EARLY_CALLS ? HEARTBEAT_EARLY_EVERY : HEARTBEAT_EVERY) == 0)
  {
    m_sink.Line(Format("HB frame=%u calls=%u rollbacks=%u noop_restores=%u", command.frame, m_armed_calls, m_stats.rollbacks,
                       m_stats.noop_restores) +
                (m_dsp_state ? " " + m_dsp_state() : std::string()));
  }
  // Due every hardware_check_every calls; done on the next call that is not a resimulation (the layout must not change there).
  if (m_armed_calls % m_options.hardware_check_every == 0)
    m_hardware_due = true;
  if (m_hardware_due && !m_resim)
  {
    m_hardware_due = false;
    RefreshExclusions();
  }

  if (m_resim)
    return Resim(command, fighters);

  if (m_have_history && command.frame != m_last_frame + 1)
  {
    m_last_frame = command.frame;
    Reset("frame number jumped");
  }
  return Normal(command, fighters);
}

// ---- (5) Dolphin glue ---------------------------------------------------------------------------------------------------

namespace
{
class DolphinMemory final : public GuestMemory
{
public:
  explicit DolphinMemory(Core::System& system) : m_system(system) {}

  bool Read(u32 addr, void* dst, size_t n) const override
  {
    auto& memory = m_system.GetMemory();
    if (!Inside(memory, addr, n))
      return false;
    memory.CopyFromEmu(dst, addr, n);
    return true;
  }

  bool Write(u32 addr, const void* src, size_t n) override
  {
    auto& memory = m_system.GetMemory();
    if (!Inside(memory, addr, n))
      return false;
    memory.CopyToEmu(addr, src, n);  // a plain memcpy: no JIT invalidation (InvalidateCode below does that)
    return true;
  }

  // Forced invalidation (the code bytes are restored to what they were, not modified by the game): the JIT drops every block
  // translated from the range, so overlays / RELs in a restored heap are recompiled from the restored bytes.
  void InvalidateCode(u32 addr, size_t n) override { m_system.GetJitInterface().InvalidateICache(addr, static_cast<u32>(n), true); }

  // Time base. What the game's mftb / OSGetTime returns is SystemTimers::GetFakeTimeBase() = CoreTiming fake_TB_start_value +
  // (ticks - fake_TB_start_ticks) / TIMER_RATIO (the JIT inlines exactly this; PowerPCState's TL / TU are only a cache refreshed
  // by each mftb). So the time base is set back by shifting fake_TB_start_value by (value - now), modulo 2^64, and the cached
  // TL / TU are written too (PowerPCManager::WriteFullTimeBaseValue). The decrementer and the CoreTiming event queue are not touched.
  bool ReadTimeBase(u64* out) const override
  {
    *out = m_system.GetSystemTimers().GetFakeTimeBase();
    return true;
  }
  bool WriteTimeBase(u64 value) override
  {
    const u64 now = m_system.GetSystemTimers().GetFakeTimeBase();
    auto& core_timing = m_system.GetCoreTiming();
    core_timing.SetFakeTBStartValue(core_timing.GetFakeTBStartValue() + (value - now));
    m_system.GetPowerPC().WriteFullTimeBaseValue(value);
    return true;
  }

private:
  // The range must lie entirely in the emulated MEM1 or MEM2 (checked here so MemoryManager never raises a panic alert).
  static bool Inside(const Memory::MemoryManager& memory, u32 addr, size_t n)
  {
    u64 end = 0;
    if (addr >= MEM1_BASE && addr < MEM1_BASE + static_cast<u64>(memory.GetRamSizeReal()))
      end = MEM1_BASE + static_cast<u64>(memory.GetRamSizeReal());
    else if (addr >= MEM2_BASE && addr < MEM2_BASE + static_cast<u64>(memory.GetExRamSizeReal()))
      end = MEM2_BASE + static_cast<u64>(memory.GetExRamSizeReal());
    else
      return false;
    return static_cast<u64>(addr) + n <= end;
  }

  Core::System& m_system;
};

// The memory emulated hardware reads or writes by itself, read from Dolphin's hardware state at the time of the query:
//   GX command processor FIFO   System::GetCommandProcessor().GetFifo().CPBase / CPEnd (public, atomics)
//   VI external framebuffers    System::GetVideoInterface().GetXFBAddressTop() / Bottom() (public); size is not exposed, so a
//                               generous 640*576*2 bytes each is used
//   audio DMA source block      DSP registers AUDIO_DMA_START_HI / LO / CONTROL_LEN through the MMIO mapping (0x0C005030..36;
//                               the DSPManager's own state is private), length = NumBlocks * 32
//   DVD DMA destination         DI registers DIMAR / DILENGTH / DICR through the MMIO mapping (0x0C006014..1C, plus 0x0D006014..1C
//                               on a Wii); only while TSTART is set (a transfer in flight)
// Not reachable: Wii IOS /dev/di reads (their output address lives inside IOS HLE, not in DIMAR), ARAM DMA, and GPU-thread
// writes into texture memory (EFB copies); these are reported once in the log.
class DolphinHardware final : public HardwareView
{
public:
  DolphinHardware(Core::System& system, Sink& sink) : m_system(system)
  {
    sink.Line("NOTE hardware: GX FIFO via GetCommandProcessor().GetFifo(), XFB via GetVideoInterface().GetXFBAddressTop/Bottom(), "
              "audio DMA and DVD DMA via MMIO registers 0x0C005030 / 0x0C006014");
    sink.Line("NOTE hardware: not reachable: Wii IOS /dev/di read destinations, ARAM DMA, GPU-thread texture copies");
  }

  void Query(std::vector<Exclusion>* out) override
  {
    // GX command processor FIFO. CPEnd is the last 32-byte block of the FIFO, hence the + 32.
    {
      auto& fifo = m_system.GetCommandProcessor().GetFifo();
      const u32 base = fifo.CPBase.load();
      const u32 end = fifo.CPEnd.load();
      if (base != 0 && end > base)
        Add(out, "gx_fifo", base, static_cast<u64>(end) - base + 32);
    }
    // VI external framebuffers.
    {
      auto& vi = m_system.GetVideoInterface();
      constexpr u64 XFB_BYTES = 640ull * 576 * 2;
      if (const u32 top = vi.GetXFBAddressTop(); top != 0)
        Add(out, "xfb_top", top, XFB_BYTES);
      if (const u32 bottom = vi.GetXFBAddressBottom(); bottom != 0)
        Add(out, "xfb_bottom", bottom, XFB_BYTES);
    }
    MMIO::Mapping* mmio = m_system.GetMemory().GetMMIOMapping();
    if (mmio == nullptr)
      return;
    // Audio DMA (DSP registers).
    {
      const u32 start = (static_cast<u32>(mmio->Read<u16>(m_system, 0x0C005030)) << 16) | mmio->Read<u16>(m_system, 0x0C005032);
      const u32 blocks = mmio->Read<u16>(m_system, 0x0C005036) & 0x7FFF;
      if (start != 0 && blocks != 0)
        Add(out, "audio_dma", start & ~0x1Fu, static_cast<u64>(blocks) * 32);
    }
    // DVD DMA in flight.
    QueryDvd(out, mmio, 0x0C006000, "dvd_dma");
    if (m_system.IsWii())
      QueryDvd(out, mmio, 0x0D006000, "dvd_dma_wii");
  }

private:
  void QueryDvd(std::vector<Exclusion>* out, MMIO::Mapping* mmio, u32 base, const char* what)
  {
    const u32 address = mmio->Read<u32>(m_system, base + 0x14);
    const u32 length = mmio->Read<u32>(m_system, base + 0x18);
    const u32 control = mmio->Read<u32>(m_system, base + 0x1C);
    if ((control & 1) != 0 && address != 0 && length != 0)  // TSTART: a command is running
      Add(out, what, address, length);
  }

  // `physical` is a hardware (physical) address; Rev3 sees it at 0x80000000 | physical (MEM1 0x00.., MEM2 0x10.. -> 0x90..).
  static void Add(std::vector<Exclusion>* out, const char* what, u32 physical, u64 length)
  {
    const u32 start = 0x80000000u | (physical & 0x1FFFFFFFu);
    const u32 region_end = RegionEnd(start);
    if (region_end == 0 || length == 0)
      return;
    const u64 end = std::min<u64>(static_cast<u64>(start) + length, region_end);
    out->push_back({what, start, static_cast<u32>(end)});
  }

  Core::System& m_system;
};

// One-line DSP / audio DMA state, read through the MMIO mapping without side effects (the DSP mailbox "from DSP" registers are not
// read: reading the low half pops a mail, and on LLE the high half steps the DSP):
//   csr         0x0C00500A DSP control register
//   to_dsp_hi   0x0C005000 mailbox to the DSP, high half (bit 15 = a mail the DSP has not taken yet)
//   ai_dma      0x0C005036 bit 15 = audio DMA enabled; src 0x0C005030/32 = source address; blocks = low 15 bits of 0x0C005036 (32-byte
//               blocks per transfer); left = 0x0C00503A blocks remaining. If `left` stops changing between HB lines the audio DMA stopped.
std::string DescribeDsp(Core::System& system)
{
  MMIO::Mapping* mmio = system.GetMemory().GetMMIOMapping();
  if (mmio == nullptr)
    return "dsp=n/a";
  const auto r16 = [&](u32 address) { return static_cast<u32>(mmio->Read<u16>(system, address)); };
  const u32 csr = r16(0x0C00500A);
  const u32 to_dsp_hi = r16(0x0C005000);
  const u32 src = (r16(0x0C005030) << 16) | r16(0x0C005032);
  const u32 control = r16(0x0C005036);
  const u32 left = r16(0x0C00503A);
  return Format("dsp csr=0x%04X to_dsp_hi=0x%04X ai_dma=%s src=0x%08X blocks=%u left=%u", csr, to_dsp_hi,
                (control & 0x8000) != 0 ? "on" : "off", src, control & 0x7FFF, left);
}

class FileSink final : public Sink
{
public:
  explicit FileSink(const std::string& path)
  {
    File::CreateFullPath(path);
    File::OpenFStream(m_file, path, std::ios_base::out | std::ios_base::app);
    const std::time_t now = std::time(nullptr);
    char stamp[64] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    Line(Format("=== Rev3 SyncTest log opened %s UTC ===", stamp));
  }

  void Line(const std::string& text) override
  {
    if (m_file.is_open())
      m_file << text << std::endl;  // std::endl flushes: the log survives a crash of the emulator
    INFO_LOG_FMT(SLIPPI, "Rev3 {}", text);
  }

  void Osd(const std::string& text) override { OSD::AddMessage(text, OSD::Duration::VERY_LONG, OSD::Color::RED); }

private:
  std::ofstream m_file;
};

// What the game sees when SyncTest is off: the module is created on the first command and then only echoes.
struct Module
{
  bool initialized = false;
  std::unique_ptr<DolphinMemory> memory;
  std::unique_ptr<DolphinHardware> hardware;
  std::unique_ptr<FileSink> sink;
  std::unique_ptr<SyncTest> synctest;  // null = off
};

Module& GetModule()
{
  static Module module;
  return module;
}

// Environment helpers. Unset or empty = fallback.
std::string GetEnv(const char* name)
{
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

bool ParseInt(const std::string& text, long* out)
{
  if (text.empty())
    return false;
  const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  char* end = nullptr;
  const long value = std::strtol(text.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
  if (end == nullptr || *end != '\0')
    return false;
  *out = value;
  return true;
}

bool ReadTextFile(const std::string& path, std::string* out)
{
  std::ifstream file;
  File::OpenFStream(file, path, std::ios_base::in | std::ios_base::binary);
  if (!file.is_open())
    return false;
  out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

void Initialize(Module& module, Core::System& system)
{
  module.initialized = true;

  const std::string k_text = GetEnv("REV3_SYNCTEST");
  if (k_text.empty())
    return;
  long k = 0;
  if (!ParseInt(k_text, &k) || k < 0 || k > 8)
  {
    ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: REV3_SYNCTEST={} is not 0..8; SyncTest is off", k_text);
    OSD::AddMessage("Rev3 SyncTest: REV3_SYNCTEST must be 1..8; SyncTest is off", OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }
  if (k == 0)
    return;

  SyncTestOptions options;
  options.k = static_cast<int>(k);
  long value = 0;
  const std::string every = GetEnv("REV3_SYNCTEST_EVERY");
  if (!every.empty())
  {
    if (ParseInt(every, &value) && value >= 1 && value <= 1000000)
      options.every = static_cast<int>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SYNCTEST_EVERY={}", every);
  }
  const std::string window = GetEnv("REV3_STACK_WINDOW");
  if (!window.empty())
  {
    if (ParseInt(window, &value) && value >= 0x200 && value <= 0x100000)
      options.stack_window = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_STACK_WINDOW={} (0x200..0x100000)", window);
  }

  const std::string arm = GetEnv("REV3_SYNCTEST_ARM");
  if (!arm.empty())
  {
    std::string lower = arm;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "pad")
      options.arm_by_pad = true;
    else if (ParseInt(arm, &value) && value >= 0 && value <= 100000000)
      options.arm_commands = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SYNCTEST_ARM={} (a number or pad)", arm);
  }
  options.noop = GetEnv("REV3_SYNCTEST_NOOP") == "1";
  options.invalidate_code = GetEnv("REV3_SYNCTEST_ICACHE") != "0";
  options.restore_timebase = GetEnv("REV3_SYNCTEST_TB") == "1";
  const bool hardware_exclude = GetEnv("REV3_SYNCTEST_HW_EXCLUDE") != "0";

  std::string log_path = GetEnv("REV3_SYNCTEST_LOG");
  if (log_path.empty())
    log_path = File::GetUserPath(D_LOGS_IDX) + "rev3_synctest.log";
  module.sink = std::make_unique<FileSink>(log_path);

  const std::string whitelist_path = GetEnv("REV3_WHITELIST");
  options.whitelist_path = whitelist_path;
  std::string text;
  std::string error;
  std::vector<Range> ranges;
  if (whitelist_path.empty())
    error = "REV3_WHITELIST is not set";
  else if (!ReadTextFile(whitelist_path, &text))
    error = "cannot read " + whitelist_path;
  else
    ParseWhitelist(text, &ranges, &error);
  u64 hook_bytes = 0;
  if (!ranges.empty())
  {
    hook_bytes = SubtractRange(&ranges, HOOK_REGION_START, HOOK_REGION_END);
    if (ranges.empty())
      error = "every range lies inside the hook region 0x80002400-0x80002A00";
  }
  if (ranges.empty())
  {
    const std::string message = "Rev3 SyncTest: whitelist unusable (" + error + "); SyncTest is off";
    module.sink->Line("ERROR " + message);
    ERROR_LOG_FMT(SLIPPI, "{}", message);
    module.sink->Osd(message);
    return;
  }

  const std::string exclude_path = GetEnv("REV3_SYNCTEST_EXCLUDE");
  if (!exclude_path.empty())
  {
    std::string exclude_text;
    if (!ReadTextFile(exclude_path, &exclude_text))
      error = "cannot read REV3_SYNCTEST_EXCLUDE " + exclude_path;
    else
      ApplyExclusions(exclude_text, &ranges, *module.sink, &error);
    if (!error.empty())
    {
      const std::string message = "Rev3 SyncTest: exclude file unusable (" + error + "); SyncTest is off";
      module.sink->Line("ERROR " + message);
      ERROR_LOG_FMT(SLIPPI, "{}", message);
      module.sink->Osd(message);
      return;
    }
  }

  if (hook_bytes != 0)
  {
    module.sink->Line(Format("NOTE removed %llu bytes of the hook region 0x%08X-0x%08X from the whitelist",
                             static_cast<unsigned long long>(hook_bytes), HOOK_REGION_START, HOOK_REGION_END));
  }
  module.memory = std::make_unique<DolphinMemory>(system);
  if (hardware_exclude)
    module.hardware = std::make_unique<DolphinHardware>(system, *module.sink);
  module.synctest = std::make_unique<SyncTest>(options, std::move(ranges), *module.memory, *module.sink, std::function<u64()>(),
                                               module.hardware.get());
  if (options.restore_timebase)
  {
    module.sink->Line("NOTE timebase accessor: read SystemTimers::GetFakeTimeBase(); write CoreTiming::SetFakeTBStartValue(start + (value - now)) "
                      "and PowerPCManager::WriteFullTimeBaseValue(value); decrementer and CoreTiming events untouched");
  }
  Core::System* const sys = &system;
  module.synctest->SetDspState([sys]() { return DescribeDsp(*sys); });
  module.sink->Osd(Format("Rev3 SyncTest on: K=%d, every %d frame(s), log %s", options.k, options.every, log_path.c_str()));
}
}  // namespace

void HandleCommand(Core::System& system, const u8* command, size_t size, std::vector<u8>* read_queue)
{
  Module& module = GetModule();
  if (!module.initialized)
    Initialize(module, system);

  Command parsed;
  Reply reply;
  if (!ParseCommand(command, size, &parsed))
  {
    // Right id and magic but another version: say nothing useful; the game falls back to its own pads on a bad reply.
    ERROR_LOG_FMT(SLIPPI, "Rev3: command with unsupported protocol version {}", command[1]);
    read_queue->assign(REPLY_SIZE, 0);
    return;
  }

  if (module.synctest != nullptr && Core::IsCPUThread())
    reply = module.synctest->OnCommand(parsed);
  else
  {
    reply.flags = 0;
    reply.frame = parsed.frame;
    reply.pads = parsed.pads;
  }

  read_queue->assign(REPLY_SIZE, 0);
  EncodeReply(reply, read_queue->data());
}

void Shutdown()
{
  Module& module = GetModule();
  if (module.synctest != nullptr)
    module.synctest->Finish();
  module.synctest.reset();
  module.hardware.reset();
  module.memory.reset();
  module.sink.reset();
  module.initialized = false;  // the next command reads the environment again and appends to the log
}
}  // namespace Rev3
