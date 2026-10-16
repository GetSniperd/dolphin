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
means the whitelist is incomplete or the hook point is not equivalent. It only runs once armed: both fighters found for
`REV3_SYNCTEST_ARM` consecutive hook calls (default 600 = 10 s, so the stage has finished loading), or toggled by P1's Z
button; never in menus or loading.

Run (set the variables before starting Slippi_Dolphin.exe; the patched game with the v2 hook must be what boots):

    set REV3_SYNCTEST=2
    set REV3_WHITELIST=D:\Project_A_ExtraData\Research\rev3mod\whitelist.json
    Slippi_Dolphin.exe -e <Rev3 iso or Riivolution-patched boot>

| variable | meaning |
|---|---|
| `REV3_SYNCTEST=K` | 1..8 frames to roll back; absent or 0 = off |
| `REV3_WHITELIST=path` | whitelist.json `{"ranges":[{"start":"0x80...","end":"0x80..."}]}`, hex strings or numbers, end exclusive; required |
| (automatic) | the hook's own region 0x80002400-0x80002A00 is cut out of every whitelist range (logged as a NOTE); it must survive a restore |
| `REV3_SYNCTEST_EVERY=N` | roll back when `frame % N == 0` (default 1 = every frame) |
| `REV3_STACK_WINDOW=bytes` | bytes saved from the hook's sp upward (default 0x4000) |
| `REV3_SYNCTEST_ARM=N` or `pad` | N = arm after N consecutive hook calls with both fighters found (default 600; fighters exist while a stage is still loading, 60 was too short and froze the first real run). `pad` = arm when P1 presses Z (PADStatus button 0x0010, edge triggered) while fighters are present, disarm on the next press |
| `REV3_SYNCTEST_NOOP=1` | diagnosis: once armed, every frame capture S[n] and immediately restore S[n] (same frame), reply RESTORED with frame n and inputs n. Memory is unchanged in effect, but the game hook's RESTORED path (register reload from its stack frame) runs every frame. No comparisons. If the game runs fine in this mode the hook is cleared and the freeze comes from the memory rewind itself |
| `REV3_SYNCTEST_HW_EXCLUDE=0` | do not cut hardware-DMA regions out of the snapshot (default on, see below) |
| `REV3_SYNCTEST_ICACHE=0` | do not invalidate the JIT for restored ranges (default on) |
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

Log lines: `SYNCTEST start K=.. EVERY=.. window=.. whitelist=.. ranges=.. bytes=.. ... arm=N|pad noop=0|1 hw_exclude=0|1 icache=0|1`
once; `NOTE armed at frame N (M hook calls with both fighters)` / `NOTE armed by Z at frame N` / `NOTE disarmed by Z at frame N`;
`NOTE exclude <what> 0xSTART-0xEND (n bytes were in the whitelist)` with `<what>` = gx_fifo, xfb_top, xfb_bottom, audio_dma,
dvd_dma, dvd_dma_wii, followed by `NOTE snapshot now ranges=.. bytes=.. snapshot_bytes=..` when the snapshot shrank;
`NOTE hardware: ...` (which accessors are used and which are not reachable); `NOTE history cleared: <why>`;
`RESTORE n -> m begin` then `RESTORE n -> m bytes=.. us=.. icache_us=..` for the first 10 restores (a log that ends right after a
`begin` line, or right after a RESTORE line, shows where the game hung: inside the restore, or after it); `HB frame=.. calls=..
rollbacks=.. noop_restores=..` every 300 hook calls while armed (flushed; the last HB bounds when the game froze); `ERROR ...` (sp differs from the snapshot's: that frame is not rolled back; restore not honoured by the game);
`MISMATCH frame=N K=.. rollback=R fields=a ranges=b` followed by `  pP field ref=0x.. resim=0x..` and
`  range 0xSTART-0xEND crc ref=.. resim=.. words=n first(addr:ref>resim)= ...` (full detail for the first 20 mismatching frames,
then counts only; an on-screen message appears on the first mismatch); `SUMMARY frame=.. rollbacks=.. frames_compared=..
mismatching_frames=.. restore_us_mean=.. restore_us_max=.. icache_us_mean=.. icache_us_max=.. sp_mismatches=..
noop_restores=..` every 600 compared frames (every 600 restores in noop mode) and `SUMMARY final` on exit. `restore_us` is the
time to write the ranges back, `icache_us` the JIT invalidation.

Narrowing a freeze, cheapest first: `REV3_SYNCTEST_ARM=pad` (arm by hand once the fight is running), `REV3_SYNCTEST_NOOP=1` (only
the hook's register reload, memory unchanged), then a real run; `REV3_SYNCTEST_HW_EXCLUDE=0` and `REV3_SYNCTEST_ICACHE=0` switch
the hardware cut-outs and the JIT invalidation off to see which one matters.

A clean run: a fight of a few thousand frames ends with `mismatching_frames=0`, `sp_mismatches=0`, no `MISMATCH` or `ERROR`
lines, and a restore time of a few hundred microseconds. The state machine is separate from Dolphin (`Rev3::SyncTest` talks to a
`GuestMemory` / `Sink` interface); `Source/UnitTests/Core/Rev3RollbackTest.cpp` drives it with a fake game (target `tests`,
filter `Rev3*`).
