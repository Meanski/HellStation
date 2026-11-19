<p align="center">
  <img src="docs/assets/banner.svg" alt="HellStation: Diablo II, Lord of Destruction, native on PS2" width="100%">
</p>

<p align="center">
  <img alt="Platform: PlayStation 2" src="https://img.shields.io/badge/platform-PlayStation%202-1f4ea0?style=for-the-badge">
  <img alt="Game: Diablo II LoD 1.14d" src="https://img.shields.io/badge/Diablo%20II-LoD%201.14d-8b1a1a?style=for-the-badge">
  <img alt="Status: in game in 32 MB on PCSX2" src="https://img.shields.io/badge/status-in%20game%20in%2032%20MB%20on%20PCSX2-e36209?style=for-the-badge">
  <br>
  <a href="https://github.com/Meanski/HellStation/issues"><img alt="Open issues" src="https://img.shields.io/github/issues/Meanski/HellStation?style=for-the-badge&color=6e7681"></a>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-2ea44f?style=for-the-badge"></a>
  <a href="https://github.com/jaenster/redivivus"><img alt="Built on Redivivus" src="https://img.shields.io/badge/built%20on-Redivivus-5a1410?style=for-the-badge"></a>
</p>

<p align="center">
  <a href="#status">Status</a> ·
  <a href="#fitting-in-32-mb">Fitting in 32 MB</a> ·
  <a href="#build-and-run">Build</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="PORTING_LOG.md">Dev log</a>
</p>

---

**Diablo II: Lord of Destruction 1.14d, running natively on PlayStation 2 hardware, as emulated by
PCSX2.** It isn't emulated, streamed or remade: it's the game's own code, compiled for the PS2's
Emotion Engine, with everything a PS2 needs underneath it. That means a fake Windows, a GS
renderer, DualShock 2 input, and a build tuned for the EE.

> [!NOTE]
> **It now fits in a retail PS2's 32 MB, as PCSX2 emulates one.** Creating a character, the Rogue
> Encampment and Harrogath (Act 5, Hell) all run with PCSX2's Extended RAM off, with under 1 MB to
> spare. In November 2026 the project nearly stopped over memory. It isn't on a disc or real
> hardware yet, there's no sound, and fights outside town survive but slow down in big packs. The numbers are below
> and in the [dev log](PORTING_LOG.md).

## Status

| | Where it stands |
|:-:|-|
| ✅ | Boots to the LoD main menu on an emulated PS2 (PCSX2) |
| ✅ | In game: new character, Rogue Encampment, walking, inventory, at **25 FPS** (the game's own cap) |
| ✅ | Reaches Act 5 (Harrogath, Hell difficulty) with a level 96 character, at about **25 FPS** in 640×480 |
| ✅ | Fits a retail PS2's **32 MB** (PCSX2, Extended RAM off): character creation, both towns ([#5](https://github.com/Meanski/HellStation/issues/5)) |
| ✅ | DualShock 2 as a virtual mouse plus D2's hotkeys |
| ✅ | Harrogath's lower town at **25 FPS** in 32 MB (it was 15: a byte-at-a-time `memcpy` and a blocking vsync wait) |
| ⚠️ | Walking down the stairs into lower Harrogath stalls for about a minute at 2-5 FPS: the 2 MB sprite cache thrashes |
| ⚠️ | Fights outside town (Act 1, Black Marsh) run in 32 MB without running out, but big mixed packs drop to 4-10 FPS: the sprite cache is too small to hold them |
| ❓ | Not tried yet: long sessions |
| ⏳ | Not started: sound, naming a character with the pad, a disc image, real hardware |

## Fitting in 32 MB

A retail PS2 has 32 MB of RAM. After the kernel, the 7.4 MB program and the stack, the game gets
**24.2 MB**. Peak memory on PCSX2, measured with a report built into the port:

| | Rogue Encampment (Act 1) | Harrogath (Act 5, Hell) |
|-|-|-|
| As first measured | 61.1 MB | |
| After the cheap fixes | 36.8 MB | 37.0 MB |
| After replacing Fog's allocator | 27.5 MB | 28.2 MB |
| After every fix below | **23.3 MB** | **23.4 MB** |
| What a retail PS2 has | 24.2 MB | 24.2 MB |

The bottom row is measured with the heap stopped at a retail 32 MB and PCSX2's Extended RAM off.
Creating a character peaks at 22.7 MB. The fixes that got it from 61 to 23 MB:

- **Telling the game it has 32 MB, not 256 MB.** It sizes its caches from what Windows reports.
- **Keeping large and small allocations apart.** The town had been failing to load with 11 MB
  free but cut into pieces too small to use.
- **Thread stacks of 128 KiB, not 1 MiB.** Measured: they never used more than 16 KiB.
- **Capping the sprite cache at 2 MB.** That costs frame rate: sprites get decoded again more
  often.
- **Playing at 640×480 instead of 800×600.** Little memory, but Act 5 went from 15 FPS to about 25.
- **Exact-size allocations instead of the game's pool allocator.** The game rounds every block up
  to a power of two and keeps half-empty 512 KiB chunks around. Above 256 bytes, the port now hands
  out exactly what's asked for. That saved 9 MB, at no cost in speed.
- **The same for Storm, the game's other allocator.** It reserved a 64 KiB window for every line of
  code that allocates: 2.2 MB held for 0.3 MB used.
- **Compact MPQ indexes.** The archives' hash tables are mostly empty slots: `d2data.mpq` uses
  10,815 of 65,536. Keeping only the used ones frees 2.5 MB.
- **Big temporary buffers kept away from small objects,** so freeing them leaves gaps that close up
  again instead of holes nothing fits in.
- **Loading class animations one class at a time** on the character-creation screen, which had all
  seven loaded at once (12.3 MB).

What's left is thin: under 1 MB of headroom. Sound usually takes 3-5 MB, so it has to live in the
sound chip's own 2 MB. Fights run without running out of memory, but big packs are slow. Long
sessions and real hardware are still to be tried. The full measurements and each step are in the [dev log](PORTING_LOG.md).

## You bring the game

> [!IMPORTANT]
> This repository holds no game data and never will. You need your own Diablo II and Lord of
> Destruction, patched to 1.14d, and a BIOS dumped from your own PS2.

Put these files in a folder (default `~/d2data`, or point `$D2DATA` at it):

```
d2char.mpq  d2data.mpq  d2exp.mpq  d2music.mpq  d2sfx.mpq  d2speech.mpq
d2video.mpq  d2xmusic.mpq  d2xtalk.mpq  d2xvideo.mpq  patch_d2.mpq
```

## Build and run

You'll need macOS on Apple Silicon, with Docker and PCSX2. The PS2 toolchain runs in the
`ps2dev/ps2dev` container, which `tools/port/ps2.sh` wraps.

```sh
git config core.hooksPath tools/hooks          # once: keeps game data and binaries out of git

tools/port/ps2.sh cmake -S . -B build-ps2 -G Ninja -DREDIVIVUS_PORT=ON \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ps2-ee.cmake
tools/port/ps2.sh cmake --build build-ps2

tools/port/run-pcsx2.sh --seconds 5
```

This build stops its heap at a retail 32 MB. For debugging with headroom, configure with
`-DHS_PS2_EXTENDED_RAM=ON` and add `--extra-ram` to the run: the heap can then use PCSX2's 128 MB.

`run-pcsx2.sh` stages the ELF next to your game folder, turns on the PCSX2 settings the port needs
(host file access, and Extended RAM with `--extra-ram`), and starts it. Add `--script tools/port/scripts/town-walk.txt` to
watch a scripted run make a Barbarian and walk around the Rogue Encampment.

<details>
<summary>Other targets</summary>

The port also builds for 32-bit Linux with SDL2 (the debugging target), and the stock Windows build
is kept working. Both are covered in [AGENTS.md](AGENTS.md).

</details>

## Controls

| DualShock 2 | Does |
|-|-|
| Left stick / right stick | Cursor, fast / precise |
| ✕ or R2 | Left click |
| ○ or L2 | Right click |
| Start | Game menu (Esc) |
| △ / □ / R1 | Inventory / skill tree / character sheet |
| Select | Automap |
| L1 (hold) | Stand still (Shift) |
| L3 / R3 | Toggle run / show items |
| D-pad | Belt slots 1–4 |

This is layer 0: the pad acts as a mouse. Walking with the stick, auto-targeting and pad-driven
panels were designed in [docs/port/controller.md](docs/port/controller.md) but not built.

## How it works

- **No game code is edited.** Everything PS2-specific sits beside the decompiled tree: a
  forced-include prelude, a Win32 shim (`Redivivus/port/win32/`), link-time overrides, and the
  backends (`Redivivus/port/backend/`).
- **Rendering** uses the game's own software renderer. Each 8-bit frame goes to the GS as a
  paletted texture with its colour table.
- **Files** are read straight from your MPQs through the `host:` device (PCSX2's host file system,
  for now).
- **The bugs are the fun part**, and they're all in the [dev log](PORTING_LOG.md). For example,
  NPCs used to teleport mid-walk because PCSX2's EE recompiler mishandles one unaligned-load
  instruction (`LWR`). The build now compensates, and a report for PCSX2 is
  [drafted](docs/port/upstream/pcsx2-lwr-sign-extension.md).

## Credits

- **[Redivivus](https://github.com/jaenster/redivivus)** by jaenster and the Redivivus contributors:
  the reverse-engineered 1.14d this is built on, imported as a snapshot (the first commit here, the
  revision in [`UPSTREAM`](UPSTREAM)). HellStation wouldn't exist without it. Fixes that belong to
  Redivivus go back there.
- **[ps2dev](https://github.com/ps2dev)**: the PS2 toolchain, ps2sdk and gsKit.
- **[PCSX2](https://pcsx2.net)**: the emulator used for bring-up.
- The controller design was informed by d2decomped, D2Vita, DevilutionX and D2R. None of their code
  is used.

## License

MIT, see [LICENSE](LICENSE). The imported Redivivus code keeps its own copyright notice there, and
its `.cpp.map` files are used under its terms: credit to Redivivus, linked above.

<sub>Diablo and Blizzard Entertainment are trademarks of Blizzard Entertainment, Inc. PlayStation
is a trademark of Sony Interactive Entertainment. HellStation is not affiliated with or endorsed by
either.</sub>
