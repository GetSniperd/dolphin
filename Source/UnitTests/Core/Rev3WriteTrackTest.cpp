// Unit tests for Core/Slippi/Rev3WriteTrack: the host write tracker, run for real (mprotect / VirtualProtect and the stand-alone fault
// handler) on plain host buffers: one memory object mapped twice, like Dolphin's arena maps guest RAM into several views. Also the
// SyncTest integration tests that need the real tracker. No Dolphin core, no game data. Run: build target `tests`,
// then `tests --gtest_filter=Rev3WriteTrack*`.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "Core/Slippi/Rev3Rollback.h"
#include "Core/Slippi/Rev3WriteTrack.h"

namespace
{
using namespace Rev3;

constexpr u32 kGuestBase = 0x80000000;
constexpr u32 kBackingBytes = 0x100000;  // 1 MB = 256 pages
constexpr u32 kPageBytes = 0x1000;

// One piece of memory mapped twice (views A and B), zero filled.
class Backing
{
public:
  Backing()
  {
#ifdef _WIN32
    m_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kBackingBytes, nullptr);
    if (m_handle != nullptr)
    {
      m_a = static_cast<u8*>(MapViewOfFile(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, kBackingBytes));
      m_b = static_cast<u8*>(MapViewOfFile(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, kBackingBytes));
    }
#else
    FILE* file = tmpfile();
    if (file != nullptr)
    {
      const int fd = fileno(file);
      if (ftruncate(fd, kBackingBytes) == 0)
      {
        void* a = mmap(nullptr, kBackingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void* b = mmap(nullptr, kBackingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        m_a = a == MAP_FAILED ? nullptr : static_cast<u8*>(a);
        m_b = b == MAP_FAILED ? nullptr : static_cast<u8*>(b);
      }
      fclose(file);  // the mappings keep the object alive
    }
#endif
  }
  ~Backing()
  {
#ifdef _WIN32
    if (m_a != nullptr)
      UnmapViewOfFile(m_a);
    if (m_b != nullptr)
      UnmapViewOfFile(m_b);
    if (m_handle != nullptr)
      CloseHandle(m_handle);
#else
    if (m_a != nullptr)
      munmap(m_a, kBackingBytes);
    if (m_b != nullptr)
      munmap(m_b, kBackingBytes);
#endif
  }
  bool Ok() const { return m_a != nullptr && m_b != nullptr; }
  u8* A() const { return m_a; }
  u8* B() const { return m_b; }
  std::vector<WriteView> Views() const { return {{kGuestBase, m_a, kBackingBytes}, {kGuestBase, m_b, kBackingBytes}}; }

private:
  u8* m_a = nullptr;
  u8* m_b = nullptr;
#ifdef _WIN32
  HANDLE m_handle = nullptr;
#endif
};

// volatile: the fault handler reads it behind the compiler's back
thread_local volatile bool t_cpu_thread = false;
bool Probe(u32* pc, u32* lr)
{
  *pc = 0x80123450;
  *lr = 0x80123456;
  return t_cpu_thread;
}

struct Rig
{
  Backing mem;
  WriteTracker tracker;
  bool handler_ok = false;

  Rig()
  {
    std::string why;
    handler_ok = InstallStandaloneHandler(&why);
    tracker.MakeCurrent();
    tracker.SetHandlerInstalled(handler_ok);
    tracker.SetContextProbe(&Probe);
    if (mem.Ok())
      tracker.SetViews(mem.Views(), &why);
  }
  ~Rig() { UninstallStandaloneHandler(); }

  // Writes `value` to byte `offset` of the guest address range through view A or B.
  void Write(bool view_a, u32 offset, u8 value)
  {
    volatile u8* p = (view_a ? mem.A() : mem.B()) + offset;
    *p = value;
  }
  std::vector<DirtyPage> Poll()
  {
    std::vector<DirtyPage> out;
    tracker.Poll(&out);
    return out;
  }
  bool WatchAll()
  {
    std::string why;
    return tracker.SetWatched({}, &why);
  }
};

TEST(Rev3WriteTrack, SetViewsRejectsUnusableLists)
{
  WriteTracker tracker;
  std::string why;
  EXPECT_FALSE(tracker.SetViews({}, &why));
  EXPECT_FALSE(why.empty());
  Backing mem;
  ASSERT_TRUE(mem.Ok());
  EXPECT_FALSE(tracker.SetViews({{kGuestBase, mem.A() + 1, kPageBytes}}, &why));  // unaligned host
  EXPECT_FALSE(tracker.SetViews({{kGuestBase + 4, mem.A(), kPageBytes}}, &why));  // unaligned guest
  EXPECT_FALSE(tracker.SetViews({{kGuestBase, mem.A(), kPageBytes}, {kGuestBase, mem.A(), kPageBytes}}, &why));  // overlapping
  EXPECT_FALSE(tracker.SetViews({{0x10000000, mem.A(), kPageBytes}}, &why));  // not RAM
  EXPECT_TRUE(tracker.SetViews(mem.Views(), &why));
  EXPECT_EQ(2u, tracker.Stats().views);
}

TEST(Rev3WriteTrack, RefusesToProtectWithoutAHandler)
{
  Backing mem;
  ASSERT_TRUE(mem.Ok());
  WriteTracker tracker;
  std::string why;
  ASSERT_TRUE(tracker.SetViews(mem.Views(), &why));
  EXPECT_FALSE(tracker.SetWatched({}, &why));
  EXPECT_NE(std::string::npos, why.find("handler"));
  mem.A()[0] = 1;  // still writable: nothing was protected
}

TEST(Rev3WriteTrack, FirstWriteFaultsRecordsThePreimageAndLetsTheWriteThrough)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  std::memset(rig.mem.A() + 3 * kPageBytes, 0x5A, kPageBytes);
  ASSERT_TRUE(rig.WatchAll());
  EXPECT_TRUE(rig.tracker.Active());
  EXPECT_TRUE(rig.tracker.IsClean(kGuestBase + 3 * kPageBytes));
  EXPECT_TRUE(rig.Poll().empty());

  t_cpu_thread = true;
  rig.Write(true, 3 * kPageBytes + 17, 0x77);
  t_cpu_thread = false;
  EXPECT_EQ(0x77, rig.mem.B()[3 * kPageBytes + 17]);  // the write went through, visible in the other view

  const std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  EXPECT_EQ(kGuestBase + 3 * kPageBytes, dirty[0].addr);
  EXPECT_EQ(0x5A, dirty[0].preimage[17]);  // the page as it was BEFORE the write
  for (u32 i = 0; i < kPageBytes; ++i)
    ASSERT_EQ(0x5A, dirty[0].preimage[i]) << i;
  EXPECT_EQ(0x80123450u, dirty[0].pc);
  EXPECT_EQ(0x80123456u, dirty[0].lr);
  EXPECT_TRUE(dirty[0].cpu_thread);
  EXPECT_FALSE(rig.tracker.IsClean(kGuestBase + 3 * kPageBytes));

  // The page is writable in every view now: more writes do not fault again.
  rig.Write(true, 3 * kPageBytes + 18, 1);
  rig.Write(false, 3 * kPageBytes + 19, 2);
  const WriteWatchStats stats = rig.tracker.Stats();
  EXPECT_EQ(1u, stats.faults);
  EXPECT_EQ(0u, stats.stale_faults);
  EXPECT_EQ(0u, stats.protect_errors);
}

TEST(Rev3WriteTrack, AWriteThroughTheSecondViewFaultsToo)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(false, 9 * kPageBytes, 4);
  const std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  EXPECT_EQ(kGuestBase + 9 * kPageBytes, dirty[0].addr);
  EXPECT_FALSE(dirty[0].cpu_thread);
  rig.Write(true, 9 * kPageBytes + 1, 5);  // view A was unprotected as well
  EXPECT_EQ(1u, rig.tracker.Stats().faults);
  EXPECT_EQ(0u, rig.tracker.Stats().stale_faults);
}

TEST(Rev3WriteTrack, OnlyPagesNotFullyInsideTheKeepSetAreWatched)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  std::string why;
  // Keep: pages 0 and 1 completely, and the middle of page 2 (an edge page: watched).
  ASSERT_TRUE(rig.tracker.SetWatched({{kGuestBase, kGuestBase + 2 * kPageBytes}, {kGuestBase + 2 * kPageBytes + 0x100, kGuestBase + 2 * kPageBytes + 0x200}}, &why));
  EXPECT_FALSE(rig.tracker.IsClean(kGuestBase));
  EXPECT_FALSE(rig.tracker.IsClean(kGuestBase + kPageBytes));
  EXPECT_TRUE(rig.tracker.IsClean(kGuestBase + 2 * kPageBytes));
  rig.Write(true, 5, 1);
  rig.Write(true, kPageBytes + 5, 1);
  EXPECT_EQ(0u, rig.tracker.Stats().faults);
  rig.Write(true, 2 * kPageBytes + 0x150, 1);  // inside the kept bytes of an edge page still faults: the page is watched as a whole
  EXPECT_EQ(1u, rig.tracker.Stats().faults);
  // Adjacent keep ranges that together cover a page count as covering it.
  Rig rig2;
  ASSERT_TRUE(rig2.tracker.SetWatched({{kGuestBase + 4 * kPageBytes, kGuestBase + 4 * kPageBytes + 0x800}, {kGuestBase + 4 * kPageBytes + 0x800, kGuestBase + 5 * kPageBytes}}, &why));
  EXPECT_FALSE(rig2.tracker.IsClean(kGuestBase + 4 * kPageBytes));
}

TEST(Rev3WriteTrack, RearmOfAnUnchangedPageGuardsItAgain)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  std::memset(rig.mem.A() + 7 * kPageBytes, 0x33, kPageBytes);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(true, 7 * kPageBytes + 1, 0x33);  // the same value again
  std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  EXPECT_EQ(0u, dirty[0].equal_rearms);
  EXPECT_TRUE(rig.tracker.Rearm({dirty[0].addr}).empty());
  EXPECT_TRUE(rig.tracker.IsClean(dirty[0].addr));
  EXPECT_TRUE(rig.Poll().empty());
  rig.Write(false, 7 * kPageBytes + 2, 0x33);  // guarded again, in the other view too
  dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  EXPECT_EQ(1u, dirty[0].equal_rearms);
  EXPECT_EQ(2u, rig.tracker.Stats().faults);
  EXPECT_EQ(1u, rig.tracker.Stats().rearms);
}

TEST(Rev3WriteTrack, RearmOfAChangedPageStaysDirtyAndSaysSo)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(true, 11 * kPageBytes + 1, 0x99);
  std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  const std::vector<u32> changed = rig.tracker.Rearm({dirty[0].addr});  // the caller was wrong: the page differs from the preimage
  ASSERT_EQ(1u, changed.size());
  EXPECT_EQ(dirty[0].addr, changed[0]);
  EXPECT_EQ(1u, rig.Poll().size());  // still dirty
  EXPECT_FALSE(rig.tracker.IsClean(dirty[0].addr));
  rig.Write(true, 11 * kPageBytes + 2, 1);  // and writable
  EXPECT_EQ(1u, rig.tracker.Stats().faults);
  EXPECT_EQ(1u, rig.tracker.Stats().rearm_changed);
}

TEST(Rev3WriteTrack, ReleaseMakesAPageWritableAndUnwatched)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(true, 2 * kPageBytes, 1);
  ASSERT_EQ(1u, rig.Poll().size());
  rig.tracker.Release({kGuestBase + 2 * kPageBytes, kGuestBase + 4 * kPageBytes});  // one dirty, one still armed
  EXPECT_TRUE(rig.Poll().empty());
  rig.Write(true, 4 * kPageBytes, 1);
  rig.Write(false, 4 * kPageBytes + 1, 1);
  EXPECT_EQ(1u, rig.tracker.Stats().faults);  // no new fault
  EXPECT_FALSE(rig.tracker.IsClean(kGuestBase + 4 * kPageBytes));
}

TEST(Rev3WriteTrack, StopMakesEverythingWritableAndInactive)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(true, 0, 1);
  rig.tracker.Stop();
  EXPECT_FALSE(rig.tracker.Active());
  EXPECT_TRUE(rig.Poll().empty());
  for (u32 p = 0; p < kBackingBytes / kPageBytes; ++p)
  {
    rig.Write(p % 2 == 0, p * kPageBytes, 7);
    ASSERT_FALSE(rig.tracker.IsClean(kGuestBase + p * kPageBytes));
  }
  EXPECT_EQ(1u, rig.tracker.Stats().faults);
  EXPECT_EQ(0u, rig.tracker.Stats().stale_faults);
}

TEST(Rev3WriteTrack, ChangingTheViewsWhileWatchingRestarts)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  rig.Write(true, 5 * kPageBytes, 1);
  std::string why;
  EXPECT_TRUE(rig.tracker.SetViews(rig.mem.Views(), &why));  // identical list: ignored
  EXPECT_TRUE(rig.tracker.Active());
  rig.tracker.Stop();
  ASSERT_TRUE(rig.WatchAll());
  EXPECT_TRUE(rig.tracker.SetViews({{kGuestBase, rig.mem.A(), kBackingBytes}}, &why));  // one view less, while watching
  EXPECT_FALSE(rig.tracker.Active());
  EXPECT_EQ(1u, rig.tracker.Stats().restarts);
  EXPECT_TRUE(rig.Poll().empty());
  EXPECT_FALSE(rig.tracker.IsClean(kGuestBase + 5 * kPageBytes));
}

TEST(Rev3WriteTrack, SelfTestSeesTheFaultAndLeavesThePageGuarded)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  std::string why;
  EXPECT_TRUE(rig.tracker.SelfTest(&why)) << why;
  EXPECT_TRUE(rig.Poll().empty());
  EXPECT_TRUE(rig.tracker.IsClean(kGuestBase));
}

TEST(Rev3WriteTrack, SelfTestFailsWhenNothingIsWatched)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  std::string why;
  EXPECT_FALSE(rig.tracker.SelfTest(&why));
  EXPECT_FALSE(why.empty());
}

TEST(Rev3WriteTrack, ManyThreadsWritingOnePageRaceToOneFault)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  std::memset(rig.mem.A() + 6 * kPageBytes, 0x11, kPageBytes);
  ASSERT_TRUE(rig.WatchAll());
  constexpr int kThreads = 8;
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
  {
    threads.emplace_back([&, t] {
      ready.fetch_add(1);
      while (ready.load() < kThreads)
      {
      }
      rig.Write(t % 2 == 0, 6 * kPageBytes + t, static_cast<u8>(0xA0 + t));
    });
  }
  for (std::thread& thread : threads)
    thread.join();
  for (int t = 0; t < kThreads; ++t)
    EXPECT_EQ(0xA0 + t, rig.mem.A()[6 * kPageBytes + t]);  // every write landed
  const std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  for (u32 i = 0; i < kPageBytes; ++i)
    ASSERT_EQ(0x11, dirty[0].preimage[i]) << i;  // the preimage is from before ANY of the writes, whichever thread was first
  EXPECT_EQ(1u, rig.tracker.Stats().faults);
  EXPECT_EQ(0u, rig.tracker.Stats().protect_errors);
}

TEST(Rev3WriteTrack, ManyThreadsOverManyPagesLoseNothing)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  constexpr u32 kPages = kBackingBytes / kPageBytes;
  for (u32 p = 0; p < kPages; ++p)
    std::memset(rig.mem.A() + p * kPageBytes, static_cast<int>(p), kPageBytes);
  ASSERT_TRUE(rig.WatchAll());
  constexpr int kThreads = 6;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
  {
    threads.emplace_back([&, t] {
      for (u32 p = static_cast<u32>(t); p < kPages; p += 3)  // overlapping pages between threads
        rig.Write((p + static_cast<u32>(t)) % 2 == 0, p * kPageBytes + 64 + static_cast<u32>(t), static_cast<u8>(0xF0 + t));
    });
  }
  for (std::thread& thread : threads)
    thread.join();
  const std::vector<DirtyPage> dirty = rig.Poll();
  EXPECT_EQ(kPages, dirty.size());
  for (const DirtyPage& d : dirty)
  {
    const u32 p = (d.addr - kGuestBase) / kPageBytes;
    for (u32 i = 0; i < kPageBytes; ++i)
      ASSERT_EQ(static_cast<u8>(p), d.preimage[i]) << "page " << p << " byte " << i;
  }
  EXPECT_EQ(kPages, rig.tracker.Stats().faults);
  EXPECT_EQ(0u, rig.tracker.Stats().protect_errors);
}

// One writer keeps rewriting a byte with the value it already has while the main thread polls and re-arms. After thousands of equal
// re-arms the first real change must still be caught.
TEST(Rev3WriteTrack, ARealChangeIsCaughtAfterManyEqualRearms)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  ASSERT_TRUE(rig.WatchAll());
  const u32 addr = kGuestBase + 12 * kPageBytes;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    while (!stop.load())
      rig.Write(true, 12 * kPageBytes + 5, 0);  // the value it has
  });
  u64 rearmed = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (rearmed < 3000 && std::chrono::steady_clock::now() < deadline)
  {
    for (const DirtyPage& d : rig.Poll())
    {
      if (d.addr == addr)
      {
        EXPECT_TRUE(rig.tracker.Rearm({addr}).empty());
        ++rearmed;
      }
    }
  }
  stop.store(true);
  writer.join();
  EXPECT_GE(rearmed, 20u);  // a VM with slow cross-CPU TLB flushes manages only a few dozen per second
  for (const DirtyPage& d : rig.Poll())
  {
    if (d.addr == addr)
      rig.tracker.Rearm({addr});
  }
  ASSERT_TRUE(rig.tracker.IsClean(addr) || rig.Poll().size() == 1);
  rig.Write(true, 12 * kPageBytes + 5, 1);  // a real change
  const std::vector<DirtyPage> dirty = rig.Poll();
  ASSERT_EQ(1u, dirty.size());
  EXPECT_EQ(0, dirty[0].preimage[5]);
  EXPECT_GT(dirty[0].equal_rearms, 0u);
  EXPECT_EQ(0u, rig.tracker.Stats().protect_errors);
}

// A writer toggles a byte (0 / 1) while the main thread polls and re-arms; the last write leaves 1. A page that differs from its preimage
// must NEVER end up guarded and clean: that would be a lost write (the protect-then-compare order of Rearm is what prevents it).
TEST(Rev3WriteTrack, NoWriteIsLostWhileARearmRacesTheWriter)
{
  Rig rig;
  ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
  const u32 addr = kGuestBase + 12 * kPageBytes;
  u64 rearm_calls = 0;
  for (int iteration = 0; iteration < 150; ++iteration)
  {
    rig.tracker.Stop();
    std::memset(rig.mem.A() + 12 * kPageBytes, 0, kPageBytes);
    ASSERT_TRUE(rig.WatchAll());
    std::atomic<bool> done{false};
    std::thread writer([&] {
      u8 v = 0;
      for (int i = 0; i < 3000; ++i)
      {
        v ^= 1;
        rig.Write(true, 12 * kPageBytes + 5, v);
      }
      rig.Write(true, 12 * kPageBytes + 5, 1);
      done.store(true);
    });
    while (!done.load())
    {
      for (const DirtyPage& d : rig.Poll())
      {
        if (d.addr == addr)
        {
          rig.tracker.Rearm({addr});
          ++rearm_calls;
        }
      }
    }
    writer.join();
    for (const DirtyPage& d : rig.Poll())
    {
      if (d.addr == addr)
        rig.tracker.Rearm({addr});
    }
    ASSERT_FALSE(rig.tracker.IsClean(addr)) << "iteration " << iteration << ": byte is 1, the preimage 0, yet the page is guarded";
    const std::vector<DirtyPage> dirty = rig.Poll();
    ASSERT_EQ(1u, dirty.size());
    ASSERT_EQ(0, dirty[0].preimage[5]);
  }
  EXPECT_GT(rearm_calls, 0u);
  EXPECT_EQ(0u, rig.tracker.Stats().protect_errors);
}

#ifndef _WIN32
// Faults the tracker does not own must reach the handler that was installed before it (Dolphin's, in the real thing).
struct PreviousHandlerState
{
  static inline std::atomic<int> calls{0};
  static inline void* page = nullptr;
};

void PreviousHandler(int, siginfo_t* info, void*)
{
  PreviousHandlerState::calls.fetch_add(1);
  // Repair the fault the way Dolphin's handler would resolve its own: make the page accessible, then return.
  mprotect(PreviousHandlerState::page, kPageBytes, PROT_READ | PROT_WRITE);
  (void)info;
}

TEST(Rev3WriteTrack, FaultsOutsideTheViewsAreChainedToThePreviousHandler)
{
  void* other = mmap(nullptr, kPageBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, other);
  PreviousHandlerState::page = other;
  PreviousHandlerState::calls = 0;

  struct sigaction mine;
  std::memset(&mine, 0, sizeof(mine));
  mine.sa_sigaction = PreviousHandler;
  mine.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&mine.sa_mask);
  struct sigaction before;
  ASSERT_EQ(0, sigaction(SIGSEGV, &mine, &before));
  {
    Rig rig;  // installs the stand-alone handler on top of PreviousHandler
    ASSERT_TRUE(rig.mem.Ok() && rig.handler_ok);
    ASSERT_TRUE(rig.WatchAll());

    // 1. A tracked write: ours, the previous handler must not see it.
    rig.Write(true, 3 * kPageBytes, 1);
    EXPECT_EQ(0, PreviousHandlerState::calls.load());
    EXPECT_EQ(1u, rig.tracker.Stats().faults);

    // 2. A fault on an address that is not in any view: chained, and the previous handler's repair lets the access go on.
    volatile u8* p = static_cast<volatile u8*>(other);
    *p = 42;
    EXPECT_EQ(1, PreviousHandlerState::calls.load());
    EXPECT_EQ(42, *p);
    EXPECT_EQ(1u, rig.tracker.Stats().faults);

    // 3. A tracked page again, after the chained fault: still ours.
    rig.Write(false, 8 * kPageBytes, 1);
    EXPECT_EQ(1, PreviousHandlerState::calls.load());
    EXPECT_EQ(2u, rig.tracker.Stats().faults);
  }
  // The stand-alone handler is gone and the previous one is back.
  struct sigaction now;
  ASSERT_EQ(0, sigaction(SIGSEGV, nullptr, &now));
  EXPECT_EQ(reinterpret_cast<void*>(PreviousHandler), reinterpret_cast<void*>(now.sa_sigaction));
  sigaction(SIGSEGV, &before, nullptr);
  munmap(other, kPageBytes);
}

TEST(Rev3WriteTrack, AFaultNobodyOwnsStillKillsTheProcess)
{
  // With the default action behind the stand-alone handler, an unowned fault must end in the usual crash, not in a loop or a swallow.
  EXPECT_DEATH(
      {
        Backing mem;
        WriteTracker tracker;
        std::string why;
        InstallStandaloneHandler(&why);
        tracker.MakeCurrent();
        tracker.SetHandlerInstalled(true);
        tracker.SetViews(mem.Views(), &why);
        tracker.SetWatched({}, &why);
        void* bad = mmap(nullptr, kPageBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *static_cast<volatile u8*>(bad) = 1;
      },
      "");
}
#endif
}  // namespace
