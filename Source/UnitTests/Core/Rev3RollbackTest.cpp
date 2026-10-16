// Unit tests for Core/Slippi/Rev3Rollback: the wire codec, the whitelist parser, the SyncTest state machine and its network mode (M3), driven by a fake
// guest memory and a tiny deterministic "game" (no Dolphin core, no game data). Run: build target `tests`, then `tests --gtest_filter=Rev3*`.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Hash.h"
#include "Core/Slippi/Rev3Rollback.h"
#include "Core/Slippi/Rev3Session.h"

namespace
{
using namespace Rev3;

// ---- fakes -----------------------------------------------------------------------------------------------------------------

// MEM1 (24 MB, zero filled) and, when asked for, MEM2 (64 MB); reads and writes outside them fail like the real accessor's range check.
class FakeMemory final : public GuestMemory
{
public:
  explicit FakeMemory(bool with_mem2 = false) : m_mem1(0x1800000, 0)
  {
    if (with_mem2)
      m_mem2.assign(0x4000000, 0);
  }

  bool Read(u32 addr, void* dst, size_t n) const override
  {
    const u8* p = Locate(addr, n);
    if (p == nullptr)
      return false;
    std::memcpy(dst, p, n);
    return true;
  }
  bool Write(u32 addr, const void* src, size_t n) override
  {
    u8* p = Locate(addr, n);
    if (p == nullptr)
      return false;
    if (drop_writes_hi > drop_writes_lo && n >= 16 && addr < drop_writes_hi && addr + n > drop_writes_lo)
      return true;  // a restore bug: a bulk write (the game's own word writes are shorter) "succeeds" and changes nothing
    std::memcpy(p, src, n);
    return true;
  }
  u32 drop_writes_lo = 0;  // writes of 16+ bytes that touch [lo, hi) are silently dropped (0, 0 = off)
  u32 drop_writes_hi = 0;

  // Direct pointer for the page tracker; switch off to exercise its Read() fallback.
  const u8* Peek(u32 addr, size_t n) const override
  {
    if (!peek_enabled)
      return nullptr;
    return Locate(addr, n);
  }
  bool peek_enabled = true;

  void InvalidateCode(u32 addr, size_t n) override { invalidated.emplace_back(addr, n); }
  std::vector<std::pair<u32, size_t>> invalidated;  // every InvalidateCode call, in order

  bool ReadTimeBase(u64* out) const override
  {
    *out = time_base;
    return true;
  }
  bool WriteTimeBase(u64 value) override
  {
    time_base = value;
    ++time_base_writes;
    return true;
  }
  u64 time_base = 5000000;  // the emulated time base; FakeGame advances it by TB_PER_FRAME every frame
  int time_base_writes = 0;

  u32 U32(u32 addr) const
  {
    u8 b[4] = {};
    Read(addr, b, 4);
    return (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | b[3];
  }
  void SetU32(u32 addr, u32 v)
  {
    const u8 b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)};
    Write(addr, b, 4);
  }
  u32 U16(u32 addr) const
  {
    u8 b[2] = {};
    Read(addr, b, 2);
    return (u32(b[0]) << 8) | b[1];
  }
  void SetU16(u32 addr, u32 v)
  {
    const u8 b[2] = {u8(v >> 8), u8(v)};
    Write(addr, b, 2);
  }

private:
  u8* Locate(u32 addr, size_t n)
  {
    if (addr >= 0x80000000 && u64(addr) + n <= 0x81800000)
      return &m_mem1[addr - 0x80000000];
    if (!m_mem2.empty() && addr >= 0x90000000 && u64(addr) + n <= 0x94000000)
      return &m_mem2[addr - 0x90000000];
    return nullptr;
  }
  const u8* Locate(u32 addr, size_t n) const { return const_cast<FakeMemory*>(this)->Locate(addr, n); }
  std::vector<u8> m_mem1;
  std::vector<u8> m_mem2;
};

class FakeSink final : public Sink
{
public:
  void Line(const std::string& text) override { lines.push_back(text); }
  void Osd(const std::string& text) override { osd.push_back(text); }

  size_t Count(const std::string& needle) const
  {
    size_t n = 0;
    for (const std::string& l : lines)
      if (l.find(needle) != std::string::npos)
        ++n;
    return n;
  }

  bool Contains(const std::string& needle) const
  {
    for (const std::string& l : lines)
    {
      if (l.find(needle) != std::string::npos)
        return true;
    }
    return false;
  }

  std::vector<std::string> lines;
  std::vector<std::string> osd;
};

// Hardware that uses memory on its own: Query returns whatever `list` holds right now.
class FakeHardware final : public HardwareView
{
public:
  void Query(std::vector<Exclusion>* out) override
  {
    ++queries;
    out->insert(out->end(), list.begin(), list.end());
  }
  bool DiscReadInFlight() override { return disc_busy; }
  std::vector<Exclusion> list;
  int queries = 0;
  bool disc_busy = false;
};

// The emulated audio hardware: a counter the "game" advances every frame, a few variable-length bytes (like the HLE mail queue),
// two pending CoreTiming events and the PI DSP / AI cause bits. Records how it was driven.
class FakeAudio final : public AudioHardware
{
public:
  explicit FakeAudio(FakeMemory* memory) : m_memory(memory) {}

  const char* DspKind() const override { return "HLE"; }
  std::vector<std::string> EventTypeNames() const override { return {"AudioDMACallback", "DSPint"}; }

  bool SaveState(std::vector<u8>* out) override
  {
    ++saves;
    if (fail_save)
      return false;
    out->assign(5 + extra.size() + extra_padding, 0);
    for (int i = 0; i < 4; ++i)
      (*out)[i] = u8(counter >> (8 * i));
    (*out)[4] = u8(extra.size());
    std::copy(extra.begin(), extra.end(), out->begin() + 5);
    return true;
  }
  bool LoadState(const std::vector<u8>& in) override
  {
    ++loads;
    // Memory (and its JIT invalidation) must already be back when the audio hardware is loaded.
    invalidated_at_load.push_back(m_memory->invalidated.size());
    if (fail_load)
      return false;
    counter = 0;
    for (int i = 0; i < 4; ++i)
      counter |= u32(in[i]) << (8 * i);
    extra.assign(in.begin() + 5, in.begin() + 5 + in[4]);
    return true;
  }
  void PendingEvents(std::vector<AudioEvent>* out) override { *out = events; }
  void ReplaceEvents(const std::vector<AudioEvent>& e) override
  {
    ++replaces;
    events = e;
  }
  u32 InterruptBits() override { return irq; }
  void SetInterruptBits(u32 bits) override
  {
    ++irq_sets;
    irq = bits;
  }

  // What the hardware does by itself while a frame runs.
  void Advance()
  {
    ++counter;
    extra.assign(vary_size ? counter % 3 : 0, 7);
    events = {{"AudioDMACallback", 1000 - static_cast<s64>(counter % 1000), 0}, {"DSPint", 50 + static_cast<s64>(counter % 7), 0x80}};
    irq = (counter & 1) != 0 ? 0x40 : 0x20;
  }
  std::string Key() const
  {
    std::string k = std::to_string(counter) + "/" + std::to_string(extra.size()) + "/" + std::to_string(irq);
    for (const AudioEvent& e : events)
      k += "/" + e.type + ":" + std::to_string(e.cycles) + ":" + std::to_string(e.userdata);
    return k;
  }

  u32 counter = 0;
  std::vector<u8> extra;
  std::vector<AudioEvent> events;
  u32 irq = 0;
  bool vary_size = true;
  size_t extra_padding = 0;  // makes the state bigger than the sanity limit in the oversize test
  bool fail_save = false;
  bool fail_load = false;
  int saves = 0;
  int loads = 0;
  int replaces = 0;
  int irq_sets = 0;
  std::vector<size_t> invalidated_at_load;  // FakeMemory::invalidated.size() at each LoadState

private:
  FakeMemory* m_memory;
};

// The layout of the fake game's memory.
constexpr u32 TEAM[2] = {0x80400000, 0x80401000};
constexpr u32 NODE[2] = {0x80410000, 0x80411000};
constexpr u32 FIGHTER[2] = {0x80420000, 0x80430000};
constexpr u32 STATE_A = 0x80500000;  // whitelisted: 0x100 bytes
constexpr u32 STATE_B = 0x80600000;  // whitelisted: 0x40 bytes
constexpr u32 HIDDEN = 0x80700000;   // NOT whitelisted
constexpr u32 SP = 0x80800000;       // the hook's stack pointer
constexpr u32 WINDOW = 0x400;

constexpr u64 TB_PER_FRAME = 1000;

constexpr u32 HW_WORD = STATE_A + 0x44;  // a word "hardware" writes by itself every frame (inside the excluded 0x40..0x80)
constexpr u32 FIELD_X = 0x84;
constexpr u32 FIELD_HEALTH = 0x28;
constexpr u32 FIELD_ANGLEA0 = 0xA0;
constexpr u32 FIELD_Y = 0x88;
constexpr u32 FIELD_COMBO_COUNT = 0x9AC;
constexpr u32 LATE_PAGE = 0x80A00000;  // a page nothing seeds; only SetLatePage makes it change
constexpr u32 LATE_WALK = 0x80C00000;  // SetWalker's first page

// A deterministic game: every step adds the pads to counters in two whitelisted ranges and mirrors them into the fighters. With
// `leak` set, a hidden counter outside the whitelist also feeds the state (what an incomplete whitelist looks like).
class FakeGame
{
public:
  explicit FakeGame(bool leak, u32 late_leak_from = 0) : m_leak(leak), m_late_from(late_leak_from)
  {
    for (int p = 0; p < 2; ++p)
    {
      m_mem.SetU32(0x8038FE50 + 4 * p, TEAM[p]);
      m_mem.SetU32(TEAM[p] + 0x0C, NODE[p]);
      m_mem.SetU32(NODE[p], FIGHTER[p]);
    }
  }

  std::vector<Range> Ranges() const { return {{STATE_A, STATE_A + 0x100}, {STATE_B, STATE_B + 0x40}}; }

  static std::array<PadBytes, 2> PadsFor(u32 frame)
  {
    std::array<PadBytes, 2> pads{};
    for (size_t i = 0; i < 12; ++i)
    {
      pads[0][i] = u8(frame * 3 + i);
      pads[1][i] = u8(frame * 5 + 2 * i);
    }
    return pads;
  }

  // One hook call plus the game frame that follows it. Returns the reply.
  Reply Frame(SyncTest& test, u32 sp = SP)
  {
    // The hook's stack frame holds its frame counter; a RESTORED reply must bring the old value back.
    m_mem.SetU32(sp, m_counter);
    m_mem.SetU32(sp + 4, 0xC0FFEE00 + m_counter);
    // The time base the hook call of this frame sees: a frame visited again (after a restore) must see the same value again.
    const auto seen = m_tb_seen.emplace(m_counter, m_mem.time_base);
    if (!seen.second && seen.first->second != m_mem.time_base)
      m_tb_consistent = false;
    // The audio hardware the capture of this frame sees; a RESTORED reply must bring exactly this state back.
    if (m_audio != nullptr)
      m_audio_seen.emplace(m_counter, m_audio->Key());
    Command c;
    c.protocol = m_protocol;
    c.frame = m_counter;
    c.pads = PadsFor(m_counter);
    if (m_z.has_value())
    {
      c.pads[0][0] = 0;
      c.pads[0][1] = *m_z ? 0x10 : 0x00;
    }
    c.sp = sp;
    if (m_kit != nullptr)
      m_kit->OnCommand(&c, test.Resimulating());  // the determinism kit sees the hook call first, as Rev3::HandleCommand does
    const Reply r = test.OnCommand(c);
    if (r.flags & REPLY_FLAG_RESTORED)
    {
      ++m_restores_seen;
      m_hw_word_intact = m_hw_word_intact && m_mem.U32(HW_WORD) == m_hw_ticks;
      EXPECT_EQ(m_mem.U32(sp), r.frame) << "stack frame not restored";
      EXPECT_EQ(m_mem.U32(sp + 4), 0xC0FFEE00 + r.frame);
      m_counter = r.frame;
      if (m_audio != nullptr)
      {
        const auto it = m_audio_seen.find(r.frame);
        if (it == m_audio_seen.end() || it->second != m_audio->Key())
          ++m_audio_wrong;
        else
          ++m_audio_right;
      }
    }
    Step(r.pads);
    if (m_audio != nullptr)
      m_audio->Advance();  // the audio hardware runs on during the frame, rewound or not
    if (m_hw_writes)
      m_mem.SetU32(HW_WORD, ++m_hw_ticks);  // the hardware's own write, after the frame
    m_mem.time_base += TB_PER_FRAME;
    ++m_counter;
    return r;
  }

  void AttachAudio(FakeAudio* audio) { m_audio = audio; }
  void AttachKit(DeterminismKit* kit) { m_kit = kit; }
  int AudioRight() const { return m_audio_right; }  // RESTORED replies after which the audio hardware equalled its state at capture
  int AudioWrong() const { return m_audio_wrong; }

  bool TimeBaseConsistent() const { return m_tb_consistent; }  // every revisited frame saw the time base it saw the first time

  void EnableHardwareWord() { m_hw_writes = true; }  // "hardware" starts writing HW_WORD every frame
  void SetZ(bool down) { m_z = down; }  // from now on port 0's buttons carry Z (0x0010) or nothing
  bool HwWordIntact() const { return m_hw_word_intact; }  // every RESTORED reply left the hardware word alone
  int RestoresSeen() const { return m_restores_seen; }

  void SetProtocol(u8 protocol) { m_protocol = protocol; }  // the hook protocol its commands carry (2 or 3)
  void SetQuiet() { m_quiet = true; }  // Step changes nothing: only the hook's stack frame (written by Frame) changes
  // From this frame on a counter in the page LATE_PAGE (outside every seed) counts up and feeds p1's combo_count.
  void SetLatePage(u32 from_frame) { m_late_page_from = from_frame; }
  // From this frame on every frame writes one constant word into a new page: LATE_WALK + (frame - from) * stride.
  void SetWalker(u32 from_frame, u32 stride) { m_walk_from = from_frame, m_walk_stride = stride; }

  // Called at the start of every game step with the frame being simulated (also for replayed frames): extra game state, deterministic
  // in the frame number and the memory.
  void SetStepHook(std::function<void(FakeMemory&, u32)> hook) { m_step_hook = std::move(hook); }

  void Step(const std::array<PadBytes, 2>& pads)
  {
    if (m_step_hook)
      m_step_hook(m_mem, m_counter);
    if (m_quiet)
      return;
    if (m_late_page_from != 0 && m_counter >= m_late_page_from)
    {
      const u32 v = m_mem.U32(LATE_PAGE);
      m_mem.SetU32(LATE_PAGE, v + 1);
      m_mem.SetU32(FIGHTER[0] + FIELD_COMBO_COUNT, v);
    }
    if (m_walk_from != 0 && m_counter >= m_walk_from)
      m_mem.SetU32(LATE_WALK + (m_counter - m_walk_from) * m_walk_stride, 0x1234);
    const u32 hidden = m_mem.U32(HIDDEN);
    m_mem.SetU32(HIDDEN, hidden + 1);
    const u32 leak = m_leak ? hidden : 0;
    const u32 a = m_mem.U32(STATE_A) + pads[0][0] + pads[1][0] + leak;
    const u32 b = m_mem.U32(STATE_B + 4) + pads[0][3] * 2 + pads[1][5];
    m_mem.SetU32(STATE_A, a);
    m_mem.SetU32(STATE_A + 0x80, a ^ 0x5A5A5A5A);
    m_mem.SetU32(STATE_B + 4, b);
    m_mem.SetU32(FIGHTER[0] + FIELD_X, a);
    if (m_late_from != 0 && m_counter >= m_late_from)
    {
      m_mem.SetU32(FIGHTER[0] + FIELD_ANGLEA0, hidden);  // a second, later leak into other fighter fields:
      m_mem.SetU32(FIGHTER[1] + FIELD_Y, hidden);        // p1 anglea0 (in the test's snapshot) and p2 y (not in it)
    }
    m_mem.SetU32(FIGHTER[1] + FIELD_HEALTH, b);
  }

  FakeMemory& Mem() { return m_mem; }
  u32 Counter() const { return m_counter; }

private:
  FakeMemory m_mem;
  bool m_leak;
  u32 m_late_from;
  bool m_quiet = false;
  u8 m_protocol = PROTOCOL_VERSION;
  std::function<void(FakeMemory&, u32)> m_step_hook;
  u32 m_late_page_from = 0;
  u32 m_walk_from = 0;
  u32 m_walk_stride = 0;
  FakeAudio* m_audio = nullptr;
  DeterminismKit* m_kit = nullptr;
  std::map<u32, std::string> m_audio_seen;
  int m_audio_right = 0;
  int m_audio_wrong = 0;
  std::map<u32, u64> m_tb_seen;
  bool m_tb_consistent = true;
  u32 m_counter = 1000;
  std::optional<bool> m_z;
  bool m_hw_writes = false;
  u32 m_hw_ticks = 0;
  bool m_hw_word_intact = true;
  int m_restores_seen = 0;
};

// A synthetic nw4r::ef image, laid out the way FindEfPool / CheckEfLists read it (all addresses and offsets from Rev3Rollback.h): the
// EffectSystem singleton with the manager pointer and the activity groups, the manager (heap bounds, pool counts, pool objects), one
// object array per pool allocated from the heap, intrusive lists with the nw4r::ut::List semantics (Remove clears the node's links),
// and the handle table behind the heap. It is built in a FakeMemory, so tests can break it byte by byte.
class FakeEf
{
public:
  static constexpr u32 kEffects = 4;
  static constexpr u32 kEmitters = 4;
  static constexpr u32 kPms = 4;
  static constexpr u32 kParticles = 40;
  static constexpr u32 kStride[4] = {0xA0, 0x154, 0xBC, 0xE4};
  static constexpr u32 kCounts[4] = {kEffects, kEmitters, kPms, kParticles};

  explicit FakeEf(FakeMemory& mem, u32 mm = 0x80B00000, u32 heap_size = 0x40000, u32 groups = 1) : m_mem(mem), m_mm(mm)
  {
    m_heap_start = mm + 0x40;
    m_heap_end = m_heap_start + heap_size;
    m_next = m_heap_start;
    mem.SetU32(EF_SYSTEM_ADDR, mm);
    mem.SetU32(EF_BLOCK_GLOBAL, mm);
    mem.SetU32(mm + EF_MM_HEAP_START, m_heap_start);
    mem.SetU32(mm + EF_MM_HEAP_END, m_heap_end);
    mem.SetU32(EF_HANDLE_TABLE_GLOBAL, m_heap_end);  // the handle table follows the heap, inside the allocation's tail
    mem.SetU32(EF_SYSTEM_ADDR + 0xC014, groups);
    m_activity = Alloc(groups * 0x1C);
    mem.SetU32(EF_SYSTEM_ADDR + 0xC018, m_activity);
    for (u32 g = 0; g < groups; ++g)
    {
      ListInit(m_activity + g * 0x1C, 0x14);
      ListInit(m_activity + g * 0x1C + 0xC, 0x14);
    }
    for (u32 k = 0; k < 4; ++k)
      m_pool[k] = Alloc(0x24);  // the four pool objects first: they share the heap's first page with the manager's other bookkeeping
    for (u32 k = 0; k < 4; ++k)
    {
      m_array[k] = Alloc(kStride[k] * kCounts[k]);
      mem.SetU32(mm + EF_MM_COUNTS[k], kCounts[k]);
      mem.SetU32(mm + EF_MM_POOLS[k], m_pool[k]);
      mem.SetU32(m_pool[k], m_array[k]);
      ListInit(m_pool[k] + 4, 0);     // free
      ListInit(m_pool[k] + 0x10, 0);  // in use
      mem.SetU32(m_pool[k] + 0x1C, kStride[k]);
      for (u32 i = 0; i < kCounts[k]; ++i)
        Append(m_pool[k] + 4, m_array[k] + i * kStride[k]);
    }
  }

  u32 Alloc(u32 size)
  {
    const u32 at = m_next;
    m_next += (size + 0x1F) & ~0x1Fu;
    EXPECT_LE(m_next, m_heap_end);
    return at;
  }
  void ListInit(u32 list, u32 offset)
  {
    m_mem.SetU32(list, 0);
    m_mem.SetU32(list + 4, 0);
    m_mem.SetU16(list + 8, 0);
    m_mem.SetU16(list + 0xA, offset);
  }
  u32 Head(u32 list) const { return m_mem.U32(list); }
  u32 Tail(u32 list) const { return m_mem.U32(list + 4); }
  u32 Count(u32 list) const { return m_mem.U16(list + 8); }
  u32 Next(u32 list, u32 node) const { return m_mem.U32(node + m_mem.U16(list + 0xA) + 4); }
  void Append(u32 list, u32 node)
  {
    const u32 off = m_mem.U16(list + 0xA);
    const u32 tail = Tail(list);
    m_mem.SetU32(node + off, tail);
    m_mem.SetU32(node + off + 4, 0);
    if (tail != 0)
      m_mem.SetU32(tail + off + 4, node);
    else
      m_mem.SetU32(list, node);
    m_mem.SetU32(list + 4, node);
    m_mem.SetU16(list + 8, Count(list) + 1);
  }
  void Remove(u32 list, u32 node)
  {
    const u32 off = m_mem.U16(list + 0xA);
    const u32 prev = m_mem.U32(node + off);
    const u32 next = m_mem.U32(node + off + 4);
    if (prev != 0)
      m_mem.SetU32(prev + off + 4, next);
    else
      m_mem.SetU32(list, next);
    if (next != 0)
      m_mem.SetU32(next + off, prev);
    else
      m_mem.SetU32(list + 4, prev);
    m_mem.SetU32(node + off, 0);
    m_mem.SetU32(node + off + 4, 0);
    m_mem.SetU16(list + 8, Count(list) - 1);
  }
  // Takes the first free object of pool k (0 effect, 1 emitter, 2 pm, 3 particle), moves it to the in-use list and returns it.
  u32 AllocObject(u32 k)
  {
    const u32 node = Head(m_pool[k] + 4);
    if (node == 0)
      return 0;
    Remove(m_pool[k] + 4, node);
    Append(m_pool[k] + 0x10, node);
    return node;
  }
  // The usual chain: effect in group 0, emitter in the effect, pm in the emitter, `particles` particles in the pm. Returns the pm.
  u32 BuildChain(u32 particles)
  {
    m_effect = AllocObject(0);
    Append(m_activity, m_effect);
    ListInit(m_effect + 0x24, 0x14);
    ListInit(m_effect + 0x30, 0x14);
    m_emitter = AllocObject(1);
    Append(m_effect + 0x24, m_emitter);
    ListInit(m_emitter + 0xC0, 0x14);
    ListInit(m_emitter + 0xCC, 0x14);
    m_pm = AllocObject(2);
    Append(m_emitter + 0xC0, m_pm);
    ListInit(m_pm + 0x38, 0x14);
    ListInit(m_pm + 0x44, 0x14);
    for (u32 i = 0; i < particles; ++i)
      Append(m_pm + 0x38, AllocObject(3));
    return m_pm;
  }
  // The game allocates one more particle into the pm's list: free list -> in-use list and the pm's list (what CreateParticle does).
  u32 CreateParticle()
  {
    const u32 node = AllocObject(3);
    if (node != 0)
      Append(m_pm + 0x38, node);
    return node;
  }

  u32 mm() const { return m_mm; }
  u32 heap_start() const { return m_heap_start; }
  u32 heap_end() const { return m_heap_end; }
  u32 activity() const { return m_activity; }
  u32 pool(u32 k) const { return m_pool[k]; }
  u32 array(u32 k) const { return m_array[k]; }
  u32 effect() const { return m_effect; }
  u32 emitter() const { return m_emitter; }
  u32 pm() const { return m_pm; }
  u32 particles_list() const { return m_pm + 0x38; }
  u32 particle(u32 i) const { return m_array[3] + i * kStride[3]; }

private:
  FakeMemory& m_mem;
  u32 m_mm;
  u32 m_heap_start = 0;
  u32 m_heap_end = 0;
  u32 m_next = 0;
  u32 m_activity = 0;
  std::array<u32, 4> m_array{};
  std::array<u32, 4> m_pool{};
  u32 m_effect = 0;
  u32 m_emitter = 0;
  u32 m_pm = 0;
};

SyncTestOptions Options(int k, int every = 1)
{
  SyncTestOptions o;
  o.k = k;
  o.every = every;
  o.stack_window = WINDOW;
  o.whitelist_path = "test.json";
  o.arm_commands = 60;  // the product default is 600; most tests do not need to wait that long
  o.adaptive = false;   // the classic tests use a fixed whitelist; the adaptive snapshot set has its own tests at the end
  return o;
}

// The adaptive snapshot set: no whitelist needed, the set is learned. `slices` pages are hashed per call in 1/slices parts.
SyncTestOptions AdaptiveOptions(int k, u32 slices = 16, u32 learn = 20)
{
  SyncTestOptions o = Options(k);
  o.adaptive = true;
  o.scan_slices = slices;
  o.learn_cycles = learn;
  return o;
}

bool Overlaps(const Range& r, u32 lo, u32 hi)
{
  return r.start < hi && lo < r.end;
}

bool SetContains(const std::vector<Range>& set, u32 addr, size_t n = 4)
{
  for (const Range& r : set)
  {
    if (addr >= r.start && u64(addr) + n <= r.end)
      return true;
  }
  return false;
}

std::function<u64()> FakeClock()
{
  auto now = std::make_shared<u64>(0);
  return [now]() { return *now += 7; };
}
}  // namespace

// ---- codec ------------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, CommandCodec)
{
  std::array<u8, CMD_SIZE> raw{};
  raw[0] = CMD_ID;
  raw[1] = PROTOCOL_VERSION;
  raw[4] = 0x52, raw[5] = 0x42, raw[6] = 0x4B, raw[7] = 0x31;
  raw[8] = 0x00, raw[9] = 0x00, raw[10] = 0x01, raw[11] = 0x02;  // frame 258
  for (u8 i = 0; i < 12; ++i)
  {
    raw[0x0C + i] = i + 1;
    raw[0x18 + i] = 0x80 + i;
  }
  raw[0x24] = 0xFF, raw[0x25] = 0xFF, raw[0x26] = 0xFF, raw[0x27] = 0xFF;  // -1
  raw[0x28] = 0x80, raw[0x29] = 0x41, raw[0x2A] = 0x23, raw[0x2B] = 0x40;  // sp

  EXPECT_TRUE(IsCommand(raw.data(), raw.size()));
  Command c;
  ASSERT_TRUE(ParseCommand(raw.data(), raw.size(), &c));
  EXPECT_EQ(c.frame, 258u);
  EXPECT_EQ(c.pads[0][0], 1);
  EXPECT_EQ(c.pads[0][11], 12);
  EXPECT_EQ(c.pads[1][0], 0x80);
  EXPECT_EQ(c.pad_return, -1);
  EXPECT_EQ(c.sp, 0x80412340u);

  // Not ours: wrong id, wrong magic, too short, other version.
  auto bad = raw;
  bad[0] = 0xB1;
  EXPECT_FALSE(IsCommand(bad.data(), bad.size()));
  bad = raw;
  bad[7] = 0x32;
  EXPECT_FALSE(IsCommand(bad.data(), bad.size()));
  EXPECT_FALSE(IsCommand(raw.data(), 63));
  bad = raw;
  bad[1] = 1;
  EXPECT_TRUE(IsCommand(bad.data(), bad.size()));
  EXPECT_FALSE(ParseCommand(bad.data(), bad.size(), &c));

  Reply r;
  r.flags = REPLY_FLAG_RESTORED;
  r.frame = 0x01020304;
  r.pads = c.pads;
  std::array<u8, REPLY_SIZE> out;
  out.fill(0xEE);
  EncodeReply(r, out.data());
  EXPECT_EQ(out[0], 'R');
  EXPECT_EQ(out[1], 'B');
  EXPECT_EQ(out[2], 'R');
  EXPECT_EQ(out[3], '1');
  EXPECT_EQ(out[7], 1);
  EXPECT_EQ(out[8], 1);
  EXPECT_EQ(out[11], 4);
  EXPECT_EQ(out[0x0C], 0);
  EXPECT_EQ(out[0x10], 1);
  EXPECT_EQ(out[0x1C], 0x80);
  EXPECT_EQ(out[0x2B], 0x0);
  EXPECT_EQ(out[63], 0);
}

// ---- whitelist --------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, WhitelistParsing)
{
  std::vector<Range> ranges;
  std::string error;
  ASSERT_TRUE(ParseWhitelist(R"({"ranges":[{"start":"0x80005520","end":"0x80005940"},{"start":2151182400,"end":"0x804DEC00"}]})",
                             &ranges, &error))
      << error;
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].start, 0x80005520u);
  EXPECT_EQ(ranges[0].end, 0x80005940u);
  EXPECT_EQ(ranges[1].start, 2151182400u);
  EXPECT_EQ(ranges[1].end, 0x804DEC00u);

  // Extra keys are ignored; MEM2 is fine.
  ASSERT_TRUE(ParseWhitelist(R"({"x":1,"ranges":[{"start":"0x90000000","end":"0x90001000","note":"heap"}]})", &ranges, &error));
  EXPECT_EQ(ranges.size(), 1u);

  const char* const bad[] = {
      "not json",
      R"({"ranges":[]})",
      R"({"nothing":1})",
      R"({"ranges":[{"start":"0x80000100","end":"0x80000100"}]})",    // empty
      R"({"ranges":[{"start":"0x80000200","end":"0x80000100"}]})",    // backwards
      R"({"ranges":[{"start":"0x80000000"}]})",                       // no end
      R"({"ranges":[{"start":"zz","end":"0x80000100"}]})",            // not a number
      R"({"ranges":[{"start":"0x00001000","end":"0x00002000"}]})",    // outside MEM1 / MEM2
      R"({"ranges":[{"start":"0x817FFF00","end":"0x81800100"}]})",    // runs past MEM1
      R"({"ranges":[{"start":-5,"end":10}]})",
  };
  for (const char* text : bad)
  {
    ranges.clear();
    error.clear();
    EXPECT_FALSE(ParseWhitelist(text, &ranges, &error)) << text;
    EXPECT_FALSE(error.empty()) << text;
    EXPECT_TRUE(ranges.empty()) << text;
  }
}

TEST(Rev3Rollback, HookRegionIsCutOut)
{
  std::vector<Range> r = {{0x80002000, 0x80003000}, {0x80002500, 0x80002600}, {0x80002A00, 0x80002B00}, {0x80001000, 0x80002400},
                          {0x80005000, 0x80006000}};
  EXPECT_EQ(SubtractRange(&r, HOOK_REGION_START, HOOK_REGION_END), 0x600u + 0x100u);
  const std::vector<Range> want = {{0x80002000, 0x80002400}, {0x80002A00, 0x80003000}, {0x80002A00, 0x80002B00},
                                   {0x80001000, 0x80002400}, {0x80005000, 0x80006000}};
  ASSERT_EQ(r.size(), want.size());
  for (size_t i = 0; i < r.size(); ++i)
  {
    EXPECT_EQ(r[i].start, want[i].start) << i;
    EXPECT_EQ(r[i].end, want[i].end) << i;
  }
}

TEST(Rev3Rollback, FieldTableMatchesChecksumModule)
{
  // rev3-mod rbchecksum.py: 61 fields of 4 bytes, first "action" at 0x140, last "str_pending" at 0x13EC, spanning 0x24..0x1648.
  EXPECT_EQ(FIELDS.size(), 61u);
  EXPECT_EQ(FIELD_BYTES, 244u);
  EXPECT_STREQ(FIELDS.front().name, "action");
  EXPECT_EQ(FIELDS.front().offset, 0x140);
  EXPECT_STREQ(FIELDS.back().name, "str_pending");
  EXPECT_EQ(FIELDS.back().offset, 0x13EC);
  u16 lo = 0xFFFF, hi = 0;
  for (const FieldDef& f : FIELDS)
  {
    lo = std::min(lo, f.offset);
    hi = std::max<u16>(hi, f.offset);
  }
  EXPECT_EQ(lo, 0x24);
  EXPECT_EQ(hi, 0x1644);
}

// ---- SyncTest ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, NoFightersJustEchoes)
{
  FakeGame game(false);
  game.Mem().SetU32(0x8038FE50, 0);  // team 0 pointer gone: not a fight
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.frame, frame);
    EXPECT_EQ(r.pads, FakeGame::PadsFor(frame));
  }
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_FALSE(test.Active());
}

TEST(Rev3Rollback, CleanRunHasNoMismatches)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());

  // The first 59 commands are not enough to start; the 60th starts the history (first_n = frame 1059).
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());

  const u32 first_n = game.Counter() + 0;  // the command of the 60th call
  EXPECT_EQ(first_n, 1059u);
  // n = first_n, first_n + 1: building history. n = first_n + 2: restore to first_n, resimulate first_n + 1 and first_n + 2.
  std::vector<std::pair<u32, u32>> seen;  // (flags, frame) per hook call
  for (int i = 0; i < 9; ++i)
  {
    const Reply r = game.Frame(test);
    seen.emplace_back(r.flags, r.frame);
  }
  const std::vector<std::pair<u32, u32>> expected = {
      {0, 1059}, {0, 1060}, {1, 1059},  // rollback at 1061 -> back to 1059
      {0, 1060}, {0, 1061},             // resimulation (1061 is the real run: no second rollback)
      {1, 1060},                        // 1062 -> back to 1060
      {0, 1061}, {0, 1062},
      {1, 1061},
  };
  EXPECT_EQ(seen, expected);

  for (int i = 0; i < 1000; ++i)
    game.Frame(test);
  while (test.Resimulating())  // finish the replay in progress so every rollback has its K comparisons
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  const SyncTestStats& s = test.Stats();
  EXPECT_GT(s.rollbacks, 50u);
  EXPECT_EQ(s.frames_compared, s.rollbacks * 2);
  EXPECT_EQ(s.mismatching_frames, 0u);
  EXPECT_EQ(s.sp_mismatches, 0u);
  EXPECT_EQ(s.restore_us_max, 7u);  // the fake clock advances 7 us per call
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_TRUE(sink.osd.empty());
  EXPECT_TRUE(sink.Contains("SYNCTEST start K=2 EVERY=1 window=0x400 whitelist=test.json ranges=2 bytes=320"));
  EXPECT_TRUE(sink.Contains("arm=60 noop=0"));
  EXPECT_TRUE(sink.Contains("NOTE armed at frame 1059 (60 hook calls with both fighters)"));
  EXPECT_EQ(s.icache_us_max, 7u);
  EXPECT_EQ(s.noop_restores, 0u);
  EXPECT_TRUE(sink.Contains("SUMMARY frame="));  // 600 frames compared were passed

  test.Finish();
  test.Finish();  // only once
  int finals = 0;
  for (const std::string& l : sink.lines)
    finals += l.find("SUMMARY final") != std::string::npos;
  EXPECT_EQ(finals, 1);
}

TEST(Rev3Rollback, EveryNthFrame)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(3, 5), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  // A rollback needs frame % 5 == 0; each one replays 3 frames, so about 400 / 5 minus the resimulated frames.
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_LT(test.Stats().rollbacks, 80u);
  EXPECT_EQ(test.Stats().frames_compared, test.Stats().rollbacks * 3);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, IncompleteWhitelistIsReported)
{
  FakeGame game(true);  // a hidden counter outside the whitelist feeds the state
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
    game.Frame(test);

  EXPECT_GT(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains("MISMATCH frame="));
  EXPECT_TRUE(sink.Contains("K=2"));
  EXPECT_TRUE(sink.Contains("  p1 x ref=0x"));  // fighter 1's x differs
  EXPECT_TRUE(sink.Contains("  range 0x80500000-0x80500100 crc ref="));
  EXPECT_TRUE(sink.Contains("first(addr:ref>resim)= 0x80500000:"));
  ASSERT_EQ(sink.osd.size(), 1u);  // on screen only once
  EXPECT_NE(sink.osd[0].find("MISMATCH"), std::string::npos);

  // Full detail for the first 20 mismatching frames, then counts only.
  int detailed = 0;
  for (const std::string& l : sink.lines)
    detailed += l.rfind("MISMATCH frame=", 0) == 0;
  EXPECT_EQ(detailed, 20);
  EXPECT_GT(test.Stats().mismatching_frames, 20u);
  EXPECT_TRUE(sink.Contains("detail is logged for the first 20"));
}

TEST(Rev3Rollback, StackPointerMismatchSkipsTheRestore)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_GT(rollbacks, 0u);

  // Drive the state machine to a normal frame whose rollback point has another sp.
  // The game is mid-cycle at an arbitrary place, so run until a normal frame (not resimulating) comes next.
  while (test.Resimulating())
    game.Frame(test);
  const Reply r = game.Frame(test, SP + 0x40);
  EXPECT_EQ(r.flags, 0u);  // no restore under a different sp
  EXPECT_EQ(test.Stats().sp_mismatches, 1u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_TRUE(sink.Contains("ERROR"));
  EXPECT_TRUE(sink.Contains("differs from the snapshot"));

  // And it carries on afterwards.
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, LosingTheFightClearsHistory)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(test.Active());

  game.Mem().SetU32(0x8038FE50 + 4, 0);  // team 1 gone (loading screen)
  for (int i = 0; i < 20; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.frame, frame);
  }
  EXPECT_FALSE(test.Active());
  EXPECT_FALSE(test.Resimulating());
  EXPECT_TRUE(sink.Contains("fighters lost"));

  // Back in a fight: 60 commands must pass again before any rollback.
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, GameThatIgnoresTheRestoreIsDetected)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 61; ++i)
    game.Frame(test);
  // Next call is the rollback; the "hook" ignores the RESTORED frame and carries on counting up.
  Command c;
  c.frame = game.Counter();
  c.pads = FakeGame::PadsFor(c.frame);
  c.sp = SP;
  const Reply r = test.OnCommand(c);
  ASSERT_EQ(r.flags, REPLY_FLAG_RESTORED);
  c.frame = r.frame + 5;  // wrong: the restore asks for r.frame + 1 next
  const Reply r2 = test.OnCommand(c);
  EXPECT_EQ(r2.flags, 0u);
  EXPECT_FALSE(test.Resimulating());
  EXPECT_TRUE(sink.Contains("ERROR resimulation expected frame"));
}

// ---- arm delay, noop mode, hardware exclusions, pad toggle, JIT invalidation ------------------------------------------------

TEST(Rev3Rollback, ArmsOnlyAfterTheConfiguredDelay)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.arm_commands = 600;  // the product default
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 599; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_FALSE(sink.Contains("NOTE armed"));

  game.Frame(test);  // the 600th call arms
  EXPECT_TRUE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE armed at frame 1599"));
  for (int i = 0; i < 30; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // Losing the fighters disarms and the delay starts over.
  game.Mem().SetU32(0x8038FE50 + 4, 0);
  game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("fighters lost"));
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 599; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_FALSE(test.Active());
}

TEST(Rev3Rollback, HeartbeatEvery60ThenEvery300CallsWhileArmed)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 60 + 899; ++i)  // armed on call 60; 899 armed calls after it plus the arming call = 900
    game.Frame(test);
  int hb = 0;
  for (const std::string& l : sink.lines)
    hb += l.rfind("HB frame=", 0) == 0;
  // armed calls 60, 120 .. 600 (10 lines), then 900
  EXPECT_EQ(hb, 11);
  EXPECT_TRUE(sink.Contains(" calls=60 rollbacks="));
  EXPECT_TRUE(sink.Contains(" calls=600 rollbacks="));
  EXPECT_FALSE(sink.Contains("calls=660 "));
  EXPECT_TRUE(sink.Contains("calls=900 "));
}

TEST(Rev3Rollback, DspStateIsLoggedAtArmAndInEveryHeartbeat)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  int calls = 0;
  test.SetDspState([&calls]() { return "dsp csr=0x0804 left=" + std::to_string(++calls); });
  for (int i = 0; i < 60 + 130; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE dsp at arm: dsp csr=0x0804 left=1"));
  int hb_with_dsp = 0;
  for (const std::string& l : sink.lines)
    hb_with_dsp += l.rfind("HB frame=", 0) == 0 && l.find(" dsp csr=0x0804 left=") != std::string::npos;
  EXPECT_EQ(hb_with_dsp, 2);  // armed calls 60 and 120
}

TEST(Rev3Rollback, FirstTenRestoresAreLogged)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  int done = 0, begin = 0;
  for (const std::string& l : sink.lines)
  {
    done += l.rfind("RESTORE ", 0) == 0 && l.find(" bytes=") != std::string::npos;
    begin += l.rfind("RESTORE ", 0) == 0 && l.find(" begin") != std::string::npos;
  }
  EXPECT_EQ(done, 10);
  EXPECT_EQ(begin, 10);
  EXPECT_TRUE(sink.Contains("RESTORE 1061 -> 1059 bytes=" + std::to_string(320 + WINDOW) + " us=7 icache_us=7"));
}

TEST(Rev3Rollback, RestoresElevenToThirtyAreOneShortLine)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_GT(test.Stats().restores, 40u);
  int full = 0, shortline = 0, begin = 0;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("RESTORE ", 0) != 0)
      continue;
    begin += l.find(" begin") != std::string::npos;
    full += l.find(" bytes=") != std::string::npos;
    shortline += l.find(" bytes=") == std::string::npos && l.find(" begin") == std::string::npos && l.find(" us=7") != std::string::npos;
  }
  EXPECT_EQ(begin, 10);
  EXPECT_EQ(full, 10);
  EXPECT_EQ(shortline, 20);  // restores 11..30; none after
}

TEST(Rev3Rollback, NoopModeRestoresTheSameFrameEveryFrame)
{
  FakeGame game(true);  // even a leaky game cannot mismatch: nothing is compared
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.noop = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 59; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);

  for (int i = 0; i < 700; ++i)
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);  // checks that the stack frame came back
    EXPECT_EQ(r.flags, REPLY_FLAG_RESTORED);
    EXPECT_EQ(r.frame, frame);                       // same frame ...
    EXPECT_EQ(r.pads, FakeGame::PadsFor(frame));     // ... same inputs
    EXPECT_EQ(game.Counter(), frame + 1);            // the game moves on normally
  }
  const SyncTestStats& s = test.Stats();
  EXPECT_EQ(s.noop_restores, 700u);
  EXPECT_EQ(s.rollbacks, 0u);
  EXPECT_EQ(s.frames_compared, 0u);
  EXPECT_EQ(s.mismatching_frames, 0u);
  EXPECT_FALSE(test.Resimulating());
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_FALSE(sink.Contains("ERROR"));
  EXPECT_TRUE(sink.Contains("noop=1"));
  EXPECT_TRUE(sink.Contains("SUMMARY frame="));  // every 600 noop restores
  test.Finish();
  EXPECT_TRUE(sink.Contains("noop_restores=700"));
}

TEST(Rev3Rollback, HardwareRegionsAreSubtractedFromTheSnapshot)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  game.EnableHardwareWord();
  hw.list = {{"gx_fifo", STATE_A + 0x40, STATE_A + 0x80},  // inside a whitelist range: 0x40 bytes go
             {"xfb_top", 0x80900000, 0x80980000}};          // not in the whitelist at all: logged, nothing removed
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 300; ++i)
    game.Frame(test);

  EXPECT_GT(hw.queries, 0);
  EXPECT_TRUE(sink.Contains("NOTE exclude gx_fifo 0x80500040-0x80500080 (64 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE exclude xfb_top 0x80900000-0x80980000 (0 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE snapshot now ranges=3"));
  size_t bytes = 0;
  for (const Range& r : test.Ranges())
  {
    bytes += r.end - r.start;
    EXPECT_TRUE(r.end <= STATE_A + 0x40 || r.start >= STATE_A + 0x80);
  }
  EXPECT_EQ(bytes, 320u - 0x40u);
  EXPECT_EQ(test.SnapshotBytes(), bytes + WINDOW);

  // The hardware's own word was never rewound, and since it is not in the snapshot the replay does not differ because of it.
  EXPECT_GT(game.RestoresSeen(), 20);
  EXPECT_TRUE(game.HwWordIntact());
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_EQ(test.Stats().sp_mismatches, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));

  // Control: without the exclusion the same word is rewound and the replay differs.
  FakeGame game2(false);
  game2.EnableHardwareWord();
  FakeSink sink2;
  SyncTest test2(Options(2), game2.Ranges(), game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 300; ++i)
    game2.Frame(test2);
  EXPECT_FALSE(game2.HwWordIntact());
  EXPECT_GT(test2.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, NewHardwareRegionsClearTheHistoryAndOnlyGrow)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  hw.list = {{"audio_dma", STATE_B + 0x08, STATE_B + 0x10}};  // the game's own state at STATE_B + 4 stays in
  SyncTestOptions o = Options(2);
  o.hardware_check_every = 50;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Ranges().size(), 3u);  // STATE_A, STATE_B + 0 .. 8, STATE_B + 0x10 .. 0x40
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_GT(rollbacks, 0u);

  // A second buffer shows up (double buffering); the first stays excluded even when the hardware no longer reports it.
  hw.list = {{"audio_dma", STATE_B + 0x20, STATE_B + 0x30}};
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE exclude audio_dma 0x80600020-0x80600030"));
  EXPECT_TRUE(sink.Contains("history cleared, frame"));
  const std::vector<Range> want = {{STATE_A, STATE_A + 0x100}, {STATE_B, STATE_B + 0x08}, {STATE_B + 0x10, STATE_B + 0x20}, {STATE_B + 0x30, STATE_B + 0x40}};
  ASSERT_EQ(test.Ranges().size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    EXPECT_EQ(test.Ranges()[i].start, want[i].start) << i;
    EXPECT_EQ(test.Ranges()[i].end, want[i].end) << i;
  }
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  // Seeing the same region again is not news.
  int lines = 0;
  for (const std::string& l : sink.lines)
    lines += l.rfind("NOTE exclude", 0) == 0;
  EXPECT_EQ(lines, 2);
}

TEST(Rev3Rollback, PadZArmsAndDisarms)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.arm_by_pad = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  game.SetZ(false);
  for (int i = 0; i < 300; ++i)  // fighters present for a long time, no press: never armed
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("arm=pad"));

  game.SetZ(true);  // press: armed
  game.Frame(test);
  EXPECT_TRUE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE armed by Z at frame 1300"));
  for (int i = 0; i < 20; ++i)  // holding Z does nothing
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  game.SetZ(false);
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_TRUE(test.Active());
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // A press in the middle of a resimulation waits until it is over.
  while (!test.Resimulating())
    game.Frame(test);
  game.SetZ(true);
  game.Frame(test);
  EXPECT_TRUE(test.Active());
  game.SetZ(false);
  for (int i = 0; i < 6; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_TRUE(sink.Contains("NOTE disarmed by Z"));
  EXPECT_FALSE(test.Resimulating());
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 50; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // And a new press arms again.
  game.SetZ(true);
  game.Frame(test);
  EXPECT_TRUE(test.Active());
}

TEST(Rev3Rollback, RestoredRangesAreInvalidatedInTheJit)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  ASSERT_GT(rollbacks, 0u);
  // Two ranges per restore, each invalidated once with its own start and length.
  ASSERT_EQ(game.Mem().invalidated.size(), rollbacks * 2);
  EXPECT_EQ(game.Mem().invalidated[0], std::make_pair(STATE_A, size_t(0x100)));
  EXPECT_EQ(game.Mem().invalidated[1], std::make_pair(STATE_B, size_t(0x40)));
  EXPECT_EQ(test.Stats().icache_us_total, test.Stats().restore_us_total);  // the fake clock: 7 us each

  FakeGame game2(false);
  FakeSink sink2;
  SyncTestOptions o = Options(2);
  o.invalidate_code = false;
  SyncTest test2(o, game2.Ranges(), game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 70; ++i)
    game2.Frame(test2);
  EXPECT_GT(test2.Stats().rollbacks, 0u);
  EXPECT_TRUE(game2.Mem().invalidated.empty());
}

// ---- field coverage diagnostics ---------------------------------------------------------------------------------------------

namespace
{
size_t CountInFieldRange(u32 range_bytes)  // fields of a fighter whose 4 bytes lie inside [0, range_bytes)
{
  size_t n = 0;
  for (const FieldDef& f : FIELDS)
    n += (u32(f.offset) + 4 <= range_bytes);
  return n;
}
}  // namespace

TEST(Rev3Rollback, ArmLogsFighterAddressesAndFieldCoverage)
{
  FakeGame game(false);
  FakeSink sink;
  // Only the first 0x100 bytes of fighter 1 are whitelisted; fighter 2 not at all.
  std::vector<Range> ranges = game.Ranges();
  ranges.push_back({FIGHTER[0], FIGHTER[0] + 0x100});
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 61; ++i)
    game.Frame(test);

  EXPECT_TRUE(sink.Contains("NOTE fighters p1=0x80420000 p2=0x80430000"));
  const size_t in1 = CountInFieldRange(0x100);
  ASSERT_GT(in1, 0u);
  ASSERT_LT(in1, FIELD_COUNT);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=" + std::to_string(in1) + "/61 NOT IN SNAPSHOT:"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=0/61 NOT IN SNAPSHOT: action@0x80430140"));
  // p1's list names a field past 0x100 (action is at 0x140) but not one inside (x at 0x84).
  std::string p1_line;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("NOTE field coverage p1", 0) == 0)
      p1_line = l;
  }
  EXPECT_NE(p1_line.find(" action@0x80420140"), std::string::npos);
  EXPECT_EQ(p1_line.find(" x@0x80420084"), std::string::npos);

  // Everything in: no list.
  FakeGame game2(false);
  FakeSink sink2;
  std::vector<Range> all = game2.Ranges();
  all.push_back({FIGHTER[0], FIGHTER[0] + 0x1700});
  all.push_back({FIGHTER[1], FIGHTER[1] + 0x1700});
  SyncTest test2(Options(2), all, game2.Mem(), sink2, FakeClock());
  for (int i = 0; i < 61; ++i)
    game2.Frame(test2);
  EXPECT_TRUE(sink2.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_FALSE(sink2.Contains("NOT IN SNAPSHOT"));
}

TEST(Rev3Rollback, MismatchNamesTheAddressAndTheSnapshotRangeOfEachNewField)
{
  FakeGame game(true, 1100);  // leak into p1 x from the start, into p1 anglea0 and p2 y from frame 1100
  FakeSink sink;
  std::vector<Range> ranges = game.Ranges();
  ranges.push_back({FIGHTER[0], FIGHTER[0] + 0x100});  // p1 x (0x84) and anglea0 (0xA0) are inside; p2 y is not
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);

  EXPECT_TRUE(sink.Contains("  p1 x ref=0x"));
  EXPECT_TRUE(sink.Contains(" addr=0x80420084 in 0x80420000-0x80420100"));

  // The address is added the first time a field name differs, not on every line.
  int x_lines = 0, x_annotated = 0, y_lines = 0, y_annotated = 0;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("  p1 x ref=", 0) == 0)
    {
      ++x_lines;
      x_annotated += l.find(" addr=") != std::string::npos;
    }
    if (l.rfind("  p2 y ref=", 0) == 0)
    {
      ++y_lines;
      y_annotated += l.find(" addr=") != std::string::npos;
    }
  }
  EXPECT_GT(x_lines, 5);
  EXPECT_EQ(x_annotated, 1);

  // Fields that differ for the first time after the first 20 detailed frames still get a block, with their address.
  EXPECT_TRUE(sink.Contains("  p1 anglea0 ref=0x"));
  EXPECT_TRUE(sink.Contains(" addr=0x804200A0 in 0x80420000-0x80420100"));
  EXPECT_GE(y_lines, 1);
  EXPECT_EQ(y_annotated, 1);
  EXPECT_TRUE(sink.Contains(" addr=0x80430088 NOT IN SNAPSHOT"));
  int detailed = 0;
  for (const std::string& l : sink.lines)
    detailed += l.rfind("MISMATCH frame=", 0) == 0;
  EXPECT_GT(detailed, 20);
  EXPECT_LT(detailed, 40);
  EXPECT_GT(test.Stats().mismatching_frames, 40u);
}

// ---- time base ---------------------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, TimeBaseIsRestoredWhenEnabled)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.restore_timebase = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_TRUE(game.TimeBaseConsistent());  // a replayed frame saw the same time base as the first run
  EXPECT_EQ(game.Mem().time_base_writes, static_cast<int>(test.Stats().rollbacks));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains("NOTE timebase restore on"));
  EXPECT_TRUE(sink.Contains(" tb=1"));
  // K = 2 frames of 1000 ticks moved back; shown in the full lines and in the short ones (restores 11..30).
  EXPECT_TRUE(sink.Contains("RESTORE 1061 -> 1059 bytes=" + std::to_string(320 + WINDOW) + " us=7 icache_us=7 tb_delta=2000"));
  int short_with_tb = 0;
  for (const std::string& l : sink.lines)
    short_with_tb += l.rfind("RESTORE ", 0) == 0 && l.find(" bytes=") == std::string::npos && l.find(" tb_delta=2000") != std::string::npos;
  EXPECT_EQ(short_with_tb, 20);
}

TEST(Rev3Rollback, TimeBaseIsLeftAloneByDefault)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(game.Mem().time_base_writes, 0);
  EXPECT_FALSE(game.TimeBaseConsistent());  // control: the replayed frames see a later time base
  EXPECT_FALSE(sink.Contains("tb_delta="));
  EXPECT_FALSE(sink.Contains("NOTE timebase"));
  EXPECT_TRUE(sink.Contains(" tb=0"));
}

TEST(Rev3Rollback, TimeBaseRestoreWorksInNoopMode)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.noop = true;
  o.restore_timebase = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().noop_restores, 30u);
  EXPECT_TRUE(game.TimeBaseConsistent());
  EXPECT_TRUE(sink.Contains("tb_delta=0"));  // same frame: nothing moved
}

// ---- user exclusions (REV3_SYNCTEST_EXCLUDE) ---------------------------------------------------------------------------------

TEST(Rev3Rollback, UserExclusionsAreSubtractedAndLogged)
{
  FakeGame game(false);
  FakeSink sink;
  std::vector<Range> ranges = game.Ranges();
  std::string error;
  ASSERT_TRUE(ApplyExclusions(R"({"ranges":[{"start":"0x80500040","end":"0x80500080"},{"start":"0x80900000","end":"0x80900100"}]})",
                              &ranges, sink, &error))
      << error;
  EXPECT_TRUE(sink.Contains("NOTE exclude user 0x80500040-0x80500080 (64 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE exclude user 0x80900000-0x80900100 (0 bytes were in the whitelist)"));
  size_t bytes = 0;
  for (const Range& r : ranges)
    bytes += r.end - r.start;
  EXPECT_EQ(bytes, 320u - 0x40u);
  EXPECT_EQ(ranges.size(), 3u);

  // Used for a SyncTest: the word the "hardware" writes inside the excluded part is never rewound.
  game.EnableHardwareWord();
  SyncTest test(Options(2), ranges, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(game.RestoresSeen(), 20);
  EXPECT_TRUE(game.HwWordIntact());
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_TRUE(sink.Contains(" bytes=256 "));  // the start line: 320 - 64 whitelisted bytes remain
}

TEST(Rev3Rollback, UserExclusionsRejectBadInput)
{
  FakeGame game(false);
  FakeSink sink;
  const std::vector<Range> original = game.Ranges();
  std::vector<Range> ranges = original;
  std::string error;
  EXPECT_FALSE(ApplyExclusions("not json", &ranges, sink, &error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(ApplyExclusions(R"({"ranges":[{"start":"0x80500000","end":"0x80600040"}]})", &ranges, sink, &error));  // removes all
  EXPECT_NE(error.find("every range"), std::string::npos);
  ASSERT_EQ(ranges.size(), original.size());  // untouched on failure
  EXPECT_EQ(ranges[0].start, original[0].start);
  EXPECT_EQ(ranges[1].end, original[1].end);
  EXPECT_FALSE(sink.Contains("NOTE exclude user"));
}

// ---- the adaptive snapshot set ------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, MergeRangesSortsAndJoins)
{
  const std::vector<Range> merged = MergeRanges({{0x300, 0x400}, {0x100, 0x200}, {0x200, 0x250}, {0x380, 0x500}, {0x700, 0x700}});
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].start, 0x100u);
  EXPECT_EQ(merged[0].end, 0x250u);
  EXPECT_EQ(merged[1].start, 0x300u);
  EXPECT_EQ(merged[1].end, 0x500u);
}

TEST(Rev3Rollback, PageTrackerLearnsPagesThatChange)
{
  for (const bool peek : {true, false})  // direct pointers and the Read() fallback must agree
  {
    FakeMemory mem;
    mem.peek_enabled = peek;
    PageTracker tracker(mem, 4);
    const u32 busy = 0x80100000, busy2 = 0x80101000, quiet = 0x80200000, once = 0x80300000;
    mem.SetU32(quiet, 0x1234);

    // Cycle 1 only takes baselines (22528 pages in 4 slices); nothing can be dynamic yet.
    for (int i = 0; i < 4; ++i)
      EXPECT_EQ(tracker.Scan(), 0u);
    EXPECT_EQ(tracker.Cycles(), 1u);
    EXPECT_EQ(tracker.DynamicPages(), 0u);

    for (u32 cycle = 0; cycle < 3; ++cycle)
    {
      mem.SetU32(busy, 100 + cycle);        // changes every cycle
      mem.SetU32(busy2 + 0xFFC, 7 + cycle);  // the last word of the next page
      if (cycle == 0)
        mem.SetU32(once, 5);                // changes once, then never again
      for (int i = 0; i < 4; ++i)
        tracker.Scan();
    }
    EXPECT_EQ(tracker.Cycles(), 4u);
    EXPECT_TRUE(tracker.IsDynamic(busy));
    EXPECT_TRUE(tracker.IsDynamic(busy + 0xFFF));
    EXPECT_TRUE(tracker.IsDynamic(busy2));
    EXPECT_TRUE(tracker.IsDynamic(once));  // sticky
    EXPECT_FALSE(tracker.IsDynamic(quiet));
    EXPECT_FALSE(tracker.IsDynamic(0x80400000));
    EXPECT_EQ(tracker.DynamicPages(), 3u);

    std::vector<Range> ranges;
    tracker.AppendDynamicRanges(&ranges);
    ASSERT_EQ(ranges.size(), 2u);  // the two neighbouring pages are one range
    EXPECT_EQ(ranges[0].start, busy);
    EXPECT_EQ(ranges[0].end, busy + 0x2000);
    EXPECT_EQ(ranges[1].start, once);
    EXPECT_EQ(ranges[1].end, once + 0x1000);

    // A restore rewrites pages: their old hashes are forgotten, so the rewrite is not taken for a change by the game.
    tracker.Reset();
    EXPECT_EQ(tracker.DynamicPages(), 0u);
    EXPECT_EQ(tracker.Cycles(), 0u);
    EXPECT_FALSE(tracker.IsDynamic(busy));
    for (int i = 0; i < 4; ++i)
      tracker.Scan();                       // baselines again
    mem.SetU32(quiet, 0x9999);              // "restored" to other content ...
    tracker.InvalidateBaseline({{quiet, quiet + 4}});
    mem.SetU32(0x80500000, 1);              // ... while a page nobody told the tracker about really changed
    for (int i = 0; i < 4; ++i)
      tracker.Scan();
    EXPECT_FALSE(tracker.IsDynamic(quiet));
    EXPECT_TRUE(tracker.IsDynamic(0x80500000));
    EXPECT_EQ(tracker.DynamicPages(), 1u);
  }
}

TEST(Rev3Rollback, AdaptiveSetAlwaysHoldsBothFightersAndTheStackWindow)
{
  FakeGame game(false);
  game.SetQuiet();  // nothing in memory changes except the hook's stack frame
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());  // no whitelist at all
  EXPECT_TRUE(sink.Contains("adaptive=1 slices=16 learn=20"));
  for (int i = 0; i < 400; ++i)
    game.Frame(test);

  ASSERT_TRUE(test.Active());
  EXPECT_EQ(test.DynamicPages(), 1u);  // only the page of the stack frame
  const std::vector<Range> want = {{FIGHTER[0], FIGHTER[0] + 0x1800}, {FIGHTER[1], FIGHTER[1] + 0x1800}, {SP, SP + 0x1000}};
  ASSERT_EQ(test.Ranges().size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    EXPECT_EQ(test.Ranges()[i].start, want[i].start) << i;
    EXPECT_EQ(test.Ranges()[i].end, want[i].end) << i;
  }
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=61/61"));
  EXPECT_FALSE(sink.Contains("NOT IN SNAPSHOT"));
  EXPECT_TRUE(sink.Contains("NOTE adaptive set at arm: dynamic_pages=1 (4096 bytes)"));
  EXPECT_TRUE(sink.Contains("NOTE snapshot now ranges=3 bytes=" + std::to_string(2 * 0x1800 + 0x1000)));
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, AdaptiveSetLearnsStateThatNoWhitelistNamed)
{
  for (const bool peek : {true, false})
  {
    FakeGame game(true);  // the hidden counter outside any whitelist feeds the state (the classic failure of a fixed whitelist)
    game.Mem().peek_enabled = peek;
    FakeSink sink;
    SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 320 + 400; ++i)
      game.Frame(test);
    while (test.Resimulating())
      game.Frame(test);

    EXPECT_TRUE(test.Active());
    EXPECT_GT(test.Stats().rollbacks, 100u);
    EXPECT_EQ(test.Stats().mismatching_frames, 0u);
    EXPECT_FALSE(sink.Contains("MISMATCH"));
    EXPECT_FALSE(sink.Contains("ERROR"));
    EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN));
    EXPECT_TRUE(SetContains(test.Ranges(), STATE_A));
    EXPECT_TRUE(SetContains(test.Ranges(), STATE_B + 4));
    EXPECT_FALSE(SetContains(test.Ranges(), 0x80900000));  // memory that never changes stays out
    EXPECT_EQ(test.Stats().pages_learned_armed, 0u);       // all of it was learned before arming
    EXPECT_TRUE(sink.Contains("NOTE learning complete: 20 scan cycles (slices=16)"));
    EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot=61/61"));
    // Scan cost is in the heartbeat and the summary (the fake clock makes every timed call 7 us).
    EXPECT_TRUE(sink.Contains(" dyn="));
    EXPECT_TRUE(sink.Contains(" scan_us=7.0 scan_us_max=7"));
    test.Finish();
    EXPECT_TRUE(sink.Contains("SUMMARY final "));
    EXPECT_TRUE(sink.Contains(" dynamic_pages="));
    EXPECT_TRUE(sink.Contains(" scan_us_mean=7.0 scan_us_max=7"));
  }
}

TEST(Rev3Rollback, WhitelistStaysAnOptionalSeed)
{
  FakeGame game(true);
  FakeSink sink;
  // The seed names STATE_A / STATE_B only; the hidden counter is learned on top, the seed is kept even where nothing changes.
  std::vector<Range> seed = game.Ranges();
  seed.push_back({0x80900000, 0x80900100});  // never changes
  SyncTest test(AdaptiveOptions(2), seed, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 320 + 100; ++i)
    game.Frame(test);
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80900000));
  EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_GT(test.Stats().rollbacks, 20u);
}

TEST(Rev3Rollback, ArmWaitsForTheLearningWindow)
{
  // Count mode: the 60 calls of REV3_SYNCTEST_ARM are not enough; 20 cycles of 16 scans = 320 calls are.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 319; ++i)
      EXPECT_EQ(game.Frame(test).flags, 0u);
    EXPECT_FALSE(test.Active());
    EXPECT_FALSE(test.LearningDone());
    EXPECT_EQ(test.Stats().rollbacks, 0u);
    EXPECT_TRUE(sink.Contains("NOTE arm deferred: learning (3/20 cycles)"));  // first asked for at call 60
    EXPECT_FALSE(sink.Contains("NOTE armed"));

    game.Frame(test);  // call 320 completes the 20th cycle: learned and armed together
    EXPECT_TRUE(test.LearningDone());
    EXPECT_TRUE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE learning complete: 20 scan cycles"));
    EXPECT_TRUE(sink.Contains("NOTE armed at frame 1319 (320 hook calls with both fighters)"));
    int deferred = 0;
    for (const std::string& l : sink.lines)
      deferred += l.rfind("NOTE arm deferred", 0) == 0;
    EXPECT_EQ(deferred, 1);  // logged once, not on every call
  }
  // A long N later than the learning window: N decides.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_commands = 600;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 599; ++i)
      game.Frame(test);
    EXPECT_TRUE(test.LearningDone());
    EXPECT_FALSE(test.Active());
    EXPECT_FALSE(sink.Contains("arm deferred"));
    game.Frame(test);
    EXPECT_TRUE(test.Active());
  }
  // Pad mode: a Z press during learning is remembered and arms by itself when learning completes.
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_by_pad = true;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    game.SetZ(false);
    for (int i = 0; i < 49; ++i)
      game.Frame(test);
    game.SetZ(true);  // call 50
    game.Frame(test);
    game.SetZ(false);
    EXPECT_TRUE(sink.Contains("NOTE arm deferred: learning (3/20 cycles)"));
    EXPECT_FALSE(test.Active());
    for (int i = 0; i < 269; ++i)  // calls 51..319
      game.Frame(test);
    EXPECT_FALSE(test.Active());
    EXPECT_EQ(test.Stats().rollbacks, 0u);
    game.Frame(test);  // call 320
    EXPECT_TRUE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE armed by Z at frame 1319"));
    for (int i = 0; i < 50; ++i)
      game.Frame(test);
    EXPECT_GT(test.Stats().rollbacks, 5u);
    EXPECT_EQ(test.Stats().mismatching_frames, 0u);

    // Z again disarms as before; pressing before learning is done and again cancels the wish.
    game.SetZ(true);
    while (test.Resimulating())
      game.Frame(test);
    game.Frame(test);
    game.SetZ(false);
    EXPECT_FALSE(test.Active());
    EXPECT_TRUE(sink.Contains("NOTE disarmed by Z"));
  }
  {
    FakeGame game(false);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(2);
    o.arm_by_pad = true;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    game.SetZ(false);
    for (int i = 0; i < 10; ++i)
      game.Frame(test);
    game.SetZ(true);
    game.Frame(test);
    game.SetZ(false);
    game.Frame(test);
    game.SetZ(true);  // second press: cancel
    game.Frame(test);
    game.SetZ(false);
    EXPECT_TRUE(sink.Contains("NOTE arm cancelled by Z"));
    for (int i = 0; i < 400; ++i)
      game.Frame(test);
    EXPECT_FALSE(test.Active());  // learning completed, but nobody wants it armed
    EXPECT_TRUE(test.LearningDone());
  }
}

TEST(Rev3Rollback, ExclusionsWinOverLearnedAndSeededPages)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  hw.list = {{"audio_dma", STATE_B + 0x10, STATE_B + 0x20}, {"xfb_top", 0x80700000, 0x80701000}};  // cuts the hidden counter's page
  std::vector<Range> seed = game.Ranges();
  seed.push_back({0x80002000, 0x80003000});  // seeded across the hook region
  SyncTest test(AdaptiveOptions(2), seed, game.Mem(), sink, FakeClock(), &hw);
  test.SetUserExclusions({{STATE_A + 0x200, STATE_A + 0x300}, {FIGHTER[1] + 0x1700, FIGHTER[1] + 0x1800}});  // fighter fields end at 0x1648
  for (int i = 0; i < 320 + 300; ++i)
  {
    game.Mem().SetU32(0x80002500, i);  // the hook's own counter changes every frame: its page is dynamic, its region still cut
    game.Frame(test);
  }
  ASSERT_TRUE(test.Active());
  EXPECT_TRUE(test.LearningDone());
  const std::vector<std::pair<u32, u32>> cuts = {{HOOK_REGION_START, HOOK_REGION_END},
                                                 {STATE_A + 0x200, STATE_A + 0x300},
                                                 {FIGHTER[1] + 0x1700, FIGHTER[1] + 0x1800},
                                                 {STATE_B + 0x10, STATE_B + 0x20},
                                                 {0x80700000, 0x80701000}};
  for (const Range& r : test.Ranges())
  {
    for (const auto& c : cuts)
      EXPECT_FALSE(Overlaps(r, c.first, c.second)) << std::hex << r.start << "-" << r.end << " vs " << c.first;
  }
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80002000));  // the rest of that page is in
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80002A00));
  EXPECT_TRUE(SetContains(test.Ranges(), STATE_A));
  EXPECT_TRUE(SetContains(test.Ranges(), FIGHTER[1] + 0x1000));
  EXPECT_GT(test.DynamicPages(), 3u);  // the cut pages were learned as dynamic, then cut
  EXPECT_TRUE(sink.Contains("NOTE exclude xfb_top 0x80700000-0x80701000"));
  EXPECT_TRUE(sink.Contains("in_snapshot=61/61"));
  EXPECT_FALSE(sink.Contains("ERROR"));
}

TEST(Rev3Rollback, AFighterFieldThatIsCutRefusesToArm)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  test.SetUserExclusions({{FIGHTER[1] + 0x100, FIGHTER[1] + 0x200}});  // takes the fields at 0x140, 0x150, 0x158 ... away
  for (int i = 0; i < 1000; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.Stats().rollbacks, 0u);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
  EXPECT_TRUE(sink.Contains("NOTE field coverage p2 fighter=0x80430000 in_snapshot="));
  EXPECT_TRUE(sink.Contains(" NOT IN SNAPSHOT: action@0x80430140"));
  EXPECT_TRUE(sink.Contains("ERROR frame 1319: not all fighter fields are inside the snapshot set"));
  int errors = 0;
  for (const std::string& l : sink.lines)
    errors += l.rfind("ERROR", 0) == 0;
  EXPECT_EQ(errors, 1);  // once, not on every call
  EXPECT_FALSE(sink.Contains("NOTE armed"));
}

TEST(Rev3Rollback, ResimulatedFramesAreNotScanned)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(3), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 320; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  int resim_calls = 0;
  for (int i = 0; i < 200; ++i)
  {
    const bool resim = test.Resimulating();  // this call replays a frame
    const u64 scans = test.Stats().scans;
    game.Frame(test);
    EXPECT_EQ(test.Stats().scans, scans + (resim ? 0 : 1));
    resim_calls += resim;
  }
  EXPECT_GT(resim_calls, 100);  // K = 3: three of every four calls replay
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, PagesThatChangeLaterAreAddedAndTheHistoryIsCleared)
{
  FakeGame game(false);
  game.SetLatePage(1500);  // armed at frame 1319; from 1500 a counter in an unseen page feeds p1's combo_count
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 1500)  // with a rollback every frame the game advances one frame per K + 1 hook calls
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_FALSE(SetContains(test.Ranges(), LATE_PAGE));
  const size_t bytes_before = test.SnapshotBytes();
  const u32 rollbacks_before = test.Stats().rollbacks;
  EXPECT_GT(rollbacks_before, 50u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // Until the tracker comes round to that page the replay differs; then the page is added and the set rebuilt.
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  EXPECT_TRUE(SetContains(test.Ranges(), LATE_PAGE));
  EXPECT_GE(test.Stats().pages_learned_armed, 1u);
  EXPECT_GT(test.SnapshotBytes(), bytes_before);
  EXPECT_TRUE(sink.Contains("NOTE learned +1 pages (now ranges="));
  EXPECT_TRUE(sink.Contains("history cleared"));
  EXPECT_GT(test.Stats().mismatching_frames, 0u);  // the missing page was noticed before it was learned

  // From here on the replays agree again.
  while (test.Resimulating())
    game.Frame(test);
  const u32 mismatches = test.Stats().mismatching_frames;
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 300; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks + 30);
  EXPECT_EQ(test.Stats().mismatching_frames, mismatches);
  EXPECT_TRUE(sink.Contains("NOTE field coverage p1 fighter=0x80420000 in_snapshot=61/61"));
}

TEST(Rev3Rollback, LearningWhileArmedIsRateLimitedInTheLog)
{
  FakeGame game(false);
  // One new page per frame from frame 4400, each in the next scan slice (1024 slices of 22 pages): 60 growths in a row. The
  // first scan cycle only records baselines and the second finds the game's own pages, so learning 3 cycles (3072 calls) is
  // enough to arm with a complete set.
  game.SetWalker(4400, 22 * 0x1000);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2, 1024, 3), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 4400 + 1100)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GE(test.Stats().pages_learned_armed, 50u);
  int learned = 0;
  for (const std::string& l : sink.lines)
    learned += l.rfind("NOTE learned +", 0) == 0;
  EXPECT_GE(learned, 10);
  EXPECT_LE(learned, 14);
  EXPECT_TRUE(sink.Contains("NOTE further learning is logged at most every 300 hook calls"));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, LosingTheFightForgetsEverythingLearned)
{
  FakeGame game(true);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GT(test.DynamicPages(), 3u);

  game.Mem().SetU32(0x8038FE50 + 4, 0);  // loading screen
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.Active());
  EXPECT_EQ(test.DynamicPages(), 0u);
  EXPECT_EQ(test.LearnCycles(), 0u);
  EXPECT_FALSE(test.LearningDone());

  // A new match learns from scratch: no rollback for 320 calls even though 60 would do for the count.
  game.Mem().SetU32(0x8038FE50 + 4, TEAM[1]);
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 319; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  for (int i = 0; i < 40; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  int complete = 0;
  for (const std::string& l : sink.lines)
    complete += l.rfind("NOTE learning complete", 0) == 0;
  EXPECT_EQ(complete, 2);
}

TEST(Rev3Rollback, ExclusionsMayEmptyTheSeedWhenTheSetIsAdaptive)
{
  FakeSink sink;
  std::vector<Range> ranges = {{STATE_A, STATE_A + 0x100}};
  std::vector<Range> parsed;
  std::string error;
  const char* const json = R"({"ranges":[{"start":"0x80500000","end":"0x80500100"}]})";
  EXPECT_FALSE(ApplyExclusions(json, &ranges, sink, &error, &parsed, true));  // classic: nothing would be left
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_TRUE(ApplyExclusions(json, &ranges, sink, &error, &parsed, false));
  EXPECT_TRUE(ranges.empty());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].start, STATE_A);
  EXPECT_EQ(parsed[0].end, STATE_A + 0x100);
}

// ---- pointer based exclusions and the disc I/O gate ---------------------------------------------------------------------------

TEST(Rev3Rollback, PointerExclusionsAreParsedAndKeptApart)
{
  FakeSink sink;
  std::vector<Range> ranges = {{STATE_A, STATE_A + 0x100}};
  std::vector<Range> parsed;
  std::vector<PtrExclusion> ptrs;
  std::string error;
  const char* const json =
      R"({"ranges":[{"ptr":"0x8049D000","size":"0x3F0000","note":"nw4r sound system"},{"start":"0x80500040","end":"0x80500080"},)"
      R"({"ptr":2152143696,"size":100000}]})";
  ASSERT_TRUE(ApplyExclusions(json, &ranges, sink, &error, &parsed, false, &ptrs)) << error;
  ASSERT_EQ(ptrs.size(), 2u);
  EXPECT_EQ(ptrs[0].ptr, 0x8049D000u);
  EXPECT_EQ(ptrs[0].size, 0x3F0000u);
  EXPECT_EQ(ptrs[0].note, "nw4r sound system");
  EXPECT_EQ(ptrs[1].ptr, 2152143696u);
  EXPECT_EQ(ptrs[1].size, 100000u);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(ranges.size(), 2u);  // the fixed entry was subtracted, the pointer ones were not
  EXPECT_TRUE(sink.Contains("NOTE exclude user 0x80500040-0x80500080 (64 bytes were in the whitelist)"));
  EXPECT_TRUE(sink.Contains("NOTE exclude ptr entry 0x8049D000 size=0x3F0000 note=nw4r sound system"));

  // Without somewhere to put them, pointer entries are an error; so are bad ones.
  std::vector<Range> untouched = {{STATE_A, STATE_A + 0x100}};
  EXPECT_FALSE(ApplyExclusions(json, &untouched, sink, &error));
  EXPECT_NE(error.find("pointer entries"), std::string::npos);
  const char* const bad[] = {
      R"({"ranges":[{"ptr":"0x8049D000"}]})",                       // no size
      R"({"ranges":[{"ptr":"0x8049D000","size":0}]})",              // empty
      R"({"ranges":[{"ptr":"0x00001000","size":"0x100"}]})",        // pointer outside MEM1 / MEM2
      R"({"ranges":[{"ptr":"nope","size":"0x100"}]})",
      R"({"ranges":[]})",
  };
  for (const char* text : bad)
  {
    error.clear();
    EXPECT_FALSE(ApplyExclusions(text, &untouched, sink, &error, &parsed, false, &ptrs)) << text;
    EXPECT_FALSE(error.empty()) << text;
  }
}

TEST(Rev3Rollback, PointerExclusionsAreResolvedAtArmAndOnEveryRescan)
{
  FakeGame game(false);
  FakeSink sink;
  constexpr u32 PTR = 0x80450000;  // the "game's" pointer variable
  SyncTestOptions o = AdaptiveOptions(2);
  o.hardware_check_every = 50;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());  // no hardware view at all: pointer entries still work
  // The pointer variable's own page is cut too: the test changes it by hand, which a restore would otherwise undo.
  test.SetUserExclusions({{PTR, PTR + 0x1000}}, {{PTR, 0x2000, "heap"}});
  game.Mem().SetU32(PTR, 0);  // not allocated yet
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  int skipped = 0;
  for (const std::string& l : sink.lines)
    skipped += l.rfind("NOTE exclude ptr 0x80450000 skipped: pointer is 0", 0) == 0;
  EXPECT_EQ(skipped, 1);  // said once, not on every re-check
  EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN));

  // The heap gets allocated over the hidden counter's page: cut at the next re-check.
  game.Mem().SetU32(PTR, HIDDEN);
  for (int i = 0; i < 120; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE exclude ptr 0x80450000 -> 0x80700000-0x80702000 (4096 bytes were in the whitelist)"));
  for (const Range& r : test.Ranges())
    EXPECT_FALSE(Overlaps(r, HIDDEN, HIDDEN + 0x2000));

  // It moves (a new match): the new place is cut as well, the old cut stays.
  game.Mem().SetU32(PTR, 0x80A00000 + 0x4000);
  for (int i = 0; i < 120; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE exclude ptr 0x80450000 -> 0x80A04000-0x80A06000"));
  EXPECT_FALSE(SetContains(test.Ranges(), HIDDEN));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // A garbage pointer is skipped with its own note.
  game.Mem().SetU32(PTR, 0x12345678);
  for (int i = 0; i < 60; ++i)
    game.Frame(test);
  EXPECT_TRUE(sink.Contains("NOTE exclude ptr 0x80450000 skipped: pointer is outside MEM1 / MEM2 (value 0x12345678)"));
}

TEST(Rev3Rollback, RollbacksWaitWhileADiscReadIsInFlight)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  while (test.Resimulating())
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_GT(rollbacks, 5u);
  EXPECT_EQ(test.Stats().io_postponed, 0u);

  hw.disc_busy = true;  // a replay in progress finishes, no new rollback starts
  for (int i = 0; i < 30; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().io_postponed, 30u);
  int lines = 0;
  for (const std::string& l : sink.lines)
    lines += l.rfind("NOTE rollback postponed: disc read in flight", 0) == 0;
  EXPECT_EQ(lines, 5);  // the first few are logged, the count continues in SUMMARY

  hw.disc_busy = false;  // idle again: the captures made meanwhile are valid rollback targets
  for (int i = 0; i < 30; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks + 5);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_EQ(test.Stats().sp_mismatches, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  test.Finish();
  EXPECT_TRUE(sink.Contains(" io_postponed=30"));
}

TEST(Rev3Rollback, NoopRestoresAlsoWaitForTheDisc)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  SyncTestOptions o = Options(2);
  o.noop = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 80; ++i)
    game.Frame(test);
  const u32 noops = test.Stats().noop_restores;
  EXPECT_GT(noops, 10u);
  hw.disc_busy = true;
  for (int i = 0; i < 10; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u);
  EXPECT_EQ(test.Stats().noop_restores, noops);
  EXPECT_EQ(test.Stats().io_postponed, 10u);
}

// ---- audio hardware rewind ----------------------------------------------------------------------------------------------------

TEST(Rev3Rollback, AudioHardwareIsRewoundWithMemory)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  EXPECT_TRUE(test.AudioHardwareOn());
  EXPECT_TRUE(sink.Contains("NOTE audio hw rewind on (dsp=HLE, bytes=5, events=AudioDMACallback,DSPint)"));

  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  const SyncTestStats& s = test.Stats();
  EXPECT_GT(s.rollbacks, 50u);
  EXPECT_EQ(s.mismatching_frames, 0u);
  // After every restore the audio hardware was exactly what the capture of that frame saw: state, events, interrupt bits.
  EXPECT_EQ(game.AudioWrong(), 0);
  EXPECT_EQ(game.AudioRight(), static_cast<int>(s.rollbacks));
  EXPECT_EQ(audio.loads, static_cast<int>(s.rollbacks));
  EXPECT_EQ(audio.replaces, static_cast<int>(s.rollbacks));
  EXPECT_EQ(audio.irq_sets, static_cast<int>(s.rollbacks));
  EXPECT_EQ(s.audio_restores, s.rollbacks);
  // Memory was written back and the JIT invalidated before the audio hardware was loaded.
  EXPECT_FALSE(audio.invalidated_at_load.empty());
  if (!audio.invalidated_at_load.empty())
    EXPECT_GT(*std::min_element(audio.invalidated_at_load.begin(), audio.invalidated_at_load.end()), 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_TRUE(sink.Contains("NOTE audio hw at arm: bytes="));
  EXPECT_TRUE(sink.Contains("events=2 [AudioDMACallback+"));
  // The RESTORE lines (full and short) carry the audio size and event count.
  int with_audio = 0;
  for (const std::string& l : sink.lines)
    with_audio += l.rfind("RESTORE ", 0) == 0 && l.find(" audio_events=2") != std::string::npos && l.find(" audio_bytes=") != std::string::npos;
  EXPECT_EQ(with_audio, 30);  // 10 full + 20 short lines are logged
  test.Finish();
  EXPECT_TRUE(sink.Contains("audio_restores=" + std::to_string(s.rollbacks)));
}

TEST(Rev3Rollback, AudioStateSizeMayVaryWithoutError)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  EXPECT_TRUE(test.AudioHardwareOn());
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(game.AudioWrong(), 0);
  EXPECT_TRUE(sink.Contains("NOTE audio state size 5 -> "));
  EXPECT_FALSE(sink.Contains("ERROR"));
  int notes = 0;
  for (const std::string& l : sink.lines)
    notes += l.find("NOTE audio state size") != std::string::npos;
  EXPECT_EQ(notes, 3);  // rate limited
}

TEST(Rev3Rollback, AudioHardwareOffByDefaultChangesNothing)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  EXPECT_FALSE(test.AudioHardwareOn());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 10u);
  EXPECT_EQ(test.Stats().audio_restores, 0u);
  EXPECT_FALSE(sink.Contains("audio_bytes="));
  EXPECT_FALSE(sink.Contains("NOTE audio"));
  const std::string full = "RESTORE 1061 -> 1059 bytes=" + std::to_string(320 + WINDOW) + " us=7 icache_us=7";
  bool exact = false;
  for (const std::string& l : sink.lines)
    exact = exact || l == full;  // the line is unchanged: nothing appended
  EXPECT_TRUE(exact);
}

TEST(Rev3Rollback, FailedAudioSaveFallsBackToMemoryOnly)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  const int loads = audio.loads;
  EXPECT_GT(rollbacks, 10u);
  audio.fail_save = true;  // e.g. the measure and write passes disagreed
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.AudioHardwareOn());
  EXPECT_TRUE(sink.Contains("ERROR frame "));
  EXPECT_TRUE(sink.Contains("the audio hardware state could not be saved"));
  EXPECT_TRUE(sink.Contains("NOTE audio hw rewind off:"));
  // Rollbacks go on with memory only, and nothing touches the audio hardware any more.
  EXPECT_GT(test.Stats().rollbacks, rollbacks + 10u);
  EXPECT_LE(audio.loads, loads + 2);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, FailedAudioLoadFallsBackToMemoryOnly)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  audio.fail_load = true;
  for (int i = 0; i < 150; ++i)
    game.Frame(test);
  EXPECT_FALSE(test.AudioHardwareOn());
  EXPECT_EQ(audio.loads, 1);  // tried once, then off
  EXPECT_EQ(audio.replaces, 0);
  EXPECT_TRUE(sink.Contains("was not consumed exactly on restore"));
  EXPECT_TRUE(sink.Contains("NOTE audio hw rewind off:"));
  EXPECT_GT(test.Stats().rollbacks, 20u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, UnusableAudioStateIsNotEnabled)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  audio.extra_padding = 5u << 20;  // 5 MB: more than the DSP / AI registers can be (a GameCube ARAM dump, say)
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  EXPECT_FALSE(test.AudioHardwareOn());
  EXPECT_TRUE(sink.Contains("ERROR audio hardware state is unusable at start"));

  FakeAudio failing(&game.Mem());
  failing.fail_save = true;
  test.SetAudioHardware(&failing);
  EXPECT_FALSE(test.AudioHardwareOn());

  // A state that grows past the limit while running switches the audio off at the capture.
  FakeAudio growing(&game.Mem());
  game.AttachAudio(&growing);
  test.SetAudioHardware(&growing);
  EXPECT_TRUE(test.AudioHardwareOn());
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  growing.extra_padding = 5u << 20;
  game.Frame(test);
  EXPECT_FALSE(test.AudioHardwareOn());
  EXPECT_TRUE(sink.Contains("the audio hardware state is "));
  for (int i = 0; i < 50; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, 5u);
}

TEST(Rev3Rollback, NoopModeAlsoRestoresTheAudioHardware)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTestOptions o = Options(2);
  o.noop = true;
  SyncTest test(o, game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  const SyncTestStats& s = test.Stats();
  EXPECT_GT(s.noop_restores, 100u);
  EXPECT_EQ(s.audio_restores, s.noop_restores);
  EXPECT_EQ(game.AudioWrong(), 0);  // same frame, same audio state: nothing changed by the round trip
  EXPECT_EQ(game.AudioRight(), static_cast<int>(s.noop_restores));
  EXPECT_FALSE(sink.Contains("ERROR"));
}

TEST(Rev3Rollback, AudioIsLeftAloneWhenTheRestoreIsRefused)
{
  FakeGame game(false);
  FakeAudio audio(&game.Mem());
  game.AttachAudio(&audio);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetAudioHardware(&audio);
  for (int i = 0; i < 70; ++i)
    game.Frame(test);
  while (test.Resimulating())
    game.Frame(test);
  const int loads = audio.loads;
  const Reply r = game.Frame(test, SP + 0x40);  // another sp: the restore is refused, memory and audio stay as they are
  EXPECT_EQ(r.flags, 0u);
  EXPECT_EQ(audio.loads, loads);
  EXPECT_EQ(audio.replaces, loads);
  EXPECT_TRUE(test.AudioHardwareOn());
}

// ---- determinism kit (milestone M1) -----------------------------------------------------------------------------------------

namespace
{
// An in-memory LineWriter: the lines, and how often it was flushed (and how many lines were written by then).
class FakeLines final : public LineWriter
{
public:
  void Line(const std::string& text) override { lines.push_back(text); }
  void Flush() override { flushed_at.push_back(lines.size()); }
  std::vector<std::string> lines;
  std::vector<size_t> flushed_at;  // lines.size() at each Flush
};

// The invented fighter pair of rev3-mod tests/test_rbchecksum.py: the same bytes at the same addresses.
constexpr u32 CS_A1 = 0x80500000;
constexpr u32 CS_A2 = 0x80510000;
constexpr size_t CS_FIGHTER_SIZE = 0x1800;
constexpr u32 CS_GOLDEN = 0xCDD50D8D;  // checksum(frame 300, pattern(1) as player 1, pattern(2) as player 2), frozen in test_rbchecksum.py
constexpr u32 CS_GOLDEN_FRAME = 300;

// pattern(seed) of test_rbchecksum.py: bytes((seed * 131 + i * 7 + (i >> 8) * 13) & 0xFF for i in range(0x1800)).
std::vector<u8> CsPattern(u32 seed)
{
  std::vector<u8> v(CS_FIGHTER_SIZE);
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<u8>((seed * 131 + i * 7 + (i >> 8) * 13) & 0xFF);
  return v;
}

// world() of test_rbchecksum.py: team pointers at 0x8038FE50 -> team struct (+0x0C -> node, node word 0 = fighter) for two teams.
void CsWorld(FakeMemory* m, u32 a1 = CS_A1, u32 a2 = CS_A2)
{
  const u32 addrs[2] = {a1, a2};
  for (u32 team = 0; team < 2; ++team)
  {
    const u32 t = 0x80400000 + 0x100 * team;
    const u32 node = 0x80401000 + 0x100 * team;
    m->SetU32(0x8038FE50 + 4 * team, t);
    m->SetU32(t + 0x0C, node);
    m->SetU32(node, addrs[team]);
    const std::vector<u8> pattern = CsPattern(team + 1);
    m->Write(addrs[team], pattern.data(), pattern.size());
  }
}

// The byte stream of rbchecksum.py's docstring built here, independent of FighterChecksum: u32 BE frame, per player u32 BE id and
// the 4 bytes at each offset of the table, in the table's order. `offsets` is rbchecksum.cpp_offsets() as it was when written.
const u16 CS_OFFSETS[61] = {
    0x140, 0x150, 0x158, 0x134, 0x50,  0x54,  0x58,  0x5C,  0x60,  0x64,  0x68,  0x6C,  0x70,  0x84,  0x88,  0x8C,
    0x9C,  0xA0,  0xC4,  0xC8,  0xCC,  0xE8,  0xEC,  0xF0,  0xF4,  0xF8,  0xFC,  0x100, 0x108, 0x10C, 0x110, 0x12C,
    0x24,  0x28,  0x44,  0x4C,  0x2C,  0x38,  0x9A4, 0x9D0, 0x9D4, 0x9DC, 0x9C0, 0x9B8, 0x9AC, 0x9B0, 0x1640, 0x1644,
    0x184, 0x178, 0x17C, 0x180, 0x3D8, 0x3E4, 0x1CC, 0x1D0, 0x13CC, 0x13D0, 0x13E4, 0x13E8, 0x13EC};

void PutBE32(std::vector<u8>* v, u32 x)
{
  v->push_back(u8(x >> 24));
  v->push_back(u8(x >> 16));
  v->push_back(u8(x >> 8));
  v->push_back(u8(x));
}

u32 HandBuiltChecksum(u32 frame, const std::vector<std::pair<u32, std::vector<u8>>>& players)
{
  std::vector<u8> stream;
  PutBE32(&stream, frame);
  for (const auto& [id, data] : players)
  {
    PutBE32(&stream, id);
    for (const u16 off : CS_OFFSETS)
      stream.insert(stream.end(), data.begin() + off, data.begin() + off + 4);
  }
  return Common::ComputeCRC32(stream.data(), stream.size());
}

PadBytes Pad(u8 seed)
{
  PadBytes p{};
  for (size_t i = 0; i < p.size(); ++i)
    p[i] = static_cast<u8>(seed + 17 * i);
  return p;
}

std::string TextOf(const std::vector<std::string>& lines)
{
  std::string s;
  for (const std::string& l : lines)
    s += l + "\n";
  return s;
}
}  // namespace

TEST(Rev3Rollback, ChecksumTableIsRbchecksumsTable)
{
  // rbchecksum.py: cpp_offsets() in FIELDS order, FIELDS_VERSION 1, fields_digest() = crc32("name:offset:size;" per field + "v1").
  ASSERT_EQ(FIELDS.size(), 61u);
  std::string digest_text;
  for (size_t i = 0; i < FIELDS.size(); ++i)
  {
    EXPECT_EQ(FIELDS[i].offset, CS_OFFSETS[i]) << FIELDS[i].name;
    digest_text += std::string(FIELDS[i].name) + ":" + std::to_string(FIELDS[i].offset) + ":4;";
  }
  digest_text += "v1";
  EXPECT_EQ(Common::ComputeCRC32(reinterpret_cast<const u8*>(digest_text.data()), digest_text.size()), 0xD4076EF2u);
}

TEST(Rev3Rollback, FighterChecksumGoldenValue)
{
  FakeMemory mem;
  CsWorld(&mem);
  u32 chk = 0;
  EXPECT_EQ(FighterChecksum(mem, CS_GOLDEN_FRAME, &chk), 2u);
  EXPECT_EQ(chk, CS_GOLDEN);
  // And the stream built by hand here gives the same number (test_equals_hand_built_stream).
  EXPECT_EQ(HandBuiltChecksum(CS_GOLDEN_FRAME, {{1, CsPattern(1)}, {2, CsPattern(2)}}), CS_GOLDEN);
}

TEST(Rev3Rollback, FighterChecksumWithOneOrNoFighter)
{
  u32 chk = 0;
  {
    FakeMemory mem;
    CsWorld(&mem);
    mem.SetU32(0x8038FE50 + 4, 0);  // team 1 gone: player 2 is skipped, not zero filled
    EXPECT_EQ(FighterChecksum(mem, 4, &chk), 1u);
    EXPECT_EQ(chk, 0x5B93E4F5u);  // rbchecksum.checksum(4, [(1, ...)]) of the same invented memory
    EXPECT_EQ(chk, HandBuiltChecksum(4, {{1, CsPattern(1)}}));
  }
  {
    FakeMemory mem;
    CsWorld(&mem);
    mem.SetU32(0x8038FE50, 0);  // team 0 gone: only player 2, id 2
    EXPECT_EQ(FighterChecksum(mem, 4, &chk), 1u);
    EXPECT_EQ(chk, 0x87810635u);  // rbchecksum.checksum(4, [(2, ...)])
  }
  {
    FakeMemory mem;  // a menu: nothing there
    EXPECT_EQ(FighterChecksum(mem, 7, &chk), 0u);
    EXPECT_EQ(chk, 0xBF204ABFu);  // rbchecksum.checksum(7, []) = crc32 of the frame alone
  }
}

TEST(Rev3Rollback, FighterChecksumSeesEveryHashedBitAndNothingElse)
{
  FakeMemory base;
  CsWorld(&base);
  u32 want = 0;
  FighterChecksum(base, 9, &want);
  // every bit of every one of the 61 fields of either player changes it
  for (const u32 fighter : {CS_A1, CS_A2})
  {
    for (const u16 off : CS_OFFSETS)
    {
      for (u32 bit = 0; bit < 32; ++bit)
      {
        FakeMemory m;
        CsWorld(&m);
        m.SetU32(fighter + off, m.U32(fighter + off) ^ (1u << bit));
        u32 got = 0;
        FighterChecksum(m, 9, &got);
        ASSERT_TRUE(got != want);
      }
    }
  }
  // the excluded fields (held, press, pressed, ground_y, the string route pointers) and unlisted bytes do not
  std::vector<u32> excluded = {0xB94, 0xB98, 0xBA0, 0xA20, 0x0, 0x4, 0x1000, 0x1700};
  for (u32 r = 0; r < 12; ++r)
    excluded.push_back(0xEC0 + 0x14 * r);
  for (const u32 off : excluded)
  {
    FakeMemory m;
    CsWorld(&m);
    m.SetU32(CS_A1 + off, ~m.U32(CS_A1 + off));
    m.SetU32(CS_A2 + off, ~m.U32(CS_A2 + off));
    u32 got = 0;
    FighterChecksum(m, 9, &got);
    EXPECT_EQ(got, want) << off;
  }
  // fighter addresses are not hashed, the frame and the player order are
  FakeMemory moved;
  CsWorld(&moved, 0x80600000, 0x80620000);
  u32 got = 0;
  FighterChecksum(moved, 9, &got);
  EXPECT_EQ(got, want);
  FighterChecksum(base, 10, &got);
  EXPECT_TRUE(got != want);
  FakeMemory swapped;
  CsWorld(&swapped);
  const std::vector<u8> first = CsPattern(1), second = CsPattern(2);
  swapped.Write(CS_A1, second.data(), second.size());  // player 1 now has player 2's data and the other way round
  swapped.Write(CS_A2, first.data(), first.size());
  FighterChecksum(swapped, 9, &got);
  EXPECT_TRUE(got != want);
}

TEST(Rev3Rollback, FighterChecksumHashesRawFloatBits)
{
  u32 plus_zero = 0, minus_zero = 0;
  FakeMemory a;
  CsWorld(&a);
  a.SetU32(CS_A1 + 0x84, 0x00000000);
  FighterChecksum(a, 1, &plus_zero);
  a.SetU32(CS_A1 + 0x84, 0x80000000);  // -0.0 == +0.0 as values, but a different fighter state
  FighterChecksum(a, 1, &minus_zero);
  EXPECT_TRUE(plus_zero != minus_zero);
}

TEST(Rev3Rollback, MemoryCrcIsZlibOverTheRangesInOrder)
{
  FakeMemory mem;
  const char digits[] = "123456789";
  mem.Write(0x80900000, digits, 9);
  EXPECT_EQ(MemoryCrc(mem, {{0x80900000, 0x80900009}}), 0xCBF43926u);  // the standard CRC-32 check value
  EXPECT_EQ(MemoryCrc(mem, {}), 0u);
  // two ranges = the CRC of their bytes back to back, in file order
  mem.Write(0x80900100, "ABC", 3);
  const u32 ab = MemoryCrc(mem, {{0x80900000, 0x80900004}, {0x80900100, 0x80900103}});
  const u8 joined[] = {'1', '2', '3', '4', 'A', 'B', 'C'};
  EXPECT_EQ(ab, Common::ComputeCRC32(joined, sizeof(joined)));
  EXPECT_TRUE(ab != MemoryCrc(mem, {{0x80900100, 0x80900103}, {0x80900000, 0x80900004}}));
  // without Peek the copy path gives the same number, including ranges bigger than one copy chunk
  mem.peek_enabled = false;
  EXPECT_EQ(MemoryCrc(mem, {{0x80900000, 0x80900004}, {0x80900100, 0x80900103}}), ab);
  const u32 big_direct = [&] {
    mem.peek_enabled = true;
    return MemoryCrc(mem, {{0x80A00000, 0x80C80000}});
  }();
  mem.peek_enabled = false;
  EXPECT_EQ(MemoryCrc(mem, {{0x80A00000, 0x80C80000}}), big_direct);
  // an unreadable range (outside MEM1 of the fake) is skipped and counted
  size_t unreadable = 0;
  EXPECT_EQ(MemoryCrc(mem, {{0x90000000, 0x90000010}}, &unreadable), 0u);
  EXPECT_EQ(unreadable, 1u);
}

TEST(Rev3Rollback, PadRecordLineFormat)
{
  std::array<PadBytes, 2> pads{};
  pads[0] = {0x12, 0x34, 0x00, 0xFF, 0x7F, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  pads[1] = {0xAB, 0xCD, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE};
  EXPECT_STREQ(FormatPadRecordLine(4000000000u, pads, 1).c_str(), "4000000000 123400ff7f80010203040506 abcdef0000000000000000fe 1");
  EXPECT_STREQ(FormatPadRecordLine(0, pads, -1).c_str(), "0 123400ff7f80010203040506 abcdef0000000000000000fe -1");
  EXPECT_STREQ(PAD_RECORD_HEADER, "# rev3 pad record v1");
}

TEST(Rev3Rollback, TraceLineFormat)
{
  EXPECT_STREQ(FormatTraceLine(300, 2, 0xCDD50D8D, 0x0000BEEF).c_str(), "T frame=300 fighters=2 chk=0xCDD50D8D mem=0x0000BEEF");
  EXPECT_STREQ(FormatTraceLine(0, 0, 0, 0).c_str(), "T frame=0 fighters=0 chk=0x00000000 mem=0x00000000");
}

TEST(Rev3Rollback, PadScriptParsing)
{
  const std::array<PadBytes, 2> p1 = {Pad(1), Pad(2)};
  const std::array<PadBytes, 2> p2 = {Pad(3), Pad(4)};
  // what the record writes: header, lines; plus CRLF, upper-case hex, a blank line, a comment, a line without the return field,
  // junk, and no newline after the last line
  std::string text = std::string(PAD_RECORD_HEADER) + "\n" + FormatPadRecordLine(10, p1, 0) + "\r\n\n# a comment\n";
  text += "garbage here\n";
  text += "12 0000000000000000000000AB 0000000000000000000000cd\n";
  text += "11 short 0\n";
  text += FormatPadRecordLine(20, p2, -7);
  PadScript script;
  std::string error;
  ASSERT_TRUE(ParsePadScript(text, &script, &error));
  EXPECT_EQ(script.frames.size(), 3u);
  EXPECT_EQ(script.first, 10u);
  EXPECT_EQ(script.last, 20u);
  EXPECT_EQ(script.ignored_lines, 2u);  // "garbage here" and "11 short 0"
  EXPECT_EQ(script.frames[10].pads, p1);
  EXPECT_EQ(script.frames[10].pad_return, 0);
  EXPECT_EQ(script.frames[20].pads, p2);
  EXPECT_EQ(script.frames[20].pad_return, -7);
  EXPECT_EQ(script.frames[12].pads[0][11], 0xABu);
  EXPECT_EQ(script.frames[12].pads[1][11], 0xCDu);
  EXPECT_EQ(script.frames[12].pad_return, 0);

  // a frame listed twice keeps the later line; the order of the lines does not matter for first / last
  PadScript twice;
  ASSERT_TRUE(ParsePadScript(FormatPadRecordLine(5, p1, 1) + "\n" + FormatPadRecordLine(3, p1, 2) + "\n" + FormatPadRecordLine(5, p2, 3) + "\n",
                             &twice, &error));
  EXPECT_EQ(twice.frames.size(), 2u);
  EXPECT_EQ(twice.first, 3u);
  EXPECT_EQ(twice.last, 5u);
  EXPECT_EQ(twice.frames[5].pads, p2);

  // nothing usable
  PadScript none;
  EXPECT_FALSE(ParsePadScript("", &none, &error));
  EXPECT_FALSE(ParsePadScript(std::string(PAD_RECORD_HEADER) + "\n# nothing else\n", &none, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(ParsePadScript("99999999999 " + std::string(24, '0') + " " + std::string(24, '0') + " 0\n", &none, &error));  // frame > u32
  EXPECT_FALSE(ParsePadScript("1 " + std::string(24, 'g') + " " + std::string(24, '0') + " 0\n", &none, &error));
}

namespace
{
// Kit setup for the tests: a fake memory with the invented fighters, options for record / script / trace.
struct KitRig
{
  KitRig() { CsWorld(&mem); }
  std::unique_ptr<DeterminismKit> Make()
  {
    return std::make_unique<DeterminismKit>(options, mem, sink, options.record_path.empty() ? nullptr : &record,
                                            options.trace_path.empty() ? nullptr : &trace);
  }
  Command Cmd(u32 frame) const
  {
    Command c;
    c.frame = frame;
    c.pads = {Pad(static_cast<u8>(frame)), Pad(static_cast<u8>(frame + 100))};
    c.pad_return = 1;
    return c;
  }
  FakeMemory mem;
  FakeSink sink;
  FakeLines record;
  FakeLines trace;
  DeterminismOptions options;
};
}  // namespace

TEST(Rev3Rollback, PadRecordWritesHeaderAndOneLinePerHookCall)
{
  KitRig rig;
  rig.options.record_path = "pads.txt";
  auto kit = rig.Make();
  EXPECT_TRUE(rig.sink.Contains("NOTE pad record -> pads.txt"));
  ASSERT_EQ(rig.record.lines.size(), 1u);
  EXPECT_STREQ(rig.record.lines[0].c_str(), "# rev3 pad record v1");

  for (u32 f = 100; f < 110; ++f)
  {
    Command c = rig.Cmd(f);
    const Command before = c;
    kit->OnCommand(&c, false);
    EXPECT_EQ(c.pads, before.pads);  // no script: the live pads are left alone
  }
  ASSERT_EQ(rig.record.lines.size(), 11u);
  EXPECT_STREQ(rig.record.lines[1].c_str(), FormatPadRecordLine(100, rig.Cmd(100).pads, 1).c_str());
  EXPECT_STREQ(rig.record.lines[10].c_str(), FormatPadRecordLine(109, rig.Cmd(109).pads, 1).c_str());

  // a resimulated call is not recorded
  Command c = rig.Cmd(105);
  kit->OnCommand(&c, true);
  EXPECT_EQ(rig.record.lines.size(), 11u);
  EXPECT_EQ(kit->RecordedLines(), 10u);

  // the file the record wrote is a valid script
  PadScript script;
  std::string error;
  ASSERT_TRUE(ParsePadScript(TextOf(rig.record.lines), &script, &error));
  EXPECT_EQ(script.frames.size(), 10u);
  EXPECT_EQ(script.frames[103].pads, rig.Cmd(103).pads);
  EXPECT_EQ(script.frames[103].pad_return, 1);
}

TEST(Rev3Rollback, PadRecordHeaderIsOptionalWhenAppending)
{
  KitRig rig;
  rig.options.record_path = "pads.txt";
  rig.options.record_header = false;
  auto kit = rig.Make();
  EXPECT_TRUE(rig.record.lines.empty());
}

TEST(Rev3Rollback, PadRecordFlushesEverySixtyLinesAndAtFinish)
{
  KitRig rig;
  rig.options.record_path = "pads.txt";
  auto kit = rig.Make();
  for (u32 f = 0; f < 59; ++f)
  {
    Command c = rig.Cmd(f);
    kit->OnCommand(&c, false);
  }
  EXPECT_TRUE(rig.record.flushed_at.empty());
  Command c = rig.Cmd(59);
  kit->OnCommand(&c, false);
  ASSERT_EQ(rig.record.flushed_at.size(), 1u);
  EXPECT_EQ(rig.record.flushed_at[0], 61u);  // header + 60 lines
  for (u32 f = 60; f < 150; ++f)
  {
    Command d = rig.Cmd(f);
    kit->OnCommand(&d, false);
  }
  EXPECT_EQ(rig.record.flushed_at.size(), 2u);  // 120 calls so far: the second flush
  kit->Finish();
  EXPECT_EQ(rig.record.flushed_at.size(), 3u);
  EXPECT_EQ(rig.record.flushed_at.back(), rig.record.lines.size());
}

TEST(Rev3Rollback, PadScriptReplacesThePadsOfListedFramesOnly)
{
  KitRig rig;
  const std::array<PadBytes, 2> scripted = {Pad(0x55), Pad(0x66)};
  std::string text = FormatPadRecordLine(10, scripted, 0) + "\n" + FormatPadRecordLine(12, scripted, -3) + "\n";
  std::string error;
  ASSERT_TRUE(ParsePadScript(text, &rig.options.script, &error));
  rig.options.script_path = "script.txt";
  auto kit = rig.Make();
  EXPECT_TRUE(rig.sink.Contains("NOTE pad script script.txt frames=2 first=10 last=12"));

  Command c = rig.Cmd(9);  // before the first listed frame: live
  kit->OnCommand(&c, false);
  EXPECT_EQ(c.pads, rig.Cmd(9).pads);
  c = rig.Cmd(10);
  kit->OnCommand(&c, false);
  EXPECT_EQ(c.pads, scripted);
  c = rig.Cmd(11);  // a hole in the script: live
  kit->OnCommand(&c, false);
  EXPECT_EQ(c.pads, rig.Cmd(11).pads);
  EXPECT_EQ(c.pad_return, 1);
  c = rig.Cmd(12);
  kit->OnCommand(&c, false);
  EXPECT_EQ(c.pads, scripted);
  EXPECT_EQ(c.pad_return, -3);
  EXPECT_FALSE(kit->ScriptEnded());  // the last listed frame itself has not passed yet
  EXPECT_FALSE(rig.sink.Contains("NOTE pad script ended"));

  c = rig.Cmd(13);  // after the end: live pads, and the note, once
  kit->OnCommand(&c, false);
  EXPECT_EQ(c.pads, rig.Cmd(13).pads);
  EXPECT_TRUE(kit->ScriptEnded());
  c = rig.Cmd(14);
  kit->OnCommand(&c, false);
  int notes = 0;
  for (const std::string& l : rig.sink.lines)
    notes += l == "NOTE pad script ended at frame 12";
  EXPECT_EQ(notes, 1);
}

TEST(Rev3Rollback, PadScriptEndIsAlsoNotedInTheTrace)
{
  KitRig rig;
  std::string error;
  ASSERT_TRUE(ParsePadScript(FormatPadRecordLine(10, {Pad(1), Pad(2)}, 0) + "\n", &rig.options.script, &error));
  rig.options.script_path = "s.txt";
  rig.options.trace_path = "t.txt";
  auto kit = rig.Make();
  for (u32 f = 9; f <= 11; ++f)
  {
    Command c = rig.Cmd(f);
    kit->OnCommand(&c, false);
  }
  bool noted = false;
  for (const std::string& l : rig.trace.lines)
    noted = noted || l == "NOTE pad script ended at frame 10";
  EXPECT_TRUE(noted);
  EXPECT_FALSE(rig.trace.flushed_at.empty());
}

TEST(Rev3Rollback, ScriptedPadsAreSubstitutedDuringResimulationToo)
{
  KitRig rig;
  const std::array<PadBytes, 2> scripted = {Pad(0x55), Pad(0x66)};
  std::string error;
  ASSERT_TRUE(ParsePadScript(FormatPadRecordLine(10, scripted, 0) + "\n" + FormatPadRecordLine(20, scripted, 0) + "\n", &rig.options.script,
                             &error));
  rig.options.script_path = "s.txt";
  rig.options.record_path = "r.txt";
  auto kit = rig.Make();
  Command c = rig.Cmd(10);
  kit->OnCommand(&c, true);
  EXPECT_EQ(c.pads, scripted);
  EXPECT_EQ(rig.record.lines.size(), 1u);  // only the header: nothing recorded in a resimulation
  // a resimulated call after the script's end does not end it either (only the first run of a frame counts)
  c = rig.Cmd(30);
  kit->OnCommand(&c, true);
  EXPECT_FALSE(kit->ScriptEnded());
}

TEST(Rev3Rollback, TraceWritesAChecksumLineEveryNthFrame)
{
  KitRig rig;
  rig.options.trace_path = "t.txt";
  rig.options.trace_every = 30;
  auto kit = rig.Make();
  EXPECT_TRUE(rig.sink.Contains("NOTE trace -> t.txt every=30 ranges=0 bytes=0"));
  for (u32 f = 295; f <= 331; ++f)
  {
    Command c = rig.Cmd(f);
    kit->OnCommand(&c, false);
  }
  ASSERT_EQ(rig.trace.lines.size(), 2u);  // frames 300 and 330
  EXPECT_STREQ(rig.trace.lines[0].c_str(), "T frame=300 fighters=2 chk=0xCDD50D8D mem=0x00000000");  // the golden value of rbchecksum
  u32 chk = 0;
  FighterChecksum(rig.mem, 330, &chk);
  EXPECT_STREQ(rig.trace.lines[1].c_str(), FormatTraceLine(330, 2, chk, 0).c_str());
  EXPECT_EQ(rig.trace.flushed_at.size(), 2u);  // flushed after every line
  EXPECT_EQ(kit->TraceLines(), 2u);

  // resimulated calls are not traced
  Command c = rig.Cmd(360);
  kit->OnCommand(&c, true);
  EXPECT_EQ(rig.trace.lines.size(), 2u);
}

TEST(Rev3Rollback, TraceReportsNoFightersAsZero)
{
  KitRig rig;
  rig.options.trace_path = "t.txt";
  rig.options.trace_every = 10;
  auto kit = rig.Make();
  rig.mem.SetU32(0x8038FE50, 0);  // only one fighter left: reported as none, checksum 0
  Command c = rig.Cmd(20);
  kit->OnCommand(&c, false);
  rig.mem.SetU32(0x8038FE50 + 4, 0);
  c = rig.Cmd(30);
  kit->OnCommand(&c, false);
  ASSERT_EQ(rig.trace.lines.size(), 2u);
  EXPECT_STREQ(rig.trace.lines[0].c_str(), "T frame=20 fighters=0 chk=0x00000000 mem=0x00000000");
  EXPECT_STREQ(rig.trace.lines[1].c_str(), "T frame=30 fighters=0 chk=0x00000000 mem=0x00000000");
}

TEST(Rev3Rollback, TraceMemIsTheCrcOfTheRangesAndFollowsTheMemory)
{
  KitRig rig;
  rig.options.trace_path = "t.txt";
  rig.options.trace_every = 1;
  ASSERT_TRUE(ParseWhitelist(R"({"ranges":[{"start":"0x80900000","end":"0x80900009"},{"start":2156921088,"end":2156921091}]})",
                             &rig.options.ranges, nullptr));
  rig.options.ranges_path = "r.json";
  rig.mem.Write(0x80900000, "123456789", 9);
  rig.mem.Write(0x80900100, "ABC", 3);
  auto kit = rig.Make();
  EXPECT_TRUE(rig.sink.Contains("NOTE trace -> t.txt every=1 ranges=2 bytes=12"));
  Command c = rig.Cmd(1);
  kit->OnCommand(&c, false);
  const u8 joined[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C'};
  u32 chk = 0;
  FighterChecksum(rig.mem, 1, &chk);
  EXPECT_STREQ(rig.trace.lines[0].c_str(), FormatTraceLine(1, 2, chk, Common::ComputeCRC32(joined, sizeof(joined))).c_str());
  rig.mem.Write(0x80900100, "ABD", 3);  // a changed byte shows in mem, not in the fighter checksum
  c = rig.Cmd(2);
  kit->OnCommand(&c, false);
  const u8 joined2[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'D'};
  EXPECT_STREQ(rig.trace.lines[1].c_str(), FormatTraceLine(2, 2, [&] { u32 x = 0; FighterChecksum(rig.mem, 2, &x); return x; }(),
                                                           Common::ComputeCRC32(joined2, sizeof(joined2))).c_str());
}

TEST(Rev3Rollback, TraceRangesJsonAcceptsHexStringsAndNumbers)
{
  std::vector<Range> ranges;
  std::string error;
  ASSERT_TRUE(ParseWhitelist(R"({"ranges":[{"start":"0x80000000","end":"0x80000100"},{"start":2415919104,"end":2415919360}]})", &ranges, &error));
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[1].start, 0x90000000u);
  EXPECT_EQ(ranges[1].end, 0x90000100u);
}

TEST(Rev3Rollback, KitWorksAlongsideSyncTestAndSeesEachFrameOnce)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());

  // a script for the frames 1000..1299 whose pads are not the game's own
  std::string text;
  for (u32 f = 1000; f < 1300; ++f)
    text += FormatPadRecordLine(f, {Pad(static_cast<u8>(f * 7)), Pad(static_cast<u8>(f * 11))}, 0) + "\n";
  FakeSink log;
  FakeLines record;
  FakeLines trace;
  DeterminismOptions options;
  std::string error;
  ASSERT_TRUE(ParsePadScript(text, &options.script, &error));
  options.script_path = "s.txt";
  options.record_path = "r.txt";
  options.trace_path = "t.txt";
  options.trace_every = 30;
  DeterminismKit kit(options, game.Mem(), log, &record, &trace);
  game.AttachKit(&kit);

  std::vector<u32> frames_with_pads_wrong;
  for (int i = 0; i < 1500; ++i)  // about 500 frames: every rollback costs K extra hook calls
  {
    const u32 frame = game.Counter();
    const Reply r = game.Frame(test);
    if (r.flags == 0 && frame >= 1000 && frame < 1300 && r.pads != std::array<PadBytes, 2>{Pad(static_cast<u8>(frame * 7)), Pad(static_cast<u8>(frame * 11))})
      frames_with_pads_wrong.push_back(frame);
  }
  while (test.Resimulating())
    game.Frame(test);

  EXPECT_TRUE(frames_with_pads_wrong.empty());   // SyncTest echoed the scripted pads, not the live ones
  EXPECT_GT(test.Stats().rollbacks, 100u);        // it was armed and rolling back all along
  EXPECT_EQ(test.Stats().mismatching_frames, 0u); // and the scripted run is as clean as a normal one
  EXPECT_FALSE(sink.Contains("MISMATCH"));

  // Every frame once in the record, in order, no matter how often SyncTest replayed it: header + one line per first run.
  ASSERT_GT(record.lines.size(), 100u);
  EXPECT_STREQ(record.lines[0].c_str(), PAD_RECORD_HEADER);
  for (size_t i = 1; i < record.lines.size(); ++i)
    EXPECT_EQ(std::stoul(record.lines[i].substr(0, record.lines[i].find(' '))), 1000u + (i - 1));
  // The script ended after frame 1299; the trace has lines at frames 1020, 1050, ... only
  EXPECT_TRUE(kit.ScriptEnded());
  EXPECT_TRUE(log.Contains("NOTE pad script ended at frame 1299"));
  u32 trace_lines = 0;
  for (const std::string& l : trace.lines)
  {
    if (l.rfind("T frame=", 0) != 0)
      continue;
    ++trace_lines;
    EXPECT_EQ(std::stoul(l.substr(8)) % 30, 0u);
  }
  EXPECT_EQ(trace_lines, kit.TraceLines());
  EXPECT_GT(trace_lines, 10u);
}

TEST(Rev3Rollback, ReplayingARecordGivesTheRecordRunsTrace)
{
  // The point of the kit: replay a record and get the record run's trace. Run A plays live pads and records them; run B has other
  // live pads but plays A's record as its script, and must write the same trace lines (chk and mem) and the same record.
  const auto run = [](u32 salt, const std::string& script, FakeLines* record, FakeLines* trace) {
    FakeMemory mem;
    CsWorld(&mem);
    FakeSink log;
    DeterminismOptions options;
    std::string error;
    if (!script.empty())
    {
      EXPECT_TRUE(ParsePadScript(script, &options.script, &error));
      options.script_path = "s.txt";
    }
    options.record_path = "r.txt";
    options.trace_path = "t.txt";
    options.trace_every = 10;
    EXPECT_TRUE(ParseWhitelist(R"({"ranges":[{"start":"0x80900000","end":"0x80900100"},{"start":"0x80910000","end":"0x80910040"}]})",
                               &options.ranges, &error));
    DeterminismKit kit(options, mem, log, record, trace);
    for (u32 frame = 0; frame < 300; ++frame)
    {
      Command c;
      c.frame = frame;
      c.pads = {Pad(static_cast<u8>(frame * 3 + salt)), Pad(static_cast<u8>(frame * 5 + salt))};
      c.pad_return = 1;
      kit.OnCommand(&c, false);
      // the "game": the pads the kit handed back feed the fighters' health and a counter in the traced memory
      mem.SetU32(CS_A1 + 0x28, mem.U32(CS_A1 + 0x28) * 31 + c.pads[0][0] + c.pads[0][3]);
      mem.SetU32(CS_A2 + 0x28, mem.U32(CS_A2 + 0x28) * 17 + c.pads[1][5]);
      mem.SetU32(0x80900000 + 4 * (frame % 64), mem.U32(0x80900000 + 4 * (frame % 64)) + c.pads[0][1]);
    }
    EXPECT_FALSE(kit.ScriptEnded());
  };
  FakeLines record_a, trace_a, record_b, trace_b, record_c, trace_c;
  run(1, "", &record_a, &trace_a);
  run(99, TextOf(record_a.lines), &record_b, &trace_b);
  run(2, "", &record_c, &trace_c);  // other pads, no script: a different game
  ASSERT_EQ(trace_a.lines.size(), 30u);
  EXPECT_EQ(trace_a.lines, trace_b.lines);
  EXPECT_EQ(record_a.lines, record_b.lines);  // re-recording a replay gives back the pads
  EXPECT_TRUE(trace_a.lines != trace_c.lines);
}

// ---- network mode (M3): the snapshot machinery without SyncTest's own state machine ---------------------------------------------------

namespace
{
// The fixed (non-adaptive) seed of a net test: the classic game ranges plus both fighter structs (NetPrepareArm refuses a set that does not
// hold all 61 fields; the adaptive set always does).
std::vector<Range> NetSeed()
{
  std::vector<Range> seed = FakeGame(false).Ranges();
  seed.push_back({FIGHTER[0], FIGHTER[0] + 0x1700});
  seed.push_back({FIGHTER[1], FIGHTER[1] + 0x1700});
  return seed;
}

// Drives SyncTest::Net* the way Rev3::Net::SyncTestStore does: one NetObserve per hook call, captures when the session says so, and the
// game's own frame (FakeGame::Step with the pads of that frame) in between.
struct NetRig
{
  explicit NetRig(const SyncTestOptions& options, const std::vector<Range>& seed = {}, bool leak = false)
      : game(leak), test(options, seed, game.Mem(), sink, FakeClock())
  {
  }
  Command Cmd(u32 frame) const
  {
    Command c;
    c.frame = frame;
    c.pads = FakeGame::PadsFor(frame);
    c.sp = SP;
    return c;
  }
  void Observe(u32 frame, bool resimulating = false)
  {
    game.Mem().SetU32(SP, frame);  // the hook's stack frame holds its frame counter, as in FakeGame::Frame
    test.NetObserve(Cmd(frame), resimulating);
  }
  bool Capture(u32 frame)
  {
    SyncTest::NetSnapshotInfo info;
    const bool ok = test.NetCapture(frame, SP, &info);
    if (ok)
      infos[frame] = info;
    return ok;
  }
  void Advance(u32 frame, bool late = false)
  {
    game.Step(FakeGame::PadsFor(frame));
    if (late)  // from now on a counter in a page nothing seeds feeds p1's combo_count (FakeGame::SetLatePage does this inside Frame)
    {
      const u32 v = game.Mem().U32(LATE_PAGE);
      game.Mem().SetU32(LATE_PAGE, v + 1);
      game.Mem().SetU32(FIGHTER[0] + FIELD_COMBO_COUNT, v);
    }
  }
  FakeGame game;
  FakeSink sink;
  SyncTest test;
  std::map<u32, SyncTest::NetSnapshotInfo> infos;
};
}  // namespace

TEST(Rev3Rollback, NetModeCapturesAndRestoresFramesInARing)
{
  NetRig rig(Options(6), NetSeed());  // k = MAXR + 1 = 6: MAXR + 2 = 7 slots
  EXPECT_EQ(rig.test.NetSlotCount(), 7u);
  EXPECT_FALSE(rig.test.NetPrepareArm());  // no NetObserve yet: no fighters known
  EXPECT_FALSE(rig.Capture(1000));
  EXPECT_TRUE(rig.sink.Contains("ERROR"));

  for (u32 f = 1000; f < 1010; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(1010);
  EXPECT_TRUE(rig.test.NetFightersFound());
  EXPECT_EQ(rig.test.NetFighters()[0], FIGHTER[0]);
  EXPECT_EQ(rig.test.NetFighters()[1], FIGHTER[1]);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_TRUE(rig.sink.Contains("NOTE net snapshot set: ranges="));
  EXPECT_TRUE(rig.sink.Contains("in_snapshot=61/61"));

  std::map<u32, std::array<u32, 3>> state;  // the game words at the moment of each capture
  std::map<u32, u32> checksum;
  for (u32 f = 1010; f <= 1020; ++f)
  {
    if (f != 1010)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f)) << f;
    state[f] = {rig.game.Mem().U32(STATE_A), rig.game.Mem().U32(STATE_A + 0x80), rig.game.Mem().U32(STATE_B + 4)};
    u32 chk = 0;
    ASSERT_EQ(FighterChecksum(rig.game.Mem(), f, &chk), 2u);
    checksum[f] = chk;
    rig.Advance(f);
  }

  // The capture reports the fighters' fields: the same numbers the live checksum is made of.
  for (u32 f = 1010; f <= 1020; ++f)
    EXPECT_EQ(Net::ChecksumOfFields(static_cast<s32>(f), Net::FieldsFromRaw(rig.infos[f].fields, 3)), checksum[f]) << f;

  // 7 slots: the newest 7 frames are there, older ones were overwritten.
  for (u32 f = 1010; f <= 1013; ++f)
    EXPECT_FALSE(rig.test.NetHas(f)) << f;
  for (u32 f = 1014; f <= 1020; ++f)
  {
    EXPECT_TRUE(rig.test.NetHas(f)) << f;
    EXPECT_EQ(rig.test.NetSlotSp(f), SP);
  }
  EXPECT_EQ(rig.test.NetSlotSp(1013), 0u);
  EXPECT_FALSE(rig.test.NetHas(1021));

  // Restore: the game words and the stack frame of that call come back.
  ASSERT_TRUE(rig.test.NetRestore(1016));
  EXPECT_EQ(rig.game.Mem().U32(STATE_A), state[1016][0]);
  EXPECT_EQ(rig.game.Mem().U32(STATE_A + 0x80), state[1016][1]);
  EXPECT_EQ(rig.game.Mem().U32(STATE_B + 4), state[1016][2]);
  EXPECT_EQ(rig.game.Mem().U32(SP), 1016u);
  u32 chk = 0;
  FighterChecksum(rig.game.Mem(), 1016, &chk);
  EXPECT_EQ(chk, checksum[1016]);
  EXPECT_NE(rig.game.Mem().U32(STATE_A), state[1020][0]);

  rig.test.NetClear();
  for (u32 f = 1010; f <= 1020; ++f)
    EXPECT_FALSE(rig.test.NetHas(f)) << f;
  EXPECT_EQ(rig.test.NetSlotSp(1016), 0u);
  EXPECT_FALSE(rig.test.NetLayoutPending());
}

TEST(Rev3Rollback, NetModeLosingTheFightersForgetsTheSet)
{
  NetRig rig(AdaptiveOptions(6));
  rig.Observe(1000);
  EXPECT_TRUE(rig.test.NetFightersFound());
  rig.game.Mem().SetU32(0x8038FE50, 0);  // team 0 gone: a menu / loading
  rig.Observe(1001);
  EXPECT_FALSE(rig.test.NetFightersFound());
  EXPECT_FALSE(rig.test.NetPrepareArm());
  EXPECT_FALSE(rig.Capture(1001));
  EXPECT_FALSE(rig.test.LearningDone());  // the tracker starts again when the fighters come back
}

TEST(Rev3Rollback, NetModeLearnedPagesAreDeferredUntilTheNextClear)
{
  NetRig rig(AdaptiveOptions(6));
  u32 f = 1000;
  for (; f < 3000 && !rig.test.LearningDone(); ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.LearningDone());
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  const std::vector<Range> installed = rig.test.Ranges();
  EXPECT_FALSE(SetContains(installed, LATE_PAGE));

  // Slots are in use; from now on the game changes a page nobody seeded. The tracker finds it, the set must NOT change under the slots.
  bool pending = false;
  for (int i = 0; i < 600 && !pending; ++i, ++f)
  {
    if (i != 0)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f, true);
    pending = rig.test.NetLayoutPending();
  }
  ASSERT_TRUE(pending);
  EXPECT_TRUE(rig.sink.Contains("while slots are in use"));
  EXPECT_FALSE(SetContains(rig.test.Ranges(), LATE_PAGE));
  EXPECT_EQ(rig.test.Ranges().size(), installed.size());
  EXPECT_GE(rig.test.Stats().pages_learned_armed, 1u);
  rig.Observe(f);
  EXPECT_TRUE(rig.Capture(f));  // still working with the old layout
  EXPECT_TRUE(rig.test.NetRestore(f));

  rig.test.NetClear();  // the session drained: the new set is installed with the slots empty
  EXPECT_TRUE(SetContains(rig.test.Ranges(), LATE_PAGE));
  EXPECT_FALSE(rig.test.NetLayoutPending());
  EXPECT_FALSE(rig.test.NetHas(f));
  EXPECT_TRUE(rig.sink.Contains("NOTE net snapshot set now ranges="));
  rig.Observe(f + 1);
  EXPECT_TRUE(rig.Capture(f + 1));
  EXPECT_TRUE(rig.test.NetRestore(f + 1));
}

TEST(Rev3Rollback, NetPrepareArmForcesTheEfPoolAndLogsIt)
{
  NetRig rig(AdaptiveOptions(6));
  FakeEf ef(rig.game.Mem());
  ef.BuildChain(3);
  u32 f = 1000;
  for (int i = 0; i < 330; ++i, ++f)  // a learning phase in which nothing in the ef pool ever changes
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_TRUE(rig.test.EfPoolFound());
  EXPECT_TRUE(rig.sink.Contains("NOTE ef pool 0x80B00000-"));
  EXPECT_FALSE(rig.sink.Contains("NOTE ef pool not found"));
  ASSERT_FALSE(rig.test.EfPoolInfo().ranges.empty());
  for (const Range& r : rig.test.EfPoolInfo().ranges)
    EXPECT_TRUE(SetContains(rig.test.Ranges(), r.start, r.end - r.start)) << std::hex << r.start;
  EXPECT_TRUE(SetContains(rig.test.Ranges(), 0x80B00040 + 0x20000));  // a heap page nothing wrote: only the forced range has it
}

TEST(Rev3Rollback, NetPrepareArmSaysWhyTheEfPoolIsMissingAndOffSwitchWorks)
{
  {
    NetRig rig(AdaptiveOptions(6));  // no EffectSystem in this memory
    for (u32 f = 1000; f < 1330; ++f)
    {
      rig.Observe(f);
      rig.Advance(f);
    }
    ASSERT_TRUE(rig.test.NetPrepareArm());
    EXPECT_FALSE(rig.test.EfPoolFound());
    EXPECT_TRUE(rig.sink.Contains("NOTE ef pool not found: "));
  }
  {
    SyncTestOptions o = AdaptiveOptions(6);
    o.ef_pool = false;
    NetRig rig(o);
    FakeEf ef(rig.game.Mem());
    for (u32 f = 1000; f < 1330; ++f)
    {
      rig.Observe(f);
      rig.Advance(f);
    }
    ASSERT_TRUE(rig.test.NetPrepareArm());
    EXPECT_TRUE(rig.sink.Contains("NOTE ef pool off"));
    EXPECT_FALSE(SetContains(rig.test.Ranges(), 0x80B00040 + 0x20000));
  }
}

TEST(Rev3Rollback, NetModeRestoreWithAPendingSetIsCountedAndTheStatsLineCarriesTheEfCounters)
{
  NetRig rig(AdaptiveOptions(6));
  u32 f = 1000;
  for (int i = 0; i < 330; ++i, ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  rig.game.Mem().SetU32(LATE_PAGE, 1);
  bool pending = false;
  for (int i = 0; i < 600 && !pending; ++i, ++f)
  {
    if (i != 0)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f, true);
    pending = rig.test.NetLayoutPending();
  }
  ASSERT_TRUE(pending);
  rig.Observe(f);
  ASSERT_TRUE(rig.Capture(f));
  ASSERT_TRUE(rig.test.NetRestore(f));
  EXPECT_EQ(rig.test.Stats().late_learn_rollbacks, 1u);
  const std::string line = rig.test.NetStatsLine();
  EXPECT_NE(line.find("restores_with_pending_set=1"), std::string::npos) << line;
  EXPECT_NE(line.find("ef_bad=0"), std::string::npos) << line;
  EXPECT_NE(line.find("layout_refused=0"), std::string::npos) << line;
  rig.test.NetClear();
  rig.Observe(f + 1);
  ASSERT_TRUE(rig.Capture(f + 1));
  const u32 before = rig.test.Stats().late_learn_rollbacks;
  ASSERT_TRUE(rig.test.NetRestore(f + 1));
  EXPECT_EQ(rig.test.Stats().late_learn_rollbacks, before);  // the set is current again: sound
}

TEST(Rev3Rollback, NetModeResimulatedCallsAreNotScanned)
{
  NetRig rig(AdaptiveOptions(6));
  for (u32 f = 1000; f < 1100; ++f)
  {
    rig.Observe(f, true);  // every call a replay: nothing may be hashed
    rig.Advance(f);
  }
  EXPECT_EQ(rig.test.Stats().scans, 0u);
  EXPECT_EQ(rig.test.LearnCycles(), 0u);
  rig.Observe(1100);
  EXPECT_GT(rig.test.Stats().scans, 0u);
}

TEST(Rev3Rollback, NetModeStatsLineIsOneNoteLine)
{
  NetRig rig(Options(6), NetSeed());
  for (u32 f = 1000; f < 1003; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  rig.Capture(1003);
  rig.Advance(1003);
  rig.Observe(1004);
  rig.Capture(1004);
  ASSERT_TRUE(rig.test.NetRestore(1003));
  const std::string line = rig.test.NetStatsLine();
  EXPECT_EQ(line.rfind("NOTE snapshot stats restores=1 ", 0), 0u) << line;
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(Rev3Rollback, NetModeRestoreLinesAreLimitedThenSummarisedPerWindow)
{
  SyncTestOptions options = Options(6);
  options.restore_log_full = 3;    // netplay policy: 3 result lines in full ...
  options.restore_log_window = 10; // ... then one `RESTORE summary` per 10 frames
  NetRig rig(options, NetSeed());
  for (u32 f = 1000; f < 1003; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  for (u32 f = 1003; f < 1043; ++f)  // 40 restores, one per frame
  {
    rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    ASSERT_TRUE(rig.test.NetRestore(f));
    rig.Advance(f);
  }
  EXPECT_EQ(rig.test.Stats().restores, 40u);
  EXPECT_EQ(rig.sink.Count("RESTORE 10"), 6u);  // 3 x (begin + result line)
  EXPECT_EQ(rig.sink.Count(" begin"), 3u);
  EXPECT_GE(rig.sink.Count("RESTORE summary frames="), 3u);
  rig.test.NetFlushLog();
  // Every restore past the third is in exactly one summary.
  u32 total = 0;
  for (const std::string& line : rig.sink.lines)
  {
    const size_t at = line.find("RESTORE summary ");
    if (at == std::string::npos)
      continue;
    unsigned n = 0;
    ASSERT_EQ(sscanf(line.c_str() + line.find(" n="), " n=%u", &n), 1) << line;
    EXPECT_LE(n, 10u) << line;
    total += n;
  }
  EXPECT_EQ(total, 37u);
  const size_t before = rig.sink.lines.size();
  rig.test.NetFlushLog();  // nothing pending: no empty summary
  EXPECT_EQ(rig.sink.lines.size(), before);
}

TEST(Rev3Rollback, TheClassicRestorePolicyIsUnchangedWithoutTheNetOption)
{
  NetRig rig(Options(6), NetSeed());
  for (u32 f = 1000; f < 1003; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  for (u32 f = 1003; f < 1053; ++f)
  {
    rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    ASSERT_TRUE(rig.test.NetRestore(f));
    rig.Advance(f);
  }
  EXPECT_EQ(rig.sink.Count(" begin"), 10u);        // the first 10 in full
  EXPECT_EQ(rig.sink.Count("RESTORE summary"), 0u);
}

TEST(Rev3Rollback, NetRangeCrcsReportEachRangeAsItWasAtTheCaptureOrAsItIsNow)
{
  NetRig rig(Options(6), NetSeed());
  for (u32 f = 1000; f < 1003; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  ASSERT_TRUE(rig.test.NetPrepareArm());
  rig.Observe(1003);
  ASSERT_TRUE(rig.Capture(1003));

  std::vector<RangeHash> slot, live;
  ASSERT_TRUE(rig.test.NetRangeCrcs(1003, false, &slot));
  ASSERT_EQ(slot.size(), rig.test.Ranges().size());
  for (size_t i = 0; i < slot.size(); ++i)
  {
    EXPECT_EQ(slot[i].start, rig.test.Ranges()[i].start);
    EXPECT_EQ(slot[i].end, rig.test.Ranges()[i].end);
  }
  ASSERT_TRUE(rig.test.NetRangeCrcs(1999, true, &live));  // a frame without a slot: the live memory (nothing has changed since the capture)
  EXPECT_EQ(live.size(), slot.size());
  for (size_t i = 0; i < slot.size(); ++i)
    EXPECT_EQ(live[i].crc, slot[i].crc) << i;
  EXPECT_FALSE(rig.test.NetRangeCrcs(1999, false, &live));  // no slot and not asked for the live state

  rig.Advance(1003);  // the game moves on: the slot keeps the old bytes, the live memory changed
  std::vector<RangeHash> slot_after, live_after;
  ASSERT_TRUE(rig.test.NetRangeCrcs(1003, false, &slot_after));
  ASSERT_TRUE(rig.test.NetRangeCrcs(1999, true, &live_after));
  for (size_t i = 0; i < slot.size(); ++i)
    EXPECT_EQ(slot_after[i].crc, slot[i].crc) << i;
  size_t changed = 0;
  for (size_t i = 0; i < slot.size(); ++i)
    changed += live_after[i].crc != slot[i].crc ? 1 : 0;
  EXPECT_GT(changed, 0u);
}

TEST(Rev3Rollback, ReadFighterFieldsIsWhatTheChecksumIsMadeOf)
{
  FakeMemory mem;
  CsWorld(&mem);
  std::array<std::array<u8, FIELD_BYTES>, 2> raw{};
  u8 present = 0;
  EXPECT_EQ(ReadFighterFields(mem, &raw, &present), 2u);
  EXPECT_EQ(present, 3);
  EXPECT_EQ(Net::ChecksumOfFields(static_cast<s32>(CS_GOLDEN_FRAME), Net::FieldsFromRaw(raw, present)), CS_GOLDEN);
  // The raw bytes are the big-endian words at the table's offsets.
  const std::vector<u8> pattern = CsPattern(1);
  for (size_t i = 0; i < FIELDS.size(); ++i)
    EXPECT_EQ(0, std::memcmp(raw[0].data() + 4 * i, pattern.data() + FIELDS[i].offset, 4)) << FIELDS[i].name;

  mem.SetU32(0x8038FE50, 0);  // player 1 gone: its array stays zero, only bit 1 is set
  EXPECT_EQ(ReadFighterFields(mem, &raw, &present), 1u);
  EXPECT_EQ(present, 2);
  EXPECT_EQ(raw[0], (std::array<u8, FIELD_BYTES>{}));
  u32 chk = 0;
  FighterChecksum(mem, 9, &chk);
  EXPECT_EQ(Net::ChecksumOfFields(9, Net::FieldsFromRaw(raw, present)), chk);

  FakeMemory empty;
  EXPECT_EQ(ReadFighterFields(empty, &raw, &present), 0u);
  EXPECT_EQ(present, 0);
}

TEST(Rev3Rollback, PadRecordCanRecordWhatTheGameActuallyGot)
{
  // Netplay: the pads in the command are the local pad (port 0) and junk; the pads the game gets are in the REPLY. With record_reply the kit
  // writes the reply's pads (and the command's pad_return), once per frame, not for restored replies or replayed frames.
  KitRig rig;
  rig.options.record_path = "pads.txt";
  rig.options.record_reply = true;
  auto kit = rig.Make();
  Reply normal;
  normal.pads = {Pad(7), Pad(9)};
  for (u32 f = 10; f < 14; ++f)
  {
    Command c = rig.Cmd(f);
    kit->OnCommand(&c, false);
    kit->OnReply(c, normal, false);
  }
  Command c = rig.Cmd(14);
  kit->OnCommand(&c, false);
  Reply restored;
  restored.flags = REPLY_FLAG_RESTORED;
  restored.frame = 12;
  restored.pads = {Pad(1), Pad(2)};
  kit->OnReply(c, restored, false);  // not recorded: the hook continues from frame 12
  Command replay = rig.Cmd(12);
  kit->OnCommand(&replay, true);
  kit->OnReply(replay, normal, true);  // not recorded: a replayed frame
  ASSERT_EQ(rig.record.lines.size(), 5u);  // header + frames 10..13
  EXPECT_STREQ(rig.record.lines[1].c_str(), FormatPadRecordLine(10, normal.pads, 1).c_str());
  EXPECT_STREQ(rig.record.lines[4].c_str(), FormatPadRecordLine(13, normal.pads, 1).c_str());
  EXPECT_EQ(kit->RecordedLines(), 4u);
}

// ---- freeze dump (REV3_SYNCTEST_DUMP) -----------------------------------------------------------------------------------------

namespace
{
// A DumpSource with settable registers and hardware.
class FakeDumpSource final : public DumpSource
{
public:
  bool ReadCpu(DumpCpu* out) override
  {
    *out = cpu;
    out->valid = cpu_ok;
    return cpu_ok;
  }
  bool ReadHw(DumpHw* out) override
  {
    ++hw_reads;
    *out = hw;
    return hw_ok;
  }
  void AudioLines(std::vector<std::string>* out) override
  {
    ++audio_calls;
    out->push_back("dsp csr=0x0004 fake");
  }
  void ExtraLines(std::vector<std::string>* out, bool racy) override { out->push_back(racy ? "hw di fake racy" : "hw di fake"); }
  DumpCpu cpu;
  bool cpu_ok = true;
  DumpHw hw;
  bool hw_ok = true;
  int hw_reads = 0;
  int audio_calls = 0;
};

// A millisecond clock the test advances by hand.
struct FakeMs
{
  std::shared_ptr<u64> now = std::make_shared<u64>(1000);
  std::function<u64()> Fn() const
  {
    auto n = now;
    return [n]() { return *n; };
  }
};

constexpr u32 T1 = 0x80600000;  // running thread
constexpr u32 T2 = 0x80601000;  // waiting in the GX draw-done queue
constexpr u32 T3 = 0x80602000;  // waiting in the VI retrace queue, stack canary smashed
constexpr u32 OS_ACTIVE = 0x800000DC;
constexpr u32 OS_CURRENT = 0x800000E4;
constexpr u32 Q_DRAWDONE = 0x8049D950;
constexpr u32 Q_RETRACE = 0x8049D4B8;
constexpr u32 STACK_T1 = 0x80610000;

// An OSThread with the fields the dump reads.
void MakeThread(FakeMemory& mem, u32 t, u16 state, s32 prio, u32 queue, u32 pc, u32 lr, u32 sp, u32 stack_end, u32 stack_base, bool canary,
                u32 next_active)
{
  mem.Write(t + 0x2C8, std::array<u8, 2>{u8(state >> 8), u8(state)}.data(), 2);
  mem.SetU32(t + 0x2CC, 0);
  mem.SetU32(t + 0x2D0, static_cast<u32>(prio));
  mem.SetU32(t + 0x2DC, queue);
  mem.SetU32(t + 0x2E0, 0);  // link.next in the sleep queue
  mem.SetU32(t + 0x198, pc);
  mem.SetU32(t + 0x84, lr);
  mem.SetU32(t + 0x04, sp);
  mem.SetU32(t + 0x2FC, next_active);
  mem.SetU32(t + 0x304, stack_base);
  mem.SetU32(t + 0x308, stack_end);
  mem.SetU32(stack_end, canary ? 0xDEADBABE : 0x12345678);
}

// A stack: frames[i] = {sp, lr}; each frame's back-chain word points to the next one, the last one to 0.
void MakeStack(FakeMemory& mem, const std::vector<std::pair<u32, u32>>& frames)
{
  for (size_t i = 0; i < frames.size(); ++i)
  {
    mem.SetU32(frames[i].first, i + 1 < frames.size() ? frames[i + 1].first : 0);
    mem.SetU32(frames[i].first + 4, frames[i].second);
  }
}

// Three active threads (T1 running), a waiter in two sleep queues, the SDK flags and the IPC queue.
void MakeFakeOs(FakeMemory& mem)
{
  mem.SetU32(OS_ACTIVE, T1);
  mem.SetU32(OS_CURRENT, T1);
  mem.SetU32(0x800000D4, T1);  // current context
  mem.SetU32(0x800000D8, T1);  // FPU owner
  MakeThread(mem, T1, 2, 16, 0, 0xAAAA0000, 0xAAAA0004, 0x80611FF0, STACK_T1, STACK_T1 + 0x2000, true, T2);
  MakeThread(mem, T2, 4, 20, Q_DRAWDONE, 0x80123450, 0x80123460, 0x80611800, STACK_T1 + 0x4000, STACK_T1 + 0x6000, true, T3);
  MakeThread(mem, T3, 4, 24, Q_RETRACE, 0x80345670, 0x80345680, 0x80612800, STACK_T1 + 0x8000, STACK_T1 + 0xA000, false, 0);
  MakeStack(mem, {{0x80611800, 0x80111110}, {0x80611840, 0x80222220}, {0x80611900, 0x80333330}});
  mem.SetU32(Q_DRAWDONE, T2);
  mem.SetU32(Q_DRAWDONE + 4, T2);
  mem.SetU32(Q_RETRACE, T3);
  mem.SetU32(Q_RETRACE + 4, T3);
  const u8 flag = 1;
  mem.Write(0x8049D958, &flag, 1);
  mem.SetU32(0x8049D4DC, 0x1234);
  mem.SetU32(0x8049CF48, 0x1230);
  mem.SetU32(0x8049DAB8, 1);  // scheduler disabled
  mem.SetU32(0x8049DAC0, 0x00010000);
  mem.SetU32(0x8049D984, 0x80620000);  // IPC object; its queue at +0x2C is empty
  mem.SetU32(0x803C19C0, 0x00000003);
  mem.SetU32(0x803C11D4, 7);
  mem.SetU32(0x803C1A70, 0x01000000);
}

DumpCpu MakeFakeCpu()
{
  DumpCpu cpu;
  cpu.valid = true;
  cpu.pc = 0x80001000;
  cpu.npc = 0x80001004;
  cpu.lr = 0x80002000;
  cpu.ctr = 0x80003000;
  cpu.msr = 0x0000A032;
  cpu.srr0 = 0x80004000;
  cpu.srr1 = 0x0000B032;
  cpu.cr = 0x28000042;
  cpu.exceptions = 4;
  for (u32 i = 0; i < 32; ++i)
    cpu.gpr[i] = 0x1000 + i;
  cpu.gpr[1] = 0x80611F00;
  cpu.gpr[2] = 0x80500002;
  cpu.gpr[13] = 0x80500013;
  return cpu;
}

bool HasLine(const std::vector<std::string>& lines, const std::string& needle)
{
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

}  // namespace

TEST(Rev3Rollback, BackChainFollowsTheSavedSpAndLr)
{
  FakeMemory mem;
  MakeStack(mem, {{0x80500000, 0x80001000}, {0x80500040, 0x80002000}, {0x80500100, 0x80003000}});
  std::vector<StackFrame> frames;
  ASSERT_EQ(WalkBackChain(mem, 0x80500000, 16, &frames), 3u);
  EXPECT_EQ(frames[0].sp, 0x80500000u);
  EXPECT_EQ(frames[0].lr, 0x80001000u);
  EXPECT_EQ(frames[1].sp, 0x80500040u);
  EXPECT_EQ(frames[1].lr, 0x80002000u);
  EXPECT_EQ(frames[2].sp, 0x80500100u);  // the last frame (back-chain 0) is still listed
  EXPECT_EQ(frames[2].lr, 0x80003000u);
  EXPECT_EQ(WalkBackChain(mem, 0x80500000, 2, &frames), 2u);  // limited
  EXPECT_EQ(WalkBackChain(mem, 0x80500000, 0, &frames), 0u);
  // A back-chain that does not increase stops the walk after the frame that points back.
  mem.SetU32(0x80500100, 0x80500040);
  EXPECT_EQ(WalkBackChain(mem, 0x80500000, 16, &frames), 3u);
  mem.SetU32(0x80500100, 0x80500100);  // points at itself
  EXPECT_EQ(WalkBackChain(mem, 0x80500000, 16, &frames), 3u);
  // Zero, unaligned and unreadable stack pointers give nothing.
  EXPECT_EQ(WalkBackChain(mem, 0, 16, &frames), 0u);
  EXPECT_EQ(WalkBackChain(mem, 0x80500002, 16, &frames), 0u);
  EXPECT_EQ(WalkBackChain(mem, 0x90000000, 16, &frames), 0u);  // MEM2 does not exist in the fake
  EXPECT_EQ(WalkBackChain(mem, 0x817FFFFC, 16, &frames), 0u);  // only 4 of the 8 bytes are readable
  EXPECT_EQ(WalkBackChain(mem, 0x80000000, 16, &frames), 1u);  // a readable frame of zeros: listed, then the chain ends
  // A next sp that is unreadable ends the walk after the frame that holds it.
  mem.SetU32(0x80500100, 0x90001000);
  EXPECT_EQ(WalkBackChain(mem, 0x80500000, 16, &frames), 3u);
}

TEST(Rev3Rollback, FreezeDumpListsRegistersAndTheBackChain)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  MakeStack(mem, {{0x80611F00, 0x80AAAAA0}, {0x80611F80, 0x80BBBBB0}});
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  FreezeDump dump(mem, sink, src);
  const std::vector<std::string> lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "ppc pc=0x80001000 npc=0x80001004 lr=0x80002000 ctr=0x80003000 msr=0x0000A032 srr0=0x80004000 srr1=0x0000B032 "
                             "cr=0x28000042 r1=0x80611F00 r2=0x80500002 r13=0x80500013"));
  EXPECT_TRUE(HasLine(lines, "ppc r3=0x00001003 r4=0x00001004 r5=0x00001005 r6=0x00001006 r7=0x00001007 r8=0x00001008 r9=0x00001009 "
                             "r10=0x0000100A r11=0x0000100B r12=0x0000100C exceptions=0x00000004"));
  EXPECT_TRUE(HasLine(lines, "frame 0 sp=0x80611F00 lr=0x80AAAAA0"));
  EXPECT_TRUE(HasLine(lines, "frame 1 sp=0x80611F80 lr=0x80BBBBB0"));
  EXPECT_EQ(src.audio_calls, 0);  // not a full dump
  // The registers are unavailable: one line says so, the rest of the dump carries on.
  src.cpu_ok = false;
  const std::vector<std::string> no_cpu = dump.Lines(false);
  EXPECT_TRUE(HasLine(no_cpu, "ppc unavailable"));
  EXPECT_TRUE(HasLine(no_cpu, "os cur_thread=0x80600000"));
}

TEST(Rev3Rollback, FreezeDumpWalksTheThreadsAndUsesLiveRegistersForTheRunningOne)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  FreezeDump dump(mem, sink, src);
  const std::vector<std::string> lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "os cur_thread=0x80600000 cur_context=0x80600000 fpu_owner=0x80600000 sched_disable=0x00000001 "
                             "run_queue_bits=0x00010000 resched=0x00000000 active_head=0x80600000"));
  EXPECT_TRUE(HasLine(lines, "threads n=3 end=null"));
  // Running thread: pc / lr / sp from the live registers, not from its stale context (0xAAAA0000).
  EXPECT_TRUE(HasLine(lines, "thread 0x80600000 state=2 prio=16 suspend=0 queue=0x00000000 pc=0x80001000 lr=0x80002000 sp=0x80611F00 "
                             "canary=ok stack=0x80610000..0x80612000 running=1 live=1"));
  EXPECT_FALSE(HasLine(lines, "0xAAAA0000"));
  // A waiting thread: the saved context, queue and canary.
  EXPECT_TRUE(HasLine(lines, "thread 0x80601000 state=4 prio=20 suspend=0 queue=0x8049D950 pc=0x80123450 lr=0x80123460 sp=0x80611800 "
                             "canary=ok stack=0x80614000..0x80616000"));
  EXPECT_TRUE(HasLine(lines, "thread 0x80602000 state=4 prio=24 suspend=0 queue=0x8049D4B8 pc=0x80345670"));
  EXPECT_TRUE(HasLine(lines, "canary=bad stack=0x80618000..0x8061A000"));
  // Its 6-frame back-chain from the saved sp (3 frames exist).
  EXPECT_TRUE(HasLine(lines, "thread 0x80601000 frame 0 sp=0x80611800 lr=0x80111110"));
  EXPECT_TRUE(HasLine(lines, "thread 0x80601000 frame 2 sp=0x80611900 lr=0x80333330"));
  EXPECT_FALSE(HasLine(lines, "thread 0x80601000 frame 3 "));
}

TEST(Rev3Rollback, FreezeDumpThreadListSurvivesLoopsBadPointersAndLength)
{
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  {
    FakeMemory mem;
    MakeFakeOs(mem);
    mem.SetU32(T3 + 0x2FC, T1);  // the list loops back
    FreezeDump dump(mem, sink, src);
    EXPECT_TRUE(HasLine(dump.Lines(false), "threads n=3 end=cycle(0x80600000)"));
  }
  {
    FakeMemory mem;
    MakeFakeOs(mem);
    mem.SetU32(T3 + 0x2FC, 0x12345678);  // not a pointer
    FreezeDump dump(mem, sink, src);
    EXPECT_TRUE(HasLine(dump.Lines(false), "threads n=3 end=bad_ptr(0x12345678)"));
  }
  {
    FakeMemory mem;
    MakeFakeOs(mem);
    mem.SetU32(T3 + 0x2FC, 0x80600002);  // not aligned
    FreezeDump dump(mem, sink, src);
    EXPECT_TRUE(HasLine(dump.Lines(false), "threads n=3 end=bad_ptr(0x80600002)"));
  }
  {
    FakeMemory mem;
    MakeFakeOs(mem);
    // 40 distinct threads in a chain: at most 32 are listed.
    mem.SetU32(OS_ACTIVE, 0x80700000);
    for (u32 i = 0; i < 40; ++i)
    {
      const u32 t = 0x80700000 + i * 0x400;
      MakeThread(mem, t, 1, 10, 0, 0x80001000 + i, 0, 0x80611000, 0x80720000 + i * 0x20, 0x80730000, true, i + 1 < 40 ? t + 0x400 : 0);
    }
    FreezeDump dump(mem, sink, src);
    const std::vector<std::string> lines = dump.Lines(false);
    EXPECT_TRUE(HasLine(lines, "threads n=32 end=max"));
    int threads = 0;
    for (const std::string& l : lines)
      threads += l.rfind("thread 0x", 0) == 0 && l.find(" state=") != std::string::npos;
    EXPECT_EQ(threads, 32);
  }
  {
    // A thread in MEM2 (not readable in the fake memory): fields are '?', nothing crashes.
    FakeMemory mem;
    MakeFakeOs(mem);
    mem.SetU32(OS_ACTIVE, 0x90000000);
    FreezeDump dump(mem, sink, src);
    const std::vector<std::string> lines = dump.Lines(false);
    EXPECT_TRUE(HasLine(lines, "threads n=1 end=unreadable_link"));
    EXPECT_TRUE(HasLine(lines, "thread 0x90000000 state=? prio=? suspend=? queue=? pc=? lr=? sp=? canary=? stack=?..?"));
  }
  {
    // No OS at all (zeroed memory): an empty list, no thread lines.
    FakeMemory mem;
    FreezeDump dump(mem, sink, src);
    const std::vector<std::string> lines = dump.Lines(false);
    EXPECT_TRUE(HasLine(lines, "threads n=0 end=null"));
    EXPECT_TRUE(HasLine(lines, "queue ipc skipped: pointer *0x8049D984 is 0x00000000"));
  }
}

TEST(Rev3Rollback, FreezeDumpShowsTheSleepQueuesAndTheGameFlags)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  FreezeDump dump(mem, sink, src);
  std::vector<std::string> lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "queue gx_drawdone at=0x8049D950 head=0x80601000 tail=0x80601000 waiters=1[0x80601000] head_state=0x0004 "
                             "head_pc=0x80123450 head_lr=0x80123460 drawdone_flag=0x01"));
  EXPECT_TRUE(HasLine(lines, "queue vi_retrace at=0x8049D4B8 head=0x80602000 tail=0x80602000 waiters=1[0x80602000] head_state=0x0004 "
                             "head_pc=0x80345670 head_lr=0x80345680 retrace_count=0x00001234 game_last_count=0x00001230"));
  EXPECT_TRUE(HasLine(lines, "queue dvd at=0x8049D7B0 head=0x00000000 tail=0x00000000 waiters=0"));
  EXPECT_TRUE(HasLine(lines, "queue nw4r_strm at=0x8049D238 head=0x00000000"));
  EXPECT_TRUE(HasLine(lines, "queue ax at=0x8049D6A0 head=0x00000000"));
  EXPECT_TRUE(HasLine(lines, "queue sc at=0x804655A0 head=0x00000000"));
  EXPECT_TRUE(HasLine(lines, "queue ipc at=0x8062002C head=0x00000000 tail=0x00000000 waiters=0 ipc_object=0x80620000"));
  EXPECT_TRUE(HasLine(lines, "game frame_loop_flags=0x00000003 main_state=0x00000007 no_gx=0x01000000"));

  // Two waiters in one queue, linked through OSThread.link (+0x2E0), and a loop in the link.
  mem.SetU32(Q_DRAWDONE, T2);
  mem.SetU32(T2 + 0x2E0, T3);
  mem.SetU32(Q_DRAWDONE + 4, T3);
  EXPECT_TRUE(HasLine(dump.Lines(false), "queue gx_drawdone at=0x8049D950 head=0x80601000 tail=0x80602000 waiters=2[0x80601000,0x80602000]"));
  mem.SetU32(T3 + 0x2E0, T2);
  EXPECT_TRUE(HasLine(dump.Lines(false), "waiters=2[0x80601000,0x80602000] more"));
  // A queue head that is not a thread pointer is shown but not followed.
  mem.SetU32(Q_RETRACE, 0x12345678);
  lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "queue vi_retrace at=0x8049D4B8 head=0x12345678 tail=0x80602000 waiters=0[] bad_link=0x12345678 retrace_count="));
  // The IPC pointer is skipped when it points outside MEM1 / MEM2.
  mem.SetU32(0x8049D984, 0x12345678);
  EXPECT_TRUE(HasLine(dump.Lines(false), "queue ipc skipped: pointer *0x8049D984 is 0x12345678"));
}

TEST(Rev3Rollback, FreezeDumpShowsTheHardware)
{
  FakeMemory mem;
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  src.hw.pe_ok = true;
  src.hw.pe_status = 0x000C;
  src.hw.pe_token = 0x0042;
  src.hw.cp_ok = true;
  src.hw.cp_base = 0x80500000;
  src.hw.cp_end = 0x8050FFE0;
  src.hw.cp_rd = 0x80501000;
  src.hw.cp_wr = 0x80501400;
  src.hw.cp_dist = 0x400;
  src.hw.cp_bp = 0x80500800;
  src.hw.cp_gp_read = true;
  src.hw.cp_link = true;
  src.hw.pi_ok = true;
  src.hw.pi_cause = 0x0000010C;
  src.hw.pi_mask = 0x0000FFFF;
  src.hw.pi_fifo_base = 0x00400000;
  src.hw.pi_fifo_end = 0x0040FFE0;
  src.hw.pi_fifo_wptr = 0x00401000;
  src.hw.vi_ok = true;
  src.hw.vi_line = 263;
  src.hw.vi_di0 = 0x90AE010F;
  FreezeDump dump(mem, sink, src);
  std::vector<std::string> lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "hw pe pe_status=0x000C pe_token=0x0042"));
  EXPECT_TRUE(HasLine(lines, "hw cp status=busy base=0x80500000 end=0x8050FFE0 rd=0x80501000 wr=0x80501400 rw_dist=0x400 bp=0x80500800 "
                             "gp_read=1 bp_enable=0 link=1 hi_wm=0 lo_wm=0"));
  EXPECT_TRUE(HasLine(lines, "hw pi cause=0x0000010C mask=0x0000FFFF fifo_base=0x00400000 fifo_end=0x0040FFE0 fifo_wptr=0x00401000"));
  EXPECT_TRUE(HasLine(lines, "hw vi line=263 di0=0x90AE010F"));
  src.hw.cp_dist = 0;
  EXPECT_TRUE(HasLine(dump.Lines(false), "hw cp status=idle "));
  src.hw.cp_breakpoint = true;
  src.hw.cp_dist = 0x40;
  EXPECT_TRUE(HasLine(dump.Lines(false), "hw cp status=breakpoint "));
  // Groups that could not be read say so; a hardware view that fails altogether gives one line.
  src.hw.pe_ok = false;
  src.hw.cp_ok = false;
  lines = dump.Lines(false);
  EXPECT_TRUE(HasLine(lines, "hw pe unavailable"));
  EXPECT_TRUE(HasLine(lines, "hw cp unavailable"));
  src.hw_ok = false;
  EXPECT_TRUE(HasLine(dump.Lines(false), "hw unavailable"));
}

TEST(Rev3Rollback, FreezeDumpWritesPrefixedLinesBetweenBeginAndEnd)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  FakeMs ms;
  FreezeDump dump(mem, sink, src, 2000, ms.Fn());
  dump.Tick(321);
  *ms.now += 2500;
  dump.Dump("stall", true, false, "stall_ms=2000");
  ASSERT_GT(sink.lines.size(), 10u);
  EXPECT_EQ(sink.lines.front(), "DUMP stall begin racy=1 frame=321 last_cmd_age_ms=2500 stall_ms=2000");
  EXPECT_EQ(sink.lines.back(), "DUMP stall end");
  for (const std::string& l : sink.lines)
    EXPECT_EQ(l.rfind("DUMP stall ", 0), 0u) << l;
  EXPECT_EQ(src.audio_calls, 0);  // racy / not full: no audio lines
  // A full, consistent dump asks the source for the audio lines and says racy=0.
  sink.lines.clear();
  dump.Dump("arm", false, true);
  EXPECT_EQ(sink.lines.front(), "DUMP arm begin racy=0 frame=321 last_cmd_age_ms=2500");
  EXPECT_TRUE(sink.Contains("DUMP arm dsp csr=0x0004 fake"));
  EXPECT_EQ(src.audio_calls, 1);
}

TEST(Rev3Rollback, RestoreHwLineHasTheHardwareAndTheSoftwareFlags)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  FakeSink sink;
  FakeDumpSource src;
  src.hw.pe_ok = true;
  src.hw.pe_status = 0x0008;
  src.hw.pe_token = 0x0099;
  src.hw.cp_ok = true;
  src.hw.cp_dist = 0x120;
  src.hw.cp_rd = 0x80501000;
  src.hw.cp_wr = 0x80501120;
  FreezeDump dump(mem, sink, src);
  EXPECT_EQ(dump.RestoreHwLine(12, 8, false),
            "RESTOREHW 12 -> 8 pe_status=0x0008 pe_token=0x0099 cp_rw_dist=0x120 cp_rd=0x80501000 cp_wr=0x80501120 vi_count=0x00001234 "
            "drawdone=0x01 sched_disable=0x00000001");
  EXPECT_EQ(dump.RestoreHwLine(12, 8, true),
            "RESTOREHW 12 -> 8 after pe_status=0x0008 pe_token=0x0099 cp_rw_dist=0x120 cp_rd=0x80501000 cp_wr=0x80501120 vi_count=0x00001234 "
            "drawdone=0x01 sched_disable=0x00000001");
  src.hw_ok = false;
  EXPECT_EQ(dump.RestoreHwLine(1, 0, false),
            "RESTOREHW 1 -> 0 pe_status=? pe_token=? cp_rw_dist=? cp_rd=? cp_wr=? vi_count=0x00001234 drawdone=0x01 sched_disable=0x00000001");
}

TEST(Rev3Rollback, StallIsReportedOncePerStallInAnyModeButNotWhileTheSessionWaits)
{
  FakeMemory mem;
  FakeSink sink;
  FakeDumpSource src;
  FakeMs ms;
  FreezeDump dump(mem, sink, src, 2000, ms.Fn());
  EXPECT_FALSE(dump.StallDue());  // no command yet: the game is not running the hook
  dump.Tick(1);
  *ms.now += 1999;
  EXPECT_FALSE(dump.StallDue());  // shorter than the limit
  *ms.now += 1;
  EXPECT_TRUE(dump.StallDue());  // not armed, no SyncTest: a stall all the same
  EXPECT_FALSE(dump.StallDue());  // once per stall
  *ms.now += 5000;
  EXPECT_FALSE(dump.StallDue());
  dump.Tick(2);  // the game is alive again: the next silence is a new stall
  EXPECT_EQ(dump.LastFrame(), 2u);
  *ms.now += 2000;
  EXPECT_EQ(dump.LastAgeMs(), 2000u);
  // The netplay session is waiting for the peer: no command arrives on purpose.
  dump.SetBlocked(true);
  EXPECT_TRUE(dump.Blocked());
  *ms.now += 60000;
  EXPECT_FALSE(dump.StallDue());
  dump.SetBlocked(false);
  EXPECT_FALSE(dump.StallDue());  // the wait itself is not silence: the timer restarts when it ends
  *ms.now += 1999;
  EXPECT_FALSE(dump.StallDue());
  *ms.now += 1;
  EXPECT_TRUE(dump.StallDue());  // ... and a game that still does not come back is a stall
}

TEST(Rev3Rollback, SyncTestDumpsAtArmAndLogsTheHardwareAroundTheFirstRestores)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  src.hw.pe_ok = true;
  src.hw.cp_ok = true;
  FakeMs ms;
  FreezeDump dump(game.Mem(), sink, src, 2000, ms.Fn());
  test.SetFreezeDump(&dump);
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  ASSERT_GT(test.Stats().rollbacks, 20u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);

  // One baseline dump at arm, after the arm notes, as a consistent (racy=0) full dump.
  int arm_begin = 0;
  int arm_end = 0;
  for (const std::string& l : sink.lines)
  {
    arm_begin += l.rfind("DUMP arm begin racy=0 frame=", 0) == 0;
    arm_end += l == "DUMP arm end";
  }
  EXPECT_EQ(arm_begin, 1);
  EXPECT_EQ(arm_end, 1);
  EXPECT_TRUE(sink.Contains("DUMP arm dsp csr=0x0004 fake"));
  EXPECT_TRUE(sink.Contains("DUMP arm ppc pc=0x80001000"));

  // Around each of the first 10 restores: RESTORE begin, RESTOREHW (before), RESTOREHW after, RESTORE done.
  int before = 0;
  int after = 0;
  for (size_t i = 0; i < sink.lines.size(); ++i)
  {
    const std::string& l = sink.lines[i];
    if (l.rfind("RESTOREHW ", 0) != 0)
      continue;
    const bool is_after = l.find(" after pe_status=") != std::string::npos;
    (is_after ? after : before)++;
    ASSERT_GT(i, 0u);
    ASSERT_LT(i + 1, sink.lines.size());
    if (is_after)
    {
      EXPECT_EQ(sink.lines[i + 1].rfind("RESTORE ", 0), 0u) << sink.lines[i + 1];
      EXPECT_NE(sink.lines[i + 1].find(" bytes="), std::string::npos);
    }
    else
    {
      EXPECT_EQ(sink.lines[i - 1].rfind("RESTORE ", 0), 0u) << sink.lines[i - 1];
      EXPECT_NE(sink.lines[i - 1].find(" begin"), std::string::npos);
    }
  }
  EXPECT_EQ(before, 10);
  EXPECT_EQ(after, 10);

  // HandleCommand (not SyncTest) ticks the dump; here the test does.
  dump.Tick(7);
  EXPECT_EQ(dump.LastFrame(), 7u);
}

TEST(Rev3Rollback, SyncTestWithoutADumpLogsNoDumpLines)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 200; ++i)
    game.Frame(test);
  ASSERT_GT(test.Stats().rollbacks, 20u);
  EXPECT_FALSE(sink.Contains("DUMP "));
  EXPECT_FALSE(sink.Contains("RESTOREHW"));
}

TEST(Rev3Rollback, FreezeDumpShowsTheWordsAroundTheDvdAndStreamQueuesAndTheExtraHardwareLines)
{
  FakeMemory mem;
  mem.SetU32(0x8049D7B0 - 0x20, 0x11111111);
  mem.SetU32(0x8049D7B0 + 0x3C, 0x22222222);
  mem.SetU32(0x8049D238, 0x33333333);
  FakeSink sink;
  FakeDumpSource src;
  FreezeDump dump(mem, sink, src);
  const std::vector<std::string> lines = dump.Lines(false, true);
  EXPECT_TRUE(HasLine(lines, "words dvd base=0x8049D790 +0x00: 0x11111111 0x00000000"));
  EXPECT_TRUE(HasLine(lines, "words dvd base=0x8049D790 +0x40:"));
  EXPECT_TRUE(HasLine(lines, "0x22222222"));
  EXPECT_TRUE(HasLine(lines, "words nw4r_strm base=0x8049D218 +0x00:"));
  EXPECT_TRUE(HasLine(lines, "0x33333333"));
  EXPECT_TRUE(HasLine(lines, "hw di fake racy"));
  EXPECT_TRUE(HasLine(dump.Lines(false, false), "hw di fake"));
  EXPECT_FALSE(HasLine(dump.Lines(false, false), "fake racy"));
}

// ---- the nw4r::ef effect pool: range computation, inclusion file, list check, forced inclusion, EFCHECK ------------------------------

TEST(Rev3Rollback, EfPoolRangesComeFromTheEffectSystemSingleton)
{
  FakeMemory mem;
  FakeEf ef(mem);
  ef.BuildChain(3);
  EfPool pool;
  std::string why;
  ASSERT_TRUE(FindEfPool(mem, &pool, &why)) << why;
  EXPECT_EQ(pool.mm, 0x80B00000u);
  EXPECT_EQ(pool.heap_start, 0x80B00040u);
  EXPECT_EQ(pool.heap_end, 0x80B00040u + 0x40000u);
  EXPECT_EQ(pool.counts[0], FakeEf::kEffects);
  EXPECT_EQ(pool.counts[1], FakeEf::kEmitters);
  EXPECT_EQ(pool.counts[2], FakeEf::kPms);
  EXPECT_EQ(pool.counts[3], FakeEf::kParticles);
  // The allocation: mm .. heap end + the 0xC20 tail (handle table and slack), widened to pages.
  EXPECT_EQ(pool.main.start, 0x80B00000u);
  EXPECT_EQ(pool.main.end, 0x80B41000u);
  // Plus the singleton itself (0x803D9588 + 0xC070, widened).
  ASSERT_EQ(pool.ranges.size(), 2u);
  EXPECT_EQ(pool.ranges[0].start, 0x803D9000u);
  EXPECT_EQ(pool.ranges[0].end, 0x803E6000u);
  EXPECT_EQ(pool.ranges[1].start, pool.main.start);
  EXPECT_EQ(pool.ranges[1].end, pool.main.end);
  EXPECT_EQ(pool.bytes, (0x803E6000u - 0x803D9000u) + (0x80B41000u - 0x80B00000u));
  for (const Range& r : pool.ranges)
  {
    EXPECT_EQ(r.start % PAGE_SIZE, 0u);
    EXPECT_EQ(r.end % PAGE_SIZE, 0u);
  }
  EXPECT_NE(pool.detail.find("instance=0x803D9000-0x803E6000"), std::string::npos) << pool.detail;
  EXPECT_NE(pool.detail.find("handle_table=0x80B40040(inside the block)"), std::string::npos) << pool.detail;
  EXPECT_NE(pool.detail.find("handle_pool0=none"), std::string::npos) << pool.detail;
}

TEST(Rev3Rollback, EfPoolRangeIsClippedToTheRegionAndTakesTheHandlePools)
{
  FakeMemory mem;
  FakeEf ef(mem, 0x817E0000, 0x1FF00);  // heap end 0x817FFF40: the tail runs past the end of MEM1
  EfPool pool;
  std::string why;
  ASSERT_TRUE(FindEfPool(mem, &pool, &why)) << why;
  EXPECT_EQ(pool.main.end, 0x81800000u);
  EXPECT_EQ(pool.heap_end, 0x817FFF40u);

  // A handle pool outside the block: {free, first, n = 4, payload = 0x20} = 0x10 + 4 * 0x24 bytes.
  mem.SetU32(EF_HANDLE_POOL_GLOBALS[0], 0x80C20F80);
  mem.SetU32(0x80C20F80 + 8, 4);
  mem.SetU32(0x80C20F80 + 0xC, 0x20);
  // A second one with n = 0 does not validate.
  mem.SetU32(EF_HANDLE_POOL_GLOBALS[1], 0x80C30000);
  ASSERT_TRUE(FindEfPool(mem, &pool, &why)) << why;
  EXPECT_TRUE(SetContains(pool.ranges, 0x80C20F80, 0x10 + 4 * 0x24));  // crosses a page border: both pages
  EXPECT_TRUE(SetContains(pool.ranges, 0x80C21000));
  EXPECT_FALSE(SetContains(pool.ranges, 0x80C30000));
  EXPECT_NE(pool.detail.find("handle_pool0=0x80C20F80(n=4,payload=0x20)"), std::string::npos) << pool.detail;
  EXPECT_NE(pool.detail.find("handle_pool1=none"), std::string::npos) << pool.detail;

  // The handle table pointer elsewhere: its own pages.
  mem.SetU32(EF_HANDLE_TABLE_GLOBAL, 0x80C40010);
  ASSERT_TRUE(FindEfPool(mem, &pool, &why)) << why;
  EXPECT_TRUE(SetContains(pool.ranges, 0x80C40010, EF_HANDLE_TABLE_BYTES));
  EXPECT_EQ(pool.detail.find("(inside the block)"), std::string::npos) << pool.detail;
}

TEST(Rev3Rollback, EfPoolIsNotFoundWhenTheEffectSystemIsNotSetUp)
{
  EfPool pool;
  std::string why;
  {
    FakeMemory mem;  // all zero: the singleton has no manager yet
    EXPECT_FALSE(FindEfPool(mem, &pool, &why));
    EXPECT_NE(why.find("0x803D9588 is 0"), std::string::npos) << why;
  }
  {
    FakeMemory mem;
    mem.SetU32(EF_SYSTEM_ADDR, 0x80B00010);  // not 32-byte aligned
    EXPECT_FALSE(FindEfPool(mem, &pool, &why));
    EXPECT_NE(why.find("32-byte aligned"), std::string::npos) << why;
    mem.SetU32(EF_SYSTEM_ADDR, 0x12345600);  // not guest RAM
    EXPECT_FALSE(FindEfPool(mem, &pool, &why));
    EXPECT_NE(why.find("MEM1 / MEM2"), std::string::npos) << why;
  }
  {
    FakeMemory mem;
    mem.SetU32(EF_SYSTEM_ADDR, 0x80B00000);  // a manager whose heap fields are zero
    EXPECT_FALSE(FindEfPool(mem, &pool, &why));
    EXPECT_NE(why.find("implausible heap"), std::string::npos) << why;
    mem.SetU32(0x80B00000 + EF_MM_HEAP_START, 0x80B00040);
    mem.SetU32(0x80B00000 + EF_MM_HEAP_END, 0x80B00040 + 0x4000000);  // 64 MB: far too big, and past MEM1
    EXPECT_FALSE(FindEfPool(mem, &pool, &why));
    EXPECT_NE(why.find("implausible heap"), std::string::npos) << why;
  }
  EXPECT_TRUE(pool.ranges.empty());
}

TEST(Rev3Rollback, InclusionFileTakesTheSameEntriesAsTheExclusionFile)
{
  FakeSink sink;
  std::vector<Range> fixed;
  std::vector<PtrExclusion> ptrs;
  std::string error;
  ASSERT_TRUE(LoadInclusions(R"({"ranges":[{"start":"0x80700000","end":"0x80701000"},
      {"ptr":"0x8049D000","size":"0x3F0000","note":"snd"},{"start":2415919104,"end":"0x90001000"}]})",
                             sink, &fixed, &ptrs, &error))
      << error;
  ASSERT_EQ(fixed.size(), 2u);
  EXPECT_EQ(fixed[0].start, 0x80700000u);
  EXPECT_EQ(fixed[0].end, 0x80701000u);
  EXPECT_EQ(fixed[1].start, 0x90000000u);
  EXPECT_EQ(fixed[1].end, 0x90001000u);
  ASSERT_EQ(ptrs.size(), 1u);
  EXPECT_EQ(ptrs[0].ptr, 0x8049D000u);
  EXPECT_EQ(ptrs[0].size, 0x3F0000u);
  EXPECT_EQ(ptrs[0].note, "snd");
  EXPECT_TRUE(sink.Contains("NOTE include user 0x80700000-0x80701000 (4096 bytes"));
  EXPECT_TRUE(sink.Contains("NOTE include user 0x90000000-0x90001000 (4096 bytes"));
  EXPECT_TRUE(sink.Contains("NOTE include ptr entry 0x8049D000 size=0x3F0000 note=snd"));

  const char* const bad[] = {
      "not json",
      R"({"ranges":[]})",
      R"({"nothing":1})",
      R"({"ranges":[{"start":"0x00001000","end":"0x00002000"}]})",                       // not guest RAM
      R"({"ranges":[{"start":"0x80000200","end":"0x80000100"}]})",                       // backwards
      R"({"ranges":[{"ptr":"0x8049D000"}]})",                                            // no size
      R"({"ranges":[{"ptr":"0x00001000","size":"0x100"}]})",                             // pointer outside guest RAM
      R"({"ranges":[{"ptr":"0x8049D000","size":0}]})",                                   // empty
  };
  for (const char* text : bad)
  {
    FakeSink quiet;
    std::vector<Range> f = {{1, 2}};
    std::vector<PtrExclusion> p;
    error.clear();
    EXPECT_FALSE(LoadInclusions(text, quiet, &f, &p, &error)) << text;
    EXPECT_FALSE(error.empty()) << text;
    EXPECT_TRUE(quiet.lines.empty()) << text;  // nothing logged for a file that is refused
    ASSERT_EQ(f.size(), 1u) << text;           // outputs untouched
    EXPECT_EQ(f[0].start, 1u) << text;
  }
}

TEST(Rev3Rollback, EfCheckAcceptsAConsistentImageAndCountsWhatItWalked)
{
  FakeMemory mem;
  FakeEf ef(mem);
  ef.BuildChain(5);
  const EfCheckResult r = CheckEfLists(mem);
  EXPECT_TRUE(r.ran);
  EXPECT_TRUE(r.ok) << r.list << " " << r.why;
  EXPECT_EQ(r.objects[0], 1u);
  EXPECT_EQ(r.objects[1], 1u);
  EXPECT_EQ(r.objects[2], 1u);
  EXPECT_EQ(r.objects[3], 5u);
  // Lists: 2 per group + 2 per effect + 2 per emitter + 2 per pm + 2 per pool (free, in use) = 2 + 2 + 2 + 2 + 8.
  EXPECT_EQ(r.lists, 16u);
  // Nodes: the chain (1 + 1 + 1 + 5) plus every pool object once, in its free or in-use list (4 + 4 + 4 + 40).
  EXPECT_EQ(r.nodes, 8u + 52u);

  // An effect system that is not set up is not an error, there is just nothing to check.
  FakeMemory empty;
  const EfCheckResult none = CheckEfLists(empty);
  EXPECT_FALSE(none.ran);
  EXPECT_TRUE(none.ok);
}

TEST(Rev3Rollback, EfCheckNamesTheBrokenListAndNode)
{
  struct Case
  {
    const char* name;
    std::function<void(FakeMemory&, FakeEf&)> damage;
    const char* list;  // prefix of the failing list's name
    const char* why;   // substring of the reason
    u32 list_cap;
  };
  const Case cases[] = {
      {"cycle back to the head",
       [](FakeMemory& m, FakeEf& e) { m.SetU32(e.particle(4) + 0x14 + 4, e.particle(0)); },
       "particles@pm=", "prev link is 0x00000000, expected", EF_CHECK_LIST_CAP},
      {"wrong prev link", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.particle(2) + 0x14, 0xDEADBEEF); }, "particles@pm=", "prev link is 0xDEADBEEF",
       EF_CHECK_LIST_CAP},
      {"count too big", [](FakeMemory& m, FakeEf& e) { m.SetU16(e.particles_list() + 8, 7); }, "particles@pm=", "walked 5 nodes but the count says 7",
       EF_CHECK_LIST_CAP},
      {"tail is not the last node", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.particles_list() + 4, e.particle(2)); }, "particles@pm=",
       "but the tail is", EF_CHECK_LIST_CAP},
      {"link into stale memory outside the heap", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.particle(1) + 0x14 + 4, 0x80100000); }, "particles@pm=",
       "outside the ef heap", EF_CHECK_LIST_CAP},
      {"wrong list offset", [](FakeMemory& m, FakeEf& e) { m.SetU16(e.particles_list() + 0xA, 0x10); }, "particles@pm=", "list offset is 0x10, expected 0x14",
       EF_CHECK_LIST_CAP},
      {"count 0 with a head", [](FakeMemory& m, FakeEf& e) { m.SetU16(e.particles_list() + 8, 0); }, "particles@pm=", "empty and non-empty",
       EF_CHECK_LIST_CAP},
      {"list longer than the cap", [](FakeMemory&, FakeEf&) {}, "particles@pm=", "more than 3 nodes", 3},
      {"free list of the particle pool loops", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.particle(7) + 4, e.particle(7)); }, "pool_free[particle]",
       "prev link", EF_CHECK_LIST_CAP},
      {"emitter list of the effect points at a particle", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.effect() + 0x24, e.particle(3)); },
       "emitters@effect=", "", EF_CHECK_LIST_CAP},
      {"activity group count is garbage", [](FakeMemory& m, FakeEf&) { m.SetU32(EF_SYSTEM_ADDR + 0xC014, 1000); }, "EffectSystem", "activity groups",
       EF_CHECK_LIST_CAP},
      {"manager without a heap", [](FakeMemory& m, FakeEf& e) { m.SetU32(e.mm() + EF_MM_HEAP_END, 0); }, "EffectSystem", "no usable heap",
       EF_CHECK_LIST_CAP},
  };
  for (const Case& c : cases)
  {
    FakeMemory mem;
    FakeEf ef(mem);
    ef.BuildChain(5);
    c.damage(mem, ef);
    const EfCheckResult r = CheckEfLists(mem, c.list_cap);
    EXPECT_TRUE(r.ran) << c.name;
    EXPECT_FALSE(r.ok) << c.name;
    EXPECT_EQ(r.list.rfind(c.list, 0), 0u) << c.name << ": " << r.list;
    EXPECT_NE(r.why.find(c.why), std::string::npos) << c.name << ": " << r.why;
  }
  // The node is named too: a link into stale memory names the stale address; a prev mismatch names the node whose link is wrong.
  FakeMemory mem;
  FakeEf ef(mem);
  ef.BuildChain(5);
  mem.SetU32(ef.particle(2) + 0x14, 0xDEADBEEF);
  const EfCheckResult r = CheckEfLists(mem);
  EXPECT_EQ(r.node, ef.particle(2));
  EXPECT_EQ(r.list_addr, ef.particles_list());
  char name[64];
  std::snprintf(name, sizeof(name), "particles@pm=0x%08X", ef.pm());
  EXPECT_EQ(r.list, name);
}

TEST(Rev3Rollback, EfDumpLinesShowTheSingletonTheHeadsAndTheCheck)
{
  FakeMemory mem;
  {
    const std::vector<std::string> lines = EfDumpLines(mem);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "ef sys=0x803D9588 mm=0x00000000 (effect system not set up)");
  }
  FakeEf ef(mem);
  ef.BuildChain(5);
  std::vector<std::string> lines = EfDumpLines(mem);
  EXPECT_TRUE(HasLine(lines, "ef sys=0x803D9588 mm=0x80B00000 heap=0x80B00040-0x80B40040 pool_sizes effect=4 emitter=4 pm=4 particle=40 groups=1"));
  EXPECT_TRUE(HasLine(lines, "ef group0 effects head=0x"));
  EXPECT_TRUE(HasLine(lines, "n=1 off=0x14"));
  EXPECT_TRUE(HasLine(lines, "ef pool particle obj=0x"));
  EXPECT_TRUE(HasLine(lines, "ef pool effect obj=0x"));
  EXPECT_TRUE(HasLine(lines, "ef check ok lists=16 nodes=60 effects=1 emitters=1 pms=1 particles=5"));

  mem.SetU32(ef.particle(4) + 0x14 + 4, ef.particle(0));  // the freeze: a cyclic particle list
  lines = EfDumpLines(mem);
  EXPECT_TRUE(HasLine(lines, "ef check BAD list=particles@pm=0x"));
  EXPECT_TRUE(HasLine(lines, "list_addr=0x"));
}

TEST(Rev3Rollback, FreezeDumpCarriesTheEfLines)
{
  FakeMemory mem;
  MakeFakeOs(mem);
  FakeEf ef(mem);
  ef.BuildChain(2);
  FakeSink sink;
  FakeDumpSource src;
  src.cpu = MakeFakeCpu();
  FreezeDump dump(mem, sink, src);
  EXPECT_TRUE(HasLine(dump.Lines(false, true), "ef sys=0x803D9588 mm=0x80B00000"));
  EXPECT_TRUE(HasLine(dump.Lines(true, false), "ef check ok"));
  dump.Dump("stall", true, false);
  EXPECT_TRUE(sink.Contains("DUMP stall ef sys=0x803D9588"));
}

namespace
{
// A game whose frames allocate one particle per frame from `first` on (the fake's CreateParticle) and keep a counter in the pm struct and in
// the particle pool object, so that the pages holding the list heads are dynamic long before: only the particle array's later pages are fresh.
void EfStepHook(FakeGame* game, FakeEf* ef, u32 first)
{
  game->SetStepHook([ef, first](FakeMemory& mem, u32 frame) {
    mem.SetU32(ef->pm() + 0xA0, frame);
    mem.SetU32(ef->pool(3) + 0x20, frame);
    if (frame >= first)
      ef->CreateParticle();
  });
}

bool LinesContain(const FakeSink& sink, const std::string& a, const std::string& b)
{
  for (const std::string& l : sink.lines)
  {
    if (l.find(a) != std::string::npos && l.find(b) != std::string::npos)
      return true;
  }
  return false;
}
}  // namespace

TEST(Rev3Rollback, ArmForcesTheEfPoolIntoTheSetThatNothingHasTouched)
{
  const auto run = [](bool forced, std::vector<Range>* set, std::vector<std::string>* log, EfPool* info) {
    FakeGame game(false);
    FakeEf ef(game.Mem());
    ef.BuildChain(3);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(3);
    o.ef_pool = forced;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 330; ++i)
      game.Frame(test);
    EXPECT_TRUE(test.Active());
    *set = test.Ranges();
    *log = sink.lines;
    *info = test.EfPoolInfo();
    EXPECT_EQ(test.EfPoolFound(), forced);
  };
  std::vector<Range> with;
  std::vector<Range> without;
  std::vector<std::string> log_with;
  std::vector<std::string> log_without;
  EfPool info_with;
  EfPool info_without;
  run(true, &with, &log_with, &info_with);
  run(false, &without, &log_without, &info_without);

  // Nothing in the pool ever changed, so the page tracker alone leaves it out ...
  EXPECT_FALSE(SetContains(without, 0x80B00000));
  EXPECT_FALSE(SetContains(without, 0x80B00040 + 0x20000));
  // ... and the forced run has all of it: the manager page, the heap, the tail and the singleton.
  ASSERT_FALSE(info_with.ranges.empty());
  for (const Range& r : info_with.ranges)
    EXPECT_TRUE(SetContains(with, r.start, r.end - r.start)) << std::hex << r.start;
  EXPECT_TRUE(SetContains(with, 0x80B00000, 0x41000));
  EXPECT_TRUE(SetContains(with, EF_SYSTEM_ADDR, 0x100));

  const auto has = [](const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines)
    {
      if (l.find(needle) != std::string::npos)
        return true;
    }
    return false;
  };
  EXPECT_TRUE(has(log_with, "NOTE ef pool 0x80B00000-0x80B41000 bytes=266240"));
  EXPECT_TRUE(has(log_with, "NOTE ef pool total bytes=319488 ranges=2 mm=0x80B00000 heap=0x80B00040-0x80B40040 effects=4 emitters=4 particle_managers=4 particles=40"));
  EXPECT_TRUE(has(log_with, "NOTE forced include: ranges=2 bytes=319488; the snapshot set grows by "));
  EXPECT_TRUE(has(log_without, "NOTE ef pool off (REV3_EF_POOL=0)"));
  EXPECT_FALSE(has(log_without, "NOTE ef pool 0x"));
}

TEST(Rev3Rollback, MissingEfPoolIsLoggedAndHarmless)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(3), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 450; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_FALSE(test.EfPoolFound());
  EXPECT_TRUE(test.ForcedRanges().empty());
  EXPECT_TRUE(sink.Contains("NOTE ef pool not found: the memory manager pointer at EffectSystem 0x803D9588 is 0"));
  EXPECT_FALSE(sink.Contains("NOTE forced include"));
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_EQ(test.Stats().ef_checks, test.Stats().restores);  // the check ran (and found nothing to check)
  EXPECT_EQ(test.Stats().ef_bad, 0u);
  EXPECT_FALSE(sink.Contains("EFCHECK"));
}

TEST(Rev3Rollback, EfPoolIsForcedIntoTheClassicSetToo)
{
  FakeGame game(false);
  FakeEf ef(game.Mem());
  ef.BuildChain(3);
  FakeSink sink;
  SyncTest test(Options(3), game.Ranges(), game.Mem(), sink, FakeClock());  // fixed whitelist, no page tracker
  for (int i = 0; i < 120; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_TRUE(SetContains(test.Ranges(), 0x80B00000, 0x41000));
  EXPECT_TRUE(SetContains(test.Ranges(), STATE_A, 0x100));  // the whitelist is still there
  EXPECT_TRUE(SetContains(test.Ranges(), STATE_B, 0x40));
  EXPECT_GT(test.Stats().rollbacks, 5u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, UserInclusionsAreForcedAtArmAndTheCutsStillWin)
{
  FakeGame game(false);
  game.Mem().SetU32(0x80E00000, 0x80E10000);  // a pointer to a block the game allocated
  FakeSink sink;
  FakeHardware hw;
  hw.list.push_back({"test_dma", 0x80D01000, 0x80D01800});
  SyncTestOptions o = AdaptiveOptions(3);
  o.ef_pool = false;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock(), &hw);
  test.SetUserInclusions({{0x80D00000, 0x80D02000}, {0x80002000, 0x80003000}},
                         {{0x80E00000, 0x3000, "block"}, {0x80E00100, 0x1000, "unset"}});  // the second pointer holds 0
  for (int i = 0; i < 330; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  const std::vector<Range>& set = test.Ranges();
  EXPECT_TRUE(SetContains(set, 0x80D00000, 0x1000));
  EXPECT_TRUE(SetContains(set, 0x80D01800, 0x800));
  EXPECT_FALSE(SetContains(set, 0x80D01000, 4));  // the hardware cut wins over the inclusion
  EXPECT_FALSE(SetContains(set, 0x80D017FC, 4));
  EXPECT_TRUE(SetContains(set, 0x80002000, 0x400));  // the hook region 0x80002400-0x80002A00 is cut out of it
  EXPECT_FALSE(SetContains(set, HOOK_REGION_START, 4));
  EXPECT_TRUE(SetContains(set, 0x80002A00, 0x600));
  EXPECT_TRUE(SetContains(set, 0x80E10000, 0x3000));  // the pointer entry, resolved when arming
  EXPECT_FALSE(SetContains(set, 0x80E13000, 4));
  EXPECT_TRUE(sink.Contains("NOTE include ptr 0x80E00000 -> 0x80E10000-0x80E13000 (12288 bytes) note=block"));
  EXPECT_TRUE(sink.Contains("NOTE include ptr 0x80E00100 skipped: pointer is 0"));
  EXPECT_TRUE(sink.Contains("NOTE forced include: ranges=3"));
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
}

TEST(Rev3Rollback, WithoutThePoolARollbackLeavesBrokenEfListsAndEfCheckSaysWhere)
{
  FakeGame game(false);
  FakeEf ef(game.Mem());
  ef.BuildChain(3);
  EfStepHook(&game, &ef, 1200);  // from frame 1200 one new particle per frame; the third one lives in a page nothing has touched yet
  FakeSink sink;
  SyncTestOptions o = AdaptiveOptions(3, 64, 3);  // a scan cycle is 64 calls: the fresh page is noticed late, as in the real run
  o.ef_pool = false;
  o.ef_check_restores = EF_CHECK_ALL;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 260 && test.Stats().ef_bad == 0; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GE(test.Stats().rollbacks, 1u);
  EXPECT_GE(test.Stats().ef_bad, 1u);
  // The failing list is named, with the node and the fact that its page is not in the snapshot.
  EXPECT_TRUE(LinesContain(sink, "EFCHECK bad after restore", "in_snapshot=0"));
  EXPECT_TRUE(LinesContain(sink, "EFCHECK bad after restore", "pool_free[particle]") || LinesContain(sink, "EFCHECK bad after restore", "particles@pm="));
}

TEST(Rev3Rollback, WithThePoolForcedTheEfListsSurviveEveryRollback)
{
  FakeGame game(false);
  FakeEf ef(game.Mem());
  ef.BuildChain(3);
  EfStepHook(&game, &ef, 1200);
  FakeSink sink;
  SyncTestOptions o = AdaptiveOptions(3, 64, 3);
  o.ef_check_restores = EF_CHECK_ALL;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 800; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GT(test.Stats().rollbacks, 100u);
  EXPECT_GT(test.Stats().ef_checks, 100u);
  EXPECT_EQ(test.Stats().ef_bad, 0u);
  EXPECT_EQ(test.Stats().ef_bad_before, 0u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_FALSE(sink.Contains("EFCHECK bad"));
  EXPECT_TRUE(sink.Contains("EFCHECK ok after restore"));
  // The particles really were allocated by the game, and the replays agree with the first run: the pool ends up with the same lists.
  EXPECT_GT(ef.Count(ef.particles_list()), 10u);
  EXPECT_TRUE(CheckEfLists(game.Mem()).ok);
  test.Finish();
  EXPECT_TRUE(sink.Contains("ef_checks="));
}

TEST(Rev3Rollback, EfCheckRunsForTheFirstNRestoresOnlyAndCanBeSwitchedOff)
{
  const auto run = [](u32 limit) {
    FakeGame game(false);
    FakeEf ef(game.Mem());
    ef.BuildChain(3);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(3);
    o.ef_check_restores = limit;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    for (int i = 0; i < 700; ++i)
      game.Frame(test);
    EXPECT_GT(test.Stats().restores, 20u);
    return std::make_pair(test.Stats().ef_checks, test.Stats().restores);
  };
  EXPECT_EQ(run(5).first, 5u);
  EXPECT_EQ(run(0).first, 0u);
  const auto all = run(EF_CHECK_ALL);
  EXPECT_EQ(all.first, all.second);
}

TEST(Rev3Rollback, EfCheckAlsoLooksAtTheStateBeforeTheWriteBack)
{
  // A first run that already left a broken list (the game's own state, not the restore's doing) is reported as `before`.
  FakeGame game(false);
  FakeEf ef(game.Mem());
  ef.BuildChain(3);
  FakeSink sink;
  SyncTestOptions o = AdaptiveOptions(3);
  o.ef_check_restores = EF_CHECK_ALL;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 330 && !test.Active(); ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  game.Mem().SetU16(ef.particles_list() + 8, 9);  // count says 9, the list holds 3
  // Snapshots taken from here on all carry the bad count, so before AND after fail; the first restore's `before` is the one that matters.
  for (int i = 0; i < 20; ++i)
    game.Frame(test);
  EXPECT_GE(test.Stats().ef_bad_before, 1u);
  EXPECT_TRUE(LinesContain(sink, "EFCHECK bad before restore", "walked 3 nodes but the count says 9"));
}

TEST(Rev3Rollback, LearningAPageWhileArmedBlocksRollbacksToOlderSnapshots)
{
  FakeGame game(false);
  game.SetLatePage(1500);
  FakeSink sink;
  const int k = 3;
  SyncTest test(AdaptiveOptions(k), {}, game.Mem(), sink, FakeClock());
  Reply last;
  while (game.Counter() < 1500)
    last = game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_GT(test.Stats().rollbacks, 50u);
  EXPECT_FALSE(sink.Contains("NOTE learned +"));
  for (int i = 0; i < 400 && !sink.Contains("NOTE learned +"); ++i)
    last = game.Frame(test);
  ASSERT_TRUE(sink.Contains("NOTE learned +"));
  // The layout changed: every older snapshot is gone. No restore in the call that learned, nor in the next k - 1 calls; the first one may
  // come once k new frames are captured, and it restores a snapshot of the NEW layout.
  const u32 rollbacks = test.Stats().rollbacks;
  EXPECT_EQ(last.flags, 0u);
  for (int i = 0; i < k - 1; ++i)
    EXPECT_EQ(game.Frame(test).flags, 0u) << i;
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  for (int i = 0; i < 10; ++i)
    game.Frame(test);
  EXPECT_GT(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().layout_refused, 0u);
  // The page was noticed up to one scan cycle late and rollbacks had been running meanwhile: logged as a warning and counted.
  EXPECT_GE(test.Stats().late_learn_rollbacks, 1u);
  EXPECT_TRUE(LinesContain(sink, "NOTE learned +1 pages", "new=[0x80A00000-0x80A01000]"));
  EXPECT_TRUE(LinesContain(sink, "NOTE learned +1 pages", "WARNING a rollback ran within the last scan cycle"));
  test.Finish();
  EXPECT_TRUE(sink.Contains("layout_refused=0 late_learn_rollbacks="));
}

// ---- learned-page reporting (names the pages; the include-file tool reads these lines) ----------------------------------------------

TEST(Rev3Rollback, FormatRangeListNamesRangesAndCapsTheList)
{
  EXPECT_EQ(FormatRangeList({}, 4), "");
  EXPECT_EQ(FormatRangeList({{0x80A00000, 0x80A01000}}, 4), "0x80A00000-0x80A01000");
  const std::vector<Range> five = {{0x80000000, 0x80001000}, {0x80002000, 0x80003000}, {0x80004000, 0x80005000},
                                   {0x90000000, 0x90002000}, {0x93FFF000, 0x94000000}};
  EXPECT_EQ(FormatRangeList(five, 3), "0x80000000-0x80001000,0x80002000-0x80003000,0x80004000-0x80005000,+2 more");
  EXPECT_EQ(FormatRangeList(five, 5), "0x80000000-0x80001000,0x80002000-0x80003000,0x80004000-0x80005000,0x90000000-0x90002000,"
                                       "0x93FFF000-0x94000000");
  EXPECT_EQ(FormatRangeList(five, 0), "+5 more");
}

TEST(Rev3Rollback, NewRangesAndPageCounts)
{
  const std::vector<Range> before = {{0x80001000, 0x80003000}, {0x80010000, 0x80011000}};
  const std::vector<Range> after = {{0x80001000, 0x80004000}, {0x8000F000, 0x80011000}, {0x80020000, 0x80021000}};
  const std::vector<Range> fresh = NewRanges(after, before);
  ASSERT_EQ(fresh.size(), 3u);
  EXPECT_EQ(fresh[0].start, 0x80003000u);
  EXPECT_EQ(fresh[0].end, 0x80004000u);
  EXPECT_EQ(fresh[1].start, 0x8000F000u);
  EXPECT_EQ(fresh[1].end, 0x80010000u);
  EXPECT_EQ(fresh[2].start, 0x80020000u);
  EXPECT_EQ(RangesBytes(fresh), 0x3000u);
  EXPECT_EQ(RangesPages(fresh), 3u);
  EXPECT_EQ(RangesPages({{0x80000000, 0x80000010}, {0x80002000, 0x80002800}}), 1u);  // 0x810 bytes round up to one page
  EXPECT_EQ(RangesPages({}), 0u);
  EXPECT_TRUE(NewRanges(before, before).empty());
  EXPECT_EQ(CountTouching(fresh, before), 2u);  // 0x80003000 starts where an old range ends; 0x8000F000-0x80010000 ends where one starts
}

TEST(Rev3Rollback, PadRangesIsClippedToTheRegion)
{
  // 1 page each side, merged where the pads meet.
  std::vector<Range> padded = PadRanges({{0x80005000, 0x80006000}, {0x80007000, 0x80008000}}, 0x1000);
  ASSERT_EQ(padded.size(), 1u);
  EXPECT_EQ(padded[0].start, 0x80004000u);
  EXPECT_EQ(padded[0].end, 0x80009000u);
  // Not past the start of MEM1, not past the end of MEM1 / MEM2, never across the gap between them.
  padded = PadRanges({{0x80000000, 0x80001000}, {0x817FF000, 0x81800000}, {0x90000000, 0x90001000}, {0x93FFF000, 0x94000000}}, 0x3000);
  ASSERT_EQ(padded.size(), 4u);
  EXPECT_EQ(padded[0].start, 0x80000000u);
  EXPECT_EQ(padded[0].end, 0x80004000u);
  EXPECT_EQ(padded[1].start, 0x817FC000u);
  EXPECT_EQ(padded[1].end, 0x81800000u);
  EXPECT_EQ(padded[2].start, 0x90000000u);
  EXPECT_EQ(padded[2].end, 0x90004000u);
  EXPECT_EQ(padded[3].start, 0x93FFC000u);
  EXPECT_EQ(padded[3].end, 0x94000000u);
  // Zero pad = merge only; ranges outside both regions are untouched; an empty range disappears.
  padded = PadRanges({{0x2000, 0x3000}, {0x2800, 0x3800}, {0x80001000, 0x80001000}}, 0);
  ASSERT_EQ(padded.size(), 1u);
  EXPECT_EQ(padded[0].start, 0x2000u);
  EXPECT_EQ(padded[0].end, 0x3800u);
  padded = PadRanges({{0x2000, 0x3000}}, 0x1000);
  ASSERT_EQ(padded.size(), 1u);
  EXPECT_EQ(padded[0].start, 0x2000u);
  EXPECT_EQ(padded[0].end, 0x3000u);
}

TEST(Rev3Rollback, LearnedSummaryNamesEveryPageLearnedAfterArming)
{
  FakeGame game(false);
  game.SetLatePage(1500);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(3), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 1500)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_FALSE(sink.Contains("NOTE learned summary"));
  for (int i = 0; i < 400 && !sink.Contains("NOTE learned +"); ++i)
    game.Frame(test);
  ASSERT_TRUE(LinesContain(sink, "NOTE learned +1 pages", "new=[0x80A00000-0x80A01000]"));
  // The periodic SUMMARY line carries the summary only when it changed; Finish always writes it once.
  test.Finish();
  EXPECT_EQ(test.LearnedRanges().size(), 1u);
  EXPECT_TRUE(sink.Contains("NOTE learned summary pages=1 ranges=[0x80A00000-0x80A01000]"));
  EXPECT_TRUE(sink.Contains("NOTE learned adjacency growths=1 ranges_touching_old_set=0 learn_pad_pages=0"));
  int summaries = 0;
  for (const std::string& l : sink.lines)
    summaries += l.rfind("NOTE learned summary", 0) == 0;
  EXPECT_EQ(summaries, 1);
}

TEST(Rev3Rollback, LearnedSummaryIsWrittenEvenWhenNothingWasLearned)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2), {}, game.Mem(), sink, FakeClock());
  for (int i = 0; i < 400; ++i)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  test.Finish();
  EXPECT_TRUE(sink.Contains("NOTE learned summary pages=0 ranges=[]"));
}

TEST(Rev3Rollback, RateLimitedLearnLinesStillNameEveryPageInTheSummary)
{
  FakeGame game(false);
  game.SetWalker(4400, 22 * 0x1000);  // see LearningWhileArmedIsRateLimitedInTheLog: 60 growths in a row, each 22 pages further on
  FakeSink sink;
  SyncTest test(AdaptiveOptions(2, 1024, 3), {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 4400 + 1100)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  test.Finish();
  // Every learned page is in the summary, whether or not its own line was logged: the walker's pages are 22 pages apart, so one range each.
  EXPECT_EQ(RangesPages(test.LearnedRanges()), test.Stats().pages_learned_armed);
  EXPECT_GE(test.LearnedRanges().size(), 50u);
  std::string summary;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("NOTE learned summary", 0) == 0)
      summary = l;
  }
  EXPECT_NE(summary.find("0x80C00000-0x80C01000"), std::string::npos) << summary;
  EXPECT_EQ(summary.find("more"), std::string::npos) << summary;  // far under MAX_LEARN_SUMMARY_RANGES
  // A "NOTE learned +n" line is capped (16 ranges named, the rest counted) and the pages named in all of them are the learned ones.
  size_t named = 0;
  for (const std::string& l : sink.lines)
  {
    if (l.rfind("NOTE learned +", 0) != 0)
      continue;
    const size_t at = l.find("new=[");
    ASSERT_NE(at, std::string::npos) << l;
    const size_t close = l.find(']', at);
    ASSERT_NE(close, std::string::npos) << l;
    const std::string list = l.substr(at + 5, close - at - 5);
    named += static_cast<size_t>(std::count(list.begin(), list.end(), '-'));
  }
  EXPECT_GE(named, 10u);
}

TEST(Rev3Rollback, LearnPadTakesTheNeighbouringPagesAlong)
{
  for (const u32 pad : {0u, 1u, 2u})
  {
    FakeGame game(false);
    game.SetLatePage(1500);
    FakeSink sink;
    SyncTestOptions o = AdaptiveOptions(3);
    o.learn_pad_pages = pad;
    SyncTest test(o, {}, game.Mem(), sink, FakeClock());
    while (game.Counter() < 1500)
      game.Frame(test);
    ASSERT_TRUE(test.Active());
    for (int i = 0; i < 400 && !sink.Contains("NOTE learned +"); ++i)
      game.Frame(test);
    ASSERT_TRUE(sink.Contains("NOTE learned +"));
    EXPECT_TRUE(SetContains(test.Ranges(), LATE_PAGE));
    EXPECT_EQ(SetContains(test.Ranges(), LATE_PAGE - 0x1000), pad >= 1) << pad;
    EXPECT_EQ(SetContains(test.Ranges(), LATE_PAGE + 0x1000), pad >= 1) << pad;
    EXPECT_EQ(SetContains(test.Ranges(), LATE_PAGE + 0x2000), pad >= 2) << pad;
    EXPECT_FALSE(SetContains(test.Ranges(), LATE_PAGE + 0x3000)) << pad;
    // The log names the whole learned run, pad included.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "new=[0x%08X-0x%08X]", LATE_PAGE - pad * 0x1000, LATE_PAGE + 0x1000 + pad * 0x1000);
    const std::string expect = buf;
    EXPECT_TRUE(LinesContain(sink, "NOTE learned +1 pages", expect)) << pad;
  }
}

TEST(Rev3Rollback, LearnPadMakesTheNeighbourPageFreeToLearnLater)
{
  // A page next to a learned one that starts to change later is already in the set: no relayout, no cleared history.
  FakeGame game(false);
  game.SetLatePage(1500);
  FakeSink sink;
  SyncTestOptions o = AdaptiveOptions(3);
  o.learn_pad_pages = 1;
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 1500)
    game.Frame(test);
  for (int i = 0; i < 400 && !sink.Contains("NOTE learned +"); ++i)
    game.Frame(test);
  ASSERT_TRUE(sink.Contains("NOTE learned +"));
  for (int i = 0; i < 30; ++i)
    game.Frame(test);
  const size_t bytes = test.SnapshotBytes();
  const u32 growths = static_cast<u32>(test.LearnedRanges().size());
  game.Mem().SetU32(LATE_PAGE + 0x1000, 1);  // the neighbour page changes for the first time
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  EXPECT_EQ(test.SnapshotBytes(), bytes);
  EXPECT_EQ(test.LearnedRanges().size(), growths);
}

// ---- restore verify (REV3_RB_VERIFY) ---------------------------------------------------------------------------------------------

namespace
{
VerifyOptions VOpts(u32 restores, u32 every, u32 slots = 1, bool heal = false)
{
  VerifyOptions v;
  v.restores = restores;
  v.every = every;
  v.slots = slots;
  v.heal = heal;
  return v;
}

// A verifier over a memory with MEM1 + MEM2 and a known set of changes after the shadow of frame 100 was taken.
struct VerifierRig
{
  explicit VerifierRig(bool heal = false, bool peek = true) : mem(true), verifier(VOpts(VERIFY_ALL, 1, 1, heal), mem, sink, FakeClock())
  {
    mem.peek_enabled = peek;
    mem.SetU32(0x80010000, 0x11111111);  // something non-zero in the shadow
    mem.SetU32(0x90030000 + 5 * 32 + 8, 0x22222222);
    EXPECT_TRUE(verifier.TakeShadow(100));
    mem.SetU32(0x80010020 + 4, 0xAAAA0001);         // block 0x80010020
    mem.SetU32(0x80010100 + 28, 0xAAAA0002);        // block 0x80010100, same page
    mem.SetU32(0x80021000, 0xAAAA0003);             // another page, another 64 KB region
    mem.SetU32(0x90030000 + 5 * 32 + 8, 0xAAAA0004);  // MEM2
    mem.SetU32(0x80030040, 0xAAAA0005);             // inside the skip range below
    in.from_frame = 103;
    in.to_frame = 100;
    in.skip = {{0x80030000, 0x80031000}};
    in.in_set = {{0x80010000, 0x80011000}};
  }
  FakeMemory mem;
  FakeSink sink;
  RestoreVerifier verifier;
  VerifyInput in;
};
}  // namespace

TEST(Rev3Rollback, VerifierCountsBlocksPagesAndRegionsExactly)
{
  for (const bool peek : {true, false})  // with and without a direct host pointer (the Read() fallback)
  {
    VerifierRig rig(false, peek);
    const VerifyResult r = rig.verifier.Check(rig.in);
    ASSERT_TRUE(r.checked);
    EXPECT_EQ(r.wrong_blocks, 4u) << "peek=" << peek;
    EXPECT_EQ(r.wrong_pages, 3u);
    EXPECT_EQ(r.in_set_blocks, 2u);
    EXPECT_EQ(r.outside_set_blocks, 2u);
    EXPECT_EQ(r.first_addr, 0x80010020u);
    EXPECT_EQ(r.first_snapshot[1], 0u);
    EXPECT_EQ(r.first_memory[1], 0xAAAA0001u);
    EXPECT_TRUE(r.has_first_in_set);
    EXPECT_EQ(r.first_in_set_addr, 0x80010020u);
    ASSERT_EQ(r.regions.size(), 3u);
    EXPECT_EQ(r.regions[0].base, 0x80010000u);
    EXPECT_EQ(r.regions[0].blocks, 2u);
    EXPECT_EQ(r.regions[0].pages, 1u);
    EXPECT_EQ(r.regions[1].base, 0x80020000u);
    EXPECT_EQ(r.regions[2].base, 0x90030000u);
    ASSERT_EQ(r.outside_pages.size(), 2u);
    EXPECT_EQ(r.outside_pages[0].start, 0x80021000u);
    EXPECT_EQ(r.outside_pages[0].end, 0x80022000u);
    EXPECT_EQ(r.outside_pages[1].start, 0x90030000u);
    EXPECT_EQ(r.skipped_bytes, 0x1000u);
    EXPECT_EQ(r.compared_bytes, 0x5800000u - 0x1000u);  // 24 MB + 64 MB minus the skipped page
    EXPECT_EQ(r.healed_blocks, 0u);
    EXPECT_EQ(rig.verifier.DistinctBlocks(), 4u);
    EXPECT_EQ(rig.verifier.DistinctPages(), 3u);
    EXPECT_EQ(rig.verifier.Checked(), 1u);
    EXPECT_EQ(rig.verifier.Wrong(), 1u);
    EXPECT_EQ(rig.verifier.FirstWrongFrom(), 103u);
    EXPECT_EQ(rig.verifier.FirstWrongTo(), 100u);
    EXPECT_EQ(rig.verifier.FirstWrongBlocks(), 4u);
    EXPECT_EQ(rig.mem.U32(0x80021000), 0xAAAA0003u) << "no heal: memory is left as it is";
    // Checking again counts the restore again but no new distinct blocks.
    const VerifyResult again = rig.verifier.Check(rig.in);
    EXPECT_EQ(again.wrong_blocks, 4u);
    EXPECT_EQ(rig.verifier.DistinctBlocks(), 4u);
    EXPECT_EQ(rig.verifier.WrongBlocksTotal(), 8u);
    EXPECT_EQ(rig.verifier.FirstWrongBlocks(), 4u) << "the first wrong restore stays the first";
  }
}

TEST(Rev3Rollback, VerifierHealCopiesTheShadowBackOverWrongBlocksOnly)
{
  VerifierRig rig(true);
  const VerifyResult r = rig.verifier.Check(rig.in);
  EXPECT_EQ(r.wrong_blocks, 4u);
  EXPECT_EQ(r.healed_blocks, 4u);
  ASSERT_EQ(r.healed_pages.size(), 3u);  // 0x80010000, 0x80021000, 0x90030000 (not adjacent)
  EXPECT_EQ(rig.mem.U32(0x80010024), 0u);
  EXPECT_EQ(rig.mem.U32(0x8001011C), 0u);
  EXPECT_EQ(rig.mem.U32(0x80021000), 0u);
  EXPECT_EQ(rig.mem.U32(0x90030000 + 5 * 32 + 8), 0x22222222u);
  EXPECT_EQ(rig.mem.U32(0x80030040), 0xAAAA0005u) << "the skipped range is not touched";
  EXPECT_EQ(rig.mem.U32(0x80010000), 0x11111111u);
  const VerifyResult again = rig.verifier.Check(rig.in);
  EXPECT_EQ(again.wrong_blocks, 0u);
  EXPECT_EQ(rig.verifier.Checked(), 2u);
  EXPECT_EQ(rig.verifier.Wrong(), 1u);
}

TEST(Rev3Rollback, VerifierComparesPartialBlocksAtTheEdgesOfSkipRanges)
{
  FakeMemory mem;
  FakeSink sink;
  RestoreVerifier v(VOpts(VERIFY_ALL, 1), mem, sink, FakeClock());
  ASSERT_TRUE(v.TakeShadow(7));
  VerifyInput in;
  in.to_frame = 7;
  in.skip = {{0x80000010, 0x80000030}};  // starts and ends in the middle of 32-byte blocks
  mem.SetU32(0x80000014, 1);             // inside the skip range: invisible
  mem.SetU32(0x80000028, 1);             // same
  EXPECT_EQ(v.Check(in).wrong_blocks, 0u);
  mem.SetU32(0x8000000C, 1);             // block 0x80000000, outside the skip range
  mem.SetU32(0x80000030, 1);             // block 0x80000020, outside the skip range (the block is split by the skip range)
  const VerifyResult r = v.Check(in);
  EXPECT_EQ(r.wrong_blocks, 2u);
  EXPECT_EQ(r.first_addr, 0x80000000u);
  EXPECT_EQ(r.first_memory[3], 1u);
}

TEST(Rev3Rollback, VerifierShadowSlotsRotateAndTheBudgetEndsTheCopying)
{
  FakeMemory mem;
  FakeSink sink;
  RestoreVerifier v(VOpts(2, 4, 2), mem, sink, FakeClock());
  EXPECT_TRUE(v.WantsShadow(8));
  EXPECT_FALSE(v.WantsShadow(9));
  EXPECT_FALSE(v.HasShadow(8));
  ASSERT_TRUE(v.TakeShadow(8));
  ASSERT_TRUE(v.TakeShadow(12));
  EXPECT_TRUE(v.HasShadow(8));
  EXPECT_TRUE(v.HasShadow(12));
  ASSERT_TRUE(v.TakeShadow(16));  // slot of frame 8
  EXPECT_FALSE(v.HasShadow(8));
  EXPECT_TRUE(v.HasShadow(12));
  EXPECT_TRUE(v.HasShadow(16));
  EXPECT_EQ(v.Shadows(), 3u);
  EXPECT_EQ(v.ShadowBytes(), 2u * 0x1800000u);  // two slots of MEM1 (this memory has no MEM2)
  VerifyInput in;
  in.to_frame = 8;
  EXPECT_FALSE(v.Check(in).checked) << "no shadow of frame 8 any more";
  EXPECT_EQ(v.Checked(), 0u) << "an unchecked restore does not use the budget";
  in.to_frame = 12;
  EXPECT_TRUE(v.Check(in).checked);
  EXPECT_TRUE(v.Active());
  in.to_frame = 16;
  EXPECT_TRUE(v.Check(in).checked);
  EXPECT_FALSE(v.Active());
  EXPECT_FALSE(v.WantsShadow(20)) << "the budget (2 verified restores) is used up: no more shadows";
  v.DropShadows();
  EXPECT_FALSE(v.HasShadow(12));
}

TEST(Rev3Rollback, VerifyEveryPicksFramesTheRestoresLandOn)
{
  EXPECT_EQ(AutoVerifyEvery(2, 1), 3u);
  EXPECT_EQ(AutoVerifyEvery(4, 1), 5u);
  EXPECT_EQ(AutoVerifyEvery(1, 2), 3u) << "P = 2 never lands on a target n - 1 with n even";
  EXPECT_EQ(AutoVerifyEvery(2, 4), 3u);
  EXPECT_TRUE(VerifyEveryCanHit(3, 2, 1));
  EXPECT_FALSE(VerifyEveryCanHit(2, 1, 2));
  EXPECT_FALSE(VerifyEveryCanHit(4, 1, 2));
  EXPECT_TRUE(VerifyEveryCanHit(4, 2, 2));
  EXPECT_FALSE(VerifyEveryCanHit(0, 2, 1));
}

namespace
{
SyncTestOptions VerifiedOptions(int k, const VerifyOptions& v, int every = 1)
{
  SyncTestOptions o = Options(k, every);
  o.verify = v;
  return o;
}

// The fake game's whitelist plus both fighter structs, so that the hidden counter's page is the only thing the set misses.
std::vector<Range> WithFighters(const FakeGame& game)
{
  std::vector<Range> r = game.Ranges();
  r.push_back({FIGHTER[0], FIGHTER[0] + 0x1800});
  r.push_back({FIGHTER[1], FIGHTER[1] + 0x1800});
  return r;
}

void RunFrames(FakeGame& game, SyncTest& test, int frames)
{
  for (int i = 0; i < frames; ++i)
    game.Frame(test);
}
}  // namespace

TEST(Rev3Rollback, RestoreVerifyNamesThePageTheWhitelistMisses)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3)), WithFighters(game), game.Mem(), sink, FakeClock());
  ASSERT_NE(test.Verifier(), nullptr);
  EXPECT_TRUE(sink.Contains("NOTE verify on: restores=all every=1 slots=3 heal=0"));
  RunFrames(game, test, 150);
  while (test.Resimulating())
    game.Frame(test);
  const RestoreVerifier& v = *test.Verifier();
  EXPECT_GT(v.Checked(), 20u);
  EXPECT_EQ(v.Wrong(), v.Checked()) << "the hidden counter outside the whitelist changes in every rolled-back frame";
  EXPECT_EQ(v.InSetBlocksTotal(), 0u) << "everything the restore rewrote is right";
  EXPECT_EQ(v.OutsideSetBlocksTotal(), v.WrongBlocksTotal());
  EXPECT_EQ(v.DistinctBlocks(), 1u);
  EXPECT_EQ(v.DistinctPages(), 1u);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u) << "the hidden counter does not feed the state in this game";
  EXPECT_TRUE(sink.Contains("VERIFY first wrong restore: at frame "));
  EXPECT_TRUE(sink.Contains("blocks of 32 bytes"));
  EXPECT_TRUE(sink.Contains("wrong_blocks=1 pages=1 in_set=0 outside_set=1 first=[0x80700000: snap "));
  EXPECT_TRUE(sink.Contains("regions64k=1 [0x80700000:1b/1p]"));
  EXPECT_TRUE(sink.Contains("missing_pages=1 [0x80700000-0x80701000]"));
  EXPECT_FALSE(sink.Contains("WARNING blocks inside"));
  EXPECT_EQ(sink.osd.size(), 1u) << "one on-screen message for the first wrong restore";
  EXPECT_FALSE(SetContains(test.Ranges(), HIDDEN)) << "without heal the set is not changed";
  EXPECT_FALSE(test.VerifyPagesPending());
  // The SUMMARY line and the final line carry the counts.
  test.Finish();
  EXPECT_TRUE(sink.Contains("restores_checked="));
  EXPECT_TRUE(sink.Contains("restores_wrong="));
  EXPECT_TRUE(sink.Contains("first_wrong_frame="));
  EXPECT_TRUE(sink.Contains("shadow_ms="));
  EXPECT_TRUE(sink.Contains("VERIFY summary restores_checked="));
  EXPECT_TRUE(sink.Contains("distinct_blocks=1 in distinct_pages=1"));
}

TEST(Rev3Rollback, RestoreVerifyHealRepairsTheFirstRestoreAndLearnsThePage)
{
  // The hidden counter feeds the state (an incomplete whitelist): without heal every replay differs.
  {
    FakeGame game(true);
    FakeSink sink;
    SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3, false)), WithFighters(game), game.Mem(), sink, FakeClock());
    RunFrames(game, test, 150);
    EXPECT_GT(test.Stats().mismatching_frames, 5u);
  }
  FakeGame game(true);
  FakeSink sink;
  SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3, true)), WithFighters(game), game.Mem(), sink, FakeClock());
  RunFrames(game, test, 150);
  while (test.Resimulating())
    game.Frame(test);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u) << "the first restore was healed, so the replay agreed";
  EXPECT_FALSE(sink.Contains("MISMATCH"));
  EXPECT_TRUE(SetContains(test.Ranges(), HIDDEN)) << "the page joined the snapshot set";
  EXPECT_EQ(test.Verifier()->Wrong(), 1u) << "after the relayout the page is rewound by the snapshot itself";
  EXPECT_GT(test.Verifier()->Checked(), 10u);
  EXPECT_EQ(test.Stats().verify_pages_added, 1u);
  EXPECT_TRUE(sink.Contains("NOTE learned +1 pages (now ranges="));
  EXPECT_TRUE(sink.Contains("new=[0x80700000-0x80701000] (restore verify heal)"));
  EXPECT_TRUE(sink.Contains("healed; added to the snapshot set at the next layout"));
  EXPECT_FALSE(test.VerifyPagesPending());
  ASSERT_EQ(test.VerifyExtraRanges().size(), 1u);
  EXPECT_EQ(test.VerifyExtraRanges()[0].start, 0x80700000u);
  // The learned-page bookkeeping (and learned_include.py's input) knows the page.
  test.Finish();
  EXPECT_TRUE(sink.Contains("NOTE learned summary pages=1 ranges=[0x80700000-0x80701000]"));
}

TEST(Rev3Rollback, RestoreVerifyOfACompleteAdaptiveSetIsClean)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTestOptions o = AdaptiveOptions(2);
  o.verify = VOpts(15, 1, 3);
  SyncTest test(o, {}, game.Mem(), sink, FakeClock());
  while (game.Counter() < 1600 && test.Verifier()->Checked() < 15)
    game.Frame(test);
  ASSERT_TRUE(test.Active());
  EXPECT_EQ(test.Verifier()->Checked(), 15u);
  EXPECT_EQ(test.Verifier()->Wrong(), 0u) << "every page the game writes was learned before arming";
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_FALSE(sink.Contains("VERIFY first wrong"));
  EXPECT_TRUE(sink.Contains("VERIFY restore "));  // the first clean ones are logged as "ok"
  EXPECT_TRUE(sink.Contains(" ok wrong_blocks=0 compared_mb="));
  // The budget is used up: no more shadows are taken.
  const u32 shadows = test.Verifier()->Shadows();
  RunFrames(game, test, 30);
  EXPECT_EQ(test.Verifier()->Shadows(), shadows);
  EXPECT_EQ(test.Verifier()->Checked(), 15u);
}

TEST(Rev3Rollback, RestoreVerifySeesARestoreThatDidNotWriteBack)
{
  FakeGame game(false);
  // A bug in the restore: writes into STATE_B (inside the whitelist) are lost.
  game.Mem().drop_writes_lo = STATE_B;
  game.Mem().drop_writes_hi = STATE_B + 0x40;
  FakeSink sink;
  SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3)), WithFighters(game), game.Mem(), sink, FakeClock());
  RunFrames(game, test, 120);
  EXPECT_GT(test.Verifier()->Wrong(), 0u);
  EXPECT_GT(test.Verifier()->InSetBlocksTotal(), 0u);
  EXPECT_TRUE(sink.Contains("WARNING blocks inside the rewritten memory differ"));
  EXPECT_TRUE(sink.Contains("first_in_set=[0x") || sink.Contains("in_set=1"));
}

TEST(Rev3Rollback, RestoreVerifyLeavesOutWhatTheRestoreCutsOnPurpose)
{
  // The hardware cut and the ignore list both cover the hidden counter's page: nothing is wrong.
  for (int variant = 0; variant < 2; ++variant)
  {
    FakeGame game(false);
    FakeSink sink;
    FakeHardware hw;
    SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3)), WithFighters(game), game.Mem(), sink, FakeClock(), &hw);
    if (variant == 0)
      hw.list.push_back({"audio_dma", HIDDEN, HIDDEN + 0x1000});
    else
      test.SetVerifyIgnore({{HIDDEN, HIDDEN + 0x1000}});
    RunFrames(game, test, 120);
    ASSERT_GT(test.Verifier()->Checked(), 10u) << "variant " << variant;
    EXPECT_EQ(test.Verifier()->Wrong(), 0u) << "variant " << variant;
  }
}

TEST(Rev3Rollback, RestoreVerifyDoesNotCompareTheDeadStackBelowTheHook)
{
  // One word 0x100 below the hook's sp and one 0x6000 below it change every frame (leftovers of callees). Without a thread record only
  // stack_below (0x4000) bytes are skipped; with the running thread's stack end in the OSThread the whole dead stack is.
  for (int variant = 0; variant < 2; ++variant)
  {
    FakeGame game(false);
    if (variant == 1)
    {
      game.Mem().SetU32(0x800000E4, 0x80900000);          // OSGetCurrentThread
      game.Mem().SetU32(0x80900000 + 0x304, SP + 0x10000);  // stack base (highest)
      game.Mem().SetU32(0x80900000 + 0x308, SP - 0x8000);   // stack end (lowest)
    }
    game.SetStepHook([](FakeMemory& mem, u32 frame) {
      mem.SetU32(SP - 0x100, frame);
      mem.SetU32(SP - 0x6000, frame);
    });
    FakeSink sink;
    SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3)), WithFighters(game), game.Mem(), sink, FakeClock());
    test.SetVerifyIgnore({{HIDDEN, HIDDEN + 0x1000}});  // only the stack words are under test
    RunFrames(game, test, 120);
    ASSERT_GT(test.Verifier()->Checked(), 10u);
    if (variant == 0)
    {
      EXPECT_EQ(test.Verifier()->Wrong(), test.Verifier()->Checked()) << "the word 0x6000 below sp is outside the fallback";
      EXPECT_TRUE(sink.Contains("first=[0x807FA000:"));  // SP - 0x6000 rounded down to its 32-byte block
      EXPECT_FALSE(sink.Contains("0x807FFF00"));
    }
    else
    {
      EXPECT_EQ(test.Verifier()->Wrong(), 0u) << "the thread's stack end covers both";
    }
  }
}

TEST(Rev3Rollback, RestoreVerifyIsOffByDefaultAndCostsNothing)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), WithFighters(game), game.Mem(), sink, FakeClock());
  EXPECT_EQ(test.Verifier(), nullptr);
  RunFrames(game, test, 100);
  EXPECT_FALSE(sink.Contains("VERIFY"));
  EXPECT_FALSE(sink.Contains("restores_checked"));
  EXPECT_EQ(test.VerifyStatsFields(), "");
  test.Finish();
  EXPECT_FALSE(sink.Contains("VERIFY summary"));
}

TEST(Rev3Rollback, RestoreVerifyWarnsWhenTheShadowFramesCanNeverBeRestoreTargets)
{
  FakeGame game(false);
  FakeSink sink;
  // K=1, EVERY=2: restores go to n - 1 with n even, i.e. odd frames; shadows at multiples of 2 never match.
  SyncTest test(VerifiedOptions(1, VOpts(VERIFY_ALL, 2, 1), 2), WithFighters(game), game.Mem(), sink, FakeClock());
  EXPECT_TRUE(sink.Contains("NOTE verify: shadows at multiples of 2 never match a restore target"));
  // The automatic value avoids that.
  FakeSink sink2;
  FakeGame game2(false);
  SyncTest test2(VerifiedOptions(1, VOpts(VERIFY_ALL, 0, 1), 2), WithFighters(game2), game2.Mem(), sink2, FakeClock());
  EXPECT_TRUE(sink2.Contains("every=3 slots=1"));
  EXPECT_FALSE(sink2.Contains("never match"));
}

TEST(Rev3Rollback, RestoreVerifyDropsItsShadowsWhenTheHistoryIsCleared)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(VerifiedOptions(2, VOpts(VERIFY_ALL, 1, 3)), WithFighters(game), game.Mem(), sink, FakeClock());
  RunFrames(game, test, 100);
  ASSERT_TRUE(test.Active());
  ASSERT_GT(test.Verifier()->Shadows(), 3u);
  game.Mem().SetU32(0x8038FE50, 0);  // the fight is over: fighters lost, history cleared
  game.Frame(test);
  EXPECT_FALSE(test.Active());
  for (u32 f = 1000; f < 1200; ++f)
    EXPECT_FALSE(test.Verifier()->HasShadow(f)) << f;
}

// ---- merge of the learned-page work into the netplay path ------------------------------------------------------------------------

namespace
{
// Drives a NetRig to an installed set: the learning phase, NetPrepareArm, then `frames` captured frames; leaves `f` at the next frame.
u32 NetArmAndRun(NetRig& rig, int frames, bool late)
{
  u32 f = 1000;
  for (; f < 3000 && !rig.test.LearningDone(); ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  EXPECT_TRUE(rig.test.NetPrepareArm());
  for (int i = 0; i < frames; ++i, ++f)
  {
    if (i != 0)
      rig.Observe(f);
    EXPECT_TRUE(rig.Capture(f));
    rig.Advance(f, late);
  }
  return f;
}
}  // namespace

TEST(Rev3Rollback, NetModeNamesTheLearnedPagesWhileSlotsAreInUseAndAtTheClear)
{
  NetRig rig(AdaptiveOptions(6));
  u32 f = 1000;
  for (; f < 3000 && !rig.test.LearningDone(); ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_FALSE(rig.sink.Contains("NOTE learned +")) << "the first arm builds the set, it learns nothing";
  bool pending = false;
  for (int i = 0; i < 600 && !pending; ++i, ++f)
  {
    if (i != 0)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f, true);
    pending = rig.test.NetLayoutPending();
  }
  ASSERT_TRUE(pending);
  char expect[64];
  std::snprintf(expect, sizeof(expect), "new=[0x%08X-0x%08X]", LATE_PAGE, LATE_PAGE + 0x1000);
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +1 pages while slots are in use", expect));
  EXPECT_TRUE(LinesContain(rig.sink, "the snapshot set grows at the next arm; new=[", expect));

  rig.test.NetClear();
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +1 pages at clear (frame", expect));
  ASSERT_EQ(rig.test.LearnedRanges().size(), 1u);
  EXPECT_EQ(rig.test.LearnedRanges()[0].start, LATE_PAGE);
  // Installed once: the arm that follows learns nothing new.
  const size_t lines = rig.sink.Count("NOTE learned +");
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_EQ(rig.sink.Count("NOTE learned +"), lines);
}

TEST(Rev3Rollback, NetPrepareArmNamesPagesLearnedSinceTheLastArm)
{
  NetRig rig(AdaptiveOptions(6));
  u32 f = NetArmAndRun(rig, 600, true);
  ASSERT_TRUE(rig.test.NetLayoutPending());
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());  // what SyncTestStore::Relayout does: no NetClear before
  char expect[64];
  std::snprintf(expect, sizeof(expect), "new=[0x%08X-0x%08X]", LATE_PAGE, LATE_PAGE + 0x1000);
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +1 pages at arm (frame", expect));
  EXPECT_FALSE(rig.test.NetLayoutPending());
  EXPECT_TRUE(SetContains(rig.test.Ranges(), LATE_PAGE));
  // The summary lists it once and is written once (the second call finds nothing changed).
  const std::string note = rig.test.NetSummaryNote();
  EXPECT_TRUE(rig.sink.Contains("NOTE learned summary pages=1 ranges=[0x"));
  const size_t summaries = rig.sink.Count("NOTE learned summary");
  rig.test.NetSummaryNote();
  EXPECT_EQ(rig.sink.Count("NOTE learned summary"), summaries);
  EXPECT_EQ(note, " io_dirty_slots=0 io_dirty_hw=0 io_dirty_game=0 io_dirty_skipped=0");  // no verify, no hiding: only the disc read counters
}

TEST(Rev3Rollback, NetModeLearnedSummaryIsWrittenEvenWhenNothingWasLearned)
{
  NetRig rig(AdaptiveOptions(6));
  NetArmAndRun(rig, 10, false);
  rig.test.NetSummaryNote();
  EXPECT_TRUE(rig.sink.Contains("NOTE learned summary pages=0 ranges=[]"));
}

TEST(Rev3Rollback, NetModeAppliesTheLearnPadOption)
{
  SyncTestOptions o = AdaptiveOptions(6);
  o.learn_pad_pages = 1;
  NetRig rig(o);
  u32 f = NetArmAndRun(rig, 600, true);
  ASSERT_TRUE(rig.test.NetLayoutPending());
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_TRUE(SetContains(rig.test.Ranges(), LATE_PAGE));
  EXPECT_TRUE(SetContains(rig.test.Ranges(), LATE_PAGE - 0x1000));
  EXPECT_TRUE(SetContains(rig.test.Ranges(), LATE_PAGE + 0x1000));
  char expect[64];
  std::snprintf(expect, sizeof(expect), "new=[0x%08X-0x%08X]", LATE_PAGE - 0x1000, LATE_PAGE + 0x2000);
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +3 pages at arm (frame", expect));
}

TEST(Rev3Rollback, NetModeANewMatchBuildsItsFirstSetWithoutCallingItLearned)
{
  NetRig rig(AdaptiveOptions(6));
  NetArmAndRun(rig, 10, false);
  rig.test.NetClear();
  rig.game.Mem().SetU32(0x8038FE50, 0);  // the fight is over: fighters lost
  rig.Observe(5000);
  rig.game.Mem().SetU32(0x8038FE50, TEAM[0]);
  u32 f = 5001;
  for (; f < 9000 && !rig.test.LearningDone(); ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_FALSE(rig.sink.Contains("NOTE learned +"));
}

TEST(Rev3Rollback, NetModeRestoreVerifyHealQueuesTheMissingPageForTheNextRelayout)
{
  SyncTestOptions o = Options(3);
  o.verify = VOpts(VERIFY_ALL, 1, 4, true);
  NetRig rig(o, NetSeed(), true);  // the hidden counter feeds the game state and no seed names its page
  EXPECT_TRUE(rig.sink.Contains("NOTE verify on: restores=all every=1 slots=4 heal=1"));
  u32 f = 1000;
  for (; f < 1010; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  for (; f < 1014; ++f)
  {
    if (f != 1010)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f);
  }
  rig.Observe(f);
  EXPECT_FALSE(rig.test.NetLayoutPending());
  ASSERT_TRUE(rig.test.NetRestore(1011));
  ASSERT_NE(rig.test.Verifier(), nullptr);
  EXPECT_EQ(rig.test.Verifier()->Checked(), 1u) << "the restore was verified in the netplay path";
  EXPECT_EQ(rig.test.Verifier()->Wrong(), 1u);
  EXPECT_TRUE(rig.sink.Contains("VERIFY restore 1014 -> 1011 wrong_blocks="));
  EXPECT_TRUE(rig.test.NetLayoutPending()) << "the session must drain and relayout";
  EXPECT_TRUE(rig.sink.Contains("NOTE restore verify heal found pages missing"));
  EXPECT_FALSE(SetContains(rig.test.Ranges(), HIDDEN)) << "slots are in use: the set does not change yet";

  rig.test.NetClear();
  EXPECT_TRUE(SetContains(rig.test.Ranges(), HIDDEN));
  EXPECT_FALSE(rig.test.NetLayoutPending());
  EXPECT_EQ(rig.test.Stats().verify_pages_added, 1u);
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +1 pages at clear (frame", "new=[0x80700000-0x80701000] (1 of them found by the restore verify heal)"));
  const std::string stats = rig.test.NetStatsLine();
  EXPECT_NE(stats.find(" restores_checked=1 restores_wrong=1"), std::string::npos) << stats;
  const std::string note = rig.test.NetSummaryNote();
  EXPECT_NE(note.find(" restores_checked=1 restores_wrong=1"), std::string::npos) << note;
  EXPECT_NE(note.find(" verify_pages_added=1"), std::string::npos) << note;
  EXPECT_TRUE(rig.sink.Contains("NOTE learned summary pages=1 ranges=[0x80700000-0x80701000]"));
}

TEST(Rev3Rollback, NetModeRestoreVerifyHealPagesAreNamedAtTheArmToo)
{
  SyncTestOptions o = Options(3);
  o.verify = VOpts(VERIFY_ALL, 1, 4, true);
  NetRig rig(o, NetSeed(), true);
  u32 f = 1000;
  for (; f < 1010; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  for (; f < 1014; ++f)
  {
    if (f != 1010)
      rig.Observe(f);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetRestore(1011));
  ASSERT_TRUE(rig.test.NetPrepareArm());
  EXPECT_TRUE(LinesContain(rig.sink, "NOTE learned +1 pages at arm (frame", "new=[0x80700000-0x80701000]"));
  EXPECT_EQ(rig.test.Stats().verify_pages_added, 1u);
  EXPECT_TRUE(SetContains(rig.test.Ranges(), HIDDEN));
}

// ---- hook protocol 3: accepting it, the flag word, HELLO ---------------------------------------------------------------------------

namespace
{
std::array<u8, CMD_SIZE> RawCommand(u8 version)
{
  std::array<u8, CMD_SIZE> raw{};
  raw[0] = CMD_ID;
  raw[1] = version;
  raw[4] = 0x52, raw[5] = 0x42, raw[6] = 0x4B, raw[7] = 0x31;
  raw[11] = 9;  // frame 9
  return raw;
}

struct FlagHider final : Net::ResimHider
{
  explicit FlagHider(ResimHideFlags& f) : flags(f) {}
  void SetHidden(bool hidden) override { flags.Set(hidden); }
  ResimHideFlags& flags;
};
}  // namespace

TEST(Rev3Rollback, ProtocolsTwoAndThreeAreAcceptedAndOthersAreNot)
{
  for (const u8 version : {u8(2), u8(3)})
  {
    const auto raw = RawCommand(version);
    Command c;
    ASSERT_TRUE(ParseCommand(raw.data(), raw.size(), &c)) << int(version);
    EXPECT_EQ(c.protocol, version);
    EXPECT_EQ(c.frame, 9u);
  }
  for (const u8 version : {u8(0), u8(1), u8(4), u8(0xFF)})
  {
    const auto raw = RawCommand(version);
    Command c;
    EXPECT_FALSE(ParseCommand(raw.data(), raw.size(), &c)) << int(version);
  }
}

TEST(Rev3Rollback, ResimHideEnvValuesParse)
{
  struct Case
  {
    const char* text;
    u32 flags;
    bool ok;
  };
  const Case cases[] = {{"", HIDE_DRAW, true}, {"0", 0, true},   {"off", 0, true},  {"1", 1, true},   {"draw", 1, true}, {"DRAW", 1, true},
                        {"2", 2, true},        {"sound", 2, true}, {"3", 3, true},  {"all", 3, true},  {"4", 1, false},   {"hide", 1, false}};
  for (const Case& c : cases)
  {
    u32 flags = 99;
    EXPECT_EQ(ParseResimHide(c.text, &flags), c.ok) << c.text;
    EXPECT_EQ(flags, c.flags) << c.text;
  }
}

TEST(Rev3Rollback, TheFlagWordIsWrittenBigEndianWhileHiddenAndClearedAfterwards)
{
  FakeMemory mem;
  FakeSink sink;
  ResimHideFlags flags(mem, HIDE_ALL, &sink);
  flags.SetProtocol(PROTOCOL_VERSION_HIDE);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0u);
  flags.Set(true);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 3u);
  u8 raw[4];
  ASSERT_TRUE(mem.Read(HIDE_FLAGS_ADDR, raw, 4));
  EXPECT_EQ(raw[0], 0);
  EXPECT_EQ(raw[3], 3);
  const u32 writes = flags.Writes();
  flags.Set(true);  // already there: no second write
  EXPECT_EQ(flags.Writes(), writes);
  flags.Set(false);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0u);
  flags.Set(false);
  EXPECT_EQ(flags.Writes(), writes + 1);
  EXPECT_TRUE(sink.Contains("NOTE resim hiding v3: flags=3 (draw+sound) written to 0x8000241C"));
  EXPECT_EQ(sink.Count("NOTE resim hiding"), 1u);
}

TEST(Rev3Rollback, EachFlagValueWritesItsOwnBits)
{
  for (const u32 want : {0u, 1u, 2u, 3u})
  {
    FakeMemory mem;
    ResimHideFlags flags(mem, want);
    flags.SetProtocol(3);
    flags.Set(true);
    EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), want);
    flags.Set(false);
    EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0u);
  }
}

TEST(Rev3Rollback, ProtocolTwoNeverTouchesTheFlagWordAndSaysWhy)
{
  FakeMemory mem;
  mem.SetU32(HIDE_FLAGS_ADDR, 0x77);  // whatever a v2 hook keeps there is not ours
  FakeSink sink;
  ResimHideFlags flags(mem, HIDE_DRAW, &sink);
  flags.SetProtocol(PROTOCOL_VERSION);
  flags.Set(true);
  flags.Set(false);
  flags.Set(true);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0x77u);
  EXPECT_EQ(flags.Writes(), 0u);
  EXPECT_EQ(sink.Count("NOTE resim hiding: the hook speaks protocol 2"), 1u);
  u32 a = 0, b = 0;
  EXPECT_FALSE(flags.ReadCounters(&a, &b));
}

TEST(Rev3Rollback, TheCountersAreReadFromTheV3DataBlock)
{
  FakeMemory mem;
  ResimHideFlags flags(mem, HIDE_DRAW);
  flags.SetProtocol(3);
  mem.SetU32(HIDE_ITERATIONS_ADDR, 1234);
  mem.SetU32(HIDE_SOUNDS_ADDR, 5);
  u32 iterations = 0, sounds = 0;
  ASSERT_TRUE(flags.ReadCounters(&iterations, &sounds));
  EXPECT_EQ(iterations, 1234u);
  EXPECT_EQ(sounds, 5u);
  EXPECT_EQ(flags.SummaryFields(), " hide_protocol=3 hide_flags=1 hide_writes=0 hidden_iterations=1234 skipped_sounds=5");
}

TEST(Rev3Rollback, ResimHidingDrivesTheFlagWordFromTheRestoredReplyToTheLastResimulatedCall)
{
  FakeMemory mem;
  FakeSink sink;
  ResimHideFlags flags(mem, HIDE_DRAW, &sink);
  flags.SetProtocol(3);
  FlagHider hider(flags);
  Net::ResimHiding hiding(&hider, sink);
  hiding.Update(false);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0u);
  hiding.Update(true);  // the RESTORED reply
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 1u);
  hiding.Update(true);
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 1u);
  hiding.Update(false);  // the reply of the last resimulated frame (or the session leaving, or shutdown)
  EXPECT_EQ(mem.U32(HIDE_FLAGS_ADDR), 0u);
}

TEST(Rev3Rollback, SyncTestSetsTheFlagsOnlyWhileItResimulates)
{
  FakeGame game(false);
  game.SetProtocol(3);
  FakeSink sink;
  ResimHideFlags flags(game.Mem(), HIDE_DRAW, &sink);
  SyncTest test(Options(3), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetResimHide(&flags);
  int hidden_calls = 0;
  for (int i = 0; i < 200; ++i)
  {
    game.Frame(test);
    EXPECT_EQ(game.Mem().U32(HIDE_FLAGS_ADDR), test.Resimulating() ? 1u : 0u) << i;
    hidden_calls += test.Resimulating() ? 1 : 0;
  }
  EXPECT_GT(hidden_calls, 20) << "SyncTest rolled back and resimulated";
  EXPECT_GE(flags.Writes(), 4u);
  game.Mem().SetU32(0x8038FE50, 0);  // losing the fight in the middle of things clears the history; the word must follow
  game.Frame(test);
  EXPECT_EQ(game.Mem().U32(HIDE_FLAGS_ADDR), 0u);
  test.Finish();
  EXPECT_EQ(game.Mem().U32(HIDE_FLAGS_ADDR), 0u);
  EXPECT_TRUE(sink.Contains("SUMMARY final"));
  EXPECT_TRUE(LinesContain(sink, "SUMMARY final", " hide_protocol=3 hide_flags=1"));
}

TEST(Rev3Rollback, SyncTestWithHooksOfProtocolTwoWritesNothing)
{
  FakeGame game(false);
  FakeSink sink;
  ResimHideFlags flags(game.Mem(), HIDE_ALL, &sink);
  SyncTest test(Options(3), game.Ranges(), game.Mem(), sink, FakeClock());
  test.SetResimHide(&flags);
  for (int i = 0; i < 150; ++i)
    game.Frame(test);
  EXPECT_EQ(flags.Writes(), 0u);
  EXPECT_EQ(game.Mem().U32(HIDE_FLAGS_ADDR), 0u);
  EXPECT_TRUE(sink.Contains("NOTE resim hiding: the hook speaks protocol 2"));
}

TEST(Rev3Rollback, TheHideCountersGoIntoTheNetSummaryNote)
{
  NetRig rig(AdaptiveOptions(6));
  ResimHideFlags flags(rig.game.Mem(), HIDE_DRAW);
  flags.SetProtocol(3);
  rig.test.SetHideCounters(&flags);
  rig.game.Mem().SetU32(HIDE_ITERATIONS_ADDR, 42);
  const std::string note = rig.test.NetSummaryNote();
  EXPECT_NE(note.find(" hide_protocol=3 hide_flags=1 hide_writes=0 hidden_iterations=42 skipped_sounds=0"), std::string::npos) << note;
}

// ---- slots captured during a disc read are never rollback targets ----------------------------------------------------------------------

namespace
{
constexpr u32 DVD_BLOCK = 0x80415640;  // some command block the game's DVD library is executing
}

TEST(Rev3Rollback, TheGamesDvdLibraryViewIsReadFromTheExecutingPointer)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  EXPECT_FALSE(test.GameDvdBusy());                          // 0: idle
  game.Mem().SetU32(DVD_EXECUTING_ADDR, DVD_DUMMY_BLOCK);
  EXPECT_FALSE(test.GameDvdBusy());                          // the dummy block: between two commands
  game.Mem().SetU32(DVD_EXECUTING_ADDR, 0x12345678);
  EXPECT_FALSE(test.GameDvdBusy());                          // not a pointer into MEM1 / MEM2: not the DVD library's
  game.Mem().SetU32(DVD_EXECUTING_ADDR, DVD_BLOCK);
  EXPECT_TRUE(test.GameDvdBusy());
  EXPECT_TRUE(test.DiscBusy());
}

TEST(Rev3Rollback, NetModeRecordsWhetherADiscReadWasInFlightAtEveryCapture)
{
  NetRig rig(Options(6), NetSeed());
  u32 f = 1000;
  for (; f < 1010; ++f)
  {
    rig.Observe(f);
    rig.Advance(f);
  }
  rig.Observe(f);
  ASSERT_TRUE(rig.test.NetPrepareArm());
  for (; f < 1016; ++f)
  {
    if (f != 1010)
      rig.Observe(f);
    // frame 1012: the game's DVD library has a command executing; frame 1014: only the hardware view would say so (not wired here)
    rig.game.Mem().SetU32(DVD_EXECUTING_ADDR, (f == 1012 || f == 1013) ? DVD_BLOCK : 0);
    ASSERT_TRUE(rig.Capture(f));
    rig.Advance(f);
  }
  for (u32 g = 1010; g < 1016; ++g)
    EXPECT_EQ(rig.test.NetClean(g), !(g == 1012 || g == 1013)) << g;
  EXPECT_TRUE(rig.test.NetClean(5)) << "no slot: nothing to call dirty";
  EXPECT_EQ(rig.test.Stats().io_dirty_slots, 2u);
  EXPECT_EQ(rig.test.Stats().io_dirty_game, 2u);
  EXPECT_EQ(rig.test.Stats().io_dirty_hw, 0u);
  EXPECT_TRUE(rig.sink.Contains("NOTE slot of frame 1012 captured with a disc read in flight (hardware=0 game_dvd=1)"));
  const std::string note = rig.test.NetSummaryNote();
  EXPECT_NE(note.find(" io_dirty_slots=2 io_dirty_hw=0 io_dirty_game=2 io_dirty_skipped=0"), std::string::npos) << note;
  EXPECT_NE(rig.test.NetStatsLine().find(" io_dirty_slots=2 "), std::string::npos);
  // A new capture of the same frame (a resimulation) replaces the record.
  rig.game.Mem().SetU32(DVD_EXECUTING_ADDR, 0);
  rig.Observe(1016);
  ASSERT_TRUE(rig.Capture(1013));
  EXPECT_TRUE(rig.test.NetClean(1013));
  EXPECT_FALSE(rig.test.NetClean(1012));
  // A slot that is overwritten or cleared is not dirty any more.
  rig.test.NetClear();
  EXPECT_TRUE(rig.test.NetClean(1012));
}

TEST(Rev3Rollback, TheHardwareViewMarksSlotsDirtyToo)
{
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 80; ++i)
    game.Frame(test);
  hw.disc_busy = true;
  for (int i = 0; i < 4; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Stats().io_dirty_slots, 4u);
  EXPECT_EQ(test.Stats().io_dirty_hw, 4u);
  EXPECT_EQ(test.Stats().io_dirty_game, 0u);
}

TEST(Rev3Rollback, SyncTestDoesNotRestoreASlotCapturedDuringADiscRead)
{
  // K = 2, every frame: with the disc busy for 3 frames the slots of those frames are dirty. Once idle again the rollbacks that would restore
  // them are skipped (no earlier slot exists in SyncTest's ring), the others run, and nothing mismatches.
  FakeGame game(false);
  FakeSink sink;
  FakeHardware hw;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock(), &hw);
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  while (test.Resimulating())
    game.Frame(test);
  hw.disc_busy = true;
  for (int i = 0; i < 3; ++i)
    game.Frame(test);
  hw.disc_busy = false;
  const u32 rollbacks = test.Stats().rollbacks;
  for (int i = 0; i < 40; ++i)
    game.Frame(test);
  EXPECT_GE(test.Stats().io_dirty_skipped, 1u);
  EXPECT_LE(test.Stats().io_dirty_skipped, 3u);
  EXPECT_GT(test.Stats().rollbacks, rollbacks + 5);
  EXPECT_EQ(test.Stats().mismatching_frames, 0u);
  EXPECT_FALSE(sink.Contains("ERROR"));
  test.Finish();
  EXPECT_TRUE(LinesContain(sink, "SUMMARY final", " io_dirty_slots=3 io_dirty_hw=3 io_dirty_game=0 io_dirty_skipped="));
}

TEST(Rev3Rollback, TheGameViewPostponesSyncTestRollbacksLikeTheHardwareView)
{
  FakeGame game(false);
  FakeSink sink;
  SyncTest test(Options(2), game.Ranges(), game.Mem(), sink, FakeClock());
  for (int i = 0; i < 100; ++i)
    game.Frame(test);
  while (test.Resimulating())
    game.Frame(test);
  const u32 rollbacks = test.Stats().rollbacks;
  game.Mem().SetU32(DVD_EXECUTING_ADDR, DVD_BLOCK);
  for (int i = 0; i < 10; ++i)
    game.Frame(test);
  EXPECT_EQ(test.Stats().rollbacks, rollbacks);
  EXPECT_EQ(test.Stats().io_postponed, 10u);
}
