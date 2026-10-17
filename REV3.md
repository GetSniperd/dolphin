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
| `REV3_ALARMCHECK=0\|1` | the OS alarm queue watch (default 1, see "Alarm queue"): the queue is walked at every capture and around every restore |
| `REV3_RB_IOS_GATE=1` | a pending IOS IPC event (CoreTiming `IPCEvent` / `IPCInterrupt` / `FinishDICommand`) counts like a disc read in flight (default 0 = only counted, see "Alarm queue") |
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

(Netplay: the same idea, extended, in "Busy hardware" under Netplay (M4): the game's DVD library view and slots captured during a read.)

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

In netplay (`REV3_NET_ROLLBACK=1`) the set changes only in `NetPrepareArm` / `NetClear`, so the pool is forced at every `NetPrepareArm` (same
`NOTE ef pool ...` / `NOTE forced include ...` lines in the net log; `REV3_EF_POOL`, `REV3_SNAPSHOT_INCLUDE`, `REV3_EFCHECK` are read by the
same code as for SyncTest, and a bad include file turns the snapshot machinery off, i.e. lockstep). Pages learned while a stretch is armed
are installed at the next arm (`NOTE learned +n pages while slots are in use`); a restore that runs while such a set is pending is unsound for
those pages, so it is counted. The `NOTE snapshot stats` line (end of a stretch) ends with `ef_checks=.. ef_bad=.. ef_bad_before=..
layout_refused=.. restores_with_pending_set=..`; `ef_bad` and `layout_refused` must be 0.

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

**In netplay (merged into rev3-net).** The set changes only in `NetPrepareArm` / `NetClear` (`SyncTest::NetInstallSet`). The first install of a match builds the
set and learns nothing (`m_net_armed_once`, cleared when the fighters are lost: a new match builds its first set again). Every later install names what
joined, in the format `learned_include.py` reads: `NOTE learned +n pages while slots are in use (frame f): the snapshot set grows at the next arm; new=[..]` when
`NetObserve` first sees the growth, and `NOTE learned +n pages at arm (frame f): new=[..]` (`at clear` when the session drained first) when the set is installed,
which names every page that joined since the previous install (also pages found after the first line). `LearnedSummaryLine()` is written before every SUMMARY line
(the store's `SummaryNote()`, called by `RollbackSession::SummaryLine`) and at shutdown. `REV3_SNAPSHOT_PAD` and `REV3_RB_VERIFY*` are read by the shared `BuildSyncTest`, so
they apply on the net path with no extra code (`NOTE learn pad ..` / `NOTE verify on ..` go to the net log). The runner must still hand `REV3_SNAPSHOT_INCLUDE` /
`REV3_SNAPSHOT_PAD` to the fork (rev3-mod netplay.py `KEEP_REV3_VARS`).

Tests: `Rev3Rollback.EfPool*`, `InclusionFile*`, `EfCheck*`, `EfDump*`, `FreezeDumpCarriesTheEfLines`, `ArmForcesTheEfPool*`, `WithoutThePool*`,
`WithThePoolForced*`, `UserInclusionsAreForced*`, `LearningAPageWhileArmed*`, `NetPrepareArm*Ef*`, `NetModeRestoreWithAPendingSet*` (a synthetic ef image with the same offsets),
`NetModeNamesTheLearnedPages*`, `NetPrepareArmNamesPagesLearned*`, `NetModeLearnedSummary*`, `NetModeAppliesTheLearnPadOption`, `NetModeANewMatch*`.

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

**In netplay (merged into rev3-net).** `Capture` / `Restore` are shared, so the shadows are taken at every `NetCapture` and checked after every `NetRestore` (`VERIFY` lines
in the net log). `ParseVerifyEnv` runs in `BuildSyncTest` for the net path too. Netplay restores land on arbitrary frames: the automatic P (K + 1 = MAXR + 2) verifies about
one restore in P; `REV3_RB_VERIFY_EVERY=1 REV3_RB_VERIFY_SLOTS=2` verifies depth-1 rollbacks (176 MB, an 88 MB copy per frame: a diagnosis run, not a match to play). The net
`NOTE snapshot stats` line and every SUMMARY line end with ` restores_checked=.. restores_wrong=.. verify_blocks=.. first_wrong_frame=.. shadow_ms=.. verify_pages_added=..`
and shutdown writes the `VERIFY summary` line. Heal in netplay: the pages go to `m_verify_extra`, the session is told the layout is pending (`NetLayoutPending()`, line
`NOTE restore verify heal found pages missing from the snapshot set`), so it drains and relayouts like for a learned page; `NetInstallSet` then names them
(`NOTE learned +n pages at arm|clear (frame f): new=[..] (k of them found by the restore verify heal)`, counted in `verify_pages_added`). The heal also writes guest memory behind
the session on that machine only: use `REV3_RB_VERIFY_HEAL=1` in netplay for a diagnosis run.
Tests: `NetModeRestoreVerifyHeal*`.

## Host write tracking (`REV3_RB_WRITE_TRACK`, off by default)

Why: the adaptive set finds the pages that change by hashing them (16 slices per hook call, so a page is noticed up to one scan cycle after its first
change), and a late page costs a relayout that **drops the snapshot history** (25 of them in one netplay match; the freeze came from a particle in a page
that was not learned yet). Write tracking learns a page at the moment it is first written, with its old contents in hand, so the set can grow and the
history stays. Idea borrowed from another project's host write tracking (verify mode, "RESTORE MISS" style logs); no code taken.

How it works (`Rev3WriteTrack.{h,cpp}`, `WriteWatch` interface and the SyncTest side in `Rev3Rollback.{h,cpp}`):

- **Where the guest RAM is on the host** (`MemoryManager::GetRamViews()`): the linear views `m_ram` / `m_exram` (interpreter, DMA, GPU thread, `CopyToEmu`), the
  fastmem physical view (`m_physical_base`, RAM at 0, EXRAM at 0x10000000) and the fastmem logical view, one mapping per BAT-mapped piece
  (`m_logical_mapped_entries`, now with their `physical_address`, ~128 KB each, up to ~1400, the same RAM can be mapped twice by cached + uncached BATs).
  Protecting a page in one view does not protect it in the others, so the tracker protects **every** view of every watched page (a CSR table
  page -> host addresses, built once per view list, immutable, published with an atomic pointer; old tables are kept until exit because a handler may
  still read one). Measured on the Linux sandbox with 900 views: arming 20736 pages is 1 ms and 800 protection calls; releasing one page again 0.2 ms.
- **What is watched**: every page that is not *fully* inside the keep set = snapshot set U cuts (hook region, user exclusions, hardware cuts). With
  tracking on the snapshot set is **page aligned** (`PageAlignRanges` in `ComputeRanges`: fighter tails, the stack window and forced ranges grow to whole
  pages, a few pages = ~30 KB in all), so a page is either wholly in the set or wholly out and no watched page needs a partial comparison. Pages that a
  cut only partly covers (a hardware region that does not start on a page boundary) stay watched.
- **First write** (`WriteTracker::OnFault`, inside the signal / exception handler, any thread): the page state goes Armed -> Claimed (compare-and-swap, so one
  thread owns the page), the 4 KB page is copied into a lazily committed preimage slot (88 MB of address space), the guest pc / lr are recorded when the
  faulting thread is the CPU thread (`Probe`; the JIT keeps pc at the start of the current block, lr is exact), the page is made writable in all views, state
  Dirty. The handler returns "handled" and the store runs again. A second thread that faults on a page being claimed waits for the owner (yield); a fault inside a
  registered view on a page that is already writable (stale) is made writable in that view and resumed; a fault outside the registered views is not ours
  (returns false, the caller chains on). The handler only uses atomics, `memcpy` and `mprotect` / `VirtualProtect`.
- **Hook order**: `EMM::SetWriteFaultHook` (`Core/MemTools.cpp`) is called BEFORE `JitInterface::HandleFault` in the POSIX handler (`SEGV_ACCERR`) and the Windows
  vectored handler (access type 1 = write); a tracked write never reaches the JIT's fastmem backpatching (which would treat any fault in the arena as a
  load / store to rewrite to the slow path). macOS Mach exceptions do not call it: `SetWriteFaultHook` returns false there and tracking is "not possible".
- **Each hook call** (`SyncTest::PollWrites`, before the call's capture, also during resimulations): the dirty pages are compared with their preimage. Different:
  the page joins the set (`PageTracker::MarkDynamic`). Equal (a store of the same bytes): `Rearm` protects it again (protect first, compare after, so a
  racing write is either seen by the comparison or faults: no third case; a write that raced the verdict makes the page dirty again and it is learned);
  after `REV3_RB_WRITE_TRACK_REARM` (4) equal rewrites it is learned anyway (a page rewritten every frame costs a fault every frame; 4 KB of copy is cheaper).
- **Growing with the history kept** (`SyncTest::GrowKeepingHistory`): a watched page that was not written since it was armed has the same contents in every
  snapshot slot taken since; the preimage IS that content. The new bytes of the set are filled into every valid slot from the preimages of the learned pages
  (or from live memory for pages that are still clean, e.g. the learn-pad neighbours), the old bytes are copied over, the slots get the new layout
  generation and the restore verify's shadows stay valid. The next restore can go back across the growth. If a needed page has neither (not watched, no
  view), the old behaviour applies (relayout, history cleared; logged `history cleared`); in the middle of a resimulation that fallback waits until the
  resimulation is over (`wt_deferred`).
- **When**: tracking starts when both fighters are found (loads are never tracked: the kernel-side writes of a load cannot be seen) and stops with
  `NOTE write tracking off (not gameplay)` when they are lost. The hash scan is skipped while tracking (`PageTracker::SkipSlice` keeps the LEARN clock, so
  arming happens at the same time as before). At arm the watch is refreshed with the final set and cuts; `RefreshExclusions` does the same when a cut appears.
- **Views change** (BAT update, fastmem arena set up / torn down): `MemoryManager` calls `ViewsChanging` right BEFORE it unmaps anything; the glue makes all
  guest RAM writable (`Stop`). `PollWrites` sees the watch inactive, logs `NOTE Dolphin remapped its memory views, write tracking restarted`, clears the
  history (writes in the gap went unseen) and starts again with the new views.
- **Fallback**: no hook (Mach), no handler installed, host page size other than 4 KB, a self test that did not fault (`SelfTest` writes the same byte back to a
  watched page after arming and expects exactly that page dirty and every other view writable): `NOTE write tracking not possible (why): learning pages by hash
  scan from here`, once, and the hash scan does the learning as before. Nothing stays protected.
- **Guest-visible behaviour**: none. A protected page only delays the store by one signal; the store is re-executed. Nothing is emulated differently, no
  emulated time passes. (A page written by the kernel on Dolphin's behalf, `read()` / `recv()` straight into guest memory, would fail with EFAULT instead of
  faulting; Dolphin's disc, IOS file and socket paths copy through `CopyToEmu`, and tracking is only on during a fight. `WRITE TRACK MISS` in mode 2 and the
  restore verify would show it.)

| variable | meaning |
|---|---|
| `REV3_RB_WRITE_TRACK=0\|1\|2` | 0 / unset = off (hash scan). 1 = faults learn the pages, no scanning. 2 = faults AND the hash scan; every page the scan finds that the faults did not announce is a `WRITE TRACK MISS` (a verify mode for tracking itself; the set still follows the scan, with the old history clear). Needs the adaptive set |
| `REV3_RB_DIRTY_LOG=N` | log the first N dirty pages of the run: `DIRTY f<frame> 0xPAGE pc=0x.. lr=0x.. cpu\|other changed\|equal\|promoted\|pending` (pc / lr 0 when another thread wrote it: GPU, DSP, IOS) |
| `REV3_RB_WRITE_TRACK_REARM=N` | equal rewrites before a page is learned anyway, 1..200, default 4 |

Log lines:

- `NOTE write tracking requested: REV3_RB_WRITE_TRACK=.. dirty_log=.. rearm_max=..` at start (the environment as parsed).
- `NOTE write tracking armed (host page protection, V views of P pages): W pages watched, E edge pages (learned on their first write), rearm_max=4, X ms`
  (V and P tell whether Dolphin's views were found: expect the linear views + physical view + the BAT pieces, and 6144 + 16384 pages with MEM2).
- `NOTE write tracking not possible (..): learning pages by hash scan from here`, `NOTE write tracking off (not gameplay)`, `NOTE Dolphin remapped its memory
  views, write tracking restarted`, `NOTE write tracking lost (..)`.
- `NOTE learned +n pages (now ranges=.. bytes=..) frame F, history kept; new=[0xS-0xE,..]` (same format as before, `history kept` instead of `history cleared`).
- `HB ... wt_faults=.. wt_pages=.. wt_poll_us_max=..` in the heartbeat; after every SUMMARY line and at the end:
  `SNAPSHOT kind=write_track last=.. MB max=.. MB pages=.. of watched=.. faults=.. arms=.. learned=.. equal_rearms=.. promoted_equal=.. grown_kept=..
  grown_cleared=.. deferred=.. restarts=.. misses=.. stale=.. protect_calls=.. protect_ms=.. poll_ms=.. | restores checked=.. wrong=.. blocks=..
  first_wrong_frame=.. shadow_ms=..` (`kind=adaptive` with the hash scan; the `|` part only with `REV3_RB_VERIFY`). `stale` counts faults on pages already writable
  (a racing second thread; harmless). The fork has no counter for "edge bytes always copied": a page-aligned set has none.

Tests (Linux, real faults): `Rev3WriteTrack.*` (`Rev3WriteTrackTest.cpp`: two views of one memory object, first write faults and records the preimage, a write through
the second view faults too, keep ranges decide what is watched, rearm of equal / changed pages, release, stop, view-list change, self test, 8 threads on one page,
6 threads over 256 pages, 150 rounds of a writer racing re-arms with no lost write, a real change caught after thousands of equal re-arms, faults outside the views
chained to the previous `sigaction` handler, an unowned fault still killing the process, and **Dolphin's real `EMM` handler** asking the hook first and passing the
rest on), `Rev3Rollback.WriteTracking*` / `*Tracking*` (SyncTest with a fake `WriteWatch`: late page learned with the history kept and no mismatch, the hash scan
control run, `restores_wrong = 0` under the restore verify while pages keep appearing, pages before arming, equal rewrites rearmed then promoted, fallbacks, stop and
restart, remapped views, missing baseline deferred past a resimulation, live-memory neighbours, mode 2 misses, the dirty log) and `Rev3Rollback.RealWriteTracking*`
(SyncTest + the real tracker on a 24 MB mmap'd MEM1 with a real handler: every restore verified right while pages appear, a write through an unregistered view reported
as a miss, no handler -> fallback). The Windows branch (`VirtualProtect`, vectored handler, `CreateFileMapping` views in the tests) is written by inspection and compiled
by the `rev3-windows` workflow, not run.

What D runs (needs a fight; the environment is inherited by `phase3.bat`): `set REV3_RB_WRITE_TRACK=1`, `set REV3_RB_DIRTY_LOG=60`, `set REV3_RB_VERIFY=40`,
`phase3.bat synctest --k 4`. Success: `NOTE write tracking armed (...)` with a believable view count, `SNAPSHOT kind=write_track ...` with `restores checked=.. wrong=0`,
`grown_cleared=0`, `restarts=0`, `late_learn_rollbacks=0` in the SUMMARY line and no `mismatching_frames`; `NOTE learned +n pages ... history kept` lines instead of
`history cleared`. Then `set REV3_RB_WRITE_TRACK=2`: `misses=0` and no `WRITE TRACK MISS` means the faults saw everything the scan did. If tracking says `not possible`, the
reason is in the line; send the log. The `DIRTY` lines name the code that writes each new page (`lr`): useful for the pages that still surprise us.

What remains (not done, and why):
- **Mode 2 proper (delta capture)**: watching the pages that ARE in the set too, so a capture copies only the pages written since the last one. That would cut the capture
  cost (20 MB per frame today) but needs re-protecting every set page after each capture (a fault per set page per frame) and a per-slot dirty bitmap; the fault
  rate on the fighter / effect pages is thousands per frame, so it only pays with a cheaper path than a signal. Left out on purpose.
- **Apple Mach exception path** (`EMM::SetWriteFaultHook` returns false there) and non-4 KB host pages (Apple silicon, some Linux ARM): "not possible", hash scan.
- **Kernel-side writes** into guest memory fail with EFAULT instead of faulting; not seen by tracking (mode 2 / restore verify show them).
- **Netplay** (`rev3-net`): merged, see "Host write tracking in netplay" below (untested on Windows and in a real match).

Merging into rev3-net: `Rev3WriteTrack.*`, `MemTools`, `Memmap` and the `WriteWatch` glue come over unchanged; the SyncTest-side pieces are `PollWrites`, `GrowKeepingHistory`,
`WatchStart` / `WatchStop` / `WatchRefresh`, `ParseWriteTrackEnv` and `SetWriteWatch`. In the net session: (a) call `ParseWriteTrackEnv(&options, sink)` where the net options are read and create
the `DolphinWriteWatch` (it lives in the anonymous namespace of the glue; move it to the header or construct it from `Initialize`) before the session's first command, and keep
`REV3_RB_WRITE_TRACK*` in the environment list that `KEEP_REV3_VARS` / the launcher passes on; (b) replace the scan in `NetObserve` (the `ScanSlice(frame)` step) with
`PollWrites()` when `WriteTrackingOn()`, and use its growth in place of the `GATE relayout` path: the ring of the session has the same slot layout (ranges back to back, then the stack
window), `GrowKeepingHistory` extends every valid slot and bumps the layout generation; the session must also remap whatever else it keeps per layout (its `m_range_offsets` copy); (c) call
`WatchStart` when the session arms and `WatchStop` when it disarms (`NetClear`); (d) append `SnapshotLine()` to `NetStatsLine` output; (e) the rollback depth is small in netplay, so
the growth almost always happens with history; a page first written during a resimulation is learned at the next `PollWrites` (deferred only when it cannot be extended); (f) netplay
restores write the set pages only, and the set is page aligned, so they cause no faults of their own.

### Host write tracking in netplay (merged into rev3-net; REV3_RB_WRITE_TRACK=1|2)

Same variables as SyncTest (`REV3_RB_WRITE_TRACK`, `_REARM`, `REV3_RB_DIRTY_LOG`; read by the shared `BuildSyncTest`, so they apply to the net path, the DolphinWriteWatch is
created there too). What the net path does with it (`SyncTest::NetObserve`, `NetPrepareArm`, `NetClear`, `NetLeave`; `PollWrites` / `GrowKeepingHistory` are the SyncTest ones):

- **Start**: `NetObserve` starts the watch when both fighters are found (not on a resimulated call), like SyncTest, so the first set of a match is learned by faults too
  (`PageTracker::SkipSlice` keeps the LEARN clock: arming happens at the same time as with the hash scan). **Stop**: `NOTE write tracking off (not gameplay)` when the fighters are
  lost (loading), and for good when the session ends or the emulator stops (`NetLeave`: `NOTE write tracking off (left the session)`; `NetModule::OnCommand` calls it on the
  call that ended the session, `Shutdown` again). After that the hash scan learns, as without tracking.
- **Every call polls first**: `PollWrites()` runs in `NetObserve` before the session captures anything, resimulated calls included; it replaces the hash scan (`ScanSlice` only
  advances the learning clock in mode 1; mode 2 keeps scanning and logs `WRITE TRACK MISS`). `NetPrepareArm` and `NetClear` end with `WatchRefresh()` (the installed set and the
  cuts decide what stays guarded).
- **A page joins while slots are in use**: `GrowKeepingHistory` extends every valid slot of the ring in place from the preimages (new layout generation, the slot ring, `m_range_offsets`
  and the restore verify's shadows stay consistent). `NetLayoutPending()` stays false, so the session never sees `gate.layout_pending` and there is **no `GATE relayout`**:
  `NOTE learned +n pages (now ranges=.. bytes=..) frame f, history kept; new=[..]` replaces the pair `NOTE learned +n pages while slots are in use` + `GATE relayout`
  (the lines `learned_include.py` reads still match). The slot of a frame captured before the growth is restorable afterwards (test `NetWriteTrackingGrowsTheSlotsInPlace*`).
- **Fallback = the old drain**: when a slot cannot be extended (a page that is neither in a preimage nor clean), nothing is dropped under the session: `m_net_pending` is set,
  `NOTE learned +n pages while slots are in use (frame f): the slots cannot be extended, the snapshot set grows at the next relayout; new=[..]`, the session drains and relayouts
  (`GATE relayout`, `NetPrepareArm` installs the set, `NOTE learned +n pages at arm`). In a resimulation the growth waits for the next call (`wt_deferred`). The Dolphin views being
  remapped also asks for that drain (`NOTE write tracking restarted while slots are in use`), because the history cannot be trusted after a gap.
- **Counting both**: SUMMARY gets ` wt_grown_kept=.. wt_grown_cleared=.. wt_deferred=.. wt_restarts=.. wt_misses=..` (empty with tracking off); `wt_grown_cleared` is the number of
  growths that fell back to the drain (the session counts the drains in `relayouts=`), `wt_grown_kept` the ones that did not need it. The `SNAPSHOT kind=write_track ...` line follows
  every SUMMARY line (`SnapshotStore::AfterSummaryLine`) and is written again after the final `NOTE snapshot stats` (which carries the same fields). Success in a real match:
  `relayouts=0` (or far fewer than without tracking), `wt_grown_kept=n`, `wt_grown_cleared=0`, `GATE relayout` absent, `late_learn_rollbacks=0`.
- **Loopback check done here**: a synthetic game over real ENet with two NetModules and a fake watch that writes a page nobody seeded from frame 300 (scratch harness, not in the
  repository): without tracking 1 `GATE relayout` per side and DESYNC lines (the hash scan finds the page late); with `REV3_RB_WRITE_TRACK=1` or 2 no relayout, `history kept`, no desync.
  The real Linux write-fault path is the `Rev3Rollback.RealWriteTracking*` / `Rev3WriteTrack.*` tests; Windows (`VirtualProtect`, vectored handler) is compiled by CI only.

`REV3_RB_HASHCHECK` in netplay is **local only**: `NetCapture` keeps the first run's hashes of a frame across the recapture, and the first call of a resimulation (the rollback
target, which must start in the memory the first run had) is compared with them: `HASHCHECK frame=.. ok|DIFF` lines and the `hashcheck_*` fields on the SUMMARY lines. Later
resimulated frames are not compared (the corrected pads make them differ legitimately), and only frames that were hashed (`frame % N == 0`) can be compared, so use a small N (1 costs
~10 ms per frame). No hashes cross the network; the cross-peer diff below is still design only.

## Desync region diff (`REV3_RB_HASHCHECK`, off by default)

**What it answers.** Restore verify says whether a restore put memory back. This says whether the *resimulation* ended in the same memory as the
first run, for ALL of guest memory and not only the snapshot set and the fighter fields (`Compare`). When a replay differs, the question
that matters is where; the answer is a list of 4 KB pages, grouped by 64 KB region.

**How it works.** `MemoryHasher` hashes every readable page of MEM1 + MEM2 (22528 pages, the same `HashPage` the page tracker uses; about 9 ms for
the 88 MB, estimated from the measured 5-10 GB/s of `HashPage` on the sandbox VM; D's `hashcheck_ms=` in SUMMARY has the real cost) except the pages that overlap a skip range. With `REV3_RB_HASHCHECK=N` SyncTest
hashes at capture when `frame % N == 0`, in the first run and again in the resimulation, and `HashCheck` compares the two page by page.
Skipped, because the restore or the hardware deliberately leaves them alone or they differ between two visits of a frame: the hook region,
`REV3_SYNCTEST_EXCLUDE`, the hardware cuts, `REV3_RB_VERIFY_IGNORE` and the dead stack below the hook's sp (the same `VerifyDeadStack` as
restore verify). A page overlapping a skip range is skipped whole. Cost is one hash pass per hashed frame, twice per rollback; pick N so that
it is not every frame (`N=30` is half a second apart).

**Log lines**

| Line | Meaning |
|---|---|
| `NOTE hash check on: every N frames ...` | at start |
| `HASHCHECK frame=m ok compared=C skipped=S us=U` | the first 3 equal frames |
| `HASHCHECK frame=m DIFF pages=p (in_set=a outside_set=b) compared=C regions64k=R [0xBASE:p,..] ranges=[0xS-0xE,..] last_equal_frame=L` | the first 30 differing frames. `in_set`: the page was rewound by the snapshot (nondeterminism inside the set, or a host thread); `outside_set`: nothing rewound it (a page the set misses, or memory the game does not reproduce by itself). At most 16 regions and 16 ranges are listed |
| SUMMARY `hashcheck_compared= hashcheck_differ= hashcheck_last_equal_frame= hashcheck_first_diff_frame= hashcheck_pages= hashcheck_ms=` | only when on |

**What D runs.** `set REV3_RB_HASHCHECK=30`, then `phase3.bat synctest --k 4` on a stage as for the other checks. Read it like this:
`hashcheck_differ=0` means that every hashed frame ended in the same memory in the replay. Otherwise the `DIFF` lines name the pages;
`outside_set` pages that recur in every line are either missing from the set (add them with `REV3_SNAPSHOT_INCLUDE`, or read them in
the `VERIFY ... missing_pages` list) or are memory the game never reproduces by itself (timers, uninitialised scratch): those must be on the
exclusion list of any hash two peers compare, and this run is how that list is found.

**Netplay design (not built; the local part is merged, see "Host write tracking in netplay").** On a desync (the existing checksum of the fighter fields disagrees) both peers
send, at the desync frame f, the same three levels, each level only for what the previous one found different:
1. `RegionHashes` of the frame: 1408 x 8 bytes = 11 KB. `DiffRegions` names the regions that differ (typically 1-5).
2. The page hashes of those regions: 16 x 8 bytes per region. `DiffHashes` on the two vectors names the pages.
3. The 32-byte block hashes of the differing pages (128 x 4 bytes per page), which needs the bytes of frame f on both peers: a per-frame
   shadow (restore verify's `TakeShadow`) or the snapshot slot. Only worth it for the first desync of a session.
Both peers must use the *same skip set*: a skip range that exists on one side only makes a region hash differ although nothing is wrong.
Apply the exclusion list learned with `REV3_RB_HASHCHECK` to both. The three functions are pure (no Dolphin), so rev3-net can use them as
they are; the parts to bring over are `MemoryHasher`, `MemoryHashes`, `DiffHashes`, `RegionHashes`, `DiffRegions`, `RegionBase` and the
`HashMemory` / `HashCheck` pair in SyncTest (its `Capture` calls `HashMemory`; `Resim` calls `HashCheck` after `Compare`).

**Tests.** `MemoryHasher*` (exact pages in MEM1 and MEM2, skip ranges, with and without a direct pointer, no MEM2, empty hashes, 64 KB regions) and
`HashCheck*` (equal run, a page the whitelist misses is named as `outside_set`, a page in the set as `in_set`, exclusions / hardware cuts /
ignore list / dead stack are left out, off by default).

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
| `ppc pc= npc= lr= ctr= msr= srr0= srr1= cr= r1= r2= r13=`, `ppc r3=..r12= exceptions=` and `ppc r14=..r31=` | registers and the pending exception bits (`PowerPCState::Exceptions`: 0x1 decrementer, 0x4 external interrupt are just interrupts waiting while EE is off, not a fault). `r14..r31` are the callee-saved registers: halted in `__OSUnhandledException`, r25..r28 are its arguments |
| `frame <i> sp=<sp> lr=<*(sp+4)>` | up to 16 frames of the back-chain from r1 (`*(sp)` is the next sp, `*(sp+4)` the saved LR word; stops at an unreadable, zero or unaligned sp or when the next sp does not increase). Frame 0's lr is the slot of the current function and stale unless it is in a call; from frame 1 on `lr` is a return address into the function that owns the frame |
| `os cur_thread= cur_context= fpu_owner= sched_disable= run_queue_bits= resched= active_head=` | `*0x800000E4`, `*0x800000D4`, `*0x800000D8`, `*0x8049DAB8` (scheduler-disable count: nonzero while blocked is the freeze signature), `*0x8049DAC0`, `*0x8049DABC`, `*0x800000DC` |
| `threads n=<count> end=null\|cycle(..)\|bad_ptr(..)\|max\|unreadable_link` | the active thread list from `*0x800000DC` via `+0x2FC` (at most 32) |
| `thread 0x.. state= prio= suspend= queue= pc= lr= sp= canary=ok\|bad\|? stack=<end>..<base> [running=1 live=1]` | OSThread `+0x2C8` (u16: 1 ready, 2 running, 4 waiting, 8 moribund), `+0x2D0`, `+0x2CC`, `+0x2DC`; saved context pc `+0x198`, lr `+0x84`, gpr1 `+0x04`; canary = `*(+0x308) == 0xDEADBABE` at the stack end; the running thread (`*0x800000E4`) uses the live registers |
| `thread 0x.. frame <i> sp= lr=` | 6 frames of the back-chain from the thread's sp |
| `thread 0x.. ctx srr0= srr1= lr= ctr= cr= r1= ctx_state= in_stack=1\|0\|? frames=[lr0,lr1,..] [exc=1]` | the thread's SAVED OSContext (what the exception vector or the scheduler stored in the OSThread; offsets cited in the code comment: gpr[i] `+4*i`, CR `+0x80`, LR `+0x84`, CTR `+0x88`, SRR0 `+0x198`, SRR1 `+0x19C`, state u16 `+0x1A2`, bit 0 = FPRs saved, bit 1 = exception), for EVERY thread including the running one (whose `thread` line above uses the live registers). For the thread that took the unhandled exception this context IS the fault: srr0 = the faulting instruction, lr / r1 its caller. `in_stack` = r1 inside `[stack end, stack base]`; `frames` = `*(sp+4)` of up to 8 frames walked from the saved r1 (same convention as the `frame` lines); `exc=1` marks the context the OS reported (see `exc`) |
| `exc none halted=0` or `exc error=<n\|?> srr0= srr1= dsisr= dar= lr= r1= ctx= ctx_src=r26\|cur_context args=r25_r28\|spr name= insn= ctx_state= last_int= last_int_srr0= spr_dsisr= spr_dar=` | the exception the OS halted on, right after the `os` line. Active when the CPU is in `PPCHalt` (0x802A18B8), in `__OSUnhandledException` (0x80315A0C) or has it on the back-chain. In `PPCHalt` the arguments are live: `error`/`ctx`/`dsisr`/`dar` = r25/r26/r27/r28 (`args=r25_r28`); otherwise `error=?` and DSISR / DAR come from the SPRs (`args=spr`; stale unless the last DSI is the fault) and `ctx` is `*0x800000D4`. `srr0 srr1 lr r1` are read from that context, `insn` = the word at its srr0, `name` = dsi / isi / alignment / program / memory_protection ..., `last_int` / `last_int_srr0` = the SDK's `__OSLastInterrupt` s16 (0x8049DA84) and its SRR0 (0x8049DA80; the "Last interrupt" line), `spr_*` = the SPR values always |
| `exc ctxgpr r0..r7: ..` (4 rows) | r0..r31 of that context |
| `exc stack sp=<r1> +0x00: ..` (2 rows) and `exc stackscan sp=<r1> n=<k> [+<off>:<word>,..]` | 16 raw words from the fault's r1, and the words in the 0x400 bytes above it (to the thread's stack base) that look like code addresses (0x80004000..0x80338BE0, word aligned): a guess at the callers when the back-chain is broken |
| `queue <name> at= head= tail= waiters=<n>[<threads via +0x2E0>] head_state= head_pc= head_lr= [extra]` | sleep queues `gx_drawdone 0x8049D950` (+ `drawdone_flag` = byte `0x8049D958`), `vi_retrace 0x8049D4B8` (+ `retrace_count` `*0x8049D4DC`, `game_last_count` `*0x8049CF48`), `dvd 0x8049D7B0`, `nw4r_strm 0x8049D238`, `ax 0x8049D6A0`, `ipc` (`*0x8049D984 + 0x2C`; `queue ipc skipped: pointer ..` when the pointer is not in MEM1 / MEM2), `sc 0x804655A0` |
| `game frame_loop_flags= main_state= no_gx=` | `*0x803C19C0`, `*0x803C11D4`, `*0x803C1A70` as raw u32 (a byte flag shows in the top byte) |
| `hw pe pe_status= pe_token=` | MMIO 0x0C00100A (PE control / interrupt status) and 0x0C00100E (token) |
| `hw cp status=idle\|busy\|breakpoint base= end= rd= wr= rw_dist= bp= gp_read= bp_enable= link= hi_wm= lo_wm=` | `GetCommandProcessor().GetFifo()`; `idle` = rw_dist 0, `breakpoint` = the breakpoint flag |
| `hw pi cause= mask= fifo_base= fifo_end= fifo_wptr=` | processor interface interrupt cause / mask and the CPU write-gather FIFO pointers |
| `words dvd base=.. +0x00: w w w w w w w w` (3 rows), `words nw4r_strm ...` | 24 raw words from 0x20 bytes before the DVD queue (`0x8049D7B0`) and before the nw4r stream queue (`0x8049D238`), i.e. the neighbouring SDK state, for decoding by hand |
| `words dvdlog base=0x80460854 +0x00: ..` (4 rows) | the DVD library's command log: 5 records of 5 words (`StampCommand` 0x802D0624 writes the command and a tick; `*0x8049D7E0`, in the `words dvd` row at +0x40, is the next record), i.e. the last DVD commands the game's library issued |
| `hw di disr= cmd0= dimar= dilength= dicr= wii_dicr= ios_di_busy=0\|1\|none\|skipped(racy)` | DVD interface registers (0x0C006000 status, 0x0C006008 command word 0, DIMAR, DILENGTH, DICR with TSTART in bit 0; 0x0D00601C on a Wii) and `DIDevice::IsBusy()` of IOS `/dev/di` (not read in a racy dump: it walks the IOS device map) |
| `hw vi line= di0=` | vertical beam position (0x0C00202C) and display interrupt 0 (0x0C002030; bit 31 = flag, bit 28 = enable) |
| `dsp csr= to_dsp_hi= ai_dma= src= blocks= left=` and `audio dsp=HLE\|LLE irq= events=<n> [type+cycles,..]` | the existing DSP / audio DMA line and the pending audio CoreTiming events; consistent dumps only (not racy, not at shutdown) |

**Reading an unhandled exception halt** (`pc` in `PPCHalt` 0x802A18B8, `lr=0x80315CE4`). The SDK's exception vector (`OSExceptionVector` 0x80313420 +
`OSDefaultExceptionHandler` 0x803134BC) saves the fault into the running thread's own OSContext, does `stwu r1,-8(r1)` on the faulting thread's stack
(no stack switch; the LR slot of that 8 byte frame is never written, so junk like `0x00400002` above `__OSUnhandledException`'s frame is normal) and
jumps to `__OSUnhandledException`, which prints "Unhandled Exception <n>" and `OSDumpContext(ctx)` (r0..r31, LR, CR, SRR0/1, GQRs, FPRs if the
FP flag is set, a 16 frame back-chain from the saved r1), DSISR / DAR / TB, an explanation for DSI / ISI / alignment / program, and "Last
interrupt", through `OSReport` -> MSL `__write_console` (0x8019E694, the UART; nobody reads it in Dolphin unless the game's symbol map is loaded
and the OSREPORT_HLE log is on), then spins in `PPCHalt`. So the dump of a halted game shows: the live `pc/srr0` = the LAST INTERRUPT taken while
the report was printing (inside `OSDumpContext`), the live r3..r12 = leftovers of the printf engine (r12 = `__write_console`; r4 = 0x10FC / 0x1104 in the two soak runs, probably the number of characters printed),
and `exceptions=0x5` = decrementer + external interrupt pending with EE off. None of that is the fault. The fault is the `exc` line (error number,
srr0, DAR) and the `thread .. ctx` line of the faulting thread. A frame is `stwu r1,-N`: frame sizes are in main.elf, so the faulting r1 can be
matched to a call chain by adding them up (the stream thread of soak `20261016_212543` runs 8 and 9: root `0x909C9598`, `ThreadFunc` -0x10, `ExecuteTask` -0x20,
`StrmDataLoadTask::Execute` -0x20, `LoadStreamData` -0x70, `DvdLockedFileStream::Read` -0x20, `DvdFileStream::Read` -0x10, `DVDReadPrio` -0x20 = `0x909C9488`,
which is exactly the faulting r1). The dump stays address-only; `tools/rollback/soak.py --symbols` maps the addresses.

How to use it: start Dolphin with `REV3_SYNCTEST=4` (K=4) as before, play until it freezes, wait 3 seconds (the stall dump needs 2 s plus
the pause), then close Dolphin. The log then ends with the `DUMP stall` blocks and a `DUMP shutdown` block. Read the thread with `state=4`
and its `pc` / `lr` / `queue`, and the queue lines: a thread waiting in `gx_drawdone` while the CP is `idle` and `drawdone_flag` is 0, or in
`vi_retrace` while `retrace_count` equals `game_last_count`, shows which event was lost. Variables: `REV3_SYNCTEST_DUMP=0` off,
`REV3_SYNCTEST_DUMP_STALL_MS=n` (100..600000), `REV3_SYNCTEST_DUMP_PAUSE=0` (no CPU pause, racy dump only).

Tests: `Rev3Rollback.FreezeDump*`, `BackChain*`, `RestoreHwLine*`, `Stall*`, `SyncTestDumpsAtArm*` (fake memory with a fake OS); the exception lines:
`FreezeDumpPrintsEachThreadsSavedContext`, `FreezeDumpReportsTheUnhandledExceptionTheOsHaltedOn` (a synthetic OSThread holding a DSI context, CPU in
`PPCHalt` with r25..r28 set; raw stack and stack scan), `FreezeDumpExceptionFallsBackWhenTheArgumentsAreNotUsable` (unusable r25 / r26, a racy dump
inside `OSReport`, no readable context, no registers).

## Alarm queue (`REV3_ALARMCHECK`, `REV3_RB_IOS_GATE`; Oct 17, 2026, soak `20261016_233913` run 7)

What froze: the host's sound pump thread (0x803D3D80, prio 3) spun for 60 s in `InsertAlarm` (0x80313694, walk 0x80313768..0x8031382C) called from `OSSetAlarm` <- `OSSleepTicks` <- `zz_8018d1c0_`,
MSR 0x1032 (interrupts off), the main thread READY and never scheduled. The SDK keeps ONE doubly linked list of pending `OSAlarm`s sorted by fire time (head 0x8049DA38, tail 0x8049DA3C;
OSAlarm: +0 handler, +8 fire s64, +0x10 prev, +0x14 next, +0x18 period s64, +0x28 userData) and `InsertAlarm` walks it from the head until the first node that fires later.
The period catch-up is one division, not a loop, and the saved LR shows no call after the prologue, so the spin was the list walk: it ends only on a null `next` or a later node, so
the list was **cyclic** (proven from the disassembly and the registers; the cycle itself is not seen, the dump had no queue lines then).

What a restore does and does not touch (code reading, `Rev3Rollback.cpp`): the whitelist pages and the stack window are written back; the time base only with `REV3_SYNCTEST_TB=1` (run 7 had it off);
the decrementer (mtdec value and CoreTiming's `DecCallback` event) is never saved or restored; the audio rewind removes and re-adds only DSP / AI events. The walk never reads the time base, and
`DecrementerExceptionCallback` re-programs the decrementer from the queue head, so a late or early decrementer only shifts when alarms fire; **it cannot create a cycle, and no decrementer
save/restore was added** (nothing shows it is needed). The candidates left (not proven): (1) an alarm armed while already queued (`OSSetAlarm` on a node that is still in the list): the port in the
unit tests (`InsertAlarmLikeTheSdk`) shows that re-arming a queued node with another fire time makes a forward cycle in most cases (a scratch simulation: 73 % of random double insertions, and the
next insert then never ends); IOS replies are the way a restore can cause it: Dolphin's `EmulationKernel::EnqueueIPCReply` writes the reply into guest memory at once and hands it to the PPC
through the CoreTiming event `IPCEvent` 500 ticks later, the fork gates only on `/dev/di`, so a restore across a pending reply leaves a completion callback (the PlayRecord alarm 0x80465320 is
re-armed from NAND completions) running against rewound state; (2) a half-rewound queue: a node inside the snapshot, its neighbours or head/tail outside (write tracking with `wt_misses=0` argues against it).

What is added:
- Freeze dump lines (`DUMP .. time ..`, `alarmq ..`, `playrecord ..`): `time tb=.. dec=.. dec_event=+<cycles>|none|? core_ticks=..` (the fake time base, the decrementer as mfdec reads it, whether CoreTiming
  has a `DecCallback` pending and when; `?` in a racy dump, which does not read CoreTiming's queue), `alarmq head=.. tail=.. n=.. orphans=.. status=ok|empty|<problems> now=<system time> tps=..` and per
  node `alarmq[i] at=.. name=.. handler=0x..(Name) fire=.. due=+x.xxxms period=.. prev=.. next=.. user=.. tag=..` (`on_stack_of=<thread>` when the node sits on a thread's stack, as a sleeping thread's does),
  `alarmq orphan[i]` for nodes only the backward chain from the tail reaches, and the PlayRecord state words (0x8049C9A8 ...). Problems: `cycle(0x.. index n is index m)`, `prev_link`, `order`,
  `tail`, `head_tail`, `bad_pointer`, `too_long`, `backward_cycle`, `backward_end`. The walk is read-only and bounded (128 nodes), so a dump of a cyclic queue terminates.
- SyncTest `ALARMCHECK` (default on): `AlarmWatch` at every capture, `AlarmCheck` before and after every restore write-back. A bad queue logs `ALARMCHECK bad capture frame N last_ok_frame=M last_restore=a->b ios=[..]`
  (only the transition, so the first bad frame is placed between two captures), `ALARMCHECK bad before|after restore a -> b last_ok_frame=.. ios=[..]` and the queue itself as `ALARMCHECK alarmq ..` lines; the first 12 are
  written, then `ALARMCHECK further failures are only counted`. One `ALARMCHECK ok capture ..` line shows the check ran. The SUMMARY / net summary fields: `alarm_checks alarm_bad_captures alarm_bad_before
  alarm_bad_after ipc_slots ipc_restores`. Reading it: `bad capture` with a good `last_ok_frame` before = the first run broke the queue; `before` good and `after` bad = the write-back or what it left outside
  (IOS replies) did it.
- IOS events: `ipc_slots` / `ipc_restores` count captures and restores made with an `IPCEvent` / `IPCInterrupt` / `FinishDICommand` pending, and the first five restores log `NOTE restore a -> b with IOS events pending: IPCEvent+500,..`.
  `REV3_RB_IOS_GATE=1` makes them gate like a disc read in flight (the slot is not a rollback target, rollbacks wait while one is pending): an A/B switch for the next soak, off by default because it may
  postpone rollbacks often.

What to look for in the next soak: `ALARMCHECK bad` lines (frame, `last_ok_frame`, `ios=[..]`, the node that is queued twice), `ipc_restores` against `restores`, and in any freeze dump the `alarmq` block (a cycle shows as
`status=cycle(..)`, the nodes before it show what armed twice: `name=`, `handler=`, `due=`). If `ALARMCHECK bad before` appears with a non-empty `ios=[..]` the IOS hypothesis is confirmed and the gate is the fix to try.

Tests: `Rev3Alarm.*` (17: queue walk, double insertion and the endless SDK walk, orphans, dump lines, SyncTest ALARMCHECK in the first run and after a half-rewound restore, off switch, IOS counters and gate).

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

## Netplay (M3): direct connect, lockstep, rollback behind a gate

Milestone M3 of the netplay plan: two emulators talk to each other and nobody else. Set `REV3_NET_ROLE` on both machines and the 0xA0 hook
answers come from a `Rev3::Net::RollbackSession` (`Rev3Session.{h,cpp}`, the pure state machine of M2) instead of the echo / SyncTest
path. Files: `Slippi/Rev3NetLink.{h,cpp}` (pure: wire, options, HELLO, arm gate; unit tests in `Rev3NetLinkTest.cpp`),
`Slippi/Rev3Net.{h,cpp}` (ENet transport, snapshot store on SyncTest's network mode, the per-command module),
`Rev3Rollback.cpp` (HandleCommand branch, options glue), `EXI_DeviceSlippi.cpp` (skips Slippi's online stack).

**Default is lockstep.** Without `REV3_NET_ROLLBACK=1` the game is held until the remote pad of the frame has arrived (with input delay
D); no snapshots are taken, nothing is scanned, the only extra work is a fighter checksum read every 30th frame. Rollback code is
present and unit/loopback tested, but the gate is off until you ask for it.

### Variables (read once, at the first hook command)

| variable | default | meaning |
|---|---|---|
| `REV3_NET_ROLE=host\|guest` | unset = no netplay | turns everything below on. The host is game port 0 (P1), the guest port 1 (P2). The local pad is always command port 0 on both machines; the reply carries both ports. Anything else is an `ERROR` on screen and in the Rev3 log and the game plays offline |
| `REV3_NET_PEER=<ip>:<port>` | required | the other machine (IPv4 or host name, port 1..65535). Only this IP is accepted; a connection from anywhere else is refused (`NET refused a connection from ...`). `<ip>:*` = **accept only** (Oct 17): this side never connects out and takes the first connection from that IP at any port, answering to wherever it came from. For the HOST of a session through a local UDP tunnel, Porthole on Steam above all: the guest connects to the tunnel's port on its own PC (`REV3_NET_PEER=127.0.0.1:<that port>`, its own `REV3_NET_PORT` another number), and the host's packets arrive from the tunnel's proxy at `127.0.0.1:<a port nobody knows in advance>`, so the host runs with `REV3_NET_PEER=127.0.0.1:*`. Log: `NET accept only: waiting for the peer to connect from 127.0.0.1 (any port) on UDP 2626`. Direct connect (both sides connecting at once) stays the default; rev3-mod `netplay.bat --via porthole` sets all of this |
| `REV3_NET_PORT=n` | 2626 | UDP port this emulator listens on and sends from. Both sides connect to each other at the same moment from these fixed ports (hole punching style), so use one pair like host 2626 / guest 2627 and open or forward them; two connections result, both are used, duplicates are dropped by sequence number |
| `REV3_NET_DELAY=n` | 2 | input delay D in frames, 0..15. The host's value wins (a difference is logged, not a mismatch) |
| `REV3_NET_ROLLBACK=0\|1` | 0 | 1 lets the arm gate open (prediction, rollback). 0 = pure lockstep. Part of HELLO: both sides must agree |
| `REV3_NET_ROLLBACK_MAX=n` | 5 | MAXR, the most frames ever predicted = deepest rollback, 1..7. Snapshot slots = MAXR + 2 |
| `REV3_NET_ARM=n` | 600 | hook calls in a row with both fighters found before the gate may open (SyncTest's number, 10 s) |
| `REV3_NET_QUIET=n` | 60 | hook calls in a row without a disc read in flight before the gate may open |
| `REV3_NET_TIMEOUT_S=n` | 120 | connect + HELLO (the frame-0 barrier). D starts the two emulators by hand |
| `REV3_NET_WAIT_MS=n` | 10000 | no remote pad this long in lockstep (or no answer to an ARM_PROPOSE) = the peer is gone, play on offline |
| `REV3_NET_LOG=file` | `<User>\Logs\rev3_net_<role>.log` | the net log (below). The Rev3 log (`rev3_synctest.log`) only gets a pointer line |
| `REV3_NET_STABLE=file` | none | `{"ranges":[...]}`: memory whose CRC both sides compare at arm time (START_CHECK); the file's CRC is part of HELLO so both must use the same file |
| `REV3_NETSIM`, `REV3_NET_BUSY`, `REV3_NET_RANGE_CRC` | | M4, see "Netplay (M4)" |
| `REV3_PAD_SCRIPT` / `REV3_PAD_RECORD` / `REV3_TRACE` | | as in the determinism kit, with the netplay meaning below |
| `REV3_SYNCTEST` | | ignored in a netplay run (noted in the net log) |

With `REV3_NET_ROLE` set: no Slippi Rust extensions, user manager / login, matchmaking, spectator server, replay writing or game reports
start (a packet capture shows only the peer); `EXI_DeviceSlippi` keeps its Melee-side objects but never calls into Rust and answers
nothing except 0xA0. `REV3_PAD_SCRIPT` replaces the LOCAL pad (command port 0) before the session sees it, so a scripted guest plays
a recorded game. `REV3_PAD_RECORD` records the pads the game actually got (the reply's two ports) once per frame: not for resimulated
frames and not for the call a rollback answers, so a record of a rollback session replays as the same game. `REV3_TRACE` is plain play.

### Wire

Frame numbers on the wire are the hook's frame counter + 1 (Slippi's first frame is 1; one constant, `WIRE_FRAME_OFFSET`). The hook
counter is the session clock; the first command must be frame 0 or the run leaves the session (`ERROR NET the first command is frame N`).

| message | channel | content |
|---|---|---|
| PAD `0x80` | 1, unsequenced | `u8 id, s32 newest wire frame, u8 sender port, s32 checksum frame, u32 checksum`, then the unacked local pads, 8 bytes each, newest first (Slippi's `SendSlippiPad` layout, big endian) |
| PAD_ACK `0x81` | 2, unsequenced | `u8 id, s32 acked wire frame, u8 sender port` |
| HELLO `0x90`, START_CHECK `0x91`, DESYNC_DUMP `0x92`, BYE `0x93`, RANGE_CRC `0x94` (M4, testing), ARM_PROPOSE `0x95`, ARM_ACCEPT `0x96`, ARM_REJECT `0x97` (Oct 17) | 0, reliable | `u8 id, u32 sequence, payload` (payloads: `Rev3Session.h`) |

The receiver stores every pad of a packet by its own frame and acks R, the highest frame with all pads up to it present, so a lost packet
never makes an ack lie; the first 64 or more remote pads are contiguous from frame 0. A sender re-sends its unacked pads after about
one round trip (at least 15 ms) without an ack, from the same wait loop that blocks the game (Slippi only re-sends on the next queued
pad). Every PAD packet carries the checksum of the latest final frame (every 30th frame).

### HELLO (both machines must match, else the session is refused with an OSD line and a log entry, and the game plays offline)

Game id (`RNEEDA`), hook protocol (the version byte of the hook's first command: 2, or 3 with the hiding patches, M6), fields version and digest (0xD4076EF2), fork build (`Common::GetScmRevGitStr`), CPU core, CPU thread
(single / dual), SyncGPU, DSP engine (HLE / LLE / LLE thread), custom RTC (enabled + value), overclock, MMU, FastDiscSpeed, cheats,
Bluetooth passthrough, Wii Remote sources 1..4 + balance board, the CRC-32 of the hook code region 0x800024A0-0x8000291F (read when the
first command arrives; the data block 0x80002400-0x8000249F changes every frame and is not included), `REV3_NET_ROLLBACK`, the gate
numbers, and the `REV3_NET_STABLE` file CRC. Two equal roles are a mismatch. The input delay is the host's.

### The arm gate (only with `REV3_NET_ROLLBACK=1`)

Opens after: both fighters found for `REV3_NET_ARM` normal calls in a row, no disc read in flight for `REV3_NET_QUIET` calls, nobody at
health 0 (fighter + 0x28), the adaptive snapshot set learned, the session in lockstep. Then the snapshot set is installed and the two
machines agree on the frame to arm at (ARM_PROPOSE / ARM_ACCEPT, next section); at that frame they exchange START_CHECK (the exact state)
and prediction starts. How it closes again (fighters lost, a
fighter at health 0) and what a disc read does is the M4 gate rule below: both machines close at the same frame, decided on final
frames only. The set never changes while snapshot slots are in use.

### The net log (`REV3_NET_LOG`)

One file per run, appended, flushed per line, no timestamps, so rev3-mod `tools/rollback/phase4.py` reads it: lines start with
`ROLLBACK`, `STALL`, `DESYNC`, `ERROR`, `CHK`, `GATE`, `SUMMARY` (and `NET`, `HELLO`, `SESSION`, `LEAVE`, `RESTORE`, `RESIM`, `NOTE`,
`ARM`). `CHK frame=<N> value=0x<8 hex>` is the checksum of final frame N (every 30th); both machines log the same numbers in a clean game.
On screen (OSD): `connected to <peer>`, HELLO mismatch (with the first differing item), desync, `the peer left`.

Typical clean start: `NET start ...`, `HELLO sent`, `NET connected to ...`, `HELLO ok peer=...`, `SESSION lockstep from frame 0`, `CHK frame=0 ...`.
`NET connect timeout ...` and `LEAVE ... reason=...` explain every exit; `SUMMARY` and `NET closing: rtt_us=..` are written at shutdown.

### Verification so far

- Unit tests (`Rev3NetLinkTest.cpp`, additions to `Rev3SessionTest.cpp` and `Rev3RollbackTest.cpp`): PAD / ACK layout and loss, reordering,
  resend timing, reliable envelope and dedupe, option parsing, HELLO mismatches, the arm gate, SyncTest network mode (ring, restore,
  deferred layout changes), checksum-from-fields equals `FighterChecksum`, record of the reply pads.
- A Linux loopback run of the real `Rev3Net.cpp` over real UDP (ENet), two sessions in one process with a deterministic game: 600-3000
  frames, lockstep and rollback (with random delay on one side, hundreds of rollbacks), identical final states, matching `CHK` lines,
  no `DESYNC`/`ERROR`; a HELLO mismatch refuses cleanly. That run found and fixed a barrier deadlock in the session (the HELLO was only
  sent from `Step`, not from the wait that blocks it).
- Not yet run: two real Dolphin processes with Rev3 (D does that; see the report in the commit / PR).

## Netplay (M4): network simulator, hiding resimulated frames, a deterministic gate, log volume, desync dump

Milestone M4 makes rollback testable and safe to leave running: a simulated bad network in the real transport, the first version of
hiding resimulated frames, a gate whose open/close decisions both machines always take at the same frame, a log that stays readable in a
long match, and a desync report that names the first wrong field. Files: `Slippi/Rev3NetSim.{h,cpp}` (new, pure, tests in
`Rev3NetSimTest.cpp`), `Rev3Session.{h,cpp}` (gate rule, hold / relayout, hiding interface, log limits, dump, range CRCs),
`Rev3NetLink.{h,cpp}` (options, arm gate tracker), `Rev3Net.{h,cpp}` (NetSim in the network thread, hiding hookup),
`Rev3Rollback.{h,cpp}` (RESTORE log policy, `NetRangeCrcs`, `DolphinResimHider`), `Core.{h,cpp}` (the resim throttle flag).

### Variables

| variable | default | meaning |
|---|---|---|
| `REV3_NETSIM=delay_ms=80,jitter_ms=30,loss_pct=5,seed=1` | unset = off | the network simulator (below). Any subset of keys, any order; unset keys are 0, `seed` defaults to 1. A bad value is `ERROR NET REV3_NETSIM=...` in the net log and **no** simulation (the run is not the test that was asked for) |
| `REV3_NET_BUSY=n` | 1 | consecutive armed calls with a disc read in flight before a HOLD starts (below; 1 = from the first busy call, was 3 until the dirty-slot fix); 0 = never hold. 0..1000. Local to a machine, not part of HELLO |
| `REV3_NET_RANGE_CRC=1` | 0 | testing only: both sides exchange the CRC of every snapshot range at each checksum frame (below). Both machines should set it; one side without it simply gets no comparison |

### REV3_NETSIM

Implemented in `Rev3NetSim.cpp` and applied in `EnetTransport` on what the network thread **receives**, after ENet and before the session sees
it. Every received message (PAD, PAD_ACK, and the reliable ones) is held until `now + delay_ms +- jitter_ms` (uniform, never below 0; jitter
reorders messages like a real network; ties leave in arrival order). `loss_pct` of the **PAD / PAD_ACK** packets are dropped; reliable
messages (HELLO, START_CHECK, DESYNC_DUMP, BYE, RANGE_CRC) are late but never dropped, which is what ENet's own retransmission gives you.
The ENet connect handshake is not a message of this layer and is never touched. Both machines apply it to what they receive, so the round
trip is `2 x delay_ms` (`delay_ms=80` is a 160 ms RTT). The random draws are splitmix64 (no `std::` distributions: their output differs between
standard libraries) from two streams per instance, one for the loss draws (only PAD / ACK packets consume it) and one for jitter, seeded from
`seed` and the role (host 0, guest 1) so the two sides do not mirror each other. The same seed and the same arrival sequence give the same
drops and release times on every platform. The network thread shortens its ENet wait to the next release time.

Log: `NET netsim delay_ms=80 jitter_ms=30 loss_pct=5 seed=1` at start; `SUMMARY` carries `netsim_dropped=` and `netsim_held=` (messages
held for a non-zero time).

### Hiding resimulated frames, v1

While `RollbackSession::Resimulating()` is true (from the RESTORED reply to the end of the last re-run frame):

- the **throttle is off** (`Core::SetIsThrottlerResimDisabled`; `GetIsThrottlerTempDisabled()` returns it OR-ed with the user's flag. A flag of
  its own, because the hotkey scheduler rewrites the user's flag every loop), so the re-run frames cost CPU time only, not wall time;
- the **audio output is muted** (`SoundStream::SetVolume(0)`; restored with `AudioCommon::UpdateSoundStream`, i.e. the user's volume, or 0 if
  the user muted). The DSP is never paused or touched: the emulated audio hardware keeps its timing. Backends without volume control
  (`SupportsVolumeChanges`) ignore it.

Both are restored when the resimulation ends, when the session leaves (desync, BYE, timeout) and at shutdown. The log has one line
`NOTE resim hiding v1: throttle off + mute` the first time. The session talks to an injectable `ResimHider` (`Rev3Session.h`); the unit tests use a fake.
Not in v1: skipping the render / SFX of re-run frames inside the game (M6, below), so with a protocol 2 hook a resimulated frame still draws.

### The gate rule (both machines always agree on WHEN the shared mode changes)

The shared modes are LOCKSTEP and ARMED. In M3 the gate was evaluated on the live state every call, which a misprediction can change: one machine
could see a fighter at health 0 in a timeline that never happened and close while the other did not. Now:

- **Open** is an agreement over the wire (see "Arming is an agreement", below). It is decided in LOCKSTEP, where every frame is final, and takes
  effect at a frame F both machines named; a side whose local conditions differ answers "not ready" and both stay in lockstep while pads keep
  flowing. (Until Oct 17 the side that was ready sent START_CHECK and blocked for the peer's; see the deadlock below.)
- **Close** (a fighter at health 0, or fighters not found) is decided on **final frames only**. The live flags of every call (resimulated
  ones too) are recorded with the capture of the frame; when frame F becomes final (all remote pads below F were played as confirmed) its record
  is the same on both machines. The first final frame F whose record says "lost" schedules the close for frame **F + MAXR + 1**
  (`GATE close scheduled final_frame=F effective_frame=E why=...`). F is always declared final by call F + MAXR (the stall rule keeps
  N - R <= MAXR; the session declares finals before it rolls back, so a run of rollbacks cannot delay it), so neither machine has passed
  E when it learns of the close. Until E both keep predicting; at call E both enter DRAIN (`GATE close frame=E ...`), confirm everything, roll
  back what is due and continue in lockstep. A close applied after its frame would be `GATE close late` and counts in `gate_late` (must be 0).
  A live state, which may be a misprediction's, never closes anything. A frame without fighters can't be snapshotted, so it is never predicted
  (the call waits for the real pad) and is recorded as lost.
- **Local reasons never change the shared mode**, because the peer cannot know about them:
  - *busy hardware*: see the next section;
  - *the snapshot set would change* on one machine (a newly learned page, a hardware cut) -> **RELAYOUT**: that machine drains (waits for
    every remote pad, rolls back what is due), installs the new set and carries on ARMED (`GATE relayout frame=..`, `GATE relayout done`). The peer
    sees a short stall and nothing else; the frames played are the same, so the final states cannot differ.

The unit tests (`Rev3SessionTest.cpp`) include a mispredicted live state that looks lost on one machine only (no close on either), the
other direction (a misprediction hides a real loss until the true timeline makes the frame final), and a 150-seed simulation with a
"health 0" that is a function of the live state, mispredictions, delay, jitter, loss, busy hardware and relayouts, which asserts that both
machines log the same `GATE close frame=` lines in the same order and that `gate_late` stays 0. The simulation counts the calls where the live
state said "lost" and the true timeline did not (a gate that followed the live state would have closed one side there) and the test requires
that they happen; a mutation that follows the live state makes the tests fail.

### Arming is an agreement, not a wait (Oct 17; the soak deadlock)

**What happened.** D's bot soak 20261016_212543: in 2 of 6 runs both machines left after exactly 10 s at the start of round 2 (`run_6`: guest
`ARM frame=8848 refused: no START_CHECK from the peer within 10000 ms`, then `LEAVE ... reason=no remote pad for 10000 ms (frame 8851, R=8850)`; `run_2`:
host, frame 9835). Round 1 had ended with `GATE close` at the same frame and `DRAIN done` on both. The old `TryArm` sent START_CHECK and **blocked** for the
peer's as soon as ITS gate was ready. The gate inputs are local (the hardware quiet streak, the learned snapshot set, write tracking, the prepare
step), so the other machine's gate opened some frames later; in those frames it ran lockstep, needed the first machine's pad for frame N + D + 1, and the
first machine was blocked in the arm wait and had stopped sending pads. Each waited for the other until `wait_timeout_ms`. The host of run_2 then played
on offline while the runner waited for it (verdict TIMEOUT; see "Bot soak" for the classifier fix).

**The rule.** Nothing may wait for a decision the peer takes from local inputs; a machine that cannot yet arm must not stop the pad stream. Arming is
now agreed in three small reliable messages (payloads in `Rev3Session.h`; `id` numbers the proposer's proposals):

| message | meaning |
|---|---|
| ARM_PROPOSE `id, frame F, at_frame, gate text` | the sender's gate is ready; arm at call F. F = its current call + a margin (about one round trip + 2 * D + 6 frames, at least `arm_margin_min` = 12, at most `arm_margin_cap` = 150; +16 after each TooLate) |
| ARM_ACCEPT `id, F, at_frame` | the answerer's gate is ready now and F is still ahead of it; it **will** arm at F |
| ARM_REJECT `id, F, at_frame, reason, gate text` | `not ready` / `too late` (already at or past F: the proposer grows its margin and proposes again at once) / `not lockstep` / `crossed` (both proposed at once: the host's proposal wins, the guest drops its own) / `busy` (already agreed to another) |

- The proposer goes on playing LOCKSTEP (its pads flow) and arms at call F **only if the ACCEPT is in**; the accepter arms at F unconditionally, because
  it said yes. A rejected side retries with a back-off (30 frames, doubling up to 300) and the rejecter proposes itself when its gate turns ready.
- At F both machines decide on **shared** state only (both fighters found, nobody at health 0: identical on both in lockstep). If it fails both cancel
  alike and silently (`ARM id=.. frame=.. cancelled: ...`), a fighter-less F is not an error.
- The only wait is the proposer's at call F when the answer has not arrived (`Block::ArmAnswer`, `arm_waits` / `arm_wait_ms_max` in SUMMARY). The answer
  is sent the moment the peer reads the proposal, and every wait of the session reads messages, so it does not depend on the peer's gate. More than
  `wait_timeout_ms` without it = a dead link = `LEAVE reason=no answer to ARM_PROPOSE id=.. frame=.. within .. ms`.
- START_CHECK (the exact state at F) is sent when arming and **compared whenever the peer's arrives**, without any wait. A machine that is ARMED while
  the peer is still in its last lockstep call is safe (lockstep frames are final; the armed one only predicts when the pad is not yet there). A
  mismatch is `DESYNC start frame=..` and the session is left. The first ARMED call with busy hardware starts as a HOLD (the busy check of that call
  was not seen by the gate).
- Review of the other waits (all pad waits, none on a decision): LOCKSTEP / HOLD / STALL / DRAIN wait for the peer's pad of a frame <= the current one,
  which the peer sent before it could wait (a call stores and sends its own pad for N + D before it blocks); RELAYOUT is a local drain; the scheduled
  CLOSE has no wait of its own (both enter DRAIN at F + MAXR + 1 from final frames). Header comment "EVERY WAIT AND THE PAD STREAM" has the table.
  The gate-close rule above is unchanged.

**Log lines** (net log): `ARM propose id=.. frame=.. (call N + margin M; rtt .. ms, delay D) own gate: <inputs>`, `ARM accept id=.. frame=..`,
`ARM agreed id=.. frame=..`, `ARM reject id=.. frame=.. reason=.. ...; own gate: ..; peer gate: ..` (on the machine that refused), `ARM propose id=.. rejected
by the peer: <reason> ... own gate / peer gate`, `ARM id=.. frame=.. cancelled: ..`, `ARM armed id=.. frame=.. (own|the peer's proposal)`, `GATE open frame=..`,
`CHK start frame=.. (both machines agree)`, `ARM crossed: dropping own proposal ...`. The first 8 refusal lines are written in full, then every 20th
(the rest are counted in `arm_rejected`). The gate inputs are `fighters_found=1 (streak n/ARM) hardware_quiet=1 (streak n/QUIET) health_zero=0
learning_done=1 layout_pending=0 open=0 prepared=0 write_track=armed learned=.. deferred=.. restarts=.. misses=..` (`GateTracker::Describe`,
`SyncTest::NetWriteTrackText`) so a refusal says which input held that machine back. SUMMARY gains `arm_proposed arm_accepted arm_rejected arm_cancelled
arm_waits arm_wait_ms_max` (`arm_refused` counts the refusals this machine gave).

**What to look for in the next soak.** `ARM propose` followed by `ARM accept` and `ARM agreed` / `GATE open frame=` with the same frame on both logs at
the start of every round; no `LEAVE ... no remote pad` at a round change; `arm_wait_ms_max` in the tens of milliseconds (a round trip), not seconds; if
one side shows many `ARM reject ... not ready`, the `own gate` text names the input that is slow (a quiet streak that keeps restarting = a disc read
every few frames; `learning_done=0` = the adaptive set is still learning; `prepare_failed` = the snapshot set was unusable).

Tests: `Rev3Session.*Arm*` (the protocol, crossing, expiry, back-off, cancel at F, START_CHECK compare), the two-machine simulation with one gate ready
N frames before the other (`OneGateReadyFramesBeforeTheOtherNeverDeadlocksAndBothArmAtTheSameFrame`, N = 1..89 frames, loss and jitter; the old code left
after 10 s in 4 of 10 sampled cases, all with N >= 8), `WhenTheOtherGateNeverBecomesReadyBothStayInLockstep...` (pads keep flowing, no LEAVE), and the 1000-seed lossy run, whose
parameters now include a random per-machine gate delay.

### Busy hardware (disc read / DVD DMA)

A rollback while a disc read is in flight can't rewind the read, so in M3 it was postponed until it would exceed MAXR and then **forced**
(`forced=hardware_busy` on the ROLLBACK line). M4 stops that from happening in normal play without changing the shared mode: after
`REV3_NET_BUSY` (1, the first busy call) armed calls in a row with a disc read in flight the machine starts a **HOLD**: it predicts nothing while the read lasts
(each call waits for the real remote pad of its own frame, `GATE hold frame=..`), so no rollback can become due while the hardware is busy. The
HOLD ends after 2 calls without a read (`GATE hold end frame=.. held_calls=..`). A misprediction made before the hold is still postponed
while the depth allows, as before. The gate itself stays open for the peer: a HOLD is invisible on the wire, it is only a machine that
waits for pads more often. `SUMMARY` counts `hw_holds`, `hold_calls`, `postponed`, `forced_hw`; a `forced_hw` that is not 0 in normal play is a bug report. This
is the one place where the brief's "close the gate on sustained busy hardware" is done as "hold" instead: a close must be the same on both
machines, and the peer cannot see our disc.

**Busy means two views (since the dirty-slot fix).** A disc read counts as in flight when the hardware / IOS says so (`HardwareView::DiscReadInFlight`: DI TSTART or
the IOS /dev/di device busy) OR the game's own DVD library does: the u32 at `0x8049D870` (`executing`, decompiled dvd.c: `stateReady` 0x802D32D4 sets it to 0 when the
queue is empty or to the popped command block, and to `0x80460520` (DummyCommandBlock) while a command's callback runs; `DVDReadAbsAsyncPrio` 0x802D458C starts
`stateReady` only while it is 0) is a pointer other than 0 / the dummy block (`SyncTest::GameDvdBusy`). Both feed the gate (HOLD, quiet) and every capture.

**Dirty slots.** D's freeze at the match end (net_d2_r5, bfb8716): CPU idle, the main thread in `DVDCancel` (stopping the BGM stream) waiting on the DVD thread queue, the stream
loader in `DVDReadPrio`: the game's DVD library believed a read was executing that IOS no longer had. A rollback had restored a frame captured while a stream read was in flight
(issued before the slot, completed before the restore), so its completion never came again and every later DVD command queued behind it. The restore-time check (`io_postponed`) cannot see
that: the hardware is idle at restore time. So: (1) every capture records `io_dirty` (either view; `NOTE slot of frame f captured with a disc read in flight (hardware=.. game_dvd=..)`
for the first 5); (2) a slot with `io_dirty` is never a rollback target while an earlier clean slot is in the ring: `RollbackSession::ChooseCleanTarget` takes the newest earlier clean
slot (`ROLLBACK redirected frame=g -> f`; the frames in between are verified and are resimulated with the pads they were played with; the depth grows by the difference), and with no clean
slot it restores the dirty one anyway and writes `WARNING ROLLBACK frame=g: its slot was captured with a disc read in flight and no earlier slot in the ring is clean` (a desync / freeze
risk, counted); (3) the HOLD starts at the first busy call (`REV3_NET_BUSY=1`), so no frame is predicted while a read is in flight and a dirty slot is practically never a misprediction.
SyncTest cannot redirect (its ring is K + 1 slots): a restore of a dirty slot is skipped (`io_dirty_skipped`). SUMMARY lines (net: the session's, then the store's note; SyncTest: its own) carry
`io_redirected=.. io_no_clean_slot=..` (session) and ` io_dirty_slots=.. io_dirty_hw=.. io_dirty_game=.. io_dirty_skipped=..` (snapshot machinery; `io_dirty_game` counts captures where only
the game view saw the read). Residual: a rollback that is FORCED while a read is in flight (depth reached MAXR) restores into a game whose library may already have moved on; `forced_hw` must stay 0.
Tests: `Rev3Session.ARollbackWhoseSlotWasCaptured*`, `TheRedirectSkips*`, `WithNoCleanSlotInTheRing*`, `CleanSlotsRollBack*`; `Rev3Rollback.TheGamesDvdLibraryView*`, `NetModeRecordsWhetherADiscRead*`,
`TheHardwareViewMarksSlotsDirtyToo`, `SyncTestDoesNotRestoreASlotCaptured*`, `TheGameViewPostpones*`; `Rev3NetLink.TheDefaultHoldStartsAtTheFirstBusyCall*`.

### Log volume

`ROLLBACK`, `STALL` and `RESIM` detail lines of the session: the first 200 of each kind are written in full (a `STALL end` or `RESIM done` line goes with
its start line, written or left out together). After that, one line per window of 600 frames, only when the window left lines out:
`ROLLBACK depth_hist=1:12,2:7,3:2 window=1210-1809 rollbacks=21 stalls=3 stall_ms=41.0 postponed=0 forced_hw=0 lines_left_out=48`. The counts
cover **all** events of the window, written or not; `SUMMARY` has the exact totals (`depth_hist=[..]`, `log_suppressed=`). SyncTest's `RESTORE` lines follow the same
numbers in a netplay run (200 in full, then `RESTORE summary frames=a-b n=.. us_mean=.. us_max=..` per window). `CHK`, `DESYNC`, `ERROR`, `GATE`, `LEAVE`,
`SUMMARY` are never limited.

### Desync dump

On a checksum mismatch both sides exchange the field trace of the last frames (M3) and each writes, to its own net log:

    DESYNC first_diff frame=106 field=p1.<name> own=0x.. peer=0x.. fields_differing=3 last_equal_frame=105
      <up to 8 differing fields>
    DESYNC fields frame=106 side=own player=1 <all 61 fields of that player>
    DESYNC fields frame=106 side=peer player=1 ...
    (and player=2)

So the log alone names the first differing frame and field, and holds both sides' complete 61-field arrays for it.

`REV3_NET_RANGE_CRC=1` (testing only) additionally sends, at every checksum frame, the CRC-32 of each snapshot range (reliable message `0x94`; taken
from the frame's snapshot slot, or from live memory at the frame's own call in lockstep) and logs `NOTE range_crc frame=N ranges=.. differing=..`
plus one line per differing range. It never triggers a desync (some ranges legitimately differ between two healthy runs: audio and picture
memory); it is what tells you which memory to look at when the field checksum fires. `SUMMARY` has `range_checks=` and `range_diff_frames=`.

### Verification

- Unit tests (`Rev3NetSimTest.cpp` new: parsing, determinism per seed, stream separation, band and ordering of jitter, loss only for PAD / ACK, loss rate,
  independence of the loss pattern from jitter and reliable traffic; additions to `Rev3SessionTest.cpp`, `Rev3NetLinkTest.cpp`,
  `Rev3RollbackTest.cpp` for the gate rule, hold, relayout, hiding, log limits, the dump, range CRCs, options, `RESTORE` windows, `NetRangeCrcs`).
- Loopback run (real `Rev3Net.cpp` + ENet over 127.0.0.1, two NetModules in one process, deterministic game, fake hider) with
  `REV3_NETSIM=delay_ms=80,jitter_ms=30,loss_pct=5,seed=N`: see the commit message for the numbers per seed. Both sides ended in identical states,
  `CHK` lines matched, no `DESYNC` or `ERROR`, hiding was off at the end, and with a fighter that goes to health 0 as a function of the state
  both sides logged the same `GATE close` lines.
- Not yet run: two real Dolphin processes with Rev3 and `REV3_NETSIM` (D does that), the real throttle / mute under a real audio backend.

## Netplay (M5): pacing, MAXR measurement, stall run, log lines

Milestone M5 keeps the two machines in step so that one of them is not pinned at the stall limit all match, measures what a rollback costs,
and makes sure neither side can hang. Motivation: D's loopback with `delay_ms=80,jitter_ms=30,loss_pct=5` (D=2, MAXR=5, RTT about 137 ms, 7788 frames):
host stalls 1902, predicted 3147, rollbacks 0; guest stalls 5, predicted 403, rollbacks 42. Files: `Rev3Session.{h,cpp}` (`TimeSync`, `DecidePace`,
`RollbackSession::Pace()`, resimulation cost, adaptive MAXR, stall run), `Rev3NetLink.{h,cpp}` (RTT fix, `PadLink` offsets, options, HELLO `maxr`),
`Rev3Net.{h,cpp}` (`EnetTransport` answers the session's pacing questions), `Rev3Rollback.cpp` (`DolphinPacer`), `CoreTiming.{h,cpp}`
(`SleepAndResetThrottle`). Design reasoning and the numbers of the test network: rev3-mod `docs/findings.md` "Pacing (M5)".

### Why a machine ends up ahead, and why a plain sleep does not fix it

Dolphin's throttle makes up lost time: a machine that waited for the peer (a stall) runs the next frames without sleeping until it is back on its
schedule (a lag beyond `MAIN_MAX_FALLBACK`, 100 ms, is forgiven). Two such machines settle with the leader at the stall limit (`N - R > MAXR`: it
waits, catches up, waits again) and the follower never predicting. A hidden resimulation used to run with the throttle off and every Throttle call restarted the clock, so
the machine that rolls back often lost the re-run time for good and drifted later; the one that never rolls back stayed ahead (D's first M5 loopback: 120 rollbacks of
~7 ms = ~2 % of the guest's speed, host stalls 1691 of 8650 frames, adv +2.45 / -2.75 for the whole match, pacing at 1 % always at its maximum and never enough).
The rollback itself works; the pair was just not balanced. Two fixes (M5 fix 2, below): the throttle keeps its schedule through a resimulation, and pacing gets more
authority. And a delay must (1) go to the machine that is ahead and (2) survive the throttle's catch-up:
`CoreTimingManager::SleepAndResetThrottle(delay)` sleeps and then restarts the throttle clock at the current cycle (the emulated clock is untouched;
CPU thread only; `DolphinPacer` calls it, and does nothing while the limiter is off). The actuator is a `PaceActuator` interface, so the session stays pure.

### What is measured (`TimeSync`)

Slippi's idea (`SlippiNetplay.cpp` per received pad :295-316, `CalcTimeOffsetUs` :1664-1733): each time a newer remote pad arrives, `sample = (arrival - RTT/2) - our
send time of the SAME frame` = how much later than us the peer sent that frame; positive = this machine is **ahead**. (Slippi uses our newest send time plus
`frame_time * (our newest - their frame)`, which assumes our frames go out 16.7 ms apart; a throttle catch-up burst breaks that, so the exact frame's send time is used
while it is still known (last 64 frames) and Slippi's formula only for a frame we have not sent yet.) Only the first arrival of a newer frame is a sample (a re-sent packet says nothing),
none without an RTT estimate, a window of the last 30 samples is sorted and the **middle third averaged** (jitter spikes and late re-sends fall out at both ends).
The RTT is the ACK time minus the FIRST send of the acked frame (EWMA 7/8); only pads re-sent by the resend timer give no sample (Karn). (Until M5 the sample needed
a pad sent exactly once, which in ARMED, with a packet per frame repeating every unacked pad and an RTT longer than a frame, almost never happens: the RTT stayed 0.)

### The throttle keeps its schedule through a resimulation (M5 fix 2)

`Core::SetIsThrottlerResimDisabled(true/false)` (the hider's switch) now also calls `CoreTimingManager::BeginResim()` / `EndResim()`. While a resimulation runs, `Throttle()`
returns at once (no sleep, no reset); `EndResim()` adds the emulated cycles the re-run frames took to the throttle's reference cycle. The schedule (reference time + cycles / clock) is then
exactly what it would have been without the rollback: the machine that rolled back pays for the re-run out of the next frames (it runs them unthrottled until it is back on its schedule,
like after any stall, bounded by `MAIN_MAX_FALLBACK`) instead of restarting its clock and being later by the cost of every rollback for the rest of the match. No game state is touched;
only host wall time. In the test network a guest that rolls back on 8 % of the frames, 2 or 8 ms per re-run frame, now stays level with **no pacing at all** (stalls under 60 on both
sides together), where the old behaviour pinned the host (over 500 stalls).

Also: after a correction `TimeSync::Reset()` now ignores remote frames up to our newest sent one (their send times on our side are from before the delay), so the window after a correction
holds only samples that describe the new timing.

### What it does (`RollbackSession::Pace()`, `DecidePace`)

ARMED, **normal calls only** (not LOCKSTEP, where the wait for the pad is the pacing; not DRAIN or the call that ends one; not a resimulation: `m_pace_allowed`). A check
is due every `interval_frames` = 30 frames (Slippi `SLIPPI_ONLINE_LOCKSTEP_INTERVAL`), the first one 30 frames after the gate opened, and runs when the window has at least
`min_samples` (15) samples and an RTT exists. It slows only when **ahead**; behind it does nothing (the peer slows). Changes host wall time only: the reply, the pads
and the game state are decided before and untouched. A correction is spread over hook calls in slices of at most `slice_us`, so no frame is stretched by more
than 4 ms; when the last slice is out, the offset window is cleared (the next check needs fresh samples measured with the new timing).

| number | default | unit | meaning (Slippi source) |
|---|---|---|---|
| `interval_frames` | 30 | frames | between two checks (SlippiNetplay.h:35) |
| `hard_frames` | 120 | frames since the gate opened | hard window; later gentle (EXI_DeviceSlippi.cpp:1437) |
| `t1_us` | 10000 | microseconds | hard: ahead by this much or less = nothing (EXI:1426) |
| `hard_target_us` | 5000 | microseconds | hard: leave this much lead after the correction |
| `hard_max_us` | 83415 (5 frames) | microseconds | hard: most delay one check may ask for (EXI:1451) |
| `gentle_deadband_us` | 8000 | microseconds | gentle: ahead by this much or less = nothing (EXI:1521) |
| `gentle_window_frames` | 2 | frames of lead | lead at which the gentle correction reaches its maximum (Slippi 3, EXI:1514; the lead cannot grow past the stall limit, about 3 frames, so 3 never reached the maximum) |
| `gentle_max_permille` | 20 | thousandths of the 30-frame interval | largest gentle slowdown, 2.0 % = 10 ms per check (Slippi 0.5 %; the first M5 build had 1.0 %) |
| `slice_us` | 4000 | microseconds | most one hook call is delayed |
| `min_samples` | 15 | offsets | needed in the window before a check may decide |

Hard: ahead by more than `t1_us` loses `min(offset - hard_target_us, hard_max_us)`. Gentle: ahead by more than the dead band loses
`interval * permille/1000 * min(offset, 2 frames) / 2 frames`. Worst case in the hard window: about 4 checks x 83 ms; afterwards at most 10 ms per 500 ms (2 %).

**Skew at the gate.** In lockstep every call waits for the peer's pad and then runs unthrottled until it is back on its schedule, so neither machine's throttle
clock says anything about the other. When the gate opens (both at the same frame) the two are some frames apart (about the one-way latency plus jitter plus the ARM_PROPOSE /
START_CHECK exchange; the test network starts them 0-8 frames apart). Nothing in the protocol removes it, which is what the hard window is for: the machine that is ahead gives it back
at once (at most 5 frames per check), the one that is behind does nothing.

### Variables

| variable | default | meaning |
|---|---|---|
| `REV3_NET_PACE=0\|1` | 1 | 0: measure and log, never delay (`action=off ... would=hard:35000`), the A/B baseline. Local to a machine, not part of HELLO (pacing changes no game state) |
| `REV3_NET_PACE_T1_US`, `_HARD_MAX_US`, `_SLICE_US`, `_GENTLE_PERMILLE` | 10000, 83415, 4000, 20 | the numbers of the table above (0..100000, 0..300000, 100..16000, 0..100) |
| `REV3_NET_ROLLBACK_MAX` | 5 | MAXR, 1..7 (Slippi's `ROLLBACK_MAX_FRAMES`). **Both machines must use the same value**: the gate close is scheduled MAXR + 1 frames after a final frame, so HELLO now carries `maxr` and a difference refuses the session and names it |
| `REV3_NET_MAXR_ADAPT=1` | 0 | adaptive `MAXR_eff` (below), local |
| `REV3_NET_MAXR_BUDGET_US` | 14000 | microseconds: the resimulation of K frames should cost less than this at its p95 (1000..100000) |

### MAXR measurement and the adaptive rule

Every rollback is timed from the RESTORED reply to the end of its last re-run frame (`resim_n`, `resim_skipped` for a rollback that waited on the network in the middle).
`SUMMARY` carries `resim_ms_p50`, `resim_ms_p95`, `resim_ms_max` (milliseconds per rollback, nearest-rank percentiles over the last 4096). With
`REV3_NET_MAXR_ADAPT=1`: the cost per frame of a rollback is cost / depth (this over-estimates shallow rollbacks, so it errs towards a smaller K); `MAXR_eff` is the
largest K <= MAXR with p95(cost per frame) x K < `REV3_NET_MAXR_BUDGET_US`, at least 1; it needs 8 measured rollbacks, goes **down at once** and **up by one after 16 further measured rollbacks** (and never above the configured MAXR), logs
`MAXR eff 5 -> 3 (p95 resimulation cost ... ms per frame, budget ... ms, configured 5, n rollbacks measured)`, and `SUMMARY` has `maxr_eff`, `maxr_eff_min`, `maxr_changes`. It is
local: only the stall rule (`N - R > MAXR_eff`) and the depth of a postponed rollback use it; the gate close, the slot count and HELLO use the configured MAXR.
Off by default: lowering the stall limit turns rollbacks into stalls, which is only worth it on a machine that cannot re-run MAXR frames in time.

### Disconnect and timeouts

- **Stall run.** Slippi disconnects after 420 frames (7 s) of a player needing inputs it does not have. Here a stall is one blocking wait, so: a single wait of 420 frame times (7000 ms,
  `BlockTimeoutMs`) leaves (`STALL timeout`, `stall: no remote pad for 420 frames`), and **420 consecutive stalled calls** whose waits add up to at least 420 frame times leave too
  (`stall: 420 consecutive stalled frames, 8.4 s of waiting for the peer (the link cannot keep up)`). A link that stalls every call by a millisecond or two is slow but playable and does not
  end the session; any call without a stall resets the run. `SUMMARY` has `stall_run_max`.
- 10 s without any pad from a peer that is still connected (`wait_timeout_ms`) and a lost connection leave as before. The frame-0 barrier times out after 120 s.
- **Closing the window** (while a wait or a frame is running) calls `Abort("the emulator was closed")`: `LEAVE frame=.. reason=..`, `SUMMARY`, and a **BYE** carrying the reason. The peer logs
  `BYE from the peer: the emulator was closed` and `LEAVE ... reason=the peer left (the emulator was closed)`, ends its wait at once and plays on offline. A crash sends no BYE: the
  peer's 10 s no-pad timeout (or ENet's disconnect) ends the wait.

### Log lines

`PACE frame=130 since_gate=100 offset_us=45000 adv=2.70 rtt_ms=137.0 samples=30 action=hard sleep_us=40000` once per check (every 30 armed frames while pacing can decide). `action`: `none` (not
ahead), `hard`, `gentle`, `behind`, `off` (pacing disabled or no actuator: `would=hard:40000` follows). `adv` is the offset in frames of 16.683 ms. Volume: the first 120 checks in full, then
hard corrections and one check in 20 (the count of the rest is `pace_lines_left_out`); a 10-minute match has about 1200 checks, so about 180 lines. `NET pacing on (hard 120 frames, t1 10000 us, ...)` at start.
`SUMMARY` (and the one written at a leave) gained: `pace=on|off pace_checks pace_slowdowns pace_sleep_ms_total adv_mean adv_max adv_min` (frames; positive = ahead; over all checks) `pace_lines_left_out
stall_run_max resim_n resim_skipped resim_ms_p50 resim_ms_p95 resim_ms_max maxr_eff maxr_eff_min maxr_changes`. rev3-mod `phase4.py` reads all of these (`phase4.bat loopback --pace off|on`).

### What D should run, and what success looks like

    .\tools\rollback\phase4.bat loopback --rollback --netsim delay_ms=80,jitter_ms=30,loss_pct=5,seed=1
    .\tools\rollback\phase4.bat loopback --rollback --netsim delay_ms=80,jitter_ms=30,loss_pct=5,seed=1 --pace off     (the baseline, tag ..._nopace)
    optional: --rollback-max 7, --maxr-adapt, a longer match

Success, in `net_<tag>.md`: the **host's stalls a small fraction of 1902** and no LOPSIDED flag, stalls and rollbacks on both sides of the same order, `pace_slowdowns` > 0 on the machine that
was ahead (and few on the other) with `pace_sleep_ms_total` a small part of the match, `adv_mean` near 0 (a few tenths of a frame), the same `CHK` lines on both sides (verdict CLEAN), no `DESYNC`/`ERROR`, and
`resim_ms_p95` under about 14 ms. The `--pace off` run should look like the old one (host pinned), which shows the effect is the pacing.

### Verification

- Unit tests (`Rev3SessionTest.cpp` 103 tests, `Rev3NetLinkTest.cpp` 45, `Rev3NetSimTest.cpp` 14, `Rev3RollbackTest.cpp` 116, all passing): `DecidePace` table, `TimeSync` (same-frame send time, middle third,
  Slippi's fallback), checks every 30 armed frames in slices and a cleared window, hard then gentle, never for a machine that is behind, pacing off, no RTT / too few samples, nothing in LOCKSTEP, 420 stalled calls leave while
  slow short stalls and interrupted runs do not, window close sends BYE and the peer logs the reason, resimulation cost percentiles, adaptive MAXR down / up / bounds, PACE log volume and the SUMMARY keys; PadLink RTT while streaming, timer
  re-sends, offsets through two links; option parsing; HELLO `maxr`.
- Closed-loop test network (`SimMachine` with Dolphin's catch-up throttle, the throttle schedule kept through a resimulation (or, `resim_restarts_clock`, restarted as before the fix), the actual `TimeSync` and RTT estimator): with the host's pad moving, the guest's still and one side starting 5 frames
  ahead (50 ms + 30 ms jitter, 5 % loss, D=2, MAXR=5) the host stalls 1837 / 1869 / 1855 / 1847 times with pacing off (pinned for the whole run) and 168 / 22 / 22 / 22 with it on (mean advantage under 1 frame, the host sleeps 51-74 ms in a 41 s run); a peer whose clock is 1.5 % slower is followed;
  with the old restart behaviour pacing still removes most of the stalls but runs at its maximum (adv_mean stays near 2 frames at 8 ms per re-run frame). Both sides end in the reference state. 1000 seeds of lossy, jittery runs (gate re-opens, busy hardware, relayouts, random throttle and resimulation costs, start skews) still agree with the reference and no
  delay is ever requested outside a normal ARMED call, nor larger than one slice. The 1000 seeds without the throttle model pass as before.
- Not yet run: two real Dolphin processes with pacing (D does that), `SleepAndResetThrottle` under a real audio/video backend. The thresholds are the model's and Slippi's; D's numbers will retune them.

## Netplay (M6): the game skips drawing and sounds in resimulated frames (hook protocol 3)

rev3-mod `docs/resim_skip.md` is the decision record (what is patched in the game and why). The fork side:

- **Protocol.** `ParseCommand` accepts the version byte 2 and 3 (same layout; `Command::protocol` carries it, `PROTOCOL_VERSION_HIDE = 3`). HELLO carries the protocol of the first
  command (`HelloInputs::hook_protocol`), so both machines must run the same patch set: a 2 / 3 mix is `protocol: own=2 peer=3` and the session is refused.
- **Flag word.** With protocol 3 the fork writes a big-endian u32 to `0x8000241C` (inside the hook region, never in a snapshot): `REV3_RESIM_HIDE` flags while a resimulation runs,
  0 otherwise. `ResimHideFlags` (pure, `Rev3Rollback.cpp`) does it; in netplay `DolphinResimHider::SetHidden` calls it next to the v1 throttle-off + mute (kept), so the word is
  set at the RESTORED reply and cleared at the reply of the last resimulated frame, when the session leaves and at shutdown (the destructor). Protocol 2: nothing is written
  (`NOTE resim hiding: the hook speaks protocol 2 ...` once, if hiding was wanted).
- **SyncTest.** `REV3_SYNCTEST_HIDE=1` makes SyncTest set the same flags (`REV3_RESIM_HIDE`) from its RESTORED reply to the reply of its last resimulated frame (cleared when the history
  is reset and in `Finish`), so a SyncTest run tests the patches: the hidden resimulation must reproduce the drawn original run (no `MISMATCH`).
- **Counters.** The SUMMARY lines (SyncTest and netplay) end with ` hide_protocol=3 hide_flags=1 hide_writes=n hidden_iterations=n skipped_sounds=n ... hidden_copies=n hidden_presents=n`:
  the u32 counters at `0x800029E4` (main-loop iterations that skipped drawing) and `0x800029E8` (sound starts skipped). `hidden_iterations` should be about the sum of (N - g) over the
  rollbacks; `skipped_sounds` is 0 unless bit 1 is set. `hidden_copies` / `hidden_presents` (`0x800029F8` / `0x800029FC`, rbhook timing-faithful hide, Oct 17; they read 0 with an older
  rbhook) count the EFB copies and `VISetNextFrameBuffer` calls skipped in hidden iterations: in the default draw mode both are about `hidden_iterations` (every hidden iteration took the
  game's draw arm, so it also waited for the retrace), in `fast` mode both are 0.
- **Draw modes (Oct 17).** FLAGS bits 3-4 pick how a hidden iteration spends emulated time (rev3-mod `docs/resim_skip.md` section 9). Default (`1` / `3`): the game's own end-of-frame and
  present code run as in a drawn frame, only the 3D scene, `GXCopyDisp` and `VISetNextFrameBuffer` are skipped, so `GXDrawDone`, `VIFlush`, `VIWaitForRetrace` (and the 30 fps retrace
  spin) and `G3dReset` stay and a resimulated frame takes the same emulated time as the first run. `9` / `fast`, `11` / `fastall`: the first v3 hide (the game's skip arm: no waits;
  A/B for the soak). `17` / `keep3d`, `19` / `keep3dall`: the 3D scene is drawn too, only the copy and the present are skipped. The fork's own resimulation changes only host time:
  `Core::SetIsThrottlerResimDisabled` makes `Throttle()` return and `EndResim()` moves the throttle reference cycle; no emulated event, VI field or cycle count is touched.

| variable | default | meaning |
|---|---|---|
| `REV3_RESIM_HIDE=0\|1\|2\|3\|9\|11\|17\|19` (or `off\|draw\|sound\|all\|fast\|fastall\|keep3d\|keep3dall`) | 1 (draw) | the flags written while a resimulation runs: bit 0 no EFB copy / presenting (and no 3D scene unless `keep3d`), bit 1 the sound start funnel returns "started" without starting, bits 3-4 the draw mode (8 = fast hide, 16 = keep the 3D scene; see "Draw modes"). 0 = the v1 hiding only. A bad value is an `ERROR` line and `draw`. Needs a hook built with `rbhook --protocol 3`. Local to a machine (not in HELLO; the protocol is) |
| `REV3_SYNCTEST_HIDE=1` | 0 | SyncTest only: set the flags during its resimulations |

What D runs: regenerate the INI with `--protocol 3` (rev3-mod `docs/resim_skip.md` section 7), then SyncTest K = 1..7 with `REV3_SYNCTEST_HIDE=1`, then loopback netplay with the netsim,
then `REV3_RESIM_HIDE=all` for the listening test.
Tests: `ProtocolsTwoAndThree*`, `ResimHideEnvValuesParse`, `TheFlagWordIs*`, `EachFlagValue*`, `TheDrawModeBits*`, `ProtocolTwoNeverTouchesTheFlagWord*`, `TheCountersAreRead*`, `ResimHidingDrivesTheFlagWord*`,
`SyncTestSetsTheFlagsOnlyWhileItResimulates`, `SyncTestWithHooksOfProtocolTwo*`, `TheHideCountersGoIntoTheNetSummaryNote`, `Rev3NetLink.HelloCarriesTheHookProtocol*`.
Not done: the optional refinement of resim_skip.md 5.4 (clear bit 0 one call earlier so the corrected frame N - 1 is drawn).

## Bot soak (`REV3_PAD_BOT`, `REV3_NET_EXIT`, unattended matches)

Lets two Dolphins play a whole match against each other with nobody at the keyboard and quit by themselves, so a soak (many matches, with
`REV3_NETSIM`) can run overnight. The driver is `tools/rollback/soak.py` in rev3-mod; this section is the fork side. Code: `Rev3Bot.h/.cpp` (pure,
unit tested in `Rev3BotTest.cpp`), glue in `Rev3Rollback.cpp` (`DeterminismKit::OnCommand`, `InitializeBot`, `ObserveExit`, `ExitWatcher`).

| Variable | Default | Meaning |
|---|---|---|
| `REV3_PAD_BOT=<seed>` | off | the local pad (command port 0) is a generated, person-like stream once the fight has started. `<seed>` is a number (u64); the same seed on the same side gives the same pad stream, the host (game port 0) and the guest (port 1) get different streams from the same seed. A bad value is an `ERROR` in the Rev3 log and the bot stays off |
| `REV3_NET_EXIT=match\|frames:N\|rounds:N` | unset = never | when to quit: `match` = after the match result (below), `frames:N` = N game frames after the fight was found, `rounds:N` = after N KOs (a round ends with a KO) |
| `REV3_NET_EXIT_WAIT_S=<s>` | 5 | seconds the exit waits after the decision, so both sides see the same last frames and the result screen loads (s) |
| `REV3_NET_FREEZE_EXIT_S=<s>` | 0 = off | the game thread has not entered the hook for this long after it ran before (s; needs the freeze dump on) -> the process writes `LEAVE ... reason=freeze` / `EXIT freeze` to the Rev3 log and ends with exit code 70 (`std::_Exit`, so the freeze dump of `REV3_SYNCTEST_DUMP` has been written first) |
| `REV3_NET_MAX_S=<s>` | 0 = off | wall clock cap for the whole process (s) -> exit code 71 |

| `REV3_PAD_MIRROR=1` | off | GUEST ONLY (the host logs `NOTE REV3_PAD_MIRROR is for the guest only: ignored on the host` and plays its own pad). The guest's local pad is the newest confirmed remote (host) pad (`Predict()`: the host's pad of frame `max(R, D-1)`, neutral before the first one) instead of its own controller; the guest's own controller is not read for the game (the diagnostics below still count its err bytes). It is sent to the host like any local pad, so both machines agree on what the game plays and a `REV3_PAD_RECORD` of the run holds the mirrored pad (the record keeps the reply pads, so a replay is exact although mirroring depends on timing). One person driving the host window moves both players' cursors; used by `soak.py record-menu` (default; `--no-mirror` leaves it off). Not part of HELLO (local to a machine); `REV3_PAD_SCRIPT` / `REV3_PAD_BOT` on the guest are overridden by it. Log: `NOTE pad mirror on (REV3_PAD_MIRROR=1): ...` |

Local pad diagnostics (every netplay run; D's first record-menu, Oct 16: the guest's pad was neutral on all 7972 frames and the logs could not say why). Per side, per fresh hook call, from the raw local port (command port 0): the err byte (+10) goes into `err_hist=[e0,e-1,e-2,e-3,other]` (0 = data, -1 PAD_ERR_NO_CONTROLLER, -2 NOT_READY, -3 TRANSFER, anything else) and a non-neutral stored local pad (buttons, sticks or triggers; with a mirror: the mirrored pad) counts in `local_active`; `local_mirrored` counts the calls that used the mirror. SUMMARY gets ` local_active=N local_mirrored=N err_hist=[..]` (before the `netsim_*` / `pace=` fields). Log lines: `LOCAL first input frame=<hook frame> pad=<16 hex> err=<n>[ (mirrored from the host)]` once, at the first non-neutral local pad; `NOTE local pad neutral for 10 s (err=<n>; err_hist=[..] ...; local_active=N)` at most once per 10 s of the machine's clock while the pad has been neutral that long AND no fight is on (fighters not found = menus), with an explanation on the first one: err 0 and nothing pressed = the controller is connected but the render window has no keyboard focus (Dolphin's input gate: `Core::UpdateInputGate` with `BackgroundInput = False` needs `Host_RendererHasFocus()`, and a window with `RenderToMain = False` is only focused by clicking its picture) or the pad is unmapped; err -1 = no controller on SI port 0 (`SIDevice0` is not a Standard Controller: the fork's default is 12, the Wii U adapter device, which reads a real adapter only).

Takeover (Rev3: scene type 0x803C11D4 = 2 is a match, rev3_menu.py `SCENE_TYPE`; fighters are found through the TEAMS chain, health at
fighter+0x28, rev3_menu.py / fighter walk in `ReadGameObs`): before the first frame at which both fighters are found the bot sends a menu pad
(a short A or Start pulse about every 48 frames, jittered, so character and stage select run to the end by themselves). A recorded menu script
(`REV3_PAD_SCRIPT`, Option A) is the one that really picks the characters and the stage: it is what `soak.py record-menu` records and what each
soak run plays on both sides, so the characters and the stage are fixed and the fight is reached the same way every run. When the fight is found
the bot takes the pad over for good, even if the script has frames left. The take system (`tools/takes`) could not be reused to reach a versus
fight: its navigator is picture and probe driven and targets Training, and the select-screen cursor has no known memory address.

Match over (`ExitController`, numbers in `ExitTuning`, all in hook calls = frames): `rounds:N` and `match` count KOs (a health of zero for 30
calls in a row, a one call zero while loading is ignored). `match` ends 600 calls after the fighters were lost, 60 calls after the scene type
left 2 once at least 2 rounds were played, or 1800 calls after the last KO with nothing else seen. The exact Rev3 result-scene values are not
known, so these are deliberately redundant. The first rule that fires logs `EXIT planned frame=.. reason="..." ko=..`.

Quitting: `Host_Message(WMUserStop)` (the same path as closing the window; Dolphin must run with `-b`, otherwise the Qt main window asks for a
confirmation or stays open). `Rev3::Shutdown` then writes `BYE` (reason `planned exit: ...`, so the peer logs `LEAVE reason=the peer left (planned
exit: ...)`) and `SUMMARY`. If the stop has not ended the process after 20 s (`STOP_GRACE_MS`) it ends with exit code 72.

**An unplanned LEAVE ends the run too (Oct 17).** With `REV3_NET_EXIT` set, a net session that leaves for any reason that is not a planned exit (a wait
timeout, a desync, a HELLO mismatch, a lost connection, `the peer left (<anything but planned exit / the emulator was closed>)`; `Bot::IsPlannedLeaveReason`)
makes the exit controller decide at once: the net log gets `EXIT unplanned leave reason=".." frame=.. ko=.. stop_in_calls=.. (exit code 73)`,
`REV3_NET_EXIT_WAIT_S` seconds later (counted in hook calls, the game keeps running offline meanwhile) the usual `EXIT stop requested` and the stop, and
`Rev3::Shutdown` ends the process with `std::_Exit(73)` after the logs are closed. Before this a side that had left carried on offline until the runner's
cap (D's run_2: killed 60 s after the other side exited, verdict TIMEOUT although the LEAVE was the cause). A leave after a planned decision changes
nothing (that stop is already under way). With `REV3_NET_EXIT` unset the game still goes on offline after a leave, as before. Code 73 skips the rest of
Dolphin's own shutdown, which a soak does not need.

Exit codes: 0 = clean stop, 70 = freeze, 71 = `REV3_NET_MAX_S`, 72 = stop did not finish, 73 = the net session left unplanned (the `LEAVE` and `EXIT unplanned leave` lines
have the reason). Anything else (and a crash) is the runner's CRASH; rev3-mod `soak.py` reports 73 as PROBLEM with the LEAVE reason.

Log lines (Rev3 log; the net log gets the `EXIT stop requested` line): `NOTE bot on (REV3_PAD_BOT=..)`, `NOTE automatic exit ..`, `NOTE exit watcher ..`, `BOT fight found frame=..`, `BOT takeover frame=.. why=.. seed=.. port=..`, `BOT mode=fight|menu frame=..`, `EXIT watching ..`, `EXIT ko=<n> frame=..`, `EXIT planned reason=".." frame=.. ko=..`, `EXIT unplanned leave reason=".." frame=.. ko=.. ..`, `EXIT stop requested ..`, `EXIT freeze ..`, `EXIT code=70 (freeze)`, `EXIT max_s ..`, `EXIT stop did not end the process ..`, `ERROR REV3_PAD_BOT ..` / `ERROR REV3_NET_EXIT ..`.

Tests: `Rev3Bot.*` (32; `AnUnplannedLeave*`, `ALeaveAfterAPlannedDecision*`, `OnlyThePeersPlannedExit*` for code 73), `Rev3Rollback.*Bot*` / `ReadGameObs*`; `Rev3Session.Mirror*` / `LocalPadDiagnostics*` / `ANeutralLocalPad*` and `Rev3NetLink.PadMirror*` (the mirror, the counters, the 10 s note, the option).

## Known gaps (after M6 fork side)

- Pacing and MAXR measurement are built (M5) but only run in the test network; the real numbers are D's loopback (above). Not done: a pacing OSD line (the log has it), and the adaptive MAXR is off by default.
- Hiding in the game (M6) needs the INI regenerated with protocol 3 and is untested in the real game; with a protocol 2 hook only the v1 hiding (throttle off + mute) runs.
- Internet play (NAT traversal beyond simultaneous open, relay, matchmaking) is M7; direct connect only.
- A HOLD makes a machine wait for the peer's pads more often during a disc read; there is no measurement of how often yet.
- No spectators, no replay file; rollback depth is capped at 7; the first command must be frame 0 (a game already in progress cannot join); a
  window closed while the peer waits ends the peer's wait through the BYE (or the wait timeout), not instantly if the machine crashes.
