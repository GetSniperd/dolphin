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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <picojson.h>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/SoundStream.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/Host.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/IOS/DI/DI.h"
#include "Core/IOS/IOS.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Slippi/Rev3Net.h"
#include "Core/Slippi/Rev3WriteTrack.h"
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
constexpr u32 ALARM_BAD_LOGS = 12;                // ALARMCHECK bad lines written in full (each with the queue); later failures are only counted
constexpr u32 ALARM_OK_LOGS = 1;                  // ALARMCHECK ok lines in all (one, at the first capture): a log that shows the check ran

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
  if (!IsCommand(data, size) || (data[1] != PROTOCOL_VERSION && data[1] != PROTOCOL_VERSION_HIDE))
    return false;
  out->protocol = data[1];
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

// ---- (1b) hiding resimulated frames, hook protocol 3 (M6) --------------------------------------------------------------------

bool ParseResimHide(const std::string& text, u32* flags)
{
  *flags = HIDE_DEFAULT;
  if (text.empty())
    return true;
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "0" || lower == "off")
    *flags = 0;
  else if (lower == "1" || lower == "draw")
    *flags = HIDE_DRAW;
  else if (lower == "2" || lower == "sound")
    *flags = HIDE_SOUND;
  else if (lower == "3" || lower == "all")
    *flags = HIDE_ALL;
  else
    return false;
  return true;
}

ResimHideFlags::ResimHideFlags(GuestMemory& memory, u32 flags, Sink* log, u32 always)
    : m_memory(memory), m_log(log), m_flags(flags & HIDE_ALL), m_always(always & DET_VOICE)
{
}

// The most VOICE lines one run writes: a hit needs the bit set while a voice is live, a few per round, plus the resimulated repeats.
constexpr u32 MAX_VOICE_LINES = 400;

void ResimHideFlags::SetProtocol(u8 protocol)
{
  if (protocol != m_protocol)
  {
    m_protocol = protocol;
    m_known = false;  // another hook: nobody knows what the word holds
    if (m_protocol == PROTOCOL_VERSION_HIDE && m_always != 0)
      Set(m_hidden);  // the always-on bits go in now, before the first voice of the match (Set writes hidden ? flags : 0, plus them)
  }
  // Voice gate hits (the game found the bit set with a live voice slot): one line per change of the counter, as seen at the next command. The hit happens in the
  // logic of frame N, after N's command, and `last` is the hook counter then = N + 1 = the frame whose START state holds the bit (a DESYNC first_diff frame).
  u32 hits = 0;
  u32 last = 0;
  if (m_protocol == PROTOCOL_VERSION_HIDE && m_log != nullptr && ReadVoiceGate(&hits, &last) && hits != m_voice_hits_seen)
  {
    m_voice_hits_seen = hits;
    if (m_voice_lines < MAX_VOICE_LINES)
    {
      ++m_voice_lines;
      m_log->Line(Format("VOICE gate: a voice start found move_flags2 bit 0x10000 set with a live voice slot at frame %u (hits=%u; %s)", last, hits,
                         (m_always & DET_VOICE) != 0 ? "det_voice on: treated as clear, same on both machines" : "det_voice off: Rev3 drops the voice if the slot is live"));
    }
  }
}

void ResimHideFlags::Set(bool hidden)
{
  m_hidden = hidden;
  if (m_protocol != PROTOCOL_VERSION_HIDE)
  {
    if (hidden && m_flags != 0 && !m_noted_v2 && m_log != nullptr)
    {
      m_noted_v2 = true;
      m_log->Line(Format("NOTE resim hiding: the hook speaks protocol %u, not 3, so REV3_RESIM_HIDE=%u is not written (regenerate the INI with rbhook --protocol 3)",
                         m_protocol, m_flags));
    }
    return;
  }
  const u32 value = (hidden ? m_flags : 0) | m_always;
  if (m_known && m_word == value)
    return;
  u8 raw[4];
  WriteBE32(raw, value);
  if (!m_memory.Write(HIDE_FLAGS_ADDR, raw, sizeof(raw)))
    return;
  m_known = true;
  m_word = value;
  ++m_writes;
  if (value != 0 && (value & HIDE_ALL) != 0 && !m_noted_on)
  {
    m_noted_on = true;
    if (m_log != nullptr)
    {
      m_log->Line(Format("NOTE resim hiding v3: flags=%u (%s%s%s) written to 0x%08X while a resimulation runs", value & HIDE_ALL,
                         (value & HIDE_DRAW) != 0 ? "draw" : "", (value & HIDE_ALL) == HIDE_ALL ? "+" : "", (value & HIDE_SOUND) != 0 ? "sound" : "",
                         HIDE_FLAGS_ADDR));
    }
  }
  if ((value & DET_VOICE) != 0 && !m_noted_det)
  {
    m_noted_det = true;
    if (m_log != nullptr)
      m_log->Line(Format("NOTE det_voice on: FLAGS bit 2 written to 0x%08X for the whole session (the voice start no longer depends on the sound slot table)", HIDE_FLAGS_ADDR));
  }
}

bool ResimHideFlags::ReadCounters(u32* hidden_iterations, u32* skipped_sounds) const
{
  if (m_protocol != PROTOCOL_VERSION_HIDE)
    return false;
  u8 a[4];
  u8 b[4];
  if (!m_memory.Read(HIDE_ITERATIONS_ADDR, a, sizeof(a)) || !m_memory.Read(HIDE_SOUNDS_ADDR, b, sizeof(b)))
    return false;
  *hidden_iterations = ReadBE32(a);
  *skipped_sounds = ReadBE32(b);
  return true;
}

bool ResimHideFlags::ReadVoiceGate(u32* hits, u32* last_frame) const
{
  if (m_protocol != PROTOCOL_VERSION_HIDE)
    return false;
  u8 a[4];
  u8 b[4];
  if (!m_memory.Read(HIDE_VOICE_HITS_ADDR, a, sizeof(a)) || !m_memory.Read(HIDE_VOICE_LAST_ADDR, b, sizeof(b)))
    return false;
  *hits = ReadBE32(a);
  *last_frame = ReadBE32(b);
  return true;
}

std::string ResimHideFlags::SummaryFields() const
{
  u32 iterations = 0;
  u32 sounds = 0;
  if (!ReadCounters(&iterations, &sounds))
    return Format(" hide_protocol=%u hide_flags=%u hide_writes=%u", m_protocol, m_flags, m_writes);
  u32 hits = 0;
  u32 last = 0;
  ReadVoiceGate(&hits, &last);
  return Format(" hide_protocol=%u hide_flags=%u hide_writes=%u hidden_iterations=%u skipped_sounds=%u det_voice=%u voice_gate_hits=%u voice_gate_last=%u", m_protocol,
                m_flags, m_writes, iterations, sounds, (m_always & DET_VOICE) != 0 ? 1u : 0u, hits, last);
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

// ---- (3d) memory hash check ---------------------------------------------------------------------------------------------------
// MemoryHasher / DiffHashes / RegionHashes (declared in Rev3Rollback.h, "memory hash check"): the page-hash comparison of ALL guest memory
// behind REV3_RB_HASHCHECK and the first level of the netplay desync diff.

namespace
{
// Sets mark[i] = 1 for every tracked page that overlaps one of `ranges` (a range is clipped to MEM1 and MEM2; a byte takes its whole page).
void MarkPages(const std::vector<Range>& ranges, std::vector<u8>* mark)
{
  mark->assign(TRACKED_PAGES, 0);
  for (const Range& r : ranges)
  {
    if (r.end <= r.start)
      continue;
    const std::pair<u32, u32> regions[2] = {{MEM1_BASE, MEM1_END}, {MEM2_BASE, MEM2_END}};
    for (const auto& region : regions)
    {
      const u32 lo = std::max(r.start, region.first);
      const u32 hi = std::min(r.end, region.second);
      if (hi <= lo)
        continue;
      const size_t first = PageIndex(lo);
      const size_t last = PageIndex(hi - 1);
      for (size_t i = first; i <= last && i < TRACKED_PAGES; ++i)
        (*mark)[i] = 1;
    }
  }
}
}  // namespace

MemoryHasher::MemoryHasher(GuestMemory& memory, std::function<u64()> micros)
    : m_memory(memory), m_micros(std::move(micros)), m_buf(PAGE_SIZE)
{
  if (!m_micros)
    m_micros = SteadyMicros;
}

void MemoryHasher::Hash(const std::vector<Range>& skip, MemoryHashes* out)
{
  const u64 start = m_micros();
  out->page.resize(TRACKED_PAGES);
  out->valid.assign(TRACKED_PAGES, 0);
  std::vector<u8> skipped;
  MarkPages(skip, &skipped);
  out->hashed = 0;
  out->skipped = 0;
  for (size_t i = 0; i < TRACKED_PAGES; ++i)
  {
    if (skipped[i] != 0)
    {
      ++out->skipped;
      continue;
    }
    const u32 addr = PageAddress(i);
    const u8* page = m_memory.Peek(addr, PAGE_SIZE);
    if (page == nullptr)
    {
      if (!m_memory.Read(addr, m_buf.data(), PAGE_SIZE))
        continue;  // not backed by memory (e.g. MEM2 of a GameCube): neither hashed nor counted as skipped
      page = m_buf.data();
    }
    out->page[i] = HashPage(page);
    out->valid[i] = 1;
    ++out->hashed;
  }
  out->us = m_micros() - start;
}

std::vector<u32> DiffHashes(const MemoryHashes& a, const MemoryHashes& b, u32* compared)
{
  std::vector<u32> differing;
  u32 both = 0;
  if (!a.Empty() && !b.Empty() && a.page.size() == TRACKED_PAGES && b.page.size() == TRACKED_PAGES)
  {
    for (size_t i = 0; i < TRACKED_PAGES; ++i)
    {
      if (a.valid[i] == 0 || b.valid[i] == 0)
        continue;
      ++both;
      if (a.page[i] != b.page[i])
        differing.push_back(PageAddress(i));
    }
  }
  if (compared != nullptr)
    *compared = both;
  return differing;
}

std::vector<u64> RegionHashes(const MemoryHashes& hashes)
{
  std::vector<u64> regions(HASHCHECK_REGIONS, 0);
  if (hashes.Empty() || hashes.page.size() != TRACKED_PAGES)
    return regions;
  constexpr size_t PAGES_PER_REGION = HASHCHECK_REGION_BYTES / PAGE_SIZE;
  for (size_t r = 0; r < HASHCHECK_REGIONS; ++r)
  {
    u64 h = 0x9E3779B97F4A7C15ull;
    bool any = false;
    for (size_t k = 0; k < PAGES_PER_REGION; ++k)
    {
      const size_t i = r * PAGES_PER_REGION + k;
      if (hashes.valid[i] == 0)
        continue;  // the position is mixed in below, so a skipped page does not shift the others
      any = true;
      h = (Rotl64(h, 23) ^ hashes.page[i] ^ (static_cast<u64>(k) << 56)) * 0xC2B2AE3D27D4EB4Full;
    }
    regions[r] = any ? (h | 1) : 0;  // 0 is reserved for "no page hashed"
  }
  return regions;
}

std::vector<u32> DiffRegions(const std::vector<u64>& a, const std::vector<u64>& b)
{
  std::vector<u32> differing;
  const size_t n = std::min(a.size(), b.size());
  for (size_t r = 0; r < n; ++r)
  {
    if (a[r] != 0 && b[r] != 0 && a[r] != b[r])
      differing.push_back(static_cast<u32>(r));
  }
  return differing;
}

u32 RegionBase(size_t index)
{
  return PageAddress(index * (HASHCHECK_REGION_BYTES / PAGE_SIZE));
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

  // Memory hash check (REV3_RB_HASHCHECK): page hashes of all of guest memory every Nth frame, first run against resimulation.
  if (m_options.hash_check_every != 0)
  {
    m_hasher = std::make_unique<MemoryHasher>(memory, m_micros);
    m_sink.Line(Format("NOTE hash check on: every %u frames, all of MEM1 + MEM2 in %zu pages of 4 KB (about 9 ms per hashed frame); frames that "
                       "differ are named in HASHCHECK lines",
                       m_options.hash_check_every, static_cast<size_t>(TRACKED_PAGES)));
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

bool SyncTest::CollectExclusions()
{
  if (m_hardware == nullptr && m_ptr_exclusions.empty())
    return false;
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
  if (!layout_changed && cuts_added)
    WatchRefresh();  // the new cuts take pages away from the watch too (a layout change refreshes it after the rebuild)
  return layout_changed;
}

void SyncTest::RefreshExclusions(bool at_arm)
{
  if (!CollectExclusions())
    return;
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
  // Was a disc read in flight at this moment? A slot captured with one is never a rollback target (see DVD_EXECUTING_ADDR).
  {
    const bool hw = m_hardware != nullptr && m_hardware->DiscReadInFlight();
    const bool game = GameDvdBusy();
    // IOS IPC events (CoreTiming IPCEvent / IPCInterrupt / FinishDICommand) still on their way: a reply that lands after a restore of this
    // slot. Counted always (ipc_slots); a rollback target only with options.ios_gate (REV3_RB_IOS_GATE).
    std::string ipc_text;
    const u32 ipc = IosPending(&ipc_text);
    if (ipc != 0)
      ++m_stats.ipc_slots;
    const bool ios = ipc != 0 && m_options.ios_gate;
    slot.io_dirty = hw || game || ios;
    if (slot.io_dirty)
    {
      ++m_stats.io_dirty_slots;
      m_stats.io_dirty_hw += hw ? 1 : 0;
      m_stats.io_dirty_game += game ? 1 : 0;
      if (m_stats.io_dirty_slots <= MAX_IO_POSTPONE_LOGS)
        m_sink.Line(Format("NOTE slot of frame %u captured with a disc read in flight (hardware=%d game_dvd=%d): not a rollback target%s", command.frame, hw ? 1 : 0,
                           game ? 1 : 0, ios ? Format(" ios_events=%s", ipc_text.c_str()).c_str() : ""));
    }
  }
  // The OS alarm queue at this moment (the live state, rolled back or not): the first bad frame places the damage between two captures.
  AlarmWatch(command.frame);
  // Restore verify: a full copy of guest memory at this very moment, for the frames a restore may land on (a failed copy is logged by the
  // verifier and only means this frame is not verified).
  if (m_verify != nullptr && m_verify->WantsShadow(command.frame))
    m_verify->TakeShadow(command.frame);
  HashMemory(slot, command);

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

  // Classic policy (SyncTest): the first 10 restores log a begin and a done line, up to 30 a short one. Netplay policy
  // (options.restore_log_full): that many result lines, then one summary per window of frames.
  const bool net_policy = m_options.restore_log_full != 0;
  const bool log_it = m_restore_logs < (net_policy ? m_options.restore_log_full : MAX_RESTORE_LOGS);
  const bool log_short = !net_policy && !log_it && m_restore_logs < MAX_SHORT_RESTORE_LOGS;
  if (log_it && m_restore_logs < MAX_RESTORE_LOGS)
    m_sink.Line(Format("RESTORE %u -> %u begin", command.frame, to));
  // Freeze dump: the hardware and the software flags that wait on it, right before the write-back (the first 10 restores).
  const bool dump_hw = log_it && m_restore_logs < MAX_RESTORE_LOGS && m_dump != nullptr;
  if (dump_hw)
    m_sink.Line(m_dump->RestoreHwLine(command.frame, to, false));

  // IOS replies still on their way across this restore (the hypothesis for a double-queued alarm; see REV3.md "Alarm queue").
  m_alarm_restore_from = command.frame;
  m_alarm_restore_to = to;
  m_alarm_restore_ipc.clear();
  if (IosPending(&m_alarm_restore_ipc) != 0)
  {
    ++m_stats.ipc_restores;
    if (m_stats.ipc_restores <= MAX_IO_POSTPONE_LOGS)
      m_sink.Line(Format("NOTE restore %u -> %u with IOS events pending: %s (%u so far)", command.frame, to, m_alarm_restore_ipc.c_str(),
                         m_stats.ipc_restores));
  }
  // The alarm queue as the game left it, just before the write-back.
  AlarmCheck("before", command.frame, to);

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
  if (dump_hw)
    m_sink.Line(m_dump->RestoreHwLine(command.frame, to, true));

  // The ef lists after the write-back: the same checker, this time on the rewound state (the one the game resumes from).
  EfCheck("after", command.frame, to);
  // The alarm queue after the write-back (and the time base / audio restore): the state the game resumes from.
  AlarmCheck("after", command.frame, to);
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
  else if (net_policy)
  {
    NoteRestoreWindow(command.frame, us, icache_us);
  }
  return true;
}

void SyncTest::NoteRestoreWindow(u32 frame, u64 us, u64 icache_us)
{
  const u32 window = static_cast<u32>(std::max<s32>(1, m_options.restore_log_window));
  if (!m_rwin.begun)
  {
    m_rwin.begun = true;
    m_rwin.start = frame;
  }
  else if (frame >= m_rwin.start + window)
  {
    FlushRestoreWindow();
    m_rwin.begun = true;
    m_rwin.start = frame;
  }
  m_rwin.last = frame;
  ++m_rwin.n;
  m_rwin.us_total += us;
  m_rwin.us_max = std::max(m_rwin.us_max, us);
  m_rwin.icache_max = std::max(m_rwin.icache_max, icache_us);
}

void SyncTest::FlushRestoreWindow()
{
  if (m_rwin.n != 0)
  {
    m_sink.Line(Format("RESTORE summary frames=%u-%u n=%u us_mean=%.1f us_max=%llu icache_us_max=%llu", m_rwin.start, m_rwin.last, m_rwin.n,
                       static_cast<double>(m_rwin.us_total) / static_cast<double>(m_rwin.n), static_cast<unsigned long long>(m_rwin.us_max),
                       static_cast<unsigned long long>(m_rwin.icache_max)));
  }
  m_rwin = RestoreWindow{};
}

void SyncTest::NetFlushLog()
{
  FlushRestoreWindow();
}

bool SyncTest::NetRangeCrcs(u32 frame, bool live, std::vector<RangeHash>* out) const
{
  if (m_ranges.empty())
    return false;
  out->clear();
  if (NetHas(frame))
  {
    const Slot& slot = m_ring[frame % m_ring.size()];
    size_t offset = 0;
    for (const Range& r : m_ranges)
    {
      const size_t len = r.end - r.start;
      if (offset + len > slot.data.size())
        return false;
      RangeHash e;
      e.start = r.start;
      e.end = r.end;
      e.crc = Common::ComputeCRC32(slot.data.data() + offset, len);
      out->push_back(e);
      offset += len;
    }
    return true;
  }
  if (!live)
    return false;
  for (const Range& r : m_ranges)
  {
    RangeHash e;
    e.start = r.start;
    e.end = r.end;
    e.crc = MemoryCrc(m_memory, {r});
    out->push_back(e);
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
bool SyncTest::GameDvdBusy() const
{
  u8 raw[4];
  if (!m_memory.Read(DVD_EXECUTING_ADDR, raw, sizeof(raw)))
    return false;
  const u32 executing = ReadBE32(raw);
  return executing != 0 && executing != DVD_DUMMY_BLOCK && PointerOk(executing);
}

bool SyncTest::DiscBusy()
{
  return (m_hardware != nullptr && m_hardware->DiscReadInFlight()) || GameDvdBusy() || (m_options.ios_gate && IosPending(nullptr) != 0);
}

u32 SyncTest::IosPending(std::string* what) const
{
  if (m_hardware == nullptr)
    return 0;
  return m_hardware->IosEventsPending(what);
}

// ---- the alarm queue watch ---------------------------------------------------------------------------------------------------------
// Rev3: the SDK's OSAlarm queue is one doubly linked list (head 0x8049DA38, tail 0x8049DA3C); a cycle makes InsertAlarm spin for ever with
// interrupts off (soak 20261016_233913 run 7). AlarmWatch looks at the live state at each capture, AlarmCheck around each restore, so the
// log says WHEN the queue went bad: in the first run (capture bad), between the last good capture and a restore (before bad, with
// last_ok_frame), or by the restore itself (before ok, after bad: the write-back, TB / DEC left alone, or IOS replies that crossed it).
void SyncTest::AlarmWatch(u32 frame)
{
  if (m_options.alarm_check == 0)
    return;
  const AlarmQueueReport r = ReadAlarmQueue(m_memory);
  ++m_stats.alarm_checks;
  if (r.Ok())
  {
    if (m_alarm_was_bad)
      m_sink.Line(Format("ALARMCHECK ok again at capture of frame %u (was bad since after frame %u)", frame, m_alarm_last_ok_frame));
    m_alarm_was_bad = false;
    m_alarm_last_ok_frame = frame;
    if (m_alarm_ok_logs < ALARM_OK_LOGS)
      AlarmLog(Format("ok capture frame %u", frame).c_str(), r);
    return;
  }
  ++m_stats.alarm_bad_captures;
  if (!m_alarm_was_bad)
  {
    // Only the transition is logged (a cyclic queue stays cyclic): the frame, the last good frame, the latest restore and its IOS events.
    AlarmLog(Format("bad capture frame %u last_ok_frame=%u last_restore=%u->%u ios=[%s]", frame, m_alarm_last_ok_frame, m_alarm_restore_from,
                    m_alarm_restore_to, m_alarm_restore_ipc.c_str())
                 .c_str(),
             r);
  }
  m_alarm_was_bad = true;
}

void SyncTest::AlarmCheck(const char* when, u32 from, u32 to)
{
  if (m_options.alarm_check == 0)
    return;
  const bool after = std::strcmp(when, "after") == 0;
  const AlarmQueueReport r = ReadAlarmQueue(m_memory);
  ++m_stats.alarm_checks;
  if (r.Ok())
    return;  // no log line: the line after "RESTOREHW .. after" must stay the RESTORE result line; the count is in SUMMARY alarm_checks
  if (after)
    ++m_stats.alarm_bad_after;
  else
    ++m_stats.alarm_bad_before;
  AlarmLog(Format("bad %s restore %u -> %u last_ok_frame=%u ios=[%s]", when, from, to, m_alarm_last_ok_frame, m_alarm_restore_ipc.c_str()).c_str(), r);
}

// One header line `ALARMCHECK <what> status=<Status()>` and, for a bad queue, the `alarmq` lines of the walk (as a freeze dump prints them).
// Bad results: the first ALARM_BAD_LOGS are written, then one line says the rest is only counted. Good results: ALARM_OK_LOGS lines.
void SyncTest::AlarmLog(const char* what, const AlarmQueueReport& r)
{
  if (r.Ok())
  {
    ++m_alarm_ok_logs;
    m_sink.Line(Format("ALARMCHECK %s status=%s n=%zu", what, r.Status().c_str(), r.nodes.size()));
    return;
  }
  ++m_alarm_bad_logs;
  if (m_alarm_bad_logs > ALARM_BAD_LOGS)
  {
    if (m_alarm_bad_logs == ALARM_BAD_LOGS + 1)
      m_sink.Line("ALARMCHECK further failures are only counted (SUMMARY alarm_bad_captures / alarm_bad_before / alarm_bad_after)");
    return;
  }
  m_sink.Line(Format("ALARMCHECK %s status=%s", what, r.Status().c_str()));
  std::optional<s64> now;
  u64 tb = 0;
  if (m_memory.ReadTimeBase(&tb))
    now = AlarmSystemTime(m_memory, tb);
  u32 tps = 0;
  u8 bus[4];
  if (m_memory.Read(OS_BUS_CLOCK, bus, sizeof(bus)))
    tps = ReadBE32(bus) / 4;
  for (const std::string& line : AlarmQueueLines(r, now, tps, {}))
    m_sink.Line("ALARMCHECK " + line);
}

std::string SyncTest::AlarmStatsFields() const
{
  return Format(" alarm_checks=%u alarm_bad_captures=%u alarm_bad_before=%u alarm_bad_after=%u ipc_slots=%u ipc_restores=%u", m_stats.alarm_checks,
                m_stats.alarm_bad_captures, m_stats.alarm_bad_before, m_stats.alarm_bad_after, m_stats.ipc_slots, m_stats.ipc_restores);
}

bool SyncTest::NetClean(u32 frame) const
{
  const Slot& s = m_ring[frame % m_ring.size()];
  return !(s.valid && s.frame == frame && s.io_dirty);
}

std::string SyncTest::IoStatsFields() const
{
  return Format(" io_dirty_slots=%u io_dirty_hw=%u io_dirty_game=%u io_dirty_skipped=%u", m_stats.io_dirty_slots, m_stats.io_dirty_hw, m_stats.io_dirty_game,
                m_stats.io_dirty_skipped);
}

bool SyncTest::DiscReadPostpones(u32 frame)
{
  if (!DiscBusy())
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
  if (due && SlotFor(n - k).io_dirty)
  {
    // The slot was captured with a disc read in flight: SyncTest restores exactly K frames back and has no earlier slot to take instead.
    ++m_stats.io_dirty_skipped;
    return Echo(command);
  }
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
  HashCheck(reference, m_scratch, m);
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
              IoStatsFields() + AlarmStatsFields() + VerifyStatsFields() + HashCheckStatsFields() +
              (m_hide != nullptr ? m_hide->SummaryFields() : std::string()));
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
  if (m_hide != nullptr)
  {
    m_hide_on = false;
    m_hide->Set(false);  // never leave the game hiding
  }
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

void SyncTest::SetWriteWatch(WriteWatch* watch)
{
  m_watch = watch;
  if (watch != nullptr && m_options.write_track != 0)
  {
    m_ranges = ComputeRanges();  // the set is page-aligned from now on
    RebuildLayout();
  }
}

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
    if (m_net_mode)
    {
      // The session owns the slots (it would find them gone): ask for its local relayout drain, which installs the set and clears them.
      if (m_net_active && !m_net_pending)
      {
        m_net_pending = true;
        m_sink.Line(Format("NOTE write tracking restarted while slots are in use (frame %u): the history is cleared at the next relayout", m_last_frame));
      }
    }
    else if (m_armed)
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
    else if (!(m_armed || (m_net_mode && m_net_active)))
    {
      // Before arming there is no history: Arm() (netplay: NetPrepareArm) computes the set from the tracker's dynamic pages.
      m_watch->Release(learn);
    }
    else
    {
      const bool had_history = m_net_mode ? m_net_active : m_have_history;
      const bool resim = m_net_mode ? m_net_resim : m_resim;
      const std::vector<Range> before = m_ranges;
      const bool kept = had_history && GrowKeepingHistory(ranges, dirty);
      if (!kept && m_net_mode && m_net_pending)
      {
        // The drain that installs the set is already asked for; the pages stay dirty (their preimages stay valid) until it has run. Pages that
        // appear meanwhile are marked dynamic above, so that drain installs them too.
      }
      else if (!kept && resim)
      {
        // Cannot extend the slots and must not drop them in the middle of a resimulation: try again after it (the pages stay dirty and
        // dynamic; their preimages stay valid until released).
        ++m_stats.wt_deferred;
      }
      else if (!kept && m_net_mode)
      {
        // Netplay fallback: the slots cannot be extended (a page without baseline). The slots are the session's: do not drop them here, ask for
        // its local relayout drain (NetLayoutPending -> GATE relayout), which installs the set (NetInstallSet names and counts the pages) and
        // clears them. m_ranges stays as it is; the learned pages stay dirty (preimages valid) until the drain has run.
        m_net_pending = true;
        ++m_stats.wt_grown_cleared;
        m_sink.Line(Format("NOTE learned +%llu pages while slots are in use (frame %u): the slots cannot be extended, the snapshot set grows at the "
                           "next relayout; new=[%s]",
                           static_cast<unsigned long long>(RangesPages(NewRanges(ranges, before))), m_last_frame,
                           FormatRangeList(NewRanges(ranges, before), MAX_LEARN_LIST_RANGES).c_str()));
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
          // A growth that kept the history also settles a drain that only a failed growth had asked for.
          if (m_net_mode && m_net_pending && SameRanges(ComputeRanges(), m_ranges))
            m_net_pending = false;
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

std::string SyncTest::WriteTrackFields() const
{
  if (m_options.write_track == 0)
    return {};
  return Format(" wt_grown_kept=%u wt_grown_cleared=%u wt_deferred=%u wt_restarts=%u wt_misses=%u", m_stats.wt_grown_kept, m_stats.wt_grown_cleared,
                m_stats.wt_deferred, m_stats.wt_restarts, m_stats.wt_misses);
}

std::string SyncTest::NetWriteTrackText() const
{
  if (m_options.write_track == 0)
    return "off";
  return Format("%s learned=%u deferred=%u restarts=%u misses=%u", m_watching ? "armed" : "idle", m_stats.wt_pages_learned, m_stats.wt_deferred,
                m_stats.wt_restarts, m_stats.wt_misses);
}

std::string SyncTest::SnapshotLine() const
{
  const char* kind = (m_watching || m_stats.wt_arms != 0) ? "write_track" : (m_options.adaptive ? "adaptive" : "classic");
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

// ---- memory hash check: the SyncTest side ----

// Hashes all of guest memory into slot.hashes when this frame is one of the every-Nth. Skipped (never compared, because the restore or the
// hardware deliberately leaves them alone, or they are known to differ between two visits of the same frame): the hook region, the
// REV3_SYNCTEST_EXCLUDE ranges, the hardware cuts, REV3_RB_VERIFY_IGNORE and the dead part of the hook thread's stack (VerifyDeadStack).
void SyncTest::HashMemory(Slot& slot, const Command& command)
{
  if (m_hasher == nullptr)
    return;
  if (command.frame % m_options.hash_check_every != 0)
  {
    slot.hashes.hashed = 0;  // a slot of the ring is reused: whatever it held is not this frame's
    return;
  }
  std::vector<Range> skip;
  skip.push_back({HOOK_REGION_START, HOOK_REGION_END});
  skip.insert(skip.end(), m_user_exclusions.begin(), m_user_exclusions.end());
  for (const Exclusion& e : m_exclusions_logged)
    skip.push_back({e.start, e.end});
  skip.insert(skip.end(), m_verify_ignore.begin(), m_verify_ignore.end());
  const Range dead = VerifyDeadStack(m_memory, command.sp, m_options.verify.stack_below != 0 ? m_options.verify.stack_below : 0x4000);
  if (dead.end > dead.start)
    skip.push_back(dead);
  m_hasher->Hash(skip, &slot.hashes);
  slot.hashes.frame = command.frame;
  m_stats.hc_us_total += slot.hashes.us;
}

// Frame m was resimulated: compare the page hashes of the first run (`reference`) with those of the resimulation (`live`).
void SyncTest::HashCheck(const Slot& reference, const Slot& live, u32 frame)
{
  if (m_hasher == nullptr || reference.hashes.Empty() || live.hashes.Empty() || reference.hashes.frame != frame || live.hashes.frame != frame)
    return;
  u32 compared = 0;
  const std::vector<u32> differing = DiffHashes(reference.hashes, live.hashes, &compared);
  ++m_stats.hc_compared;
  if (differing.empty())
  {
    m_stats.hc_last_equal_frame = frame;
    if (m_hc_ok_logs < HASHCHECK_MAX_OK_LOGS)
    {
      ++m_hc_ok_logs;
      m_sink.Line(Format("HASHCHECK frame=%u ok compared=%u skipped=%u us=%llu", frame, compared, live.hashes.skipped,
                         static_cast<unsigned long long>(live.hashes.us)));
    }
    return;
  }

  ++m_stats.hc_differ;
  m_stats.hc_pages_total += differing.size();
  if (m_stats.hc_first_diff_frame == 0)
  {
    m_stats.hc_first_diff_frame = frame;
    m_sink.Osd(Format("Rev3 hash check: memory differs at frame %u (%zu pages), see the log", frame, differing.size()));
  }
  if (m_hc_diff_logs >= HASHCHECK_MAX_DIFF_LOGS)
    return;
  ++m_hc_diff_logs;

  // in_set: the page overlaps the snapshot set or the stack window (it was rewound, so a difference is nondeterminism inside the set);
  // outside_set: nothing rewound it (a page the set misses, or memory the game does not reproduce by itself).
  std::vector<Range> set = m_ranges;
  if (live.stack_len != 0)
    set.push_back({live.sp, live.sp + live.stack_len});
  std::vector<u8> in_set_mark;
  MarkPages(set, &in_set_mark);
  size_t in_set = 0;
  std::vector<std::pair<u32, u32>> regions;  // region base, differing pages in it
  std::string pages;
  size_t listed = 0;
  size_t i = 0;
  while (i < differing.size())
  {
    size_t j = i;
    while (j + 1 < differing.size() && differing[j + 1] == differing[j] + PAGE_SIZE)
      ++j;
    if (listed++ < HASHCHECK_MAX_RANGES_LISTED)
      pages += Format("%s0x%08X-0x%08X", pages.empty() ? "" : ",", differing[i], differing[j] + PAGE_SIZE);
    i = j + 1;
  }
  if (listed > HASHCHECK_MAX_RANGES_LISTED)
    pages += Format(",+%zu more", listed - HASHCHECK_MAX_RANGES_LISTED);
  for (const u32 addr : differing)
  {
    in_set += in_set_mark[PageIndex(addr)] != 0 ? 1 : 0;
    const u32 base = addr & ~(HASHCHECK_REGION_BYTES - 1);
    if (regions.empty() || regions.back().first != base)
      regions.push_back({base, 0});
    ++regions.back().second;
  }
  std::string region_text;
  for (size_t r = 0; r < regions.size(); ++r)
  {
    if (r == HASHCHECK_MAX_REGIONS_LISTED)
    {
      region_text += Format(",+%zu more", regions.size() - HASHCHECK_MAX_REGIONS_LISTED);
      break;
    }
    region_text += Format("%s0x%08X:%u", r == 0 ? "" : ",", regions[r].first, regions[r].second);
  }
  m_sink.Line(Format("HASHCHECK frame=%u DIFF pages=%zu (in_set=%zu outside_set=%zu) compared=%u regions64k=%zu [%s] ranges=[%s] "
                     "last_equal_frame=%u",
                     frame, differing.size(), in_set, differing.size() - in_set, compared, regions.size(), region_text.c_str(), pages.c_str(),
                     m_stats.hc_last_equal_frame));
}

std::string SyncTest::HashCheckStatsFields() const
{
  if (m_hasher == nullptr)
    return {};
  return Format(" hashcheck_compared=%u hashcheck_differ=%u hashcheck_last_equal_frame=%u hashcheck_first_diff_frame=%u hashcheck_pages=%llu "
                "hashcheck_ms=%.1f",
                m_stats.hc_compared, m_stats.hc_differ, m_stats.hc_last_equal_frame, m_stats.hc_first_diff_frame,
                static_cast<unsigned long long>(m_stats.hc_pages_total), static_cast<double>(m_stats.hc_us_total) / 1000.0);
}

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
        if (m_net_mode && !m_net_pending)
        {
          // Netplay: slots are in use, so the session must drain and relayout (NetLayoutPending); NetInstallSet names the pages then.
          m_net_pending = true;
          m_sink.Line(Format("NOTE restore verify heal found pages missing from the snapshot set (frame %u): they join at the next arm", command.frame));
        }
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

// One page-tracker slice per normal hook call (SyncTest::OnCommand and NetObserve): timed for the HB / SUMMARY lines; logs once when the
// LEARN scan cycles are done. Returns the pages that just became dynamic.
u32 SyncTest::ScanSlice(u32 frame)
{
  // Host write tracking (REV3_RB_WRITE_TRACK): mode 1 finds the written pages through faults (PollWrites), so the slice only advances the
  // learning clock; mode 2 also scans and every page the scan finds that no fault announced is a miss.
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
                           "protected): [%s]", frame, static_cast<unsigned long long>(pages), FormatRangeList(missed, 8).c_str()));
    }
  }
  if (!m_learn_done && m_tracker.Cycles() >= m_options.learn_cycles)
  {
    m_learn_done = true;
    m_sink.Line(Format("NOTE learning complete: %u scan cycles (slices=%u) dynamic_pages=%zu (%zu bytes) at frame %u", m_tracker.Cycles(),
                       m_tracker.Slices(), m_tracker.DynamicPages(), m_tracker.DynamicPages() * PAGE_SIZE, frame));
  }
  return added;
}

Reply SyncTest::OnCommand(const Command& command)
{
  if (m_hide != nullptr)
    m_hide->SetProtocol(command.protocol);
  const Reply reply = OnCommandImpl(command);
  SyncHideFlags();  // the RESTORED reply starts the hiding, the reply of the last resimulated frame (or a reset) ends it
  return reply;
}

// REV3_SYNCTEST_HIDE: the flags follow m_resim, written once per change (m_resim is false again when the history is cleared or the
// resimulation ends, so a SyncTest that resets in the middle of one never leaves the game hiding).
void SyncTest::SyncHideFlags()
{
  if (m_hide == nullptr || m_hide_on == m_resim)
    return;
  m_hide_on = m_resim;
  m_hide->Set(m_resim);
}

Reply SyncTest::OnCommandImpl(const Command& command)
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
    const u32 added = ScanSlice(command.frame);
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
    if (m_watching)
    {
      const WriteWatchStats ws = m_watch->Stats();
      line += Format(" wt_faults=%llu wt_pages=%u wt_poll_us_max=%llu", static_cast<unsigned long long>(ws.faults), m_stats.wt_pages_learned,
                     static_cast<unsigned long long>(m_stats.wt_poll_us_max));
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

// ---- network mode (milestone M3) -----------------------------------------------------------------------------------------
// The snapshot machinery of SyncTest for Rev3::Net::RollbackSession (see the NetObserve block in Rev3Rollback.h). Capture / Restore /
// RestoreAudio / the hardware cuts are the functions above, unchanged; what is new is only when the set (m_ranges) may change: never
// while slots are in use.

void SyncTest::NetObserve(const Command& command, bool resimulating)
{
  m_net_mode = true;
  m_net_resim = resimulating;
  m_net_cmd = command;
  m_last_sp = command.sp;
  m_last_frame = command.frame;
  std::array<u32, 2> fighters{};
  m_net_found = FindFighters(&fighters);
  if (!m_net_found)
  {
    WatchStop("not gameplay");  // loading rewrites memory without our knowing it: nothing learned from faults carries over
    // Loading or a menu: a new match starts from scratch (loading rewrites most of memory), like SyncTest::OnCommand. Its first set is built, not
    // learned (the fighters and the pools sit elsewhere in the new match).
    m_streak = 0;
    m_set_sp = 0;
    m_net_armed_once = false;
    if (m_options.adaptive)
    {
      m_tracker.Reset();
      m_learn_done = false;
    }
    return;
  }
  m_fighters = fighters;
  if (m_streak < 0xFFFFFFFFu)
    ++m_streak;
  // Host write tracking (REV3_RB_WRITE_TRACK): the watch starts as soon as both fighters are found (the first set is learned by it too), and every
  // call, resimulated ones included, looks at the pages written since the previous one BEFORE the session captures anything: a page joins the
  // set (the slots in use are extended with their history) before the next capture. Mode 1 replaces the hash scan below, mode 2 keeps it.
  if (m_options.write_track != 0 && m_watch != nullptr && !m_watching && !m_watch_failed && !m_net_left && !resimulating)
    WatchStart("");
  PollWrites();
  if (resimulating)
    return;  // replayed frames are never scanned (the first run's frames are the reference)

  if (m_options.adaptive)
  {
    const u32 added = ScanSlice(command.frame);
    if (added != 0 && m_net_active)
    {
      m_stats.pages_learned_armed += added;
      if (!m_net_pending)
      {
        const std::vector<Range> due = ComputeRanges();
        if (!SameRanges(due, m_ranges))
        {
          m_net_pending = true;
          // new=[..]: the pages named here (tools/rollback/learned_include.py reads it); NetInstallSet names every page again when the set is
          // installed, including those learned after this line.
          m_sink.Line(Format("NOTE learned +%u pages while slots are in use (frame %u): the snapshot set grows at the next arm; new=[%s]", added,
                             command.frame, FormatRangeList(NewRanges(due, m_ranges), MAX_LEARN_LIST_RANGES).c_str()));
        }
      }
    }
  }
  // Same pace as SyncTest (every hardware_check_every calls while armed); a cut that takes something from the set is noted, installed later.
  if (m_net_active && ++m_net_calls % m_options.hardware_check_every == 0 && !m_net_pending && CollectExclusions())
  {
    m_net_pending = true;
    m_sink.Line(Format("NOTE hardware cut changes the snapshot set (frame %u): installed at the next arm", command.frame));
  }
}

bool SyncTest::NetPrepareArm()
{
  if (!m_net_found)
    return false;
  m_set_sp = RegionEnd(m_net_cmd.sp) != 0 ? m_net_cmd.sp : 0;
  CollectExclusions();  // new cuts join m_exclusions_logged; the set is rebuilt with all of them right below
  ResolveInclusions();  // the ef pool and REV3_SNAPSHOT_INCLUDE: in the set whatever the page tracker has seen (same as the SyncTest arm)
  NetInstallSet(ComputeRanges(), m_net_cmd.frame, "at arm");
  RebuildLayout();
  ClearHistory();
  m_net_pending = false;
  m_net_active = false;
  m_net_calls = 0;
  WatchRefresh();  // the set holds the forced ranges and the stack window now, the cuts are known: release what is covered, guard the rest

  size_t worst = FIELD_COUNT;
  for (size_t p = 0; p < 2; ++p)
  {
    size_t inside = 0;
    for (const FieldDef& f : FIELDS)
      inside += !RangeContaining(m_fighters[p] + f.offset).empty();
    worst = std::min(worst, inside);
  }
  m_sink.Line(Format("NOTE net snapshot set: ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu dynamic_pages=%zu scan_cycles=%u", m_ranges.size(),
                     m_ranges_bytes, m_snapshot_bytes, m_ring.size(), m_tracker.DynamicPages(), m_tracker.Cycles()));
  m_sink.Line(Format("NOTE fighters p1=0x%08X p2=0x%08X", m_fighters[0], m_fighters[1]));
  LogFieldCoverage(m_fighters, nullptr);
  if (worst != FIELD_COUNT)
  {
    Error(Format("frame %u: not all fighter fields are inside the snapshot set (an exclusion cuts a fighter struct); not arming",
                 m_net_cmd.frame));
    return false;
  }
  if (m_dsp_state)
    m_sink.Line("NOTE dsp at arm: " + m_dsp_state());
  if (m_dump != nullptr)
    m_dump->Dump("arm", false, true, Format("frame=%u net", m_net_cmd.frame));
  return true;
}

// The first set of a match is built (the page tracker's learning phase, the pools, the fighters), not learned. From then on every page that
// joins is a late-learned page, whether the tracker found it while slots were in use, a hardware cut moved, or the restore verify heal found
// it missing: it goes to the all-time list (NoteLearned) and is named in one line that tools/rollback/learned_include.py parses.
void SyncTest::NetInstallSet(std::vector<Range> next, u32 frame, const char* when)
{
  if (m_net_armed_once)
  {
    const std::vector<Range> fresh = NoteLearned(m_ranges, next);
    m_learned_unlogged_ranges.clear();  // SyncTest's rate limited line is not used here; the list would only grow
    if (!fresh.empty())
    {
      // The part of `fresh` that the restore verify heal added (fresh minus (fresh minus m_verify_extra)).
      const u64 healed = RangesPages(NewRanges(fresh, NewRanges(fresh, m_verify_extra)));
      m_stats.verify_pages_added += static_cast<u32>(healed);
      m_sink.Line(Format("NOTE learned +%llu pages %s (frame %u): new=[%s]%s", static_cast<unsigned long long>(RangesPages(fresh)), when, frame,
                         FormatRangeList(fresh, MAX_LEARN_LIST_RANGES).c_str(),
                         healed != 0 ? Format(" (%llu of them found by the restore verify heal)", static_cast<unsigned long long>(healed)).c_str() : ""));
    }
  }
  m_ranges = std::move(next);
  m_net_armed_once = true;
}

void SyncTest::NetClear()
{
  std::vector<Range> due = ComputeRanges();
  if (!SameRanges(due, m_ranges))
  {
    NetInstallSet(std::move(due), m_net_cmd.frame, "at clear");
    RebuildLayout();
    m_sink.Line(Format("NOTE net snapshot set now ranges=%zu bytes=%zu snapshot_bytes=%zu slots=%zu", m_ranges.size(), m_ranges_bytes,
                       m_snapshot_bytes, m_ring.size()));
  }
  ClearHistory();
  m_net_pending = false;
  m_net_active = false;
  m_net_calls = 0;
  m_net_restore_check = false;
  WatchRefresh();  // the pages the tracker learned while slots were in use (or that stayed dirty after a failed growth) are in the set now
}

// The session ended (left, or the emulator is stopping): write tracking stops for good. Writable again at once, the fault hook stays installed
// until the module goes (it passes unknown faults on). The hash scan learns from here on, like without tracking.
void SyncTest::NetLeave()
{
  m_net_left = true;
  WatchStop("left the session");
}

bool SyncTest::NetCapture(u32 frame, u32 sp, NetSnapshotInfo* info)
{
  if (!m_net_found)
  {
    Error(Format("frame %u: capture without both fighters", frame));
    return false;
  }
  Command command = m_net_cmd;
  command.frame = frame;
  command.sp = sp;
  Slot& slot = SlotFor(frame);
  // REV3_RB_HASHCHECK, local part: the first call of a resimulation (the restore target) must start in the memory the first run had at that
  // frame, so the hashes of the first run's capture are kept across the recapture and compared with the new ones. Later resimulated frames
  // legitimately differ (the corrected pads), and the other peer's hashes are not exchanged (REV3.md "Desync region diff").
  const bool hash_compare = m_hasher != nullptr && m_net_restore_check && frame == m_net_restore_target;
  Slot reference;
  if (m_net_restore_check && frame == m_net_restore_target)
    m_net_restore_check = false;
  if (hash_compare && slot.valid && slot.frame == frame)
    reference.hashes = std::move(slot.hashes);
  if (!Capture(slot, command, m_fighters))
  {
    slot.valid = false;
    return false;
  }
  if (hash_compare)
    HashCheck(reference, slot, frame);
  m_net_active = true;
  info->fields = slot.fields;
  return true;
}

bool SyncTest::NetHas(u32 frame) const
{
  const Slot& s = m_ring[frame % m_ring.size()];
  return s.valid && s.frame == frame;
}

u32 SyncTest::NetSlotSp(u32 frame) const
{
  return NetHas(frame) ? m_ring[frame % m_ring.size()].sp : 0;
}

bool SyncTest::NetRestore(u32 frame)
{
  // The set only changes at NetPrepareArm / NetClear (the layout-generation guard in Restore refuses a slot from an older layout). A restore
  // while m_net_pending is set rewinds a set that is already known to be missing pages the game has since written: unsound for those pages
  // (the netplay twin of SyncTest's late-learn rollback), so it is counted and shows in the stats line.
  if (m_net_pending)
    ++m_stats.late_learn_rollbacks;
  m_net_restore_check = true;
  m_net_restore_target = frame;
  return Restore(SlotFor(frame), m_net_cmd, frame);
}

std::string SyncTest::NetStatsLine() const
{
  const auto mean = [](u64 total, u64 n) { return n != 0 ? static_cast<double>(total) / static_cast<double>(n) : 0.0; };
  return Format("NOTE snapshot stats restores=%u restore_us_mean=%.1f restore_us_max=%llu icache_us_mean=%.1f icache_us_max=%llu "
                "audio_restores=%u audio_us_mean=%.1f audio_us_max=%llu io_postponed=%u pages_learned_while_in_use=%u sp_mismatches=%u "
                "scans=%llu scan_us_mean=%.1f scan_us_max=%llu ef_checks=%u ef_bad=%u ef_bad_before=%u layout_refused=%u "
                "restores_with_pending_set=%u",
                m_stats.restores, mean(m_stats.restore_us_total, m_stats.restores), static_cast<unsigned long long>(m_stats.restore_us_max),
                mean(m_stats.icache_us_total, m_stats.restores), static_cast<unsigned long long>(m_stats.icache_us_max),
                m_stats.audio_restores, mean(m_stats.audio_us_total, m_stats.audio_restores),
                static_cast<unsigned long long>(m_stats.audio_us_max), m_stats.io_postponed, m_stats.pages_learned_armed,
                m_stats.sp_mismatches, static_cast<unsigned long long>(m_stats.scans), mean(m_stats.scan_us_total, m_stats.scans),
                static_cast<unsigned long long>(m_stats.scan_us_max), m_stats.ef_checks, m_stats.ef_bad, m_stats.ef_bad_before,
                m_stats.layout_refused, m_stats.late_learn_rollbacks) +
         IoStatsFields() + AlarmStatsFields() + VerifyStatsFields() + HashCheckStatsFields() + WriteTrackFields();
}

std::string SyncTest::NetSummaryNote()
{
  LearnedSummaryLine();  // only when the learned pages changed since the last such line
  std::string text = IoStatsFields() + AlarmStatsFields() + VerifyStatsFields() + HashCheckStatsFields() + WriteTrackFields();
  if (m_verify != nullptr)
    text += Format(" verify_pages_added=%u", m_stats.verify_pages_added);
  if (m_hide_counters != nullptr)
    text += m_hide_counters->SummaryFields();
  return text;
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

Bot::GameObs ReadGameObs(const GuestMemory& memory)
{
  Bot::GameObs obs;
  std::array<u32, 2> fighters{};
  obs.fighters_found = FindFighterStructs(memory, &fighters) == 2;
  if (obs.fighters_found)
  {
    for (const u32 fighter : fighters)
    {
      u8 word[4];
      if (memory.Read(fighter + Bot::FIGHTER_HEALTH_OFFSET, word, 4) && ReadBE32(word) == 0)
        obs.health_zero = true;
    }
  }
  u8 type[4];
  u8 sub[4];
  if (memory.Read(Bot::SCENE_TYPE_ADDR, type, 4) && memory.Read(Bot::SCENE_SUB_ADDR, sub, 4))
  {
    obs.scene_valid = true;
    obs.scene_type = ReadBE32(type);
    obs.scene_sub = ReadBE32(sub);
  }
  return obs;
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

size_t ReadFighterFields(const GuestMemory& memory, std::array<std::array<u8, FIELD_BYTES>, 2>* raw, u8* present)
{
  std::array<u32, 2> fighters{};
  FindFighterStructs(memory, &fighters);
  std::vector<u8> span(FIELD_SPAN_HI - FIELD_SPAN_LO);
  size_t found = 0;
  *present = 0;
  for (size_t p = 0; p < 2; ++p)
  {
    (*raw)[p].fill(0);
    if (fighters[p] == 0 || !memory.Read(fighters[p] + FIELD_SPAN_LO, span.data(), span.size()))
      continue;
    for (size_t f = 0; f < FIELD_COUNT; ++f)
      std::copy_n(span.data() + (FIELDS[f].offset - FIELD_SPAN_LO), 4, (*raw)[p].data() + 4 * f);
    *present = static_cast<u8>(*present | (1u << p));
    ++found;
  }
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

  if (m_bot != nullptr)
  {
    Bot::BotPad bot_pad;
    const bool script_active = scripted && frame <= m_options.script.last;
    if (m_bot->Choose(frame, script_active, ReadGameObs(m_memory), &bot_pad))
    {
      command->pads[0] = bot_pad;
      command->pad_return = std::numeric_limits<s32>::min();  // 0x80000000 = port 0 has data (D's pad record: PADRead's usual return)
    }
  }

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

  if (m_record != nullptr && !m_options.record_reply)
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

void DeterminismKit::OnReply(const Command& command, const Reply& reply, bool resimulating)
{
  if (m_record == nullptr || !m_options.record_reply || resimulating || (reply.flags & REPLY_FLAG_RESTORED) != 0)
    return;
  m_record->Line(FormatPadRecordLine(command.frame, reply.pads, command.pad_return));
  ++m_record_lines;
  if (++m_record_pending >= PAD_RECORD_FLUSH_EVERY)
  {
    m_record->Flush();
    m_record_pending = 0;
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
// OSContext (the first 0x2C8 bytes of an OSThread), offsets from the decomp: the SDK exception vector OSExceptionVector 0x80313420 and
// OSDefaultExceptionHandler 0x803134BC store r0..r31 at +0x00 (stw r0,0(r4); stw r1,4(r4); stmw r6,0x18(r4)), CR +0x80, LR +0x84, CTR +0x88,
// XER +0x8C, SRR0 +0x198, SRR1 +0x19C and OR 2 into the u16 at +0x1A2; OSDumpContext 0x8031529C prints exactly these (gpr[i] = word i, LR = word 0x21,
// CR = 0x20, SRR0 = 0x66, SRR1 = 0x67, "+0x1A2 & 1" = FPRs saved) and starts the SDK's own back-chain walk at word 1 (gpr1).
constexpr u32 TH_CTX_GPR0 = 0x00;
constexpr u32 TH_CTX_CR = 0x80;
constexpr u32 TH_CTX_CTR = 0x88;
constexpr u32 TH_CTX_XER = 0x8C;
constexpr u32 TH_CTX_SRR1 = 0x19C;
constexpr u32 TH_CTX_STATE = 0x1A2;  // u16: bit 0 FPRs saved, bit 1 exception (set by the vector, cleared by OSLoadContext)
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
constexpr u32 DUMP_DVDLOG_BASE = 0x80460854;  // DVD library command log (5 records of 5 words; decomp dvd.c StampCommand / the 0x8046085C.. stores)
constexpr u32 DUMP_DVDLOG_WORDS = 32;         // words dumped from it (multiple of 8)
constexpr size_t DUMP_MAX_THREADS = 32;
constexpr size_t DUMP_MAX_WAITERS = 8;
constexpr size_t DUMP_PPC_FRAMES = 16;
constexpr size_t DUMP_THREAD_FRAMES = 6;
constexpr size_t DUMP_CTX_FRAMES = 8;  // frames walked from a saved context's r1

// The unhandled exception path (Rev3 decomp + main.elf). The vector (OSExceptionVector, copied to 0x80000100.. + n * 0x100) saves the context
// into *(0x800000C0) (physical; the same object as OSGetCurrentContext 0x800000D4, normally the running thread's OSThread), sets r3 = exception
// number, r4 = *(0x800000D4), r5 = DSISR, r6 = DAR, does `stwu r1,-8(r1)` ON THE FAULTING THREAD'S OWN STACK (no stack switch) and branches to
// __OSUnhandledException 0x80315A0C (a 0x30 byte frame). Its prologue keeps the arguments in r25 = error, r26 = context, r27 = DSISR, r28 = DAR
// (mr r25,r3 / mr r26,r4 / mr r27,r5 / mr r28,r6), then OSReport("Unhandled Exception %d"), OSDumpContext(ctx), OSReport("DSISR = .. DAR = ..") and ends with
// `bl PPCHalt` (0x802A18B8, 0x14 bytes, a leaf spin loop) at 0x80315CE0: so while halted the live r25..r28 ARE the arguments. The frame above
// __OSUnhandledException is the vector's 8 byte frame: its LR slot is never written (any junk there is normal).
constexpr u32 PPC_HALT_START = 0x802A18B8;
constexpr u32 PPC_HALT_SIZE = 0x14;
constexpr u32 OS_UNHANDLED_START = 0x80315A0C;
constexpr u32 OS_UNHANDLED_SIZE = 0x2F0;
constexpr size_t OS_UNHANDLED_REG_ERROR = 25;
constexpr size_t OS_UNHANDLED_REG_CONTEXT = 26;
constexpr size_t OS_UNHANDLED_REG_DSISR = 27;
constexpr size_t OS_UNHANDLED_REG_DAR = 28;
constexpr u32 OS_MAX_EXCEPTION = 0x20;                // the SDK's __OSErrorTable has 17 entries; anything above this in r25 is not an error number
constexpr u32 OS_LAST_INTERRUPT = 0x8049DA84;         // s16 __OSLastInterrupt (r13 - 0x5E9C), printed by the "Last interrupt (%d)" line
constexpr u32 OS_LAST_INTERRUPT_SRR0 = 0x8049DA80;    // its SRR0 (r13 - 0x5EA0)
// Return address heuristic for the stack scan: Rev3's executable segments (main.elf program headers: 0x80004000 + 0x26A0 and 0x80016320 + 0x3228C0).
constexpr u32 TEXT_START = 0x80004000;
constexpr u32 TEXT_END = 0x80338BE0;
constexpr u32 EXC_STACK_ROWS = 2;          // rows of 8 words printed from the faulting r1
constexpr u32 EXC_SCAN_BYTES = 0x400;      // stack bytes above the faulting r1 scanned for code addresses (also stops at the thread's stack base)
constexpr size_t EXC_SCAN_MAX = 24;        // code addresses listed

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
  // The callee-saved registers: while halted in __OSUnhandledException r25..r28 are its arguments (error, context, DSISR, DAR), see the exc line.
  std::string saved = "ppc";
  for (size_t r = 14; r <= 31; ++r)
    saved += Format(" r%zu=0x%08X", r, cpu.gpr[r]);
  out->push_back(saved);
  std::vector<StackFrame> frames;
  WalkBackChain(memory, cpu.gpr[1], DUMP_PPC_FRAMES, &frames);
  if (frames.empty())
    out->push_back(Format("frame none: r1=0x%08X is not a readable stack pointer", cpu.gpr[1]));
  AppendFrames(out, "", frames);
}

bool InRange(u32 address, u32 start, u32 size)
{
  return address >= start && address - start < size;
}

// The back-chain of a saved r1 as `[lr0,lr1,..]` (each frame's `*(sp+4)`, same convention as the `frame` lines), at most DUMP_CTX_FRAMES.
std::string FrameList(const GuestMemory& memory, u32 sp)
{
  std::vector<StackFrame> frames;
  WalkBackChain(memory, sp, DUMP_CTX_FRAMES, &frames);
  std::string list;
  for (const StackFrame& frame : frames)
    list += Format("%s0x%08X", list.empty() ? "" : ",", frame.lr);
  return "[" + list + "]";
}

// One thread's SAVED OSContext (what the vector / the scheduler stored in the OSThread): `thread 0x.. ctx srr0= srr1= lr= ctr= cr= r1= ctx_state=
// in_stack= frames=[..]`. For the running thread it is the context of the last exception or switch, NOT the live registers: for the thread that
// faulted it is the fault itself (srr0 = the faulting instruction, lr / r1 = its caller, see AppendException). `exc=1` marks the context the OS
// reported as the unhandled exception's. in_stack = r1 lies inside [stack end, stack base].
void AppendThreadContext(std::vector<std::string>* out, const GuestMemory& memory, u32 t, u32 exc_ctx)
{
  const std::optional<u32> srr0 = Rd32(memory, t + TH_CTX_SRR0);
  const std::optional<u32> srr1 = Rd32(memory, t + TH_CTX_SRR1);
  const std::optional<u32> lr = Rd32(memory, t + TH_CTX_LR);
  const std::optional<u32> ctr = Rd32(memory, t + TH_CTX_CTR);
  const std::optional<u32> cr = Rd32(memory, t + TH_CTX_CR);
  const std::optional<u32> sp = Rd32(memory, t + TH_CTX_GPR1);
  const std::optional<u32> ctx_state = Rd16(memory, t + TH_CTX_STATE);
  const std::optional<u32> stack_base = Rd32(memory, t + TH_STACK_BASE);
  const std::optional<u32> stack_end = Rd32(memory, t + TH_STACK_END);
  std::string in_stack = "?";
  if (sp && stack_base && stack_end)
    in_stack = (*sp >= *stack_end && *sp <= *stack_base) ? "1" : "0";
  out->push_back(Format("thread 0x%08X ctx srr0=%s srr1=%s lr=%s ctr=%s cr=%s r1=%s ctx_state=%s in_stack=%s frames=%s%s", t, Hex32(srr0).c_str(),
                        Hex32(srr1).c_str(), Hex32(lr).c_str(), Hex32(ctr).c_str(), Hex32(cr).c_str(), Hex32(sp).c_str(),
                        Hex16(ctx_state).c_str(), in_stack.c_str(), sp ? FrameList(memory, *sp).c_str() : "?",
                        exc_ctx == t ? " exc=1" : ""));
}

// One OSThread. `live` = the running thread: pc / lr / sp come from the live registers instead of its (stale) saved context.
void AppendThread(std::vector<std::string>* out, const GuestMemory& memory, u32 t, const DumpCpu* live, u32 exc_ctx)
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
  AppendThreadContext(out, memory, t, exc_ctx);
}

// The raw stack at the faulting context's r1, for a fault whose back-chain is broken (a leaf function, or a stack that was overwritten):
//   exc stack sp=<r1>: EXC_STACK_ROWS rows of 8 words from r1, and
//   exc stackscan sp=<r1> n=<k> [+<offset>:<word>,..]: the words up to EXC_SCAN_BYTES above r1 (or to the thread's stack base) that look like code
//   addresses (TEXT_START..TEXT_END, word aligned), i.e. a guess at the return addresses of the callers.
void AppendFaultStack(std::vector<std::string>* out, const GuestMemory& memory, u32 context)
{
  const std::optional<u32> sp = Rd32(memory, context + TH_CTX_GPR1);
  if (!sp || (*sp & 3) != 0)
    return;
  for (u32 row = 0; row < EXC_STACK_ROWS; ++row)
  {
    std::string words;
    for (u32 i = row * 8; i < row * 8 + 8; ++i)
      words += " " + Hex32(Rd32(memory, *sp + 4 * i));
    out->push_back(Format("exc stack sp=0x%08X +0x%02X:%s", *sp, row * 32, words.c_str()));
  }
  u64 limit = static_cast<u64>(*sp) + EXC_SCAN_BYTES;
  if (const std::optional<u32> base = Rd32(memory, context + TH_STACK_BASE); base && *base > *sp && *base < limit)
    limit = *base;
  std::string list;
  size_t found = 0;
  for (u64 at = *sp; at < limit && found < EXC_SCAN_MAX; at += 4)
  {
    const std::optional<u32> word = Rd32(memory, static_cast<u32>(at));
    if (!word)
      break;
    if (*word >= TEXT_START && *word < TEXT_END && (*word & 3) == 0)
    {
      list += Format("%s+0x%X:0x%08X", list.empty() ? "" : ",", static_cast<u32>(at - *sp), *word);
      ++found;
    }
  }
  out->push_back(Format("exc stackscan sp=0x%08X n=%zu [%s]", *sp, found, list.c_str()));
}

const char* ExceptionName(u32 error)
{
  // The vector order of the SDK (OSExceptionInit): 0x80000100 + 0x100 * n, with 15 = the Wii's memory protection error (the AI / ARAM / DI DMA report).
  static const char* const names[16] = {"system_reset", "machine_check", "dsi",   "isi",       "external",          "alignment",
                                        "program",      "fp_unavailable", "decrementer", "syscall", "trace",          "perf_monitor",
                                        "breakpoint",   "system_management", "thermal", "memory_protection"};
  return error < 16 ? names[error] : "unknown";
}

// The exception the OS halted on. Active when the CPU is in PPCHalt, in __OSUnhandledException, or has it on its back-chain (a racy dump taken
// while the OS is still printing). In PPCHalt the arguments are live in r25..r28 (see the constants); elsewhere the error number is unknown
// and DSISR / DAR come from the SPRs (the same values the vector read, unless a later DSI replaced them). The context is the OSContext the OS
// was given (r26), or OSGetCurrentContext. Returns that context (0 = none) so the thread line can be marked. Lines:
//   exc none halted=0
//   exc error=<n> srr0= srr1= dsisr= dar= lr= r1= ctx= ctx_src=r26|cur_context args=r25_r28|spr name= insn=<word at srr0> ctx_state= last_int= last_int_srr0=
//        spr_dsisr= spr_dar=
//   exc ctxgpr r0..r7: ... (4 rows: r0..r31 of that context)
u32 AppendException(std::vector<std::string>* out, const GuestMemory& memory, const DumpCpu& cpu, bool cpu_ok)
{
  const std::optional<u32> current = Rd32(memory, OS_CURRENT_CONTEXT);
  const bool in_halt = cpu_ok && InRange(cpu.pc, PPC_HALT_START, PPC_HALT_SIZE);
  bool active = in_halt;
  if (cpu_ok && !active)
  {
    active = InRange(cpu.pc, OS_UNHANDLED_START, OS_UNHANDLED_SIZE) || InRange(cpu.lr, OS_UNHANDLED_START, OS_UNHANDLED_SIZE);
    std::vector<StackFrame> frames;
    WalkBackChain(memory, cpu.gpr[1], DUMP_PPC_FRAMES, &frames);
    for (const StackFrame& frame : frames)
      active = active || InRange(frame.lr, OS_UNHANDLED_START, OS_UNHANDLED_SIZE);
  }
  if (!active)
  {
    out->push_back("exc none halted=0");
    return 0;
  }
  u32 context = current.value_or(0);
  const char* context_src = "cur_context";
  std::optional<u32> error;
  u32 dsisr = cpu.dsisr;
  u32 dar = cpu.dar;
  const char* args = "spr";
  if (in_halt)
  {
    if (DumpPtrOk(cpu.gpr[OS_UNHANDLED_REG_CONTEXT]))
    {
      context = cpu.gpr[OS_UNHANDLED_REG_CONTEXT];
      context_src = "r26";
    }
    if (cpu.gpr[OS_UNHANDLED_REG_ERROR] < OS_MAX_EXCEPTION)
      error = cpu.gpr[OS_UNHANDLED_REG_ERROR];
    dsisr = cpu.gpr[OS_UNHANDLED_REG_DSISR];
    dar = cpu.gpr[OS_UNHANDLED_REG_DAR];
    args = "r25_r28";
  }
  const bool readable = DumpPtrOk(context);
  std::optional<u32> srr0;
  std::optional<u32> insn;
  if (readable)
  {
    srr0 = Rd32(memory, context + TH_CTX_SRR0);
    if (srr0)
      insn = Rd32(memory, *srr0);
  }
  const std::optional<u32> last_int = Rd16(memory, OS_LAST_INTERRUPT);  // s16: shown as the signed value
  std::string last_int_text = "?";
  if (last_int)
    last_int_text = Format("%d", (*last_int & 0x8000) != 0 ? static_cast<int>(*last_int) - 0x10000 : static_cast<int>(*last_int));
  out->push_back(Format("exc error=%s srr0=%s srr1=%s dsisr=0x%08X dar=0x%08X lr=%s r1=%s ctx=0x%08X ctx_src=%s args=%s name=%s insn=%s "
                        "ctx_state=%s last_int=%s last_int_srr0=%s spr_dsisr=0x%08X spr_dar=0x%08X",
                        error ? Format("%u", *error).c_str() : "?", Hex32(srr0).c_str(), readable ? Hex32(Rd32(memory, context + TH_CTX_SRR1)).c_str() : "?", dsisr, dar,
                        readable ? Hex32(Rd32(memory, context + TH_CTX_LR)).c_str() : "?",
                        readable ? Hex32(Rd32(memory, context + TH_CTX_GPR1)).c_str() : "?", context, context_src, args,
                        error ? ExceptionName(*error) : "?", Hex32(insn).c_str(),
                        readable ? Hex16(Rd16(memory, context + TH_CTX_STATE)).c_str() : "?", last_int_text.c_str(),
                        Hex32(Rd32(memory, OS_LAST_INTERRUPT_SRR0)).c_str(), cpu.dsisr, cpu.dar));
  if (readable)
  {
    for (u32 row = 0; row < 32; row += 8)
    {
      std::string words;
      for (u32 i = row; i < row + 8; ++i)
        words += " " + Hex32(Rd32(memory, context + TH_CTX_GPR0 + 4 * i));
      out->push_back(Format("exc ctxgpr r%u..r%u:%s", row, row + 7, words.c_str()));
    }
    AppendFaultStack(out, memory, context);
  }
  return readable ? context : 0;
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
  const u32 exc_ctx = AppendException(out, memory, cpu, cpu_ok);
  out->push_back(Format("threads n=%zu end=%s", threads.size(), end.c_str()));
  for (const u32 thread : threads)
    AppendThread(out, memory, thread, cpu_ok && current && *current == thread ? &cpu : nullptr, exc_ctx);
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
  // The DVD library's own command log (dvd.c StampCommand 0x802D0624: a ring of 5 records x 5 words at 0x80460854, next record
  // index in *0x8049D7E0, which the `words dvd` row at +0x40 shows): which DVD command the library stamped last, 32 words in 4 rows.
  for (u32 row = 0; row < DUMP_DVDLOG_WORDS; row += 8)
  {
    std::string words;
    for (u32 i = row; i < row + 8; ++i)
      words += " " + Hex32(Rd32(memory, DUMP_DVDLOG_BASE + 4 * i));
    out->push_back(Format("words dvdlog base=0x%08X +0x%02X:%s", DUMP_DVDLOG_BASE, 4 * row, words.c_str()));
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

// ---- the OS alarm queue (see Rev3Rollback.h "THE ALARM QUEUE") ------------------------------------------------------------------

namespace
{
constexpr u32 ALARM_BYTES = 0x2C;              // OSAlarm: handler .. userData
constexpr size_t ALARM_PROBLEMS_PER_KIND = 3;  // problems of one kind named in a report (a cycle makes every later node "wrong")

struct KnownAddress
{
  u32 address;
  const char* name;
};
// Alarm objects that are statics in this build (decomp: the first argument of OSSetAlarm / OSSetPeriodicAlarm / OSCreateAlarm).
constexpr KnownAddress ALARM_OBJECTS[] = {
    {0x80465320, "playrecord"},      // OSPlayRecord: 1 s retry or 60 s write timer of the play record (NAND)
    {0x804654C0, "playtime"},        // OSPlayTime: trial time limit
    {0x8040BBA0, "wpad"},            // WPAD manage timer, periodic
    {0x8040FA90, "wud"},             // WUD state machine timer, periodic
    {0x804531A0, "bte"},             // Bluetooth stack tick, periodic
    {0x80460550, "dvd_fatal"},       // dvd FatalAlarmHandler
    {0x80460580, "dvd_cover"},       // dvd CoverAlarmHandler, periodic
    {0x80460620, "dvd_reset"},       // dvd ResetAlarmHandler
    {0x80403870, "remote_speaker"},  // RemoteSpeakerManager singleton 0x80403868 + 8, periodic 405000 ticks (6.67 ms)
};
constexpr KnownAddress ALARM_HANDLERS[] = {
    {0x8031D128, "SleepAlarmHandler"},  // OSSleepTicks: resumes the sleeping thread (userData)
    {0x8031E258, "PlayRecordAlarmCallback"},
    {0x8031F204, "__OSPlayTimeAlarmExpired"},
    {0x802887F8, "WPADiManageHandler0"},
    {0x80296790, "__wudInitHandler0"},
    {0x802968C0, "__wudShutdownHandler0"},
    {0x80295128, "__wudSyncHandler0"},
    {0x80295A0C, "__wudStackHandler0"},
    {0x8029578C, "__wudDeleteHandler0"},
    {0x802A4AFC, "BTUInterruptHandler"},
    {0x802D0B2C, "FatalAlarmHandler"},
    {0x802D32B0, "CoverAlarmHandler"},
    {0x802D289C, "ResetAlarmHandler"},
    {0x8020FB2C, "RemoteSpeakerAlarmProc"},
};

u64 ReadBE64(const u8* p)
{
  return (static_cast<u64>(ReadBE32(p)) << 32) | ReadBE32(p + 4);
}

bool ReadAlarmNode(const GuestMemory& memory, u32 addr, AlarmNode* out)
{
  u8 raw[ALARM_BYTES];
  if (!DumpPtrOk(addr) || !memory.Read(addr, raw, sizeof(raw)))
    return false;
  out->addr = addr;
  out->handler = ReadBE32(raw + 0x00);
  out->tag = ReadBE32(raw + 0x04);
  out->fire = static_cast<s64>(ReadBE64(raw + 0x08));
  out->prev = ReadBE32(raw + 0x10);
  out->next = ReadBE32(raw + 0x14);
  out->period = static_cast<s64>(ReadBE64(raw + 0x18));
  out->start = static_cast<s64>(ReadBE64(raw + 0x20));
  out->user = ReadBE32(raw + 0x28);
  return true;
}

// Adds a problem unless ALARM_PROBLEMS_PER_KIND of the same kind (the text up to the first '(') are already there.
void AddAlarmProblem(AlarmQueueReport* r, const std::string& text)
{
  const std::string kind = text.substr(0, text.find('('));
  size_t same = 0;
  for (const std::string& p : r->problems)
  {
    if (p.compare(0, kind.size(), kind) == 0 && (p.size() == kind.size() || p[kind.size()] == '('))
      ++same;
  }
  if (same < ALARM_PROBLEMS_PER_KIND)
    r->problems.push_back(text);
}

bool HasNode(const std::vector<AlarmNode>& nodes, u32 addr)
{
  for (const AlarmNode& n : nodes)
  {
    if (n.addr == addr)
      return true;
  }
  return false;
}
}  // namespace

const char* AlarmObjectName(u32 address)
{
  for (const KnownAddress& k : ALARM_OBJECTS)
  {
    if (k.address == address)
      return k.name;
  }
  return nullptr;
}

const char* AlarmHandlerName(u32 address)
{
  for (const KnownAddress& k : ALARM_HANDLERS)
  {
    if (k.address == address)
      return k.name;
  }
  return nullptr;
}

std::string AlarmQueueReport::Status() const
{
  if (!readable)
    return "unreadable";
  if (problems.empty())
    return nodes.empty() ? "empty" : "ok";
  std::string text;
  for (const std::string& p : problems)
    text += (text.empty() ? "" : ";") + p;
  return text;
}

// The same walk as InsertAlarm (head, then each node's +0x14 next), with a repeat check. Forward: every node's prev must be the node before
// it (InsertAlarm and the decrementer callback keep that), the fire times must not decrease (InsertAlarm inserts before the first node that
// fires later), the last node must be the tail. Backward from the tail by +0x10 prev must end at the head; nodes only that chain finds
// are the ones a double insertion lost from the forward list.
AlarmQueueReport ReadAlarmQueue(const GuestMemory& memory)
{
  AlarmQueueReport r;
  const std::optional<u32> head = Rd32(memory, ALARM_QUEUE_HEAD);
  const std::optional<u32> tail = Rd32(memory, ALARM_QUEUE_TAIL);
  if (!head || !tail)
    return r;
  r.readable = true;
  r.head = *head;
  r.tail = *tail;
  if ((r.head == 0) != (r.tail == 0))
    AddAlarmProblem(&r, Format("head_tail(head=0x%08X tail=0x%08X)", r.head, r.tail));

  u32 cur = r.head;
  u32 expected_prev = 0;
  bool forward_clean_end = false;
  while (true)
  {
    if (cur == 0)
    {
      forward_clean_end = true;
      break;
    }
    if (r.nodes.size() >= ALARM_WALK_MAX)
    {
      AddAlarmProblem(&r, Format("too_long(%zu nodes)", r.nodes.size()));
      break;
    }
    if (HasNode(r.nodes, cur))
    {
      size_t first = 0;
      while (first < r.nodes.size() && r.nodes[first].addr != cur)
        ++first;
      AddAlarmProblem(&r, Format("cycle(0x%08X index %zu is index %zu)", cur, r.nodes.size(), first));
      break;
    }
    AlarmNode n;
    if (!ReadAlarmNode(memory, cur, &n))
    {
      AddAlarmProblem(&r, r.nodes.empty() ? Format("bad_pointer(0x%08X as the head)", cur) :
                                         Format("bad_pointer(0x%08X after index %zu)", cur, r.nodes.size() - 1));
      break;
    }
    n.prev_ok = n.prev == expected_prev;
    if (!n.prev_ok)
      AddAlarmProblem(&r, Format("prev_link(index %zu 0x%08X prev=0x%08X expected=0x%08X)", r.nodes.size(), cur, n.prev, expected_prev));
    if (!r.nodes.empty())
    {
      n.order_ok = n.fire >= r.nodes.back().fire;
      if (!n.order_ok)
        AddAlarmProblem(&r, Format("order(index %zu 0x%08X fires before index %zu)", r.nodes.size(), cur, r.nodes.size() - 1));
    }
    expected_prev = cur;
    cur = n.next;
    r.nodes.push_back(n);
  }
  if (forward_clean_end && !r.nodes.empty() && r.nodes.back().addr != r.tail)
    AddAlarmProblem(&r, Format("tail(0x%08X, the walk ends at 0x%08X)", r.tail, r.nodes.back().addr));

  // Backward from the tail.
  std::vector<AlarmNode> back;
  u32 b = r.tail;
  while (b != 0)
  {
    if (back.size() >= ALARM_WALK_MAX || HasNode(back, b))
    {
      AddAlarmProblem(&r, Format("backward_cycle(0x%08X)", b));
      break;
    }
    AlarmNode n;
    if (!ReadAlarmNode(memory, b, &n))
    {
      AddAlarmProblem(&r, Format("backward_bad_pointer(0x%08X)", b));
      break;
    }
    back.push_back(n);
    b = n.prev;
  }
  if (!back.empty() && b == 0 && back.back().addr != r.head)
    AddAlarmProblem(&r, Format("backward_end(0x%08X is not the head)", back.back().addr));
  for (const AlarmNode& n : back)
  {
    if (r.orphans.size() < 8 && !HasNode(r.nodes, n.addr))
      r.orphans.push_back(n);
  }
  return r;
}

std::optional<s64> AlarmSystemTime(const GuestMemory& memory, u64 tb)
{
  u8 raw[8];
  if (!memory.Read(OS_SYSTEM_TIME_BIAS, raw, sizeof(raw)))
    return std::nullopt;
  return static_cast<s64>(tb + ReadBE64(raw));
}

namespace
{
std::string AlarmNodeText(const AlarmNode& n, const std::optional<s64>& now, u32 tps, const std::vector<AlarmThreadStack>& stacks)
{
  std::string name;
  if (const char* known = AlarmObjectName(n.addr))
    name = Format(" name=%s", known);
  for (const AlarmThreadStack& t : stacks)
  {
    if (n.addr >= t.stack_end && n.addr < t.stack_base)
      name += Format(" on_stack_of=0x%08X", t.thread);
  }
  std::string handler = Format("0x%08X", n.handler);
  if (const char* known = AlarmHandlerName(n.handler))
    handler += Format("(%s)", known);
  std::string due;
  if (now && tps != 0)
    due = Format(" due=%+.3fms", static_cast<double>(n.fire - *now) * 1000.0 / static_cast<double>(tps));
  std::string period;
  if (n.period != 0)
  {
    period = tps != 0 ? Format(" period=%lld(%.3fms)", static_cast<long long>(n.period),
                               static_cast<double>(n.period) * 1000.0 / static_cast<double>(tps)) :
                        Format(" period=%lld", static_cast<long long>(n.period));
  }
  std::string flags;
  if (!n.prev_ok)
    flags += " prev_bad";
  if (!n.order_ok)
    flags += " order_bad";
  return Format("at=0x%08X%s handler=%s fire=0x%016llX%s%s prev=0x%08X next=0x%08X user=0x%08X tag=0x%08X%s", n.addr, name.c_str(),
                handler.c_str(), static_cast<unsigned long long>(n.fire), due.c_str(), period.c_str(), n.prev, n.next, n.user, n.tag,
                flags.c_str());
}
}  // namespace

std::vector<std::string> AlarmQueueLines(const AlarmQueueReport& report, const std::optional<s64>& now, u32 ticks_per_second,
                                         const std::vector<AlarmThreadStack>& thread_stacks)
{
  std::vector<std::string> out;
  if (!report.readable)
  {
    out.push_back(Format("alarmq unreadable: head 0x%08X / tail 0x%08X are not in guest memory", ALARM_QUEUE_HEAD, ALARM_QUEUE_TAIL));
    return out;
  }
  const std::string now_text = now ? Format("0x%016llX", static_cast<unsigned long long>(*now)) : std::string("?");
  out.push_back(Format("alarmq head=0x%08X tail=0x%08X n=%zu orphans=%zu status=%s now=%s tps=%u", report.head, report.tail,
                       report.nodes.size(), report.orphans.size(), report.Status().c_str(), now_text.c_str(), ticks_per_second));
  for (size_t i = 0; i < report.nodes.size(); ++i)
  {
    if (i >= ALARM_LINES_MAX)
    {
      out.push_back(Format("alarmq ... %zu more nodes", report.nodes.size() - i));
      break;
    }
    out.push_back(Format("alarmq[%zu] ", i) + AlarmNodeText(report.nodes[i], now, ticks_per_second, thread_stacks));
  }
  for (size_t i = 0; i < report.orphans.size(); ++i)
    out.push_back(Format("alarmq orphan[%zu] ", i) + AlarmNodeText(report.orphans[i], now, ticks_per_second, thread_stacks));
  return out;
}

namespace
{
// The active threads' stacks, for "this alarm sits on that thread's stack" (a sleeping thread's OSSleepTicks alarm lives in its frame).
std::vector<AlarmThreadStack> AlarmThreadStacks(const GuestMemory& memory)
{
  std::vector<AlarmThreadStack> out;
  u32 t = Rd32(memory, OS_ACTIVE_THREADS).value_or(0);
  while (t != 0 && DumpPtrOk(t) && out.size() < DUMP_MAX_THREADS)
  {
    bool seen = false;
    for (const AlarmThreadStack& known : out)
      seen = seen || known.thread == t;
    if (seen)
      break;
    AlarmThreadStack s;
    s.thread = t;
    s.stack_end = Rd32(memory, t + TH_STACK_END).value_or(0);
    s.stack_base = Rd32(memory, t + TH_STACK_BASE).value_or(0);
    out.push_back(s);
    t = Rd32(memory, t + TH_ACTIVE_NEXT).value_or(0);
  }
  return out;
}

// The PlayRecord state machine's words (decomp PlayRecordCallback @ 0x8031E264): its alarm is the usual neighbour of a lost IOS reply.
void AppendPlayRecord(std::vector<std::string>* out, const GuestMemory& memory)
{
  out->push_back(Format("playrecord state=%s retry=%s last_result=%s error=%s stop=%s", Signed(Rd32(memory, 0x8049C9A8)).c_str(),
                        Hex32(Rd32(memory, 0x8049DB08)).c_str(), Signed(Rd32(memory, 0x8049DB0C)).c_str(),
                        Hex32(Rd32(memory, 0x8049DAFC)).c_str(), Hex32(Rd32(memory, 0x8049DB00)).c_str()));
}

// `time ...` (the clocks) and the alarm queue of a freeze dump. Without a clock the alarm lines carry no relative times.
void AppendAlarms(std::vector<std::string>* out, const GuestMemory& memory, const DumpClock& clock, bool clock_ok)
{
  u32 tps = 0;
  if (const std::optional<u32> bus = Rd32(memory, OS_BUS_CLOCK); bus && *bus != 0)
    tps = *bus / 4;
  std::optional<s64> now;
  if (clock_ok)
  {
    now = AlarmSystemTime(memory, clock.tb);
    const std::string dec_event = !clock.dec_event_known ? std::string("dec_event=?") :
                                  clock.dec_event      ? Format("dec_event=+%lld cycles", static_cast<long long>(clock.dec_event_cycles)) :
                                                         std::string("dec_event=none");
    out->push_back(Format("time tb=0x%016llX dec=0x%08X %s core_ticks=%llu", static_cast<unsigned long long>(clock.tb), clock.dec,
                          dec_event.c_str(), static_cast<unsigned long long>(clock.core_ticks)));
  }
  else
  {
    out->push_back("time unavailable");
  }
  const AlarmQueueReport report = ReadAlarmQueue(memory);
  for (std::string& line : AlarmQueueLines(report, now, tps, AlarmThreadStacks(memory)))
    out->push_back(std::move(line));
  AppendPlayRecord(out, memory);
}
}  // namespace

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
  {
    DumpClock clock;
    const bool clock_ok = m_source.ReadClock(&clock, racy) && clock.valid;
    AppendAlarms(&out, m_memory, clock, clock_ok);
  }
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

  // Wii IOS requests and replies on their way: CoreTiming events IPCEvent (EmulationKernel::EnqueueIPCReply / EnqueueIPCRequest: IOS writes
  // the reply into guest memory at once and the event hands it to the PPC), IPCInterrupt (the IPC interrupt to the PPC) and FinishDICommand.
  // CPU thread only (GetPendingEvents). *what = "IPCEvent+123,IPCInterrupt+40" (remaining CPU cycles).
  u32 IosEventsPending(std::string* what) override
  {
    if (!m_system.IsWii())
      return 0;
    auto& core_timing = m_system.GetCoreTiming();
    if (!m_ios_types_looked_up)
    {
      for (const char* name : IOS_EVENT_TYPES)
      {
        if (CoreTiming::EventType* type = core_timing.FindEventType(name); type != nullptr)
          m_ios_types.push_back(type);
      }
      m_ios_types_looked_up = !m_ios_types.empty();
    }
    if (m_ios_types.empty())
      return 0;
    const s64 now = static_cast<s64>(core_timing.GetTicks());
    u32 count = 0;
    std::string list;
    for (const CoreTiming::Event& event : core_timing.GetPendingEvents())
    {
      if (event.type == nullptr || std::find(m_ios_types.begin(), m_ios_types.end(), event.type) == m_ios_types.end())
        continue;
      ++count;
      if (what != nullptr && count <= 8)
        list += Format("%s%s+%lld", list.empty() ? "" : ",", event.type->name->c_str(), static_cast<long long>(std::max<s64>(event.time - now, 0)));
    }
    if (what != nullptr)
      *what = list;
    return count;
  }

private:
  static constexpr const char* IOS_EVENT_TYPES[] = {"IPCEvent", "IPCInterrupt", "FinishDICommand"};
  std::vector<CoreTiming::EventType*> m_ios_types;
  bool m_ios_types_looked_up = false;

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
    out->dsisr = ppc.spr[SPR_DSISR];
    out->dar = ppc.spr[SPR_DAR];
    out->cr = ppc.cr.Get();
    out->exceptions = ppc.Exceptions;
    for (size_t i = 0; i < out->gpr.size(); ++i)
      out->gpr[i] = ppc.gpr[i];
    out->valid = true;
    return true;
  }

  // The clocks the OS alarm module depends on. Rev3: the game's OSGetTime is the fake time base; the alarm module programs the decrementer
  // (mtdec) for the earliest alarm and Dolphin keeps it as the CoreTiming event "DecCallback" plus FakeDecStartValue / Ticks. A restore leaves
  // both alone unless REV3_SYNCTEST_TB=1 (time base only); this line is what to compare with the `alarmq` fire times.
  bool ReadClock(DumpClock* out, bool racy) override
  {
    auto& timers = m_system.GetSystemTimers();
    auto& core_timing = m_system.GetCoreTiming();
    out->tb = timers.GetFakeTimeBase();
    out->dec = timers.GetFakeDecrementer();
    out->core_ticks = core_timing.GetTicks();
    if (!racy)
    {
      out->dec_event_known = true;
      if (CoreTiming::EventType* type = core_timing.FindEventType("DecCallback"); type != nullptr)
      {
        const s64 now = static_cast<s64>(out->core_ticks);
        for (const CoreTiming::Event& event : core_timing.GetPendingEvents())
        {
          if (event.type == type)
          {
            out->dec_event = true;
            out->dec_event_cycles = event.time - now;
            break;
          }
        }
      }
    }
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
    if (!racy)
    {
      // Every pending CoreTiming event by type (count and the soonest in CPU cycles): what is still on its way (IPC replies, VI, audio).
      auto& core_timing = m_system.GetCoreTiming();
      const s64 now = static_cast<s64>(core_timing.GetTicks());
      std::vector<std::pair<std::string, std::pair<u32, s64>>> by_type;
      for (const CoreTiming::Event& event : core_timing.GetPendingEvents())
      {
        const std::string name = event.type != nullptr && event.type->name != nullptr ? *event.type->name : std::string("?");
        auto it = std::find_if(by_type.begin(), by_type.end(), [&](const auto& e) { return e.first == name; });
        if (it == by_type.end())
          by_type.push_back({name, {1, event.time - now}});
        else
          ++it->second.first;  // events are in firing order: the first one seen is the soonest
      }
      std::string list;
      for (const auto& e : by_type)
        list += Format("%s%s x%u +%lld", list.empty() ? "" : ", ", e.first.c_str(), e.second.first, static_cast<long long>(e.second.second));
      out->push_back(Format("ct events %zu types [%s]", by_type.size(), list.c_str()));
    }
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

// The soak's host-side exits (Rev3Bot.h): a thread that wakes every 250 ms and applies the pure decisions of Rev3Bot.h.
//   REV3_NET_FREEZE_EXIT_S  the freeze dump's stall limit plus this many seconds without a hook command (the net session's deliberate waits for
//                           the peer excluded): write a last dump, `LEAVE ... reason=freeze`, and end the process hard with EXIT_CODE_FREEZE. A
//                           frozen game is usually a CPU thread that never comes back, so a polite stop would not finish.
//   REV3_NET_MAX_S          wall seconds since the watcher started (the first hook command): request a stop; if the process is still there
//                           STOP_GRACE_MS later, end it with EXIT_CODE_MAX_S.
//   a planned stop          NoteStop() is called by the CPU thread when it asked the host to stop; if the process is still there STOP_GRACE_MS
//                           later it is ended with EXIT_CODE_STOP_HANG (a clean stop takes about a second).
// Ending the process is std::_Exit: no destructors run (the logs are flushed per line; the pad record / trace may lose their last lines).
class ExitWatcher
{
public:
  using StopFn = std::function<void()>;
  ExitWatcher(FreezeDump* dump, Sink& sink, u32 freeze_exit_s, u32 max_s, StopFn request_stop)
      : m_shared(std::make_shared<Shared>())
  {
    m_shared->start = std::chrono::steady_clock::now();
    std::shared_ptr<Shared> shared = m_shared;
    Sink* const log = &sink;
    m_thread = std::thread([shared, dump, log, freeze_exit_s, max_s, request_stop]() { Run(*shared, dump, *log, freeze_exit_s, max_s, request_stop); });
  }

  ~ExitWatcher()
  {
    if (m_thread.joinable())
      m_thread.detach();
  }

  // CPU thread: a stop was asked for (the first call counts).
  void NoteStop()
  {
    s64 expected = -1;
    m_shared->stop_at_ms.compare_exchange_strong(expected, ElapsedMs(*m_shared));
  }

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
    std::chrono::steady_clock::time_point start;
    std::atomic<s64> stop_at_ms{-1};       // elapsed ms of the first stop request, -1 = none
    std::atomic<bool> stop_for_max{false};  // that request was the wall-clock cap's
  };

  static s64 ElapsedMs(const Shared& shared)
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shared.start).count();
  }

  static void Run(Shared& shared, FreezeDump* dump, Sink& sink, u32 freeze_exit_s, u32 max_s, const StopFn& request_stop)
  {
    constexpr std::chrono::milliseconds POLL(250);  // watcher poll period
    while (true)
    {
      {
        std::unique_lock<std::mutex> lock(shared.mutex);
        shared.cv.wait_for(lock, POLL, [&shared]() { return shared.stop.load(); });
      }
      if (shared.stop.load())
        break;
      const s64 elapsed = ElapsedMs(shared);
      if (dump != nullptr && Bot::FreezeExitDue(dump->LastAgeMs(), dump->Seen(), dump->Blocked(), dump->StallMs(), freeze_exit_s))
      {
        const u64 age = dump->LastAgeMs();
        const u32 frame = dump->LastFrame();
        sink.Line(Format("EXIT freeze: no hook command for %llu ms (last frame %u; REV3_NET_FREEZE_EXIT_S=%u after the %u ms stall limit)",
                         static_cast<unsigned long long>(age), frame, freeze_exit_s, dump->StallMs()));
        dump->Dump("freeze", true, false, "REV3_NET_FREEZE_EXIT_S");
        sink.Line(Format("LEAVE frame=%u from=WATCHDOG reason=freeze (no hook command for %llu ms)", frame, static_cast<unsigned long long>(age)));
        sink.Line(Format("EXIT code=%d (freeze)", Bot::EXIT_CODE_FREEZE));
        std::_Exit(Bot::EXIT_CODE_FREEZE);
      }
      if (shared.stop_at_ms.load() < 0 && Bot::MaxSecondsDue(static_cast<u64>(elapsed), max_s))
      {
        sink.Line(Format("EXIT max_s: REV3_NET_MAX_S=%u wall seconds are up; stop requested", max_s));
        s64 expected = -1;
        shared.stop_at_ms.compare_exchange_strong(expected, elapsed);
        shared.stop_for_max.store(true);
        if (request_stop)
          request_stop();
      }
      const s64 stop_at = shared.stop_at_ms.load();
      if (stop_at >= 0 && Bot::StopHangDue(static_cast<u64>(elapsed - stop_at)))
      {
        const int code = shared.stop_for_max.load() ? Bot::EXIT_CODE_MAX_S : Bot::EXIT_CODE_STOP_HANG;
        sink.Line(Format("EXIT stop did not end the process within %u ms; ending it (code %d)", Bot::STOP_GRACE_MS, code));
        std::_Exit(code);
      }
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
  // `title` names the log in its first line ("=== <title> log opened <time> UTC ===", which starts with '=' so line counters skip it).
  explicit FileSink(const std::string& path, const char* title = "Rev3 SyncTest")
  {
    File::CreateFullPath(path);
    File::OpenFStream(m_file, path, std::ios_base::out | std::ios_base::app);
    const std::time_t now = std::time(nullptr);
    char stamp[64] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    Line(Format("=== %s log opened %s UTC ===", title, stamp));
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
  void OsdInfo(const std::string& text) override { OSD::AddMessage(text, OSD::Duration::NORMAL, OSD::Color::GREEN); }

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

// Host write tracking (REV3_RB_WRITE_TRACK; Rev3WriteTrack.h, REV3.md "Host write tracking"). The WriteTracker does the protecting; this class
// tells it where Dolphin's host views of guest RAM are (MemoryManager::GetRamViews()), receives the faults through EMM::SetWriteFaultHook and
// follows the views when Dolphin rebuilds them: MemoryManager calls ViewsChanging right BEFORE it unmaps a view, which makes all guest RAM
// writable again; the next SetWatched (a restart, logged by SyncTest) fetches the new views.
class DolphinWriteWatch final : public WriteWatch
{
public:
  explicit DolphinWriteWatch(Core::System& system) : m_system(system)
  {
    s_self = this;
    m_tracker.MakeCurrent();
    m_tracker.SetContextProbe(&Probe);
    m_hook_installed = EMM::IsExceptionHandlerSupported() && EMM::SetWriteFaultHook(&WriteTracker::HandleFault);
    m_tracker.SetHandlerInstalled(m_hook_installed);
    system.GetMemory().SetViewsChangingCallback(&ViewsChanging);
  }
  ~DolphinWriteWatch() override
  {
    m_tracker.Stop();  // everything writable BEFORE the hook goes away: a fault with nobody to answer it would be handed to the JIT
    m_system.GetMemory().SetViewsChangingCallback(nullptr);
    EMM::SetWriteFaultHook(nullptr);
    s_self = nullptr;
  }

  std::string Describe() const override { return m_tracker.Describe(); }
  bool SetWatched(const std::vector<Range>& keep, std::string* why) override
  {
    if (!m_hook_installed)
    {
      *why = "no fault handler installed (this platform's exception handler cannot call the write tracker)";
      return false;
    }
    Memory::MemoryManager& memory = m_system.GetMemory();
    if (!m_views_set || m_generation != memory.GetViewGeneration())
    {
      std::vector<WriteView> views;
      for (const Memory::RamView& v : memory.GetRamViews())
        views.push_back({v.guest_address, v.host, v.size});
      m_generation = memory.GetViewGeneration();
      m_views_set = m_tracker.SetViews(views, why);
      if (!m_views_set)
        return false;
    }
    return m_tracker.SetWatched(keep, why);
  }
  void Stop() override { m_tracker.Stop(); }
  void Poll(std::vector<DirtyPage>* out) override { m_tracker.Poll(out); }
  std::vector<u32> Rearm(const std::vector<u32>& pages) override { return m_tracker.Rearm(pages); }
  void Release(const std::vector<u32>& pages) override { m_tracker.Release(pages); }
  bool IsClean(u32 page_addr) const override { return m_tracker.IsClean(page_addr); }
  bool Active() const override { return m_tracker.Active(); }
  WriteWatchStats Stats() const override { return m_tracker.Stats(); }
  bool SelfTest(std::string* why) override { return m_tracker.SelfTest(why); }

private:
  // Runs inside the fault handler: no allocation, no locks. The guest pc is the start of the JIT block that stores (the JIT keeps pc per
  // block), lr is exact. Only the CPU thread has a PPC context.
  static bool Probe(u32* pc, u32* lr)
  {
    if (!Core::IsCPUThread())
      return false;
    const PowerPC::PowerPCState& ppc = Core::System::GetInstance().GetPPCState();
    *pc = ppc.pc;
    *lr = LR(ppc);
    return true;
  }
  static void ViewsChanging()
  {
    if (s_self != nullptr)
      s_self->m_tracker.Stop();
  }

  Core::System& m_system;
  WriteTracker m_tracker;
  bool m_hook_installed = false;
  bool m_views_set = false;
  u32 m_generation = 0;
  static inline DolphinWriteWatch* s_self = nullptr;
};

// What the game sees when SyncTest is off: the module is created on the first command and then only echoes.
struct Module
{
  bool initialized = false;
  std::unique_ptr<DolphinMemory> memory;
  std::unique_ptr<DolphinHardware> hardware;
  std::unique_ptr<DolphinAudioHw> audio;
  std::unique_ptr<DolphinWriteWatch> watch;  // REV3_RB_WRITE_TRACK; null = off
  std::unique_ptr<FileSink> sink;
  std::unique_ptr<SyncTest> synctest;  // null = off
  std::unique_ptr<FileLineWriter> pad_record;
  std::unique_ptr<FileLineWriter> trace;
  std::unique_ptr<DeterminismKit> kit;  // null = none of REV3_PAD_RECORD / REV3_PAD_SCRIPT / REV3_TRACE is usable
  std::unique_ptr<Net::PaceActuator> pacer;  // a DolphinPacer; netplay runs only
  std::unique_ptr<Net::ResimHider> hider;  // a DolphinResimHider (defined further down); netplay runs only; hiding off when it goes
  std::unique_ptr<ResimHideFlags> hide;    // M6: the protocol 3 flag word; netplay (written by hider) or SyncTest with REV3_SYNCTEST_HIDE=1 (written by synctest)
  std::unique_ptr<FileSink> net_sink;    // the net log (REV3_NET_LOG), netplay runs only
  std::unique_ptr<Net::NetModule> net;   // null = not a netplay run (or netplay unusable: the game then runs offline)
  std::unique_ptr<DolphinDumpSource> dump_source;  // freeze dump (REV3_SYNCTEST_DUMP); null = off
  std::unique_ptr<FreezeDump> dump;
  std::unique_ptr<StallWatchdog> watchdog;
  int exit_code = 0;                                // set when the process must end with a code (Bot::EXIT_CODE_LEFT): Shutdown ends it at the very end
  Bot::BotOptions bot_options;                      // REV3_PAD_BOT / REV3_NET_EXIT / REV3_NET_EXIT_WAIT_S / REV3_NET_FREEZE_EXIT_S / REV3_NET_MAX_S
  std::unique_ptr<Bot::PadBotController> bot;       // REV3_PAD_BOT; also held by the kit (SetBot)
  std::unique_ptr<Bot::ExitController> exit_ctl;    // REV3_NET_EXIT
  std::unique_ptr<ExitWatcher> exit_watcher;        // freeze exit, wall-clock cap, stop hang
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
  const bool bot_wanted = !GetEnv("REV3_PAD_BOT").empty();  // the kit carries the bot (InitializeBot attaches it)
  if (record_path.empty() && script_path.empty() && trace_path.empty() && !bot_wanted)
    return;
  FileSink& sink = EnsureSink(module);
  const auto fail = [&sink](const std::string& what) {
    sink.Line("ERROR " + what);
    ERROR_LOG_FMT(SLIPPI, "Rev3: {}", what);
    sink.Osd("Rev3: " + what);
  };

  DeterminismOptions options;
  options.record_reply = NetplayRequested();  // netplay: the record holds what the game got (the reply), local and remote
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
  if (module.pad_record == nullptr && module.trace == nullptr && options.script_path.empty() && !bot_wanted)
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

// REV3_RESIM_HIDE (see ParseResimHide): the flags to write while a resimulation runs; a bad value is reported and the default (draw) used.
u32 ReadResimHide(Sink& log)
{
  u32 flags = HIDE_DEFAULT;
  const std::string text = GetEnv("REV3_RESIM_HIDE");
  if (!ParseResimHide(text, &flags))
  {
    log.Line("ERROR REV3_RESIM_HIDE=" + text + " is none of 0|off, 1|draw, 2|sound, 3|all; using draw");
    ERROR_LOG_FMT(SLIPPI, "Rev3: ignoring REV3_RESIM_HIDE={}", text);
  }
  return flags;
}

// REV3_DET_VOICE: 1 (default) = FLAGS bit 2 on for the whole session; 0 = Rev3's own voice gate (to reproduce the desync, or an A/B run). Returns DET_VOICE or 0.
u32 ReadDetVoice(Sink& log)
{
  const std::string text = GetEnv("REV3_DET_VOICE");
  if (text.empty() || text == "1")
    return DET_VOICE;
  if (text != "0")
    log.Line("ERROR REV3_DET_VOICE=" + text + " is neither 0 nor 1; using 1");
  return text == "0" ? 0u : DET_VOICE;
}

// Builds module.synctest from the REV3_SYNCTEST_* variables (K frames; in netplay K = MAXR + 1 so the ring has MAXR + 2 slots) and the
// REV3_WHITELIST seed. False (logged): not usable; module.synctest stays null.
bool BuildSyncTest(Module& module, Core::System& system, long k, bool net)
{
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
  // REV3_ALARMCHECK: 1 (default) = the OS alarm queue is walked at every capture and around every restore, 0 = off.
  const std::string alarmcheck = GetEnv("REV3_ALARMCHECK");
  if (alarmcheck == "0" || alarmcheck == "1")
    options.alarm_check = alarmcheck == "1" ? 1 : 0;
  else if (!alarmcheck.empty())
    ERROR_LOG_FMT(SLIPPI, "Rev3 SyncTest: ignoring REV3_ALARMCHECK={} (0 or 1)", alarmcheck);
  // REV3_RB_IOS_GATE=1: a pending IOS IPC event counts like a disc read in flight (default 0: counted only).
  options.ios_gate = GetEnv("REV3_RB_IOS_GATE") == "1";
  const bool hardware_exclude = GetEnv("REV3_SYNCTEST_HW_EXCLUDE") != "0";

  std::string log_path;
  EnsureSink(module, &log_path);
  // A netplay run writes everything (NOTE lines, ERROR lines, RESTORE lines) to the net log, the one file tools/rollback/phase4.py reads.
  Sink& sink = (net && module.net_sink != nullptr) ? static_cast<Sink&>(*module.net_sink) : static_cast<Sink&>(*module.sink);
  sink.Line(Format("NOTE alarm queue watch %s, IOS gate %s (REV3_ALARMCHECK / REV3_RB_IOS_GATE)", options.alarm_check != 0 ? "on" : "off",
                   options.ios_gate ? "on" : "off"));
  if (net)
  {
    options.restore_log_full = Net::LOG_FULL_LINES;
    options.restore_log_window = Net::LOG_WINDOW_FRAMES;
  }
  std::vector<Range> verify_ignore;
  ParseVerifyEnv(&options, &verify_ignore, sink);  // REV3_RB_VERIFY*: restore verify (off unless REV3_RB_VERIFY is set), also in a netplay run
  ParseWriteTrackEnv(&options, sink);               // REV3_RB_WRITE_TRACK*: host write tracking (off unless REV3_RB_WRITE_TRACK is 1 / 2)
  ParseHashCheckEnv(&options, sink);                // REV3_RB_HASHCHECK: all-memory page hash comparison (off unless set)

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
    sink.Line("NOTE no REV3_WHITELIST: the snapshot set is learned in the match (adaptive)");
  }
  if (!error.empty())
  {
    const std::string message = "Rev3 SyncTest: whitelist unusable (" + error + "); SyncTest is off";
    sink.Line("ERROR " + message);
    ERROR_LOG_FMT(SLIPPI, "{}", message);
    sink.Osd(message);
    return false;
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
      ApplyExclusions(exclude_text, &ranges, sink, &error, &user_exclusions, !options.adaptive,
                       &ptr_exclusions);
    if (!error.empty())
    {
      const std::string message = "Rev3 SyncTest: exclude file unusable (" + error + "); SyncTest is off";
      sink.Line("ERROR " + message);
      ERROR_LOG_FMT(SLIPPI, "{}", message);
      sink.Osd(message);
      return false;
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
      LoadInclusions(include_text, sink, &include_ranges, &include_ptrs, &error);
    if (!error.empty())
    {
      const std::string message = "Rev3 SyncTest: include file unusable (" + error + "); SyncTest is off";
      sink.Line("ERROR " + message);
      ERROR_LOG_FMT(SLIPPI, "{}", message);
      sink.Osd(message);
      return false;
    }
  }

  if (hook_bytes != 0)
  {
    sink.Line(Format("NOTE removed %llu bytes of the hook region 0x%08X-0x%08X from the whitelist",
                             static_cast<unsigned long long>(hook_bytes), HOOK_REGION_START, HOOK_REGION_END));
  }
  if (options.learn_pad_pages != 0)
  {
    sink.Line(Format("NOTE learn pad pages=%u (REV3_SNAPSHOT_PAD): every learned page also takes %u KB on each side into the snapshot set",
                             options.learn_pad_pages, options.learn_pad_pages * PAGE_SIZE / 1024));
  }
  if (hardware_exclude)
    module.hardware = std::make_unique<DolphinHardware>(system, sink);
  module.synctest = std::make_unique<SyncTest>(options, std::move(ranges), *module.memory, sink, std::function<u64()>(),
                                               module.hardware.get());
  module.synctest->SetUserExclusions(std::move(user_exclusions), std::move(ptr_exclusions));
  module.synctest->SetUserInclusions(std::move(include_ranges), std::move(include_ptrs));
  module.synctest->SetVerifyIgnore(std::move(verify_ignore));
  if (options.write_track != 0 && options.adaptive)
  {
    module.watch = std::make_unique<DolphinWriteWatch>(system);
    module.synctest->SetWriteWatch(module.watch.get());
  }
  if (options.restore_timebase)
  {
    sink.Line("NOTE timebase accessor: read SystemTimers::GetFakeTimeBase(); write CoreTiming::SetFakeTBStartValue(start + (value - now)) "
                      "and PowerPCManager::WriteFullTimeBaseValue(value); decrementer and CoreTiming events untouched");
  }
  if (GetEnv("REV3_SYNCTEST_AUDIO_HW") == "0")
  {
    sink.Line("NOTE audio hw rewind off (REV3_SYNCTEST_AUDIO_HW=0): memory only");
  }
  else
  {
    module.audio = std::make_unique<DolphinAudioHw>(system);
    module.synctest->SetAudioHardware(module.audio.get());
  }
  Core::System* const sys = &system;
  module.synctest->SetDspState([sys]() { return DescribeDsp(*sys); });
  AttachDump(module);  // RESTOREHW lines around the first restores (into the net log in a netplay run)
  // REV3_SYNCTEST_HIDE=1: SyncTest sets the protocol 3 hiding flags (REV3_RESIM_HIDE, default draw) while it resimulates, so a SyncTest run
  // tests the patches: the hidden resimulation must reproduce the drawn original run.
  if (!net && GetEnv("REV3_SYNCTEST_HIDE") == "1")
  {
    const u32 flags = ReadResimHide(sink);
    module.hide = std::make_unique<ResimHideFlags>(*module.memory, flags, &sink, ReadDetVoice(sink));
    module.synctest->SetResimHide(module.hide.get());
    sink.Line(Format("NOTE REV3_SYNCTEST_HIDE=1: resim hiding flags=%u are set during every resimulation (needs hook protocol 3)", flags));
  }
  if (!net)
    sink.Osd(Format("Rev3 SyncTest on: K=%d, every %d frame(s), log %s", options.k, options.every, log_path.c_str()));
  return true;
}

// Collects what HELLO compares from Dolphin's configuration (Rev3NetLink.h HelloInputs). Read on the CPU thread at the first command.
Net::HelloInputs CollectHelloInputs(u32 stable_crc)
{
  Net::HelloInputs in;
  in.build = Common::GetScmRevGitStr();
  in.game_id = SConfig::GetInstance().GetGameID();
  switch (Config::Get(Config::MAIN_CPU_CORE))
  {
  case PowerPC::CPUCore::Interpreter:
    in.cpu_core = "Interpreter";
    break;
  case PowerPC::CPUCore::JIT64:
    in.cpu_core = "JIT64";
    break;
  case PowerPC::CPUCore::JITARM64:
    in.cpu_core = "JITARM64";
    break;
  case PowerPC::CPUCore::CachedInterpreter:
    in.cpu_core = "CachedInterpreter";
    break;
  default:
    in.cpu_core = Format("core%d", static_cast<int>(Config::Get(Config::MAIN_CPU_CORE)));
    break;
  }
  in.cpu_thread = Config::Get(Config::MAIN_CPU_THREAD);
  in.sync_gpu = Config::Get(Config::MAIN_SYNC_GPU);
  in.dsp_hle = Config::Get(Config::MAIN_DSP_HLE);
  in.dsp_thread = Config::Get(Config::MAIN_DSP_THREAD);
  in.rtc_enabled = Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE);
  in.rtc_value = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
  in.overclock_enabled = Config::Get(Config::MAIN_OVERCLOCK_ENABLE);
  in.overclock = Config::Get(Config::MAIN_OVERCLOCK);
  in.mmu = Config::Get(Config::MAIN_MMU);
  in.fast_disc_speed = Config::Get(Config::MAIN_FAST_DISC_SPEED);
  in.cheats = Config::Get(Config::MAIN_ENABLE_CHEATS);
  in.bluetooth_passthrough = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED);
  for (int i = 0; i < 4; ++i)
    in.wiimote_sources[static_cast<size_t>(i)] = static_cast<int>(Config::Get(Config::GetInfoForWiimoteSource(i)));
  in.wiimote_sources[4] = static_cast<int>(Config::Get(Config::WIIMOTE_BB_SOURCE));
  in.stable_crc = stable_crc;
  return in;
}

// Hiding of resimulated frames (REV3.md "Netplay (M4)", "Resimulation hiding (M6)"). v1: throttle off + audio OUTPUT muted, both only while
// the session resimulates. With hook protocol 3 the game's own draw / sound skip is switched on too, by the flag word ResimHideFlags writes.
//   throttle  Core::SetIsThrottlerResimDisabled: its own flag, because the hotkey scheduler rewrites the user's temp-disable flag every loop.
//   mute      the sound stream's volume goes to 0 (SoundStream::SetVolume; backends without volume control ignore it) and back to the
//             user's setting (AudioCommon::UpdateSoundStream: volume, or 0 when the user has muted). The DSP is never paused or touched,
//             so the emulated audio hardware keeps its timing and the next real frame sounds right.
class DolphinResimHider final : public Net::ResimHider
{
public:
  // `flags` (optional, not owned) must outlive this object.
  DolphinResimHider(Core::System& system, ResimHideFlags* flags) : m_system(system), m_flags(flags) {}
  ~DolphinResimHider() override { SetHidden(false); }

  void SetHidden(bool hidden) override
  {
    if (m_flags != nullptr)
      m_flags->Set(hidden);  // protocol 3 only: the flags while hidden, 0 otherwise (also on leave and at shutdown, which come through here)
    Core::SetIsThrottlerResimDisabled(hidden);
    if (hidden)
    {
      if (SoundStream* stream = m_system.GetSoundStream(); stream != nullptr)
        stream->SetVolume(0);
    }
    else
    {
      AudioCommon::UpdateSoundStream(m_system);
    }
  }

private:
  Core::System& m_system;
  ResimHideFlags* m_flags;
};

// Pacing delays (M5, REV3.md "Netplay (M5)"): the session asks this machine to lose a few milliseconds because it runs ahead of its peer.
// CoreTimingManager::SleepAndResetThrottle sleeps and restarts the throttle clock, which is what makes the loss permanent (a bare sleep would
// be made up by the next frames running unthrottled). Nothing to do while the limiter is off (unlimited speed, fast-forward, a hidden
// resimulation): there is no schedule to be ahead of, and the throttle resets itself on every call then.
class DolphinPacer final : public Net::PaceActuator
{
public:
  explicit DolphinPacer(Core::System& system) : m_system(system) {}

  void Delay(u32 microseconds) override
  {
    if (Core::GetIsThrottlerTempDisabled() || Config::Get(Config::MAIN_EMULATION_SPEED) <= 0.0f)
      return;
    m_system.GetCoreTiming().SleepAndResetThrottle(std::chrono::duration_cast<DT>(std::chrono::microseconds(microseconds)));
  }

private:
  Core::System& m_system;
};

// REV3_NET_ROLE is set: a netplay run (REV3.md "Netplay (M3)"). SyncTest is off in this mode; the snapshot machinery (SyncTest's network
// mode) is only built when REV3_NET_ROLLBACK=1. Everything that goes wrong is logged to the Rev3 log and the net log and the game then
// runs offline (the reply echoes the pads).
void InitializeNet(Module& module, Core::System& system)
{
  FileSink& rev3_log = EnsureSink(module);
  const Net::NetOptions options = Net::ParseNetOptions([](const char* name) { return GetEnv(name); });
  if (!options.ok)
  {
    const std::string message = "Rev3 netplay: " + options.error + "; playing offline";
    rev3_log.Line("ERROR " + message);
    ERROR_LOG_FMT(SLIPPI, "{}", message);
    rev3_log.Osd(message);
    InitializeDump(module, system, rev3_log);  // playing offline: the dump still works, in the Rev3 log
    return;
  }
  const std::string net_path = options.log_path.empty() ? File::GetUserPath(D_LOGS_IDX) + "rev3_net_" + Net::RoleText(options.role) + ".log" :
                                                          options.log_path;
  module.net_sink = std::make_unique<FileSink>(net_path, "Rev3 netplay");
  FileSink& log = *module.net_sink;
  rev3_log.Line("NOTE netplay: net log " + net_path);
  InitializeDump(module, system, log);  // freeze dump into the net log (REV3_NET_LOG)
  for (const std::string& warning : options.warnings)
  {
    log.Line("NOTE " + warning);
    rev3_log.Line("NOTE " + warning);
  }
  if (!GetEnv("REV3_SYNCTEST").empty() && GetEnv("REV3_SYNCTEST") != "0")
    log.Line("NOTE REV3_SYNCTEST is ignored in a netplay run (REV3_NET_ROLE is set)");

  // The ranges START_CHECK compares (optional); their file's CRC goes into HELLO so both sides use the same file.
  std::vector<Range> stable_ranges;
  u32 stable_crc = 0;
  if (!options.stable_path.empty())
  {
    std::string text;
    std::string error;
    if (!ReadTextFile(options.stable_path, &text))
      error = "cannot read " + options.stable_path;
    else if (ParseWhitelist(text, &stable_ranges, &error))
      stable_crc = Common::ComputeCRC32(text);
    if (!error.empty())
    {
      log.Line("ERROR REV3_NET_STABLE: " + error + "; no stable-range check");
      stable_ranges.clear();
    }
  }

  if (options.rollback && !BuildSyncTest(module, system, options.max_rollback + 1, true))
  {
    log.Line("ERROR the snapshot machinery could not be built (see the Rev3 log); REV3_NET_ROLLBACK is treated as 0 (lockstep only)");
    module.synctest.reset();
  }

  Net::NetEnvironment env;
  Core::System* const sys = &system;
  env.stopping = [sys]() {
    return Core::GetState(*sys) == Core::State::Stopping || sys->GetCPU().GetState() == CPU::State::PowerDown;
  };
  env.hello = CollectHelloInputs(stable_crc);
  module.hide = std::make_unique<ResimHideFlags>(*module.memory, ReadResimHide(log), &log, ReadDetVoice(log));
  module.hider = std::make_unique<DolphinResimHider>(system, module.hide.get());
  env.hider = module.hider.get();
  module.pacer = std::make_unique<DolphinPacer>(system);
  env.pacer = module.pacer.get();
  env.dump = module.dump.get();  // the session's wait loops mark the dump blocked: waiting for the peer is not a freeze
  module.net = std::make_unique<Net::NetModule>(options, env, *module.memory, module.hardware.get(), module.synctest.get(),
                                                std::move(stable_ranges), log);
  if (module.synctest != nullptr)
    module.synctest->SetHideCounters(module.hide.get());  // the hidden iteration / skipped sound counters go into the SUMMARY lines
}

// The soak bot and exits (Rev3Bot.h): REV3_PAD_BOT, REV3_NET_EXIT, REV3_NET_EXIT_WAIT_S, REV3_NET_FREEZE_EXIT_S, REV3_NET_MAX_S. Their lines (BOT ..., EXIT ...)
// go to the net log in a netplay run, else to the Rev3 log. A variable that is set but unusable is an ERROR line and that part stays off.
void InitializeBot(Module& module)
{
  const Bot::BotOptions options = Bot::ParseBotOptions([](const char* name) { return GetEnv(name); });
  if (!options.Any() && options.warnings.empty())
    return;
  Sink& log = module.net_sink != nullptr ? static_cast<Sink&>(*module.net_sink) : static_cast<Sink&>(EnsureSink(module));
  for (const std::string& warning : options.warnings)
  {
    log.Line("ERROR " + warning);
    ERROR_LOG_FMT(SLIPPI, "Rev3: {}", warning);
  }
  module.bot_options = options;
  Sink* const sink = &log;
  const Bot::LogFn line = [sink](const std::string& text) { sink->Line(text); };
  const u32 port = GetEnv("REV3_NET_ROLE") == "guest" ? 1 : 0;  // the game port of this side (host 0, guest 1); offline: 0
  if (options.bot && module.kit != nullptr)
  {
    module.bot = std::make_unique<Bot::PadBotController>(options.seed, port, line);
    module.kit->SetBot(module.bot.get());
    log.Line(Format("NOTE bot on (REV3_PAD_BOT=%llu): the local pad is played by the bot, port %u; scripted menus first if REV3_PAD_SCRIPT is set; "
                    "it takes over at the first call with both fighters found or past the script's end",
                    static_cast<unsigned long long>(options.seed), port));
  }
  if (options.exit.enabled)
  {
    module.exit_ctl = std::make_unique<Bot::ExitController>(options.exit, options.tuning, line);
    log.Line(Format("NOTE automatic exit %s: %u s after the decision the emulator is asked to stop (BYE to the peer, SUMMARY, logs flushed)",
                    Bot::ExitSpecText(options.exit).c_str(), options.tuning.wait_calls / Bot::EXIT_CALLS_PER_SECOND));
  }
  if (options.freeze_exit_s != 0 && module.dump == nullptr)
  {
    log.Line("ERROR REV3_NET_FREEZE_EXIT_S needs the freeze dump (REV3_SYNCTEST_DUMP is 0); no freeze exit");
  }
  const bool freeze = options.freeze_exit_s != 0 && module.dump != nullptr;
  if (options.exit.enabled || freeze || options.max_s != 0)
  {
    module.exit_watcher = std::make_unique<ExitWatcher>(freeze ? module.dump.get() : nullptr, log, freeze ? options.freeze_exit_s : 0, options.max_s,
                                                        []() { Host_Message(HostMessageID::WMUserStop); });
    log.Line(Format("NOTE exit watcher: freeze_exit_s=%u max_s=%u stop_grace_ms=%u (exit codes: freeze %d, max_s %d, stop hang %d, unplanned leave %d)",
                    freeze ? options.freeze_exit_s : 0, options.max_s, Bot::STOP_GRACE_MS, Bot::EXIT_CODE_FREEZE, Bot::EXIT_CODE_MAX_S,
                    Bot::EXIT_CODE_STOP_HANG, Bot::EXIT_CODE_LEFT));
  }
}

void Initialize(Module& module, Core::System& system)
{
  module.initialized = true;

  // The determinism kit (pad record / script, trace) is independent of SyncTest and works with it off.
  module.memory = std::make_unique<DolphinMemory>(system);
  InitializeKit(module);

  if (NetplayRequested())
  {
    InitializeNet(module, system);  // creates the freeze dump too, on the net log
    InitializeBot(module);
    return;
  }
  InitializeDump(module, system, EnsureSink(module));  // the freeze dump works with SyncTest off too
  InitializeBot(module);

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
  BuildSyncTest(module, system, k, false);
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

// REV3_RB_HASHCHECK=N (frames, default 0 = off): hash all of guest memory every Nth frame in the first run and in the resimulation (HASHCHECK lines).
void ParseHashCheckEnv(SyncTestOptions* options, Sink& sink)
{
  const std::string text = GetEnv("REV3_RB_HASHCHECK");
  if (text.empty())
    return;
  long value = 0;
  if (ParseInt(text, &value) && value >= 0 && value <= 100000)
    options->hash_check_every = static_cast<u32>(value);
  else
  {
    sink.Line("ERROR REV3_RB_HASHCHECK=" + text + " is not 0..100000; the hash check is off");
    ERROR_LOG_FMT(SLIPPI, "Rev3: REV3_RB_HASHCHECK={} is not 0..100000; the hash check is off", text);
  }
}

// REV3_RB_WRITE_TRACK=0|1|2 (default 0; 1 = faults learn the pages and the hash scan is skipped, 2 = faults and the hash scan, misses reported),
// REV3_RB_DIRTY_LOG=N (log the first N dirty pages with the pc / lr of the store), REV3_RB_WRITE_TRACK_REARM=N (1..200).
void ParseWriteTrackEnv(SyncTestOptions* options, Sink& sink)
{
  const auto fail = [&sink](const std::string& what) {
    sink.Line("ERROR " + what);
    ERROR_LOG_FMT(SLIPPI, "Rev3: {}", what);
  };
  long value = 0;
  const std::string mode = GetEnv("REV3_RB_WRITE_TRACK");
  if (mode.empty())
    return;
  if (ParseInt(mode, &value) && value >= 0 && value <= 2)
    options->write_track = static_cast<u32>(value);
  else
    fail("REV3_RB_WRITE_TRACK=" + mode + " is not 0, 1 or 2; write tracking is off");
  if (options->write_track == 0)
    return;
  if (!options->adaptive)
  {
    fail("REV3_RB_WRITE_TRACK needs the adaptive snapshot set (REV3_SYNCTEST_ADAPTIVE is 0); write tracking is off");
    options->write_track = 0;
    return;
  }
  const std::string dirty = GetEnv("REV3_RB_DIRTY_LOG");
  if (!dirty.empty())
  {
    if (ParseInt(dirty, &value) && value >= 0 && value <= 1000000)
      options->dirty_log = static_cast<u32>(value);
    else
      fail("REV3_RB_DIRTY_LOG=" + dirty + " is not 0..1000000; the dirty log is off");
  }
  const std::string rearm = GetEnv("REV3_RB_WRITE_TRACK_REARM");
  if (!rearm.empty())
  {
    if (ParseInt(rearm, &value) && value >= 1 && value <= 200)
      options->write_track_rearm_max = static_cast<u32>(value);
    else
      fail("REV3_RB_WRITE_TRACK_REARM=" + rearm + " is not 1..200; using 4");
  }
  sink.Line(Format("NOTE write tracking requested: REV3_RB_WRITE_TRACK=%u dirty_log=%u rearm_max=%u", options->write_track, options->dirty_log,
                   options->write_track_rearm_max));
}

// REV3_NET_EXIT: one hook call that is not a resimulation. When the controller says the run is over: tell the session why (the BYE to the peer
// carries it), note the stop for the watcher's grace timer, and ask the host to stop the emulation the normal way (Host_Message WMUserStop: the Qt
// main window's RequestStop; the process ends when it was started with -b, rev3-mod soak.py does that). Dolphin then destroys the EXI device,
// which runs Rev3::Shutdown (BYE, SUMMARY, logs flushed).
void ObserveExit(Module& module, u32 frame)
{
  if (module.exit_ctl == nullptr || module.memory == nullptr)
    return;
  const Bot::ExitStep step = module.exit_ctl->OnCall(frame, ReadGameObs(*module.memory));
  if (!step.stop)
    return;
  Sink* const log = module.net_sink != nullptr ? static_cast<Sink*>(module.net_sink.get()) : static_cast<Sink*>(module.sink.get());
  if (log != nullptr)
    log->Line(Format("EXIT stop requested frame=%u reason=\"%s\" ko=%u", frame, step.reason.c_str(), module.exit_ctl->Kos()));
  if (module.net != nullptr && !step.left)  // after an unplanned leave the session is offline already: no BYE goes out
    module.net->SetExitReason("planned exit: " + step.reason);
  if (module.exit_watcher != nullptr)
    module.exit_watcher->NoteStop();
  Host_Message(HostMessageID::WMUserStop);
}

// REV3_NET_EXIT and the net session left for a reason that is not a planned exit (a timeout, a desync, a lost link; D's soak, Oct 16: run_2 and
// run_6 carried on offline until the runner killed them): the controller decides like for a planned exit (stop after REV3_NET_EXIT_WAIT_S), and
// the process ends with Bot::EXIT_CODE_LEFT (see Shutdown), so the runner reports PROBLEM with the LEAVE reason instead of a TIMEOUT.
void ObserveLeave(Module& module, u32 frame)
{
  std::string why;
  if (module.net == nullptr || !module.net->TakeUnplannedLeave(&why))
    return;
  if (module.exit_ctl == nullptr)
    return;  // REV3_NET_EXIT is off: the session left and the game goes on offline, as always
  if (module.exit_ctl->NoteLeave(frame, why))
    module.exit_code = Bot::EXIT_CODE_LEFT;
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

  // The freeze dump's watchdog: this command proves the game is alive (any mode, netplay included).
  if (module.dump != nullptr)
    module.dump->Tick(parsed.frame);
  if (module.hide != nullptr)
    module.hide->SetProtocol(parsed.protocol);  // netplay: the first command decides whether the flag word exists (HELLO carries it too)

  // Netplay (REV3_NET_ROLE set): the session answers every command. The kit still goes first and substitutes the LOCAL pad (command port 0)
  // before the session sees it (REV3_PAD_SCRIPT on the guest window); the record then takes what the game got from the reply.
  if (module.net != nullptr && Core::IsCPUThread())
  {
    const bool resimulating = module.net->Resimulating();
    if (module.kit != nullptr)
      module.kit->OnCommand(&parsed, resimulating);
    if (!resimulating)
      ObserveExit(module, parsed.frame);
    reply = module.net->OnCommand(parsed);
    ObserveLeave(module, parsed.frame);
    if (module.kit != nullptr)
      module.kit->OnReply(parsed, reply, resimulating);
    read_queue->assign(REPLY_SIZE, 0);
    EncodeReply(reply, read_queue->data());
    return;
  }

  // The determinism kit sees every hook call first: it may replace the pads (REV3_PAD_SCRIPT), then SyncTest (if on) works with
  // those pads, and the reply carries them.
  const bool resimulating = module.synctest != nullptr && module.synctest->Resimulating();
  if (module.kit != nullptr && Core::IsCPUThread())
    module.kit->OnCommand(&parsed, resimulating);
  if (!resimulating && Core::IsCPUThread())
    ObserveExit(module, parsed.frame);

  if (module.synctest != nullptr && Core::IsCPUThread())
    reply = module.synctest->OnCommand(parsed);
  else
  {
    reply.flags = 0;
    reply.frame = parsed.frame;
    reply.pads = parsed.pads;
  }
  if (module.kit != nullptr && Core::IsCPUThread())
    module.kit->OnReply(parsed, reply, resimulating);  // only acts in a netplay run that fell back to offline (record_reply)

  read_queue->assign(REPLY_SIZE, 0);
  EncodeReply(reply, read_queue->data());
}

bool NetplayRequested()
{
  const char* role = std::getenv("REV3_NET_ROLE");
  return role != nullptr && role[0] != '\0';
}

void Shutdown()
{
  Module& module = GetModule();
  if (module.net != nullptr)
  {
    module.net->Shutdown();  // BYE to the peer, SUMMARY, stats, network thread joined
    module.net.reset();
  }
  module.pacer.reset();
  bool exit_watcher_stuck = false;
  if (module.exit_watcher != nullptr)
  {
    exit_watcher_stuck = !module.exit_watcher->Stop(std::chrono::milliseconds(1500));  // before the freeze dump it reads goes away
    module.exit_watcher.reset();
  }
  module.exit_ctl.reset();
  if (module.kit != nullptr)
    module.kit->SetBot(nullptr);
  module.bot.reset();
  module.hider.reset();  // hiding off (a no-op when the session already restored it): the flag word is 0 again
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
  // A netplay run's SyncTest writes into the net log, whose SUMMARY is the session's: no Finish() there (it would write a second one).
  if (module.synctest != nullptr && module.net_sink == nullptr)
    module.synctest->Finish();
  module.synctest.reset();
  if (module.hide != nullptr)
  {
    module.hide->Set(false);  // never leave the game hiding (netplay: the hider has done it already; SyncTest: Finish did)
    module.hide.reset();
  }
  module.watch.reset();  // everything writable again, the fault hook removed (the guest memory is still mapped)
  if (watchdog_stuck || exit_watcher_stuck)
  {
    // The detached watchdog thread still points at these (it only touches them after the CPU pause returns, and then checks its stop
    // flag first). Leak them instead of freeing them under it.
    static_cast<void>(module.dump.release());
    static_cast<void>(module.dump_source.release());
    static_cast<void>(module.memory.release());
    static_cast<void>(module.sink.release());
    static_cast<void>(module.net_sink.release());
  }
  module.dump.reset();
  module.dump_source.reset();
  module.audio.reset();
  module.hardware.reset();
  module.net_sink.reset();  // after everything that logs into it
  module.memory.reset();
  module.sink.reset();
  module.initialized = false;  // the next command reads the environment again and appends to the log
  if (module.exit_code != 0)
  {
    // The net session left unplanned and REV3_NET_EXIT stopped the run (ObserveLeave): Dolphin would exit 0 after a normal stop, so end the process
    // here with the code the runner looks for. Every log was flushed and closed above; the rest of Dolphin's shutdown is not needed in a soak run.
    const int code = module.exit_code;
    std::_Exit(code);
  }
}
}  // namespace Rev3
