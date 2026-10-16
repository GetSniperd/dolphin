# Rev3 rollback fork (branch `rev3`)

Slippi Mainline Dolphin with rollback netcode for Naruto Clash of Ninja Revolution 3 (Wii, game ID RNE). The plan, the Rev3
addresses and the test tools live in GetSniperd/rev3-mod: `docs/rollback_fork_plan.md` (the change list), `rev3mod/rbhook.py`
(the game-side hook this emulator answers over EXI), `rev3mod/rbchecksum.py` (the desync checksum), `tools/rollback/` (probes).

- No game data in this repo: code, settings and documented addresses only.
- Build: `.github/workflows/rev3-windows.yml` runs on every push to `rev3` / `rev3-*` (and by hand from the Actions tab); the
  run's `rev3-dolphin-windows` artifact is a portable zip with Slippi_Dolphin.exe. Slippi's own multi-platform `main.yml` is
  removed on this branch to keep runs short; `slippi` keeps it.
- `Data/Sys/GameSettings/RNE.ini`: lets mainline boot Rev3 (BootManager.cpp:69-76 needs a default game INI).

## SyncTest (rollback self test, no netplay)

`Source/Core/Core/Slippi/Rev3Rollback.{h,cpp}` answers the game's per-frame EXI hook (a 64-byte command, id 0xA0, "RBK1";
reply "RBR1"; protocol v2 in rev3-mod `rev3mod/rbhook.py`). Without SyncTest it only echoes the game's own pads. With SyncTest,
every frame (or every Nth) the emulator restores the memory of K frames ago, makes the game replay those K frames with the
recorded inputs and compares the fighters' 61 fields and a CRC32 of every whitelist range with the first run. Any difference
means the snapshot set is incomplete or the hook point is not equivalent. It only runs once armed: both fighters found for
`REV3_SYNCTEST_ARM` consecutive hook calls (default 600 = 10 s, so the stage has finished loading), or toggled by P1's Z
button, and the adaptive set has finished learning (below); never in menus or loading.

### Adaptive snapshot set (default)

A fixed page list cannot work: the heap layout differs from match to match. The first whitelist (learned by page churn in one
match) had P2's fighter struct at 0x92E8B060; in the next match it was at 0x92D8B060, outside the snapshot, so P2's fields were
never rewound (`NOTE field coverage p2 ... in_snapshot=0/61`). So the snapshot set is learned in the running match
(`REV3_SYNCTEST_ADAPTIVE`, default 1):

- From the first hook call with both fighters found (long before arming) a page tracker hashes every 4 KB page of MEM1
  (0x80000000-0x81800000) and MEM2 (0x90000000-0x94000000), `1/REV3_SYNCTEST_SCAN_SLICES` of them per hook call (default 16, so
  every page is re-hashed every 16 calls; 22528 pages, 5.5 MB per call). The hash is a plain 64-bit 8-lane multiply/rotate hash
  over the 4 KB (XXH3 is not linked into Core); pages are read in place (`GetPointerForRange`, no copy). A page whose hash
  changed between two of its scans is dynamic, for good. The first scan cycle only records baselines, so `LEARN=20` means 19
  comparisons.
- Snapshot set = `REV3_WHITELIST` ranges (now an optional seed) + dynamic pages + both fighter structs (fighter .. +0x1800, so all
  61 compared fields are always inside) + the hook's stack window (as of arming), merged into sorted ranges, minus the hook region
  0x80002400-0x80002A00, the `REV3_SYNCTEST_EXCLUDE` ranges and every hardware exclusion (the cuts win over everything).
- It arms only after `REV3_SYNCTEST_LEARN` full scan cycles (default 20 = 320 hook calls) with the fighters present. With
  `ARM=pad` a Z press before that logs `NOTE arm deferred: learning (n/20 cycles)` and arms by itself when learning completes
  (a second Z before then cancels); with `ARM=N` it arms when both N calls and the learning are done. At arm it checks that all 61
  fields of both fighters are inside the set; if not (an exclusion cuts a fighter struct) it logs an `ERROR` and does not arm.
- Learning goes on while armed, on normal (first-run) frames only: resimulated frames are not scanned, and a restore resets the
  hash baseline of the pages it rewrote. A page outside the set that changes is added: the buffers are rebuilt, the history is
  cleared (no rollback until K new frames are captured) and `NOTE learned +n pages` is logged.
- Losing the fighters (loading) forgets everything learned; the next match learns from scratch.
- Scan cost: every HB and SUMMARY line shows it (`scan_us`). On a shared 2.1 GHz Xeon VM it measured 0.6-1.2 ms per call at 16
  slices (about 20 GB/s on cached data, 5-10 GB/s streaming over 88 MB); a desktop core is 2-3 times quicker. Use
  `REV3_SYNCTEST_SCAN_SLICES=32` to halve it (learning then takes twice as many calls).
- Pages that churn while the stage loads are learned too (sticky), so a long load right before arming makes the set larger;
  `dyn=` in the HB lines and `NOTE adaptive set at arm: dynamic_pages=.. bytes=..` show how large. Captures no longer CRC the whole
  set (the CRCs in a MISMATCH block are computed for the differing ranges only).

Run (set the variables before starting Slippi_Dolphin.exe; the patched game with the v2 hook must be what boots):

    set REV3_SYNCTEST=2
    rem optional seed (the snapshot set is learned in the match; without this line it starts empty):
    set REV3_WHITELIST=D:\Project_A_ExtraData\Research\rev3mod\whitelist.json
    Slippi_Dolphin.exe -e <Rev3 iso or Riivolution-patched boot>

| variable | meaning |
|---|---|
| `REV3_SYNCTEST=K` | 1..8 frames to roll back; absent or 0 = off |
| `REV3_WHITELIST=path` | whitelist.json `{"ranges":[{"start":"0x80...","end":"0x80..."}]}`, hex strings or numbers, end exclusive. Optional seed with the adaptive set (unset = learn everything; set but unreadable = SyncTest off); required with `REV3_SYNCTEST_ADAPTIVE=0` |
| `REV3_SYNCTEST_ADAPTIVE=0` | do not learn the snapshot set in the match; use the whitelist as is (default 1 = adaptive, see above) |
| `REV3_SYNCTEST_SCAN_SLICES=N` | adaptive: each hook call hashes 1/N of all pages (1..4096, default 16) |
| `REV3_SYNCTEST_LEARN=N` | adaptive: full scan cycles with fighters present before SyncTest may arm (0..1000000, default 20; a cycle is SCAN_SLICES calls, the first only records baselines) |
| (automatic) | the hook's own region 0x80002400-0x80002A00 is cut out of every whitelist range (logged as a NOTE); it must survive a restore |
| `REV3_SYNCTEST_EVERY=N` | roll back when `frame % N == 0` (default 1 = every frame) |
| `REV3_STACK_WINDOW=bytes` | bytes saved from the hook's sp upward (default 0x4000) |
| `REV3_SYNCTEST_ARM=N` or `pad` | N = arm after N consecutive hook calls with both fighters found (default 600; fighters exist while a stage is still loading, 60 was too short and froze the first real run). `pad` = arm when P1 presses Z (PADStatus button 0x0010, edge triggered) while fighters are present, disarm on the next press |
| `REV3_SYNCTEST_NOOP=1` | diagnosis: once armed, every frame capture S[n] and immediately restore S[n] (same frame), reply RESTORED with frame n and inputs n. Memory is unchanged in effect, but the game hook's RESTORED path (register reload from its stack frame) runs every frame. No comparisons. If the game runs fine in this mode the hook is cleared and the freeze comes from the memory rewind itself |
| `REV3_SYNCTEST_HW_EXCLUDE=0` | do not cut hardware-DMA regions out of the snapshot (default on, see below) |
| `REV3_SYNCTEST_ICACHE=0` | do not invalidate the JIT for restored ranges (default on) |
| `REV3_SYNCTEST_AUDIO_HW=0` | do not rewind the emulated audio hardware (DSP, AI, their CoreTiming events, the PI DSP / AI interrupt bits) together with memory; default 1 = rewind it (see "Audio hardware rewind") |
| `REV3_SYNCTEST_TB=1` | also store the emulated time base in every snapshot and set it back on restore (default 0, see "Time base" below) |
| `REV3_SYNCTEST_EXCLUDE=path` | optional JSON `{"ranges":[{"start","end"}]}` (same format as the whitelist) subtracted from the snapshot after the whitelist is loaded; each range is logged as `NOTE exclude user 0x..-0x.. (n bytes were in the whitelist)`. Use it to test excluding audio / scratch ranges without editing the whitelist. A bad file or one that removes everything (classic mode only) turns SyncTest off with an error. Entries may be pointer based, `{"ptr":"0x8049D000","size":"0x3F0000","note":"nw4r sound system"}`: the u32 at `ptr` is read when SyncTest arms and at every exclusion re-check (every 600 hook calls) and `[value, value+size)` is excluded (for heaps the game allocates at run time: sound system work block `*0x8049D000` 0x3F0000 bytes, Home Button sound block `*0x80461B50` 0x18700 bytes). Logged as `NOTE exclude ptr entry 0x.. size=.. note=..` once, then `NOTE exclude ptr 0x8049D000 -> 0xS-0xE (n bytes were in the whitelist)` per resolved region (a region that moves adds a cut, the old one stays) or `NOTE exclude ptr 0x.. skipped: pointer is 0 | pointer is outside MEM1 / MEM2 (value 0x..)` once per value |
| `REV3_EF_POOL=0` | do not force the nw4r::ef effect pool into the snapshot at arm (default 1, see "Effect pool" below) |
| `REV3_SNAPSHOT_INCLUDE=path` | optional JSON, the very entry formats of `REV3_SYNCTEST_EXCLUDE` (`{"start","end"}` ranges, `{"ptr","size","note"}` pointer entries): these regions are forced INTO the snapshot set at every arm, on top of the learned pages (and of the ef pool). Logged `NOTE include user 0x..-0x.. (n bytes ...)`, `NOTE include ptr entry ..` at start and `NOTE include ptr 0x.. -> 0xS-0xE (n bytes) note=..` / `NOTE include ptr 0x.. skipped: pointer is 0 \| outside MEM1 / MEM2` at arm, then `NOTE forced include: ranges=.. bytes=..; the snapshot set grows by n bytes`. The cuts (hook region, `REV3_SYNCTEST_EXCLUDE`, hardware regions) still win. A bad file turns SyncTest off with an error (like the exclude file). Use it to test a suspicion without rebuilding |
| `REV3_SNAPSHOT_PAD=n` | 0..16, default 0 = off. Every page the tracker learns also pulls n neighbouring 4 KB pages on each side into the snapshot set (clipped to MEM1 / MEM2, cuts still win; `NOTE learn pad pages=n ..` at start). Costs snapshot bytes (up to 2n pages per learned run), saves relayouts when a game block spans pages that start to change one after the other. Whether it would have helped a run is in `NOTE learned adjacency` (below) |
| `REV3_EFCHECK=N\|all\|0` | the ef list check around restores (see "Effect pool"): unset = the first 200 restores, `N` = the first N, `all` = every restore, `0` = off |
| `REV3_SYNCTEST_LOG=path` | log file (default `<User>\Logs\rev3_synctest.log`), appended, flushed per line |

Hardware regions (computed from Dolphin's emulated hardware when SyncTest arms and re-checked every 600 hook calls; the set
only grows, so double buffers end up fully excluded; a new region that overlaps the whitelist clears the history) are subtracted
from every snapshot range, because they are read or written behind the game's back and must not be rewound: the GX command
processor FIFO (`GetCommandProcessor().GetFifo()` CPBase..CPEnd), the VI XFBs (`GetVideoInterface().GetXFBAddressTop/Bottom()`,
640*576*2 bytes each, the VI size is not exposed), the audio DMA source block (DSP registers 0x0C005030/32/36 read through the MMIO
mapping, NumBlocks*32 bytes; the DSPManager's state is private), and a DVD DMA in flight (DI registers DIMAR/DILENGTH/DICR,
0x0C006014.. and on a Wii 0x0D006014.., while TSTART is set). Not reachable: Wii IOS `/dev/di` read destinations (inside IOS HLE; see "Disc read gate" below),
ARAM DMA, GPU-thread writes into texture memory (EFB copies). After the ranges are written back each one is invalidated in the
JIT (`JitInterface::InvalidateICache(addr, size, forced=true)`) in case a range holds overlays or RELs.

Log lines: `SYNCTEST start K=.. EVERY=.. window=.. whitelist=.. ranges=.. bytes=.. ... arm=N|pad noop=0|1 hw_exclude=0|1 icache=0|1 tb=0|1 adaptive=0|1 slices=.. learn=..`
once (whitelist=(none) and ranges=0 with the adaptive set and no seed); `NOTE armed at frame N (M hook calls with both fighters)` / `NOTE armed by Z at frame N` / `NOTE disarmed by Z at frame N`;
adaptive: `NOTE learning complete: 20 scan cycles (slices=16) dynamic_pages=.. (.. bytes) at frame N`, `NOTE arm deferred: learning (n/20
cycles)` (once per wish), `NOTE arm cancelled by Z at frame N`, at arm `NOTE adaptive set at arm: dynamic_pages=.. (.. bytes)
scan_cycles=.. slices=.. ranges=.. bytes=.. snapshot_bytes=..` and `NOTE snapshot now ranges=.. bytes=.. snapshot_bytes=.. slots=..`,
while armed `NOTE learned +n pages (now ranges=.. bytes=..) frame N, history cleared; new=[0xS-0xE,..]` followed by `NOTE snapshot now ...` (the first 10
growths, then at most one per 300 hook calls plus `NOTE further learning is logged at most every 300 hook calls`),
`ERROR frame N: not all fighter fields are inside the snapshot set (an exclusion cuts a fighter struct); not arming` (after the
`NOTE field coverage` lines naming the missing fields);
`NOTE exclude <what> 0xSTART-0xEND (n bytes were in the whitelist)` with `<what>` = gx_fifo, xfb_top, xfb_bottom, audio_dma,
dvd_dma, dvd_dma_wii, followed by `NOTE snapshot now ranges=.. bytes=.. snapshot_bytes=..` when the snapshot shrank;
`NOTE hardware: ...` (which accessors are used and which are not reachable); `NOTE history cleared: <why>`;
`RESTORE n -> m begin` then `RESTORE n -> m bytes=.. us=.. icache_us=..` for the first 10 restores (a log that ends right after a
`begin` line, or right after a RESTORE line, shows where the game hung: inside the restore, or after it), then one line
`RESTORE n -> m us=..` (no begin line) for restores 11..30; with `REV3_SYNCTEST_TB=1` each RESTORE line ends with `tb_delta=<ticks>`;
`HB frame=.. calls=.. rollbacks=.. noop_restores=.. dyn=<dynamic pages> scan_us=<mean over the interval> scan_us_max=<max> dsp csr=..
to_dsp_hi=.. ai_dma=on|off src=.. blocks=.. left=..` every 60 hook
calls for the first 600 armed calls, then every 300 (flushed; the last HB bounds when the game froze; see "Field coverage and DSP
state" below); `ERROR ...` (sp differs from the snapshot's: that frame is not rolled back; restore not honoured by the game);
`MISMATCH frame=N K=.. rollback=R fields=a ranges=b` followed by `  pP field ref=0x.. resim=0x..` and
`  range 0xSTART-0xEND crc ref=.. resim=.. words=n first(addr:ref>resim)= ...` (full detail for the first 20 mismatching frames,
then counts only; an on-screen message appears on the first mismatch); `SUMMARY frame=.. rollbacks=.. frames_compared=..
mismatching_frames=.. restore_us_mean=.. restore_us_max=.. icache_us_mean=.. icache_us_max=.. sp_mismatches=..
noop_restores=.. dynamic_pages=.. scan_us_mean=.. scan_us_max=..` every 600 compared frames (every 600 restores in noop mode) and `SUMMARY final` on exit. `restore_us` is the
time to write the ranges back, `icache_us` the JIT invalidation.

Narrowing a freeze, cheapest first: `REV3_SYNCTEST_ARM=pad` (arm by hand once the fight is running), `REV3_SYNCTEST_NOOP=1` (only
the hook's register reload, memory unchanged), then a real run; `REV3_SYNCTEST_HW_EXCLUDE=0` and `REV3_SYNCTEST_ICACHE=0` switch
the hardware cut-outs and the JIT invalidation off to see which one matters, and `REV3_SYNCTEST_AUDIO_HW=0` to see what the audio hardware rewind changes.

### Audio hardware rewind (`REV3_SYNCTEST_AUDIO_HW`, default on)

Why: with memory-only rewinds every resimulated frame matched (`fields=0` over 38 rollbacks, K=4), but the memory CRCs differed in the
OS/thread areas and in a sound thread's stack (pointers into the AX AUX buffers, OS/AX return addresses), and the game froze about 40
frames after arming. Excluding the sound memory made it freeze sooner (it cuts OSThreads out of the scheduler lists). The audio runs on
its own hardware clock: the AI DMA interrupt every ~5 ms, DSP mails and interrupts, sound threads woken by them. A memory rewind turns
AX's DSP handshake state back (AX believes the DSP is still busy with a frame whose "done" interrupt already happened) while the emulated
DSP and AI move on, so the audio pipeline stops and whatever waits on it hangs. So the audio hardware is rewound together with memory.

With every capture (same moment as the memory capture, in the hook's DMA handler on the CPU thread) the following is saved per
snapshot slot; a restore puts it back right after the memory write-back and the JIT invalidation, in this order:

1. **Hardware state** (`PointerWrap`, a measure pass then a write pass into the slot's byte buffer; read mode on restore):
   `DSPManager::DoRewindState` (DSP control register, audio DMA, ARAM DMA and AR registers, then `DSPEmulator::DoState`: with the HLE DSP
   the ucode CRCs, the AX ucode state, the mail handler; with LLE the DSP core) and `AudioInterfaceManager::DoRewindState` (AI control,
   volume, sample counter, interrupt timing, rate divisors). Both are new methods next to `DoState`, because `DoState` has side effects
   or costs that do not belong in a per-frame rewind: `DSPManager::DoState` writes the 16 MB ARAM on a GameCube (on a Wii ARAM is MEM2,
   which the memory snapshot rewinds), and `AudioInterfaceManager::DoState` also runs `Mixer::DoState` (all mixers, including the Wiimote
   speaker, Skylander and GBA ones) and keeps `m_last_cpu_time` as an absolute CoreTiming tick. The AI method leaves the mixer alone
   (it re-applies the streaming volume and the rate divisors only if they differ) and stores `m_last_cpu_time` as an age (now - it) that
   is rebased to the current tick on restore; otherwise the sample counter would jump forward by the CPU time that passed since the capture.
   The host-side audio (mixer buffers, sound backend) is not rewound: the speakers keep being fed.
2. **CoreTiming events of the audio devices.** `DSPCallback` (DSP time slices, periodic), `AudioDMACallback` (one 32-byte audio DMA block,
   periodic, raises the AID interrupt), `AICallback` (AI sample counter), `DSPint` (delayed DSP / AID / ARAM interrupts, userdata = type),
   `ARAMint` (ARAM DMA done). Each pending event of these types is saved as *remaining cycles* (event time - now) with its userdata
   (new `CoreTimingManager::GetPendingEvents()` and `FindEventType()`; the queue itself stays private). A restore calls
   `RemoveAllEvents` for these types and schedules the saved events again with the saved remaining cycles from now, in their old
   firing order, so the events stay in phase with the restored hardware state. The rest of CoreTiming (VI, SI, EXI, IOS, DVD, GPU
   events) and the global time are not touched, nor are the PPC registers (the game hook reloads its own).
3. **PI interrupt bits.** Only `INT_CAUSE_DSP` and `INT_CAUSE_AI` of the PI cause register are set to their saved values with
   `ProcessorInterfaceManager::SetInterrupt`, which also re-evaluates the PPC external exception the normal way. The mask and the
   other cause bits are untouched.

The state size varies a little from frame to frame (the HLE mail queue is a deque), so each capture measures first and sizes the slot's
buffer; that is logged at most 3 times as a NOTE and is not an error. It is an error (and the audio rewind switches itself off, SyncTest
carries on with memory only) when the measure and write passes disagree, when a load does not consume its buffer exactly, or when the
state is empty or larger than 4 MB. With the LLE DSP on its own thread the DSP thread is locked out (`DSPEmulator::PauseAndLock`) while the
state is read or written. Known gap: the HLE/LLE `control_reg_init_code_clear_time` is an absolute time base value; it only matters
while the DSP is being initialised, and `REV3_SYNCTEST_TB=1` moves the time base, not it.

Log lines: `NOTE audio hw rewind on (dsp=HLE|LLE, bytes=.., events=DSPCallback,AudioDMACallback,AICallback,DSPint,ARAMint)` once at start
(a type that is not registered is listed as `name(not registered)`); `NOTE audio hw rewind off (REV3_SYNCTEST_AUDIO_HW=0): memory only`;
`NOTE audio hw at arm: bytes=.. irq=0x.. events=n [DSPCallback+12345,...]` (the pending audio events with their remaining cycles) when
SyncTest arms; every `RESTORE` line (full and short) ends with ` audio_bytes=.. audio_events=n`; `NOTE audio state size a -> b bytes`;
`ERROR ...` plus `NOTE audio hw rewind off: <why>; memory only from now on` on a failure; and `audio_restores=.. audio_us_mean=..
audio_us_max=..` at the end of every SUMMARY line. `REV3_SYNCTEST_NOOP=1` also restores the audio hardware (same frame), which exercises
the save, load and event re-scheduling every frame without any rollback.

### Disc read gate

Rev3 streams BGM from disc during fights (nw4r StrmPlayer::LoadStreamData -> DVDReadPrio, synchronous, on a sound task thread, into
the 16 KB buffer at 0x80404860). On a Wii that read goes through IOS `/dev/di`, whose destination the snapshot cannot cut out. So a
rollback (and a noop restore) is postponed while a disc read is in flight: DI register TSTART set (0x0C00601C, and 0x0D00601C on a
Wii) or `DIDevice::IsBusy()` (a command executing or queued in the IOS DI device; accessor added in `Core/IOS/DI/DI.h`). The frame is
still captured, so the next idle frame rolls back normally. Logged: `NOTE rollback postponed: disc read in flight (frame N, k so
far)` for the first 5, and `io_postponed=` in every SUMMARY line. A `io_postponed` that keeps climbing means the DI is never idle.

### Field coverage and DSP state (diagnostics)

- At arm: `NOTE fighters p1=0x.. p2=0x..`, then per player `NOTE field coverage pP fighter=0x.. in_snapshot=a/61` and, when some of
  the 61 compared fields lie outside the snapshot (never rewound), ` NOT IN SNAPSHOT: name@0xADDR ...`. The line is repeated with
  ` after exclusion change` when a hardware exclusion later changes the layout.
- In a `MISMATCH` block, the first time a field name differs it also gets ` addr=0xADDR in 0xSTART-0xEND` (the snapshot range that
  holds it, or `stack_window ..`) or ` addr=0xADDR NOT IN SNAPSHOT`. A later frame in which a field name differs for the first time
  gets a full block even after the 20 detailed frames are used up.
- `NOTE dsp at arm: dsp csr=0x.. ...` and the same fields at the end of every HB line: `csr` = DSP control 0x0C00500A, `to_dsp_hi` =
  mailbox to the DSP (high half, bit 15 = a mail not yet taken), `ai_dma` = audio DMA enabled (0x0C005036 bit 15), `src` =
  0x0C005030/32, `blocks` = 32-byte blocks per transfer, `left` = blocks remaining (0x0C00503A). If `left` stops changing between
  two HB lines the audio DMA stopped. The mailbox "from DSP" registers are deliberately not read (reading the low half pops a mail,
  on LLE the high half steps the DSP).

### Time base (`REV3_SYNCTEST_TB=1`)

The game's `OSGetTime` is `mftb`, which Dolphin computes as `SystemTimers::GetFakeTimeBase()` = CoreTiming `fake_TB_start_value +
(ticks - fake_TB_start_ticks) / 12` (the JIT inlines exactly this; PowerPCState TL/TU are only a cache). Every snapshot stores that
value; a restore shifts `fake_TB_start_value` by (snapshot - now) with `CoreTimingManager::SetFakeTBStartValue` and also writes
`PowerPCManager::WriteFullTimeBaseValue`, so the replayed frames read the same time base as the first run. Logged: `NOTE timebase
restore on` and `tb_delta=` in the RESTORE lines (ticks moved back). The decrementer and the CoreTiming event queue are not changed.
Not needed for fighter `combo_timer` (+0x9B0): the decomp shows it is a per-tick counter with no OS time input. Risk: OSAlarm / OSThread-sleep deadlines are absolute time base values and CoreTiming events are absolute tick counts, so after the
time base moves back an alarm that was due fires later by the rolled-back amount (K frames); acceptable for a test, not for netplay.

A clean run: a fight of a few thousand frames ends with `mismatching_frames=0`, `sp_mismatches=0`, no `MISMATCH` or `ERROR`
lines, and a restore time of a few hundred microseconds. The state machine is separate from Dolphin (`Rev3::SyncTest` talks to a
`GuestMemory` / `Sink` interface); `Source/UnitTests/Core/Rev3RollbackTest.cpp` drives it with a fake game (target `tests`,
filter `Rev3*`).

## Effect pool (`REV3_EF_POOL`, `REV3_SNAPSHOT_INCLUDE`, `REV3_EFCHECK`)

Why: the first netplay run (K = 5) rolled back once (3959 -> 3954) and never sent another hook command. The freeze dump showed the main
thread RUNNING (state 2, not blocked) at pc `0x801B72D8`, the same sp 2 s and 12 s later, inside `nw4r::ef::ParticleManager::Calc`
(`0x801B5E50`, 5444 bytes; called from `Emitter::CalcParticle` `0x801ABD5C` <- `Effect::Calc` `0x801A93BC`): an endless walk of a
particle list. The earlier SyncTest freezes after 40-60 rollbacks (hit sparks) are very probably the same thing.

What the decomp says (addresses of the RNEEDA build, all in the comments of `Rev3Rollback.h`):

- `Calc` walks the particle manager's list with `node = *(node + offset + 4)` (the `ut::List` at pm + 0x38, offset = the u16 at pm + 0x42 =
  0x14) until it reads 0 (loop head `LAB_801b72dc`, next to the frozen pc). A list that is cyclic or runs into stale memory never ends.
- The objects live in object pools carved out of ONE block, allocated once at start-up by `zz_80186fd8_` (`0x80186fd8`): `block =
  zz_80170620_(size + 0xC60)` (global `0x8049CFB0`), the `MemoryManager` (0x3C bytes, nw4r `memorymanagertmp.h`) is constructed at the START
  of the block (`zz_801895d8_` `0x801895d8`), its free-list heap is `[block + 0x40, block + 0x40 + size)` (`mm + 0x24` / `mm + 0x28`), and
  Effect[n] (0xA0 bytes), Emitter[n] (0x154), ParticleManager[256] (0xBC) and Particle[2500] (0xE4) with their 0x24-byte `ObjectPool` objects
  (free list at +4, in-use list at +0x10, links at offset 0) are allocated out of that heap through the manager's vtable slot +0x60. So
  every ef object, every list head and every link is in `[mm, mm + 0x40 + size)`. The game's handle table (`0x8049CFC4`, 0x101 words) sits
  right behind the heap, inside the same allocation, and two small fixed pools (globals `0x8049CFBC` / `0x8049CFC0`) hold the handle nodes.
  The pool sizes are not constants of the emulator: the element counts of Effect / Emitter are arguments the decomp does not show, so the
  extent is read at run time.
- `EffectSystem::GetInstance` (`0x801A9C18`) returns the static `0x803D9588`: word 0 = the manager pointer (`zz_80189590_` stores it and calls
  `EffectSystem::Initialize` `0x801A9C24`), the `CreationQueue` is inside it (+0x10), `+0xC014` = the number of activity groups (the init code passes 3),
  `+0xC018` = `ActivityList[group]` (0x1C bytes: active effects at +0, closing at +0xC, both offset 0x14). Effect: emitters at +0x24 /
  +0x30; Emitter: particle managers at +0xC0 / +0xCC; ParticleManager: particles at +0x38 / +0x44.

Hypothesis behind the fix: the page tracker adds a page only after it saw the page change, up to one scan cycle (16 calls = 16 frames at the
default) later. A particle allocated into a never-touched page of the pool inside the rolled-back window is therefore NOT in the snapshot
while the list heads (in an old, dynamic page) are: the restore rewinds the heads to "this particle is free" but leaves the particle's
links as the first run set them, and the lists run into stale nodes (`pool_free` loops, `prev` links that point nowhere). The unit test
`WithoutThePoolARollbackLeavesBrokenEfListsAndEfCheckSaysWhere` reproduces exactly that on a synthetic image.

The fix, at every arm (SyncTest) and every arm of a netplay stretch (`NetPrepareArm`):

- `FindEfPool` reads the singleton and computes the page-aligned ranges that hold all ef objects: the allocation `[mm, mHeapEnd + 0xC20)`
  (clipped to the region), the singleton itself (`0x803D9588`, 0xC070 bytes) and, when they validate, the handle table and the two handle
  pools. Log: `NOTE ef pool 0xS-0xE bytes=N` (the allocation), `NOTE ef pool total bytes=T ranges=R mm=.. heap=.. effects=.. emitters=..
  particle_managers=.. particles=..`, `NOTE ef pool extras: instance=.. handle_table=.. handle_pool0=.. handle_pool1=..`,
  `NOTE forced include: ranges=.. bytes=..; the snapshot set grows by n bytes (a -> b, before the hardware cuts)` (only what the page tracker
  did not already have counts as growth); or `NOTE ef pool not found: <why>` (manager pointer 0 = effect system not set up yet, misaligned
  pointer, implausible heap) and nothing is forced; `NOTE ef pool off (REV3_EF_POOL=0)`.
- The cost is the size of the pool per snapshot slot and per restore: at least 0.62 MB (2500 particles x 0xE4 + 256 particle managers x 0xBC)
  plus the Effect / Emitter pools, so roughly 0.7-1.1 MB (the log line has the exact figure; the part the game touches is already in the
  set, `the snapshot set grows by` says how much is new). Copying 1 MB is about 0.1-0.2 ms.
- `REV3_SNAPSHOT_INCLUDE` forces more regions in the same way (above).

The check (`CheckEfLists`, `REV3_EFCHECK`): around the first N restores the walker follows the lists from the singleton (activity groups ->
effects -> emitters -> particle managers -> particles, plus the free / in-use list of each of the four pools) and verifies for every list:
head, tail and count agree about emptiness, every node is a 4-byte aligned pointer inside the ef heap, every `prev` link matches, the walk ends
at the tail after exactly `count` nodes, and it ends within 10000 nodes per list (40000 in all). Lines:

- `EFCHECK ok after restore n -> m lists=16 nodes=.. effects=.. emitters=.. pms=.. particles=.. us=..` (the first 3 good results)
- `EFCHECK bad after restore n -> m: <list> node=0x.. list=0x.. <why> page=0x.. in_snapshot=0|1 dynamic=0|1 lists=.. nodes=..` and the same
  with `before restore`: the state the game left just before the write-back (a bad `before` means the first run itself had broken the lists,
  a bad `after` with a good `before` is the restore's doing). `<list>` names the list (`particles@pm=0x..`, `pool_free[particle]`,
  `effects[g0]`, ...), `in_snapshot` says whether the node's page is inside the snapshot ranges, `dynamic` whether the tracker had seen it
  change. The first 20 failures are logged, the rest counted. The pool lists were derived from the pool init code only; if a
  `pool_free` / `pool_used` line is the only complaint on a game that runs fine, the pool code uses them differently than assumed.
- SUMMARY lines end with `ef_checks=.. ef_bad=.. ef_bad_before=.. layout_refused=.. late_learn_rollbacks=..`.
- The freeze dump has `DUMP <reason> ef ...` lines: `ef sys=0x803D9588 mm=.. heap=.. pool_sizes effect=.. emitter=.. pm=.. particle=.. groups=..
  activity=..`, `ef group<g> effects head= tail= n= off= | closing ...`, `ef pool <kind> obj=.. free ... | used ...` and `ef check ok ...` or `ef check
  BAD list=.. list_addr=.. node=.. <why>`. In a frozen `Calc` the BAD line names the particle manager whose list loops.

Layout changes while armed (`LearnWhileArmed`, hardware cuts, exclusions): every one goes through `RebuildLayout`, which invalidates every
snapshot slot, and `ClearHistory`, which restarts the history, so no rollback happens until K new frames are captured under the new layout
(test `LearningAPageWhileArmedBlocksRollbacksToOlderSnapshots`). As a guard, a slot also carries the layout generation it was captured
under and `Restore` refuses (and counts `layout_refused`) a slot of another generation; that count must stay 0. What a layout change does
NOT cover is the other direction: a page noticed late. Rollbacks that ran during the up to one scan cycle between the first change of a page
and its discovery did not rewind that page. `NOTE learned +n pages (...); new=[0xS-0xE,..]` now lists the new pages and adds `WARNING a
rollback ran within the last scan cycle without these pages` (counted in `late_learn_rollbacks`) when that happened. Any such line in a
netplay run names a region that should be added to `REV3_SNAPSHOT_INCLUDE` (or to the code, like the ef pool).

### Late-learned pages: naming them, and keeping them out of the next relayout

A page is learned when its hash differs between two scans one cycle (16 hook calls) apart, so a page the game writes only now and then
(an effect, a special move, a sound) is found in the middle of a fight, long after the 20 learning cycles. In netplay each such growth is
a relayout (`GATE relayout`): the snapshot history is dropped and no rollback is possible until it has refilled (a D match: 25 relayouts of
1-3 pages each). Three things help, in this order of cost:

1. **Name the pages.** Every `NOTE learned +n pages` line (SyncTest; the netplay line `... while slots are in use` on rev3-net, see below)
   ends in `new=[0xS-0xE,..]`: merged runs, at most 16 named then `,+N more`. A rate limited SyncTest line names every range learned since the
   previous line. `NOTE learned summary pages=N ranges=[0xS-0xE,..]` lists ALL pages learned after the first arm, merged (N = bytes / 4 KB,
   rounded up; at most 2000 ranges named). It is written with the periodic SUMMARY line when the list changed and always at shutdown
   (`pages=0 ranges=[]` when nothing was learned, so a log shows the fork has the line); `NOTE learned adjacency growths=.. ranges_touching_old_set=..
   learn_pad_pages=..` says how many of the new ranges lay directly next to a range that was already in the set.
2. **Include them next time.** `tools/rollback/learned_include.py` (rev3-mod) reads those lines from any number of logs and writes a
   `REV3_SNAPSHOT_INCLUDE` file: ranges padded to whole pages, gaps under 16 KB joined, optionally only ranges seen in at least N logs
   (`--min-logs`; the heap layout moves from match to match, so an address seen in one log only may not repeat), a `note` per range (first
   frame, logs). The fork then puts those pages in the set at every arm: no growth, no relayout. This works exactly as far as the addresses
   repeat between runs (fixed pools and static data do, a heap block may not); the tool's report says how many ranges all logs agree on.
3. **Neighbours.** `REV3_SNAPSHOT_PAD=1` is the cheap form of "pages next to a learned one": see the variable table. It is always safe
   (a page in the set that never changes is rewound to what it already holds) and costs only snapshot bytes and copy time, so it is
   off by default; try it where `ranges_touching_old_set` is a large share of `growths`. A real "same heap block" rule would need the
   game's heap block headers; that is not attempted.

Why the old history cannot simply be kept when the set grows: an old slot has no bytes for the new pages, and copying their CURRENT
contents into it would be wrong unless the page had not changed since that slot was taken, which cannot be known (that is the point: the
page was only just found to change). So a relayout stays a relayout; the fix is to make fewer of them.

Merging this into rev3-net (netplay path, which lives only there): (a) in `NetObserve`, add `; new=[%s]` to the `NOTE learned +%u pages while slots
are in use` line with `FormatRangeList(NewRanges(ComputeRanges(), m_ranges), MAX_LEARN_LIST_RANGES)`; (b) in `NetPrepareArm`, after
`ResolveInclusions()`: `std::vector<Range> next = ComputeRanges(); if (m_net_armed_once) { const auto fresh = NoteLearned(m_ranges, next); if (!fresh.empty())
Line("NOTE learned +%llu pages at arm (frame %u): new=[%s]") } m_ranges = std::move(next); m_net_armed_once = true;` (the first arm builds the
set, it learns nothing); (c) call `LearnedSummaryLine()` before the net SUMMARY line and in the net `Finish`; (d) `REV3_SNAPSHOT_PAD` is parsed in the
shared options code and needs nothing; (e) the runner must hand `REV3_SNAPSHOT_INCLUDE` / `REV3_SNAPSHOT_PAD` to the fork (rev3-mod netplay.py
`KEEP_REV3_VARS`).

Tests: `Rev3Rollback.EfPool*`, `InclusionFile*`, `EfCheck*`, `EfDump*`, `FreezeDumpCarriesTheEfLines`, `ArmForcesTheEfPool*`, `WithoutThePool*`,
`WithThePoolForced*`, `UserInclusionsAreForced*`, `LearningAPageWhileArmed*` (a synthetic ef image with the same offsets).

## Restore verify (`REV3_RB_VERIFY`, off by default)

Why: the adaptive snapshot set learns pages late, and a restore only rewinds the pages it knows. Two symptoms so far: 25 relayouts in a netplay
match, and a hit-spark particle in a not-yet-learned page that made the restored lists circular (the freeze). The idea (borrowed from another
project's "RESTORE MISS" mode; no code taken) is to ask the question directly: after a restore to frame f, is ALL of guest memory what it was
when f was captured?

How: a **shadow** is a full copy of guest MEM1 + MEM2 (24 + 64 MB = 88 MB) taken inside `SyncTest::Capture` at the same moment as the
snapshot of a frame, for the frames that are a multiple of `every` only, in a ring of `slots` shadows (slot = (frame / every) % slots). After the
write-back of `SyncTest::Restore` to frame f the verifier compares all guest memory with f's shadow, in 64 KB chunks first, then 4 KB pages, then
32-byte blocks, and reports the blocks that differ. A restore is verified only when f still has its shadow, so with 1 slot and every = K + 1 one
restore in K + 1 is checked. Both SyncTest and the netplay session use `Capture` / `Restore` (`NetCapture` / `NetRestore` call them), so the
hooks are shared. Nothing is allocated and nothing is copied while `REV3_RB_VERIFY` is unset.

Cost (unit: per shadow / per check, desktop core, measured only on the fake memory here, so treat as estimates): a shadow is a 88 MB memcpy
(roughly 8-15 ms) and holds 88 MB; a check reads both copies again (5-10 ms). After N verified restores the shadows stop being taken. `shadow_ms`
in the stats is the sum of both. This is a diagnosis mode, not for a match you want to play at full speed (a hitch of ~20 ms every K + 1 frames).

What is not compared (the restore leaves it alone on purpose): the hook region 0x80002400-0x80002A00, the `REV3_SYNCTEST_EXCLUDE` ranges (including
resolved pointer entries), every hardware cut (GX FIFO, XFB, audio DMA, DVD DMA; the set only grows), `REV3_RB_VERIFY_IGNORE`, and the **dead stack**:
`[stack end, sp)` of the thread the hook runs on (OSThread `*0x800000E4`, stackBase +0x304, stackEnd +0x308; if that is not readable or does not hold sp,
`sp - REV3_RB_VERIFY_STACK_BELOW` bytes). The hook's stack window `[sp, sp + REV3_STACK_WINDOW)` IS compared (the restore rewrote it).

| variable | meaning |
|---|---|
| `REV3_RB_VERIFY=N\|all` | verify the first N restores that land on a shadowed frame (count); `all` = never stop; unset / 0 = off |
| `REV3_RB_VERIFY_EVERY=P` | take a shadow when `frame % P == 0` (frames, 0..1000000). 0 / unset = automatic: SyncTest picks the smallest P >= K + 1 that can hit a restore target (`AutoVerifyEvery`; with EVERY>1 not every P can) |
| `REV3_RB_VERIFY_SLOTS=S` | shadows kept, 1..8, default 1 (88 MB each). One slot is enough when P >= K + 1 (SyncTest); P = 1 with K = 2 needs 3 slots to verify every restore |
| `REV3_RB_VERIFY_HEAL=1` | copy the shadow's bytes back over every wrong block (so the replay starts from the right state) and add the wrong pages that lie outside the set to the snapshot set (below) |
| `REV3_RB_VERIFY_STACK_BELOW=bytes` | fallback size of the dead stack below the hook's sp, 0x100..0x400000, default 0x4000 |
| `REV3_RB_VERIFY_IGNORE=path` | `{"ranges":[{"start","end"}]}` (the whitelist format): memory never compared (host-thread buffers that always differ, say) |

Log lines (all in the Rev3 log):

- `NOTE verify on: restores=.. every=.. slots=.. heal=.. stack_below=0x.. shadow_mb=88 per slot ...` at start; `NOTE verify: shadows at multiples of P never
  match a restore target ...` when P cannot hit (no restore will be verified).
- `VERIFY restore n -> m ok wrong_blocks=0 compared_mb=.. skipped_kb=.. us=..` (the first 3 clean restores).
- `VERIFY first wrong restore: at frame n, back to frame m, k blocks of 32 bytes` once (and one on-screen message).
- For the first 30 restores with wrong blocks: `VERIFY restore n -> m wrong_blocks=k pages=p in_set=a outside_set=b first=[0xADDR: snap w0 .. w7 | mem w0 .. w7]
  compared_mb=.. skipped_kb=.. us=..` (`snap` = the shadow's 8 words of the first wrong 32-byte block, `mem` = what guest memory holds now),
  `VERIFY restore n -> m first_in_set=[..]` (the first wrong block inside memory the restore rewrote, when it is not the first one),
  `VERIFY restore n -> m regions64k=R [0xBASE:blocks b/pages p,..]` (64 KB regions with wrong blocks, at most 16 named) and
  `VERIFY restore n -> m missing_pages=P [0xS-0xE,..]` (4 KB pages with wrong blocks that the snapshot set does not hold, merged; with heal `healed; added
  to the snapshot set at the next layout` follows).
- **How to read it**: `outside_set` blocks are pages the set is missing (what the tracker would learn late) or memory nobody can rewind (host-thread
  writes, hardware buffers: add those to `REV3_RB_VERIFY_IGNORE` to see past them); `in_set` blocks (the line then ends `WARNING blocks inside the rewritten
  memory differ`) mean the restore itself failed or something wrote behind it while it ran, which is a bug.
- SUMMARY lines end with ` restores_checked=.. restores_wrong=.. verify_blocks=.. first_wrong_frame=.. shadow_ms=..`; at shutdown
  `VERIFY summary restores_checked=.. restores_wrong=.. wrong_blocks=.. (in_set=.. outside_set=..) distinct_blocks=.. in distinct_pages=..
  first_wrong_frame=.. first_wrong_back_to=.. first_wrong_blocks=.. shadows=.. shadow_mb_held=.. shadow_ms=.. heal=.. pages_added=..`.

Heal: the wrong blocks are overwritten with the shadow's bytes (JIT invalidated for those pages, the page tracker's baseline for them reset), so the
replay that follows starts from the true state of frame m. The pages among them that lie outside the snapshot set go into `m_verify_extra`, which is
part of every `ComputeRanges()` from then on. SyncTest installs them on the first hook call after the resimulation (`ApplyVerifyPages`): relayout,
history cleared, and a line in the learned-page format so `tools/rollback/learned_include.py` can consume it:
`NOTE learned +n pages (now ranges=.. bytes=..) frame F, history cleared; new=[0xS-0xE,..] (restore verify heal)`; they also end up in
`NOTE learned summary`. With the cuts winning as everywhere. Without heal nothing changes in the set; the `missing_pages=[..]` list is the finding.

Tests: `Rev3Rollback.Verifier*`, `VerifyEvery*`, `RestoreVerify*` (exact block / page / region counts across MEM1 and MEM2, the Read fallback without Peek,
skip ranges starting mid-block, slot rotation and the budget, the missing page named, heal giving a mismatch-free replay with an incomplete whitelist, a
complete adaptive set verifying clean, a restore that drops writes reported as `in_set`, hardware cuts / ignore list / dead stack left out).

What D runs (SyncTest; the environment is inherited by `phase3.bat`): `set REV3_RB_VERIFY=40` and `phase3.bat synctest --k 4` (play 2-3 minutes in a fight).
Success: `restores_wrong=0` in the SUMMARY lines and `VERIFY summary ... restores_wrong=0`; otherwise the `VERIFY restore` lines name the pages. First try
without heal to see everything the set misses; then `set REV3_RB_VERIFY_HEAL=1` plus `REV3_RB_VERIFY_EVERY=1` / `REV3_RB_VERIFY_SLOTS=5` (K + 1 slots: every restore verified
and healed, 440 MB) to see whether the replay mismatches go away. Feed the `VERIFY ... missing_pages` / `NOTE learned` lines to `learned_include.py`.

Merging into rev3-net (netplay path): the `Capture` / `Restore` hooks and `ComputeRanges` are shared, so a merge brings them along. Remaining there:
(a) `ParseVerifyEnv(&options, &ignore, sink)` where the net options are read (it fills `options.verify`) and `SetVerifyIgnore(ignore)` on the SyncTest;
(b) netplay restores land on arbitrary frames: set `REV3_RB_VERIFY_EVERY=1 REV3_RB_VERIFY_SLOTS=2` to verify depth-1 rollbacks (the shadow of f - 1 survives the
capture of f; 176 MB, a copy per frame); the automatic P (K + 1 = MAXR + 2) rarely lands; (c) append `VerifyStatsFields()` to `NetStatsLine()` and write
`VerifySummaryText()` in the net Finish when `Verifier() != nullptr`; (d) heal's pages are in `ComputeRanges()` already: `NetPrepareArm` installs them at the next
arm (they show in the `NoteLearned(m_ranges, next)` line of the earlier merge note), and `NetObserve` may report `VerifyPagesPending()` like a pending learned
page; (e) a heal while the session is resimulating writes guest memory behind the session: only use `REV3_RB_VERIFY_HEAL=1` in netplay for a diagnosis run.

## Freeze dump (`REV3_SYNCTEST_DUMP`, default on)

Why: SyncTest replays gameplay exactly (`fields=0` on every resimulated frame) but the game freezes after about 40-60 rollbacks and the
log just stops. The dump shows WHERE the guest is stuck: PPC registers and back-chain, every OS thread (state, pc, lr, stack, canary), the
sleep queues the SDK blocks on (GX draw done, VI retrace, DVD, nw4r streaming, AX, IPC, SC) and the hardware those wait for (PE, CP FIFO,
PI, VI, DSP). The suspect is a thread waiting on a hardware event whose software flag was rewound. Read-only: it never writes guest
memory or registers.

When (all into the Rev3 log, `<User>\Logs\rev3_synctest.log`):

1. **arm**: once, right after the arm notes (a baseline of a healthy game). `DUMP arm begin racy=0 ...`.
2. **stall**: a host-side watchdog thread (wakes every 250 ms) fires in ANY mode (plain play, SyncTest armed or not, netplay in any
   mode; the dump does not depend on SyncTest) when no 0xA0 hook command arrived for
   `REV3_SYNCTEST_DUMP_STALL_MS` (default 2000 ms), once per stall (the next command re-arms it). It first writes a racy dump at once
   (`racy=1`: the CPU thread keeps running, so values can be torn; it never blocks), then, unless `REV3_SYNCTEST_DUMP_PAUSE=0`, pauses the CPU
   with `Core::CPUThreadGuard` and writes a consistent dump (`racy=0`, includes the DSP / audio lines). If the freeze is a host-side wait that
   never lets the CPU thread pause, the second dump never appears (the first one does); closing Dolphin still works, the watchdog is
   abandoned after 1.5 s. A paused emulator or a long load also counts as a stall (harmless). A netplay session that is deliberately
   waiting for the peer (its wait loops) marks itself blocked and is not reported. In a netplay run the DUMP lines go to the net log
   (`REV3_NET_LOG`), otherwise to the Rev3 log.
3. **shutdown**: always, when the Slippi EXI device is destroyed (guest memory and the PPC state are still there; the DSP is already shut
   down, so the DSP / audio lines say `dsp skipped`). `racy=1` only if the CPU is still running.

Also at every RESTORE among the first 10: `RESTOREHW n -> m pe_status=.. pe_token=.. cp_rw_dist=.. cp_rd=.. cp_wr=.. vi_count=..
drawdone=.. sched_disable=..` right before the write-back (after `RESTORE n -> m begin`) and `RESTOREHW n -> m after ...` right after the audio
hardware is put back (before the `RESTORE n -> m bytes=..` line). Compare the two lines to see which software flags the restore moves
relative to the hardware: `vi_count` = `*0x8049D4DC` (VI retrace count), `drawdone` = byte `*0x8049D958` (GX draw done flag),
`sched_disable` = `*0x8049DAB8`; `pe_status` / `pe_token` = MMIO 0x0C00100A / 0x0C00100E; `cp_*` from the command processor FIFO.

Dump format. Every line is `DUMP <reason> <line>` with `<reason>` = `arm`, `stall` or `shutdown`; numbers are hex, `?` = unreadable:

| line | content |
|---|---|
| `begin racy=<0\|1> frame=<last hook frame> last_cmd_age_ms=<ms since the last command> [note]` | start; `end` closes it |
| `ppc pc= npc= lr= ctr= msr= srr0= srr1= cr= r1= r2= r13=` and `ppc r3=..r12= exceptions=` | registers and the pending exception bits (`PowerPCState::Exceptions`) |
| `frame <i> sp=<sp> lr=<*(sp+4)>` | up to 16 frames of the back-chain from r1 (`*(sp)` is the next sp, `*(sp+4)` the saved LR word; stops at an unreadable, zero or unaligned sp or when the next sp does not increase). Frame 0's lr is the slot of the current function and stale unless it is in a call; from frame 1 on `lr` is a return address into the function that owns the frame |
| `os cur_thread= cur_context= fpu_owner= sched_disable= run_queue_bits= resched= active_head=` | `*0x800000E4`, `*0x800000D4`, `*0x800000D8`, `*0x8049DAB8` (scheduler-disable count: nonzero while blocked is the freeze signature), `*0x8049DAC0`, `*0x8049DABC`, `*0x800000DC` |
| `threads n=<count> end=null\|cycle(..)\|bad_ptr(..)\|max\|unreadable_link` | the active thread list from `*0x800000DC` via `+0x2FC` (at most 32) |
| `thread 0x.. state= prio= suspend= queue= pc= lr= sp= canary=ok\|bad\|? stack=<end>..<base> [running=1 live=1]` | OSThread `+0x2C8` (u16: 1 ready, 2 running, 4 waiting, 8 moribund), `+0x2D0`, `+0x2CC`, `+0x2DC`; saved context pc `+0x198`, lr `+0x84`, gpr1 `+0x04`; canary = `*(+0x308) == 0xDEADBABE` at the stack end; the running thread (`*0x800000E4`) uses the live registers |
| `thread 0x.. frame <i> sp= lr=` | 6 frames of the back-chain from the thread's sp |
| `queue <name> at= head= tail= waiters=<n>[<threads via +0x2E0>] head_state= head_pc= head_lr= [extra]` | sleep queues `gx_drawdone 0x8049D950` (+ `drawdone_flag` = byte `0x8049D958`), `vi_retrace 0x8049D4B8` (+ `retrace_count` `*0x8049D4DC`, `game_last_count` `*0x8049CF48`), `dvd 0x8049D7B0`, `nw4r_strm 0x8049D238`, `ax 0x8049D6A0`, `ipc` (`*0x8049D984 + 0x2C`; `queue ipc skipped: pointer ..` when the pointer is not in MEM1 / MEM2), `sc 0x804655A0` |
| `game frame_loop_flags= main_state= no_gx=` | `*0x803C19C0`, `*0x803C11D4`, `*0x803C1A70` as raw u32 (a byte flag shows in the top byte) |
| `hw pe pe_status= pe_token=` | MMIO 0x0C00100A (PE control / interrupt status) and 0x0C00100E (token) |
| `hw cp status=idle\|busy\|breakpoint base= end= rd= wr= rw_dist= bp= gp_read= bp_enable= link= hi_wm= lo_wm=` | `GetCommandProcessor().GetFifo()`; `idle` = rw_dist 0, `breakpoint` = the breakpoint flag |
| `hw pi cause= mask= fifo_base= fifo_end= fifo_wptr=` | processor interface interrupt cause / mask and the CPU write-gather FIFO pointers |
| `words dvd base=.. +0x00: w w w w w w w w` (3 rows), `words nw4r_strm ...` | 24 raw words from 0x20 bytes before the DVD queue (`0x8049D7B0`) and before the nw4r stream queue (`0x8049D238`), i.e. the neighbouring SDK state, for decoding by hand |
| `hw di disr= cmd0= dimar= dilength= dicr= wii_dicr= ios_di_busy=0\|1\|none\|skipped(racy)` | DVD interface registers (0x0C006000 status, 0x0C006008 command word 0, DIMAR, DILENGTH, DICR with TSTART in bit 0; 0x0D00601C on a Wii) and `DIDevice::IsBusy()` of IOS `/dev/di` (not read in a racy dump: it walks the IOS device map) |
| `hw vi line= di0=` | vertical beam position (0x0C00202C) and display interrupt 0 (0x0C002030; bit 31 = flag, bit 28 = enable) |
| `dsp csr= to_dsp_hi= ai_dma= src= blocks= left=` and `audio dsp=HLE\|LLE irq= events=<n> [type+cycles,..]` | the existing DSP / audio DMA line and the pending audio CoreTiming events; consistent dumps only (not racy, not at shutdown) |

How to use it: start Dolphin with `REV3_SYNCTEST=4` (K=4) as before, play until it freezes, wait 3 seconds (the stall dump needs 2 s plus
the pause), then close Dolphin. The log then ends with the `DUMP stall` blocks and a `DUMP shutdown` block. Read the thread with `state=4`
and its `pc` / `lr` / `queue`, and the queue lines: a thread waiting in `gx_drawdone` while the CP is `idle` and `drawdone_flag` is 0, or in
`vi_retrace` while `retrace_count` equals `game_last_count`, shows which event was lost. Variables: `REV3_SYNCTEST_DUMP=0` off,
`REV3_SYNCTEST_DUMP_STALL_MS=n` (100..600000), `REV3_SYNCTEST_DUMP_PAUSE=0` (no CPU pause, racy dump only).

Tests: `Rev3Rollback.FreezeDump*`, `BackChain*`, `RestoreHwLine*`, `Stall*`, `SyncTestDumpsAtArm*` (fake memory with a fake OS).

## Determinism kit (M1)

Milestone M1 of the netplay plan ("boot determinism"): play a session once, record the pads, replay the pads in a second run from the same
user folder, and compare a trace of the two. The runner is rev3-mod `tools/rollback/phase4.py` (`determinism record | replay | compare`); the
fork only writes and reads the files below. It lives in the 0xA0 command handler (`Rev3::DeterminismKit`, `Rev3Rollback.cpp` section 4b) and
works with SyncTest off (plain play) and alongside it (`REV3_SYNCTEST=K`: the kit sees every hook call first, SyncTest then works with the
scripted pads, and the reply keeps SyncTest's own flags and frame). Read once when the first command arrives; each line below is
logged once in the Rev3 log (`<User>\Logs\rev3_synctest.log` or `REV3_SYNCTEST_LOG`).

| variable | meaning |
|---|---|
| `REV3_PAD_RECORD=file` | pad record. First line `# rev3 pad record v1` (only when the file is new or empty; the file is appended to), then one line per hook call that is not a resimulation: `<frame> <port0 pad, 24 hex> <port1 pad, 24 hex> <padread return, signed int>`. `frame` = the command's frame (the hook's counter); a pad is the command's 12-byte PADStatus, lowercase hex. Flushed every 60 lines and at shutdown. Log: `NOTE pad record -> <path>` |
| `REV3_PAD_SCRIPT=file` | same format (a record is a script). Parsed at start: `NOTE pad script <path> frames=<n> first=<f> last=<l>` (`n` = distinct frames; a frame listed twice keeps its later line; unusable lines are counted in `NOTE pad script: n malformed lines ignored`). For every listed frame the hook gets the listed two pads (and the listed padread return) instead of the live ones, flags 0, or SyncTest's own flags when it is running. Frames not listed (before the first, holes, after the last) use the live pads. Comment lines (`#`), blank lines, CRLF and a missing final newline are fine. The first hook call (not a resimulation) with a frame after the last listed one writes `NOTE pad script ended at frame <last>` to the Rev3 log and to the trace (if any), once. A script that cannot be read or has no pad line is an `ERROR` in the log (and on screen) and the script stays off |
| `REV3_TRACE=file` | trace, appended, flushed after every line. Every `REV3_TRACE_EVERY`-th hook call (not a resimulation) with `frame % every == 0`: `T frame=<N> fighters=<0\|2> chk=0x<8 hex> mem=0x<8 hex>` (hex digits uppercase). `fighters` is 2 when both fighters are found through the TEAMS chain, `chk` is then the fighter checksum below; otherwise (none, or only one of them) `fighters=0 chk=0x00000000`. `mem` = CRC32 (zlib polynomial, init 0) over the bytes of all trace ranges in file order, 0 without a ranges file. The trace line is taken at hook entry, before SyncTest captures or restores anything (the state at the start of `input_handler` N). `NOTE ...` lines may be interleaved. Log: `NOTE trace -> <path> every=<n> ranges=<count> bytes=<total>` |
| `REV3_TRACE_EVERY=n` | 1..1000000, default 30 (anything else is reported and 30 is used) |
| `REV3_TRACE_RANGES=file` | `{"ranges":[{"start":"0x80...","end":"0x80..."}]}`, hex strings or numbers, end exclusive, inside MEM1 / MEM2 (the whitelist parser; an unusable file is an `ERROR` and `mem` stays 0). A range that cannot be read is skipped (`ERROR ... trace range(s) could not be read`, once) |

A resimulated hook call (SyncTest replaying a rolled-back frame) is never recorded or traced, so the record has each frame once however
often SyncTest replayed it. The scripted pads are substituted in resimulations too (SyncTest ignores them there and replays its recorded
inputs).

**Fighter checksum** (`FighterChecksum`): exactly rev3-mod `rev3mod/rbchecksum.py` `checksum(frame, fighters_bytes)`: CRC-32 (zlib, init 0)
over u32 BE `frame`, then for player 1 and 2 (a player not found is skipped, not zero-filled) the u32 BE player id (1 = team 0 -> `*0x8038FE50`,
2 = team 1) and the 61 `FIELDS` of the fighter struct as their raw 4 big-endian memory bytes each, in `FIELDS` order (floats are raw bits;
fighter addresses are not hashed). The fork's `FIELDS` table is rbchecksum's (FIELDS_VERSION 1, `fields_digest()` 0xD4076EF2); the unit tests
check the offsets, the digest, the golden value (the tests' invented fighter memory, frame 300 -> `0xCDD50D8D`) and the one-fighter /
no-fighter streams against numbers computed with the Python module. Change the table in both places and bump FIELDS_VERSION.

Tests: `Rev3Rollback.*` in `Source/UnitTests/Core/Rev3RollbackTest.cpp` (checksum, memory CRC, record / script / trace formats and parsing,
the kit driven alongside SyncTest, and a record -> script round trip giving the same trace).
