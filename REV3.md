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
| `REV3_SYNCTEST_TB=1` | also store the emulated time base in every snapshot and set it back on restore (default 0, see "Time base" below) |
| `REV3_SYNCTEST_EXCLUDE=path` | optional JSON `{"ranges":[{"start","end"}]}` (same format as the whitelist) subtracted from the snapshot after the whitelist is loaded; each range is logged as `NOTE exclude user 0x..-0x.. (n bytes were in the whitelist)`. Use it to test excluding audio / scratch ranges without editing the whitelist. A bad file or one that removes everything turns SyncTest off with an error |
| `REV3_SYNCTEST_LOG=path` | log file (default `<User>\Logs\rev3_synctest.log`), appended, flushed per line |

Hardware regions (computed from Dolphin's emulated hardware when SyncTest arms and re-checked every 600 hook calls; the set
only grows, so double buffers end up fully excluded; a new region that overlaps the whitelist clears the history) are subtracted
from every snapshot range, because they are read or written behind the game's back and must not be rewound: the GX command
processor FIFO (`GetCommandProcessor().GetFifo()` CPBase..CPEnd), the VI XFBs (`GetVideoInterface().GetXFBAddressTop/Bottom()`,
640*576*2 bytes each, the VI size is not exposed), the audio DMA source block (DSP registers 0x0C005030/32/36 read through the MMIO
mapping, NumBlocks*32 bytes; the DSPManager's state is private), and a DVD DMA in flight (DI registers DIMAR/DILENGTH/DICR,
0x0C006014.. and on a Wii 0x0D006014.., while TSTART is set). Not reachable: Wii IOS `/dev/di` read destinations (inside IOS HLE),
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
the hardware cut-outs and the JIT invalidation off to see which one matters.

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
Risk: OSAlarm / OSThread-sleep deadlines are absolute time base values and CoreTiming events are absolute tick counts, so after the
time base moves back an alarm that was due fires later by the rolled-back amount (K frames); acceptable for a test, not for netplay.

A clean run: a fight of a few thousand frames ends with `mismatching_frames=0`, `sp_mismatches=0`, no `MISMATCH` or `ERROR`
lines, and a restore time of a few hundred microseconds. The state machine is separate from Dolphin (`Rev3::SyncTest` talks to a
`GuestMemory` / `Sink` interface); `Source/UnitTests/Core/Rev3RollbackTest.cpp` drives it with a fake game (target `tests`,
filter `Rev3*`).
