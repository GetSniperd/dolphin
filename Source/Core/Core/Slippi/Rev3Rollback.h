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
//                            strings or numbers, end exclusive. Required when SyncTest is on.
//   REV3_SYNCTEST_EVERY=N    roll back when frame % N == 0 (default 1 = every frame)
//   REV3_STACK_WINDOW=bytes  bytes saved and restored from the hook's sp upward (default 0x4000; decimal or 0x hex)
//   REV3_SYNCTEST_LOG=path   log file (default <User>/Logs/rev3_synctest.log); appended to, flushed after every line
//   SyncTest only runs inside a fight (both fighters found through the TEAMS pointers for 60 consecutive commands); menus
//   and loading screens (DVD DMA) are never rolled back. See REV3.md for what a clean run looks like.
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
#include <string>
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
};

class Sink
{
public:
  virtual ~Sink() = default;
  virtual void Line(const std::string& text) = 0;  // one log line (no newline); the real sink appends and flushes
  virtual void Osd(const std::string& text) = 0;   // an on-screen message
};

// ---- SyncTest -------------------------------------------------------------------------------------------------------------
struct SyncTestOptions
{
  int k = 1;                      // frames to roll back, 1..8
  int every = 1;                  // roll back when frame % every == 0
  u32 stack_window = 0x4000;      // bytes saved from the hook's sp upward (clipped at the end of its memory region)
  std::string whitelist_path;     // only printed in the header line
};

struct SyncTestStats
{
  u32 rollbacks = 0;
  u32 frames_compared = 0;
  u32 mismatching_frames = 0;
  u32 sp_mismatches = 0;
  u64 restore_us_total = 0;
  u64 restore_us_max = 0;
};

class SyncTest
{
public:
  // `micros` returns a monotonic clock in microseconds (restore times); the default is std::chrono::steady_clock.
  SyncTest(const SyncTestOptions& options, std::vector<Range> ranges, GuestMemory& memory, Sink& sink,
           std::function<u64()> micros = {});

  // One command in, one reply out. All work (reads, snapshots, restores, logging) happens inside this call.
  Reply OnCommand(const Command& command);

  // Writes the final summary line (once). The destructor does not.
  void Finish();

  const SyncTestStats& Stats() const { return m_stats; }
  bool Active() const { return m_was_active; }
  bool Resimulating() const { return m_resim; }
  size_t SnapshotBytes() const { return m_snapshot_bytes; }

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
    std::vector<u32> crc;                                   // CRC32 per whitelist range
    std::array<PadBytes, 2> inputs{};
  };

  bool FindFighters(std::array<u32, 2>* addrs) const;
  bool Capture(Slot& slot, const Command& command, const std::array<u32, 2>& fighters);
  void Reset(const char* why);
  void Error(const std::string& text);
  Reply Echo(const Command& command) const;
  Reply Normal(const Command& command, const std::array<u32, 2>& fighters);
  Reply Resim(const Command& command, const std::array<u32, 2>& fighters);
  bool Rollback(const Command& command);
  void Compare(const Slot& reference, const Slot& live, u32 frame);
  void SummaryLine(const char* tag);
  Slot& SlotFor(u32 frame) { return m_ring[frame % m_ring.size()]; }

  SyncTestOptions m_options;
  std::vector<Range> m_ranges;
  GuestMemory& m_memory;
  Sink& m_sink;
  std::function<u64()> m_micros;

  std::vector<size_t> m_range_offsets;  // offset of each range inside Slot::data
  size_t m_ranges_bytes = 0;
  size_t m_snapshot_bytes = 0;
  std::vector<Slot> m_ring;  // k + 1 slots, slot of frame n = ring[n % (k + 1)]
  Slot m_scratch;            // the live capture of a resimulated frame, swapped into the ring after the comparison
  std::vector<u8> m_span;    // one fighter's field span, read in one go

  u32 m_streak = 0;          // consecutive commands with both fighters found
  bool m_was_active = false;  // streak >= 60 on the previous command
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

// ---- Dolphin glue ---------------------------------------------------------------------------------------------------------
// Called by CEXISlippi::DMAWrite when IsCommand(). `command` points at the DMA'd bytes (>= CMD_SIZE). Fills *read_queue with
// the 64-byte reply (clearing it first). On the CPU thread.
void HandleCommand(Core::System& system, const u8* command, size_t size, std::vector<u8>* read_queue);

// Writes the final summary and closes the log; the next command starts a fresh session. Safe to call any time.
void Shutdown();
}  // namespace Rev3
