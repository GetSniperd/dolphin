// Rev3 rollback support for Naruto Clash of Ninja Revolution 3 (Rev3, game ID RNEEDA): the emulator side of the game's
// per-frame EXI hook. Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT DOES
//   Rev3's input_handler calls a hook (rev3-mod rbhook.py) once per game frame. The hook DMA-writes a 64-byte command
//   (id 0xA0, "RBK1", the frame number, both PADStatus blocks, the hook's stack pointer) to the Slippi EXI device (channel 1,
//   chip select 1) and DMA-reads a 64-byte reply ("RBR1", flags, frame, two pads). CEXISlippi::DMAWrite hands such a command
//   to Rev3::HandleCommand() before its Melee dispatch, and the reply goes into the device's read queue. Melee traffic is
//   untouched (the id 0xA0 AND the magic must match).
//   - Without SyncTest the reply echoes both pads of the command with flags 0, so the game behaves as without the fork.
//   - SyncTest (GGPO's term) is a one-player self test of the rollback machinery, with no netplay: every EVERY-th frame it
//     restores the state of K frames ago (whitelist memory ranges + the hook's stack window), makes the game resimulate those
//     K frames with the recorded inputs, and compares the fighters' fields and a CRC32 of every whitelist range, frame by
//     frame, with what the first run produced. A difference means the whitelist is incomplete or the hook point is not
//     equivalent; it is logged with the frame, the fighter fields and the ranges involved.
//
// HOW TO ENABLE (environment variables, read once when the first command arrives)
//   REV3_SYNCTEST=K          1..8 = roll back K frames; absent / 0 = off (anything else is reported and treated as off)
//   REV3_WHITELIST=path      whitelist.json: {"ranges":[{"start":"0x803B7240","end":"0x804DEC00"}, ...]}; start / end are hex
//                            strings or numbers, end exclusive. An optional seed of the adaptive snapshot set; required only with
//                            REV3_SYNCTEST_ADAPTIVE=0.
//   REV3_SYNCTEST_EVERY=N    roll back when frame % N == 0 (default 1 = every frame)
//   REV3_STACK_WINDOW=bytes  bytes saved and restored from the hook's sp upward (default 0x4000; decimal or 0x hex)
//   REV3_SYNCTEST_LOG=path   log file (default <User>/Logs/rev3_synctest.log); appended to, flushed after every line
//   REV3_SYNCTEST_ARM=N|pad  when SyncTest arms: N = after N consecutive hook calls with both fighters found (default 600 =
//                            10 s; the stage must have finished loading); pad = when P1 presses Z (and disarm on the next
//                            press) while fighters are present
//   REV3_SYNCTEST_NOOP=1     diagnosis: every frame capture and immediately restore the same frame (RESTORED reply, frame n,
//                            inputs n); exercises the game hook's register reload without any real rollback; no comparisons
//   REV3_SYNCTEST_HW_EXCLUDE=0   do not cut hardware-DMA regions (GX FIFO, audio DMA, XFB, DVD DMA) out of the snapshot
//   REV3_SYNCTEST_ICACHE=0       do not invalidate the JIT for restored ranges
//   REV3_SYNCTEST_TB=1           also store the emulated time base (what the game's OSGetTime reads) in every snapshot and set it
//                                back on restore (default 0). Decrementer and CoreTiming are left alone.
//   REV3_SYNCTEST_EXCLUDE=path   JSON {"ranges":[{"start","end"}]} subtracted from the snapshot (logged as NOTE exclude user ...),
//                                to test excluding audio / scratch ranges without editing the whitelist
//   REV3_SYNCTEST_AUDIO_HW=0 do not rewind the emulated audio hardware (DSP, AI, their CoreTiming events and PI interrupt bits)
//                            together with memory (default 1, see "THE AUDIO HARDWARE" below)
//   REV3_SYNCTEST_ADAPTIVE=0 do not learn the snapshot set in the running match (default 1: adaptive, see below)
//   REV3_SYNCTEST_SCAN_SLICES=N  adaptive: every hook call hashes 1/N of all 4 KB pages of MEM1 + MEM2 (default 16)
//   REV3_SYNCTEST_LEARN=N    adaptive: scan cycles that must complete with fighters present before SyncTest may arm (default 20)
//   SyncTest only runs once armed (both fighters found through the TEAMS pointers); menus and loading screens are never
//   rolled back. See REV3.md for what a clean run looks like.
//
// DETERMINISM KIT (milestone M1; works with SyncTest off, and alongside it; REV3.md "Determinism kit (M1)")
//   REV3_PAD_RECORD=file     write "# rev3 pad record v1", then one line per hook call that is not a resimulation:
//                            "<frame> <port0 pad, 24 hex> <port1 pad, 24 hex> <padread return, signed>"; flushed every 60 lines
//   REV3_PAD_SCRIPT=file     same format; for every listed frame the reply carries the listed pads instead of the live ones
//                            (other frames: live pads); "NOTE pad script ended at frame N" once the last frame has passed
//   REV3_TRACE=file          every REV3_TRACE_EVERY-th frame (default 30) append
//                            "T frame=N fighters=0|2 chk=0x........ mem=0x........" (fighter checksum, CRC32 of the trace ranges)
//   REV3_TRACE_RANGES=file   {"ranges":[{"start","end"}]} (hex strings or numbers, end exclusive): the bytes `mem` covers
//   The record, the script and the trace are plain text; the runner is rev3-mod tools/rollback/phase4.py. The fighter checksum is
//   rev3-mod rev3mod/rbchecksum.py checksum(), bit for bit (FighterChecksum below).
//
// THE ADAPTIVE SNAPSHOT SET (REV3_SYNCTEST_ADAPTIVE, default on)
//   A per-match page list cannot work: the heap layout differs from match to match (the whitelist learned in one match left P2's
//   fighter struct out of the snapshot in the next). So the set of memory that is snapshotted is learned in the running match:
//   from the moment both fighters are first found, a PageTracker hashes every 4 KB page of MEM1 / MEM2 (1/SLICES of them per hook
//   call); a page whose hash changed between two of its scans is "dynamic" (sticky). The snapshot set is
//     (REV3_WHITELIST ranges, now an optional seed) + dynamic pages + both fighter structs (fighter .. +0x1800) + the hook stack
//     window, minus the hook region, REV3_SYNCTEST_EXCLUDE ranges and the hardware exclusions.
//   SyncTest arms only after LEARN full scan cycles; a page outside the set that changes later on a non-resimulated frame is
//   added (layout rebuilt, history cleared). Resimulated frames are never scanned and a restore resets the baseline of the pages
//   it rewrote, so rolling back cannot teach it anything.
//
// THE AUDIO HARDWARE (REV3_SYNCTEST_AUDIO_HW, default on)
//   The audio runs on its own hardware clock (AI DMA every ~5 ms, DSP mail / interrupt handshakes, sound threads woken by them).
//   Rewinding only memory rewinds the game's audio driver (AX's DSP handshake state, the sound threads' stacks) while the emulated
//   DSP and AI move on, and the audio pipeline (and whatever waits on it) hangs. So every capture also saves the audio hardware
//   (AudioHardware below) and every restore puts it back, right after the memory write-back and the JIT invalidation:
//   the DSP manager + DSP emulator state, the AI registers, the CoreTiming events of those devices (as remaining cycles, so
//   they stay in phase with the restored hardware state) and the DSP / AI interrupt bits of the PI. Nothing else of CoreTiming,
//   the PI or the PPC is touched.
//
// HOW IT FITS IN
//   Protocol and field table: GetSniperd/rev3-mod rev3mod/rbhook.py (game side), rev3mod/rbchecksum.py (FIELDS, TEAMS chain),
//   docs/rollback_fork_plan.md (the change list; SyncTest is its first milestone). The Dolphin glue (HandleCommand, the
//   memory accessor, the log file) is at the bottom of Rev3Rollback.cpp; the state machine (Rev3::SyncTest) only talks to the
//   small GuestMemory / Sink interfaces below, so Source/UnitTests/Core/Rev3RollbackTest.cpp drives it with a fake memory.
//
// THREADING
//   HandleCommand runs on the CPU thread inside CEXISlippi::DMAWrite (an EXI DMA is an MMIO store of the CPU thread, and
//   SlippiSavestate's Capture / Load also run there). The emulator never touches PPC registers: the game's hook reloads its
//   own registers from its (restored) stack frame.

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

namespace Core
{
class System;
}

namespace Rev3
{
// ---- the wire protocol (version 2), all big-endian ------------------------------------------------------------------------
constexpr u8 CMD_ID = 0xA0;                // +0x00
constexpr u8 PROTOCOL_VERSION = 2;         // +0x01
constexpr u32 CMD_MAGIC = 0x52424B31;      // +0x04 "RBK1"
constexpr u32 REPLY_MAGIC = 0x52425231;    // +0x00 of the reply, "RBR1"
constexpr size_t CMD_SIZE = 64;
constexpr size_t REPLY_SIZE = 64;
constexpr u32 REPLY_FLAG_RESTORED = 1;     // reply flags bit 0: guest memory was rewritten to the command of `frame`

using PadBytes = std::array<u8, 12>;       // one PADStatus (buttons u16, stickX/Y, substickX/Y, triggerL/R, analogA/B, err), raw

struct Command
{
  u32 frame = 0;                           // +0x08 the hook's frame counter
  std::array<PadBytes, 2> pads{};          // +0x0C port 0, +0x18 port 1
  s32 pad_return = 0;                      // +0x24 return value of the real PADRead
  u32 sp = 0;                              // +0x28 the hook's r1; its saved registers live at sp..sp+0x200
};

struct Reply
{
  u32 flags = 0;                           // bit 0 = REPLY_FLAG_RESTORED
  u32 frame = 0;                           // the frame the hook must continue from (read when RESTORED)
  std::array<PadBytes, 2> pads{};          // +0x10, +0x1C
};

// True when `data` is a command of ours: size >= 64, byte 0 == 0xA0, magic "RBK1" at +4. (Version is checked by ParseCommand.)
bool IsCommand(const u8* data, size_t size);
bool ParseCommand(const u8* data, size_t size, Command* out);  // false: not ours or an unknown version
void EncodeReply(const Reply& reply, u8* out64);                 // fills exactly REPLY_SIZE bytes

// ---- whitelist ------------------------------------------------------------------------------------------------------------
struct Range
{
  u32 start = 0;
  u32 end = 0;  // exclusive
};

// Parses {"ranges":[{"start":"0x80...","end":"0x80..."} | {"start":2147483648,"end":...}]}. Start / end may be hex ("0x..") or
// decimal strings or numbers. Rejects empty lists, end <= start and ranges outside MEM1 / MEM2. On failure returns false and
// sets *error. The list is kept in file order (overlaps are allowed; they are saved twice, which is harmless).
bool ParseWhitelist(const std::string& json_text, std::vector<Range>* ranges, std::string* error);

// The hook's own code and data (command / reply buffers, frame counter) live at 0x80002400-0x80002A00 (rev3-mod rbhook.py). They must
// survive a restore (the hook sets its counter itself), so they never belong in a snapshot.
constexpr u32 HOOK_REGION_START = 0x80002400;
constexpr u32 HOOK_REGION_END = 0x80002A00;  // exclusive

// Removes [lo, hi) from every range (a range around it is split in two, one inside it disappears). Returns the bytes removed.
u64 SubtractRange(std::vector<Range>* ranges, u32 lo, u32 hi);

class Sink;  // defined below

// A REV3_SYNCTEST_EXCLUDE entry whose address is read from guest memory (see ApplyExclusions).
struct PtrExclusion
{
  u32 ptr = 0;   // address of the u32 holding the start of the region
  u32 size = 0;  // bytes to exclude from that value on
  std::string note;
};

// REV3_SYNCTEST_EXCLUDE: parses `json_text` like a whitelist ({"ranges":[...]}) and subtracts each range from *ranges, logging one
// "NOTE exclude user 0xSTART-0xEND (n bytes were in the whitelist)" line per range to `sink`. False (and *ranges untouched) when the
// JSON is bad or, with require_nonempty, the exclusions would leave nothing; *error says why. `parsed` (optional) receives the
// exclusion ranges themselves (the adaptive snapshot set subtracts them from the pages it learns too).
//
// An entry may also be pointer based: {"ptr":"0x8049D000","size":"0x3F0000","note":".."} = "read the u32 at ptr when SyncTest arms (and
// on every exclusion re-check) and exclude [value, value + size)" for memory the game allocates at run time (the sound heaps). Such
// entries go to *ptrs (which must be given when the file has any; otherwise they are an error). They are not subtracted here.
bool ApplyExclusions(const std::string& json_text, std::vector<Range>* ranges, Sink& sink, std::string* error,
                     std::vector<Range>* parsed = nullptr, bool require_nonempty = true,
                     std::vector<PtrExclusion>* ptrs = nullptr);

// Sorts by start and merges overlapping or touching ranges.
std::vector<Range> MergeRanges(std::vector<Range> ranges);

// ---- the fighter checksum fields (rev3-mod rbchecksum.py FIELDS, FIELDS_VERSION 1) ---------------------------------------------
constexpr u32 TEAMS_ADDR = 0x8038FE50;     // four team pointers; team 0 -> player 1, team 1 -> player 2
constexpr size_t FIELD_COUNT = 61;
constexpr size_t FIELD_BYTES = FIELD_COUNT * 4;  // 244
struct FieldDef
{
  const char* name;
  u16 offset;  // in the fighter struct
};
extern const std::array<FieldDef, FIELD_COUNT> FIELDS;

// ---- interfaces to the outside ---------------------------------------------------------------------------------------------
class GuestMemory
{
public:
  virtual ~GuestMemory() = default;
  // Copies n bytes at guest address addr; false (and nothing touched) when the range is not entirely inside MEM1 or MEM2.
  virtual bool Read(u32 addr, void* dst, size_t n) const = 0;
  virtual bool Write(u32 addr, const void* src, size_t n) = 0;
  // A direct pointer to n readable bytes at addr (no copy; used to hash pages), or nullptr when not available: the page tracker
  // then falls back to Read(). The pointer is only used immediately.
  virtual const u8* Peek(u32 /*addr*/, size_t /*n*/) const { return nullptr; }
  // Called after a restore wrote [addr, addr + n): drop any translated code for it (the Dolphin JIT). Default: nothing to do.
  virtual void InvalidateCode(u32 /*addr*/, size_t /*n*/) {}
  // The emulated time base (TBU:TBL, 64 bit) as the game's mftb reads it right now. False: not available (default).
  virtual bool ReadTimeBase(u64* /*out*/) const { return false; }
  // Sets the time base so that the game's next mftb reads `value`. False: not available (default).
  virtual bool WriteTimeBase(u64 /*value*/) { return false; }
};

// A memory region that emulated hardware reads or writes on its own (GX FIFO, audio DMA, XFB, DVD DMA). It must not be rewound
// under the hardware, so it is cut out of the snapshot ranges.
struct Exclusion
{
  std::string what;  // "gx_fifo", "audio_dma", "xfb_top", ...
  u32 start = 0;
  u32 end = 0;       // exclusive
};

class HardwareView
{
public:
  virtual ~HardwareView() = default;
  // Appends the regions the hardware uses right now (guest addresses 0x80.. / 0x90..). Called on the CPU thread.
  virtual void Query(std::vector<Exclusion>* out) = 0;
  // True while a disc read is in flight (GC DI registers TSTART, or the Wii IOS /dev/di device has a command executing or queued).
  // Its destination cannot be cut out of the snapshot, so SyncTest postpones rollbacks while this is true. Default: never.
  virtual bool DiscReadInFlight() { return false; }
};

// ---- audio hardware (REV3_SYNCTEST_AUDIO_HW) -------------------------------------------------------------------------------------
// One pending CoreTiming event of an audio device, as remaining cycles (event time - now) so that it can be re-scheduled relative to
// the time of the restore. `type` is the CoreTiming event type name ("AudioDMACallback", "DSPCallback", "AICallback", "DSPint",
// "ARAMint"); userdata is the event's own payload (the DSPint interrupt type).
struct AudioEvent
{
  std::string type;
  s64 cycles = 0;  // >= 0
  u64 userdata = 0;
};

// The emulated audio hardware, saved and restored with every snapshot. All calls are made on the CPU thread inside the hook's DMA
// handler (the same moment as the memory capture / restore).
class AudioHardware
{
public:
  virtual ~AudioHardware() = default;
  // "HLE" or "LLE" (logged once).
  virtual const char* DspKind() const = 0;
  // The CoreTiming event type names this object watches (logged once).
  virtual std::vector<std::string> EventTypeNames() const = 0;
  // Serialises the hardware state (DSP manager, DSP emulator, AI) into *out, which is resized to the exact size (the size may
  // differ from call to call: the HLE mail queue is variable). False: the two passes (measure, write) disagreed or it failed.
  virtual bool SaveState(std::vector<u8>* out) = 0;
  // Loads a buffer written by SaveState. False: the buffer was not consumed exactly (the state may be half applied).
  virtual bool LoadState(const std::vector<u8>& in) = 0;
  // The pending events of the watched types, in firing order.
  virtual void PendingEvents(std::vector<AudioEvent>* out) = 0;
  // Removes every pending event of the watched types and schedules `events` (remaining cycles relative to now) in the given order.
  virtual void ReplaceEvents(const std::vector<AudioEvent>& events) = 0;
  // The DSP and AI cause bits of the processor interface (INT_CAUSE_DSP | INT_CAUSE_AI) and a way to set exactly those two.
  virtual u32 InterruptBits() = 0;
  virtual void SetInterruptBits(u32 bits) = 0;
};

class Sink
{
public:
  virtual ~Sink() = default;
  virtual void Line(const std::string& text) = 0;  // one log line (no newline); the real sink appends and flushes
  virtual void Osd(const std::string& text) = 0;   // an on-screen message (a problem: the real sink draws it red)
  virtual void OsdInfo(const std::string& text) { Osd(text); }  // an on-screen message that is good news (green in the real sink)
};

// ---- SyncTest -------------------------------------------------------------------------------------------------------------
struct SyncTestOptions
{
  int k = 1;                      // frames to roll back, 1..8
  int every = 1;                  // roll back when frame % every == 0
  u32 stack_window = 0x4000;      // bytes saved from the hook's sp upward (clipped at the end of its memory region)
  std::string whitelist_path;     // only printed in the header line
  u32 arm_commands = 600;         // arm after this many consecutive hook calls with both fighters found (ignored with arm_by_pad)
  bool arm_by_pad = false;        // arm / disarm on P1's Z press (edge) while fighters are present
  bool noop = false;              // capture then restore the same frame every frame; no comparisons
  bool invalidate_code = true;    // call GuestMemory::InvalidateCode for every restored range
  bool restore_timebase = false;  // REV3_SYNCTEST_TB=1: store the time base in each snapshot and set it back on restore
  u32 hardware_check_every = 600; // hook calls between two hardware exclusion checks while armed
  bool adaptive = true;           // REV3_SYNCTEST_ADAPTIVE: learn the snapshot set in the running match (see the top of this file)
  u32 scan_slices = 16;           // REV3_SYNCTEST_SCAN_SLICES: each hook call hashes 1/scan_slices of all pages (1..4096)
  u32 learn_cycles = 20;          // REV3_SYNCTEST_LEARN: full scan cycles (scan_slices calls each) needed before arming
};

// ---- page tracker (adaptive snapshot set) ------------------------------------------------------------------------------------
constexpr u32 PAGE_SIZE = 0x1000;
constexpr size_t TRACKED_PAGES = (0x1800000u + 0x4000000u) / PAGE_SIZE;  // MEM1 (24 MB) then MEM2 (64 MB): 22528 pages
constexpr u32 FIGHTER_SET_BYTES = 0x1800;  // a fighter struct is always in the snapshot set: fighter .. fighter + this (FIELDS end at 0x1648)

// Finds the pages that change while the game runs. Scan() hashes the next 1/slices of the pages (64-bit hash of 4 KB, about
// 10+ GB/s); a page whose hash differs from its previous scan becomes dynamic, for good (until Reset). Pages that are dynamic are
// no longer hashed. Unreadable pages (MEM2 on a GameCube, a fake memory) are skipped.
class PageTracker
{
public:
  PageTracker(GuestMemory& memory, u32 slices);
  void Reset();                                   // forget every hash, dynamic mark and cycle (fighters lost: a new match)
  u32 Scan();                                     // hash the next slice; returns how many pages just became dynamic
  void InvalidateBaseline(const std::vector<Range>& ranges);  // a restore rewrote these: their old hashes mean nothing
  void AppendDynamicRanges(std::vector<Range>* out) const;    // dynamic pages, adjacent ones merged
  u32 Cycles() const { return m_cycles; }         // completed scan cycles (a cycle = `slices` Scan() calls)
  u32 Slices() const { return m_slices; }
  size_t DynamicPages() const { return m_dynamic; }
  bool IsDynamic(u32 addr) const;

private:
  GuestMemory& m_memory;
  u32 m_slices;
  u32 m_slice = 0;
  u32 m_cycles = 0;
  size_t m_dynamic = 0;
  bool m_has_state = false;
  std::vector<u64> m_hash;
  std::vector<u8> m_state;  // bit 0 = hash valid, bit 1 = dynamic
  std::vector<u8> m_buf;    // page copy when Peek() is not available
};

constexpr u16 PAD_BUTTON_Z = 0x0010;  // PADStatus button bit of the Z trigger

struct SyncTestStats
{
  u32 rollbacks = 0;
  u32 frames_compared = 0;
  u32 mismatching_frames = 0;
  u32 sp_mismatches = 0;
  u32 noop_restores = 0;
  u32 restores = 0;               // rollbacks + noop restores (the divisor of the *_mean values)
  u64 restore_us_total = 0;       // writing the ranges back
  u64 restore_us_max = 0;
  u64 icache_us_total = 0;        // JIT invalidation of the restored ranges
  u64 icache_us_max = 0;
  u64 scan_us_total = 0;          // page tracker scans (adaptive)
  u64 scan_us_max = 0;
  u64 scans = 0;
  u32 pages_learned_armed = 0;    // pages added to the set while armed
  u32 io_postponed = 0;           // rollbacks postponed because a disc read was in flight
  u32 audio_restores = 0;         // restores that also put the audio hardware back
  u64 audio_us_total = 0;         // time of those audio restores (state load + event re-scheduling)
  u64 audio_us_max = 0;
};

class SyncTest
{
public:
  // `micros` returns a monotonic clock in microseconds (restore times); the default is std::chrono::steady_clock.
  // `hardware` (optional) names the regions emulated hardware uses on its own; they are subtracted from the snapshot ranges when
  // SyncTest arms, re-checked every options.hardware_check_every hook calls, and only ever grow.
  // `ranges` is the seed of the snapshot set (the whitelist); with options.adaptive it may be empty.
  SyncTest(const SyncTestOptions& options, std::vector<Range> ranges, GuestMemory& memory, Sink& sink,
           std::function<u64()> micros = {}, HardwareView* hardware = nullptr);

  // REV3_SYNCTEST_EXCLUDE ranges: cut out of every snapshot set, including the pages the adaptive tracker learns. Call before the
  // first command (the layout is rebuilt; nothing is logged).
  // `pointers`: REV3_SYNCTEST_EXCLUDE entries read from guest memory when SyncTest arms and at every exclusion re-check.
  void SetUserExclusions(std::vector<Range> exclusions, std::vector<PtrExclusion> pointers = {});

  // One command in, one reply out. All work (reads, snapshots, restores, logging) happens inside this call.
  Reply OnCommand(const Command& command);

  // Writes the final summary line (once). The destructor does not.
  void Finish();

  // Optional: returns a one-line description of the DSP / audio DMA state (read without side effects). It is logged once when
  // SyncTest arms ("NOTE dsp at arm: ...") and appended to every HB line, so a freeze shows whether audio DMA stopped.
  void SetDspState(std::function<std::string()> dsp_state) { m_dsp_state = std::move(dsp_state); }

  // Optional: the emulated audio hardware, saved with every capture and restored with every restore (see "THE AUDIO HARDWARE").
  // Logs "NOTE audio hw rewind on (dsp=.., bytes=.., events=..)". Not owned; must outlive this object. nullptr switches it off. If a
  // save or load fails later on, SyncTest logs an ERROR and carries on with memory only.
  void SetAudioHardware(AudioHardware* audio);
  bool AudioHardwareOn() const { return m_audio != nullptr; }

  const SyncTestStats& Stats() const { return m_stats; }
  bool Active() const { return m_armed; }  // armed (a fight, past the arm delay or toggled on by Z)
  const std::vector<Range>& Ranges() const { return m_ranges; }  // the current snapshot ranges (whitelist minus exclusions)
  bool Resimulating() const { return m_resim; }
  size_t SnapshotBytes() const { return m_snapshot_bytes; }
  bool LearningDone() const { return !m_options.adaptive || m_learn_done; }  // enough scan cycles to arm
  size_t DynamicPages() const { return m_tracker.DynamicPages(); }
  u32 LearnCycles() const { return m_tracker.Cycles(); }

  // ---- network mode (milestone M3) -------------------------------------------------------------------------------------------
  // Rev3::Net::RollbackSession (Rev3Session.h) drives the snapshots in a netplay session; it needs the very same capture / restore /
  // adaptive set / hardware cuts / audio rewind as SyncTest, so this class offers them without SyncTest's own state machine
  // (OnCommand is NOT called in this mode; options.k = MAXR + 1 gives MAXR + 2 slots, slot of frame n = ring[n % slots]).
  // Per hook call, in this order: NetObserve(), then (session decides) NetCapture / NetRestore / NetClear. All on the CPU thread.
  //   - The set (m_ranges) only changes in NetPrepareArm() and NetClear(), i.e. while no slot is in use: slots of an armed stretch all
  //     share one layout. Pages learned and hardware regions found while a stretch is running are only noted (NetLayoutPending()); the
  //     caller closes the gate, the session drains, and its Clear() installs the new set.
  struct NetSnapshotInfo
  {
    std::array<std::array<u8, FIELD_BYTES>, 2> fields{};  // raw big-endian field bytes (FIELDS order) of both fighters at the capture
  };
  // First thing of a hook call: remembers the command (restores use its sp), finds the fighters, runs one page-tracker slice (not on a
  // resimulated call), and notes whether the set would change (learned pages, new hardware regions every hardware_check_every calls
  // while slots are in use).
  void NetObserve(const Command& command, bool resimulating);
  bool NetFightersFound() const { return m_net_found; }
  const std::array<u32, 2>& NetFighters() const { return m_fighters; }
  bool NetLayoutPending() const { return m_net_pending; }
  // The arm gate is about to open (session in lockstep, no slot in use): re-checks the hardware cuts, installs the current set (stack
  // window of this call's sp included) and verifies that all 61 fields of both fighters are inside it. False (logged): do not arm.
  bool NetPrepareArm();
  // Forgets every slot and installs the set that is due (pending changes). Called by the session whenever the armed stretch ends / begins.
  void NetClear();
  bool NetCapture(u32 frame, u32 sp, NetSnapshotInfo* info);
  bool NetHas(u32 frame) const;
  u32 NetSlotSp(u32 frame) const;
  bool NetRestore(u32 frame);
  size_t NetSlotCount() const { return m_ring.size(); }
  // "NOTE snapshot stats ..." (restores, restore / icache / audio times, postponed reads); written at shutdown.
  std::string NetStatsLine() const;

private:
  struct Slot
  {
    bool valid = false;
    u32 frame = 0;
    u32 sp = 0;
    u32 stack_len = 0;
    std::vector<u8> data;                                   // whitelist ranges back to back, then the stack window
    std::array<u32, 2> fighter_addr{};                      // player 1 / 2 fighter struct addresses
    std::array<std::array<u8, FIELD_BYTES>, 2> fields{};    // raw field bytes per player
    std::array<PadBytes, 2> inputs{};
    bool timebase_valid = false;
    u64 timebase = 0;                                       // emulated time base at capture (only with restore_timebase)
    struct Audio
    {
      bool valid = false;
      std::vector<u8> state;                                // AudioHardware::SaveState
      std::vector<AudioEvent> events;                       // the audio devices' pending CoreTiming events
      u32 irq = 0;                                          // PI DSP / AI cause bits
    } audio;
  };

  bool FindFighters(std::array<u32, 2>* addrs) const;
  bool Capture(Slot& slot, const Command& command, const std::array<u32, 2>& fighters);
  void Reset(const char* why);
  void Error(const std::string& text);
  Reply Echo(const Command& command) const;
  Reply Normal(const Command& command, const std::array<u32, 2>& fighters);
  Reply Resim(const Command& command, const std::array<u32, 2>& fighters);
  bool Restore(const Slot& s, const Command& command, u32 to);
  void CaptureAudio(Slot& slot, const Command& command);
  // Puts the audio hardware back; returns the text appended to the RESTORE line (empty when nothing was restored).
  std::string RestoreAudio(const Slot& s, const Command& command);
  void DisableAudio(const std::string& why);
  Reply Noop(const Command& command);
  bool DiscReadPostpones(u32 frame);
  void RebuildLayout();
  // One page-tracker slice (timed, with the "learning complete" note); returns the pages that just became dynamic.
  u32 ScanSlice(u32 frame);
  // Queries the hardware / pointer exclusions and records the new ones (with their NOTE lines) in m_exclusions_logged; true when a new
  // cut takes something away from the current set. Does not touch m_ranges.
  bool CollectExclusions();
  void RefreshExclusions(bool at_arm = false);
  void ResolvePtrExclusions(std::vector<Exclusion>* out);
  void ClearHistory();
  void Compare(const Slot& reference, const Slot& live, u32 frame);
  void SummaryLine(const char* tag);
  // Logs the coverage lines; returns the smaller of the two players' in-snapshot field counts.
  size_t LogFieldCoverage(const std::array<u32, 2>& fighters, const char* when);
  // The snapshot set from its parts: seed + dynamic pages + fighters + stack window, minus hook region, user and hardware cuts.
  std::vector<Range> ComputeRanges() const;
  void LearnWhileArmed(u32 added);
  bool Arm(const Command& command, const std::array<u32, 2>& fighters);
  // "0xSTART-0xEND" of the current snapshot range that holds [addr, addr + 4); empty when none does.
  std::string RangeContaining(u32 addr) const;
  // Same, plus the slot's stack window ("stack_window 0x..-0x.."); empty when none holds it.
  std::string SnapshotRangeOf(const Slot& slot, u32 addr) const;
  Slot& SlotFor(u32 frame) { return m_ring[frame % m_ring.size()]; }

  SyncTestOptions m_options;
  std::vector<Range> m_seed;    // the whitelist (after the hook region cut)
  std::vector<Range> m_ranges;  // the current snapshot ranges = ComputeRanges() at the last rebuild
  std::vector<Range> m_user_exclusions;
  struct PtrState
  {
    PtrExclusion entry;
    bool skip_logged = false;  // the current "skipped" reason was logged
    u32 skip_value = 0;
  };
  std::vector<PtrState> m_ptr_exclusions;
  GuestMemory& m_memory;
  Sink& m_sink;
  std::function<u64()> m_micros;
  PageTracker m_tracker;

  std::vector<size_t> m_range_offsets;  // offset of each range inside Slot::data
  size_t m_ranges_bytes = 0;
  size_t m_snapshot_bytes = 0;
  std::vector<Slot> m_ring;  // k + 1 slots, slot of frame n = ring[n % (k + 1)]
  Slot m_scratch;            // the live capture of a resimulated frame, swapped into the ring after the comparison
  std::vector<u8> m_span;    // one fighter's field span, read in one go

  HardwareView* m_hardware;
  std::vector<Exclusion> m_exclusions_logged;  // every (what, range) already reported
  u32 m_exclusion_lines = 0;
  u32 m_armed_calls = 0;     // hook calls since arming
  bool m_hardware_due = false;  // an exclusion re-check is waiting for a call outside a resimulation
  bool m_learn_done = false;     // adaptive: LEARN cycles completed with fighters present
  bool m_arm_wanted = false;     // pad mode: Z was pressed and SyncTest arms as soon as it may
  bool m_defer_logged = false;   // "arm deferred: learning" was logged for this wish
  bool m_arm_blocked = false;    // coverage check failed; no arming until the fighters are lost and found again
  u32 m_set_sp = 0;              // the hook sp whose stack window is part of the set (taken at arm)
  u32 m_last_sp = 0;             // sp of the latest command
  u32 m_learn_logs = 0;          // "NOTE learned" lines written
  u32 m_learn_log_call = 0;      // m_armed_calls at the last such line
  u32 m_learned_unlogged = 0;    // pages learned since that line
  u64 m_scan_iv_total = 0;       // scan time since the last HB line (microseconds), its count and its maximum
  u32 m_scan_iv_n = 0;
  u64 m_scan_iv_max = 0;
  bool m_prev_z = false;     // Z was down on the previous command
  bool m_z_pending = false;  // a Z press not yet acted on (acted on outside a resimulation)
  u32 m_restore_logs = 0;    // RESTORE lines written (the first 10 in full, restores 11..30 as one short line)
  std::function<std::string()> m_dsp_state;
  AudioHardware* m_audio = nullptr;  // null = memory only
  size_t m_audio_bytes = 0;          // size of the latest audio state (it varies a little: the HLE mail queue)
  u32 m_audio_size_notes = 0;        // "audio state size changed" lines written
  std::array<u32, 2> m_fighters{};            // the fighter addresses of the latest command (valid while both were found)
  std::vector<std::string> m_fields_annotated;  // field names already logged with their address / snapshot range

  // network mode
  Command m_net_cmd;               // the command of the latest NetObserve
  bool m_net_found = false;        // both fighters were found by it
  bool m_net_pending = false;      // the set would change (learned pages / new hardware cuts) but slots are in use
  bool m_net_active = false;       // slots have been captured since the latest NetClear
  u32 m_net_calls = 0;             // NetObserve calls with slots in use (paces the hardware re-check)

  u32 m_streak = 0;          // consecutive commands with both fighters found
  bool m_armed = false;
  bool m_have_history = false;
  u32 m_first_n = 0;         // first frame of the current history
  u32 m_last_frame = 0;      // frame of the previous command
  bool m_resim = false;
  u32 m_resim_target = 0;
  bool m_finished = false;
  bool m_osd_mismatch_shown = false;
  bool m_suppressed_noted = false;
  u32 m_logged_mismatch_frames = 0;
  u32 m_error_count = 0;
  SyncTestStats m_stats;
};

// ---- determinism kit (milestone M1) -------------------------------------------------------------------------------------------
// Pad record / pad script / trace for the boot determinism test (record a play session's pads, replay them in a second run and
// compare the traces of the two). Pure logic over GuestMemory / Sink / LineWriter, so the unit tests drive it without Dolphin.

constexpr const char* PAD_RECORD_HEADER = "# rev3 pad record v1";  // first line of a REV3_PAD_RECORD file
constexpr u32 PAD_RECORD_FLUSH_EVERY = 60;                          // record lines between two flushes (the rest is flushed at shutdown)
constexpr u32 TRACE_EVERY_DEFAULT = 30;                             // REV3_TRACE_EVERY default: hook calls between two trace lines

// The fighter structs found through TEAMS (team 0 -> [0], team 1 -> [1]); a team that is not found gives 0. Returns how many were found.
size_t FindFighterStructs(const GuestMemory& memory, std::array<u32, 2>* addrs);

// The desync checksum of `frame`, exactly rev3-mod rbchecksum.py checksum(): CRC-32 (zlib, init 0) over u32 BE frame, then for player
// 1 and 2 (a player not found is skipped) u32 BE player id and the FIELD_COUNT fields' raw big-endian bytes in FIELDS order.
// Returns how many fighters went into it (0..2); with 0 the checksum is that of the frame alone.
size_t FighterChecksum(const GuestMemory& memory, u32 frame, u32* checksum);

// The raw big-endian bytes of the FIELDS of both fighters (FIELD_BYTES each, FIELDS order), as FighterChecksum reads them; a fighter that
// is not found leaves its array zero. *present gets bit 0 = player 1 found, bit 1 = player 2. Returns how many were found.
size_t ReadFighterFields(const GuestMemory& memory, std::array<std::array<u8, FIELD_BYTES>, 2>* raw, u8* present);

// CRC-32 (zlib, init 0) over the bytes of all `ranges` in the given order. A range that cannot be read is skipped and counted in
// *unreadable (optional).
u32 MemoryCrc(const GuestMemory& memory, const std::vector<Range>& ranges, size_t* unreadable = nullptr);

// One frame of a pad script: both pads and the PADRead return value.
struct PadScriptEntry
{
  std::array<PadBytes, 2> pads{};
  s32 pad_return = 0;
};

struct PadScript
{
  std::map<u32, PadScriptEntry> frames;  // by frame; a frame listed twice keeps its last line
  u32 first = 0;                         // lowest / highest frame listed (valid when frames is not empty)
  u32 last = 0;
  size_t ignored_lines = 0;              // lines that were neither a comment, blank nor a pad line
};

// Parses a pad record / script ("# ..." and blank lines skipped, lines in "<frame> <pad0> <pad1> [<return>]" form kept; a missing
// final newline is fine). False when it holds no pad line (*error says so); a malformed line is skipped and counted.
bool ParsePadScript(const std::string& text, PadScript* out, std::string* error);

// "<frame> <24 hex> <24 hex> <return>" (lowercase hex), no newline.
std::string FormatPadRecordLine(u32 frame, const std::array<PadBytes, 2>& pads, s32 pad_return);
// "T frame=N fighters=F chk=0x........ mem=0x........" (hex digits uppercase), no newline.
std::string FormatTraceLine(u32 frame, int fighters, u32 checksum, u32 mem);

// A line-oriented text file. Line() appends one line (no newline in `text`); Flush() makes it durable.
class LineWriter
{
public:
  virtual ~LineWriter() = default;
  virtual void Line(const std::string& text) = 0;
  virtual void Flush() = 0;
};

struct DeterminismOptions
{
  std::string record_path;        // REV3_PAD_RECORD ("" = off); the writer is given to the kit
  std::string script_path;        // REV3_PAD_SCRIPT ("" = off)
  PadScript script;               // the parsed script
  std::string trace_path;         // REV3_TRACE ("" = off)
  u32 trace_every = TRACE_EVERY_DEFAULT;  // REV3_TRACE_EVERY: a trace line when frame % trace_every == 0
  std::string ranges_path;        // REV3_TRACE_RANGES ("" = none: mem is 0)
  std::vector<Range> ranges;      // the parsed trace ranges
  bool record_header = true;      // write PAD_RECORD_HEADER first (false when the record file already has content)
  bool record_reply = false;      // netplay: record the pads the game actually got (the reply's ports, via OnReply) instead of the command's
};

// Called for every hook call before SyncTest sees it. Substitutes the scripted pads, records the pads, writes the trace line.
class DeterminismKit
{
public:
  // `record` / `trace` may be null when the matching path is empty. Logs the "NOTE pad record -> ..", "NOTE pad script ..",
  // "NOTE trace -> .." lines to `log`. Neither is owned.
  DeterminismKit(DeterminismOptions options, GuestMemory& memory, Sink& log, LineWriter* record, LineWriter* trace);

  // `resimulating` = SyncTest is replaying a rolled-back frame: pads are still substituted (SyncTest ignores them there), but nothing
  // is recorded or traced and the script cannot end. May change command->pads and pad_return.
  void OnCommand(Command* command, bool resimulating);

  // Netplay (options.record_reply): called after the session answered; writes the record line of the call from the reply's pads
  // (game port order: what the game really got, local and remote). Nothing for a resimulated call or a RESTORED reply.
  void OnReply(const Command& command, const Reply& reply, bool resimulating);

  void Finish();  // flushes the record and the trace

  u32 RecordedLines() const { return m_record_lines; }
  u32 TraceLines() const { return m_trace_lines; }
  bool ScriptEnded() const { return m_script_ended; }
  bool Active() const { return m_record != nullptr || m_trace != nullptr || !m_options.script_path.empty(); }

private:
  DeterminismOptions m_options;
  GuestMemory& m_memory;
  Sink& m_log;
  LineWriter* m_record;
  LineWriter* m_trace;
  u32 m_record_pending = 0;   // record lines since the last flush
  u32 m_record_lines = 0;
  u32 m_trace_lines = 0;
  bool m_script_ended = false;
  bool m_unreadable_logged = false;
};

// ---- Dolphin glue ---------------------------------------------------------------------------------------------------------
// Called by CEXISlippi::DMAWrite when IsCommand(). `command` points at the DMA'd bytes (>= CMD_SIZE). Fills *read_queue with
// the 64-byte reply (clearing it first). On the CPU thread.
void HandleCommand(Core::System& system, const u8* command, size_t size, std::vector<u8>* read_queue);

// True when REV3_NET_ROLE is set (netplay is wanted, whether or not its other variables turn out usable). The EXI device uses it to skip
// everything of Slippi's online stack (Rust extensions, login, matchmaking, spectator, replays, reporting): a Rev3 session talks only to
// its peer.
bool NetplayRequested();

// Writes the final summary and closes the log; the next command starts a fresh session. Safe to call any time.
void Shutdown();
}  // namespace Rev3
