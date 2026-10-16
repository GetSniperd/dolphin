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
while armed `NOTE learned +n pages (now ranges=.. bytes=..) frame N, history cleared` followed by `NOTE snapshot now ...` (the first 10
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
| `REV3_NET_PEER=<ip>:<port>` | required | the other machine (IPv4 or host name, port 1..65535). Only this IP is accepted; a connection from anywhere else is refused (`NET refused a connection from ...`) |
| `REV3_NET_PORT=n` | 2626 | UDP port this emulator listens on and sends from. Both sides connect to each other at the same moment from these fixed ports (hole punching style), so use one pair like host 2626 / guest 2627 and open or forward them; two connections result, both are used, duplicates are dropped by sequence number |
| `REV3_NET_DELAY=n` | 2 | input delay D in frames, 0..15. The host's value wins (a difference is logged, not a mismatch) |
| `REV3_NET_ROLLBACK=0\|1` | 0 | 1 lets the arm gate open (prediction, rollback). 0 = pure lockstep. Part of HELLO: both sides must agree |
| `REV3_NET_ROLLBACK_MAX=n` | 5 | MAXR, the most frames ever predicted = deepest rollback, 1..7. Snapshot slots = MAXR + 2 |
| `REV3_NET_ARM=n` | 600 | hook calls in a row with both fighters found before the gate may open (SyncTest's number, 10 s) |
| `REV3_NET_QUIET=n` | 60 | hook calls in a row without a disc read in flight before the gate may open |
| `REV3_NET_TIMEOUT_S=n` | 120 | connect + HELLO (the frame-0 barrier). D starts the two emulators by hand |
| `REV3_NET_WAIT_MS=n` | 10000 | no remote pad this long in lockstep = the peer is gone, play on offline |
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
| HELLO `0x90`, START_CHECK `0x91`, DESYNC_DUMP `0x92`, BYE `0x93`, RANGE_CRC `0x94` (M4, testing) | 0, reliable | `u8 id, u32 sequence, payload` (payloads: `Rev3Session.h`) |

The receiver stores every pad of a packet by its own frame and acks R, the highest frame with all pads up to it present, so a lost packet
never makes an ack lie; the first 64 or more remote pads are contiguous from frame 0. A sender re-sends its unacked pads after about
one round trip (at least 15 ms) without an ack, from the same wait loop that blocks the game (Slippi only re-sends on the next queued
pad). Every PAD packet carries the checksum of the latest final frame (every 30th frame).

### HELLO (both machines must match, else the session is refused with an OSD line and a log entry, and the game plays offline)

Game id (`RNEEDA`), hook protocol (2), fields version and digest (0xD4076EF2), fork build (`Common::GetScmRevGitStr`), CPU core, CPU thread
(single / dual), SyncGPU, DSP engine (HLE / LLE / LLE thread), custom RTC (enabled + value), overclock, MMU, FastDiscSpeed, cheats,
Bluetooth passthrough, Wii Remote sources 1..4 + balance board, the CRC-32 of the hook code region 0x800024A0-0x8000291F (read when the
first command arrives; the data block 0x80002400-0x8000249F changes every frame and is not included), `REV3_NET_ROLLBACK`, the gate
numbers, and the `REV3_NET_STABLE` file CRC. Two equal roles are a mismatch. The input delay is the host's.

### The arm gate (only with `REV3_NET_ROLLBACK=1`)

Opens after: both fighters found for `REV3_NET_ARM` normal calls in a row, no disc read in flight for `REV3_NET_QUIET` calls, nobody at
health 0 (fighter + 0x28), the adaptive snapshot set learned, the session in lockstep. Then the snapshot set is installed and both
machines exchange START_CHECK (the exact state at the same frame) before prediction starts. How it closes again (fighters lost, a
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
| `REV3_NET_BUSY=n` | 3 | consecutive armed calls with a disc read in flight before a HOLD starts (below); 0 = never hold. 0..1000. Local to a machine, not part of HELLO |
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
Not in v1: skipping the render / SFX of re-run frames inside the game (M6), so a resimulated frame still draws.

### The gate rule (both machines always agree on WHEN the shared mode changes)

The shared modes are LOCKSTEP and ARMED. In M3 the gate was evaluated on the live state every call, which a misprediction can change: one machine
could see a fighter at health 0 in a timeline that never happened and close while the other did not. Now:

- **Open** is decided in LOCKSTEP, where every frame is final. It takes effect at the frame where both START_CHECKs name the same frame and
  carry the same state; a side whose local conditions differ simply sends none and both stay in lockstep.
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

### Busy hardware (disc read / DVD DMA)

A rollback while a disc read is in flight can't rewind the read, so in M3 it was postponed until it would exceed MAXR and then **forced**
(`forced=hardware_busy` on the ROLLBACK line). M4 stops that from happening in normal play without changing the shared mode: after
`REV3_NET_BUSY` (3) armed calls in a row with a disc read in flight the machine starts a **HOLD**: it predicts nothing while the read lasts
(each call waits for the real remote pad of its own frame, `GATE hold frame=..`), so no rollback can become due while the hardware is busy. The
HOLD ends after 2 calls without a read (`GATE hold end frame=.. held_calls=..`). A misprediction made before the hold is still postponed
while the depth allows, as before. The gate itself stays open for the peer: a HOLD is invisible on the wire, it is only a machine that
waits for pads more often. `SUMMARY` counts `hw_holds`, `hold_calls`, `postponed`, `forced_hw`; a `forced_hw` that is not 0 in normal play is a bug report. This
is the one place where the brief's "close the gate on sustained busy hardware" is done as "hold" instead: a close must be the same on both
machines, and the peer cannot see our disc.

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

## Known gaps (after M4)

- Pacing (CalcTimeOffsetUs port: keeping the two clocks in step so the faster machine does not run into the stall rule) and MAXR measurement are M5.
- The in-game side of hiding (skip render and SFX of re-run frames, the rbhook patch) is M6; v1 only turns the throttle off and mutes the output.
- Internet play (NAT traversal beyond simultaneous open, relay, matchmaking) is M7; direct connect only.
- A HOLD makes a machine wait for the peer's pads more often during a disc read; there is no measurement of how often yet.
- No spectators, no replay file; rollback depth is capped at 7; the first command must be frame 0 (a game already in progress cannot join); a
  window closed while the peer waits ends the peer's wait through the BYE (or the wait timeout), not instantly if the machine crashes.
