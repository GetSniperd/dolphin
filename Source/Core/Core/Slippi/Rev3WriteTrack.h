// Rev3 rollback: host write tracking. Licensed under GPLv2+ like the rest of this fork.
//
// WHAT IT DOES, HOW IT FITS IN
//   Rev3::WriteTracker implements the Rev3::WriteWatch interface of Rev3Rollback.h (REV3.md "Host write tracking"): it write-protects
//   4 KB pages of the host memory that backs guest RAM, in EVERY host view of that memory, so the first write to a page that is not in the
//   snapshot set raises a fault. The fault handler copies the page as it was before the write (the "preimage"), makes the page writable
//   again in all views and lets the write go on; the CPU thread finds the page in Poll() at its next hook call. The snapshot set can then
//   take the page into the set without dropping its snapshot history (SyncTest::GrowSetKeepingHistory).
//
//   This file knows nothing about Dolphin: it works on plain host buffers (the unit test uses an mmap'd buffer with two views of one file,
//   like Dolphin's arena). Dolphin tells it where the views are (Rev3Rollback.cpp, DolphinWriteWatch, fed by MemoryManager::GetRamViews())
//   and calls HandleFault() from Core/MemTools.cpp before the JIT looks at a fault (EMM::SetWriteFaultHook). Without Dolphin the file ships
//   its own handler (InstallStandaloneHandler: POSIX sigaction with chaining to the previous handler, Windows first vectored handler);
//   the unit tests use it.
//
// RULES THE HANDLER FOLLOWS (it runs inside a signal / exception, possibly on any thread: CPU, GPU, DSP, IOS)
//   - No allocation, no locks, no logging: atomics, memcpy and mprotect / VirtualProtect only.
//   - A fault outside the registered views, or on a page that is not watched, is not ours: HandleFault returns false (the caller chains
//     on). A fault INSIDE a registered view on a page that is no longer watched (a second thread racing the first, a protection that lags
//     behind a release) is made writable again and resumed: progress is guaranteed.
//
// WHAT IT CANNOT SEE
//   - Writes done by the kernel on behalf of the process (read() / recv() straight into protected guest memory) fail with EFAULT instead of
//     faulting. Dolphin's disc paths copy through CopyToEmu (no kernel write into the arena), IOS file reads and sockets may not. Tracking is
//     only armed while a match is on (SyncTest arm), not during loads.
//   - Writes that were done before a page was armed (the caller arms at the start of a fight and compares against the snapshot set).
//
// Tunables: none here (REV3_RB_WRITE_TRACK_REARM lives in SyncTestOptions). Windows: needs the views to be separate mappings of a section
// created PAGE_READWRITE; VirtualProtect runs never span two views (SetViews splits them).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Slippi/Rev3Rollback.h"

namespace Rev3
{
// One host view of guest RAM: `size` bytes at host address `host` hold the guest bytes from guest effective address `guest` on (MEM1
// 0x80000000-0x817FFFFF or MEM2 0x90000000-0x93FFFFFF; physical | 0x80000000). `host` and `size` are multiples of the 4 KB host page.
struct WriteView
{
  u32 guest = 0;
  u8* host = nullptr;
  u32 size = 0;
};

// Called by the fault handler, on the faulting thread, for the first write to a page: fills the guest pc and lr of that thread and returns
// true when it is the emulated CPU's thread (the JIT keeps pc at the start of the current block, lr is exact). Must be async-signal-safe.
using ContextProbe = bool (*)(u32* pc, u32* lr);

class WriteTracker final : public WriteWatch
{
public:
  WriteTracker();
  ~WriteTracker() override;
  WriteTracker(const WriteTracker&) = delete;
  WriteTracker& operator=(const WriteTracker&) = delete;

  // Replaces the host views (any thread, not from the handler). Views are clipped to MEM1 / MEM2, split where a run would cross from one to
  // the other, and must not overlap on the host. While watching, a changed view list is a restart: everything is made writable, every
  // mark is dropped and Active() turns false (Stats().restarts counts it); the caller arms again after a clean start. An identical list is
  // ignored. False (and *why) when the list is not usable: no view, a host page size other than 4 KB, an unaligned view.
  bool SetViews(const std::vector<WriteView>& views, std::string* why);
  size_t ViewCount() const;

  void SetContextProbe(ContextProbe probe) { m_probe.store(probe); }
  // The handler is installed (Dolphin's EMM hook, or InstallStandaloneHandler). SetWatched refuses to protect anything before this is
  // true: without a handler the first write would kill the process.
  void SetHandlerInstalled(bool installed) { m_handler_installed.store(installed); }
  // Writes the same byte back to an armed page through a host view and checks that exactly that page went dirty. Call after SetWatched.
  // The page is armed again afterwards. False (and *why) when it did not fault: write tracking does not work here.
  bool SelfTest(std::string* why) override;

  // ---- WriteWatch ----
  std::string Describe() const override;
  bool SetWatched(const std::vector<Range>& keep, std::string* why) override;
  void Stop() override;
  void Poll(std::vector<DirtyPage>* out) override;
  std::vector<u32> Rearm(const std::vector<u32>& pages) override;
  void Release(const std::vector<u32>& pages) override;
  bool IsClean(u32 page_addr) const override;
  bool Active() const override { return m_active.load(); }
  WriteWatchStats Stats() const override;

  // ---- fault entry ----
  // The tracker the handlers talk to (at most one at a time; Install makes this one the target, the destructor removes it).
  void MakeCurrent();
  // True: the fault at host address `host_address` was a write to a registered view and is dealt with, resume the thread. False: not ours.
  static bool HandleFault(uintptr_t host_address);

private:
  struct Table;
  bool OnFault(uintptr_t host_address);
  void ProtectPages(const std::vector<u8>& mark, bool read_only, u64* calls);
  bool UnprotectPageEverywhere(const Table& table, u32 index);
  void DropAllMarks();

  std::vector<std::unique_ptr<Table>> m_tables;            // every table ever published (old ones are only freed at the end: a handler
                                                           // may still be reading one)
  std::vector<WriteView> m_views;                          // the list the latest table was built from
  std::atomic<const Table*> m_table{nullptr};
  std::unique_ptr<std::atomic<u8>[]> m_state;              // per tracked page, see the state list in the .cpp
  std::vector<u32> m_pc;
  std::vector<u32> m_lr;
  std::vector<u8> m_cpu;
  std::vector<u8> m_equal;
  u8* m_pre = nullptr;                                     // preimages, one 4 KB slot per tracked page (lazily committed)
  size_t m_pre_bytes = 0;
  std::atomic<ContextProbe> m_probe{nullptr};
  std::atomic<bool> m_handler_installed{false};
  std::atomic<bool> m_active{false};
  std::atomic<u32> m_dirty_count{0};
  std::atomic<u64> m_faults{0};
  std::atomic<u64> m_stale{0};
  std::atomic<u64> m_protect_errors{0};
  std::atomic<u32> m_in_handler{0};
  u64 m_protect_calls = 0;
  u64 m_pages_protected = 0;
  u64 m_protect_us = 0;
  u64 m_rearms = 0;
  u64 m_rearm_changed = 0;
  u64 m_polls = 0;
  u64 m_poll_us = 0;
  u32 m_restarts = 0;
};

// ---- stand-alone fault handler (unit tests, tools; Dolphin uses EMM::SetWriteFaultHook instead) ----
// POSIX: sigaction(SIGSEGV) that calls WriteTracker::HandleFault first and chains to the handler that was installed before for every fault it
// does not take. Windows: a first-in-line vectored exception handler (access violation, write). Returns false (and *why) on failure.
bool InstallStandaloneHandler(std::string* why);
void UninstallStandaloneHandler();

// Host page size in bytes (sysconf / GetSystemInfo); the tracker needs 4096.
u32 HostPageSize();
}  // namespace Rev3
