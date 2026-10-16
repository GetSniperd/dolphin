// Rev3 rollback: host write tracking (WriteTracker). See Rev3WriteTrack.h for what it does and how it fits in.
// Licensed under GPLv2+ like the rest of this fork.
//
// PAGE STATES (one atomic byte per tracked 4 KB page, index = MEM1 page, then MEM2 page: 22528 pages)
//   Off     not watched; writable in every view
//   Armed   watched, read-only in every view; contents == contents at the time it was armed (nobody wrote it since)
//   Claimed a thread took the first write fault and is copying the preimage (other faulting threads wait)
//   Dirty   written; the preimage holds the contents from before the first write; writable in every view
//   Rearm   the CPU thread is protecting the page again after finding it unchanged (a fault in this window waits for the verdict)
// Transitions: Off -> Armed (SetWatched), Armed -> Claimed -> Dirty (handler), Dirty -> Rearm -> Armed | Dirty (Rearm()), any -> Off
// (Release / Stop / SetWatched for pages that went fully inside the snapshot set).
// Rearm protects FIRST and compares AFTER: a write that happened between the caller's comparison and the protection is seen by the
// comparison, a write after the protection faults. No third case.

#include "Core/Slippi/Rev3WriteTrack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Rev3
{
namespace
{
constexpr u32 kPage = 0x1000;
constexpr u32 kMem1Start = 0x80000000;
constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kMem2Start = 0x90000000;
constexpr u32 kMem2Size = 0x04000000;
constexpr u32 kMem1Pages = kMem1Size / kPage;
constexpr u32 kPages = (kMem1Size + kMem2Size) / kPage;  // 22528 = TRACKED_PAGES
static_assert(kPages == TRACKED_PAGES, "page count");

enum State : u8
{
  kOff = 0,
  kArmed = 1,
  kClaimed = 2,
  kDirty = 3,
  kRearm = 4,
};

// Handler waits for a Claimed / Rearm page at most this many yields (each is one sched_yield); the owner needs microseconds.
constexpr int kMaxWaitYields = 200000;

// Index of the tracked page at guest effective address `ea`, or -1.
int PageIndexOf(u32 ea)
{
  if (ea >= kMem1Start && ea < kMem1Start + kMem1Size)
    return static_cast<int>((ea - kMem1Start) / kPage);
  if (ea >= kMem2Start && ea < kMem2Start + kMem2Size)
    return static_cast<int>(kMem1Pages + (ea - kMem2Start) / kPage);
  return -1;
}

u32 PageAddrOf(u32 index)
{
  if (index < kMem1Pages)
    return kMem1Start + index * kPage;
  return kMem2Start + (index - kMem1Pages) * kPage;
}

bool ProtectRange(void* address, size_t bytes, bool read_only)
{
#ifdef _WIN32
  DWORD old = 0;
  return VirtualProtect(address, bytes, read_only ? PAGE_READONLY : PAGE_READWRITE, &old) != 0;
#else
  return mprotect(address, bytes, read_only ? PROT_READ : (PROT_READ | PROT_WRITE)) == 0;
#endif
}

void YieldThread()
{
#ifdef _WIN32
  SwitchToThread();
#else
  sched_yield();
#endif
}

u64 NowUs()
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

// The tracker the fault entry talks to, and the number of handler invocations in flight (the destructor waits for them).
std::atomic<WriteTracker*> g_current{nullptr};
std::atomic<u32> g_in_handler{0};
}  // namespace

u32 HostPageSize()
{
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return static_cast<u32>(info.dwPageSize);
#else
  const long size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<u32>(size) : 0;
#endif
}

// Immutable once published. The handler reads it without a lock.
struct WriteTracker::Table
{
  struct View
  {
    uintptr_t host = 0;
    u32 size = 0;
    u32 first_page = 0;  // tracked page index of the first page of the view
  };
  std::vector<View> by_host;       // sorted by host address
  std::vector<u32> csr_start;      // kPages + 1: pages [csr_start[p], csr_start[p + 1]) of csr_host belong to page p
  std::vector<u8*> csr_host;       // host address of tracked page p in each view that has it
  u32 pages_with_view = 0;

  const View* Find(uintptr_t address) const
  {
    auto it = std::upper_bound(by_host.begin(), by_host.end(), address,
                               [](uintptr_t a, const View& v) { return a < v.host; });
    if (it == by_host.begin())
      return nullptr;
    --it;
    return address < it->host + it->size ? &*it : nullptr;
  }
};

WriteTracker::WriteTracker()
    : m_state(new std::atomic<u8>[kPages]), m_pc(kPages, 0), m_lr(kPages, 0), m_cpu(kPages, 0), m_equal(kPages, 0)
{
  for (u32 i = 0; i < kPages; ++i)
    m_state[i].store(kOff);
}

WriteTracker::~WriteTracker()
{
  // Take the handler away first: from here on no fault reaches this object, then wait for the ones inside.
  WriteTracker* self = this;
  g_current.compare_exchange_strong(self, nullptr);
  while (g_in_handler.load() != 0)
    YieldThread();
  Stop();
#ifdef _WIN32
  if (m_pre != nullptr)
    VirtualFree(m_pre, 0, MEM_RELEASE);
#else
  if (m_pre != nullptr)
    munmap(m_pre, m_pre_bytes);
#endif
}

void WriteTracker::MakeCurrent()
{
  g_current.store(this);
}

size_t WriteTracker::ViewCount() const
{
  const Table* table = m_table.load();
  return table == nullptr ? 0 : table->by_host.size();
}

bool WriteTracker::SetViews(const std::vector<WriteView>& views, std::string* why)
{
  auto fail = [&](const std::string& text) {
    if (why != nullptr)
      *why = text;
    return false;
  };
  const u32 host_page = HostPageSize();
  if (host_page != kPage)
    return fail("host page size is " + std::to_string(host_page) + " bytes, tracking needs 4096");

  // Clip to MEM1 / MEM2, split where a view would cross a region end (the page index must be contiguous inside a view).
  std::vector<WriteView> clean;
  for (const WriteView& v : views)
  {
    if (v.host == nullptr || v.size == 0)
      continue;
    if ((reinterpret_cast<uintptr_t>(v.host) & (kPage - 1)) != 0 || (v.size & (kPage - 1)) != 0 ||
        (v.guest & (kPage - 1)) != 0)
      return fail("a host view is not 4 KB aligned");
    u32 guest = v.guest;
    u8* host = v.host;
    u32 left = v.size;
    while (left != 0)
    {
      const int index = PageIndexOf(guest);
      if (index < 0)
      {
        // Outside RAM (the caller should have filtered it): skip one page at a time until RAM starts again.
        guest += kPage;
        host += kPage;
        left -= kPage;
        continue;
      }
      const u32 region_end = static_cast<u32>(index) < kMem1Pages ? kMem1Start + kMem1Size : kMem2Start + kMem2Size;
      const u32 take = std::min<u32>(left, region_end - guest);
      clean.push_back({guest, host, take});
      guest += take;
      host += take;
      left -= take;
    }
  }
  if (clean.empty())
    return fail("no host view of guest RAM");

  // Identical to what is registered: nothing to do.
  if (clean.size() == m_views.size() &&
      std::equal(clean.begin(), clean.end(), m_views.begin(),
                 [](const WriteView& a, const WriteView& b) { return a.guest == b.guest && a.host == b.host && a.size == b.size; }))
    return true;

  auto table = std::make_unique<Table>();
  table->by_host.reserve(clean.size());
  for (const WriteView& v : clean)
    table->by_host.push_back({reinterpret_cast<uintptr_t>(v.host), v.size, static_cast<u32>(PageIndexOf(v.guest))});
  std::sort(table->by_host.begin(), table->by_host.end(),
            [](const Table::View& a, const Table::View& b) { return a.host < b.host; });
  for (size_t i = 1; i < table->by_host.size(); ++i)
  {
    if (table->by_host[i - 1].host + table->by_host[i - 1].size > table->by_host[i].host)
      return fail("two host views overlap");
  }
  // CSR page -> host addresses.
  std::vector<u32> count(kPages, 0);
  for (const Table::View& v : table->by_host)
    for (u32 i = 0; i < v.size / kPage; ++i)
      ++count[v.first_page + i];
  table->csr_start.assign(kPages + 1, 0);
  for (u32 p = 0; p < kPages; ++p)
  {
    table->csr_start[p + 1] = table->csr_start[p] + count[p];
    if (count[p] != 0)
      ++table->pages_with_view;
  }
  table->csr_host.assign(table->csr_start[kPages], nullptr);
  std::vector<u32> fill(table->csr_start.begin(), table->csr_start.end() - 1);
  for (const Table::View& v : table->by_host)
    for (u32 i = 0; i < v.size / kPage; ++i)
      table->csr_host[fill[v.first_page + i]++] = reinterpret_cast<u8*>(v.host + static_cast<uintptr_t>(i) * kPage);

  if (m_active.load())
  {
    // The views changed under a running watch. The memory behind the old ones may be gone already (so it is not touched here); the
    // caller is expected to have called Stop() BEFORE the remap. Whatever was marked is meaningless now.
    DropAllMarks();
    m_active.store(false);
    ++m_restarts;
  }
  m_views = clean;
  m_tables.push_back(std::move(table));
  m_table.store(m_tables.back().get());
  return true;
}

void WriteTracker::DropAllMarks()
{
  for (u32 p = 0; p < kPages; ++p)
  {
    m_state[p].store(kOff);
    m_equal[p] = 0;
  }
  m_dirty_count.store(0);
}

std::string WriteTracker::Describe() const
{
  const Table* table = m_table.load();
  if (table == nullptr)
    return "host page protection, no views";
  char buf[160];
  std::snprintf(buf, sizeof(buf), "host page protection, %zu views of %u pages", table->by_host.size(), table->pages_with_view);
  return buf;
}

// Applies the protection to every page with mark[p] != 0 in every view, one call per run of consecutive pages inside a view.
void WriteTracker::ProtectPages(const std::vector<u8>& mark, bool read_only, u64* calls)
{
  const Table* table = m_table.load();
  if (table == nullptr)
    return;
  for (const Table::View& v : table->by_host)
  {
    const u32 pages = v.size / kPage;
    u32 i = 0;
    while (i < pages)
    {
      if (mark[v.first_page + i] == 0)
      {
        ++i;
        continue;
      }
      u32 j = i;
      while (j < pages && mark[v.first_page + j] != 0)
        ++j;
      if (!ProtectRange(reinterpret_cast<void*>(v.host + static_cast<uintptr_t>(i) * kPage), static_cast<size_t>(j - i) * kPage,
                        read_only))
        m_protect_errors.fetch_add(1);
      ++*calls;
      if (read_only)
        m_pages_protected += j - i;
      i = j;
    }
  }
}

bool WriteTracker::SetWatched(const std::vector<Range>& keep, std::string* why)
{
  auto fail = [&](const std::string& text) {
    if (why != nullptr)
      *why = text;
    return false;
  };
  const Table* table = m_table.load();
  if (table == nullptr)
    return fail("no host view of guest RAM registered");
  if (!m_handler_installed.load())
    return fail("no fault handler installed");
  if (m_pre == nullptr)
  {
    m_pre_bytes = static_cast<size_t>(kPages) * kPage;
#ifdef _WIN32
    m_pre = static_cast<u8*>(VirtualAlloc(nullptr, m_pre_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* mem = mmap(nullptr, m_pre_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    m_pre = mem == MAP_FAILED ? nullptr : static_cast<u8*>(mem);
#endif
    if (m_pre == nullptr)
      return fail("no memory for the page preimages (88 MB of address space)");
  }

  // Pages fully inside `keep` are not watched.
  std::vector<u8> covered(kPages, 0);
  {
    std::vector<Range> sorted = keep;
    std::sort(sorted.begin(), sorted.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
    std::vector<Range> merged;
    for (const Range& r : sorted)
    {
      if (r.end <= r.start)
        continue;
      if (!merged.empty() && r.start <= merged.back().end)
        merged.back().end = std::max(merged.back().end, r.end);
      else
        merged.push_back(r);
    }
    for (const Range& r : merged)
    {
      const u64 first = (static_cast<u64>(r.start) + kPage - 1) / kPage * kPage;  // first page start at or after r.start
      for (u64 a = first; a + kPage <= r.end; a += kPage)
      {
        const int index = PageIndexOf(static_cast<u32>(a));
        if (index >= 0)
          covered[index] = 1;
      }
    }
  }

  std::vector<u8> arm(kPages, 0);
  std::vector<u8> release(kPages, 0);
  u32 armed_now = 0;
  for (u32 p = 0; p < kPages; ++p)
  {
    const bool has_view = table->csr_start[p + 1] != table->csr_start[p];
    const bool wanted = has_view && covered[p] == 0;
    const u8 st = m_state[p].load();
    if (wanted)
    {
      if (st == kOff)
      {
        m_equal[p] = 0;
        m_state[p].store(kArmed);  // state first: a fault between the store and the protection is simply a normal fault later
        arm[p] = 1;
      }
      ++armed_now;
    }
    else if (st != kOff)
    {
      if (st == kDirty)
        m_dirty_count.fetch_sub(1);
      m_state[p].store(kOff);
      release[p] = 1;
      m_equal[p] = 0;
    }
  }
  const u64 t0 = NowUs();
  u64 calls = 0;
  ProtectPages(release, false, &calls);
  ProtectPages(arm, true, &calls);
  m_protect_calls += calls;
  m_protect_us += NowUs() - t0;
  (void)armed_now;
  m_active.store(true);
  return true;
}

void WriteTracker::Stop()
{
  m_active.store(false);
  if (m_table.load() != nullptr)
  {
    std::vector<u8> mark(kPages, 0);
    for (u32 p = 0; p < kPages; ++p)
    {
      if (m_state[p].exchange(kOff) != kOff)
        mark[p] = 1;
      m_equal[p] = 0;
    }
    u64 calls = 0;
    ProtectPages(mark, false, &calls);
    m_protect_calls += calls;
  }
  m_dirty_count.store(0);
}

void WriteTracker::Poll(std::vector<DirtyPage>* out)
{
  if (m_dirty_count.load() == 0 || m_pre == nullptr)
    return;
  const u64 t0 = NowUs();
  ++m_polls;
  for (u32 p = 0; p < kPages; ++p)
  {
    if (m_state[p].load() != kDirty)
      continue;
    DirtyPage d;
    d.addr = PageAddrOf(p);
    d.preimage = m_pre + static_cast<size_t>(p) * kPage;
    d.pc = m_pc[p];
    d.lr = m_lr[p];
    d.cpu_thread = m_cpu[p] != 0;
    d.equal_rearms = m_equal[p];
    out->push_back(d);
  }
  m_poll_us += NowUs() - t0;
}

std::vector<u32> WriteTracker::Rearm(const std::vector<u32>& pages)
{
  std::vector<u32> changed;
  const Table* table = m_table.load();
  if (table == nullptr || m_pre == nullptr)
    return changed;
  const u64 t0 = NowUs();
  for (u32 addr : pages)
  {
    const int index = PageIndexOf(addr & ~(kPage - 1));
    if (index < 0)
      continue;
    u8 expected = kDirty;
    if (!m_state[index].compare_exchange_strong(expected, kRearm))
      continue;
    m_dirty_count.fetch_sub(1);
    const u32 begin = table->csr_start[index];
    const u32 end = table->csr_start[index + 1];
    for (u32 i = begin; i < end; ++i)
    {
      if (!ProtectRange(table->csr_host[i], kPage, true))
        m_protect_errors.fetch_add(1);
      ++m_protect_calls;
    }
    ++m_pages_protected;
    const bool same = begin != end && std::memcmp(table->csr_host[begin], m_pre + static_cast<size_t>(index) * kPage, kPage) == 0;
    if (same)
    {
      if (m_equal[index] < 255)
        ++m_equal[index];
      ++m_rearms;
      m_state[index].store(kArmed);
    }
    else
    {
      // A write got in between the caller's comparison and the protection: back to dirty, the caller learns it.
      for (u32 i = begin; i < end; ++i)
      {
        if (!ProtectRange(table->csr_host[i], kPage, false))
          m_protect_errors.fetch_add(1);
        ++m_protect_calls;
      }
      ++m_rearm_changed;
      m_dirty_count.fetch_add(1);
      m_state[index].store(kDirty);
      changed.push_back(addr & ~(kPage - 1));
    }
  }
  m_protect_us += NowUs() - t0;
  return changed;
}

void WriteTracker::Release(const std::vector<u32>& pages)
{
  const Table* table = m_table.load();
  if (table == nullptr)
    return;
  for (u32 addr : pages)
  {
    const int index = PageIndexOf(addr & ~(kPage - 1));
    if (index < 0)
      continue;
    const u8 st = m_state[index].exchange(kOff);
    if (st == kOff)
      continue;
    if (st == kDirty)
      m_dirty_count.fetch_sub(1);
    m_equal[index] = 0;
    UnprotectPageEverywhere(*table, static_cast<u32>(index));
  }
}

bool WriteTracker::IsClean(u32 page_addr) const
{
  const int index = PageIndexOf(page_addr & ~(kPage - 1));
  return index >= 0 && m_state[index].load() == kArmed;
}

WriteWatchStats WriteTracker::Stats() const
{
  WriteWatchStats s;
  s.faults = m_faults.load();
  s.stale_faults = m_stale.load();
  s.protect_calls = m_protect_calls;
  s.pages_protected = m_pages_protected;
  s.protect_us = m_protect_us;
  s.rearms = m_rearms;
  s.rearm_changed = m_rearm_changed;
  s.polls = m_polls;
  s.poll_us = m_poll_us;
  s.restarts = m_restarts;
  s.protect_errors = m_protect_errors.load();
  const Table* table = m_table.load();
  s.views = table == nullptr ? 0 : static_cast<u32>(table->by_host.size());
  u32 watched = 0;
  for (u32 p = 0; p < kPages; ++p)
  {
    const u8 st = m_state[p].load();
    if (st != kOff)
      ++watched;
  }
  s.watched_pages = watched;
  return s;
}

bool WriteTracker::UnprotectPageEverywhere(const Table& table, u32 index)
{
  bool ok = true;
  for (u32 i = table.csr_start[index]; i < table.csr_start[index + 1]; ++i)
  {
    if (!ProtectRange(table.csr_host[i], kPage, false))
    {
      m_protect_errors.fetch_add(1);
      ok = false;
    }
  }
  return ok;
}

// ---- the handler ------------------------------------------------------------------------------------------------------
bool WriteTracker::HandleFault(uintptr_t host_address)
{
  g_in_handler.fetch_add(1);
  WriteTracker* tracker = g_current.load();
  const bool handled = tracker != nullptr && tracker->OnFault(host_address);
  g_in_handler.fetch_sub(1);
  return handled;
}

bool WriteTracker::OnFault(uintptr_t host_address)
{
  const Table* table = m_table.load();
  if (table == nullptr || m_pre == nullptr)
    return false;
  const Table::View* view = table->Find(host_address);
  if (view == nullptr)
    return false;  // not a guest RAM view: somebody else's fault
  const uintptr_t page_host = host_address & ~static_cast<uintptr_t>(kPage - 1);
  const u32 index = view->first_page + static_cast<u32>((page_host - view->host) / kPage);

  for (int spin = 0; spin < kMaxWaitYields; ++spin)
  {
    const u8 st = m_state[index].load();
    if (st == kArmed)
    {
      u8 expected = kArmed;
      if (!m_state[index].compare_exchange_strong(expected, kClaimed))
        continue;
      // This thread owns the page: the faulting view's page is readable (read-only protection), so copy the preimage from it.
      std::memcpy(m_pre + static_cast<size_t>(index) * kPage, reinterpret_cast<const void*>(page_host), kPage);
      u32 pc = 0;
      u32 lr = 0;
      bool cpu = false;
      if (ContextProbe probe = m_probe.load())
        cpu = probe(&pc, &lr);
      m_pc[index] = pc;
      m_lr[index] = lr;
      m_cpu[index] = cpu ? 1 : 0;
      const bool unprotected = UnprotectPageEverywhere(*table, index);
      u8 claimed = kClaimed;
      if (m_state[index].compare_exchange_strong(claimed, kDirty))
        m_dirty_count.fetch_add(1);
      m_faults.fetch_add(1);
      if (!unprotected)
      {
        // Could not make some view writable (out of VMAs?): make at least the faulting view's page writable so the thread can go on.
        ProtectRange(reinterpret_cast<void*>(page_host), kPage, false);
      }
      return true;
    }
    if (st == kClaimed || st == kRearm)
    {
      YieldThread();  // the owner is at work; its verdict decides
      continue;
    }
    break;  // Off or Dirty: a stale fault
  }
  // Stale (or the owner is stuck): the page is no longer ours to guard. Make this view's page writable and resume.
  m_stale.fetch_add(1);
  if (!ProtectRange(reinterpret_cast<void*>(page_host), kPage, false))
    m_protect_errors.fetch_add(1);
  return true;
}

bool WriteTracker::SelfTest(std::string* why)
{
  auto fail = [&](const std::string& text) {
    if (why != nullptr)
      *why = text;
    return false;
  };
  const Table* table = m_table.load();
  if (table == nullptr || !m_handler_installed.load())
    return fail("not set up");
  u32 page = kPages;
  for (u32 p = 0; p < kPages; ++p)
  {
    if (m_state[p].load() == kArmed)
    {
      page = p;
      break;
    }
  }
  if (page == kPages)
    return fail("no armed page to test");
  const u32 dirty_before = m_dirty_count.load();
  const u64 faults_before = m_faults.load();
  const u64 stale_before = m_stale.load();
  volatile u8* first = reinterpret_cast<volatile u8*>(table->csr_host[table->csr_start[page]]);
  const u8 value = *first;
  *first = value;  // the same byte again: the guest memory does not change, the page must still fault and go dirty
  if (m_state[page].load() != kDirty || m_faults.load() != faults_before + 1 || m_dirty_count.load() != dirty_before + 1)
    return fail("a write to an armed page did not fault");
  // Every other view of the page must be writable now: a write through it must not fault again.
  for (u32 i = table->csr_start[page] + 1; i < table->csr_start[page + 1]; ++i)
  {
    volatile u8* other = reinterpret_cast<volatile u8*>(table->csr_host[i]);
    *other = *other;
  }
  if (m_stale.load() != stale_before)
    return fail("a second view of the page stayed protected");
  if (std::memcmp(table->csr_host[table->csr_start[page]], m_pre + static_cast<size_t>(page) * kPage, kPage) != 0)
    return fail("the preimage differs from the page");
  const std::vector<u32> changed = Rearm({PageAddrOf(page)});
  if (!changed.empty())
    return fail("the test page changed");
  m_equal[page] = 0;
  m_rearms = m_rearms > 0 ? m_rearms - 1 : 0;
  return true;
}

// ---- stand-alone handlers ---------------------------------------------------------------------------------------------
#ifdef _WIN32
namespace
{
void* g_veh = nullptr;

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info)
{
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  // ExceptionInformation[0]: 0 = read, 1 = write, 8 = DEP; [1] = the address.
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 &&
      WriteTracker::HandleFault(static_cast<uintptr_t>(record->ExceptionInformation[1])))
    return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool InstallStandaloneHandler(std::string* why)
{
  if (g_veh != nullptr)
    return true;
  g_veh = AddVectoredExceptionHandler(1, VectoredHandler);  // 1 = call first
  if (g_veh == nullptr && why != nullptr)
    *why = "AddVectoredExceptionHandler failed";
  return g_veh != nullptr;
}

void UninstallStandaloneHandler()
{
  if (g_veh != nullptr)
    RemoveVectoredExceptionHandler(g_veh);
  g_veh = nullptr;
}
#else
namespace
{
struct sigaction g_previous;
bool g_installed = false;

void Sigsegv(int sig, siginfo_t* info, void* context)
{
  if (info != nullptr && WriteTracker::HandleFault(reinterpret_cast<uintptr_t>(info->si_addr)))
    return;
  // Not ours: hand it to the handler that was there before (Dolphin's, a debugger's, the default).
  if (g_previous.sa_flags & SA_SIGINFO)
  {
    if (g_previous.sa_sigaction != nullptr)
    {
      g_previous.sa_sigaction(sig, info, context);
      return;
    }
  }
  else if (g_previous.sa_handler != SIG_DFL && g_previous.sa_handler != SIG_IGN && g_previous.sa_handler != nullptr)
  {
    g_previous.sa_handler(sig);
    return;
  }
  // Default action: restore it and return, the faulting instruction runs again and the process dies the usual way.
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
}
}  // namespace

bool InstallStandaloneHandler(std::string* why)
{
  if (g_installed)
    return true;
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_sigaction = Sigsegv;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &g_previous) != 0)
  {
    if (why != nullptr)
      *why = "sigaction(SIGSEGV) failed";
    return false;
  }
  g_installed = true;
  return true;
}

void UninstallStandaloneHandler()
{
  if (!g_installed)
    return;
  sigaction(SIGSEGV, &g_previous, nullptr);
  g_installed = false;
}
#endif
}  // namespace Rev3
