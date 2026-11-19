# Handoff: where HellStation stands and what's next

Written 2026-11-18, updated 2026-11-19, for whoever picks the work up next. Read [AGENTS.md](../../AGENTS.md) first (the
contract), then this. The full history is in [PORTING_LOG.md](../../PORTING_LOG.md); the task list
is the [issue tracker](https://github.com/Meanski/HellStation/issues).

## Status: fits in 32 MB; fights survive but big packs are slow (end of 2026-11-19)

With the heap stopped at a retail 32 MB (PCSX2's Extended RAM off), the game boots, loads saves,
walks every town tested and fights in Act 1's Black Marsh without running out of memory. The
owner's goal is a disc in a stock PS2. Every change is measured at the retail cap.

What changed on 2026-11-19 (details and numbers in PORTING_LOG, each entry starts "In short:"):
- One TLSF-style heap for malloc and the VirtualAlloc regions (`backend/ps2/memory.cpp`, replaces
  newlib's whole allocator; changes the pool with interrupts off). Fragmentation at failure went
  from 1.25 to 0.84 MB.
- 485 KiB of program image back: D2Direct3D/Glide/DDraw dropped (`overrides/renderers_dropped.cpp`),
  D2CMP's duplicate item palette table shared with D2Win's (`overrides/d2cmp.cpp`). Function and
  data sections are on by default for the EE.
- Faster `memset`; `Source/D2CMP/SRC/Codec.cpp` built at `-O2` with inlining (`cmake/port.cmake`):
  a sprite decode costs ~20% less. `overrides/dcc_timing.cpp` logs `dcc:` lines (decode cost per
  KiB) every 256 decodes: use those, not frame times, to compare decoder changes (identical stairs
  runs gave 45 and 205 ms frames).
- Live input and `where` (below). Every heap failure is logged with its caller.

Later on 2026-11-19: panel art is resident only while its panel is open (1.6 MB back in fights,
`overrides/panel_art_lazy.cpp`), which pays for a **3 MB sprite cache**; the memory report lists
heap owners by file and line and big files by name; the decoder's second 110 KiB clear per
direction is gone (8% off a decode, `overrides/dcc_clear.cpp`); the profiler counts memset callers.

Next, roughly in order:
- **Busy scenes are decode-bound** even with 3 MB: some Black Marsh stretches run 4-8 FPS while
  the player's and mercenary's own components and state overlays are decoded over and over
  (`dcc: most decoded:` lines). The stairs into lower Harrogath stall in some runs. Profile again on the current build
  (`-DHS_PS2_PROFILE=ON -DHS_EE_OPT_LEVEL=-Os`, `prof` lines in `hs.log`, resolve with
  `mips64r5900el-ps2-elf-addr2line -f -C -e <build>/d2.elf`). Last profile: `CODEC_ReadBits`
  11%, `CODEC_DecodeFramePixels` 9%, `CODEC_EncodeRLE` 5.5%, `GFX_RemoveCacheEntry`
  (CelDataHash.cpp) 5%, outside the program 14%.
- **Don't retry a self-resizing sprite cache.** (The fixed 3 MB below now fits: fights peak at
  23.3 of 25.2 MB.) Evicting sprites from outside the game's own
  paths (at `InitLevel`, from a per-frame governor, or inside a failing allocation, with the
  cache's pick or hand-picked idle sprites) froze the game every time, and a limit below what the
  cache holds makes the game's next sprite allocation wait forever. A fixed 3 MB cache (1 MB tile
  LRU) makes most fights 25 FPS but peaked within 220 KiB of the limit: it needs ~1 MB more
  memory first.
- **Which build is the default**: `-O2` (default) vs `-Os` (`-DHS_EE_OPT_LEVEL=-Os`, 550 KiB more
  heap, ~2 FPS slower in lower Harrogath). All fight results are from `-Os`; `-O2` fights are
  untested since the heap and renderer changes.
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
  presented frames. Scripts are keyed to presented frames: the main menu is up by about frame 500,
  the character list about 50 frames after Single Player, and town about 60 frames after the
  difficulty; menus are at 25 FPS.
- A running game takes more input from the host: `tools/port/live.sh "move 300 40" "where"` (the
  game reads `run/live.txt` about once a second). `where` logs the monsters, objects and exits
  around the player with the screen point to click (`where:` lines in `hs.log`).
- `run-pcsx2.sh` closes any running PCSX2 first (and waits for it), so the owner's open session
  goes with it. It rewrites `~/Library/Application Support/PCSX2/inis/PCSX2.ini` keys (HostFs, PINE, ExtraMemory,
  logging). The BIOS is chosen by the owner in PCSX2's settings; never install or pick one.
- Game data is `$D2DATA` (default `~/d2data`: the 11 MPQs listed in the README), symlinked into the
  run folder, never copied. Saves land in `build-ps2/run/hsroot`.
- **Driving a running game from the host:** `tools/port/live.sh --build <dir> "<step>"...` appends
  steps to `run/live.txt` (read about once a second). Explicit `+N` offsets count from when the
  file is read, not from the previous step, so give them cumulatively. Hover is resolved while
  drawing: send `move x y`, then `click x y [right]` at least ~12 frames later. `where` logs nearby
  monsters/objects/exits with the screen point to click (`where:` lines; object 119 is Act 1's town
  waypoint). Teleport doesn't work in town. Black Marsh is at about (125, 219) in the Act I
  waypoint list.
- `tools/port/drive.py wp` / `hunt N` (env `HSB=<build dir>`) automates the Act 1 fight test
  with live input and `where`. `tools/port/scripts/harrogath-stairs.txt` is the stairs benchmark
  (Hell save; frames 3000-3500 are the stairs), `load-save-normal.txt` / `load-save-hell.txt` load
  the save and close the panels.
- **Loading the owner's save** (local only, never committed): Single Player (400,308) at frame
  550, OK (700,555) at 760, Normal (400,281) / Hell (400,367) at 840; town is up ~60 frames later.
  A new build dir has no save: copy `run/hsroot/` from another build dir.
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
