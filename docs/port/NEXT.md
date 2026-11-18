# Handoff: where HellStation stands and what's next

Written 2026-11-18 for whoever picks the work up next. Read [AGENTS.md](../../AGENTS.md) first (the
contract), then this. The full history is in [PORTING_LOG.md](../../PORTING_LOG.md); the task list
is the [issue tracker](https://github.com/Meanski/HellStation/issues).

## State at hand-off

- `main` at `0b267704`, pushed to `origin` (github.com/Meanski/HellStation). History is HellStation's
  own: one commit imports Redivivus at `dd289669c82a` (see `UPSTREAM`), then 20 of ours.
- **PS2 (Phase 2):** LoD 1.14d boots in PCSX2, reaches the Rogue Encampment, walks, opens the
  inventory: steady 25 FPS (40 ms frames), units move correctly. It needs PCSX2's Extended RAM.
  There's no sound, and the pad can't type a character name.
- **Linux (Phase 1):** town smoke re-verified 2026-11-18, before and after the #12 cleanup.
- **Stock Windows build:** not rebuilt this round; nothing it compiles was touched.

## How to run it

```sh
tools/port/ps2.sh cmake --build build-ps2                       # configured with -DHS_PS2_EXTENDED_RAM=ON
tools/port/run-pcsx2.sh --extra-ram --seconds 3 \
  --script tools/port/scripts/town-walk.txt                     # scripted: new Barbarian, walk, inventory
```

- Logs: `build-ps2/run/hs.log` (written over `host:`), and a RAM ring read over PINE into
  `build-ps2/run/ring.log` when the runner's sleep ends. A `frame N` pacing line appears every 250
  presented frames. Scripts are keyed to presented frames: town loads by about frame 2550; menus are
  at 25 FPS.
- `run-pcsx2.sh` kills any running PCSX2 first, so the owner's open session goes with it. It
  rewrites `~/Library/Application Support/PCSX2/inis/PCSX2.ini` keys (HostFs, PINE, ExtraMemory,
  logging). The BIOS is chosen by the owner in PCSX2's settings; never install or pick one.
- Game data is `$D2DATA` (default `~/d2data`: the 11 MPQs listed in the README), symlinked into the
  run folder, never copied. Saves land in `build-ps2/run/hsroot`.
- `tools/port/winshot.sh PCSX2 out.png` and `tools/port/winrec.sh PCSX2 out.mov <secs>` capture
  the PCSX2 window only. Captures stay out of the repo (game art).

## Gotchas that cost hours (don't relearn them)

1. **PCSX2's EE recompiler loses the upper half of a non-aligned `LWR`.** Every EE object goes
   through `tools/port/ee-cc-launcher.sh` -> `ee-lwr-fixup.py` (adds `sll rt,rt,0`). Symptom
   without it: negative values from packed structs compare as huge positives, e.g. units teleporting
   along walks. `-DHS_EE_LWR_FIXUP=OFF` reproduces the bug. The `CPU check:` line in `hs.log` says
   whether the running CPU has it.
2. **PCSX2 doesn't fault on misaligned word loads; it reads wrong data.** A real PS2 raises an
   address error. Treat any "works on Linux, garbage on PCSX2" as alignment first.
3. **The EE build is `-O2` with no inlining or IPA cloning** (`cmake/ps2-ee.cmake`). The overrides
   weaken and patch functions by symbol (`cmake/port.cmake`), so plain `-O2` breaks the link.
4. **The EE kernel has only 256 semaphores, and no time slicing.** Locks are semaphore-free
   (`backend/posix/threads.cpp`), and every wait or spin must yield.
5. **Single player runs the game server inline on the main thread** (lockstep with the client).
   Timing bugs show as frame pacing; game-logic bugs show identically on server and client.
6. **Docker bind mounts sometimes serve a stale file** right after an edit, giving a CMake "Parse
   error" or an old object. Rerun, or wait a few seconds.
7. **Generated code (`Source/`, `3rdparty/Storm`, `3rdparty/crashy`) is never edited.** The
   pre-commit hook enforces it; route around it with `Redivivus/port/` (prelude, shim, overrides,
   wraps). Generator defects are drafted in `docs/port/upstream/` for the owner to file.

## Suggested order of work

1. ~~**#12, cleanup**~~: done 2026-11-18 (see PORTING_LOG); the issue is ready to close.
2. **#5, measure memory before cutting anything:** peak heap and Storm/Fog pool stats at the menu,
   in town, in Act 5 and in a busy fight, logged in PORTING_LOG. About 20 MB of pool reservations
   plus a 10 MB ELF were seen at the menu. That measurement decides LoD vs classic.
3. **#2, name entry with the pad.** It unblocks creating characters without a script.
4. **#1, sound** (fake DirectSound on audsrv).
5. **#3, controller layer 1** (design in `docs/port/controller.md`).
6. Then the Retail PS2 milestone: #6 USB data, #7 saves, #9 TV modes, #8 hardware, #4 panels.

#10 (PCSX2 bug) and #11 (two Redivivus generator defects) are reports **the owner files**. Drafts
are in `docs/port/upstream/`. Never file or comment upstream yourself.

## Owner preferences

- Commit only when asked; push only when asked. Never force-push `main` (it was rewritten once,
  with explicit consent, to restart history).
- The commit email is the GitHub no-reply address (set in this repo's git config). Never put the
  owner's personal email into a commit.
- The repo is HellStation's own. Don't re-add Redivivus's scaffolding (CI, templates, its README):
  `UPSTREAM` lists what's excluded, and `tools/sync-redivivus.sh` brings in upstream code changes.
- The owner cares about the end result being visible: footage, a clean README, a working demo.
  Prioritise playable progress over infrastructure polish, and report in plain language with
  numbers.

## Local-only state (not in git)

- Footage is in `~/Movies/HellStation/` (`before-vs-after.mp4`). To film the teleport bug again,
  configure a separate build dir with `-DHS_EE_LWR_FIXUP=OFF`.
- Build logs (`build*.log`) are ignored.
