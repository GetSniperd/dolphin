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
//   REV3_SYNCTEST_DUMP=0     no freeze dump (default 1: a state dump into the Rev3 log at arm, on a stall of the hook, at shutdown;
//                            REV3_SYNCTEST_DUMP_STALL_MS=n silence that counts as a stall, default 2000; REV3_SYNCTEST_DUMP_PAUSE=0 racy dump only)
//                            together with memory (default 1, see "THE AUDIO HARDWARE" below)
//   REV3_SYNCTEST_ADAPTIVE=0 do not learn the snapshot set in the running match (default 1: adaptive, see below)
//   REV3_SYNCTEST_SCAN_SLICES=N  adaptive: every hook call hashes 1/N of all 4 KB pages of MEM1 + MEM2 (default 16)
//   REV3_SYNCTEST_LEARN=N    adaptive: scan cycles that must complete with fighters present before SyncTest may arm (default 20)
//   REV3_EF_POOL=0           do not force the nw4r::ef (effect / particle) pool into the snapshot at arm (default 1, see "THE EFFECT POOL")
//   REV3_SNAPSHOT_INCLUDE=path   JSON, same entries as REV3_SYNCTEST_EXCLUDE ({"start","end"} or {"ptr","size"}): always in the snapshot
//   REV3_EFCHECK=N|all|0     list consistency check of the effect system after the first N restores (default 200; all = every restore)
//   REV3_RB_VERIFY=N|all     restore verify (off by default): keep full shadow copies of guest memory (88 MB each) and after N restores compare ALL
//                            memory with the shadow of the frame restored to, in 32-byte blocks; the blocks that differ are pages the snapshot set
//                            misses (see "restore verify" below, REV3.md "Restore verify"). Also REV3_RB_VERIFY_EVERY / _SLOTS / _HEAL / _STACK_BELOW /
//                            _IGNORE (ParseVerifyEnv)
//   REV3_RESIM_HIDE=0|1|2|3  (or off|draw|sound|all) hook protocol 3 only (M6): what the game hides while a resimulation runs, written as a u32 to
//                            0x8000241C (default 1 = draw; see "hiding resimulated frames" below). Netplay always; SyncTest with REV3_SYNCTEST_HIDE=1
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
// THE EFFECT POOL (REV3_EF_POOL, default on; REV3.md "Effect pool")
//   Rev3's nw4r::ef effect system (hit sparks, dust, particles) keeps every Effect / Emitter / ParticleManager / Particle in object
//   pools carved out of ONE block the game allocates at start-up, linked into intrusive lists (nw4r::ut::List). The page tracker only
//   adds a page after it saw it change, up to one scan cycle late, so a particle allocated into a never-touched page of that block
//   within the rolled-back window was not restored while the list heads pointing at it were: a cyclic list and a hook command that
//   never came again (the main thread spun in ParticleManager::Calc). So at arm the whole block (found through the EffectSystem
//   singleton, FindEfPool below) is put into the snapshot set, and after a restore the lists are walked (CheckEfLists, EFCHECK lines).
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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Slippi/Rev3Bot.h"

namespace Core
{
class System;
}

namespace Rev3
{
// ---- the wire protocol (version 2 and 3), all big-endian ---------------------------------------------------------------------
// Protocol 3 (rev3-mod rbhook.py build(version=3), docs/resim_skip.md) has the very same command and reply layout; the version byte only
// says that the game carries the resimulation-hiding patches, which read the flag word HIDE_FLAGS_ADDR. ParseCommand accepts both.
constexpr u8 CMD_ID = 0xA0;                // +0x00
constexpr u8 PROTOCOL_VERSION = 2;         // +0x01 the baseline protocol
constexpr u8 PROTOCOL_VERSION_HIDE = 3;    // +0x01 baseline + the hiding patches (M6)
constexpr u32 CMD_MAGIC = 0x52424B31;      // +0x04 "RBK1"
constexpr u32 REPLY_MAGIC = 0x52425231;    // +0x00 of the reply, "RBR1"
constexpr size_t CMD_SIZE = 64;
constexpr size_t REPLY_SIZE = 64;
constexpr u32 REPLY_FLAG_RESTORED = 1;     // reply flags bit 0: guest memory was rewritten to the command of `frame`

using PadBytes = std::array<u8, 12>;       // one PADStatus (buttons u16, stickX/Y, substickX/Y, triggerL/R, analogA/B, err), raw

struct Command
{
  u8 protocol = PROTOCOL_VERSION;          // +0x01 the hook's protocol version (2 or 3)
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
bool ParseCommand(const u8* data, size_t size, Command* out);  // false: not ours or a version other than 2 and 3
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

// The game's own view of a disc read: the DVD library (RVL SDK dvd.c, 0x802D32D4 stateReady / 0x802D458C DVDReadAbsAsyncPrio, decompiled US main.dol)
// keeps the command it is executing in `executing` (DAT_8049D870): 0 when idle (DVDReadAbsAsyncPrio starts stateReady only when it is 0), a pointer to
// the command block while a command executes, DVD_DUMMY_BLOCK for the moment between a command's end and the next stateReady (its callback runs then).
// A restore to a state where this is a real block while the hardware / IOS no longer has that command leaves the library waiting for a completion
// that never comes: every later DVD command queues behind it (a freeze at match end, D's net_d2_r5 run: DVDCancel on the BGM stream).
constexpr u32 DVD_EXECUTING_ADDR = 0x8049D870;
constexpr u32 DVD_DUMMY_BLOCK = 0x80460520;  // DAT_80460520: DummyCommandBlock, `executing` while no real command runs

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

// A range and the CRC-32 (zlib, init 0) of its bytes (REV3_NET_RANGE_CRC, SyncTest::NetRangeCrcs).
struct RangeHash
{
  u32 start = 0;
  u32 end = 0;  // exclusive
  u32 crc = 0;
};

// ---- learned-page reporting (pure helpers; tools/rollback/learned_include.py reads what they print) ----------------------------
// "0xSTART-0xEND,0xSTART-0xEND,..." in the order given; at most `max_listed` entries, then ",+N more" (N = the rest). An empty list gives "".
std::string FormatRangeList(const std::vector<Range>& ranges, size_t max_listed);
// The bytes in `ranges` (they are assumed not to overlap, e.g. MergeRanges output), and that many 4 KB pages, rounded up.
u64 RangesBytes(const std::vector<Range>& ranges);
u64 RangesPages(const std::vector<Range>& ranges);
// `after` minus `before`: the parts of `after` that `before` does not cover (sorted, merged).
std::vector<Range> NewRanges(const std::vector<Range>& after, const std::vector<Range>& before);
// How many ranges of `fresh` touch (share a boundary with) a range of `old`: a page learned right next to a page that was already
// in the set, i.e. what REV3_SNAPSHOT_PAD would have caught.
size_t CountTouching(const std::vector<Range>& fresh, const std::vector<Range>& old);
// Widens every range by `pad_bytes` at both ends, clipped to the MEM1 / MEM2 region that holds it (never across the gap between them);
// the result is merged. Ranges outside both regions are kept as they are. pad_bytes 0 = MergeRanges only.
std::vector<Range> PadRanges(std::vector<Range> ranges, u32 pad_bytes);

// REV3_SNAPSHOT_INCLUDE: parses `json_text` with the very entry formats ApplyExclusions takes ({"start","end"} ranges and pointer based
// {"ptr","size","note"} entries) but for the opposite purpose: the regions are forced INTO the snapshot set when SyncTest (or a netplay
// session) arms. Fixed ranges go to *fixed, pointer entries to *ptrs (the u32 at `ptr` is read at arm and [value, value + size) is
// included). Logs one "NOTE include user 0xSTART-0xEND (n bytes)" / "NOTE include ptr entry ..." line per entry to `sink`. False (and
// the outputs untouched) when the JSON is bad or empty; *error says why. The cuts (hook region, REV3_SYNCTEST_EXCLUDE, hardware) still win.
bool LoadInclusions(const std::string& json_text, Sink& sink, std::vector<Range>* fixed, std::vector<PtrExclusion>* ptrs,
                    std::string* error);

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

// ---- nw4r::ef effect pool (REV3_EF_POOL, REV3_EFCHECK) ------------------------------------------------------------------------
// Rev3 links the nw4r effect library (EffectSystem, Effect, Emitter, ParticleManager, Particle; symbols in the decomp, libnw4r_ef.a).
// Everything below is read from the decomp (addresses of the RNEEDA build), nothing is copied from the game.
//
// Allocation (decomp zz_80186fd8_ @ 0x80186fd8 -> zz_801895d8_ @ 0x801895d8, the memorymanagertmp.h template of nw4r):
//   block = zz_80170620_(size + 0xC60) from the game heap, stored in the global 0x8049CFB0 (DAT_8049cfb0);
//   mm = block (a 0x3C-byte MemoryManager, vtable 0x8035E470) is placement-constructed at the START of the block, then
//   mm.heap = [block + 0x40, block + 0x40 + size) is a free-list heap (mHeapStart / mHeapEnd at mm + 0x24 / + 0x28), and the object
//   pools are allocated out of that heap through mm's vtable slot +0x60: Effect[n] (0xA0 bytes each), Emitter[n] (0x154), ParticleManager[n]
//   (0xBC, 256 of them) and Particle[n] (0xE4, 2500 of them), each with a 0x24-byte ObjectPool object (free list + in-use list, ut::List
//   offset 0). So ALL ef objects live in [mm, mm + 0x40 + size), and the handle table the game keeps for them (0x101 words, global
//   0x8049CFC4) directly follows the heap, inside the same allocation (block + size + 0xC60 is the allocation's end).
//   The EffectSystem singleton is the static 0x803D9588 (EffectSystem::GetInstance @ 0x801A9C18 returns it): word 0 is the mm pointer
//   (zz_80189590_ @ 0x80189590 stores it and calls EffectSystem::Initialize @ 0x801A9C24), the CreationQueue sits at +0x10 and
//   +0xC014 / +0xC018 are the activity-group count and the ActivityList[group] array (0x1C bytes each, allocated from mm's heap too).
//
// Lists (nw4r::ut::List = {head +0, tail +4, u16 count +8, u16 offset +0xA}; the link of a node is {prev, next} at node + offset):
//   ActivityList[g]: effects (offset 0x14) at +0, closing effects at +0xC;  Effect: emitters at +0x24, closing at +0x30 (offset 0x14);
//   Emitter: particle managers at +0xC0, closing at +0xCC;  ParticleManager: particles at +0x38, closing at +0x44.
//   ParticleManager::Calc @ 0x801B5E50 (5444 bytes) walks the particles list with `node = *(node + offset + 4)` (offset = the u16 at
//   pm + 0x42) until it reads 0; its loop head is LAB_801b72dc and the PC the frozen guest stood at (0x801B72D8) is next to it, so a
//   list that is cyclic or leads into stale memory never ends. The element counts of the Effect / Emitter pools are arguments of
//   zz_80186fd8_ that the decomp does not show; they are read from the manager at run time (FindEfPool logs them).
constexpr u32 EF_SYSTEM_ADDR = 0x803D9588;       // EffectSystem singleton (DAT_803d9588)
constexpr u32 EF_SYSTEM_BYTES = 0xC070;          // its size: fields up to +0xC064, rounded (the CreationQueue is inside)
constexpr u32 EF_BLOCK_GLOBAL = 0x8049CFB0;      // u32: the allocation (== mm)
constexpr u32 EF_HANDLE_TABLE_GLOBAL = 0x8049CFC4;  // u32: the 0x101-word handle table (just behind the heap, inside the allocation)
constexpr u32 EF_HANDLE_TABLE_BYTES = 0x101 * 4;
constexpr u32 EF_HANDLE_POOL_GLOBALS[2] = {0x8049CFBC, 0x8049CFC0};  // u32: two small fixed pools {free, first, n, payload, nodes...}
constexpr u32 EF_BLOCK_TAIL = 0xC20;             // allocation bytes after the heap end (size + 0xC60 - (0x40 + size))
constexpr u32 EF_POOL_MAX_BYTES = 0x2000000;     // sanity limit for the heap size (the real one is about 1 MB)
// MemoryManager fields (word index * 4).
constexpr u32 EF_MM_COUNTS[4] = {0x04, 0x0C, 0x14, 0x1C};  // effects, emitters, particle managers, particles
constexpr u32 EF_MM_POOLS[4] = {0x08, 0x10, 0x18, 0x20};   // their ObjectPool pointers
constexpr u32 EF_MM_HEAP_START = 0x24;
constexpr u32 EF_MM_HEAP_END = 0x28;
constexpr u32 EF_CHECK_LIST_CAP = 10000;         // nodes walked per list before it counts as cyclic
constexpr u32 EF_CHECK_TOTAL_CAP = 40000;        // nodes walked in one check (the four pools hold a few thousand objects)
constexpr u32 EF_CHECK_MAX_GROUPS = 64;          // activity groups the check accepts (the game uses 3)

struct EfPool
{
  u32 mm = 0;                    // the MemoryManager (== start of the allocation)
  u32 heap_start = 0;
  u32 heap_end = 0;
  std::array<u32, 4> counts{};   // effects, emitters, particle managers, particles (pool sizes)
  Range main;                    // the allocation, page aligned: [mm, heap_end + EF_BLOCK_TAIL)
  std::vector<Range> ranges;     // everything to force in, page aligned and merged (main + instance + handle table / pools)
  u32 bytes = 0;                 // total bytes of `ranges`
  std::string detail;            // human readable: what the extras are, what could not be added
};

// Reads the EffectSystem singleton and computes the ranges that hold all ef objects (see above). False and *why when the effect system
// is not set up (mm pointer 0, unreadable, implausible heap); then nothing is forced. The extras (instance, handle table, handle
// pools) are added when they validate and described in `detail`.
bool FindEfPool(const GuestMemory& memory, EfPool* out, std::string* why);

struct EfCheckResult
{
  bool ran = false;     // false: the effect system is not set up (nothing to check)
  bool ok = true;
  u32 lists = 0;        // lists walked
  u32 nodes = 0;        // nodes walked
  std::array<u32, 4> objects{};  // effects, emitters, particle managers, particles visited
  std::string list;     // the failing list ("particles@pm=0x9220F100")
  u32 list_addr = 0;    // its ut::List address
  u32 node = 0;         // the offending node (0: the list header itself)
  std::string why;
};

// Walks the ef lists from the EffectSystem singleton (activity lists -> effects -> emitters -> particle managers -> particles, plus the
// four object pools' free / in-use lists) and verifies for each list: head / tail / count agree about emptiness, every node is a 4-byte
// aligned pointer inside the ef heap, prev links match, the walk ends at the tail after exactly `count` nodes, and it ends within
// `list_cap` nodes (EF_CHECK_LIST_CAP). The first failure stops the walk. Read only.
EfCheckResult CheckEfLists(const GuestMemory& memory, u32 list_cap = EF_CHECK_LIST_CAP);

// The `ef ...` lines of the freeze dump (without the DUMP prefix): the EffectSystem pointer, the manager, the heads of every group's
// lists and pools, and the result of CheckEfLists. Read only; safe on an unset effect system.
std::vector<std::string> EfDumpLines(const GuestMemory& memory);

class Sink
{
public:
  virtual ~Sink() = default;
  virtual void Line(const std::string& text) = 0;  // one log line (no newline); the real sink appends and flushes
  virtual void Osd(const std::string& text) = 0;   // an on-screen message (a problem: the real sink draws it red)
  virtual void OsdInfo(const std::string& text) { Osd(text); }  // an on-screen message that is good news (green in the real sink)
};

// ---- hiding resimulated frames, hook protocol 3 (milestone M6; rev3-mod docs/resim_skip.md) ----------------------------------------
// With protocol 3 the game draws nothing / starts no sounds while the u32 at HIDE_FLAGS_ADDR (inside the hook region, so never in a
// snapshot) says so: bit 0 HIDE_DRAW (no 3D draw, no EFB copy, no presenting), bit 1 HIDE_SOUND (nw4r StartSound returns "started" without
// starting). The fork writes the word big-endian from the CPU thread: the flags while a resimulation runs, 0 otherwise (at the end of every
// resimulation, when the session leaves, at shutdown). Two u32 counters in the same region tell whether the patches ran at all.
constexpr u32 HIDE_FLAGS_ADDR = 0x8000241C;        // rbhook.D_FLAGS (hook data block +0x1C)
constexpr u32 HIDE_ITERATIONS_ADDR = 0x800029E4;   // rbhook.V3_DATA +4: main-loop iterations that skipped drawing
constexpr u32 HIDE_SOUNDS_ADDR = 0x800029E8;       // rbhook.V3_DATA +8: sound starts that were skipped
constexpr u32 HIDE_DRAW = 1;                       // FLAGS bit 0
constexpr u32 HIDE_SOUND = 2;                      // FLAGS bit 1
constexpr u32 HIDE_ALL = HIDE_DRAW | HIDE_SOUND;
constexpr u32 HIDE_DEFAULT = HIDE_DRAW;            // REV3_RESIM_HIDE unset: draw only (sounds as v1) until `all` has been listened to (resim_skip.md 3.4)

// REV3_RESIM_HIDE: "" -> HIDE_DEFAULT; "0" / "off" -> 0; "1" / "draw"; "2" / "sound"; "3" / "all" (case does not matter). False (and *flags =
// HIDE_DEFAULT) when the text is none of these.
bool ParseResimHide(const std::string& text, u32* flags);

class ResimHideFlags
{
public:
  // `flags`: what Set(true) writes (0 = hiding by flag is off). `log` (optional, not owned): the one-time NOTE lines.
  ResimHideFlags(GuestMemory& memory, u32 flags, Sink* log = nullptr);
  // The protocol of the command just received (every command; the first one decides). Only protocol 3 has a flag word.
  void SetProtocol(u8 protocol);
  u8 Protocol() const { return m_protocol; }
  u32 Flags() const { return m_flags; }
  // True: write the flags; false: write 0. Does nothing with protocol 2, flags 0, or when that value is already in the word. CPU thread.
  void Set(bool hidden);
  bool Hidden() const { return m_hidden; }
  u32 Writes() const { return m_writes; }
  // The two guest counters; false when the protocol is not 3 or the memory is not readable.
  bool ReadCounters(u32* hidden_iterations, u32* skipped_sounds) const;
  // " hide_protocol=3 hide_flags=1 hide_writes=4 hidden_iterations=12 skipped_sounds=0" (leading space), appended to SUMMARY lines.
  std::string SummaryFields() const;

private:
  GuestMemory& m_memory;
  Sink* m_log;
  u32 m_flags;
  u8 m_protocol = 0;          // 0 = no command yet
  bool m_hidden = false;      // the latest Set()
  bool m_known = false;       // m_word is what the guest word holds
  u32 m_word = 0;
  u32 m_writes = 0;
  bool m_noted_on = false;
  bool m_noted_v2 = false;
};

// ---- freeze dump (REV3_SYNCTEST_DUMP) ---------------------------------------------------------------------------------------
// A read-only state dump into the Rev3 log, to see WHERE the game is stuck when SyncTest freezes it (a thread waiting on a hardware event
// whose software flag was rewound: GX draw done, VI retrace, DVD, AX, IPC). Every line starts with `DUMP <reason> `. Taken (a) when
// SyncTest arms (baseline), (b) by a watchdog thread when no 0xA0 command arrived for REV3_SYNCTEST_DUMP_STALL_MS (default 2000) while
// armed, once per stall (first a racy dump, then, if the CPU can be paused, a consistent one), and (c) at shutdown. It never writes guest
// memory or registers. The addresses are Rev3's SDK globals from the decomp (see REV3.md "Freeze dump"). Everything but the Dolphin glue
// is pure (GuestMemory / Sink / DumpSource), so the unit tests drive it with a fake memory.
struct DumpCpu
{
  bool valid = false;
  u32 pc = 0;
  u32 npc = 0;
  u32 lr = 0;
  u32 ctr = 0;
  u32 msr = 0;
  u32 srr0 = 0;
  u32 srr1 = 0;
  u32 cr = 0;
  u32 exceptions = 0;  // PowerPCState::Exceptions (pending exception bits)
  std::array<u32, 32> gpr{};
};

// The hardware the freeze hunt cares about. Each group has its own ok flag (a group that cannot be read is logged as unavailable).
struct DumpHw
{
  bool pe_ok = false;
  u16 pe_status = 0;  // PE control / interrupt status register (MMIO 0x0C00100A: token / finish enables and flags)
  u16 pe_token = 0;   // PE token register (MMIO 0x0C00100E)
  bool cp_ok = false;
  u32 cp_base = 0;
  u32 cp_end = 0;
  u32 cp_rd = 0;
  u32 cp_wr = 0;
  u32 cp_dist = 0;  // CPReadWriteDistance: bytes the GPU still has to read (0 = the CP is idle)
  u32 cp_bp = 0;    // breakpoint address
  bool cp_breakpoint = false;
  bool cp_gp_read = false;
  bool cp_bp_enable = false;
  bool cp_link = false;
  bool cp_hi_wm = false;
  bool cp_lo_wm = false;
  bool pi_ok = false;
  u32 pi_cause = 0;
  u32 pi_mask = 0;
  u32 pi_fifo_base = 0;
  u32 pi_fifo_end = 0;
  u32 pi_fifo_wptr = 0;
  bool vi_ok = false;
  u32 vi_line = 0;  // vertical beam position (MMIO 0x0C00202C)
  u32 vi_di0 = 0;   // display interrupt 0 register (MMIO 0x0C002030): bit 31 = interrupt flag, bit 28 = enable
};

class DumpSource
{
public:
  virtual ~DumpSource() = default;
  virtual bool ReadCpu(DumpCpu* out) = 0;
  virtual bool ReadHw(DumpHw* out) = 0;
  // Extra lines (without the `DUMP <reason> ` prefix): the DSP / AI state and the audio CoreTiming events. Only asked for in a
  // consistent dump (arm, a paused CPU), never in a racy one. Default: none.
  virtual void AudioLines(std::vector<std::string>* /*out*/) {}
  // Extra hardware lines (the DVD interface and IOS /dev/di state), asked for in every dump. `racy`: the CPU thread is still running, so
  // anything that walks Dolphin's own containers (the IOS device map) must be skipped. Default: none.
  virtual void ExtraLines(std::vector<std::string>* /*out*/, bool /*racy*/) {}
};

struct StackFrame
{
  u32 sp = 0;
  u32 lr = 0;  // *(sp + 4), the saved LR word of that frame
};

// The PPC back-chain from `sp`: each entry is {sp, *(sp + 4)}; the next sp is *(sp). Stops after `max_frames`, at an unreadable, zero or
// unaligned sp, or when the next sp does not increase (a loop or a stack end). The last frame found is still listed. Returns the count.
size_t WalkBackChain(const GuestMemory& memory, u32 sp, size_t max_frames, std::vector<StackFrame>* out);

class FreezeDump
{
public:
  // `now_ms` returns a monotonic clock in milliseconds (default: steady_clock); `stall_ms` = silence that counts as a stall.
  FreezeDump(const GuestMemory& memory, Sink& sink, DumpSource& source, u32 stall_ms = 2000, std::function<u64()> now_ms = {});

  // The dump as lines without the prefix. `full` adds the audio lines (consistent dumps only); `racy` = the CPU thread is running.
  std::vector<std::string> Lines(bool full, bool racy = false);
  // Writes Lines() to the sink, each as `DUMP <reason> <line>`, between `begin racy=<0|1> <note>` and `end` lines. One dump at a time.
  void Dump(const char* reason, bool racy, bool full, const std::string& note = {});
  // `RESTOREHW <from> -> <to>[ after] pe_status=.. pe_token=.. cp_rw_dist=.. cp_rd=.. cp_wr=.. vi_count=.. drawdone=.. sched_disable=..`
  std::string RestoreHwLine(u32 from, u32 to, bool after);

  // CPU thread, every 0xA0 command (any mode: plain play, SyncTest armed or not, netplay): the latest frame and the time; ends a stall.
  void Tick(u32 frame);
  // The netplay session is deliberately waiting for the peer (Rev3::Net wait loops): no command arrives, which is not a freeze.
  // Leaving the blocked state restarts the stall timer (the wait itself must not count as silence).
  void SetBlocked(bool blocked);
  bool Blocked() const { return m_blocked.load(); }
  // Watchdog thread: true once per stall (no command for stall_ms since the last Tick and the session is not blocked in its wait);
  // Tick starts the next stall.
  bool StallDue();
  u32 LastFrame() const { return m_frame.load(); }
  u64 LastAgeMs() const;
  u32 StallMs() const { return m_stall_ms; }
  bool Seen() const { return m_seen.load(); }  // at least one command arrived

private:
  const GuestMemory& m_memory;
  Sink& m_sink;
  DumpSource& m_source;
  u32 m_stall_ms;
  std::function<u64()> m_now_ms;
  std::mutex m_lock;  // serialises Dump (CPU thread at arm / shutdown, watchdog thread)
  std::atomic<u64> m_last_ms{0};
  std::atomic<u32> m_frame{0};
  std::atomic<bool> m_blocked{false};
  std::atomic<bool> m_reported{false};
  std::atomic<bool> m_seen{false};  // a command arrived at least once
};

// ---- restore verify (REV3_RB_VERIFY) ------------------------------------------------------------------------------------------
// "Is guest memory after a restore really what it was when the snapshot was taken?" The snapshot set only rewinds the pages it knows;
// a page outside the set that the game wrote in the rolled-back frames keeps its new contents (a hit-spark particle in a page the
// adaptive tracker had not found yet made the effect lists circular and froze the game). The verifier keeps a SHADOW: a full copy of
// guest MEM1 + MEM2 taken at the same moment as the snapshot of a frame, and after a restore to that frame compares ALL guest memory
// with it in 32-byte blocks. A block that differs is "wrong": either a page the set misses (outside_set, the useful finding) or memory
// the restore failed to rewind / something wrote behind it (in_set, a bug or a host thread).
// Cost: a shadow is 24 + 64 MB (88 MB) per kept frame and taking one is a memcpy of that (about 8-15 ms); a check reads both copies
// again (about 5-10 ms). So shadows are taken only for every `every`-th frame and only `slots` of them are kept (a rotating ring,
// slot = (frame / every) % slots); a restore is verified only when it lands on a frame that still has its shadow. After `restores`
// verified restores shadows stop being taken (the cost disappears). Off by default (restores = 0): no memory, no copies.
constexpr u32 VERIFY_ALL = 0xFFFFFFFFu;        // VerifyOptions::restores: never stop
constexpr u32 VERIFY_BLOCK_BYTES = 32;         // granularity of the comparison and of the counts (HowlingWind reports 32-byte blocks too)
constexpr u32 VERIFY_REGION_BYTES = 0x10000;   // 64 KB regions of the per-region counts
constexpr u32 VERIFY_FIRST_WORDS = 8;          // words of the first wrong block that are printed (one block = 8 words)
constexpr u32 VERIFY_MAX_REGIONS_LISTED = 16;  // regions named in one VERIFY line
constexpr u32 VERIFY_MAX_PAGE_RANGES_LISTED = 16;
constexpr u32 VERIFY_MAX_WRONG_LOGS = 30;      // restores with wrong blocks that get the full VERIFY lines; the rest only count
constexpr u32 VERIFY_MAX_OK_LOGS = 3;          // clean restores that get a line ("VERIFY restore n -> m ok ...")

struct VerifyOptions
{
  u32 restores = 0;      // REV3_RB_VERIFY=N: verify the first N restores that land on a shadowed frame (count); VERIFY_ALL = all; 0 = off
  u32 every = 0;         // REV3_RB_VERIFY_EVERY=P: take a shadow when frame % P == 0 (frames); 0 = automatic (the owner picks, SyncTest: K + 1)
  u32 slots = 1;         // REV3_RB_VERIFY_SLOTS=S: shadows kept (1..8), 88 MB each (MEM1 + MEM2)
  bool heal = false;     // REV3_RB_VERIFY_HEAL=1: copy the shadow's bytes back over wrong blocks and add their pages to the set (next layout)
  u32 stack_below = 0x4000;  // REV3_RB_VERIFY_STACK_BELOW: bytes below the hook's sp that are dead stack and never compared (bytes), used when
                             // the running thread's stack end cannot be read from the OSThread
};

// What to compare after one restore.
struct VerifyInput
{
  u32 from_frame = 0;                  // the frame the restore was asked at (log only)
  u32 to_frame = 0;                    // the frame restored: its shadow is compared
  std::vector<Range> skip;             // memory nobody rewinds on purpose (hook region, hardware cuts, user exclusions, dead stack)
  std::vector<Range> in_set;           // the memory the restore rewrote (snapshot ranges + the stack window)
};

struct VerifyRegionCount
{
  u32 base = 0;     // start of the 64 KB region
  u32 blocks = 0;   // wrong 32-byte blocks inside it
  u32 pages = 0;    // wrong 4 KB pages inside it
};

struct VerifyResult
{
  bool checked = false;           // a shadow of to_frame existed and was compared
  u32 wrong_blocks = 0;           // 32-byte blocks that differ
  u32 wrong_pages = 0;            // 4 KB pages holding at least one
  u32 in_set_blocks = 0;          // ... of which inside the memory the restore rewrote (restore bug or a host thread writing)
  u32 outside_set_blocks = 0;     // ... of which outside it (pages the set is missing)
  u64 compared_bytes = 0;
  u64 skipped_bytes = 0;          // bytes not compared (skip ranges, memory that is not there)
  u32 first_addr = 0;             // the first wrong block (lowest address); first_snapshot / first_memory = its 8 words
  std::array<u32, VERIFY_FIRST_WORDS> first_snapshot{};
  std::array<u32, VERIFY_FIRST_WORDS> first_memory{};
  bool has_first_in_set = false;  // the first wrong block that lies in the rewritten memory, if any
  u32 first_in_set_addr = 0;
  std::array<u32, VERIFY_FIRST_WORDS> first_in_set_snapshot{};
  std::array<u32, VERIFY_FIRST_WORDS> first_in_set_memory{};
  std::vector<VerifyRegionCount> regions;  // 64 KB regions with wrong blocks, ascending
  std::vector<Range> outside_pages;        // 4 KB pages with wrong blocks outside the rewritten memory, merged
  std::vector<Range> healed_pages;         // pages whose wrong blocks were overwritten with the shadow (heal), merged
  u32 healed_blocks = 0;
  u64 us = 0;                     // time of the comparison (and the heal)
};

class RestoreVerifier
{
public:
  // `micros` is a monotonic clock in microseconds (default: steady_clock). Allocates nothing until the first shadow is taken.
  RestoreVerifier(const VerifyOptions& options, GuestMemory& memory, Sink& sink, std::function<u64()> micros = {});

  // True while shadows are still wanted: enabled and the budget of verified restores is not used up.
  bool Active() const { return m_options.restores != 0 && (m_options.restores == VERIFY_ALL || m_checked < m_options.restores); }
  // True when `frame` should get a shadow now (Active and frame % every == 0).
  bool WantsShadow(u32 frame) const { return Active() && m_options.every != 0 && frame % m_options.every == 0; }
  // Copies all of guest MEM1 + MEM2 (the parts that exist) into the shadow slot of `frame`. False: a read failed (nothing is kept).
  bool TakeShadow(u32 frame);
  bool HasShadow(u32 frame) const;
  void DropShadows();  // forget every shadow (the memory stays allocated)

  // Compares all guest memory with the shadow of in.to_frame, 32-byte blocks, minus in.skip. With options.heal also copies the shadow's
  // bytes back over every wrong block (and reports their pages in healed_pages). result.checked is false when there is no shadow.
  // Counts the restore as verified (budget) and updates the totals below. Does not log (SyncTest formats the lines).
  VerifyResult Check(const VerifyInput& in);

  const VerifyOptions& Options() const { return m_options; }
  // Totals over all checks.
  u32 Checked() const { return m_checked; }                    // restores verified
  u32 Wrong() const { return m_wrong; }                        // ... that left at least one wrong block
  u64 WrongBlocksTotal() const { return m_blocks_total; }      // wrong blocks summed over the restores
  u64 InSetBlocksTotal() const { return m_in_set_total; }
  u64 OutsideSetBlocksTotal() const { return m_outside_total; }
  u64 DistinctBlocks() const { return m_distinct_blocks; }     // different 32-byte blocks that were ever wrong
  u64 DistinctPages() const { return m_distinct_pages; }       // different 4 KB pages that were ever wrong
  u32 FirstWrongFrom() const { return m_first_wrong_from; }    // the first restore that left memory wrong: asked at this frame ...
  u32 FirstWrongTo() const { return m_first_wrong_to; }        // ... back to this one ...
  u32 FirstWrongBlocks() const { return m_first_wrong_blocks; }  // ... with this many wrong blocks (0: none so far)
  u32 Shadows() const { return m_shadows; }                    // shadows taken
  u64 ShadowUs() const { return m_shadow_us + m_check_us; }    // time spent copying shadows and comparing (microseconds)
  size_t ShadowBytes() const;                                  // bytes held by the shadow slots now

private:
  struct Shadow
  {
    bool valid = false;
    u32 frame = 0;
    std::array<std::vector<u8>, 2> data;  // MEM1, MEM2
  };
  Shadow& SlotFor(u32 frame) { return m_slots[(frame / m_options.every) % m_slots.size()]; }
  const Shadow& SlotFor(u32 frame) const { return m_slots[(frame / m_options.every) % m_slots.size()]; }
  void Probe();
  const u8* View(u32 addr, size_t n);  // host pointer to guest memory (Peek, or a copy in m_buf)

  VerifyOptions m_options;
  GuestMemory& m_memory;
  Sink& m_sink;
  std::function<u64()> m_micros;
  std::vector<Shadow> m_slots;
  std::array<bool, 2> m_present{};  // MEM1 / MEM2 readable (probed once)
  bool m_probed = false;
  bool m_error_logged = false;
  std::vector<u8> m_buf;
  std::vector<u64> m_seen_blocks;   // bitmap of 32-byte blocks that were ever wrong (MEM1 then MEM2)
  std::vector<u64> m_seen_pages;    // bitmap of 4 KB pages that were ever wrong
  u32 m_checked = 0;
  u32 m_wrong = 0;
  u64 m_blocks_total = 0;
  u64 m_in_set_total = 0;
  u64 m_outside_total = 0;
  u64 m_distinct_blocks = 0;
  u64 m_distinct_pages = 0;
  u32 m_first_wrong_from = 0;
  u32 m_first_wrong_to = 0;
  u32 m_first_wrong_blocks = 0;
  u32 m_shadows = 0;
  u64 m_shadow_us = 0;
  u64 m_check_us = 0;
};

// "gcd(a, b) divides k" test used to pick shadow frames that a SyncTest (roll back K frames every EVERY-th frame) restores to: a restore at
// frame n goes to n - K, so shadows at frame % P == 0 are hit only when some multiple of P is congruent to -K modulo EVERY.
bool VerifyEveryCanHit(u32 every_p, u32 k, u32 sync_every);
// The smallest P >= K + 1 for which VerifyEveryCanHit holds (K + 1 keeps one shadow alive until its restore: the next shadow is taken K + 1
// frames later).
u32 AutoVerifyEvery(u32 k, u32 sync_every);

// ---- SyncTest -------------------------------------------------------------------------------------------------------------
struct SyncTestOptions
{
  int k = 1;                      // frames to roll back, 1..8
  int every = 1;                  // roll back when frame % every == 0
  u32 stack_window = 0x4000;      // bytes saved from the hook's sp upward (clipped at the end of its memory region)
  std::string whitelist_path;     // only printed in the header line
  u32 arm_commands = 600;         // arm after this many consecutive hook calls with both fighters found (ignored with arm_by_pad)
  bool arm_by_pad = false;        // arm / disarm on P1's Z press (edge) while fighters are present
  u32 restore_log_full = 0;       // 0: the classic RESTORE log policy (10 in full, up to 30 short lines). N > 0 (netplay): N restores log one
                                  // result line each, then one `RESTORE summary` line per restore_log_window frames
  s32 restore_log_window = 600;   // frames per RESTORE summary line (netplay policy)
  bool noop = false;              // capture then restore the same frame every frame; no comparisons
  bool invalidate_code = true;    // call GuestMemory::InvalidateCode for every restored range
  bool restore_timebase = false;  // REV3_SYNCTEST_TB=1: store the time base in each snapshot and set it back on restore
  u32 hardware_check_every = 600; // hook calls between two hardware exclusion checks while armed
  bool adaptive = true;           // REV3_SYNCTEST_ADAPTIVE: learn the snapshot set in the running match (see the top of this file)
  u32 scan_slices = 16;           // REV3_SYNCTEST_SCAN_SLICES: each hook call hashes 1/scan_slices of all pages (1..4096)
  u32 learn_cycles = 20;          // REV3_SYNCTEST_LEARN: full scan cycles (scan_slices calls each) needed before arming
  bool ef_pool = true;            // REV3_EF_POOL: force the nw4r::ef pool into the snapshot set at arm (FindEfPool)
  u32 learn_pad_pages = 0;        // REV3_SNAPSHOT_PAD: every page the tracker learned also pulls this many 4 KB pages on each side into the set
                                  // (0 = off, 0..16). More bytes per snapshot (see the NOTE learn pad line), fewer late relayouts when a game
                                  // block spans pages that change one after the other. A page that never changes costs only copy time.
  u32 ef_check_restores = 200;    // REV3_EFCHECK: restores (count, not frames) that get the ef list check before / after the write-back;
                                  // 0 = off, EF_CHECK_ALL = every restore. 200 restores = 200 checks of a few thousand reads each (microseconds)
  VerifyOptions verify;           // REV3_RB_VERIFY*: shadow copy of guest memory and a full comparison after restores (see "restore verify" above)
};

constexpr u32 EF_CHECK_ALL = 0xFFFFFFFFu;  // SyncTestOptions::ef_check_restores: check every restore

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
  u32 io_dirty_slots = 0;         // captures made with a disc read in flight (the slot is never a rollback target) ...
  u32 io_dirty_hw = 0;            // ... of which the hardware / IOS view saw the read
  u32 io_dirty_game = 0;          // ... of which the game's DVD library has a command executing
  u32 io_dirty_skipped = 0;       // SyncTest: rollbacks not made because the slot to restore was dirty (no earlier slot exists to take instead)
  u32 audio_restores = 0;         // restores that also put the audio hardware back
  u64 audio_us_total = 0;         // time of those audio restores (state load + event re-scheduling)
  u64 audio_us_max = 0;
  u32 ef_checks = 0;              // ef list checks after a restore
  u32 ef_bad = 0;                 // of those, how many found a broken list (and before a restore, see ef_bad_before)
  u32 ef_bad_before = 0;          // checks before a restore that found a broken list (the game's own state was already bad)
  u32 layout_refused = 0;         // restores refused because the snapshot was taken under another snapshot layout
  u32 late_learn_rollbacks = 0;   // rollbacks done within one scan cycle before pages were learned (those pages were not rewound)
  u32 verify_pages_added = 0;     // pages the restore verify (heal) added to the snapshot set
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

  // REV3_SNAPSHOT_INCLUDE entries (LoadInclusions): forced into the set at every arm, together with the ef pool (options.ef_pool). Fixed
  // ranges are included as given, pointer entries are read when arming. Call before the first command (nothing is logged here).
  void SetUserInclusions(std::vector<Range> fixed, std::vector<PtrExclusion> pointers = {});
  // The ranges forced into the set at the latest arm (ef pool + user entries, before the cuts), merged.
  const std::vector<Range>& ForcedRanges() const { return m_forced; }
  const EfPool& EfPoolInfo() const { return m_ef; }
  bool EfPoolFound() const { return m_ef_found; }

  // One command in, one reply out. All work (reads, snapshots, restores, logging) happens inside this call.
  Reply OnCommand(const Command& command);

  // Optional: REV3_SYNCTEST_HIDE=1. While SyncTest resimulates (from the RESTORED reply to the reply of the last resimulated frame) the
  // flags are set, so a SyncTest run exercises the protocol 3 patches. Not owned; must outlive this object. Cleared by Finish().
  void SetResimHide(ResimHideFlags* hide) { m_hide = hide; }

  // Optional: the freeze dump (REV3_SYNCTEST_DUMP). SyncTest dumps once when it arms, logs the RESTOREHW lines around the first
  // restores, and tells it about every command (frame, time, armed) for the stall watchdog. Not owned; null = off.
  void SetFreezeDump(FreezeDump* dump) { m_dump = dump; }

  // Restore verify (options.verify, REV3_RB_VERIFY*; REV3.md "Restore verify"). Capture takes the shadows, Restore compares after the
  // write-back, so both SyncTest and the netplay session (NetCapture / NetRestore call the same two functions) are covered.
  // SetVerifyIgnore: memory the comparison never looks at (REV3_RB_VERIFY_IGNORE), on top of what the snapshot already cuts (hook region,
  // user exclusions, hardware regions) and the dead stack below the hook's sp. Call before the first command.
  void SetVerifyIgnore(std::vector<Range> ranges) { m_verify_ignore = MergeRanges(std::move(ranges)); }
  const RestoreVerifier* Verifier() const { return m_verify.get(); }
  // Heal found pages the set misses; they are part of ComputeRanges() already, but the layout (m_ranges) does not have them yet. SyncTest
  // installs them by itself between a resimulation and the next frame; the netplay session installs them at its next arm (NetPrepareArm
  // calls ComputeRanges()). The pages learned this way, merged, are VerifyExtraRanges().
  bool VerifyPagesPending() const { return m_verify_dirty; }
  // " restores_checked=.. restores_wrong=.. verify_blocks=.. first_wrong_frame=.. shadow_ms=.." (leading space), appended to the SUMMARY line;
  // empty when verify is off. The netplay stats line appends it too.
  std::string VerifyStatsFields() const;
  // "VERIFY summary ..." (the end-of-run line); only call with verify on.
  std::string VerifySummaryText() const;
  const std::vector<Range>& VerifyExtraRanges() const { return m_verify_extra; }

  // Writes the final summary line (once). The destructor does not. Also clears the resimulation hiding flags.
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
  // True while the game's DVD library has a command executing (DVD_EXECUTING_ADDR, see there).
  bool GameDvdBusy() const;
  // A disc read is in flight now, as the hardware / IOS or the game's DVD library see it. Used by the netplay gate (HOLD, quiet) and the captures.
  bool DiscBusy();
  // Netplay: false when the slot of `frame` exists and was captured with a disc read in flight (it must not be a rollback target).
  bool NetClean(u32 frame) const;
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
  // True when the set would change but slots are in use: learned pages, a new hardware cut, or pages the restore verify heal found missing.
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
  // "NOTE snapshot stats ..." (restores, restore / icache / audio times, postponed reads); written at shutdown. Ends with VerifyStatsFields()
  // (restore verify) when REV3_RB_VERIFY is on.
  std::string NetStatsLine() const;
  // Fields appended to the session's SUMMARY lines: the restore verify counters and the resimulation hiding counters (leading space, may be
  // empty). Writes the `NOTE learned summary` line first when the learned pages changed since the last one, so every SUMMARY of a netplay
  // run is preceded by it. The session reaches this through SnapshotStore::SummaryNote.
  std::string NetSummaryNote();
  // " io_dirty_slots=.. io_dirty_hw=.. io_dirty_game=.. io_dirty_skipped=.." (leading space): the disc read bookkeeping of the captures.
  std::string IoStatsFields() const;
  // Sets the hiding flags object whose counters NetSummaryNote reports (netplay: the session's ResimHiding writes the flags itself). Not owned.
  void SetHideCounters(const ResimHideFlags* hide) { m_hide_counters = hide; }
  // Writes the pending RESTORE summary window (options.restore_log_full policy); call before the final stats at shutdown.
  void NetFlushLog();
  // REV3_NET_RANGE_CRC: the CRC of each snapshot range (m_ranges) as it was at the start of `frame`: from the frame's slot, or from live
  // memory when `live` (the caller is at that frame's own hook call). False: no slot and not live, or the set is empty.
  bool NetRangeCrcs(u32 frame, bool live, std::vector<RangeHash>* out) const;

  // Learned-page bookkeeping (pages that entered the set after the first arm). NoteLearned adds `after` minus `before` to the all-time list
  // and to the list of pages not yet logged, and returns the new part. LearnedSummaryLine writes `NOTE learned summary pages=N ranges=[..]`
  // (+ `NOTE learned adjacency ..`); SyncTest::Finish and every SUMMARY line do. The netplay session calls NoteLearned when the set grows.
  std::vector<Range> NoteLearned(const std::vector<Range>& before, const std::vector<Range>& after);
  void LearnedSummaryLine();
  const std::vector<Range>& LearnedRanges() const { return m_learned_all; }

private:
  struct Slot
  {
    bool valid = false;
    u32 layout_gen = 0;                                     // m_layout_gen at the capture: the data has that layout
    u32 frame = 0;
    u32 sp = 0;
    u32 stack_len = 0;
    std::vector<u8> data;                                   // whitelist ranges back to back, then the stack window
    std::array<u32, 2> fighter_addr{};                      // player 1 / 2 fighter struct addresses
    std::array<std::array<u8, FIELD_BYTES>, 2> fields{};    // raw field bytes per player
    std::array<PadBytes, 2> inputs{};
    bool io_dirty = false;                                  // a disc read was in flight when this slot was captured (hardware / IOS or the game's DVD library):
                                                            // restoring it would leave the game waiting for a completion that has already happened
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
  Reply OnCommandImpl(const Command& command);  // OnCommand without the hiding flags bookkeeping
  void SyncHideFlags();                         // writes m_hide's flags when m_resim changed since the last call
  // Netplay: installs `next` as the snapshot set (NetPrepareArm, NetClear). After the first install of a match, the pages that join the set are
  // learned pages: NoteLearned, `NOTE learned +n pages <when> (frame f): new=[..]` (tools/rollback/learned_include.py reads it), and the part
  // that the restore verify heal found is counted in verify_pages_added. Does not rebuild the layout.
  void NetInstallSet(std::vector<Range> next, u32 frame, const char* when);
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
  std::vector<Range> ComputeRanges() const { return ComputeRanges(m_forced); }
  std::vector<Range> ComputeRanges(const std::vector<Range>& forced) const;
  // Re-reads the ef pool and the user inclusions (arm time) into m_forced and logs what was found: `NOTE ef pool 0x..-0x.. bytes=..` or
  // `NOTE ef pool not found: <why>`, the extras, `NOTE include ptr ..`, and how many bytes the forced ranges add to the set.
  void ResolveInclusions();
  // The ef list check around a restore (options.ef_check_restores): `when` = "before" / "after". Logs EFCHECK lines.
  void EfCheck(const char* when, u32 from, u32 to);
  void LearnWhileArmed(u32 added);
  // Restore verify: compares all memory with the shadow of `to` after a write-back and logs / counts the result (see VerifyOptions).
  void VerifyAfterRestore(const Slot& s, const Command& command, u32 to);
  // Installs the pages the heal added (m_verify_extra) into the layout: new set, relayout, history cleared. SyncTest only, never mid-resim.
  void ApplyVerifyPages();
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
  std::vector<Range> m_include_fixed;           // REV3_SNAPSHOT_INCLUDE ranges
  std::vector<PtrExclusion> m_include_ptrs;     // REV3_SNAPSHOT_INCLUDE pointer entries (resolved at arm)
  std::vector<Range> m_forced;                  // resolved at the latest arm: ef pool + the user entries, merged
  std::unique_ptr<RestoreVerifier> m_verify;    // null = restore verify off (options.verify.restores == 0)
  std::vector<Range> m_verify_ignore;           // REV3_RB_VERIFY_IGNORE: never compared
  std::vector<Range> m_verify_extra;            // pages the heal found missing: part of every ComputeRanges() from then on
  bool m_verify_dirty = false;                  // m_verify_extra has pages the current layout (m_ranges) lacks
  u32 m_verify_wrong_logs = 0;                  // VERIFY lines written for restores with wrong blocks (limit VERIFY_MAX_WRONG_LOGS)
  u32 m_verify_ok_logs = 0;
  EfPool m_ef;
  bool m_ef_found = false;
  u32 m_ef_checks_done = 0;                     // restores that got the check (limit: options.ef_check_restores)
  u32 m_ef_bad_logs = 0;
  u32 m_ef_ok_logs = 0;
  u32 m_layout_gen = 0;                         // bumped by every RebuildLayout; a snapshot of another generation is never restored
  u32 m_last_restore_call = 0;                  // m_armed_calls at the latest restore (late-learn detection)
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
  std::vector<Range> m_learned_all;       // every range that entered the set after the first arm (merged)
  std::vector<Range> m_learned_unlogged_ranges;  // the part of it not yet named in a "NOTE learned" line
  size_t m_learned_fresh_ranges = 0;      // learned ranges (one per growth) ... and how many touched the set as it was
  size_t m_learned_touching = 0;
  u64 m_learned_summary_pages = ~0ull;    // pages in the latest `learned summary` line (a SUMMARY line repeats it only when this changed)
  u64 m_scan_iv_total = 0;       // scan time since the last HB line (microseconds), its count and its maximum
  u32 m_scan_iv_n = 0;
  u64 m_scan_iv_max = 0;
  bool m_prev_z = false;     // Z was down on the previous command
  bool m_z_pending = false;  // a Z press not yet acted on (acted on outside a resimulation)
  u32 m_restore_logs = 0;    // RESTORE lines written (the first 10 in full, restores 11..30 as one short line)
  struct RestoreWindow       // the netplay policy: restores since the last RESTORE summary line
  {
    bool begun = false;
    u32 start = 0;
    u32 last = 0;
    u32 n = 0;
    u64 us_total = 0;
    u64 us_max = 0;
    u64 icache_max = 0;
  } m_rwin;
  void NoteRestoreWindow(u32 frame, u64 us, u64 icache_us);
  void FlushRestoreWindow();
  std::function<std::string()> m_dsp_state;
  FreezeDump* m_dump = nullptr;      // null = no freeze dump
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
  bool m_net_mode = false;         // NetObserve has been called: this is a netplay run (the restore verify heal then asks for a relayout)
  bool m_net_armed_once = false;   // a set has been installed in this match: pages joining later are learned pages (the first one learns nothing)
  ResimHideFlags* m_hide = nullptr;                // SyncTest: REV3_SYNCTEST_HIDE
  bool m_hide_on = false;                          // the latest value passed to m_hide->Set
  const ResimHideFlags* m_hide_counters = nullptr; // netplay: only read, for the summary fields

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

// What the soak bot / the automatic exit see of the game at a hook call (Bot::GameObs): both fighters found, a found fighter at health 0
// (+0x28 == 0, as the arm gate reads it), the scene type / sub words (rev3-mod tools/takes/rev3_menu.py SCENE_TYPE / SCENE_SUB). Read-only.
Bot::GameObs ReadGameObs(const GuestMemory& memory);

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

  // REV3_PAD_BOT: after the script has had its say (and before the record and the trace), the bot may replace the local pad
  // (command->pads[0], pad_return = 0x80000000) on calls that are not resimulations. Not owned; null = no bot. See Rev3Bot.h for the takeover rule.
  void SetBot(Bot::PadBotController* bot) { m_bot = bot; }
  bool HasBot() const { return m_bot != nullptr; }

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
  Bot::PadBotController* m_bot = nullptr;
};

// ---- Dolphin glue ---------------------------------------------------------------------------------------------------------
// Called by CEXISlippi::DMAWrite when IsCommand(). `command` points at the DMA'd bytes (>= CMD_SIZE). Fills *read_queue with
// the 64-byte reply (clearing it first). On the CPU thread.
void HandleCommand(Core::System& system, const u8* command, size_t size, std::vector<u8>* read_queue);

// True when REV3_NET_ROLE is set (netplay is wanted, whether or not its other variables turn out usable). The EXI device uses it to skip
// everything of Slippi's online stack (Rust extensions, login, matchmaking, spectator, replays, reporting): a Rev3 session talks only to
// its peer.
bool NetplayRequested();

// Reads REV3_RB_VERIFY, REV3_RB_VERIFY_EVERY, REV3_RB_VERIFY_SLOTS, REV3_RB_VERIFY_HEAL, REV3_RB_VERIFY_STACK_BELOW into options->verify and
// REV3_RB_VERIFY_IGNORE (a {"ranges":[..]} file) into *ignore; problems are logged to `sink` as ERROR lines and that variable stays at its
// default. Shared by SyncTest and the netplay session's options code. Defined in the Dolphin glue (needs the environment and files).
void ParseVerifyEnv(SyncTestOptions* options, std::vector<Range>* ignore, Sink& sink);

// Writes the final summary and closes the log; the next command starts a fresh session. Safe to call any time.
void Shutdown();
}  // namespace Rev3
