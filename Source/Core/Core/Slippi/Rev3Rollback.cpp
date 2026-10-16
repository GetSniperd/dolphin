// Rev3 rollback support: command/reply codec, whitelist parser, the SyncTest state machine and the Dolphin glue.
// See Rev3Rollback.h for what it does, how to enable it (REV3_SYNCTEST ...) and where it fits in. Licensed under GPLv2+.
//
// Layout of this file: (1) wire codec, (2) whitelist parser, (3) the fighter field table, (4) Rev3::SyncTest (pure logic, only
// GuestMemory / Sink / a clock), (4b) determinism kit (fighter checksum, pad record / script, trace; pure logic too),
// (5) Dolphin glue: env vars, log file, memory accessor, HandleCommand / Shutdown.
// To change which fighter fields are compared, edit FIELDS (and FIELD_COUNT in the header) to match rev3-mod rbchecksum.py.

#include "Core/Slippi/Rev3Rollback.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <picojson.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DSP.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/IOS/DI/DI.h"
#include "Core/IOS/IOS.h"
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
constexpr size_t MAX_AUDIO_STATE_BYTES = 4u << 20;  // a larger audio state is not the DSP / AI registers (the GameCube's 16 MB ARAM, say)
constexpr u32 MAX_AUDIO_SIZE_NOTES = 3;           // "audio state size changed" lines (the size varies with the HLE mail queue)
constexpr u32 MAX_RESTORE_LOGS = 10;              // the first restores log RESTORE begin / done lines (bytes, microseconds)
constexpr u32 MAX_SHORT_RESTORE_LOGS = 30;        // restores 11..30 log one short line "RESTORE n -> m us=.." (places a later hang)
constexpr size_t MAX_LEARN_LIST_RANGES = 16;      // ranges named in one "NOTE learned +n pages" line (then ",+N more"; the summary names all of them)
constexpr size_t MAX_LEARN_SUMMARY_RANGES = 2000; // ranges named in "NOTE learned summary" (about 22 bytes each; the list is merged, so far fewer occur)
constexpr u32 MAX_LEARN_LOGS = 10;                // "NOTE learned" lines written in full, then at most one per LEARN_LOG_EVERY armed calls
constexpr u32 LEARN_LOG_EVERY = 300;
constexpr u32 MAX_IO_POSTPONE_LOGS = 5;           // "NOTE rollback postponed" lines (the count continues in SUMMARY)
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

namespace
{
// One {"start","end"} item. `i` only labels error messages.
bool ParseRangeItem(const picojson::value& item, size_t i, Range* out, std::string* error)
{
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
  *out = r;
  return true;
}

// One {"ptr","size","note"} item.
bool ParsePtrItem(const picojson::value& item, size_t i, PtrExclusion* out, std::string* error)
{
  PtrExclusion e;
  const std::string label_ptr = Format("range %zu ptr", i);
  const std::string label_size = Format("range %zu size", i);
  if (!item.is<picojson::object>() || !item.contains("size") ||
      !ParseAddress(item.get("ptr"), &e.ptr, error, label_ptr.c_str()))
  {
    if (error->empty())
      *error = Format("range %zu: expected {\"ptr\":..., \"size\":...}", i);
    return false;
  }
  if (!ParseAddress(item.get("size"), &e.size, error, label_size.c_str()))
    return false;
  if (!PointerOk(e.ptr) || !PointerOk(e.ptr + 3) || e.size == 0 || e.size > 0x4000000)
  {
    *error = Format("range %zu: ptr 0x%08X must be a MEM1 / MEM2 address and size 0x%X in 1..0x4000000", i, e.ptr, e.size);
    return false;
  }
  if (item.contains("note") && item.get("note").is<std::string>())
    e.note = item.get("note").get<std::string>();
  *out = e;
  return true;
}

// {"ranges":[...]} -> the array, or false with *error.
bool ParseRangeArray(const std::string& json_text, picojson::value* root, const picojson::array** list, std::string* error)
{
  const std::string parse_error = picojson::parse(*root, json_text);
  if (!parse_error.empty())
  {
    *error = "JSON error: " + parse_error;
    return false;
  }
  if (!root->is<picojson::object>() || !root->contains("ranges") || !root->get("ranges").is<picojson::array>())
  {
    *error = "expected {\"ranges\": [...]}";
    return false;
  }
  *list = &root->get("ranges").get<picojson::array>();
  return true;
}
}  // namespace

bool ParseWhitelist(const std::string& json_text, std::vector<Range>* ranges, std::string* error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  picojson::value root;
  const picojson::array* list = nullptr;
  if (!ParseRangeArray(json_text, &root, &list, error))
    return false;
  std::vector<Range> result;
  for (size_t i = 0; i < list->size(); ++i)
  {
    Range r;
    if (!ParseRangeItem((*list)[i], i, &r, error))
      return false;
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

namespace
{
// The entries of an exclusion / inclusion list: {"start","end"} ranges into *fixed, {"ptr","size"} entries into *pointers.
bool ParseEntries(const picojson::array& list, std::vector<Range>* fixed, std::vector<PtrExclusion>* pointers, std::string* error)
{
  for (size_t i = 0; i < list.size(); ++i)
  {
    const picojson::value& item = list[i];
    if (item.is<picojson::object>() && item.contains("ptr"))
    {
      PtrExclusion e;
      if (!ParsePtrItem(item, i, &e, error))
        return false;
      pointers->push_back(e);
    }
    else
    {
      Range r;
      if (!ParseRangeItem(item, i, &r, error))
        return false;
      fixed->push_back(r);
    }
  }
  return true;
}
}  // namespace

bool ApplyExclusions(const std::string& json_text, std::vector<Range>* ranges, Sink& sink, std::string* error,
                     std::vector<Range>* parsed, bool require_nonempty, std::vector<PtrExclusion>* ptrs)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  picojson::value root;
  const picojson::array* list = nullptr;
  if (!ParseRangeArray(json_text, &root, &list, error))
    return false;
  std::vector<Range> exclusions;
  std::vector<PtrExclusion> pointer_entries;
  if (!ParseEntries(*list, &exclusions, &pointer_entries, error))
    return false;
  if (exclusions.empty() && pointer_entries.empty())
  {
    *error = "the exclusion list has no ranges";
    return false;
  }
  if (!pointer_entries.empty() && ptrs == nullptr)
  {
    *error = "pointer entries (\"ptr\") are not supported here";
    return false;
  }
  std::vector<Range> result = *ranges;
  std::vector<std::string> lines;
  for (const Range& e : exclusions)
  {
    const u64 removed = SubtractRange(&result, e.start, e.end);
    lines.push_back(Format("NOTE exclude user 0x%08X-0x%08X (%llu bytes were in the whitelist)", e.start, e.end,
                           static_cast<unsigned long long>(removed)));
  }
  for (const PtrExclusion& e : pointer_entries)
  {
    lines.push_back(Format("NOTE exclude ptr entry 0x%08X size=0x%X note=%s (resolved when SyncTest arms)", e.ptr, e.size,
                           e.note.c_str()));
  }
  if (require_nonempty && result.empty())
  {
    *error = "the exclusions remove every range of the whitelist";
    return false;
  }
  for (const std::string& line : lines)
    sink.Line(line);
  *ranges = std::move(result);
  if (parsed != nullptr)
    *parsed = std::move(exclusions);
  if (ptrs != nullptr)
    *ptrs = std::move(pointer_entries);
  return true;
}

bool LoadInclusions(const std::string& json_text, Sink& sink, std::vector<Range>* fixed, std::vector<PtrExclusion>* ptrs,
                    std::string* error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  picojson::value root;
  const picojson::array* list = nullptr;
  if (!ParseRangeArray(json_text, &root, &list, error))
    return false;
  std::vector<Range> ranges;
  std::vector<PtrExclusion> pointer_entries;
  if (!ParseEntries(*list, &ranges, &pointer_entries, error))
    return false;
  if (ranges.empty() && pointer_entries.empty())
  {
    *error = "the inclusion list has no ranges";
    return false;
  }
  for (const Range& r : ranges)
    sink.Line(Format("NOTE include user 0x%08X-0x%08X (%u bytes, forced into the snapshot when it arms)", r.start, r.end, r.end - r.start));
  for (const PtrExclusion& e : pointer_entries)
  {
    sink.Line(Format("NOTE include ptr entry 0x%08X size=0x%X note=%s (resolved when the snapshot arms)", e.ptr, e.size, e.note.c_str()));
  }
  *fixed = std::move(ranges);
  *ptrs = std::move(pointer_entries);
  return true;
}

std::vector<Range> MergeRanges(std::vector<Range> ranges)
{
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
  std::vector<Range> out;
  for (const Range& r : ranges)
  {
    if (r.end <= r.start)
      continue;
    if (!out.empty() && r.start <= out.back().end)
      out.back().end = std::max(out.back().end, r.end);
    else
      out.push_back(r);
  }
  return out;
}

// ---- (2b) learned-page reporting --------------------------------------------------------------------------------------------

std::string FormatRangeList(const std::vector<Range>& ranges, size_t max_listed)
{
  std::string out;
  for (size_t i = 0; i < ranges.size(); ++i)
  {
    if (i == max_listed)
    {
      out += Format("%s+%zu more", i == 0 ? "" : ",", ranges.size() - i);
      break;
    }
    out += Format("%s0x%08X-0x%08X", i == 0 ? "" : ",", ranges[i].start, ranges[i].end);
  }
  return out;
}

u64 RangesBytes(const std::vector<Range>& ranges)
{
  u64 total = 0;
  for (const Range& r : ranges)
    total += r.end > r.start ? r.end - r.start : 0;
  return total;
}

u64 RangesPages(const std::vector<Range>& ranges)
{
  return (RangesBytes(ranges) + PAGE_SIZE - 1) / PAGE_SIZE;
}

std::vector<Range> NewRanges(const std::vector<Range>& after, const std::vector<Range>& before)
{
  std::vector<Range> fresh = MergeRanges(after);
  for (const Range& old_range : before)
    SubtractRange(&fresh, old_range.start, old_range.end);
  return fresh;
}

size_t CountTouching(const std::vector<Range>& fresh, const std::vector<Range>& old)
{
  size_t touching = 0;
  for (const Range& f : fresh)
  {
    for (const Range& o : old)
    {
      if (o.end == f.start || o.start == f.end)
      {
        ++touching;
        break;
      }
    }
  }
  return touching;
}

std::vector<Range> PadRanges(std::vector<Range> ranges, u32 pad_bytes)
{
  for (Range& r : ranges)
  {
    if (r.end <= r.start || pad_bytes == 0)
      continue;
    u32 lo, hi;
    if (InMem1(r.start))
    {
      lo = MEM1_BASE;
      hi = MEM1_END;
    }
    else if (InMem2(r.start))
    {
      lo = MEM2_BASE;
      hi = MEM2_END;
    }
    else
    {
      continue;
    }
    r.start = r.start - lo > pad_bytes ? r.start - pad_bytes : lo;
    r.end = static_cast<u32>(std::min<u64>(static_cast<u64>(r.end) + pad_bytes, hi));
  }
  return MergeRanges(std::move(ranges));
}

// Every range grown to whole 4 KB pages (start rounded down, end rounded up, merged). Used for the snapshot set when write tracking is on:
// a page is then either wholly in the set or wholly out of it, so no watched page needs a partial comparison.
static std::vector<Range> PageAlignRanges(std::vector<Range> ranges)
{
  for (Range& r : ranges)
  {
    if (r.end <= r.start)
      continue;
    r.start &= ~(PAGE_SIZE - 1);
    r.end = static_cast<u32>(std::min<u64>((static_cast<u64>(r.end) + PAGE_SIZE - 1) & ~static_cast<u64>(PAGE_SIZE - 1),
                                           InMem2(r.start) ? MEM2_END : MEM1_END));
  }
  return MergeRanges(std::move(ranges));
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

// ---- (3b) page tracker ----------------------------------------------------------------------------------------------------

namespace
{
constexpr u8 PAGE_VALID = 1;    // the stored hash is a baseline for the next scan
constexpr u8 PAGE_DYNAMIC = 2;  // changed between two scans (sticky)

constexpr u64 Rotl64(u64 x, int r)
{
  return (x << r) | (x >> (64 - r));
}

u64 LoadWord(const u8* p)
{
  u64 v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// 64-bit hash of one 4 KB page, for change detection only (not cryptographic): eight independent lanes, each one
// h = (rotl(h, 31) ^ word) * odd constant, folded at the end. The rotation keeps two flips of the same high bit from cancelling,
// the eight chains give the CPU enough independent multiplies to run at memory speed. The guest bytes are hashed as they lie.
// Measured on a shared 2.1 GHz Xeon VM (noisy): about 20 GB/s on cached data, 5-10 GB/s streaming over the 88 MB of guest RAM,
// i.e. 0.6-1.2 ms per call for 1/16 of it (5.5 MB); a desktop core is 2-3 times quicker. HB / SUMMARY lines report the real cost
// (scan_us); REV3_SYNCTEST_SCAN_SLICES=32 halves it.
u64 HashPage(const u8* p)
{
  constexpr u64 K = 0x9E3779B97F4A7C15ull;
  u64 h0 = 0x243F6A8885A308D3ull, h1 = 0x13198A2E03707344ull, h2 = 0xA4093822299F31D0ull, h3 = 0x082EFA98EC4E6C89ull;
  u64 h4 = 0x452821E638D01377ull, h5 = 0xBE5466CF34E90C6Cull, h6 = 0xC0AC29B7C97C50DDull, h7 = 0x3F84D5B5B5470917ull;
  for (size_t i = 0; i < PAGE_SIZE; i += 64)
  {
    h0 = (Rotl64(h0, 31) ^ LoadWord(p + i)) * K;
    h1 = (Rotl64(h1, 31) ^ LoadWord(p + i + 8)) * K;
    h2 = (Rotl64(h2, 31) ^ LoadWord(p + i + 16)) * K;
    h3 = (Rotl64(h3, 31) ^ LoadWord(p + i + 24)) * K;
    h4 = (Rotl64(h4, 31) ^ LoadWord(p + i + 32)) * K;
    h5 = (Rotl64(h5, 31) ^ LoadWord(p + i + 40)) * K;
    h6 = (Rotl64(h6, 31) ^ LoadWord(p + i + 48)) * K;
    h7 = (Rotl64(h7, 31) ^ LoadWord(p + i + 56)) * K;
  }
  u64 r = h0;
  for (const u64 h : {h1, h2, h3, h4, h5, h6, h7})
    r = (Rotl64(r, 23) ^ h) * 0xC2B2AE3D27D4EB4Full;
  return r ^ (r >> 32);
}

// Page index <-> guest address: pages 0 .. 6143 are MEM1 from 0x80000000, pages 6144 .. 22527 are MEM2 from 0x90000000.
constexpr size_t MEM1_PAGE_COUNT = (MEM1_END - MEM1_BASE) / PAGE_SIZE;
static_assert(MEM1_PAGE_COUNT + (MEM2_END - MEM2_BASE) / PAGE_SIZE == TRACKED_PAGES, "page count");

u32 PageAddress(size_t index)
{
  return index < MEM1_PAGE_COUNT ? MEM1_BASE + static_cast<u32>(index) * PAGE_SIZE :
                                   MEM2_BASE + static_cast<u32>(index - MEM1_PAGE_COUNT) * PAGE_SIZE;
}

// Index of the page holding addr, or TRACKED_PAGES when it is in neither region.
size_t PageIndex(u32 addr)
{
  if (InMem1(addr))
    return (addr - MEM1_BASE) / PAGE_SIZE;
  if (InMem2(addr))
    return MEM1_PAGE_COUNT + (addr - MEM2_BASE) / PAGE_SIZE;
  return TRACKED_PAGES;
}
}  // namespace

PageTracker::PageTracker(GuestMemory& memory, u32 slices)
    : m_memory(memory), m_slices(std::clamp<u32>(slices, 1, 4096)), m_hash(TRACKED_PAGES, 0), m_state(TRACKED_PAGES, 0),
      m_buf(PAGE_SIZE)
{
}

void PageTracker::Reset()
{
  if (!m_has_state)  // called on every hook call outside a fight; clearing 22528 entries each time would be a waste
    return;
  std::fill(m_state.begin(), m_state.end(), static_cast<u8>(0));
  m_slice = 0;
  m_cycles = 0;
  m_dynamic = 0;
  m_has_state = false;
}

u32 PageTracker::Scan()
{
  m_has_state = true;
  const size_t lo = TRACKED_PAGES * m_slice / m_slices;
  const size_t hi = TRACKED_PAGES * (static_cast<size_t>(m_slice) + 1) / m_slices;
  u32 added = 0;
  for (size_t i = lo; i < hi; ++i)
  {
    u8& state = m_state[i];
    if ((state & PAGE_DYNAMIC) != 0)
      continue;  // already known to change: nothing left to learn from it
    const u32 addr = PageAddress(i);
    const u8* page = m_memory.Peek(addr, PAGE_SIZE);
    if (page == nullptr)
    {
      if (!m_memory.Read(addr, m_buf.data(), PAGE_SIZE))
        continue;  // not backed by memory (e.g. MEM2 of a GameCube)
      page = m_buf.data();
    }
    const u64 hash = HashPage(page);
    if ((state & PAGE_VALID) != 0 && m_hash[i] != hash)
    {
      state |= PAGE_DYNAMIC;
      ++m_dynamic;
      ++added;
    }
    m_hash[i] = hash;
    state |= PAGE_VALID;
  }
  if (++m_slice >= m_slices)
  {
    m_slice = 0;
    ++m_cycles;
  }
  return added;
}

void PageTracker::SkipSlice()
{
  m_has_state = true;
  if (++m_slice >= m_slices)
  {
    m_slice = 0;
    ++m_cycles;
  }
}

bool PageTracker::MarkDynamic(u32 addr)
{
  const size_t i = PageIndex(addr);
  if (i >= TRACKED_PAGES || (m_state[i] & PAGE_DYNAMIC) != 0)
    return false;
  m_has_state = true;
  m_state[i] |= PAGE_DYNAMIC;
  ++m_dynamic;
  return true;
}

void PageTracker::InvalidateBaseline(const std::vector<Range>& ranges)
{
  if (!m_has_state)
    return;
  for (const Range& r : ranges)
  {
    if (r.end <= r.start)
      continue;
    const size_t first = PageIndex(r.start);
    const size_t last = PageIndex(r.end - 1);
    if (first >= TRACKED_PAGES || last >= TRACKED_PAGES)
      continue;
    for (size_t i = first; i <= last; ++i)
      m_state[i] = static_cast<u8>(m_state[i] & ~PAGE_VALID);
  }
}

void PageTracker::AppendDynamicRanges(std::vector<Range>* out) const
{
  for (size_t i = 0; i < TRACKED_PAGES; ++i)
  {
    if ((m_state[i] & PAGE_DYNAMIC) == 0)
      continue;
    const u32 start = PageAddress(i);
    if (!out->empty() && out->back().end == start)
      out->back().end = start + PAGE_SIZE;
    else
      out->push_back({start, start + PAGE_SIZE});
  }
}

bool PageTracker::IsDynamic(u32 addr) const
{
  const size_t i = PageIndex(addr);
  return i < TRACKED_PAGES && (m_state[i] & PAGE_DYNAMIC) != 0;
}

// ---- (3c) restore verify ----------------------------------------------------------------------------------------------------
// See Rev3Rollback.h "restore verify". Pure logic over GuestMemory / Sink, driven by SyncTest::Capture (shadows) and SyncTest::Restore (checks).

namespace
{
constexpr size_t VERIFY_COPY_CHUNK = 1u << 20;  // bytes per read while a shadow is copied (bytes)
constexpr u32 VERIFY_REGION_BASE[2] = {MEM1_BASE, MEM2_BASE};
constexpr u32 VERIFY_REGION_END[2] = {MEM1_END, MEM2_END};
constexpr u32 VERIFY_REGION_SIZE[2] = {MEM1_END - MEM1_BASE, MEM2_END - MEM2_BASE};
constexpr u64 VERIFY_REGION_SIZE_TOTAL = static_cast<u64>(MEM1_END - MEM1_BASE) + (MEM2_END - MEM2_BASE);  // bytes of one full shadow (88 MB)
constexpr u64 VERIFY_BLOCKS_MEM1 = (MEM1_END - MEM1_BASE) / VERIFY_BLOCK_BYTES;
constexpr u64 VERIFY_BLOCKS_TOTAL = VERIFY_BLOCKS_MEM1 + (MEM2_END - MEM2_BASE) / VERIFY_BLOCK_BYTES;

u32 Gcd(u32 a, u32 b)
{
  while (b != 0)
  {
    const u32 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// `ranges` sorted and merged: does one hold `addr`?
bool RangesHold(const std::vector<Range>& ranges, u32 addr)
{
  auto it = std::upper_bound(ranges.begin(), ranges.end(), addr, [](u32 a, const Range& r) { return a < r.start; });
  return it != ranges.begin() && addr < (it - 1)->end;
}

// The words of the 32-byte block at `block` (8 big-endian words); bytes outside [lo, hi) are not known and read as 0. `p` points at lo.
void BlockWords(const u8* p, u32 lo, u32 hi, u32 block, std::array<u32, VERIFY_FIRST_WORDS>* out)
{
  out->fill(0);
  for (u32 i = 0; i < VERIFY_FIRST_WORDS; ++i)
  {
    const u32 a = block + 4 * i;
    if (a >= lo && static_cast<u64>(a) + 4 <= hi)
      (*out)[i] = ReadBE32(p + (a - lo));
  }
}
}  // namespace

bool VerifyEveryCanHit(u32 every_p, u32 k, u32 sync_every)
{
  if (every_p == 0 || sync_every == 0)
    return false;
  return k % Gcd(every_p, sync_every) == 0;
}

u32 AutoVerifyEvery(u32 k, u32 sync_every)
{
  for (u32 p = k + 1; p <= k + 1 + std::max<u32>(sync_every, 1); ++p)
  {
    if (VerifyEveryCanHit(p, k, sync_every))
      return p;
  }
  return k + 1;
}

RestoreVerifier::RestoreVerifier(const VerifyOptions& options, GuestMemory& memory, Sink& sink, std::function<u64()> micros)
    : m_options(options), m_memory(memory), m_sink(sink), m_micros(std::move(micros))
{
  if (!m_micros)
    m_micros = SteadyMicros;
  m_options.every = std::max<u32>(m_options.every, 1);  // 0 = "the owner did not pick one": every frame
  m_options.slots = std::clamp<u32>(m_options.slots, 1, 8);
  m_slots.resize(m_options.slots);
}

void RestoreVerifier::Probe()
{
  m_probed = true;
  for (size_t r = 0; r < 2; ++r)
  {
    u8 b = 0;
    m_present[r] = m_memory.Read(VERIFY_REGION_BASE[r], &b, 1) && m_memory.Read(VERIFY_REGION_END[r] - 1, &b, 1);
  }
}

const u8* RestoreVerifier::View(u32 addr, size_t n)
{
  if (const u8* p = m_memory.Peek(addr, n); p != nullptr)
    return p;
  if (m_buf.size() < n)
    m_buf.resize(n);
  return m_memory.Read(addr, m_buf.data(), n) ? m_buf.data() : nullptr;
}

size_t RestoreVerifier::ShadowBytes() const
{
  size_t total = 0;
  for (const Shadow& s : m_slots)
    total += s.data[0].size() + s.data[1].size();
  return total;
}

bool RestoreVerifier::HasShadow(u32 frame) const
{
  const Shadow& s = SlotFor(frame);
  return s.valid && s.frame == frame;
}

void RestoreVerifier::DropShadows()
{
  for (Shadow& s : m_slots)
    s.valid = false;
}

bool RestoreVerifier::TakeShadow(u32 frame)
{
  if (!m_probed)
    Probe();
  Shadow& s = SlotFor(frame);
  s.valid = false;
  const u64 t0 = m_micros();
  for (size_t r = 0; r < 2; ++r)
  {
    if (!m_present[r])
    {
      s.data[r].clear();
      continue;
    }
    s.data[r].resize(VERIFY_REGION_SIZE[r]);
    for (size_t off = 0; off < s.data[r].size(); off += VERIFY_COPY_CHUNK)
    {
      const size_t n = std::min<size_t>(VERIFY_COPY_CHUNK, s.data[r].size() - off);
      const u32 addr = VERIFY_REGION_BASE[r] + static_cast<u32>(off);
      if (const u8* p = m_memory.Peek(addr, n); p != nullptr)
      {
        std::memcpy(s.data[r].data() + off, p, n);
      }
      else if (!m_memory.Read(addr, s.data[r].data() + off, n))
      {
        if (!m_error_logged)
        {
          m_error_logged = true;
          m_sink.Line(Format("ERROR verify: cannot read 0x%08X-0x%08X for the shadow of frame %u; no shadow", addr,
                             addr + static_cast<u32>(n), frame));
        }
        return false;
      }
    }
  }
  s.frame = frame;
  s.valid = true;
  ++m_shadows;
  m_shadow_us += m_micros() - t0;
  return true;
}

VerifyResult RestoreVerifier::Check(const VerifyInput& in)
{
  VerifyResult res;
  Shadow& sh = SlotFor(in.to_frame);
  if (!sh.valid || sh.frame != in.to_frame)
    return res;
  const u64 t0 = m_micros();
  res.checked = true;
  const std::vector<Range> skip = MergeRanges(in.skip);
  const std::vector<Range> set = MergeRanges(in.in_set);
  if (m_seen_blocks.empty())
  {
    m_seen_blocks.assign(static_cast<size_t>((VERIFY_BLOCKS_TOTAL + 63) / 64), 0);
    m_seen_pages.assign(TRACKED_PAGES / 64 + 1, 0);
  }

  u32 last_block = 0xFFFFFFFFu;
  u32 last_page = 0xFFFFFFFFu;
  u32 last_outside_page = 0xFFFFFFFFu;
  u32 last_healed_page = 0xFFFFFFFFu;
  u32 last_region = 0xFFFFFFFFu;
  bool have_first = false;
  for (size_t r = 0; r < 2; ++r)
  {
    const u32 base = VERIFY_REGION_BASE[r];
    const u32 end = VERIFY_REGION_END[r];
    if (sh.data[r].empty())
    {
      res.skipped_bytes += VERIFY_REGION_SIZE[r];  // not there (a GameCube has no MEM2)
      continue;
    }
    // The stretches of this region that are compared: the region minus the skip ranges.
    std::vector<Range> segments;
    u32 cursor = base;
    for (const Range& sr : skip)
    {
      if (sr.end <= base || sr.start >= end)
        continue;
      const u32 lo = std::max(sr.start, base);
      const u32 hi = std::min(sr.end, end);
      if (lo > cursor)
        segments.push_back({cursor, lo});
      res.skipped_bytes += hi - lo;
      cursor = std::max(cursor, hi);
    }
    if (cursor < end)
      segments.push_back({cursor, end});

    for (const Range& seg : segments)
    {
      for (u32 a = seg.start; a < seg.end;)
      {
        // 64 KB chunks first (one memcmp each), 4 KB pages and 32-byte blocks only where a chunk differs.
        const u32 chunk_end = std::min<u32>(seg.end, (a & ~(VERIFY_REGION_BYTES - 1)) + VERIFY_REGION_BYTES);
        const u32 n = chunk_end - a;
        const u8* mem = View(a, n);
        if (mem == nullptr)
        {
          res.skipped_bytes += n;
          a = chunk_end;
          continue;
        }
        res.compared_bytes += n;
        const u8* snap = sh.data[r].data() + (a - base);
        if (std::memcmp(mem, snap, n) != 0)
        {
          for (u32 p = a; p < chunk_end;)
          {
            const u32 page_end = std::min<u32>(chunk_end, (p & ~(PAGE_SIZE - 1)) + PAGE_SIZE);
            if (std::memcmp(mem + (p - a), snap + (p - a), page_end - p) != 0)
            {
              for (u32 b = p & ~(VERIFY_BLOCK_BYTES - 1); b < page_end; b += VERIFY_BLOCK_BYTES)
              {
                const u32 lo = std::max(b, p);
                const u32 hi = std::min(b + VERIFY_BLOCK_BYTES, page_end);
                if (std::memcmp(mem + (lo - a), snap + (lo - a), hi - lo) == 0 || b == last_block)
                  continue;
                last_block = b;
                ++res.wrong_blocks;
                const u32 page = b & ~(PAGE_SIZE - 1);
                const bool in_set = RangesHold(set, b);
                if (in_set)
                  ++res.in_set_blocks;
                else
                  ++res.outside_set_blocks;
                const u32 region = b & ~(VERIFY_REGION_BYTES - 1);
                if (region != last_region)
                {
                  last_region = region;
                  res.regions.push_back({region, 0, 0});
                }
                ++res.regions.back().blocks;
                if (page != last_page)
                {
                  last_page = page;
                  ++res.wrong_pages;
                  ++res.regions.back().pages;
                }
                // The first wrong block, and the first one inside the rewritten memory (the restore's own bug, not a missing page).
                if (!have_first)
                {
                  have_first = true;
                  res.first_addr = b;
                  BlockWords(snap + (lo - a), lo, hi, b, &res.first_snapshot);
                  BlockWords(mem + (lo - a), lo, hi, b, &res.first_memory);
                }
                if (in_set && !res.has_first_in_set)
                {
                  res.has_first_in_set = true;
                  res.first_in_set_addr = b;
                  BlockWords(snap + (lo - a), lo, hi, b, &res.first_in_set_snapshot);
                  BlockWords(mem + (lo - a), lo, hi, b, &res.first_in_set_memory);
                }
                if (!in_set && page != last_outside_page)
                {
                  last_outside_page = page;
                  res.outside_pages.push_back({page, page + PAGE_SIZE});
                }
                // Distinct blocks / pages over the whole run.
                const u64 index = (r == 0 ? 0 : VERIFY_BLOCKS_MEM1) + (b - base) / VERIFY_BLOCK_BYTES;
                u64& word = m_seen_blocks[static_cast<size_t>(index / 64)];
                if ((word & (1ull << (index % 64))) == 0)
                {
                  word |= 1ull << (index % 64);
                  ++m_distinct_blocks;
                }
                const size_t page_index = PageIndex(page);
                if (page_index < TRACKED_PAGES)
                {
                  u64& pw = m_seen_pages[page_index / 64];
                  if ((pw & (1ull << (page_index % 64))) == 0)
                  {
                    pw |= 1ull << (page_index % 64);
                    ++m_distinct_pages;
                  }
                }
                if (m_options.heal)
                {
                  if (m_memory.Write(lo, snap + (lo - a), hi - lo))
                  {
                    ++res.healed_blocks;
                    if (page != last_healed_page)
                    {
                      last_healed_page = page;
                      res.healed_pages.push_back({page, page + PAGE_SIZE});
                    }
                  }
                }
              }
            }
            p = page_end;
          }
        }
        a = chunk_end;
      }
    }
  }
  res.outside_pages = MergeRanges(std::move(res.outside_pages));
  res.healed_pages = MergeRanges(std::move(res.healed_pages));

  ++m_checked;
  if (res.wrong_blocks != 0)
  {
    ++m_wrong;
    m_blocks_total += res.wrong_blocks;
    m_in_set_total += res.in_set_blocks;
    m_outside_total += res.outside_set_blocks;
    if (m_first_wrong_blocks == 0)
    {
      m_first_wrong_from = in.from_frame;
      m_first_wrong_to = in.to_frame;
      m_first_wrong_blocks = res.wrong_blocks;
    }
  }
  res.us = m_micros() - t0;
  m_check_us += res.us;
  return res;
}

// ---- (4) SyncTest -------------------------------------------------------------------------------------------------------

SyncTest::SyncTest(const SyncTestOptions& options, std::vector<Range> ranges, GuestMemory& memory, Sink& sink,
                   std::function<u64()> micros, HardwareView* hardware)
    : m_options(options), m_seed(std::move(ranges)), m_memory(memory), m_sink(sink), m_micros(std::move(micros)),
      m_tracker(memory, options.scan_slices), m_hardware(hardware)
{
  if (!m_micros)
    m_micros = SteadyMicros;
  m_options.k = std::clamp(m_options.k, 1, 8);
  m_options.every = std::max(m_options.every, 1);
  m_options.hardware_check_every = std::max<u32>(m_options.hardware_check_every, 1);
  if (!m_options.adaptive)
    m_options.write_track = 0;  // the tracking feeds the page tracker; the classic whitelist mode has none
  m_options.write_track_rearm_max = std::clamp<u32>(m_options.write_track_rearm_max, 1, 200);
  m_span.resize(FIELD_SPAN_HI - FIELD_SPAN_LO);

  // k + 1 slots plus the scratch slot used while comparing a resimulated frame. RebuildLayout sizes them for the current ranges
  // and runs again whenever the set changes (hardware exclusions, learned pages), so the buffers follow the set.
  m_ring.resize(static_cast<size_t>(m_options.k) + 1);
  m_ranges = ComputeRanges();
  RebuildLayout();

  // Restore verify (REV3_RB_VERIFY): shadows of guest memory and a full comparison after restores. Nothing is allocated until the first shadow.
  if (m_options.verify.restores != 0)
  {
    VerifyOptions v = m_options.verify;
    const u32 sync_every = static_cast<u32>(m_options.every);
    if (v.every == 0)
      v.every = AutoVerifyEvery(static_cast<u32>(m_options.k), sync_every);
    m_verify = std::make_unique<RestoreVerifier>(v, memory, sink, m_micros);
    m_sink.Line(Format("NOTE verify on: restores=%s every=%u slots=%u heal=%d stack_below=0x%X shadow_mb=%.0f per slot (MEM1 + MEM2 copy, taken at "
                       "frames that are multiples of %u; a restore is verified only when it lands on one)",
                       v.restores == VERIFY_ALL ? "all" : Format("%u", v.restores).c_str(), v.every, m_verify->Options().slots, v.heal ? 1 : 0,
                       v.stack_below, static_cast<double>(VERIFY_REGION_SIZE_TOTAL) / (1024.0 * 1024.0), v.every));
    if (!VerifyEveryCanHit(v.every, static_cast<u32>(m_options.k), sync_every))
    {
      m_sink.Line(Format("NOTE verify: shadows at multiples of %u never match a restore target (K=%d, EVERY=%u: targets are frames n - K with n %% EVERY "
                         "== 0); no restore will be verified. Pick another REV3_RB_VERIFY_EVERY",
                         v.every, m_options.k, sync_every));
    }
  }

  m_sink.Line(Format("SYNCTEST start K=%d EVERY=%d window=0x%X whitelist=%s ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu "
                     "arm=%s noop=%d hw_exclude=%d icache=%d tb=%d adaptive=%d slices=%u learn=%u",
                     m_options.k, m_options.every, m_options.stack_window, m_options.whitelist_path.c_str(), m_ranges.size(),
                     m_ranges_bytes, m_snapshot_bytes, m_ring.size(),
                     m_options.arm_by_pad ? "pad" : Format("%u", m_options.arm_commands).c_str(), m_options.noop ? 1 : 0,
                     m_hardware != nullptr ? 1 : 0, m_options.invalidate_code ? 1 : 0, m_options.restore_timebase ? 1 : 0,
                     m_options.adaptive ? 1 : 0, m_tracker.Slices(), m_options.learn_cycles));
  if (m_options.restore_timebase)
    m_sink.Line("NOTE timebase restore on");
}

void SyncTest::SetUserExclusions(std::vector<Range> exclusions, std::vector<PtrExclusion> pointers)
{
  m_user_exclusions = std::move(exclusions);
  m_ptr_exclusions.clear();
  for (PtrExclusion& e : pointers)
    m_ptr_exclusions.push_back({std::move(e), false, 0});
  m_ranges = ComputeRanges();
  RebuildLayout();
}

// The snapshot set. Legacy (non-adaptive): the whitelist in file order minus the cuts. Adaptive: whitelist seed + every dynamic
// page the tracker found + both fighter structs (fighter .. +0x1800, so all 61 FIELDS are always covered) + the hook stack window
// (as of arming), merged into sorted ranges. Always minus the hook region, the user's exclusions and every hardware exclusion seen
// so far (the cuts win over everything that was added).
std::vector<Range> SyncTest::ComputeRanges(const std::vector<Range>& forced) const
{
  std::vector<Range> all = m_seed;
  // Forced inclusions (ef pool, REV3_SNAPSHOT_INCLUDE) join before the cuts, so the cuts still win. The classic set keeps its file order
  // (overlaps saved twice) unless something is forced, then it is merged like the adaptive one.
  all.insert(all.end(), forced.begin(), forced.end());
  // Pages the restore verify (heal) found missing join like forced ranges: before the cuts, in every mode.
  all.insert(all.end(), m_verify_extra.begin(), m_verify_extra.end());
  if (!m_options.adaptive && (!forced.empty() || !m_verify_extra.empty()))
    all = MergeRanges(std::move(all));
  if (m_options.adaptive)
  {
    std::vector<Range> dynamic;
    m_tracker.AppendDynamicRanges(&dynamic);
    if (m_options.learn_pad_pages != 0)  // REV3_SNAPSHOT_PAD: the neighbours of a learned page come along (they are cut like the rest below)
      dynamic = PadRanges(std::move(dynamic), m_options.learn_pad_pages * PAGE_SIZE);
    all.insert(all.end(), dynamic.begin(), dynamic.end());
    for (u32 fighter : m_fighters)
    {
      const u32 region_end = RegionEnd(fighter);
      if (fighter != 0 && region_end != 0)
        all.push_back({fighter, std::min<u32>(fighter + FIGHTER_SET_BYTES, region_end)});
    }
    if (m_set_sp != 0)
    {
      const u32 region_end = RegionEnd(m_set_sp);
      if (region_end != 0 && m_options.stack_window != 0)
        all.push_back({m_set_sp, std::min<u32>(m_set_sp + m_options.stack_window, region_end)});
    }
    all = MergeRanges(std::move(all));
    if (m_options.write_track != 0 && m_watch != nullptr)
      all = PageAlignRanges(std::move(all));  // fighter tails, the stack window and forced ranges grow to whole pages (a few pages in all)
  }
  SubtractRange(&all, HOOK_REGION_START, HOOK_REGION_END);
  for (const Range& r : m_user_exclusions)
    SubtractRange(&all, r.start, r.end);
  for (const Exclusion& e : m_exclusions_logged)
    SubtractRange(&all, e.start, e.end);
  return all;
}

namespace
{
bool SameRanges(const std::vector<Range>& a, const std::vector<Range>& b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].start != b[i].start || a[i].end != b[i].end)
      return false;
  }
  return true;
}
}  // namespace

void SyncTest::SetUserInclusions(std::vector<Range> fixed, std::vector<PtrExclusion> pointers)
{
  m_include_fixed = std::move(fixed);
  m_include_ptrs = std::move(pointers);
}

namespace
{
u64 SumBytes(const std::vector<Range>& ranges)
{
  u64 total = 0;
  for (const Range& r : ranges)
    total += r.end - r.start;
  return total;
}
}  // namespace

// Arm time: the regions that must be in the snapshot whatever the page tracker has seen. (1) The nw4r::ef pool, found through the
// EffectSystem singleton (FindEfPool). (2) The REV3_SNAPSHOT_INCLUDE entries: fixed ranges as given, pointer entries by reading the u32.
// Replaces m_forced (a match may allocate the pool elsewhere than the last one). Logs what was found; `NOTE ef pool` carries the main range
// and its bytes. The set is NOT rebuilt here: the caller computes it right after.
void SyncTest::ResolveInclusions()
{
  std::vector<Range> forced = m_include_fixed;
  m_ef = EfPool();
  m_ef_found = false;
  if (m_options.ef_pool)
  {
    std::string why;
    if (FindEfPool(m_memory, &m_ef, &why))
    {
      m_ef_found = true;
      m_sink.Line(Format("NOTE ef pool 0x%08X-0x%08X bytes=%u", m_ef.main.start, m_ef.main.end, m_ef.main.end - m_ef.main.start));
      m_sink.Line(Format("NOTE ef pool total bytes=%u ranges=%zu mm=0x%08X heap=0x%08X-0x%08X effects=%u emitters=%u particle_managers=%u "
                         "particles=%u",
                         m_ef.bytes, m_ef.ranges.size(), m_ef.mm, m_ef.heap_start, m_ef.heap_end, m_ef.counts[0], m_ef.counts[1],
                         m_ef.counts[2], m_ef.counts[3]));
      if (!m_ef.detail.empty())
        m_sink.Line("NOTE ef pool extras: " + m_ef.detail);
      forced.insert(forced.end(), m_ef.ranges.begin(), m_ef.ranges.end());
    }
    else
    {
      m_sink.Line("NOTE ef pool not found: " + why);
    }
  }
  else
  {
    m_sink.Line("NOTE ef pool off (REV3_EF_POOL=0)");
  }
  for (const PtrExclusion& p : m_include_ptrs)
  {
    u8 word[4];
    u32 value = 0;
    const bool read = m_memory.Read(p.ptr, word, 4);
    if (read)
      value = ReadBE32(word);
    if (!read || value == 0 || !PointerOk(value))
    {
      m_sink.Line(Format("NOTE include ptr 0x%08X skipped: %s (value 0x%08X)", p.ptr,
                         !read ? "cannot read the pointer" : (value == 0 ? "pointer is 0" : "pointer is outside MEM1 / MEM2"), value));
      continue;
    }
    const u32 end = static_cast<u32>(std::min<u64>(static_cast<u64>(value) + p.size, RegionEnd(value)));
    m_sink.Line(Format("NOTE include ptr 0x%08X -> 0x%08X-0x%08X (%u bytes) note=%s", p.ptr, value, end, end - value, p.note.c_str()));
    forced.push_back({value, end});
  }
  m_forced = MergeRanges(std::move(forced));
  if (!m_forced.empty())
  {
    // What the forcing adds: the set with and without it, after the same cuts.
    const u64 without = SumBytes(ComputeRanges({}));
    const u64 with = SumBytes(ComputeRanges(m_forced));
    m_sink.Line(Format("NOTE forced include: ranges=%zu bytes=%llu; the snapshot set grows by %llu bytes (%llu -> %llu, before the hardware cuts)",
                       m_forced.size(), static_cast<unsigned long long>(SumBytes(m_forced)), static_cast<unsigned long long>(with - without),
                       static_cast<unsigned long long>(without), static_cast<unsigned long long>(with)));
  }
}

// Recomputes the offsets and buffer sizes for the current m_ranges. Every slot is invalidated: its data has the old layout.
void SyncTest::RebuildLayout()
{
  ++m_layout_gen;
  m_range_offsets.clear();
  m_ranges_bytes = 0;
  for (const Range& r : m_ranges)
  {
    m_range_offsets.push_back(m_ranges_bytes);
    m_ranges_bytes += r.end - r.start;
  }
  m_snapshot_bytes = m_ranges_bytes + m_options.stack_window;
  m_stats.snapshot_bytes_max = std::max(m_stats.snapshot_bytes_max, m_snapshot_bytes);
  for (Slot& s : m_ring)
  {
    s.valid = false;
    s.data.resize(m_snapshot_bytes);
  }
  m_scratch.valid = false;
  m_scratch.data.resize(m_snapshot_bytes);
  m_verify_dirty = false;  // the layout now has every page of m_verify_extra (RebuildLayout always follows m_ranges = ComputeRanges())
}

// Asks the emulated hardware which memory it uses on its own and cuts it out of the snapshot ranges. The set only grows (double
// buffers alternate; seeing both once is enough), and a change that touches the snapshot clears the history.
// REV3_SYNCTEST_EXCLUDE pointer entries: the u32 at `ptr` is the start of a region the game allocated at run time. A value of 0 or
// outside MEM1 / MEM2 means "not allocated (yet)": skipped, with one NOTE per distinct value. A region that moved adds a new cut; the
// old one stays (like the hardware regions, the set of cuts only grows).
void SyncTest::ResolvePtrExclusions(std::vector<Exclusion>* out)
{
  for (PtrState& p : m_ptr_exclusions)
  {
    u8 word[4];
    u32 value = 0;
    const bool read = m_memory.Read(p.entry.ptr, word, 4);
    if (read)
      value = ReadBE32(word);
    if (!read || value == 0 || !PointerOk(value))
    {
      if (!p.skip_logged || p.skip_value != value)
      {
        p.skip_logged = true;
        p.skip_value = value;
        m_sink.Line(Format("NOTE exclude ptr 0x%08X skipped: %s (value 0x%08X)", p.entry.ptr,
                           !read ? "cannot read the pointer" : (value == 0 ? "pointer is 0" : "pointer is outside MEM1 / MEM2"), value));
      }
      continue;
    }
    p.skip_logged = false;
    const u32 end = static_cast<u32>(std::min<u64>(static_cast<u64>(value) + p.entry.size, RegionEnd(value)));
    out->push_back({Format("ptr 0x%08X ->", p.entry.ptr), value, end});
  }
}

void SyncTest::RefreshExclusions(bool at_arm)
{
  if (m_hardware == nullptr && m_ptr_exclusions.empty())
    return;
  std::vector<Exclusion> found;
  if (m_hardware != nullptr)
    m_hardware->Query(&found);
  ResolvePtrExclusions(&found);
  bool layout_changed = false;
  bool cuts_added = false;
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
    cuts_added = true;
    std::vector<Range> probe = m_ranges;  // how much of the current set the region takes away
    const u64 removed = SubtractRange(&probe, e.start, e.end);
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
  {
    if (cuts_added)
      WatchRefresh();  // the new cuts take pages away from the watch too
    return;
  }
  m_ranges = ComputeRanges();  // the new cuts are part of m_exclusions_logged now
  RebuildLayout();
  ClearHistory();
  WatchRefresh();
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
size_t SyncTest::LogFieldCoverage(const std::array<u32, 2>& fighters, const char* when)
{
  size_t worst = FIELD_COUNT;
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
    worst = std::min(worst, inside);
  }
  return worst;
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
  if (m_verify != nullptr)
    m_verify->DropShadows();  // the slots they belong to are gone
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
  return FindFighterStructs(m_memory, addrs) == 2;
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
    offset += len;
  }
  if (stack_len != 0 && !m_memory.Read(command.sp, slot.data.data() + offset, stack_len))
  {
    Error(Format("frame %u: cannot read the stack window at 0x%08X", command.frame, command.sp));
    return false;
  }
  // Restore verify: a full copy of guest memory at this very moment, for the frames a restore may land on (a failed copy is logged by the
  // verifier and only means this frame is not verified).
  if (m_verify != nullptr && m_verify->WantsShadow(command.frame))
    m_verify->TakeShadow(command.frame);

  // The audio hardware at the same moment as the memory (a failure only switches the audio rewind off, see DisableAudio).
  if (m_audio != nullptr)
    CaptureAudio(slot, command);
  else
    slot.audio.valid = false;

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
  slot.layout_gen = m_layout_gen;
  slot.frame = command.frame;
  slot.sp = command.sp;
  slot.stack_len = stack_len;
  slot.inputs = command.pads;
  return true;
}

void SyncTest::SetAudioHardware(AudioHardware* audio)
{
  m_audio = nullptr;
  if (audio == nullptr)
    return;
  std::vector<u8> state;
  if (!audio->SaveState(&state) || state.empty() || state.size() > MAX_AUDIO_STATE_BYTES)
  {
    Error(Format("audio hardware state is unusable at start (%zu bytes); audio rewind off, memory only", state.size()));
    return;
  }
  m_audio = audio;
  m_audio_bytes = state.size();
  std::string names;
  for (const std::string& name : audio->EventTypeNames())
    names += (names.empty() ? "" : ",") + name;
  m_sink.Line(Format("NOTE audio hw rewind on (dsp=%s, bytes=%zu, events=%s)", audio->DspKind(), state.size(),
                     names.empty() ? "(none)" : names.c_str()));
}

// Audio rewind is a best effort on top of the memory rewind: whatever goes wrong with it is reported once and SyncTest carries on
// with memory only (the snapshots already taken keep their audio part but it is no longer used).
void SyncTest::DisableAudio(const std::string& why)
{
  Error(why);
  m_sink.Line("NOTE audio hw rewind off: " + why + "; memory only from now on");
  m_audio = nullptr;
}

void SyncTest::CaptureAudio(Slot& slot, const Command& command)
{
  Slot::Audio& a = slot.audio;
  a.valid = false;
  if (!m_audio->SaveState(&a.state))
  {
    DisableAudio(Format("frame %u: the audio hardware state could not be saved (measure and write passes disagree)", command.frame));
    return;
  }
  if (a.state.empty() || a.state.size() > MAX_AUDIO_STATE_BYTES)
  {
    DisableAudio(Format("frame %u: the audio hardware state is %zu bytes (expected 1..%zu)", command.frame, a.state.size(),
                        MAX_AUDIO_STATE_BYTES));
    return;
  }
  m_audio->PendingEvents(&a.events);
  a.irq = m_audio->InterruptBits();
  a.valid = true;
  if (a.state.size() != m_audio_bytes)
  {
    // Expected for the HLE DSP (its mail queue is a deque); only the measure / write agreement is an error.
    if (m_audio_size_notes < MAX_AUDIO_SIZE_NOTES)
    {
      ++m_audio_size_notes;
      m_sink.Line(Format("NOTE audio state size %zu -> %zu bytes (frame %u; variable-size parts such as the DSP mail queue)",
                         m_audio_bytes, a.state.size(), command.frame));
    }
    m_audio_bytes = a.state.size();
  }
}

// Order matters: the hardware state first (it rebases the AI sample-counter reference to the current tick), then the CoreTiming events
// of the audio devices (removed and re-scheduled with their saved remaining cycles), then the PI cause bits (which re-evaluates the
// external exception the normal way).
std::string SyncTest::RestoreAudio(const Slot& s, const Command& command)
{
  if (m_audio == nullptr || !s.audio.valid)
    return std::string();
  const u64 t0 = m_micros();
  if (!m_audio->LoadState(s.audio.state))
  {
    DisableAudio(Format("frame %u: the audio hardware state was not consumed exactly on restore", command.frame));
    return std::string();
  }
  m_audio->ReplaceEvents(s.audio.events);
  m_audio->SetInterruptBits(s.audio.irq);
  const u64 us = m_micros() - t0;
  ++m_stats.audio_restores;
  m_stats.audio_us_total += us;
  m_stats.audio_us_max = std::max(m_stats.audio_us_max, us);
  return Format(" audio_bytes=%zu audio_events=%zu", s.audio.state.size(), s.audio.events.size());
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
  if (s.layout_gen != m_layout_gen || s.data.size() != m_snapshot_bytes)
  {
    // Belt and braces: RebuildLayout invalidates every slot, so this cannot happen today. A snapshot taken under another layout holds
    // other ranges (or none of the pages learned since), and writing it back range by range would scramble memory.
    ++m_stats.layout_refused;
    Error(Format("frame %u: the snapshot of frame %u was taken under another snapshot layout (gen %u, now %u); not restoring", command.frame,
                 to, s.layout_gen, m_layout_gen));
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
  // Freeze dump: the hardware and the software flags that wait on it, right before the write-back (the first 10 restores).
  if (log_it && m_dump != nullptr)
    m_sink.Line(m_dump->RestoreHwLine(command.frame, to, false));

  // The ef lists as the game left them, just before they are overwritten (the first options.ef_check_restores restores).
  EfCheck("before", command.frame, to);

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
  // The pages just rewritten no longer match the hashes the tracker took before: forget them (a restore must not look like a
  // change of the game's own memory).
  if (m_options.adaptive)
    m_tracker.InvalidateBaseline(m_ranges);
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

  // The audio hardware goes back after the memory write-back and the JIT invalidation.
  tb_text += RestoreAudio(s, command);
  if (log_it && m_dump != nullptr)
    m_sink.Line(m_dump->RestoreHwLine(command.frame, to, true));

  // The ef lists after the write-back: the same checker, this time on the rewound state (the one the game resumes from).
  EfCheck("after", command.frame, to);
  m_last_restore_call = m_armed_calls;
  VerifyAfterRestore(s, command, to);  // after the timings above: the comparison is not part of the restore cost

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

// The ef list check around a restore (options.ef_check_restores). `when` "before" = the live state about to be overwritten (a bad list here
// was already broken in the first run), "after" = the rewound state. Both go through CheckEfLists. A bad result is logged as
//   EFCHECK bad after restore <from> -> <to>: <list> node=0x.. <why> page=0x.. in_snapshot=0|1 dynamic=0|1
// (in_snapshot: the node's page is inside the current snapshot ranges, dynamic: the page tracker has seen it change); the first 20 are
// logged, the rest only counted (SUMMARY ef_bad). The first 3 good results are logged too (`EFCHECK ok ...`), so a log shows the check ran.
void SyncTest::EfCheck(const char* when, u32 from, u32 to)
{
  const u32 limit = m_options.ef_check_restores;
  const bool after = std::strcmp(when, "after") == 0;
  if (limit == 0 || (limit != EF_CHECK_ALL && m_ef_checks_done >= limit))
    return;
  const u64 t0 = m_micros();
  const EfCheckResult r = CheckEfLists(m_memory);
  const u64 us = m_micros() - t0;
  if (after)
  {
    ++m_ef_checks_done;
    ++m_stats.ef_checks;
  }
  if (!r.ran)
    return;  // the effect system is not set up (`NOTE ef pool not found` at arm says why); nothing to check
  if (r.ok)
  {
    if (after && m_ef_ok_logs < 3)
    {
      ++m_ef_ok_logs;
      m_sink.Line(Format("EFCHECK ok after restore %u -> %u lists=%u nodes=%u effects=%u emitters=%u pms=%u particles=%u us=%llu", from, to,
                         r.lists, r.nodes, r.objects[0], r.objects[1], r.objects[2], r.objects[3], static_cast<unsigned long long>(us)));
    }
    return;
  }
  if (after)
    ++m_stats.ef_bad;
  else
    ++m_stats.ef_bad_before;
  ++m_ef_bad_logs;
  if (m_ef_bad_logs > 20)
  {
    if (m_ef_bad_logs == 21)
      m_sink.Line("EFCHECK further failures are only counted (SUMMARY ef_bad / ef_bad_before)");
    return;
  }
  const u32 page = r.node & ~(PAGE_SIZE - 1);
  const bool in_snapshot = r.node != 0 && !RangeContaining(r.node).empty();
  m_sink.Line(Format("EFCHECK bad %s restore %u -> %u: %s node=0x%08X list=0x%08X %s page=0x%08X in_snapshot=%d dynamic=%d lists=%u nodes=%u",
                     when, from, to, r.list.c_str(), r.node, r.list_addr, r.why.c_str(), page, in_snapshot ? 1 : 0,
                     (r.node != 0 && m_tracker.IsDynamic(r.node)) ? 1 : 0, r.lists, r.nodes));
}

// A restore under a disc read in flight would rewind memory the read is about to fill (or has half filled) from inside IOS HLE,
// where it cannot be cut out of the snapshot: the rollback waits until the DI is idle. Counted in SUMMARY io_postponed.
bool SyncTest::DiscReadPostpones(u32 frame)
{
  if (m_hardware == nullptr || !m_hardware->DiscReadInFlight())
    return false;
  ++m_stats.io_postponed;
  if (m_stats.io_postponed <= MAX_IO_POSTPONE_LOGS)
    m_sink.Line(Format("NOTE rollback postponed: disc read in flight (frame %u, %u so far)", frame, m_stats.io_postponed));
  return true;
}

// Diagnosis mode: the state just captured for this frame is written straight back and the game is told it was restored (same
// frame, same inputs). Memory is unchanged in effect, but the hook's RESTORED path (register reload from its stack frame) runs.
Reply SyncTest::Noop(const Command& command)
{
  const u32 n = command.frame;
  const Slot& slot = SlotFor(n);
  if (n % static_cast<u32>(m_options.every) != 0 || DiscReadPostpones(n) || !Restore(slot, command, n))
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
  const bool due = n >= m_first_n + k && n % static_cast<u32>(m_options.every) == 0;
  if (due && DiscReadPostpones(n))
    return Echo(command);  // the capture above stays valid as a later rollback target
  if (due && Restore(SlotFor(n - k), command, n - k))
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
    // memcmp over the two captured buffers; the CRC32s that the log shows are only computed for the ranges that differ (a CRC of
    // the whole set on every capture cost milliseconds once the set grew to several MB).
    const size_t len = m_ranges[i].end - m_ranges[i].start;
    if (std::memcmp(reference.data.data() + m_range_offsets[i], live.data.data() + m_range_offsets[i], len) != 0)
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
    m_sink.Line(Format("  range 0x%08X-0x%08X crc ref=0x%08X resim=0x%08X words=%zu%s%s", r.start, r.end,
                       Common::ComputeCRC32(a, len), Common::ComputeCRC32(b, len), differing,
                       differing != 0 ? " first(addr:ref>resim)=" : "", words.c_str()));
  }
}

void SyncTest::SummaryLine(const char* tag)
{
  LearnedSummaryLine();  // only when the learned pages changed since the last such line
  const double n = m_stats.restores != 0 ? static_cast<double>(m_stats.restores) : 1.0;
  m_sink.Line(Format("%s frame=%u rollbacks=%u frames_compared=%u mismatching_frames=%u restore_us_mean=%.1f restore_us_max=%llu "
                     "icache_us_mean=%.1f icache_us_max=%llu sp_mismatches=%u noop_restores=%u dynamic_pages=%zu scan_us_mean=%.1f "
                     "scan_us_max=%llu io_postponed=%u audio_restores=%u audio_us_mean=%.1f audio_us_max=%llu ef_checks=%u ef_bad=%u "
                     "ef_bad_before=%u layout_refused=%u late_learn_rollbacks=%u",
                     tag, m_last_frame, m_stats.rollbacks, m_stats.frames_compared, m_stats.mismatching_frames,
                     m_stats.restore_us_total / n, static_cast<unsigned long long>(m_stats.restore_us_max),
                     m_stats.icache_us_total / n, static_cast<unsigned long long>(m_stats.icache_us_max),
                     m_stats.sp_mismatches, m_stats.noop_restores, m_tracker.DynamicPages(),
                     m_stats.scans != 0 ? static_cast<double>(m_stats.scan_us_total) / static_cast<double>(m_stats.scans) : 0.0,
                     static_cast<unsigned long long>(m_stats.scan_us_max), m_stats.io_postponed, m_stats.audio_restores,
                     m_stats.audio_restores != 0 ? static_cast<double>(m_stats.audio_us_total) / m_stats.audio_restores : 0.0,
                     static_cast<unsigned long long>(m_stats.audio_us_max), m_stats.ef_checks, m_stats.ef_bad, m_stats.ef_bad_before,
                     m_stats.layout_refused, m_stats.late_learn_rollbacks) +
              VerifyStatsFields());
  m_sink.Line(SnapshotLine());
}

// Pages that entered the set after the first arm: the part of `after` that `before` lacks goes to the all-time list (merged) and to the
// list of pages not yet named in a log line. Also counts how many of the new ranges touch the old set (see CountTouching).
std::vector<Range> SyncTest::NoteLearned(const std::vector<Range>& before, const std::vector<Range>& after)
{
  std::vector<Range> fresh = NewRanges(after, before);
  if (fresh.empty())
    return fresh;
  m_learned_fresh_ranges += fresh.size();
  m_learned_touching += CountTouching(fresh, before);
  m_learned_all.insert(m_learned_all.end(), fresh.begin(), fresh.end());
  m_learned_all = MergeRanges(std::move(m_learned_all));
  m_learned_unlogged_ranges.insert(m_learned_unlogged_ranges.end(), fresh.begin(), fresh.end());
  m_learned_unlogged_ranges = MergeRanges(std::move(m_learned_unlogged_ranges));
  return fresh;
}

// `NOTE learned summary pages=N ranges=[..]` = every page learned after the first arm, merged (MAX_LEARN_SUMMARY_RANGES named at most);
// pages = bytes / 4 KB rounded up. The line after it says how many of the growths touched pages already in the set.
void SyncTest::LearnedSummaryLine()
{
  const u64 pages = RangesPages(m_learned_all);
  if (pages == m_learned_summary_pages)
    return;
  m_learned_summary_pages = pages;
  m_sink.Line(Format("NOTE learned summary pages=%llu ranges=[%s]", static_cast<unsigned long long>(pages),
                     FormatRangeList(m_learned_all, MAX_LEARN_SUMMARY_RANGES).c_str()));
  m_sink.Line(Format("NOTE learned adjacency growths=%zu ranges_touching_old_set=%zu learn_pad_pages=%u", m_learned_fresh_ranges,
                     m_learned_touching, m_options.learn_pad_pages));
}

void SyncTest::Finish()
{
  if (m_finished)
    return;
  m_finished = true;
  LearnedSummaryLine();
  SummaryLine("SUMMARY final");
  if (m_verify != nullptr)
    m_sink.Line(VerifySummaryText());
}

// Pages that changed outside the set on a normal frame while armed: grow the set. Buffers are rebuilt for the new size and the
// history is cleared (no rollback until K new frames are captured). Pages that are already inside the set, or are cut by an
// exclusion, change nothing and cost nothing.
void SyncTest::LearnWhileArmed(u32 added)
{
  m_stats.pages_learned_armed += added;
  m_learned_unlogged += added;
  std::vector<Range> ranges = ComputeRanges();
  if (SameRanges(ranges, m_ranges))
    return;
  // What is new: the new set minus the old one (named in the log, so a run shows WHICH pages the game touched after the learning phase;
  // tools/rollback/learned_include.py turns these lines into a REV3_SNAPSHOT_INCLUDE file).
  NoteLearned(m_ranges, ranges);
  m_ranges = std::move(ranges);
  RebuildLayout();
  ClearHistory();
  // A page is noticed up to one scan cycle (scan_slices hook calls) after it first changed, and every rollback in that time rewound the
  // other pages but not this one: those rollbacks were unsound for it. Only a restore within one cycle can have been affected.
  const bool late = m_last_restore_call != 0 && m_armed_calls - m_last_restore_call <= m_tracker.Slices();
  if (late)
    ++m_stats.late_learn_rollbacks;
  LogLearnedGrowth("cleared", late);
}

// The `NOTE learned` line of a growth (rate limited: the first 10 growths are logged, then at most one line per 300 armed calls, with the
// pages learned since). history = "cleared" (the slots were dropped) or "kept" (write tracking extended them).
void SyncTest::LogLearnedGrowth(const char* history, bool late)
{
  if (m_learn_logs < MAX_LEARN_LOGS || m_armed_calls - m_learn_log_call >= LEARN_LOG_EVERY)
  {
    ++m_learn_logs;
    m_learn_log_call = m_armed_calls;
    // Every range learned since the previous such line (rate limiting must not hide pages), merged, at most MAX_LEARN_LIST_RANGES named.
    const std::string where = FormatRangeList(m_learned_unlogged_ranges, MAX_LEARN_LIST_RANGES);
    m_sink.Line(Format("NOTE learned +%u pages (now ranges=%zu bytes=%zu) frame %u, history %s; new=[%s]%s", m_learned_unlogged,
                       m_ranges.size(), m_ranges_bytes, m_last_frame, history, where.c_str(),
                       late ? " WARNING a rollback ran within the last scan cycle without these pages" : ""));
    m_sink.Line(Format("NOTE snapshot now ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu", m_ranges.size(), m_ranges_bytes,
                       m_snapshot_bytes, m_ring.size()));
    m_learned_unlogged = 0;
    m_learned_unlogged_ranges.clear();
    if (m_learn_logs == MAX_LEARN_LOGS)
      m_sink.Line(Format("NOTE further learning is logged at most every %u hook calls", LEARN_LOG_EVERY));
  }
}

// ---- host write tracking: the SyncTest side ---------------------------------------------------------------------------------
// See Rev3Rollback.h "host write tracking" and REV3.md. The watch guards every page that is not fully inside the snapshot set or a cut; the
// first write to such a page announces itself, and PollWrites (once per hook call, BEFORE the call's capture) either learns the page (the
// set grows, the snapshot history stays) or arms it again when the write left it unchanged.

namespace
{
constexpr u32 WT_PAGE_MASK = PAGE_SIZE - 1;

// Pages that a range only partly covers at its start or end (they are watched although they hold set bytes).
size_t EdgePageCount(const std::vector<Range>& keep)
{
  std::vector<u32> pages;
  for (const Range& r : keep)
  {
    if ((r.start & WT_PAGE_MASK) != 0)
      pages.push_back(r.start & ~WT_PAGE_MASK);
    if ((r.end & WT_PAGE_MASK) != 0)
      pages.push_back((r.end - 1) & ~WT_PAGE_MASK);
  }
  std::sort(pages.begin(), pages.end());
  return static_cast<size_t>(std::unique(pages.begin(), pages.end()) - pages.begin());
}
}  // namespace

std::vector<Range> SyncTest::KeepRanges() const
{
  std::vector<Range> all = ComputeRanges();
  all.push_back({HOOK_REGION_START, HOOK_REGION_END});
  for (const Range& r : m_user_exclusions)
    all.push_back(r);
  for (const Exclusion& e : m_exclusions_logged)
    all.push_back({e.start, e.end});
  return MergeRanges(std::move(all));
}

void SyncTest::WatchStart(const char* why)
{
  if (m_watch == nullptr || m_options.write_track == 0 || m_watching || m_watch_failed)
    return;
  const u64 t0 = m_micros();
  const std::vector<Range> keep = KeepRanges();
  std::string reason;
  if (!m_watch->SetWatched(keep, &reason) || !m_watch->SelfTest(&reason))
  {
    m_watch->Stop();
    m_watch_failed = true;
    m_sink.Line(Format("NOTE write tracking not possible (%s): learning pages by hash scan from here", reason.c_str()));
    return;
  }
  m_watching = true;
  ++m_stats.wt_arms;
  const WriteWatchStats st = m_watch->Stats();
  m_sink.Line(Format("NOTE write tracking armed (%s)%s: %u pages watched, %zu edge pages (learned on their first write), rearm_max=%u, "
                     "%.1f ms",
                     m_watch->Describe().c_str(), why, st.watched_pages, EdgePageCount(keep), m_options.write_track_rearm_max,
                     static_cast<double>(m_micros() - t0) / 1000.0));
  if (m_options.write_track == 2)
    m_sink.Line("NOTE write tracking verify mode: the hash scan keeps running; a page it finds that no fault announced is a WRITE TRACK MISS");
}

void SyncTest::WatchStop(const char* why)
{
  if (!m_watching)
    return;
  m_watching = false;
  m_watch->Stop();
  m_sink.Line(Format("NOTE write tracking off (%s)", why));
}

void SyncTest::WatchRefresh()
{
  if (!m_watching)
    return;
  std::string reason;
  if (!m_watch->SetWatched(KeepRanges(), &reason))
  {
    m_sink.Line(Format("NOTE write tracking lost (%s): learning pages by hash scan from here", reason.c_str()));
    m_watch->Stop();
    m_watching = false;
    m_watch_failed = true;
  }
}

bool SyncTest::GrowKeepingHistory(const std::vector<Range>& ranges, const std::vector<DirtyPage>& dirty)
{
  // The pieces of the new layout: [new_off, new_off + len) comes from the old slot at old_off, or (old_off == NONE) from `source`.
  constexpr size_t NONE = ~static_cast<size_t>(0);
  struct Piece
  {
    size_t new_off;
    size_t len;
    size_t old_off;
    const u8* source;
  };
  std::vector<Piece> pieces;
  std::vector<std::vector<u8>> live_pages;  // pages read from live memory (clean pages), kept alive for the pointers in `pieces`
  live_pages.reserve(64);
  std::map<u32, const u8*> sources;
  for (const DirtyPage& d : dirty)
    sources[d.addr] = d.preimage;

  size_t new_bytes = 0;
  size_t covered_old = 0;
  size_t j = 0;
  for (const Range& r : ranges)
  {
    u32 a = r.start;
    while (a < r.end)
    {
      while (j < m_ranges.size() && m_ranges[j].end <= a)
        ++j;
      if (j < m_ranges.size() && m_ranges[j].start <= a)
      {
        const u32 stop = std::min(r.end, m_ranges[j].end);
        pieces.push_back({new_bytes + (a - r.start), stop - a, m_range_offsets[j] + (a - m_ranges[j].start), nullptr});
        covered_old += stop - a;
        a = stop;
        continue;
      }
      // Not in the old set: take it page by page from a preimage, or from live memory when the page was never written since it was armed.
      const u32 next_old = j < m_ranges.size() ? m_ranges[j].start : r.end;
      const u32 stop = std::min({r.end, next_old, (a & ~WT_PAGE_MASK) + PAGE_SIZE});
      const u32 page = a & ~WT_PAGE_MASK;
      const u8* page_data = nullptr;
      const auto it = sources.find(page);
      if (it != sources.end())
      {
        page_data = it->second;
      }
      else if (m_watch != nullptr && m_watch->IsClean(page))
      {
        std::vector<u8> bytes(PAGE_SIZE);
        if (!m_memory.Read(page, bytes.data(), PAGE_SIZE))
          return false;
        live_pages.push_back(std::move(bytes));
        page_data = live_pages.back().data();
        sources[page] = page_data;
      }
      if (page_data == nullptr)
        return false;  // written (or unwatched) and no preimage: its contents in the old slots are unknown
      pieces.push_back({new_bytes + (a - r.start), stop - a, NONE, page_data + (a - page)});
      a = stop;
    }
    new_bytes += r.end - r.start;
  }
  if (covered_old != m_ranges_bytes)
    return false;  // the new set does not contain the whole old set: not a pure growth

  const size_t new_snapshot = new_bytes + m_options.stack_window;
  std::vector<std::vector<u8>> fresh(m_ring.size());
  for (size_t i = 0; i < m_ring.size(); ++i)
  {
    const Slot& slot = m_ring[i];
    if (!slot.valid || slot.layout_gen != m_layout_gen)
      continue;
    std::vector<u8>& out = fresh[i];
    out.resize(new_snapshot);
    for (const Piece& p : pieces)
      std::memcpy(out.data() + p.new_off, p.old_off == NONE ? p.source : slot.data.data() + p.old_off, p.len);
    if (slot.stack_len != 0)
      std::memcpy(out.data() + new_bytes, slot.data.data() + m_ranges_bytes, slot.stack_len);
  }

  // Commit: new layout, slots swapped over.
  m_ranges = ranges;
  ++m_layout_gen;
  m_range_offsets.clear();
  m_ranges_bytes = 0;
  for (const Range& r : m_ranges)
  {
    m_range_offsets.push_back(m_ranges_bytes);
    m_ranges_bytes += r.end - r.start;
  }
  m_snapshot_bytes = m_ranges_bytes + m_options.stack_window;
  for (size_t i = 0; i < m_ring.size(); ++i)
  {
    Slot& slot = m_ring[i];
    if (!fresh[i].empty())
    {
      slot.data = std::move(fresh[i]);
      slot.layout_gen = m_layout_gen;
    }
    else
    {
      slot.valid = false;
      slot.data.resize(m_snapshot_bytes);
    }
  }
  m_scratch.valid = false;
  m_scratch.data.resize(m_snapshot_bytes);
  m_verify_dirty = false;
  m_stats.snapshot_bytes_max = std::max(m_stats.snapshot_bytes_max, m_snapshot_bytes);
  return true;
}

void SyncTest::PollWrites()
{
  if (!m_watching)
    return;
  const u64 t0 = m_micros();
  if (!m_watch->Active())
  {
    // The host views were remapped under us (or somebody stopped the watch): whatever was marked is gone, and writes may have gone unseen.
    // Start again; the history cannot be trusted for pages learned from now on, so it is cleared.
    ++m_stats.wt_restarts;
    m_watching = false;
    m_sink.Line("NOTE Dolphin remapped its memory views, write tracking restarted");
    if (m_armed)
    {
      ClearHistory();
      m_sink.Line(Format("NOTE history cleared: write tracking restarted (frame %u)", m_last_frame));
    }
    WatchStart(" (restart)");
    return;
  }
  std::vector<DirtyPage> dirty;
  m_watch->Poll(&dirty);
  if (dirty.empty())
    return;

  if (m_page_buf.size() != PAGE_SIZE)
    m_page_buf.resize(PAGE_SIZE);
  std::vector<u32> rearm;
  std::vector<u32> learn;  // pages that join the set
  enum class Verdict { Changed, Equal, Promoted, Pending };
  std::vector<Verdict> verdicts(dirty.size(), Verdict::Changed);
  for (size_t i = 0; i < dirty.size(); ++i)
  {
    const DirtyPage& d = dirty[i];
    if (m_tracker.IsDynamic(d.addr))
    {
      verdicts[i] = Verdict::Pending;  // learned already, the set could not grow yet (resimulation): the preimage waits for it
      learn.push_back(d.addr);
      continue;
    }
    const u8* now = m_memory.Peek(d.addr, PAGE_SIZE);
    if (now == nullptr)
    {
      if (!m_memory.Read(d.addr, m_page_buf.data(), PAGE_SIZE))
      {
        verdicts[i] = Verdict::Changed;  // cannot compare: treat as changed (the safe side: the page joins the set)
        learn.push_back(d.addr);
        continue;
      }
      now = m_page_buf.data();
    }
    if (std::memcmp(now, d.preimage, PAGE_SIZE) != 0)
    {
      verdicts[i] = Verdict::Changed;
      learn.push_back(d.addr);
    }
    else if (d.equal_rearms >= m_options.write_track_rearm_max)
    {
      verdicts[i] = Verdict::Promoted;
      ++m_stats.wt_promoted_equal;
      learn.push_back(d.addr);
    }
    else
    {
      verdicts[i] = Verdict::Equal;
      rearm.push_back(d.addr);
    }
  }
  // The dirty log (REV3_RB_DIRTY_LOG): where the writes came from. Logged before the verdict is final for re-armed pages.
  if (m_options.dirty_log != 0)
  {
    for (size_t i = 0; i < dirty.size() && m_dirty_logged < m_options.dirty_log; ++i)
    {
      const DirtyPage& d = dirty[i];
      static const char* const names[] = {"changed", "equal", "promoted", "pending"};
      m_sink.Line(Format("DIRTY f%u 0x%08X pc=0x%08X lr=0x%08X %s %s", m_last_frame, d.addr, d.pc, d.lr,
                         d.cpu_thread ? "cpu" : "other", names[static_cast<int>(verdicts[i])]));
      if (++m_dirty_logged == m_options.dirty_log)
        m_sink.Line("NOTE DIRTY log limit reached (REV3_RB_DIRTY_LOG)");
    }
  }
  if (!rearm.empty())
  {
    const std::vector<u32> raced = m_watch->Rearm(rearm);  // a write that raced the comparison: changed after all
    m_stats.wt_equal_rearms += static_cast<u32>(rearm.size() - raced.size());
    for (u32 addr : raced)
      learn.push_back(addr);
  }

  if (!learn.empty())
  {
    u32 fresh_pages = 0;
    for (u32 addr : learn)
      fresh_pages += m_tracker.MarkDynamic(addr) ? 1 : 0;
    m_stats.wt_pages_learned += fresh_pages;
    // Does the set change at all? (A learned page may lie inside a cut, or already inside a padded neighbour.)
    std::vector<Range> ranges = ComputeRanges();
    if (SameRanges(ranges, m_ranges))
    {
      m_watch->Release(learn);
    }
    else if (!m_armed)
    {
      // Before arming there is no history: Arm() computes the set from the tracker's dynamic pages.
      m_watch->Release(learn);
    }
    else
    {
      const bool had_history = m_have_history;
      const std::vector<Range> before = m_ranges;
      const bool kept = had_history && GrowKeepingHistory(ranges, dirty);
      if (!kept && m_resim)
      {
        // Cannot extend the slots and must not drop them in the middle of a resimulation: try again after it (the pages stay dirty and
        // dynamic; their preimages stay valid until released).
        ++m_stats.wt_deferred;
      }
      else
      {
        const std::vector<Range> fresh = NoteLearned(before, ranges);
        const u64 pages = RangesPages(fresh);
        m_stats.pages_learned_armed += static_cast<u32>(pages);
        m_learned_unlogged += static_cast<u32>(pages);
        if (kept)
        {
          ++m_stats.wt_grown_kept;
        }
        else
        {
          m_ranges = std::move(ranges);
          RebuildLayout();
          ClearHistory();
          ++m_stats.wt_grown_cleared;
        }
        m_stats.snapshot_bytes_max = std::max(m_stats.snapshot_bytes_max, m_snapshot_bytes);
        LogLearnedGrowth(kept ? "kept" : "cleared", false);
        WatchRefresh();  // releases the learned pages (and any page that became fully covered)
      }
    }
  }
  const u64 us = m_micros() - t0;
  m_stats.wt_poll_us_total += us;
  m_stats.wt_poll_us_max = std::max(m_stats.wt_poll_us_max, us);
}

std::string SyncTest::SnapshotLine() const
{
  const char* kind = m_watching ? "write_track" : (m_options.adaptive ? "adaptive" : "classic");
  WriteWatchStats ws;
  if (m_watch != nullptr)
    ws = m_watch->Stats();
  const size_t set_pages = static_cast<size_t>(RangesPages(m_ranges));
  const double mb = 1.0 / (1024.0 * 1024.0);
  std::string line = Format("SNAPSHOT kind=%s last=%.1f MB max=%.1f MB pages=%zu of watched=%u faults=%llu arms=%u", kind,
                            static_cast<double>(m_snapshot_bytes) * mb, static_cast<double>(m_stats.snapshot_bytes_max) * mb, set_pages,
                            ws.watched_pages, static_cast<unsigned long long>(ws.faults), m_stats.wt_arms);
  if (m_options.write_track != 0)
  {
    line += Format(" learned=%u equal_rearms=%u promoted_equal=%u grown_kept=%u grown_cleared=%u deferred=%u restarts=%u misses=%u "
                   "stale=%llu protect_calls=%llu protect_ms=%.1f poll_ms=%.1f",
                   m_stats.wt_pages_learned, m_stats.wt_equal_rearms, m_stats.wt_promoted_equal, m_stats.wt_grown_kept,
                   m_stats.wt_grown_cleared, m_stats.wt_deferred, m_stats.wt_restarts, m_stats.wt_misses,
                   static_cast<unsigned long long>(ws.stale_faults), static_cast<unsigned long long>(ws.protect_calls),
                   static_cast<double>(ws.protect_us) / 1000.0, static_cast<double>(m_stats.wt_poll_us_total) / 1000.0);
  }
  if (m_verify != nullptr)
  {
    line += Format(" | restores checked=%u wrong=%u blocks=%llu first_wrong_frame=%u shadow_ms=%.1f", m_verify->Checked(), m_verify->Wrong(),
                   static_cast<unsigned long long>(m_verify->WrongBlocksTotal()), m_verify->FirstWrongFrom(),
                   static_cast<double>(m_verify->ShadowUs()) / 1000.0);
  }
  return line;
}

// ---- restore verify: the SyncTest side ---------------------------------------------------------------------------------------

namespace
{
constexpr u32 VERIFY_OS_CURRENT_THREAD = 0x800000E4;  // OSGetCurrentThread (same global the freeze dump reads)
constexpr u32 VERIFY_TH_STACK_BASE = 0x304;           // OSThread: highest address of the stack
constexpr u32 VERIFY_TH_STACK_END = 0x308;            // OSThread: lowest address of the stack
constexpr u32 VERIFY_MAX_THREAD_STACK = 0x400000;     // a stack longer than this (bytes) is not believed

bool VerifyReadU32(const GuestMemory& memory, u32 addr, u32* out)
{
  u8 b[4];
  if (!memory.Read(addr, b, 4))
    return false;
  *out = ReadBE32(b);
  return true;
}

// The dead part of the hook thread's stack: [thread stack end, sp). Nothing below sp is live when the hook is entered, and what lies there
// is the leftovers of whatever ran below it last, which differs between two visits of the same frame. When the OSThread of the running thread
// (*0x800000E4: stackBase +0x304, stackEnd +0x308) is readable and holds sp, its real stack end is used, else `below` bytes below sp.
Range VerifyDeadStack(const GuestMemory& memory, u32 sp, u32 below)
{
  const u32 region_start = InMem1(sp) ? MEM1_BASE : (InMem2(sp) ? MEM2_BASE : sp);
  u32 lo = sp > region_start + below ? sp - below : region_start;
  u32 thread = 0;
  u32 stack_base = 0;
  u32 stack_end = 0;
  if (VerifyReadU32(memory, VERIFY_OS_CURRENT_THREAD, &thread) && PointerOk(thread) &&
      VerifyReadU32(memory, thread + VERIFY_TH_STACK_BASE, &stack_base) && VerifyReadU32(memory, thread + VERIFY_TH_STACK_END, &stack_end) &&
      PointerOk(stack_end) && stack_end <= sp && sp < stack_base && sp - stack_end <= VERIFY_MAX_THREAD_STACK)
    lo = stack_end;
  return {lo, sp};
}

std::string WordsText(const std::array<u32, VERIFY_FIRST_WORDS>& words)
{
  std::string text;
  for (const u32 w : words)
    text += Format("%s%08X", text.empty() ? "" : " ", w);
  return text;
}
}  // namespace

std::string SyncTest::VerifyStatsFields() const
{
  if (m_verify == nullptr)
    return {};
  return Format(" restores_checked=%u restores_wrong=%u verify_blocks=%llu first_wrong_frame=%u shadow_ms=%.1f", m_verify->Checked(),
                m_verify->Wrong(), static_cast<unsigned long long>(m_verify->WrongBlocksTotal()), m_verify->FirstWrongFrom(),
                static_cast<double>(m_verify->ShadowUs()) / 1000.0);
}

// The end-of-run line, in the style of the cross-project "restores left n distinct blocks wrong in m pages" report.
std::string SyncTest::VerifySummaryText() const
{
  return Format("VERIFY summary restores_checked=%u restores_wrong=%u wrong_blocks=%llu (in_set=%llu outside_set=%llu) distinct_blocks=%llu in "
                "distinct_pages=%llu first_wrong_frame=%u first_wrong_back_to=%u first_wrong_blocks=%u shadows=%u shadow_mb_held=%.0f "
                "shadow_ms=%.1f heal=%d pages_added=%u",
                m_verify->Checked(), m_verify->Wrong(), static_cast<unsigned long long>(m_verify->WrongBlocksTotal()),
                static_cast<unsigned long long>(m_verify->InSetBlocksTotal()), static_cast<unsigned long long>(m_verify->OutsideSetBlocksTotal()),
                static_cast<unsigned long long>(m_verify->DistinctBlocks()), static_cast<unsigned long long>(m_verify->DistinctPages()),
                m_verify->FirstWrongFrom(), m_verify->FirstWrongTo(), m_verify->FirstWrongBlocks(), m_verify->Shadows(),
                static_cast<double>(m_verify->ShadowBytes()) / (1024.0 * 1024.0), static_cast<double>(m_verify->ShadowUs()) / 1000.0,
                m_verify->Options().heal ? 1 : 0, m_stats.verify_pages_added);
}

// After the write-back of frame `to`: compare ALL guest memory with the shadow taken when `to` was captured. Memory the snapshot rewrote
// must equal the shadow (a difference there is a restore bug or a host thread writing behind it); memory it did not rewrite equals the
// shadow only if nothing wrote it in the rolled-back frames (a difference there is a page the set is missing, or memory that nobody can
// rewind). What is never compared: the hook region, the REV3_SYNCTEST_EXCLUDE ranges, the hardware cuts and REV3_RB_VERIFY_IGNORE (all
// deliberately left alone by the restore) and the dead stack below the hook's sp.
void SyncTest::VerifyAfterRestore(const Slot& s, const Command& command, u32 to)
{
  if (m_verify == nullptr || !m_verify->Active() || !m_verify->HasShadow(to))
    return;  // (a shadow taken before the budget ran out is not used after it: REV3_RB_VERIFY=N verifies N restores)
  VerifyInput in;
  in.from_frame = command.frame;
  in.to_frame = to;
  in.skip.push_back({HOOK_REGION_START, HOOK_REGION_END});
  in.skip.insert(in.skip.end(), m_user_exclusions.begin(), m_user_exclusions.end());
  for (const Exclusion& e : m_exclusions_logged)
    in.skip.push_back({e.start, e.end});
  in.skip.insert(in.skip.end(), m_verify_ignore.begin(), m_verify_ignore.end());
  const Range dead = VerifyDeadStack(m_memory, s.sp, m_verify->Options().stack_below);
  if (dead.end > dead.start)
    in.skip.push_back(dead);
  in.in_set = m_ranges;
  if (s.stack_len != 0)
    in.in_set.push_back({s.sp, s.sp + s.stack_len});

  const VerifyResult r = m_verify->Check(in);
  if (!r.checked)
    return;
  const double compared_mb = static_cast<double>(r.compared_bytes) / (1024.0 * 1024.0);
  if (r.wrong_blocks == 0)
  {
    if (m_verify_ok_logs < VERIFY_MAX_OK_LOGS)
    {
      ++m_verify_ok_logs;
      m_sink.Line(Format("VERIFY restore %u -> %u ok wrong_blocks=0 compared_mb=%.1f skipped_kb=%.0f us=%llu", command.frame, to, compared_mb,
                         static_cast<double>(r.skipped_bytes) / 1024.0, static_cast<unsigned long long>(r.us)));
    }
    return;
  }

  if (m_verify->WrongBlocksTotal() == r.wrong_blocks)  // this is the first restore that left memory wrong
  {
    m_sink.Line(Format("VERIFY first wrong restore: at frame %u, back to frame %u, %u blocks of %u bytes", command.frame, to, r.wrong_blocks,
                       VERIFY_BLOCK_BYTES));
    m_sink.Osd(Format("Rev3 restore verify: restore %u -> %u left %u blocks wrong, see the log", command.frame, to, r.wrong_blocks));
  }
  if (m_verify_wrong_logs < VERIFY_MAX_WRONG_LOGS)
  {
    ++m_verify_wrong_logs;
    m_sink.Line(Format("VERIFY restore %u -> %u wrong_blocks=%u pages=%u in_set=%u outside_set=%u first=[0x%08X: snap %s | mem %s] compared_mb=%.1f "
                       "skipped_kb=%.0f us=%llu%s",
                       command.frame, to, r.wrong_blocks, r.wrong_pages, r.in_set_blocks, r.outside_set_blocks, r.first_addr,
                       WordsText(r.first_snapshot).c_str(), WordsText(r.first_memory).c_str(), compared_mb,
                       static_cast<double>(r.skipped_bytes) / 1024.0, static_cast<unsigned long long>(r.us),
                       r.in_set_blocks != 0 ? " WARNING blocks inside the rewritten memory differ (restore bug or a host thread)" : ""));
    if (r.has_first_in_set && r.first_in_set_addr != r.first_addr)
      m_sink.Line(Format("VERIFY restore %u -> %u first_in_set=[0x%08X: snap %s | mem %s]", command.frame, to, r.first_in_set_addr,
                         WordsText(r.first_in_set_snapshot).c_str(), WordsText(r.first_in_set_memory).c_str()));
    std::string regions;
    size_t listed = 0;
    for (const VerifyRegionCount& g : r.regions)
    {
      if (listed++ == VERIFY_MAX_REGIONS_LISTED)
      {
        regions += Format(",+%zu more", r.regions.size() - VERIFY_MAX_REGIONS_LISTED);
        break;
      }
      regions += Format("%s0x%08X:%ub/%up", regions.empty() ? "" : ",", g.base, g.blocks, g.pages);
    }
    m_sink.Line(Format("VERIFY restore %u -> %u regions64k=%zu [%s]", command.frame, to, r.regions.size(), regions.c_str()));
    if (!r.outside_pages.empty())
      m_sink.Line(Format("VERIFY restore %u -> %u missing_pages=%llu [%s]%s", command.frame, to,
                         static_cast<unsigned long long>(RangesPages(r.outside_pages)),
                         FormatRangeList(r.outside_pages, VERIFY_MAX_PAGE_RANGES_LISTED).c_str(),
                         m_verify->Options().heal ? " healed; added to the snapshot set at the next layout" : ""));
    if (m_verify_wrong_logs == VERIFY_MAX_WRONG_LOGS)
      m_sink.Line(Format("NOTE further VERIFY restores with wrong blocks are only counted (SUMMARY restores_wrong=, VERIFY summary); first %u were logged",
                         VERIFY_MAX_WRONG_LOGS));
  }

  if (m_verify->Options().heal)
  {
    // The wrong blocks were overwritten with the shadow: the JIT must drop code translated from them and the page tracker must not read the
    // rewrite as a change of the game's own memory. The pages outside the set join it at the next layout (ApplyVerifyPages).
    if (m_options.invalidate_code)
    {
      for (const Range& g : r.healed_pages)
        m_memory.InvalidateCode(g.start, g.end - g.start);
    }
    if (m_options.adaptive)
      m_tracker.InvalidateBaseline(r.healed_pages);
    if (!r.outside_pages.empty())
    {
      std::vector<Range> extra = m_verify_extra;
      extra.insert(extra.end(), r.outside_pages.begin(), r.outside_pages.end());
      extra = MergeRanges(std::move(extra));
      if (!SameRanges(extra, m_verify_extra))
      {
        m_verify_extra = std::move(extra);
        m_verify_dirty = true;
      }
    }
  }
}

// Heal found pages the snapshot set lacks: install them (a relayout, so the history is cleared like for a learned page) and name them in
// the learned-page format, so tools/rollback/learned_include.py turns them into a REV3_SNAPSHOT_INCLUDE entry.
void SyncTest::ApplyVerifyPages()
{
  m_verify_dirty = false;
  std::vector<Range> ranges = ComputeRanges();
  if (SameRanges(ranges, m_ranges))
    return;
  const std::vector<Range> fresh = NoteLearned(m_ranges, ranges);
  const u64 pages = RangesPages(fresh);
  m_stats.verify_pages_added += static_cast<u32>(pages);
  m_ranges = std::move(ranges);
  RebuildLayout();
  ClearHistory();
  m_sink.Line(Format("NOTE learned +%llu pages (now ranges=%zu bytes=%zu) frame %u, history cleared; new=[%s] (restore verify heal)",
                     static_cast<unsigned long long>(pages), m_ranges.size(), m_ranges_bytes, m_last_frame,
                     FormatRangeList(fresh, MAX_LEARN_LIST_RANGES).c_str()));
  m_sink.Line(Format("NOTE snapshot now ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu", m_ranges.size(), m_ranges_bytes, m_snapshot_bytes,
                     m_ring.size()));
}

// Builds the snapshot set and arms, unless the fighters' fields would not all be inside it (adaptive mode: then it logs an ERROR
// and stays unarmed until the fighters are lost and found again).
bool SyncTest::Arm(const Command& command, const std::array<u32, 2>& fighters)
{
  m_set_sp = RegionEnd(command.sp) != 0 ? command.sp : 0;
  m_armed_calls = 0;
  m_last_restore_call = 0;
  m_last_frame = command.frame;
  ClearHistory();
  ResolveInclusions();  // the ef pool and REV3_SNAPSHOT_INCLUDE: in the set whatever the page tracker has seen
  m_ranges = ComputeRanges();
  RebuildLayout();
  RefreshExclusions(true);  // may cut more and rebuild again
  WatchRefresh();           // the set now holds the forced ranges and the stack window, and the cuts are known: release what is covered

  if (m_options.adaptive)
  {
    size_t worst = FIELD_COUNT;
    for (size_t p = 0; p < 2; ++p)
    {
      size_t inside = 0;
      for (const FieldDef& f : FIELDS)
        inside += !RangeContaining(fighters[p] + f.offset).empty();
      worst = std::min(worst, inside);
    }
    if (worst != FIELD_COUNT)
    {
      m_sink.Line(Format("NOTE fighters p1=0x%08X p2=0x%08X", fighters[0], fighters[1]));
      LogFieldCoverage(fighters, nullptr);
      Error(Format("frame %u: not all fighter fields are inside the snapshot set (an exclusion cuts a fighter struct); not arming",
                   command.frame));
      m_arm_blocked = true;
      ClearHistory();
      return false;
    }
  }

  m_armed = true;
  m_arm_wanted = false;
  if (m_options.arm_by_pad)
    m_sink.Line(Format("NOTE armed by Z at frame %u", command.frame));
  else
    m_sink.Line(Format("NOTE armed at frame %u (%u hook calls with both fighters)", command.frame, m_streak));
  if (m_options.adaptive)
  {
    m_sink.Line(Format("NOTE adaptive set at arm: dynamic_pages=%zu (%zu bytes) scan_cycles=%u slices=%u ranges=%zu bytes=%zu "
                       "snapshot_bytes=%zu",
                       m_tracker.DynamicPages(), m_tracker.DynamicPages() * PAGE_SIZE, m_tracker.Cycles(), m_tracker.Slices(),
                       m_ranges.size(), m_ranges_bytes, m_snapshot_bytes));
  }
  m_sink.Line(Format("NOTE snapshot now ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu", m_ranges.size(), m_ranges_bytes,
                     m_snapshot_bytes, m_ring.size()));
  m_sink.Line(Format("NOTE fighters p1=0x%08X p2=0x%08X", fighters[0], fighters[1]));
  LogFieldCoverage(fighters, nullptr);
  if (m_dsp_state)
    m_sink.Line("NOTE dsp at arm: " + m_dsp_state());
  if (m_audio != nullptr)
  {
    std::vector<u8> state;
    std::vector<AudioEvent> events;
    if (m_audio->SaveState(&state))
      m_audio->PendingEvents(&events);
    std::string list;
    for (const AudioEvent& e : events)
      list += Format("%s%s+%lld", list.empty() ? "" : ",", e.type.c_str(), static_cast<long long>(e.cycles));
    m_sink.Line(Format("NOTE audio hw at arm: bytes=%zu irq=0x%02X events=%zu [%s]", state.size(), m_audio->InterruptBits(),
                       events.size(), list.c_str()));
  }
  if (m_dump != nullptr)
    m_dump->Dump("arm", false, true, Format("frame=%u", command.frame));
  return true;
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
  m_last_sp = command.sp;

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
    m_arm_wanted = false;
    m_arm_blocked = false;
    m_defer_logged = false;
    m_set_sp = 0;
    WatchStop("not gameplay");  // loading rewrites memory without our knowing it: nothing learned from faults carries over
    if (m_options.adaptive)
    {
      // A new match starts from scratch: loading rewrites most of memory, so nothing learned before is worth keeping.
      m_tracker.Reset();
      m_learn_done = false;
    }
    if (m_armed)
    {
      m_armed = false;
      Reset("fighters lost");
    }
    return Echo(command);
  }

  // Host write tracking: starts as soon as both fighters are found (pages are watched from then on), and every hook call looks at the pages
  // written since the previous one, before anything is captured. Also during a resimulation: a page must join the set before the next
  // capture, and the slots are extended with the history kept.
  if (m_options.write_track != 0 && m_watch != nullptr && !m_watching && !m_watch_failed && !m_resim)
    WatchStart("");
  PollWrites();

  // Page tracker: one slice per hook call, never during a resimulation (those frames replay the game after a restore; the first
  // run's frames are the reference). Starts as soon as both fighters are found, long before arming. With write tracking on (mode 1) the
  // slice only advances the learning clock; in mode 2 the scan runs as well and every page it finds that no fault announced is a miss.
  if (m_options.adaptive && !m_resim)
  {
    const bool by_faults = m_watching && m_options.write_track == 1;
    const bool check_misses = m_watching && m_options.write_track == 2;
    const u64 t0 = m_micros();
    u32 added = 0;
    std::vector<Range> dynamic_before;
    std::vector<Range> keep_before;
    if (check_misses)
    {
      m_tracker.AppendDynamicRanges(&dynamic_before);
      keep_before = KeepRanges();
    }
    if (by_faults)
    {
      m_tracker.SkipSlice();
    }
    else
    {
      added = m_tracker.Scan();
      const u64 us = m_micros() - t0;
      ++m_stats.scans;
      m_stats.scan_us_total += us;
      m_stats.scan_us_max = std::max(m_stats.scan_us_max, us);
      m_scan_iv_total += us;
      m_scan_iv_max = std::max(m_scan_iv_max, us);
      ++m_scan_iv_n;
    }
    if (added != 0 && check_misses)
    {
      // Pages the scan found changing that the watch was guarding (not inside the set or a cut) and that no fault announced.
      std::vector<Range> now;
      m_tracker.AppendDynamicRanges(&now);
      std::vector<Range> missed = NewRanges(now, dynamic_before);
      for (const Range& k : keep_before)
        SubtractRange(&missed, k.start, k.end);
      const u64 pages = RangesPages(missed);
      if (pages != 0)
      {
        m_stats.wt_misses += static_cast<u32>(pages);
        if (m_stats.wt_misses <= 30)
          m_sink.Line(Format("WRITE TRACK MISS frame %u: %llu page(s) changed without a write fault (a kernel-side write, or a view that is not "
                             "protected): [%s]", command.frame, static_cast<unsigned long long>(pages),
                             FormatRangeList(missed, 8).c_str()));
      }
    }
    if (!m_learn_done && m_tracker.Cycles() >= m_options.learn_cycles)
    {
      m_learn_done = true;
      m_sink.Line(Format("NOTE learning complete: %u scan cycles (slices=%u) dynamic_pages=%zu (%zu bytes) at frame %u", m_tracker.Cycles(),
                         m_tracker.Slices(), m_tracker.DynamicPages(), m_tracker.DynamicPages() * PAGE_SIZE, command.frame));
    }
    if (added != 0 && m_armed)
      LearnWhileArmed(added);
    if (added != 0)
      WatchRefresh();  // pages the scan found are in the set (or a cut) now: no need to guard them any longer
  }

  if (!m_armed)
  {
    bool wanted;
    if (m_options.arm_by_pad)
    {
      if (m_z_pending)
      {
        m_z_pending = false;
        m_arm_wanted = !m_arm_wanted;
        m_defer_logged = false;
        if (!m_arm_wanted)
          m_sink.Line(Format("NOTE arm cancelled by Z at frame %u", command.frame));
      }
      wanted = m_arm_wanted;
    }
    else
    {
      wanted = m_streak >= std::max<u32>(m_options.arm_commands, 1);
    }
    if (!wanted || m_arm_blocked)
      return Echo(command);
    if (!LearningDone())
    {
      if (!m_defer_logged)
      {
        m_defer_logged = true;
        m_sink.Line(Format("NOTE arm deferred: learning (%u/%u cycles)", m_tracker.Cycles(), m_options.learn_cycles));
      }
      return Echo(command);
    }
    if (!Arm(command, fighters))
      return Echo(command);
  }
  else if (m_options.arm_by_pad && m_z_pending && !m_resim)
  {
    m_z_pending = false;
    m_armed = false;
    m_arm_wanted = false;
    ClearHistory();
    m_sink.Line(Format("NOTE disarmed by Z at frame %u", command.frame));
    return Echo(command);
  }

  ++m_armed_calls;
  if (m_armed_calls % (m_armed_calls <= HEARTBEAT_EARLY_CALLS ? HEARTBEAT_EARLY_EVERY : HEARTBEAT_EVERY) == 0)
  {
    std::string line = Format("HB frame=%u calls=%u rollbacks=%u noop_restores=%u", command.frame, m_armed_calls, m_stats.rollbacks,
                              m_stats.noop_restores);
    if (m_options.adaptive)
    {
      line += Format(" dyn=%zu scan_us=%.1f scan_us_max=%llu", m_tracker.DynamicPages(),
                     m_scan_iv_n != 0 ? static_cast<double>(m_scan_iv_total) / m_scan_iv_n : 0.0,
                     static_cast<unsigned long long>(m_scan_iv_max));
      m_scan_iv_total = 0;
      m_scan_iv_n = 0;
      m_scan_iv_max = 0;
    }
    m_sink.Line(line + (m_dsp_state ? " " + m_dsp_state() : std::string()));
  }
  // Due every hardware_check_every calls; done on the next call that is not a resimulation (the layout must not change there).
  if (m_armed_calls % m_options.hardware_check_every == 0)
    m_hardware_due = true;
  if (m_hardware_due && !m_resim)
  {
    m_hardware_due = false;
    RefreshExclusions();
  }
  if (m_verify_dirty && !m_resim)
    ApplyVerifyPages();  // pages the restore verify (heal) found missing, installed once the resimulation that needed them is over

  if (m_resim)
    return Resim(command, fighters);

  if (m_have_history && command.frame != m_last_frame + 1)
  {
    m_last_frame = command.frame;
    Reset("frame number jumped");
  }
  return Normal(command, fighters);
}

// ---- (4b) determinism kit (milestone M1) -------------------------------------------------------------------------------------

size_t FindFighterStructs(const GuestMemory& memory, std::array<u32, 2>* addrs)
{
  size_t found = 0;
  for (u32 team = 0; team < 2; ++team)
  {
    (*addrs)[team] = 0;
    u8 word[4];
    // rbchecksum.find_fighters: TEAMS[team] -> team struct, +0x0C -> list node, word 0 of the node -> fighter; every hop must be a
    // pointer into MEM1 / MEM2 (rbchecksum._ok).
    if (!memory.Read(TEAMS_ADDR + 4 * team, word, 4))
      continue;
    const u32 team_ptr = ReadBE32(word);
    if (!PointerOk(team_ptr) || !memory.Read(team_ptr + 0x0C, word, 4))
      continue;
    const u32 node = ReadBE32(word);
    if (!PointerOk(node) || !memory.Read(node, word, 4))
      continue;
    const u32 fighter = ReadBE32(word);
    if (!PointerOk(fighter))
      continue;
    (*addrs)[team] = fighter;
    ++found;
  }
  return found;
}

size_t FighterChecksum(const GuestMemory& memory, u32 frame, u32* checksum)
{
  std::array<u32, 2> fighters{};
  FindFighterStructs(memory, &fighters);

  // The byte stream of rbchecksum.py's docstring: u32 BE frame, then per player (ascending) u32 BE player id and the fields' raw
  // big-endian bytes in FIELDS order (floats are just 4 raw bytes; fighter addresses are not hashed).
  std::vector<u8> stream;
  stream.reserve(4 + 2 * (4 + FIELD_BYTES));
  const auto append_be32 = [&stream](u32 v) {
    stream.push_back(static_cast<u8>(v >> 24));
    stream.push_back(static_cast<u8>(v >> 16));
    stream.push_back(static_cast<u8>(v >> 8));
    stream.push_back(static_cast<u8>(v));
  };
  append_be32(frame);
  std::vector<u8> span(FIELD_SPAN_HI - FIELD_SPAN_LO);
  size_t found = 0;
  for (u32 player = 1; player <= 2; ++player)
  {
    const u32 fighter = fighters[player - 1];
    if (fighter == 0 || !memory.Read(fighter + FIELD_SPAN_LO, span.data(), span.size()))
      continue;
    append_be32(player);
    for (const FieldDef& f : FIELDS)
      stream.insert(stream.end(), span.begin() + (f.offset - FIELD_SPAN_LO), span.begin() + (f.offset - FIELD_SPAN_LO) + 4);
    ++found;
  }
  *checksum = Common::ComputeCRC32(stream.data(), stream.size());
  return found;
}

u32 MemoryCrc(const GuestMemory& memory, const std::vector<Range>& ranges, size_t* unreadable)
{
  constexpr u32 CHUNK = 1u << 20;  // copy size when the memory cannot hand out a pointer
  u32 crc = Common::StartCRC32();
  std::vector<u8> buffer;
  for (const Range& r : ranges)
  {
    const u8* direct = memory.Peek(r.start, r.end - r.start);
    if (direct != nullptr)
    {
      crc = Common::UpdateCRC32(crc, direct, r.end - r.start);
      continue;
    }
    for (u32 addr = r.start; addr < r.end;)
    {
      const u32 n = std::min(CHUNK, r.end - addr);
      buffer.resize(n);
      if (!memory.Read(addr, buffer.data(), n))
      {
        if (unreadable != nullptr)
          ++*unreadable;
        break;
      }
      crc = Common::UpdateCRC32(crc, buffer.data(), n);
      addr += n;
    }
  }
  return crc;
}

namespace
{
// Value of one hex digit, or -1.
int HexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// 24 hex digits -> 12 pad bytes.
bool ParsePadHex(const std::string& token, PadBytes* out)
{
  if (token.size() != 24)
    return false;
  for (size_t i = 0; i < 12; ++i)
  {
    const int hi = HexValue(token[2 * i]);
    const int lo = HexValue(token[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    (*out)[i] = static_cast<u8>((hi << 4) | lo);
  }
  return true;
}

bool ParseDecimal(const std::string& token, bool allow_sign, long long* out)
{
  size_t i = 0;
  bool negative = false;
  if (allow_sign && !token.empty() && (token[0] == '-' || token[0] == '+'))
  {
    negative = token[0] == '-';
    i = 1;
  }
  if (i >= token.size() || token.size() - i > 12)
    return false;
  long long value = 0;
  for (; i < token.size(); ++i)
  {
    if (token[i] < '0' || token[i] > '9')
      return false;
    value = value * 10 + (token[i] - '0');
  }
  *out = negative ? -value : value;
  return true;
}

std::string PadHex(const PadBytes& pad)
{
  static const char DIGITS[] = "0123456789abcdef";
  std::string s;
  s.reserve(24);
  for (const u8 b : pad)
  {
    s.push_back(DIGITS[b >> 4]);
    s.push_back(DIGITS[b & 15]);
  }
  return s;
}
}  // namespace

bool ParsePadScript(const std::string& text, PadScript* out, std::string* error)
{
  PadScript script;
  size_t pos = 0;
  while (pos < text.size())
  {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos)
      eol = text.size();  // no final newline
    const std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;

    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size())
    {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        ++i;
      const size_t start = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
        ++i;
      if (i > start)
        tokens.push_back(line.substr(start, i - start));
    }
    if (tokens.empty() || tokens[0][0] == '#')
      continue;

    long long frame = 0;
    long long pad_return = 0;
    PadScriptEntry entry;
    if (tokens.size() < 3 || !ParseDecimal(tokens[0], false, &frame) || frame > 0xFFFFFFFFll ||
        !ParsePadHex(tokens[1], &entry.pads[0]) || !ParsePadHex(tokens[2], &entry.pads[1]) ||
        (tokens.size() >= 4 && (!ParseDecimal(tokens[3], true, &pad_return) || pad_return < -0x80000000ll || pad_return > 0x7FFFFFFFll)))
    {
      ++script.ignored_lines;
      continue;
    }
    entry.pad_return = static_cast<s32>(pad_return);
    const u32 f = static_cast<u32>(frame);
    if (script.frames.empty())
      script.first = script.last = f;
    script.first = std::min(script.first, f);
    script.last = std::max(script.last, f);
    script.frames[f] = entry;
  }
  if (script.frames.empty())
  {
    if (error != nullptr)
      *error = "no pad lines (expected \"<frame> <24 hex> <24 hex> <return>\")";
    return false;
  }
  *out = std::move(script);
  return true;
}

std::string FormatPadRecordLine(u32 frame, const std::array<PadBytes, 2>& pads, s32 pad_return)
{
  return Format("%u %s %s %d", frame, PadHex(pads[0]).c_str(), PadHex(pads[1]).c_str(), pad_return);
}

std::string FormatTraceLine(u32 frame, int fighters, u32 checksum, u32 mem)
{
  return Format("T frame=%u fighters=%d chk=0x%08X mem=0x%08X", frame, fighters, checksum, mem);
}

DeterminismKit::DeterminismKit(DeterminismOptions options, GuestMemory& memory, Sink& log, LineWriter* record, LineWriter* trace)
    : m_options(std::move(options)), m_memory(memory), m_log(log), m_record(record), m_trace(trace)
{
  m_options.trace_every = std::max<u32>(m_options.trace_every, 1);
  if (m_record != nullptr)
  {
    if (m_options.record_header)
      m_record->Line(PAD_RECORD_HEADER);
    m_log.Line(Format("NOTE pad record -> %s", m_options.record_path.c_str()));
  }
  if (!m_options.script_path.empty())
  {
    const PadScript& s = m_options.script;
    m_log.Line(Format("NOTE pad script %s frames=%zu first=%u last=%u", m_options.script_path.c_str(), s.frames.size(), s.first, s.last));
    if (s.ignored_lines != 0)
      m_log.Line(Format("NOTE pad script: %zu malformed lines ignored", s.ignored_lines));
  }
  if (m_trace != nullptr)
  {
    size_t bytes = 0;
    for (const Range& r : m_options.ranges)
      bytes += r.end - r.start;
    m_log.Line(Format("NOTE trace -> %s every=%u ranges=%zu bytes=%zu", m_options.trace_path.c_str(), m_options.trace_every,
                      m_options.ranges.size(), bytes));
  }
}

void DeterminismKit::OnCommand(Command* command, bool resimulating)
{
  const u32 frame = command->frame;
  const bool scripted = !m_options.script_path.empty();
  if (scripted)
  {
    const auto it = m_options.script.frames.find(frame);
    if (it != m_options.script.frames.end())
    {
      command->pads = it->second.pads;
      command->pad_return = it->second.pad_return;
    }
  }
  if (resimulating)
    return;

  if (scripted && !m_script_ended && frame > m_options.script.last)
  {
    m_script_ended = true;
    const std::string note = Format("NOTE pad script ended at frame %u", m_options.script.last);
    m_log.Line(note);
    if (m_trace != nullptr)
    {
      m_trace->Line(note);
      m_trace->Flush();
    }
  }

  if (m_record != nullptr)
  {
    m_record->Line(FormatPadRecordLine(frame, command->pads, command->pad_return));
    ++m_record_lines;
    if (++m_record_pending >= PAD_RECORD_FLUSH_EVERY)
    {
      m_record->Flush();
      m_record_pending = 0;
    }
  }

  if (m_trace != nullptr && frame % m_options.trace_every == 0)
  {
    u32 checksum = 0;
    const size_t found = FighterChecksum(m_memory, frame, &checksum);
    size_t unreadable = 0;
    const u32 mem = m_options.ranges.empty() ? 0 : MemoryCrc(m_memory, m_options.ranges, &unreadable);
    if (unreadable != 0 && !m_unreadable_logged)
    {
      m_unreadable_logged = true;
      m_log.Line(Format("ERROR frame %u: %zu trace range(s) could not be read (left out of mem; logged once)", frame, unreadable));
    }
    // Both fighters or none: a lone fighter is a menu / loading state and is reported like no fighters (the checksum would
    // depend on which one is left).
    const bool both = found == 2;
    m_trace->Line(FormatTraceLine(frame, both ? 2 : 0, both ? checksum : 0, mem));
    m_trace->Flush();
    ++m_trace_lines;
  }
}

void DeterminismKit::Finish()
{
  if (m_record != nullptr)
    m_record->Flush();
  if (m_trace != nullptr)
    m_trace->Flush();
  m_record_pending = 0;
}

// ---- (4c) freeze dump ---------------------------------------------------------------------------------------------------------
// See Rev3Rollback.h "freeze dump" and REV3.md "Freeze dump". Reads guest memory and hardware only; writes nothing but log lines.

namespace
{
// Rev3's SDK globals (from the decomp, addresses only). OSThread offsets follow the standard Dolphin SDK layout
// (context 0x000..0x2C8, state, attr, suspend, priority, base, val, queue, link, queueJoin, mutex, queueMutex, linkActive, stackBase, stackEnd).
constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;  // OSGetCurrentContext
constexpr u32 OS_FPU_OWNER = 0x800000D8;        // thread whose FPRs are live
constexpr u32 OS_ACTIVE_THREADS = 0x800000DC;   // head of the active thread list (OSThread.linkActive)
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;   // OSGetCurrentThread
constexpr u32 OS_SCHED_DISABLE = 0x8049DAB8;    // OSDisableScheduler count (nonzero while a thread is blocked in a critical section)
constexpr u32 OS_RESCHED = 0x8049DABC;          // reschedule requested
constexpr u32 OS_RUN_QUEUE_BITS = 0x8049DAC0;   // bit per priority with a ready thread

constexpr u32 TH_CTX_GPR1 = 0x04;
constexpr u32 TH_CTX_LR = 0x84;
constexpr u32 TH_CTX_SRR0 = 0x198;
constexpr u32 TH_STATE = 0x2C8;       // u16: 1 ready, 2 running, 4 waiting, 8 moribund
constexpr u32 TH_SUSPEND = 0x2CC;     // s32
constexpr u32 TH_PRIORITY = 0x2D0;    // s32
constexpr u32 TH_QUEUE = 0x2DC;       // OSThreadQueue* the thread sleeps in
constexpr u32 TH_LINK_NEXT = 0x2E0;   // OSThreadLink.next inside that queue
constexpr u32 TH_ACTIVE_NEXT = 0x2FC; // OSThreadLink.next of the active list
constexpr u32 TH_STACK_BASE = 0x304;  // highest address
constexpr u32 TH_STACK_END = 0x308;   // lowest address; the canary word lives here
constexpr u32 STACK_CANARY = 0xDEADBABE;

constexpr u32 DUMP_WORDS_BEFORE = 0x20;  // bytes dumped before a queue address
constexpr u32 DUMP_WORDS_COUNT = 24;     // words dumped around a queue address (multiple of 8)
constexpr size_t DUMP_MAX_THREADS = 32;
constexpr size_t DUMP_MAX_WAITERS = 8;
constexpr size_t DUMP_PPC_FRAMES = 16;
constexpr size_t DUMP_THREAD_FRAMES = 6;

// Sleep queues ({head, tail}, 8 bytes each) the freeze can be waiting in.
struct DumpQueueDef
{
  const char* name;
  u32 address;
};
constexpr DumpQueueDef DUMP_QUEUES[] = {
    {"gx_drawdone", 0x8049D950}, {"vi_retrace", 0x8049D4B8}, {"dvd", 0x8049D7B0},
    {"nw4r_strm", 0x8049D238},   {"ax", 0x8049D6A0},         {"sc", 0x804655A0},
};
constexpr u32 DUMP_IPC_POINTER = 0x8049D984;  // *ptr + 0x2C is the IPC response queue
constexpr u32 DUMP_IPC_OFFSET = 0x2C;
constexpr u32 DUMP_GX_DRAWDONE_FLAG = 0x8049D958;  // byte
constexpr u32 DUMP_VI_RETRACE_COUNT = 0x8049D4DC;
constexpr u32 DUMP_GAME_LAST_VI_COUNT = 0x8049CF48;  // the game's last-frame count
constexpr u32 DUMP_GAME_FRAME_FLAGS = 0x803C19C0;
constexpr u32 DUMP_GAME_MAIN_STATE = 0x803C11D4;
constexpr u32 DUMP_GAME_NO_GX = 0x803C1A70;

std::optional<u32> Rd32(const GuestMemory& memory, u32 addr)
{
  u8 b[4];
  if (!memory.Read(addr, b, 4))
    return std::nullopt;
  return ReadBE32(b);
}

std::optional<u32> Rd16(const GuestMemory& memory, u32 addr)
{
  u8 b[2];
  if (!memory.Read(addr, b, 2))
    return std::nullopt;
  return (static_cast<u32>(b[0]) << 8) | b[1];
}

std::optional<u32> Rd8(const GuestMemory& memory, u32 addr)
{
  u8 b[1];
  if (!memory.Read(addr, b, 1))
    return std::nullopt;
  return static_cast<u32>(b[0]);
}

std::string Hex32(const std::optional<u32>& v)
{
  return v ? Format("0x%08X", *v) : std::string("?");
}
std::string Hex16(const std::optional<u32>& v)
{
  return v ? Format("0x%04X", *v) : std::string("?");
}
std::string Hex8(const std::optional<u32>& v)
{
  return v ? Format("0x%02X", *v) : std::string("?");
}
std::string Signed(const std::optional<u32>& v)
{
  return v ? Format("%d", static_cast<int>(static_cast<s32>(*v))) : std::string("?");
}

// A thread / queue pointer worth following: inside MEM1 / MEM2 and word aligned.
bool DumpPtrOk(u32 p)
{
  return PointerOk(p) && (p & 3) == 0;
}

void AppendFrames(std::vector<std::string>* out, const std::string& label, const std::vector<StackFrame>& frames)
{
  for (size_t i = 0; i < frames.size(); ++i)
    out->push_back(Format("%sframe %zu sp=0x%08X lr=0x%08X", label.c_str(), i, frames[i].sp, frames[i].lr));
}

void AppendCpu(std::vector<std::string>* out, const GuestMemory& memory, const DumpCpu& cpu, bool ok)
{
  if (!ok)
  {
    out->push_back("ppc unavailable");
    return;
  }
  out->push_back(Format("ppc pc=0x%08X npc=0x%08X lr=0x%08X ctr=0x%08X msr=0x%08X srr0=0x%08X srr1=0x%08X cr=0x%08X "
                        "r1=0x%08X r2=0x%08X r13=0x%08X",
                        cpu.pc, cpu.npc, cpu.lr, cpu.ctr, cpu.msr, cpu.srr0, cpu.srr1, cpu.cr, cpu.gpr[1], cpu.gpr[2], cpu.gpr[13]));
  std::string regs = "ppc";
  for (size_t r = 3; r <= 12; ++r)
    regs += Format(" r%zu=0x%08X", r, cpu.gpr[r]);
  out->push_back(regs + Format(" exceptions=0x%08X", cpu.exceptions));
  std::vector<StackFrame> frames;
  WalkBackChain(memory, cpu.gpr[1], DUMP_PPC_FRAMES, &frames);
  if (frames.empty())
    out->push_back(Format("frame none: r1=0x%08X is not a readable stack pointer", cpu.gpr[1]));
  AppendFrames(out, "", frames);
}

// One OSThread. `live` = the running thread: pc / lr / sp come from the live registers instead of its (stale) saved context.
void AppendThread(std::vector<std::string>* out, const GuestMemory& memory, u32 t, const DumpCpu* live)
{
  const std::optional<u32> state = Rd16(memory, t + TH_STATE);
  const std::optional<u32> prio = Rd32(memory, t + TH_PRIORITY);
  const std::optional<u32> suspend = Rd32(memory, t + TH_SUSPEND);
  const std::optional<u32> queue = Rd32(memory, t + TH_QUEUE);
  const std::optional<u32> stack_base = Rd32(memory, t + TH_STACK_BASE);
  const std::optional<u32> stack_end = Rd32(memory, t + TH_STACK_END);
  std::optional<u32> pc = Rd32(memory, t + TH_CTX_SRR0);
  std::optional<u32> lr = Rd32(memory, t + TH_CTX_LR);
  std::optional<u32> sp = Rd32(memory, t + TH_CTX_GPR1);
  if (live != nullptr)
  {
    pc = live->pc;
    lr = live->lr;
    sp = live->gpr[1];
  }
  std::string canary = "?";
  if (stack_end)
  {
    if (const std::optional<u32> word = Rd32(memory, *stack_end))
      canary = *word == STACK_CANARY ? "ok" : "bad";
  }
  out->push_back(Format("thread 0x%08X state=%s prio=%s suspend=%s queue=%s pc=%s lr=%s sp=%s canary=%s stack=%s..%s%s", t,
                        state ? Format("%u", *state).c_str() : "?", Signed(prio).c_str(), Signed(suspend).c_str(),
                        Hex32(queue).c_str(), Hex32(pc).c_str(), Hex32(lr).c_str(), Hex32(sp).c_str(), canary.c_str(),
                        Hex32(stack_end).c_str(), Hex32(stack_base).c_str(), live != nullptr ? " running=1 live=1" : ""));
  if (sp)
  {
    std::vector<StackFrame> frames;
    WalkBackChain(memory, *sp, DUMP_THREAD_FRAMES, &frames);
    AppendFrames(out, Format("thread 0x%08X ", t), frames);
  }
}

void AppendOs(std::vector<std::string>* out, const GuestMemory& memory, const DumpCpu& cpu, bool cpu_ok)
{
  const std::optional<u32> current = Rd32(memory, OS_CURRENT_THREAD);
  const std::optional<u32> head = Rd32(memory, OS_ACTIVE_THREADS);
  out->push_back(Format("os cur_thread=%s cur_context=%s fpu_owner=%s sched_disable=%s run_queue_bits=%s resched=%s active_head=%s",
                        Hex32(current).c_str(), Hex32(Rd32(memory, OS_CURRENT_CONTEXT)).c_str(),
                        Hex32(Rd32(memory, OS_FPU_OWNER)).c_str(), Hex32(Rd32(memory, OS_SCHED_DISABLE)).c_str(),
                        Hex32(Rd32(memory, OS_RUN_QUEUE_BITS)).c_str(), Hex32(Rd32(memory, OS_RESCHED)).c_str(),
                        Hex32(head).c_str()));

  // The active thread list, at most DUMP_MAX_THREADS, with loop detection.
  std::vector<u32> threads;
  std::string end = "null";
  u32 t = head.value_or(0);
  if (!head)
    end = "unreadable";
  while (head)
  {
    if (t == 0)
      break;
    if (!DumpPtrOk(t))
    {
      end = Format("bad_ptr(0x%08X)", t);
      break;
    }
    if (std::find(threads.begin(), threads.end(), t) != threads.end())
    {
      end = Format("cycle(0x%08X)", t);
      break;
    }
    if (threads.size() >= DUMP_MAX_THREADS)
    {
      end = "max";
      break;
    }
    threads.push_back(t);
    const std::optional<u32> next = Rd32(memory, t + TH_ACTIVE_NEXT);
    if (!next)
    {
      end = "unreadable_link";
      break;
    }
    t = *next;
  }
  out->push_back(Format("threads n=%zu end=%s", threads.size(), end.c_str()));
  for (const u32 thread : threads)
    AppendThread(out, memory, thread, cpu_ok && current && *current == thread ? &cpu : nullptr);
}

// One sleep queue {head, tail} at `address`: the first waiters (linked through OSThread.link) and the head thread's saved pc / lr.
void AppendQueue(std::vector<std::string>* out, const GuestMemory& memory, const char* name, u32 address, const std::string& extra)
{
  const std::optional<u32> head = Rd32(memory, address);
  const std::optional<u32> tail = Rd32(memory, address + 4);
  if (!head || !tail)
  {
    out->push_back(Format("queue %s at=0x%08X unreadable%s", name, address, extra.c_str()));
    return;
  }
  if (*head == 0)
  {
    out->push_back(Format("queue %s at=0x%08X head=0x00000000 tail=%s waiters=0%s", name, address, Hex32(tail).c_str(), extra.c_str()));
    return;
  }
  std::vector<u32> waiters;
  u32 t = *head;
  std::string end;
  while (t != 0)
  {
    if (!DumpPtrOk(t))
    {
      end = Format(" bad_link=0x%08X", t);
      break;
    }
    if (waiters.size() >= DUMP_MAX_WAITERS || std::find(waiters.begin(), waiters.end(), t) != waiters.end())
    {
      end = " more";
      break;
    }
    waiters.push_back(t);
    const std::optional<u32> next = Rd32(memory, t + TH_LINK_NEXT);
    if (!next)
    {
      end = " unreadable_link";
      break;
    }
    t = *next;
  }
  std::string list;
  for (const u32 w : waiters)
    list += Format("%s0x%08X", list.empty() ? "" : ",", w);
  std::string line = Format("queue %s at=0x%08X head=0x%08X tail=%s waiters=%zu[%s]%s", name, address, *head, Hex32(tail).c_str(),
                            waiters.size(), list.c_str(), end.c_str());
  if (DumpPtrOk(*head))
  {
    line += Format(" head_state=%s head_pc=%s head_lr=%s", Hex16(Rd16(memory, *head + TH_STATE)).c_str(),
                   Hex32(Rd32(memory, *head + TH_CTX_SRR0)).c_str(), Hex32(Rd32(memory, *head + TH_CTX_LR)).c_str());
  }
  out->push_back(line + extra);
}

void AppendQueues(std::vector<std::string>* out, const GuestMemory& memory)
{
  for (const DumpQueueDef& q : DUMP_QUEUES)
  {
    std::string extra;
    if (q.address == 0x8049D950)
      extra = Format(" drawdone_flag=%s", Hex8(Rd8(memory, DUMP_GX_DRAWDONE_FLAG)).c_str());
    else if (q.address == 0x8049D4B8)
      extra = Format(" retrace_count=%s game_last_count=%s", Hex32(Rd32(memory, DUMP_VI_RETRACE_COUNT)).c_str(),
                     Hex32(Rd32(memory, DUMP_GAME_LAST_VI_COUNT)).c_str());
    AppendQueue(out, memory, q.name, q.address, extra);
  }
  // The words around the DVD and nw4r stream queues: the neighbouring SDK state (DVD command block pointers / state, stream player flags),
  // for decoding by hand. DUMP_WORDS_BEFORE bytes before the queue, DUMP_WORDS_COUNT words in all.
  for (const u32 queue : {0x8049D7B0u, 0x8049D238u})
  {
    const u32 base = queue - DUMP_WORDS_BEFORE;
    for (u32 row = 0; row < DUMP_WORDS_COUNT; row += 8)
    {
      std::string words;
      for (u32 i = row; i < row + 8; ++i)
        words += " " + Hex32(Rd32(memory, base + 4 * i));
      out->push_back(Format("words %s base=0x%08X +0x%02X:%s", queue == 0x8049D7B0u ? "dvd" : "nw4r_strm", base, 4 * row, words.c_str()));
    }
  }
  // The IPC queue sits behind a pointer (*0x8049D984 + 0x2C); skipped when the pointer is not a valid object.
  const std::optional<u32> ipc = Rd32(memory, DUMP_IPC_POINTER);
  if (ipc && DumpPtrOk(*ipc))
    AppendQueue(out, memory, "ipc", *ipc + DUMP_IPC_OFFSET, Format(" ipc_object=0x%08X", *ipc));
  else
    out->push_back(Format("queue ipc skipped: pointer *0x%08X is %s", DUMP_IPC_POINTER, Hex32(ipc).c_str()));
  out->push_back(Format("game frame_loop_flags=%s main_state=%s no_gx=%s", Hex32(Rd32(memory, DUMP_GAME_FRAME_FLAGS)).c_str(),
                        Hex32(Rd32(memory, DUMP_GAME_MAIN_STATE)).c_str(), Hex32(Rd32(memory, DUMP_GAME_NO_GX)).c_str()));
}

void AppendHw(std::vector<std::string>* out, const DumpHw& hw, bool ok)
{
  if (!ok)
  {
    out->push_back("hw unavailable");
    return;
  }
  out->push_back(hw.pe_ok ? Format("hw pe pe_status=0x%04X pe_token=0x%04X", hw.pe_status, hw.pe_token) : std::string("hw pe unavailable"));
  if (hw.cp_ok)
  {
    const char* status = hw.cp_breakpoint ? "breakpoint" : (hw.cp_dist == 0 ? "idle" : "busy");
    out->push_back(Format("hw cp status=%s base=0x%08X end=0x%08X rd=0x%08X wr=0x%08X rw_dist=0x%X bp=0x%08X gp_read=%d bp_enable=%d link=%d "
                          "hi_wm=%d lo_wm=%d",
                          status, hw.cp_base, hw.cp_end, hw.cp_rd, hw.cp_wr, hw.cp_dist, hw.cp_bp, hw.cp_gp_read ? 1 : 0,
                          hw.cp_bp_enable ? 1 : 0, hw.cp_link ? 1 : 0, hw.cp_hi_wm ? 1 : 0, hw.cp_lo_wm ? 1 : 0));
  }
  else
  {
    out->push_back("hw cp unavailable");
  }
  out->push_back(hw.pi_ok ? Format("hw pi cause=0x%08X mask=0x%08X fifo_base=0x%08X fifo_end=0x%08X fifo_wptr=0x%08X", hw.pi_cause,
                                   hw.pi_mask, hw.pi_fifo_base, hw.pi_fifo_end, hw.pi_fifo_wptr) :
                            std::string("hw pi unavailable"));
  out->push_back(hw.vi_ok ? Format("hw vi line=%u di0=0x%08X", hw.vi_line, hw.vi_di0) : std::string("hw vi unavailable"));
}
}  // namespace

size_t WalkBackChain(const GuestMemory& memory, u32 sp, size_t max_frames, std::vector<StackFrame>* out)
{
  out->clear();
  while (out->size() < max_frames)
  {
    if (sp == 0 || (sp & 3) != 0)
      break;
    u8 words[8];
    if (!memory.Read(sp, words, sizeof(words)))
      break;
    const u32 next = ReadBE32(words);
    out->push_back({sp, ReadBE32(words + 4)});
    if (next <= sp)  // zero (the root), or a loop / a stack that does not grow upwards
      break;
    sp = next;
  }
  return out->size();
}

FreezeDump::FreezeDump(const GuestMemory& memory, Sink& sink, DumpSource& source, u32 stall_ms, std::function<u64()> now_ms)
    : m_memory(memory), m_sink(sink), m_source(source), m_stall_ms(std::max<u32>(stall_ms, 1)), m_now_ms(std::move(now_ms))
{
  if (!m_now_ms)
    m_now_ms = []() { return SteadyMicros() / 1000; };
}

std::vector<std::string> FreezeDump::Lines(bool full, bool racy)
{
  std::vector<std::string> out;
  DumpCpu cpu;
  const bool cpu_ok = m_source.ReadCpu(&cpu) && cpu.valid;
  DumpHw hw;
  const bool hw_ok = m_source.ReadHw(&hw);
  AppendCpu(&out, m_memory, cpu, cpu_ok);
  AppendOs(&out, m_memory, cpu, cpu_ok);
  AppendQueues(&out, m_memory);
  // The effect system's lists (a spinning ParticleManager::Calc is a list that never ends): `ef ...` lines, read only.
  for (std::string& line : EfDumpLines(m_memory))
    out.push_back(std::move(line));
  AppendHw(&out, hw, hw_ok);
  m_source.ExtraLines(&out, racy);
  if (full)
    m_source.AudioLines(&out);
  return out;
}

void FreezeDump::Dump(const char* reason, bool racy, bool full, const std::string& note)
{
  std::lock_guard<std::mutex> lock(m_lock);
  const std::string prefix = Format("DUMP %s ", reason);
  m_sink.Line(prefix + Format("begin racy=%d frame=%u last_cmd_age_ms=%llu", racy ? 1 : 0, m_frame.load(),
                              static_cast<unsigned long long>(LastAgeMs())) +
              (note.empty() ? std::string() : " " + note));
  for (const std::string& line : Lines(full, racy))
    m_sink.Line(prefix + line);
  m_sink.Line(prefix + "end");
}

std::string FreezeDump::RestoreHwLine(u32 from, u32 to, bool after)
{
  DumpHw hw;
  const bool ok = m_source.ReadHw(&hw);
  const auto field = [ok](bool group_ok, const std::string& value) { return ok && group_ok ? value : std::string("?"); };
  return Format("RESTOREHW %u -> %u%s pe_status=%s pe_token=%s cp_rw_dist=%s cp_rd=%s cp_wr=%s vi_count=%s drawdone=%s sched_disable=%s",
                from, to, after ? " after" : "", field(hw.pe_ok, Format("0x%04X", hw.pe_status)).c_str(),
                field(hw.pe_ok, Format("0x%04X", hw.pe_token)).c_str(), field(hw.cp_ok, Format("0x%X", hw.cp_dist)).c_str(),
                field(hw.cp_ok, Format("0x%08X", hw.cp_rd)).c_str(), field(hw.cp_ok, Format("0x%08X", hw.cp_wr)).c_str(),
                Hex32(Rd32(m_memory, DUMP_VI_RETRACE_COUNT)).c_str(), Hex8(Rd8(m_memory, DUMP_GX_DRAWDONE_FLAG)).c_str(),
                Hex32(Rd32(m_memory, OS_SCHED_DISABLE)).c_str());
}

void FreezeDump::Tick(u32 frame)
{
  m_frame.store(frame);
  m_last_ms.store(m_now_ms());
  m_seen.store(true);
  m_reported.store(false);
}

void FreezeDump::SetBlocked(bool blocked)
{
  if (!blocked && m_blocked.load())
    m_last_ms.store(m_now_ms());
  m_blocked.store(blocked);
}

u64 FreezeDump::LastAgeMs() const
{
  if (!m_seen.load())
    return 0;
  const u64 now = m_now_ms();
  const u64 last = m_last_ms.load();
  return now > last ? now - last : 0;
}

bool FreezeDump::StallDue()
{
  if (!m_seen.load() || m_blocked.load() || m_reported.load() || LastAgeMs() < m_stall_ms)
    return false;
  return !m_reported.exchange(true);  // claimed once; the next Tick starts a new stall
}

// ---- (4d) nw4r::ef effect pool ---------------------------------------------------------------------------------------------
// What the addresses and offsets mean (decomp functions, allocation, lists) is written down in Rev3Rollback.h "nw4r::ef effect pool".
// Everything here is pure: GuestMemory in, ranges / a check result / log lines out.

namespace
{
bool ReadWord(const GuestMemory& memory, u32 addr, u32* out)
{
  u8 raw[4];
  if (!memory.Read(addr, raw, 4))
    return false;
  *out = ReadBE32(raw);
  return true;
}

// [start, end) widened to whole 4 KB pages and clipped to the region `start` is in (which must be MEM1 / MEM2).
Range PageAligned(u32 start, u32 end)
{
  const u32 region_end = RegionEnd(start);
  const u32 lo = start & ~(PAGE_SIZE - 1);
  const u64 hi = std::min<u64>((static_cast<u64>(end) + PAGE_SIZE - 1) & ~static_cast<u64>(PAGE_SIZE - 1), region_end);
  return {lo, static_cast<u32>(hi)};
}

const char* const EF_KIND[4] = {"effect", "emitter", "pm", "particle"};
}  // namespace

bool FindEfPool(const GuestMemory& memory, EfPool* out, std::string* why)
{
  std::string scratch;
  if (why == nullptr)
    why = &scratch;
  EfPool pool;
  u32 mm = 0;
  if (!ReadWord(memory, EF_SYSTEM_ADDR, &mm))
  {
    *why = Format("cannot read the EffectSystem at 0x%08X", EF_SYSTEM_ADDR);
    return false;
  }
  if (mm == 0)
  {
    *why = Format("the memory manager pointer at EffectSystem 0x%08X is 0 (effect system not initialised yet)", EF_SYSTEM_ADDR);
    return false;
  }
  if (!PointerOk(mm) || (mm & 0x1F) != 0)
  {
    *why = Format("the memory manager pointer 0x%08X is not a 32-byte aligned MEM1 / MEM2 address", mm);
    return false;
  }
  u32 heap_start = 0;
  u32 heap_end = 0;
  if (!ReadWord(memory, mm + EF_MM_HEAP_START, &heap_start) || !ReadWord(memory, mm + EF_MM_HEAP_END, &heap_end))
  {
    *why = Format("cannot read the heap bounds of the manager at 0x%08X", mm);
    return false;
  }
  const u32 region_end = RegionEnd(mm);
  if (heap_start < mm + 0x3C || heap_end <= heap_start || heap_end > region_end || heap_end - mm > EF_POOL_MAX_BYTES)
  {
    *why = Format("implausible heap 0x%08X-0x%08X for the manager at 0x%08X (expected mm + 0x40 .. inside the same region, at most 0x%X bytes)",
                  heap_start, heap_end, mm, EF_POOL_MAX_BYTES);
    return false;
  }
  pool.mm = mm;
  pool.heap_start = heap_start;
  pool.heap_end = heap_end;
  for (size_t i = 0; i < 4; ++i)
    ReadWord(memory, mm + EF_MM_COUNTS[i], &pool.counts[i]);

  // The allocation: manager + heap + the tail behind it (handle table and slack, all one block).
  const u32 main_end = static_cast<u32>(std::min<u64>(static_cast<u64>(heap_end) + EF_BLOCK_TAIL, region_end));
  pool.main = PageAligned(mm, main_end);
  std::vector<Range> all = {pool.main};
  std::string detail;

  // The EffectSystem singleton itself (a static, MEM1): the manager pointer, the creation queue, the activity list pointers.
  const Range instance = PageAligned(EF_SYSTEM_ADDR, EF_SYSTEM_ADDR + EF_SYSTEM_BYTES);
  all.push_back(instance);
  detail += Format("instance=0x%08X-0x%08X", instance.start, instance.end);

  // The game's handle table (0x101 words behind the heap) and the two small fixed pools the handles come from.
  u32 table = 0;
  if (ReadWord(memory, EF_HANDLE_TABLE_GLOBAL, &table) && table != 0 && PointerOk(table) && (table & 3) == 0 &&
      static_cast<u64>(table) + EF_HANDLE_TABLE_BYTES <= RegionEnd(table))
  {
    all.push_back(PageAligned(table, table + EF_HANDLE_TABLE_BYTES));
    detail += Format(" handle_table=0x%08X%s", table, (table >= mm && table + EF_HANDLE_TABLE_BYTES <= main_end) ? "(inside the block)" : "");
  }
  else
  {
    detail += " handle_table=none";
  }
  for (size_t g = 0; g < 2; ++g)
  {
    u32 p = 0;
    u32 n = 0;
    u32 payload = 0;
    if (!ReadWord(memory, EF_HANDLE_POOL_GLOBALS[g], &p) || p == 0 || !PointerOk(p) || (p & 3) != 0 || !ReadWord(memory, p + 8, &n) ||
        !ReadWord(memory, p + 0xC, &payload) || n == 0 || n > 0x10000 || payload > 0x1000)
    {
      detail += Format(" handle_pool%zu=none", g);
      continue;
    }
    const u64 bytes = 0x10 + static_cast<u64>(n) * (payload + 4);
    if (static_cast<u64>(p) + bytes > RegionEnd(p))
    {
      detail += Format(" handle_pool%zu=none", g);
      continue;
    }
    all.push_back(PageAligned(p, static_cast<u32>(p + bytes)));
    detail += Format(" handle_pool%zu=0x%08X(n=%u,payload=0x%X)", g, p, n, payload);
  }
  pool.ranges = MergeRanges(std::move(all));
  pool.bytes = 0;
  for (const Range& r : pool.ranges)
    pool.bytes += r.end - r.start;
  pool.detail = std::move(detail);
  *out = std::move(pool);
  return true;
}

namespace
{
// The list walker of CheckEfLists. One instance per check; the first failure sticks.
class EfWalker
{
public:
  EfWalker(const GuestMemory& memory, u32 list_cap, u32 pool_lo, u32 pool_hi)
      : m_memory(memory), m_list_cap(list_cap), m_lo(pool_lo), m_hi(pool_hi)
  {
    m_result.ran = true;
  }

  EfCheckResult& Result() { return m_result; }

  bool Fail(const std::string& list, u32 list_addr, u32 node, const std::string& why)
  {
    m_result.ok = false;
    m_result.list = list;
    m_result.list_addr = list_addr;
    m_result.node = node;
    m_result.why = why;
    return false;
  }

  // Walks the ut::List at `list_addr` whose links sit at node + `offset`; calls visit(node) for each node (before its successor is read).
  bool Walk(const std::string& name, u32 list_addr, u32 offset, const std::function<bool(u32)>& visit)
  {
    u8 raw[12];
    if (!m_memory.Read(list_addr, raw, sizeof(raw)))
      return Fail(name, list_addr, 0, "cannot read the list header");
    const u32 head = ReadBE32(raw);
    const u32 tail = ReadBE32(raw + 4);
    const u32 count = (static_cast<u32>(raw[8]) << 8) | raw[9];
    const u32 stored_offset = (static_cast<u32>(raw[10]) << 8) | raw[11];
    ++m_result.lists;
    if (stored_offset != offset)
      return Fail(name, list_addr, 0, Format("list offset is 0x%X, expected 0x%X", stored_offset, offset));
    if (head == 0 || tail == 0 || count == 0)
    {
      if (head != 0 || tail != 0 || count != 0)
        return Fail(name, list_addr, 0, Format("empty and non-empty at once: head=0x%08X tail=0x%08X count=%u", head, tail, count));
      return true;
    }
    u32 prev = 0;
    u32 node = head;
    u32 n = 0;
    while (node != 0)
    {
      if (n >= m_list_cap || m_result.nodes >= EF_CHECK_TOTAL_CAP)
        return Fail(name, list_addr, node, Format("more than %u nodes: the list is cyclic (or its count is wrong)", n));
      if ((node & 3) != 0 || node < m_lo || static_cast<u64>(node) + offset + 8 > m_hi)
        return Fail(name, list_addr, node, Format("node is outside the ef heap 0x%08X-0x%08X (or misaligned)", m_lo, m_hi));
      u8 link[8];
      if (!m_memory.Read(node + offset, link, sizeof(link)))
        return Fail(name, list_addr, node, "cannot read the node's link");
      const u32 node_prev = ReadBE32(link);
      const u32 node_next = ReadBE32(link + 4);
      if (node_prev != prev)
        return Fail(name, list_addr, node, Format("prev link is 0x%08X, expected 0x%08X (node %u of %u)", node_prev, prev, n, count));
      ++n;
      ++m_result.nodes;
      if (!visit(node))
        return false;
      prev = node;
      node = node_next;
    }
    if (prev != tail)
      return Fail(name, list_addr, prev, Format("the walk ends at 0x%08X but the tail is 0x%08X", prev, tail));
    if (n != count)
      return Fail(name, list_addr, prev, Format("walked %u nodes but the count says %u", n, count));
    return true;
  }

private:
  const GuestMemory& m_memory;
  u32 m_list_cap;
  u32 m_lo;
  u32 m_hi;
  EfCheckResult m_result;
};

constexpr u32 EF_LIST_OFFSET = 0x14;    // offset of the activity link in Effect / Emitter / ParticleManager / Particle (List_Init(..., 0x14))
constexpr u32 EF_GROUP_STRIDE = 0x1C;   // sizeof(ActivityList)
constexpr u32 EF_SYS_GROUPS = 0xC014;   // EffectSystem: activity group count
constexpr u32 EF_SYS_ACTIVITY = 0xC018; // EffectSystem: ActivityList[] pointer
}  // namespace

EfCheckResult CheckEfLists(const GuestMemory& memory, u32 list_cap)
{
  u32 mm = 0;
  if (!ReadWord(memory, EF_SYSTEM_ADDR, &mm) || mm == 0)
    return EfCheckResult();  // not set up: ran = false
  u32 heap_start = 0;
  u32 heap_end = 0;
  if (!PointerOk(mm) || !ReadWord(memory, mm + EF_MM_HEAP_START, &heap_start) || !ReadWord(memory, mm + EF_MM_HEAP_END, &heap_end) ||
      heap_start >= heap_end || !PointerOk(heap_start) || !PointerOk(heap_end - 1))
  {
    EfWalker bad(memory, list_cap, 0, 0);
    bad.Fail("EffectSystem", EF_SYSTEM_ADDR, mm, Format("the manager 0x%08X has no usable heap (0x%08X-0x%08X)", mm, heap_start, heap_end));
    return bad.Result();
  }
  EfWalker w(memory, list_cap, heap_start, heap_end);
  EfCheckResult& r = w.Result();

  u32 groups = 0;
  u32 activity = 0;
  if (!ReadWord(memory, EF_SYSTEM_ADDR + EF_SYS_GROUPS, &groups) || !ReadWord(memory, EF_SYSTEM_ADDR + EF_SYS_ACTIVITY, &activity))
  {
    w.Fail("EffectSystem", EF_SYSTEM_ADDR, 0, "cannot read the activity list fields");
    return r;
  }
  if (groups > EF_CHECK_MAX_GROUPS || (groups != 0 && (activity < heap_start || static_cast<u64>(activity) + groups * EF_GROUP_STRIDE > heap_end)))
  {
    w.Fail("EffectSystem", EF_SYSTEM_ADDR, activity,
           Format("%u activity groups at 0x%08X (expected <= %u, inside the ef heap)", groups, activity, EF_CHECK_MAX_GROUPS));
    return r;
  }

  const auto visit_particle = [&r](u32) {
    ++r.objects[3];
    return true;
  };
  std::function<bool(u32)> visit_pm = [&](u32 pm) {
    ++r.objects[2];
    return w.Walk(Format("particles@pm=0x%08X", pm), pm + 0x38, EF_LIST_OFFSET, visit_particle) &&
           w.Walk(Format("particles_closing@pm=0x%08X", pm), pm + 0x44, EF_LIST_OFFSET, visit_particle);
  };
  std::function<bool(u32)> visit_emitter = [&](u32 emitter) {
    ++r.objects[1];
    return w.Walk(Format("pms@emitter=0x%08X", emitter), emitter + 0xC0, EF_LIST_OFFSET, visit_pm) &&
           w.Walk(Format("pms_closing@emitter=0x%08X", emitter), emitter + 0xCC, EF_LIST_OFFSET, visit_pm);
  };
  std::function<bool(u32)> visit_effect = [&](u32 effect) {
    ++r.objects[0];
    return w.Walk(Format("emitters@effect=0x%08X", effect), effect + 0x24, EF_LIST_OFFSET, visit_emitter) &&
           w.Walk(Format("emitters_closing@effect=0x%08X", effect), effect + 0x30, EF_LIST_OFFSET, visit_emitter);
  };
  for (u32 g = 0; g < groups; ++g)
  {
    const u32 list = activity + g * EF_GROUP_STRIDE;
    if (!w.Walk(Format("effects[g%u]", g), list, EF_LIST_OFFSET, visit_effect) ||
        !w.Walk(Format("effects_closing[g%u]", g), list + 0xC, EF_LIST_OFFSET, visit_effect))
    {
      return r;
    }
  }
  // The four object pools: free list at pool + 4, in-use list at pool + 0x10, links at offset 0 (zz_80189b40_ & co: List_Init(.., 0)).
  const auto nothing = [](u32) { return true; };
  for (size_t i = 0; i < 4; ++i)
  {
    u32 pool_obj = 0;
    if (!ReadWord(memory, mm + EF_MM_POOLS[i], &pool_obj) || pool_obj < heap_start || static_cast<u64>(pool_obj) + 0x24 > heap_end)
    {
      w.Fail(Format("pool[%s]", EF_KIND[i]), mm + EF_MM_POOLS[i], pool_obj, Format("the pool object 0x%08X is outside the ef heap", pool_obj));
      return r;
    }
    if (!w.Walk(Format("pool_free[%s]", EF_KIND[i]), pool_obj + 4, 0, nothing) ||
        !w.Walk(Format("pool_used[%s]", EF_KIND[i]), pool_obj + 0x10, 0, nothing))
    {
      return r;
    }
  }
  return r;
}

namespace
{
// "head=0x.. tail=0x.. n=.. off=0x.." of the ut::List at addr, "?" when unreadable.
std::string ListText(const GuestMemory& memory, u32 addr)
{
  u8 raw[12];
  if (!memory.Read(addr, raw, sizeof(raw)))
    return "?";
  return Format("head=0x%08X tail=0x%08X n=%u off=0x%X", ReadBE32(raw), ReadBE32(raw + 4), (raw[8] << 8) | raw[9], (raw[10] << 8) | raw[11]);
}
}  // namespace

std::vector<std::string> EfDumpLines(const GuestMemory& memory)
{
  std::vector<std::string> out;
  u32 mm = 0;
  if (!ReadWord(memory, EF_SYSTEM_ADDR, &mm))
  {
    out.push_back(Format("ef sys=0x%08X unreadable", EF_SYSTEM_ADDR));
    return out;
  }
  if (mm == 0)
  {
    out.push_back(Format("ef sys=0x%08X mm=0x00000000 (effect system not set up)", EF_SYSTEM_ADDR));
    return out;
  }
  u32 heap_start = 0;
  u32 heap_end = 0;
  u32 groups = 0;
  u32 activity = 0;
  std::array<u32, 4> counts{};
  ReadWord(memory, mm + EF_MM_HEAP_START, &heap_start);
  ReadWord(memory, mm + EF_MM_HEAP_END, &heap_end);
  ReadWord(memory, EF_SYSTEM_ADDR + EF_SYS_GROUPS, &groups);
  ReadWord(memory, EF_SYSTEM_ADDR + EF_SYS_ACTIVITY, &activity);
  for (size_t i = 0; i < 4; ++i)
    ReadWord(memory, mm + EF_MM_COUNTS[i], &counts[i]);
  out.push_back(Format("ef sys=0x%08X mm=0x%08X heap=0x%08X-0x%08X pool_sizes effect=%u emitter=%u pm=%u particle=%u groups=%u activity=0x%08X",
                       EF_SYSTEM_ADDR, mm, heap_start, heap_end, counts[0], counts[1], counts[2], counts[3], groups, activity));
  if (groups <= EF_CHECK_MAX_GROUPS && activity != 0)
  {
    for (u32 g = 0; g < groups && g < 8; ++g)
    {
      const u32 list = activity + g * EF_GROUP_STRIDE;
      out.push_back(Format("ef group%u effects %s | closing %s", g, ListText(memory, list).c_str(), ListText(memory, list + 0xC).c_str()));
    }
  }
  for (size_t i = 0; i < 4; ++i)
  {
    u32 pool_obj = 0;
    if (ReadWord(memory, mm + EF_MM_POOLS[i], &pool_obj) && pool_obj != 0)
    {
      out.push_back(Format("ef pool %s obj=0x%08X free %s | used %s", EF_KIND[i], pool_obj, ListText(memory, pool_obj + 4).c_str(),
                           ListText(memory, pool_obj + 0x10).c_str()));
    }
  }
  const EfCheckResult r = CheckEfLists(memory);
  if (!r.ran)
    out.push_back("ef check not run");
  else if (r.ok)
    out.push_back(Format("ef check ok lists=%u nodes=%u effects=%u emitters=%u pms=%u particles=%u", r.lists, r.nodes, r.objects[0], r.objects[1],
                         r.objects[2], r.objects[3]));
  else
    out.push_back(Format("ef check BAD list=%s list_addr=0x%08X node=0x%08X %s (lists=%u nodes=%u)", r.list.c_str(), r.list_addr, r.node,
                         r.why.c_str(), r.lists, r.nodes));
  return out;
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

  // A pointer straight into the emulated RAM, for hashing pages without a copy. Valid only while the range is inside MEM1 / MEM2.
  const u8* Peek(u32 addr, size_t n) const override
  {
    auto& memory = m_system.GetMemory();
    if (!Inside(memory, addr, n))
      return nullptr;
    return memory.GetPointerForRange(addr, n);
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
    sink.Line("NOTE hardware: not reachable: Wii IOS /dev/di read destinations (rollbacks wait while /dev/di is busy), ARAM DMA, GPU-thread texture copies");
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

  // A disc read is in flight when a GC DI transfer is running (DICR TSTART, 0x0C00601C and on a Wii 0x0D00601C) or the Wii IOS /dev/di
  // device has a command executing or queued (DIDevice::IsBusy; Rev3's BGM streaming goes through it and its destination, a 16 KB
  // buffer, is written from inside IOS HLE where the snapshot cannot cut it out).
  bool DiscReadInFlight() override
  {
    if (MMIO::Mapping* mmio = m_system.GetMemory().GetMMIOMapping(); mmio != nullptr)
    {
      if ((mmio->Read<u32>(m_system, 0x0C00601C) & 1) != 0)
        return true;
      if (m_system.IsWii() && (mmio->Read<u32>(m_system, 0x0D00601C) & 1) != 0)
        return true;
    }
    if (m_system.IsWii())
    {
      if (IOS::HLE::EmulationKernel* ios = m_system.GetIOS(); ios != nullptr)
      {
        const std::shared_ptr<IOS::HLE::Device> device = ios->GetDeviceByName("/dev/di");
        // "/dev/di" is always the DIDevice (IOS.cpp AddDevice); Dolphin builds with /GR- (no RTTI), so static_cast as
        // Boot_BS2Emu.cpp and DI.cpp do
        const auto* di = static_cast<const IOS::HLE::DIDevice*>(device.get());
        if (di != nullptr && di->IsBusy())
          return true;
      }
    }
    return false;
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

// The emulated audio hardware (REV3_SYNCTEST_AUDIO_HW, see Rev3Rollback.h "THE AUDIO HARDWARE"). What is saved, and how:
//   DSPManager::DoRewindState     DSP control / audio DMA / ARAM DMA registers + the DSP emulator's DoState (HLE: ucode CRCs, the
//                                 ucode's own state, mail handler; LLE: the DSP core). A new method next to DoState that leaves out
//                                 the ARAM contents (DoState writes 16 MB on a GameCube; on a Wii ARAM is MEM2, rewound as memory).
//   AudioInterfaceManager::DoRewindState   AI registers. AudioInterfaceManager::DoState also runs Mixer::DoState (the Wiimote speaker /
//                                 Skylander / GBA mixers too) and keeps m_last_cpu_time as an absolute tick; the new method leaves
//                                 the mixer alone (re-applying rate / volume only if they changed) and stores that time as an age.
//   CoreTiming events             the pending events of the types in AUDIO_EVENT_TYPES, as remaining cycles (CoreTimingManager::
//                                 GetPendingEvents, FindEventType added for this). A restore removes all pending events of those
//                                 types and schedules the saved ones again with their remaining cycles from now.
//   ProcessorInterface            only the INT_CAUSE_DSP and INT_CAUSE_AI bits (GetCause / SetInterrupt).
// Not touched: the rest of CoreTiming (VI, SI, EXI, IOS, DVD, GPU events), the PI mask, the host-side audio mixer buffers and the
// sound backend (so the speakers keep getting samples), the PPC registers (the game hook reloads its own).
// DSP LLE on its own thread is locked out (DSPEmulator::PauseAndLock) while the state is read or written; HLE has no lock to take.
class DolphinAudioHw final : public AudioHardware
{
public:
  explicit DolphinAudioHw(Core::System& system) : m_system(system)
  {
    auto& core_timing = m_system.GetCoreTiming();
    for (const char* name : AUDIO_EVENT_TYPES)
    {
      if (CoreTiming::EventType* type = core_timing.FindEventType(name); type != nullptr)
      {
        m_types.push_back(type);
        m_names.emplace_back(name);
      }
      else
      {
        m_names.push_back(std::string(name) + "(not registered)");
      }
    }
  }

  const char* DspKind() const override
  {
    const DSPEmulator* emulator = m_system.GetDSP().GetDSPEmulator();
    return emulator != nullptr && emulator->IsLLE() ? "LLE" : "HLE";
  }

  std::vector<std::string> EventTypeNames() const override { return m_names; }

  bool SaveState(std::vector<u8>* out) override
  {
    DSPEmulator* const emulator = m_system.GetDSP().GetDSPEmulator();
    if (emulator == nullptr)
      return false;
    emulator->PauseAndLock(true);
    u8* ptr = nullptr;
    PointerWrap measure(&ptr, 0, PointerWrap::Mode::Measure);
    DoAll(measure);
    const size_t size = reinterpret_cast<uintptr_t>(ptr);
    out->resize(size);
    ptr = out->data();
    PointerWrap writer(&ptr, size, PointerWrap::Mode::Write);
    DoAll(writer);
    const bool ok = writer.IsWriteMode() && ptr == out->data() + size;
    emulator->PauseAndLock(false);
    return ok;
  }

  bool LoadState(const std::vector<u8>& in) override
  {
    DSPEmulator* const emulator = m_system.GetDSP().GetDSPEmulator();
    if (emulator == nullptr)
      return false;
    emulator->PauseAndLock(true);
    // DoState takes a non-const buffer pointer in read mode too (it never writes to it).
    u8* ptr = const_cast<u8*>(in.data());
    PointerWrap reader(&ptr, in.size(), PointerWrap::Mode::Read);
    DoAll(reader);
    const bool ok = reader.IsReadMode() && ptr == in.data() + in.size();
    emulator->PauseAndLock(false);
    return ok;
  }

  void PendingEvents(std::vector<AudioEvent>* out) override
  {
    out->clear();
    auto& core_timing = m_system.GetCoreTiming();
    const s64 now = static_cast<s64>(core_timing.GetTicks());
    for (const CoreTiming::Event& event : core_timing.GetPendingEvents())
    {
      if (event.type == nullptr || std::find(m_types.begin(), m_types.end(), event.type) == m_types.end())
        continue;
      AudioEvent saved;
      saved.type = *event.type->name;
      saved.cycles = std::max<s64>(event.time - now, 0);
      saved.userdata = event.userdata;
      out->push_back(std::move(saved));
    }
  }

  void ReplaceEvents(const std::vector<AudioEvent>& events) override
  {
    auto& core_timing = m_system.GetCoreTiming();
    for (CoreTiming::EventType* type : m_types)
      core_timing.RemoveAllEvents(type);
    for (const AudioEvent& saved : events)
    {
      for (CoreTiming::EventType* type : m_types)
      {
        if (*type->name == saved.type)
        {
          core_timing.ScheduleEvent(saved.cycles, type, saved.userdata);
          break;
        }
      }
    }
  }

  u32 InterruptBits() override { return m_system.GetProcessorInterface().GetCause() & INT_BITS; }

  void SetInterruptBits(u32 bits) override
  {
    auto& pi = m_system.GetProcessorInterface();
    for (const u32 bit : {DSP_BIT, AI_BIT})
    {
      const bool want = (bits & bit) != 0;
      if (((pi.GetCause() & bit) != 0) != want)
        pi.SetInterrupt(bit, want);  // also re-evaluates the PPC external exception
    }
  }

private:
  static constexpr u32 DSP_BIT = static_cast<u32>(ProcessorInterface::INT_CAUSE_DSP);
  static constexpr u32 AI_BIT = static_cast<u32>(ProcessorInterface::INT_CAUSE_AI);
  static constexpr u32 INT_BITS = DSP_BIT | AI_BIT;
  // The event types of the DSP / AI devices (names as registered in SystemTimers.cpp, DSP.cpp, AudioInterface.cpp).
  static constexpr const char* AUDIO_EVENT_TYPES[] = {
      "DSPCallback",       // SystemTimers: gives the DSP emulator its time slices (periodic)
      "AudioDMACallback",  // SystemTimers: pushes one 32-byte audio DMA block to the speakers, raises the AID interrupt (periodic)
      "AICallback",        // AudioInterface: sample counter / AI interrupt timing
      "DSPint",            // DSP: delayed DSP / AID / ARAM interrupts (userdata = interrupt type)
      "ARAMint",           // DSP: ARAM DMA complete
  };

  void DoAll(PointerWrap& p)
  {
    m_system.GetDSP().DoRewindState(p);
    m_system.GetAudioInterface().DoRewindState(p);
  }

  Core::System& m_system;
  std::vector<CoreTiming::EventType*> m_types;
  std::vector<std::string> m_names;
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

// What the freeze dump reads from Dolphin (REV3_SYNCTEST_DUMP). Read-only. Safe to call from the CPU thread, from the watchdog thread
// (racy: the CPU thread keeps running, so values may be torn but nothing is written) and from the host thread at shutdown.
//   PPC state       system.GetPPCState(): pc, npc, gpr, LR / CTR / SRR0 / SRR1, msr, cr.Get(), Exceptions
//   PE              MMIO 0x0C00100A (control / interrupt status) and 0x0C00100E (token), plain 16-bit reads
//   CP              GetCommandProcessor().GetFifo() atomics (the CP status register read has side effects, so it is derived: breakpoint
//                   flag, RW distance 0 = idle)
//   PI              ProcessorInterfaceManager::GetCause / GetMask and the public CPU FIFO pointers
//   VI              MMIO 0x0C00202C (vertical beam position) and the display interrupt 0 register 0x0C002030 / 32 (16-bit halves)
//   audio (full)    DescribeDsp() and the pending audio CoreTiming events; not at shutdown (the DSP is already gone) nor when racy.
class DolphinDumpSource final : public DumpSource
{
public:
  explicit DolphinDumpSource(Core::System& system) : m_system(system) {}

  // The audio hardware object (created later, with SyncTest); without it the dump has the DSP line but not the CoreTiming events.
  void SetAudio(AudioHardware* audio) { m_audio = audio; }

  // Called before the final dump: HW::Shutdown has stopped the DSP before it destroys the EXI devices, so its registers are off limits.
  void MarkShutdown() { m_shutdown = true; }
  bool CpuRunning() const { return Core::GetState(m_system) == Core::State::Running; }

  bool ReadCpu(DumpCpu* out) override
  {
    const PowerPC::PowerPCState& ppc = m_system.GetPPCState();
    out->pc = ppc.pc;
    out->npc = ppc.npc;
    out->lr = LR(ppc);
    out->ctr = CTR(ppc);
    out->msr = ppc.msr.Hex;
    out->srr0 = SRR0(ppc);
    out->srr1 = SRR1(ppc);
    out->cr = ppc.cr.Get();
    out->exceptions = ppc.Exceptions;
    for (size_t i = 0; i < out->gpr.size(); ++i)
      out->gpr[i] = ppc.gpr[i];
    out->valid = true;
    return true;
  }

  bool ReadHw(DumpHw* out) override
  {
    if (MMIO::Mapping* mmio = m_system.GetMemory().GetMMIOMapping(); mmio != nullptr)
    {
      out->pe_status = mmio->Read<u16>(m_system, 0x0C00100A);
      out->pe_token = mmio->Read<u16>(m_system, 0x0C00100E);
      out->pe_ok = true;
      out->vi_line = mmio->Read<u16>(m_system, 0x0C00202C);
      out->vi_di0 = (static_cast<u32>(mmio->Read<u16>(m_system, 0x0C002030)) << 16) | mmio->Read<u16>(m_system, 0x0C002032);
      out->vi_ok = true;
    }
    {
      auto& fifo = m_system.GetCommandProcessor().GetFifo();
      out->cp_base = fifo.CPBase.load();
      out->cp_end = fifo.CPEnd.load();
      out->cp_rd = fifo.CPReadPointer.load();
      out->cp_wr = fifo.CPWritePointer.load();
      out->cp_dist = fifo.CPReadWriteDistance.load();
      out->cp_bp = fifo.CPBreakpoint.load();
      out->cp_breakpoint = fifo.bFF_Breakpoint.load() != 0;
      out->cp_gp_read = fifo.bFF_GPReadEnable.load() != 0;
      out->cp_bp_enable = fifo.bFF_BPEnable.load() != 0;
      out->cp_link = fifo.bFF_GPLinkEnable.load() != 0;
      out->cp_hi_wm = fifo.bFF_HiWatermark.load() != 0;
      out->cp_lo_wm = fifo.bFF_LoWatermark.load() != 0;
      out->cp_ok = true;
    }
    {
      auto& pi = m_system.GetProcessorInterface();
      out->pi_cause = pi.GetCause();
      out->pi_mask = pi.GetMask();
      out->pi_fifo_base = pi.m_fifo_cpu_base;
      out->pi_fifo_end = pi.m_fifo_cpu_end;
      out->pi_fifo_wptr = pi.m_fifo_cpu_write_pointer;
      out->pi_ok = true;
    }
    return true;
  }

  // The DVD interface registers (plain reads) and, when the CPU is not running, the IOS /dev/di busy flag (it walks the IOS device map,
  // which a running CPU thread may change). Rev3's BGM streaming reads the disc through /dev/di; a rollback during such a read is what
  // the disc read gate postpones.
  void ExtraLines(std::vector<std::string>* out, bool racy) override
  {
    MMIO::Mapping* mmio = m_system.GetMemory().GetMMIOMapping();
    std::string ios = "n/a";
    if (m_system.IsWii())
    {
      ios = "skipped(racy)";
      if (!racy)
      {
        IOS::HLE::EmulationKernel* kernel = m_system.GetIOS();
        const std::shared_ptr<IOS::HLE::Device> device = kernel != nullptr ? kernel->GetDeviceByName("/dev/di") : nullptr;
        const auto* di = static_cast<const IOS::HLE::DIDevice*>(device.get());  // always the DIDevice; no RTTI in this build
        ios = di != nullptr ? (di->IsBusy() ? "1" : "0") : "none";
      }
    }
    if (mmio == nullptr)
    {
      out->push_back(Format("hw di unavailable ios_di_busy=%s", ios.c_str()));
      return;
    }
    out->push_back(Format("hw di disr=0x%08X cmd0=0x%08X dimar=0x%08X dilength=0x%08X dicr=0x%08X wii_dicr=0x%08X ios_di_busy=%s",
                          mmio->Read<u32>(m_system, 0x0C006000), mmio->Read<u32>(m_system, 0x0C006008),
                          mmio->Read<u32>(m_system, 0x0C006014), mmio->Read<u32>(m_system, 0x0C006018),
                          mmio->Read<u32>(m_system, 0x0C00601C), m_system.IsWii() ? mmio->Read<u32>(m_system, 0x0D00601C) : 0u,
                          ios.c_str()));
  }

  void AudioLines(std::vector<std::string>* out) override
  {
    if (m_shutdown)
    {
      out->push_back("dsp skipped: the DSP is shut down before the EXI devices are");
      return;
    }
    out->push_back(DescribeDsp(m_system));
    if (m_audio == nullptr)
      return;
    std::vector<AudioEvent> events;
    m_audio->PendingEvents(&events);
    std::string list;
    for (const AudioEvent& e : events)
      list += Format("%s%s+%lld", list.empty() ? "" : ",", e.type.c_str(), static_cast<long long>(e.cycles));
    out->push_back(Format("audio dsp=%s irq=0x%02X events=%zu [%s]", m_audio->DspKind(), m_audio->InterruptBits(), events.size(),
                          list.c_str()));
  }

private:
  Core::System& m_system;
  AudioHardware* m_audio = nullptr;
  bool m_shutdown = false;
};

// The stall watchdog: a host-side thread that wakes every 250 ms and asks the FreezeDump whether the game has gone quiet while SyncTest
// is armed. Then it writes a racy dump at once (never blocks), and, if `pause` is set, tries to pause the CPU with Core::CPUThreadGuard
// for a second, consistent dump. The pause waits for the CPU thread to leave its run loop; if the freeze is a host-side wait that
// never returns, the pause never returns either, but the racy dump is already in the log. Stop() therefore waits only so long and then
// detaches the thread; the caller must then leak what the thread uses (it checks `stop` before touching anything after the pause).
class StallWatchdog
{
public:
  StallWatchdog(Core::System& system, FreezeDump& dump, bool pause) : m_shared(std::make_shared<Shared>())
  {
    std::shared_ptr<Shared> shared = m_shared;
    Core::System* const sys = &system;
    FreezeDump* const d = &dump;
    m_thread = std::thread([shared, sys, d, pause]() { Run(*shared, *sys, *d, pause); });
  }

  ~StallWatchdog()
  {
    if (m_thread.joinable())
      m_thread.detach();
  }

  // True: the thread ended and was joined. False: it is stuck in the pause, was detached, and still holds pointers into the module.
  bool Stop(std::chrono::milliseconds wait)
  {
    {
      std::lock_guard<std::mutex> lock(m_shared->mutex);
      m_shared->stop.store(true);
    }
    m_shared->cv.notify_all();
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    const bool done = m_shared->cv.wait_for(lock, wait, [this]() { return m_shared->finished; });
    lock.unlock();
    if (!m_thread.joinable())
      return done;
    if (done)
      m_thread.join();
    else
      m_thread.detach();
    return done;
  }

private:
  struct Shared
  {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool finished = false;  // under mutex
  };

  static void Run(Shared& shared, Core::System& system, FreezeDump& dump, bool pause)
  {
    constexpr std::chrono::milliseconds POLL(250);  // watchdog poll period
    while (true)
    {
      {
        std::unique_lock<std::mutex> lock(shared.mutex);
        shared.cv.wait_for(lock, POLL, [&shared]() { return shared.stop.load(); });
      }
      if (shared.stop.load())
        break;
      if (!dump.StallDue())
        continue;
      const std::string note = Format("stall_ms=%u", dump.StallMs());
      dump.Dump("stall", true, false, note);
      if (!pause || shared.stop.load())
        continue;
      Core::CPUThreadGuard guard(system);  // returns when the CPU thread is idle (or the emulation is not running)
      if (!shared.stop.load())
        dump.Dump("stall", false, true, note);
    }
    {
      std::lock_guard<std::mutex> lock(shared.mutex);
      shared.finished = true;
    }
    shared.cv.notify_all();
  }

  std::shared_ptr<Shared> m_shared;
  std::thread m_thread;
};

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

  // Called from the CPU thread and (freeze dump) from the stall watchdog thread: one line at a time.
  void Line(const std::string& text) override
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_file.is_open())
      m_file << text << std::endl;  // std::endl flushes: the log survives a crash of the emulator
    INFO_LOG_FMT(SLIPPI, "Rev3 {}", text);
  }

  void Osd(const std::string& text) override { OSD::AddMessage(text, OSD::Duration::VERY_LONG, OSD::Color::RED); }

private:
  std::mutex m_lock;
  std::ofstream m_file;
};

// A text file for the determinism kit (pad record, trace): appended to; Line() only buffers, Flush() writes through.
class FileLineWriter final : public LineWriter
{
public:
  explicit FileLineWriter(const std::string& path)
  {
    File::CreateFullPath(path);
    File::OpenFStream(m_file, path, std::ios_base::out | std::ios_base::app);
  }
  bool IsOpen() const { return m_file.is_open(); }
  void Line(const std::string& text) override
  {
    if (m_file.is_open())
      m_file << text << '\n';
  }
  void Flush() override
  {
    if (m_file.is_open())
      m_file.flush();
  }

private:
  std::ofstream m_file;
};

// What the game sees when SyncTest is off: the module is created on the first command and then only echoes.
struct Module
{
  bool initialized = false;
  std::unique_ptr<DolphinMemory> memory;
  std::unique_ptr<DolphinHardware> hardware;
  std::unique_ptr<DolphinAudioHw> audio;
  std::unique_ptr<FileSink> sink;
  std::unique_ptr<SyncTest> synctest;  // null = off
  std::unique_ptr<FileLineWriter> pad_record;
  std::unique_ptr<FileLineWriter> trace;
  std::unique_ptr<DeterminismKit> kit;  // null = none of REV3_PAD_RECORD / REV3_PAD_SCRIPT / REV3_TRACE is usable
  std::unique_ptr<DolphinDumpSource> dump_source;  // freeze dump (REV3_SYNCTEST_DUMP); null = off
  std::unique_ptr<FreezeDump> dump;
  std::unique_ptr<StallWatchdog> watchdog;
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

// The Rev3 log (default <User>/Logs/rev3_synctest.log, REV3_SYNCTEST_LOG): opened by whichever feature needs it first.
FileSink& EnsureSink(Module& module, std::string* path = nullptr)
{
  std::string log_path = GetEnv("REV3_SYNCTEST_LOG");
  if (log_path.empty())
    log_path = File::GetUserPath(D_LOGS_IDX) + "rev3_synctest.log";
  if (module.sink == nullptr)
    module.sink = std::make_unique<FileSink>(log_path);
  if (path != nullptr)
    *path = log_path;
  return *module.sink;
}

// Whether a file exists and has content (the pad record gets its header only when it is new).
bool FileHasContent(const std::string& path)
{
  return File::Exists(path) && File::GetSize(path) > 0;
}

// REV3_PAD_RECORD / REV3_PAD_SCRIPT / REV3_TRACE / REV3_TRACE_EVERY / REV3_TRACE_RANGES. A variable that is set but unusable is
// logged as an ERROR and that part stays off; the others carry on.
void InitializeKit(Module& module)
{
  const std::string record_path = GetEnv("REV3_PAD_RECORD");
  const std::string script_path = GetEnv("REV3_PAD_SCRIPT");
  const std::string trace_path = GetEnv("REV3_TRACE");
  if (record_path.empty() && script_path.empty() && trace_path.empty())
    return;
  FileSink& sink = EnsureSink(module);
  const auto fail = [&sink](const std::string& what) {
    sink.Line("ERROR " + what);
    ERROR_LOG_FMT(SLIPPI, "Rev3: {}", what);
    sink.Osd("Rev3: " + what);
  };

  DeterminismOptions options;
  if (!script_path.empty())
  {
    std::string text;
    std::string error;
    if (!ReadTextFile(script_path, &text))
      fail("REV3_PAD_SCRIPT: cannot read " + script_path + "; pad script off");
    else if (!ParsePadScript(text, &options.script, &error))
      fail("REV3_PAD_SCRIPT " + script_path + ": " + error + "; pad script off");
    else
      options.script_path = script_path;
  }
  if (!trace_path.empty())
  {
    options.trace_path = trace_path;
    const std::string every = GetEnv("REV3_TRACE_EVERY");
    long value = 0;
    if (!every.empty())
    {
      if (ParseInt(every, &value) && value >= 1 && value <= 1000000)
        options.trace_every = static_cast<u32>(value);
      else
        fail("REV3_TRACE_EVERY=" + every + " is not 1..1000000; using " + std::to_string(TRACE_EVERY_DEFAULT));
    }
    const std::string ranges_path = GetEnv("REV3_TRACE_RANGES");
    if (!ranges_path.empty())
    {
      std::string text;
      std::string error;
      if (!ReadTextFile(ranges_path, &text))
        fail("REV3_TRACE_RANGES: cannot read " + ranges_path + "; mem will be 0");
      else if (!ParseWhitelist(text, &options.ranges, &error))
        fail("REV3_TRACE_RANGES " + ranges_path + ": " + error + "; mem will be 0");
      else
        options.ranges_path = ranges_path;
    }
  }
  if (!record_path.empty())
  {
    options.record_header = !FileHasContent(record_path);
    module.pad_record = std::make_unique<FileLineWriter>(record_path);
    if (!module.pad_record->IsOpen())
    {
      fail("REV3_PAD_RECORD: cannot open " + record_path + "; pad record off");
      module.pad_record.reset();
    }
    else
    {
      options.record_path = record_path;
    }
  }
  if (!trace_path.empty())
  {
    module.trace = std::make_unique<FileLineWriter>(trace_path);
    if (!module.trace->IsOpen())
    {
      fail("REV3_TRACE: cannot open " + trace_path + "; trace off");
      module.trace.reset();
    }
  }
  if (module.pad_record == nullptr && module.trace == nullptr && options.script_path.empty())
    return;
  module.kit = std::make_unique<DeterminismKit>(std::move(options), *module.memory, sink, module.pad_record.get(), module.trace.get());
}

// REV3_SYNCTEST_DUMP (default 1; 0 = off), REV3_SYNCTEST_DUMP_STALL_MS (100..600000, default 2000: silence on the 0xA0 hook that counts as
// a stall), REV3_SYNCTEST_DUMP_PAUSE (default 1; 0 = the watchdog only writes the racy dump and never tries to pause the CPU).
// The dump lives in every mode (plain play, SyncTest armed or not, netplay), independent of SyncTest; `sink` is the log it writes to
// (the Rev3 log, or the net log in a netplay run).
void InitializeDump(Module& module, Core::System& system, Sink& sink)
{
  if (GetEnv("REV3_SYNCTEST_DUMP") == "0")
  {
    sink.Line("NOTE freeze dump off (REV3_SYNCTEST_DUMP=0)");
    return;
  }
  u32 stall_ms = 2000;
  const std::string stall_text = GetEnv("REV3_SYNCTEST_DUMP_STALL_MS");
  long value = 0;
  if (!stall_text.empty())
  {
    if (ParseInt(stall_text, &value) && value >= 100 && value <= 600000)
      stall_ms = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SYNCTEST_DUMP_STALL_MS={} (100..600000)", stall_text);
  }
  const bool pause = GetEnv("REV3_SYNCTEST_DUMP_PAUSE") != "0";
  module.dump_source = std::make_unique<DolphinDumpSource>(system);
  module.dump = std::make_unique<FreezeDump>(*module.memory, sink, *module.dump_source, stall_ms);
  module.watchdog = std::make_unique<StallWatchdog>(system, *module.dump, pause);
  sink.Line(Format("NOTE freeze dump on: at arm, when no hook command arrives for %u ms (any mode; not while the net session waits for the "
                   "peer) (pause_cpu=%d), at shutdown",
                   stall_ms, pause ? 1 : 0));
}

// Gives SyncTest the dump (RESTOREHW lines, the dump at arm) and the audio hardware object the dump reads events from.
void AttachDump(Module& module)
{
  if (module.dump == nullptr)
    return;
  if (module.synctest != nullptr)
    module.synctest->SetFreezeDump(module.dump.get());
  module.dump_source->SetAudio(module.audio.get());
}

void Initialize(Module& module, Core::System& system)
{
  module.initialized = true;

  // The determinism kit (pad record / script, trace) is independent of SyncTest and works with it off.
  module.memory = std::make_unique<DolphinMemory>(system);
  InitializeKit(module);
  InitializeDump(module, system, EnsureSink(module));  // the freeze dump works with SyncTest off too

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
  options.adaptive = GetEnv("REV3_SYNCTEST_ADAPTIVE") != "0";
  const std::string slices = GetEnv("REV3_SYNCTEST_SCAN_SLICES");
  if (!slices.empty())
  {
    if (ParseInt(slices, &value) && value >= 1 && value <= 4096)
      options.scan_slices = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SYNCTEST_SCAN_SLICES={} (1..4096)", slices);
  }
  const std::string learn = GetEnv("REV3_SYNCTEST_LEARN");
  if (!learn.empty())
  {
    if (ParseInt(learn, &value) && value >= 0 && value <= 1000000)
      options.learn_cycles = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SYNCTEST_LEARN={} (0..1000000)", learn);
  }
  options.invalidate_code = GetEnv("REV3_SYNCTEST_ICACHE") != "0";
  options.restore_timebase = GetEnv("REV3_SYNCTEST_TB") == "1";
  options.ef_pool = GetEnv("REV3_EF_POOL") != "0";
  // REV3_SNAPSHOT_PAD=<pages>: each learned page also pulls this many neighbouring 4 KB pages (each side) into the snapshot set.
  const std::string pad = GetEnv("REV3_SNAPSHOT_PAD");
  if (!pad.empty())
  {
    if (ParseInt(pad, &value) && value >= 0 && value <= 16)
      options.learn_pad_pages = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_SNAPSHOT_PAD={} (0..16)", pad);
  }
  // REV3_EFCHECK: unset = the first 200 restores, 0 = off, all = every restore, N = the first N restores.
  const std::string efcheck = GetEnv("REV3_EFCHECK");
  if (!efcheck.empty())
  {
    std::string lower = efcheck;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "all")
      options.ef_check_restores = EF_CHECK_ALL;
    else if (ParseInt(efcheck, &value) && value >= 0 && value < 1000000000)
      options.ef_check_restores = static_cast<u32>(value);
    else
      ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_EFCHECK={} (a number, 0 or all)", efcheck);
  }
  const bool hardware_exclude = GetEnv("REV3_SYNCTEST_HW_EXCLUDE") != "0";

  std::string log_path;
  EnsureSink(module, &log_path);
  std::vector<Range> verify_ignore;
  ParseVerifyEnv(&options, &verify_ignore, *module.sink);  // REV3_RB_VERIFY*: restore verify (off unless REV3_RB_VERIFY is set)

  const std::string whitelist_path = GetEnv("REV3_WHITELIST");
  options.whitelist_path = whitelist_path.empty() ? "(none)" : whitelist_path;
  std::string text;
  std::string error;
  std::vector<Range> ranges;
  // The whitelist is required without the adaptive set. With it, it is an optional seed: an unset variable is fine, a set but
  // unusable file is still an error.
  if (whitelist_path.empty())
    error = "REV3_WHITELIST is not set";
  else if (!ReadTextFile(whitelist_path, &text))
    error = "cannot read " + whitelist_path;
  else
    ParseWhitelist(text, &ranges, &error);
  const bool seed_missing = whitelist_path.empty();
  u64 hook_bytes = 0;
  if (!ranges.empty())
  {
    hook_bytes = SubtractRange(&ranges, HOOK_REGION_START, HOOK_REGION_END);
    if (ranges.empty() && !options.adaptive)
      error = "every range lies inside the hook region 0x80002400-0x80002A00";
  }
  if (options.adaptive && seed_missing)
  {
    error.clear();
    module.sink->Line("NOTE no REV3_WHITELIST: the snapshot set is learned in the match (adaptive)");
  }
  if (!error.empty())
  {
    const std::string message = "Rev3 SyncTest: whitelist unusable (" + error + "); SyncTest is off";
    module.sink->Line("ERROR " + message);
    ERROR_LOG_FMT(SLIPPI, "{}", message);
    module.sink->Osd(message);
    return;
  }

  std::vector<Range> user_exclusions;
  std::vector<PtrExclusion> ptr_exclusions;
  const std::string exclude_path = GetEnv("REV3_SYNCTEST_EXCLUDE");
  if (!exclude_path.empty())
  {
    std::string exclude_text;
    if (!ReadTextFile(exclude_path, &exclude_text))
      error = "cannot read REV3_SYNCTEST_EXCLUDE " + exclude_path;
    else
      ApplyExclusions(exclude_text, &ranges, *module.sink, &error, &user_exclusions, !options.adaptive,
                       &ptr_exclusions);
    if (!error.empty())
    {
      const std::string message = "Rev3 SyncTest: exclude file unusable (" + error + "); SyncTest is off";
      module.sink->Line("ERROR " + message);
      ERROR_LOG_FMT(SLIPPI, "{}", message);
      module.sink->Osd(message);
      return;
    }
  }

  // REV3_SNAPSHOT_INCLUDE: regions forced into the snapshot at arm (same entries as the exclude file). Same failure policy as the exclude file.
  std::vector<Range> include_ranges;
  std::vector<PtrExclusion> include_ptrs;
  const std::string include_path = GetEnv("REV3_SNAPSHOT_INCLUDE");
  if (!include_path.empty())
  {
    std::string include_text;
    if (!ReadTextFile(include_path, &include_text))
      error = "cannot read REV3_SNAPSHOT_INCLUDE " + include_path;
    else
      LoadInclusions(include_text, *module.sink, &include_ranges, &include_ptrs, &error);
    if (!error.empty())
    {
      const std::string message = "Rev3 SyncTest: include file unusable (" + error + "); SyncTest is off";
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
  if (options.learn_pad_pages != 0)
  {
    module.sink->Line(Format("NOTE learn pad pages=%u (REV3_SNAPSHOT_PAD): every learned page also takes %u KB on each side into the snapshot set",
                             options.learn_pad_pages, options.learn_pad_pages * PAGE_SIZE / 1024));
  }
  if (hardware_exclude)
    module.hardware = std::make_unique<DolphinHardware>(system, *module.sink);
  module.synctest = std::make_unique<SyncTest>(options, std::move(ranges), *module.memory, *module.sink, std::function<u64()>(),
                                               module.hardware.get());
  module.synctest->SetUserExclusions(std::move(user_exclusions), std::move(ptr_exclusions));
  module.synctest->SetUserInclusions(std::move(include_ranges), std::move(include_ptrs));
  module.synctest->SetVerifyIgnore(std::move(verify_ignore));
  if (options.restore_timebase)
  {
    module.sink->Line("NOTE timebase accessor: read SystemTimers::GetFakeTimeBase(); write CoreTiming::SetFakeTBStartValue(start + (value - now)) "
                      "and PowerPCManager::WriteFullTimeBaseValue(value); decrementer and CoreTiming events untouched");
  }
  if (GetEnv("REV3_SYNCTEST_AUDIO_HW") == "0")
  {
    module.sink->Line("NOTE audio hw rewind off (REV3_SYNCTEST_AUDIO_HW=0): memory only");
  }
  else
  {
    module.audio = std::make_unique<DolphinAudioHw>(system);
    module.synctest->SetAudioHardware(module.audio.get());
  }
  Core::System* const sys = &system;
  module.synctest->SetDspState([sys]() { return DescribeDsp(*sys); });
  AttachDump(module);
  module.sink->Osd(Format("Rev3 SyncTest on: K=%d, every %d frame(s), log %s", options.k, options.every, log_path.c_str()));
}
}  // namespace

// REV3_RB_VERIFY=N|all (verified restores; unset / 0 = off), REV3_RB_VERIFY_EVERY=P (0..1000000 frames, 0 = automatic), REV3_RB_VERIFY_SLOTS=S
// (1..8 shadows of 88 MB), REV3_RB_VERIFY_HEAL=1, REV3_RB_VERIFY_STACK_BELOW=bytes (0x100..0x400000), REV3_RB_VERIFY_IGNORE=file.
void ParseVerifyEnv(SyncTestOptions* options, std::vector<Range>* ignore, Sink& sink)
{
  const auto fail = [&sink](const std::string& what) {
    sink.Line("ERROR " + what);
    ERROR_LOG_FMT(SLIPPI, "Rev3: {}", what);
  };
  long value = 0;
  const std::string restores = GetEnv("REV3_RB_VERIFY");
  if (!restores.empty())
  {
    std::string lower = restores;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "all")
      options->verify.restores = VERIFY_ALL;
    else if (ParseInt(restores, &value) && value >= 0 && value < 1000000000)
      options->verify.restores = static_cast<u32>(value);
    else
      fail("REV3_RB_VERIFY=" + restores + " is not a number or all; restore verify is off");
  }
  if (options->verify.restores == 0)
    return;  // the other variables mean nothing without it
  const std::string every = GetEnv("REV3_RB_VERIFY_EVERY");
  if (!every.empty())
  {
    if (ParseInt(every, &value) && value >= 0 && value <= 1000000)
      options->verify.every = static_cast<u32>(value);
    else
      fail("REV3_RB_VERIFY_EVERY=" + every + " is not 0..1000000; using the automatic value");
  }
  const std::string slots = GetEnv("REV3_RB_VERIFY_SLOTS");
  if (!slots.empty())
  {
    if (ParseInt(slots, &value) && value >= 1 && value <= 8)
      options->verify.slots = static_cast<u32>(value);
    else
      fail("REV3_RB_VERIFY_SLOTS=" + slots + " is not 1..8; using 1");
  }
  options->verify.heal = GetEnv("REV3_RB_VERIFY_HEAL") == "1";
  const std::string below = GetEnv("REV3_RB_VERIFY_STACK_BELOW");
  if (!below.empty())
  {
    if (ParseInt(below, &value) && value >= 0x100 && value <= 0x400000)
      options->verify.stack_below = static_cast<u32>(value);
    else
      fail("REV3_RB_VERIFY_STACK_BELOW=" + below + " is not 0x100..0x400000; using 0x4000");
  }
  const std::string ignore_path = GetEnv("REV3_RB_VERIFY_IGNORE");
  if (!ignore_path.empty() && ignore != nullptr)
  {
    std::string text;
    std::string error;
    if (!ReadTextFile(ignore_path, &text))
      fail("REV3_RB_VERIFY_IGNORE: cannot read " + ignore_path + "; nothing is ignored");
    else if (!ParseWhitelist(text, ignore, &error))
      fail("REV3_RB_VERIFY_IGNORE " + ignore_path + ": " + error + "; nothing is ignored");
    else
      sink.Line(Format("NOTE verify ignores %zu range(s) from %s", ignore->size(), ignore_path.c_str()));
  }
}

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

  // The freeze dump's watchdog: this command proves the game is alive (any mode).
  if (module.dump != nullptr)
    module.dump->Tick(parsed.frame);

  // The determinism kit sees every hook call first: it may replace the pads (REV3_PAD_SCRIPT), then SyncTest (if on) works with
  // those pads, and the reply carries them.
  if (module.kit != nullptr && Core::IsCPUThread())
    module.kit->OnCommand(&parsed, module.synctest != nullptr && module.synctest->Resimulating());

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
  // Freeze dump: stop the watchdog first (it may be waiting to pause the CPU), then dump once more while the guest memory is still
  // mapped (HW::Shutdown destroys the EXI devices before Memory::Shutdown; the DSP is already gone, the PPC state is still readable).
  bool watchdog_stuck = false;
  if (module.watchdog != nullptr)
  {
    watchdog_stuck = !module.watchdog->Stop(std::chrono::milliseconds(1500));
    module.watchdog.reset();
  }
  if (module.dump != nullptr && module.dump_source != nullptr)
  {
    module.dump_source->MarkShutdown();
    module.dump->Dump("shutdown", module.dump_source->CpuRunning(), false);
  }
  if (module.kit != nullptr)
    module.kit->Finish();
  module.kit.reset();  // before the memory it reads
  module.pad_record.reset();
  module.trace.reset();
  if (module.synctest != nullptr)
    module.synctest->Finish();
  module.synctest.reset();
  if (watchdog_stuck)
  {
    // The detached watchdog thread still points at these (it only touches them after the CPU pause returns, and then checks its stop
    // flag first). Leak them instead of freeing them under it.
    static_cast<void>(module.dump.release());
    static_cast<void>(module.dump_source.release());
    static_cast<void>(module.memory.release());
    static_cast<void>(module.sink.release());
  }
  module.dump.reset();
  module.dump_source.reset();
  module.audio.reset();
  module.hardware.reset();
  module.memory.reset();
  module.sink.reset();
  module.initialized = false;  // the next command reads the environment again and appends to the log
}
}  // namespace Rev3
