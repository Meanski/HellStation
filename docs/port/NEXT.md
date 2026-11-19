# Handoff: where HellStation stands and what's next

Written 2026-11-18, rewritten at the end of 2026-11-19, updated through the night of 2026-11-20, for whoever picks the work up next (the
owner is changing Claude accounts, so assume no memory of earlier sessions: everything needed is in
the repo). Read [AGENTS.md](../../AGENTS.md) first (the contract), then this. The full history is
in [PORTING_LOG.md](../../PORTING_LOG.md); each entry opens with an "In short:" line. The task list
is the [issue tracker](https://github.com/Meanski/HellStation/issues).

## Status: sound works; fits in a retail 32 MB; towns ~22 FPS, fights vary

**2026-11-20 (late): the IOP's spare 1.5 MB is used.** 1 MB caches the disc (town from the
disc 44 s -> 12 s); the rest, up to 1.25 MB when run from the host, holds copies of decoded
sprites (65-73% of fight decodes become 1.8 ms copies). Both are RPC servers in our IOP module.

**2026-11-20 (overnight session): sound.** Both ports play the game's sound. On the PS2 it runs
on SPU2 hardware voices from SPU2 memory (`backend/ps2/audio.cpp` + the IOP module
`backend/ps2/iop/hsaudio.c`, embedded in the ELF). Costs: 1.6 MB of main RAM (sample cache
448 KiB in its own arena, the 651 KiB sound table, a 256 KiB lock scratch), paid for by
dropping the sprite cache from 3 to 2.5 MB; town ~22 FPS (was 25). Fights peak at 24.0 of
25.1 MB. Sounds over 128 KiB stream from the archive. Not yet listened to on the PS2. Details
and numbers: PORTING_LOG, "2026-11-20: sound".

**2026-11-20: the pad can type** (L3+R3 toggles text entry; the D-pad types), and **-Os is the
default EE build** (an -O2 build ran out of memory in the fight test with sound in).

**2026-11-20: a disc image boots.** `tools/port/make-iso.py` (or `run-pcsx2.sh --iso`) builds a
DVD image from the ELF and the user's own MPQs; booted from it, the game reads data from
`cdrom0:` through a 1 MB read cache on the IOP and keeps saves on the memory card (formats an
unformatted card in slot 1). New character -> Rogue Encampment at 22-25 FPS in PCSX2. Loads:
town 12 s, walking stalls up to 4 s. PORTING_LOG, "a disc image boots".

With the heap stopped at a retail 32 MB (PCSX2's Extended RAM off), the game boots, loads saves,
walks every town tested (Act 1 to Act 5) and fights in Act 1's Black Marsh without running out of
memory. Towns run at 22-25 FPS. Fights are 25 FPS most of the time, but some stretches drop to
4-8 FPS while sprites are decoded over and over (see "Fights" below). The owner's
goal is **a disc in a stock retail PS2**; don't pitch emulator-only or "extended RAM" setups.
Every change is measured at the retail cap.

The overnight commits on `main` are not pushed (the owner pushes). `git log origin/main..main`
lists them: the DirectSound shim and SDL mixer, the PS2 sound backend and IOP module, the memory
policies that make room for sound, the yield fix, -Os by default, the disc image and disc boot,
pad text entry, and their docs.

**Overnight gotchas (also in PORTING_LOG):** on the EE a yield only rotates the caller's
priority (`hs_yield` sleeps 100 us now); Sony's CD driver reads a DVD's path table from sector
257; PCSX2 calls an image a CD if its root directory is 2048 bytes; `fstat` on `cdrom0:` reports
nothing; the memory card's getstat answers for missing paths and it refuses O_TRUNC; PCSX2's
memory cards start unformatted; `pkill -f` on PCSX2's path kills any shell whose command line
contains that text (use `tools/port/stop-pcsx2.sh`, which matches the process name).

### What 2026-11-19 did (numbers in PORTING_LOG)

- One TLSF-style heap for malloc and the VirtualAlloc regions (`backend/ps2/memory.cpp`).
- 485 KiB of program image back: unused renderers dropped, a duplicate palette table shared.
- Faster `memset`; the DCC decoder (`Source/D2CMP/SRC/Codec.cpp`) built at `-O2` with inlining.
- **Memory report names owners**: Fog's exact-size blocks are tallied by the file and line that
  asked, and big whole-file loads are listed by name (`overrides/fog_exact_alloc.cpp`, printed
  every 4th `mem` report as `KiB in ... from File.cpp:line` and `files:` lines).
- **Panel art only while its panel is open** (`overrides/panel_art_lazy.cpp`, a wrap on
  `DRAW_UI`): skill tree, character panel, trade, mercenary, party, waypoint list, other acts'
  automap tiles. 1.6 MB back in fights. That paid for a **3 MB sprite cache** (was 2 MB).
- **Decoder clear**: the second 110 KiB block-table clear per direction now clears only what pass 1
  cleared (`overrides/dcc_clear.cpp`, via a redirect of Codec.cpp's `memset` calls). 8% off a
  decode; the first 200 decodes of every run are checked byte-for-byte against the full clear
  (`dcc clear check:` line in `hs.log`).
- Profiler counts memset callers as well as memcpy; the `dcc:` log names the most-decoded files.

### Memory at a glance (Black Marsh fight, `-Os`, 3 MB sprite cache)

Heap limit 25,182 KiB (32 MB minus 6.4 MB program image and kernel); fights peak at about
23,300 KiB taken. Of Fog's 13.4 MB of exact-size blocks: data tables kept raw 2.1 MB, sprite
cache 3 MB, tile LRU 2 MB (67-80% full in Black Marsh), string tables ~1.2 MB, UI art ~1.1 MB,
AnimData.d2 0.56 MB.

## Next, in the order I'd take them

1. **Listen to it.** Sound has only been checked by readback, voice envelopes and (on Linux) an
   analysed WAV. Run PCSX2 with audio on and listen: effects, ambience, music, the mix balance.
   If a core is silent, look at the MMIX settings in `backend/ps2/iop/hsaudio.c`.
2. **Memory for speed.** With sound in, the sprite cache is back at 2 MB (2.5 MB ran out of
   contiguous memory after minutes in a fight), so big packs are slow again. Room to win back:
   the 651 KiB sound table (sounds.txt rows, fixed 142-byte stride), the 448 KiB sample cache
   (compress it), the 256 KiB DirectSound scratch, raw data tables (2.1 MB). Fragmentation is
   the real enemy: decoded sprites are 20-600 KiB blocks churning through one heap.
3. **Fights: fewer or cheaper decodes.** Done in part: decodes are copied into up to 1.25 MB
   of the IOP's spare RAM and 73% of fight decodes became 2.8 ms copies instead of 13.5 ms
   decodes (`backend/ps2/iop/l2.c`, PORTING_LOG "a second sprite cache"). Still open: a hit
   costs 1.8 ms for 17 KiB (was 3.2 before the ring's entries were aligned); on a disc boot the
   disc cache leaves it only ~300 KiB (512 KiB of disc cache was measured: town load 17 s
   instead of 12, so the disc keeps 1 MB); stores over 32 KiB still wait.
4. **The disc: load times.** It boots and plays from a PCSX2 DVD image, with a 1 MB read cache
   on the IOP: 12 s into town, walking stalls up to 4 s. Next would be laying files out so what
   loads together sits together. Real hardware can't boot a burned DVD-R without a modchip or FreeMcBoot+ESR; USB/HDD
   loading (OPL) is the practical way onto a console and may be worth supporting.
5. **Real hardware**: unaligned loads fault there, where PCSX2 reads wrong data quietly.
   `tools/port/sanitize-sweep.sh` (Linux UBSan build at the PS2's 640x480) found four in
   town/waypoint/fight paths, now fixed, and is clean; widen it to other acts, trading, the
   stash and other classes; null
   reads too (PCSX2 logs them as "TLB Miss" and carries on: grep `pcsx2.log` after a run; one in
   GfxHash was fixed, a fight session shows none).
6. **The class-select voice lines** load on demand now, but the scripted run never requested
   one; check by clicking classes by hand.

**Don't retry a self-resizing sprite cache.** Evicting sprites from outside the game's own paths
(at `InitLevel`, from a per-frame governor, or inside a failing allocation, with the cache's pick or
hand-picked idle sprites) froze the game every time, and a limit below what the cache holds makes
the game's next sprite allocation wait forever. Fixed sizes only.

## How to run it

The current PS2 build dir is **`build-ps2-os`** (`-Os`, now the default; retail heap; has the owner's save). `build-ps2` is now -Os too after a reconfigure.
`build-ps2-osprof` is the same with the profiler (`-DHS_PS2_PROFILE=ON`). `build-ps2` is stale.

```sh
tools/port/ps2.sh cmake --build build-ps2-os
tools/port/run-pcsx2.sh --build build-ps2-os --seconds 1200 \
  --script tools/port/scripts/load-save-normal.txt              # loads the save, closes panels
HSB=build-ps2-os tools/port/drive.py wp                          # town waypoint -> Black Marsh
HSB=build-ps2-os tools/port/drive.py hunt 14                     # 14 rounds of fighting
```

- Logs: `<build>/run/hs.log` (over `host:`): `frame N` pacing every 250 frames, `mem` lines,
  owner tallies, `dcc:` decode lines, `panel art:` loads/frees. A RAM ring is read over PINE into
  `run/ring.log` when the runner's sleep ends.
- Profile: build `build-ps2-osprof`, run the same, then aggregate `prof` lines over the frames you
  care about and resolve with `tools/port/ps2.sh mips64r5900el-ps2-elf-addr2line -f -C -e
  build-ps2-osprof/d2.elf <addrs>`. `prof memset/memcpy from <addr>` lines give callers by bytes.
- Fight results vary a lot run to run (which monsters, where the mercenary walks): one run is
  25 FPS throughout, the next has 4-8 FPS stretches. Compare decoder changes with paired
  measurements (the `dcc clear check:` pattern: same decode both ways), not frame times.
- `run-pcsx2.sh` closes any running PCSX2 first, so the owner's open session goes with it. It
  rewrites PCSX2.ini keys (HostFs, PINE, ExtraMemory, logging). The BIOS is the owner's choice in
  PCSX2's settings; never install or pick one. Runs lag the owner's Mac: keep them short.
  The runner closes PCSX2 when the run ends (`--keep` leaves it open); a script that finishes
  early stops it with `tools/port/stop-pcsx2.sh`. Don't use `osascript ... quit`: PCSX2 asks
  for confirmation and the window stays open on the owner's screen.
- Scripts are keyed to presented frames: main menu by ~500, character list ~50 after Single
  Player, town ~60 after the difficulty. **Loading the owner's save** (local only, never
  committed): Single Player (400,308) at 550, OK (700,555) at 760, Normal (400,281) / Hell
  (400,367) at 840. A new build dir has no save: copy `run/hsroot/` from `build-ps2-os`.
- **Live input:** `tools/port/live.sh --build <dir> "<step>"...` appends steps to `run/live.txt`
  (read about once a second). `+N` offsets count from when the file is read, so give them
  cumulatively. Send `move x y`, then `click x y [right]` ~12 frames later. `key 0x49` (I), `0x54`
  (T, skill tree), `0x43` (C), `0x4F` (O, mercenary). `where` logs nearby monsters/objects/exits
  with the point to click (object 119 is Act 1's town waypoint; Black Marsh is about (125, 219) in
  the Act I waypoint list). Teleport doesn't work in town.
- `tools/port/winshot.sh PCSX2 out.png` / `winrec.sh PCSX2 out.mov <secs>` capture the PCSX2
  window. Captures stay out of the repo (game art): put them in a scratch folder.
- Game data is `$D2DATA` (default `~/d2data`), symlinked into the run folder, never copied.

### The other builds (definition of done)

- Stock Windows: `cmake --build build` (must link).
- Linux i686: `tools/port/linux-i386.sh cmake --build build-linux`. **The debug link needs more
  memory than Docker currently has free** (the owner runs other containers; ld was killed three
  times with ~3.9 GB free). `build-linux-nodbg` is configured with `-g0` and links fine. Town smoke
  on it: run the binary as `tools/port/run-linux.sh` does (that script hard-codes `build-linux/d2`)
  with `HS_INPUT_SCRIPT=/src/tools/port/scripts/rogue-encampment.txt`, 900 s; last pass 22,200
  frames, no halt. Don't stop the owner's containers without asking.

## Gotchas that cost hours (don't relearn them)

1. **PCSX2's EE recompiler loses the upper half of a non-aligned `LWR`.** Every EE object goes
   through `tools/port/ee-cc-launcher.sh` -> `ee-lwr-fixup.py`. Symptom without it: negative values
   from packed structs compare as huge positives (units teleporting along walks).
2. **PCSX2 doesn't fault on misaligned word loads; it reads wrong data.** A real PS2 raises an
   address error. Treat "works on Linux, garbage on PCSX2" as alignment first. Packed structs
   (e.g. D2Client's 34-byte skill-tree entries) need `memcpy` access in hand-written code.
3. **The EE build has no inlining or IPA cloning** (`cmake/ps2-ee.cmake`): overrides patch functions
   by symbol. A hand-written file that needs inlining gets a per-source exemption in
   `cmake/port.cmake` (as Codec.cpp has); otherwise even `inline` helpers become calls.
4. **The EE kernel has only 256 semaphores and no time slicing**, but an interrupt that wakes a
   higher-priority thread pre-empts. Shared state needs interrupts off (DIntr/EIntr) or a lock, and
   every wait or spin must yield.
5. **Single player runs the game server inline on the main thread.**
6. **Docker bind mounts sometimes serve a stale file** right after an edit (CMake "Parse error" or
   an old object). Rerun.
7. **Generated code (`Source/`, `3rdparty/Storm`, `3rdparty/crashy`) is never edited.** Route around
   it in `Redivivus/port/`: overrides (weaken and replace; same-file callers reach yours), wraps
   (rename the definition in one object; only callers outside it reach yours), and statics
   globalized by rename (all in `cmake/port.cmake`, documented in `docs/port/overrides.md`). A
   wrap entry can also name a *reference*: `"D2CMP|Source/D2CMP/SRC/Codec.cpp|memset"` sends that
   object's memset calls to `hs_orig_memset`.
8. **PCSX2's cycle counter is an instruction-count model**, so cycle timings on PCSX2 only rank
   changes that remove work; micro-optimisations need hardware.
9. **Wall-clock decode timings include pre-emption** of the decoding thread: compare paired runs.

## Owner preferences

- Commit only when asked; push only when asked. Never force-push `main`.
- Commits are the owner's alone: no co-author lines, no tool or model names anywhere (commit
  messages, comments, docs). The commit email is the GitHub no-reply address in this repo's git
  config. The hooks enforce this (`git config core.hooksPath tools/hooks` on a fresh clone).
- Short plain-language status updates while long work runs; report with numbers. PORTING_LOG
  entries open with an "In short:" headline (what got N FPS by doing X).
- The owner wants visible results (footage, a clean README, a working demo) and an honest story
  with numbers: playable progress over infrastructure polish.
- The repo is HellStation's own. Don't re-add Redivivus's scaffolding (CI, templates, README).

## Local-only state (not in git)

- Build dirs: `build-ps2-os` (current, has the save), `build-ps2-osprof` (profiler),
  `build-linux-nodbg` (links), `build` (mingw), others stale.
- Footage is in `~/Movies/HellStation/` (`before-vs-after.mp4`).
- Build logs (`build*.log`) are ignored.
