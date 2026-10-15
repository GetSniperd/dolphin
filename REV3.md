# Rev3 rollback fork (branch `rev3`)

Slippi Mainline Dolphin with rollback netcode for Naruto Clash of Ninja Revolution 3 (Wii, game ID RNE). The plan, the Rev3
addresses and the test tools live in GetSniperd/rev3-mod: `docs/rollback_fork_plan.md` (the change list), `rev3mod/rbhook.py`
(the game-side hook this emulator answers over EXI), `rev3mod/rbchecksum.py` (the desync checksum), `tools/rollback/` (probes).

- No game data in this repo: code, settings and documented addresses only.
- Build: `.github/workflows/rev3-windows.yml` runs on every push to `rev3` / `rev3-*` (and by hand from the Actions tab); the
  run's `rev3-dolphin-windows` artifact is a portable zip with Slippi_Dolphin.exe. Slippi's own multi-platform `main.yml` is
  removed on this branch to keep runs short; `slippi` keeps it.
- `Data/Sys/GameSettings/RNE.ini`: lets mainline boot Rev3 (BootManager.cpp:69-76 needs a default game INI).
