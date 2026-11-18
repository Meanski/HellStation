# Handoff: where HellStation stands and what's next

Written 2026-11-18 for whoever picks the work up next. Read [AGENTS.md](../../AGENTS.md) first (the
contract), then this. The full history is in [PORTING_LOG.md](../../PORTING_LOG.md); the task list
is the [issue tracker](https://github.com/Meanski/HellStation/issues).

## Status: fits in 32 MB (2026-11-19)

The memory check (#5) nearly ended the project on 2026-11-18. A day later, with the heap stopped at
a retail 32 MB and PCSX2's Extended RAM off, character creation peaks at 22.7 MB, the Rogue
Encampment at 23.3 MB and Harrogath (Act 5, Hell) at 23.4 MB, against 24.2 MB available. All are
at full speed in 640x480. The owner's goal is a disc in a stock PS2. The margin is under 1 MB, so
every new feature has to be measured at the retail cap.

Next, roughly in order:
- **The stairs into Harrogath's lower town are a 50-80 second slideshow at the retail cap** (2-5
  FPS; the lower town itself then runs at 25). The 2 MB sprite cache holds less than the
  animations in view, so DCC decoding (D2CMP `Codec.cpp`) runs every frame. Profile it with
  `-DHS_PS2_PROFILE=ON`; compare with `HS_GAME_CACHES_UNCAPPED` (no stall). PORTING_LOG's last
  2026-11-19 entry has the numbers.
- **Fights and leaving town**, at the retail cap: the first real test of the margin. Watch the
  `mem` lines and the first-failure dump (it names the caller).
- **Sound** in the SPU2's own 2 MB (the game's sound cache would be 3-5 MB in main RAM).
- **A disc**: an ISO with the ELF and the user's own data (an archive format with a compact index
  would replace the MPQs and the patched lookup), and loading over CDVD instead of `host:`.
- **Real hardware**: unaligned loads fault there, where PCSX2 reads wrong data quietly.

## State

- `main` builds for the PS2 (retail heap by default; `-DHS_PS2_EXTENDED_RAM=ON` for headroom) and
  for i686 Linux, and the Linux town smoke test passes.
- **PS2:** boots, makes a character, walks the Rogue Encampment, reaches Act 5, at 25 FPS, in a
  retail memory map. No sound; the pad can't type a name. The memory report (`mem` lines in
  `hs.log`, every 250 frames, and a full dump at the first failed allocation) is the tool to start
  from.
- **Memory policies for small-RAM targets** are in `Redivivus/port/overrides/` and listed in
  `docs/port/overrides.md`: exact-size Fog and Storm allocations, compact MPQ indexes, the sprite
  cache cap, per-class character-creation art, 640x480 in game.

## How to run it

```sh
tools/port/ps2.sh cmake --build build-ps2                       # retail heap unless -DHS_PS2_EXTENDED_RAM=ON
tools/port/run-pcsx2.sh --seconds 3 \
  --script tools/port/scripts/town-walk.txt                     # scripted: new Barbarian, walk, inventory
```

- Logs: `build-ps2/run/hs.log` (written over `host:`), and a RAM ring read over PINE into
  `build-ps2/run/ring.log` when the runner's sleep ends. A `frame N` pacing line appears every 250
  presented frames. Scripts are keyed to presented frames: town loads by about frame 2550; menus are
  at 25 FPS.
- `run-pcsx2.sh` closes any running PCSX2 first (and waits for it), so the owner's open session
  goes with it. It rewrites `~/Library/Application Support/PCSX2/inis/PCSX2.ini` keys (HostFs, PINE, ExtraMemory,
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
