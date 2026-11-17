# HellStation

Diablo II: Lord of Destruction 1.14d running natively on the PlayStation 2. It isn't emulated,
streamed or a remake: it's the game's own code, compiled for the PS2's Emotion Engine.

HellStation is built on [Redivivus](https://github.com/jaenster/redivivus), jaenster's
reverse-engineering of 1.14d into C++ that builds and runs. Redivivus supplies the game. HellStation
supplies everything a PS2 needs underneath it: a fake Windows, a GS renderer, pad input, and a
build that fits the EE.

## Status

|Milestone|State|
|-|-|
|Main menu on an emulated PS2 (PCSX2)|done|
|In game: new LoD character, Rogue Encampment, walking, inventory|done, 25 FPS (the game's own cap)|
|DualShock 2 as a virtual mouse plus D2 hotkeys (controller layer 0)|done|
|Fits a retail PS2's 32 MB of RAM|**not yet**: needs PCSX2's Extended RAM for now|
|Sound|not yet|
|Real hardware|not yet tried|
|Typing a character name with the pad|not yet (new characters need the input script for now)|

The dated engineering log, with every measurement and decision, is [PORTING_LOG.md](PORTING_LOG.md).
The plan and its exit criteria are in [docs/port/PLAN.md](docs/port/PLAN.md).

## You bring the game

This repository holds no game data and never will. You need your own Diablo II and Lord of
Destruction, patched to 1.14d. Put these files in a folder (default `~/d2data`, or set `$D2DATA`):

```
d2char.mpq  d2data.mpq  d2exp.mpq  d2music.mpq  d2sfx.mpq  d2speech.mpq
d2video.mpq  d2xmusic.mpq  d2xtalk.mpq  d2xvideo.mpq  patch_d2.mpq
```

PCSX2 also needs a BIOS dumped from your own console.

## Build and run

The host is macOS on Apple Silicon with Docker. The PS2 toolchain runs in the `ps2dev/ps2dev`
container, which `tools/port/ps2.sh` wraps.

```sh
git config core.hooksPath tools/hooks          # once: no game data, no stray binaries

tools/port/ps2.sh cmake -S . -B build-ps2 -G Ninja -DREDIVIVUS_PORT=ON \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ps2-ee.cmake -DHS_PS2_EXTENDED_RAM=ON
tools/port/ps2.sh cmake --build build-ps2

tools/port/run-pcsx2.sh --extra-ram --seconds 5
```

`run-pcsx2.sh` stages the ELF next to your game folder, turns on the PCSX2 settings the port
needs (host file access, Extended RAM), and starts it. Add
`--script tools/port/scripts/town-walk.txt` to play back a scripted run that makes a Barbarian and
walks around the Rogue Encampment.

The port also builds for 32-bit Linux with SDL2 (Phase 1, the debugging target), and the stock
Windows build is kept working. Both are covered in [AGENTS.md](AGENTS.md).

## Controls (DualShock 2)

|Pad|Does|
|-|-|
|Left stick / right stick|Cursor, fast / precise|
|✕ or R2|Left click|
|○ or L2|Right click|
|Start|Esc (game menu)|
|△ / □ / R1|Inventory / skill tree / character sheet|
|Select|Automap|
|L1 (hold)|Shift: stand still|
|L3 / R3|Toggle run / show items|
|D-pad|Belt slots 1–4|

This is layer 0, the pad as a mouse. Walking with the stick, auto-targeting and pad-driven
inventory are designed in [docs/port/controller.md](docs/port/controller.md).

## How it works

- **No game code is edited.** The decompiled tree is regenerated upstream, so everything PS2-specific
  sits beside it: a forced-include prelude, a Win32 shim (`Redivivus/port/win32/`), link-time
  overrides, and backends (`Redivivus/port/backend/`).
- **Rendering** uses the game's own software renderer. Each 8-bit frame goes to the GS as a
  paletted texture with its colour table.
- **Files** are read straight from your MPQs through the `host:` device (PCSX2's host file system
  for now).
- **Interesting bugs** are written up in [PORTING_LOG.md](PORTING_LOG.md). For example, units
  teleported along walks because PCSX2's EE recompiler mishandles one unaligned-load instruction
  (`LWR`). The build now compensates, and a report for PCSX2 is drafted in
  [docs/port/upstream/](docs/port/upstream/).

## Credits

- [Redivivus](https://github.com/jaenster/redivivus) by jaenster and the Redivivus contributors:
  the reverse-engineered game this runs, imported as a snapshot (the first commit here, with the
  upstream revision in `UPSTREAM`). Fixes that belong to Redivivus go back there.
- [ps2dev](https://github.com/ps2dev): the PS2 toolchain, ps2sdk and gsKit.
- [PCSX2](https://pcsx2.net): the emulator used for bring-up.
- Controller design was informed by d2decomped, D2Vita, DevilutionX and D2R. None of their code is
  used; see [docs/port/controller.md](docs/port/controller.md).

## License

MIT, see [LICENSE](LICENSE). The imported Redivivus code keeps its own copyright notice there, and
its `.cpp.map` files are used under its terms: credit to Redivivus, linked above.

Diablo and Blizzard Entertainment are trademarks of Blizzard Entertainment, Inc. PlayStation is a
trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by
either.
