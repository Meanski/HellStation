# Porting log

Append-only, newest last. Each entry is dated and records one fact: a stub, an alignment fix, a memory
measurement, a decision and its evidence.

## 2026-11-17: project set up

- Base: Redivivus `dd289669` (2026-11-15). `upstream` = jaenster/redivivus (fetch-only).
- Scope: LoD 1.14d. Classic is a fallback only if a measured memory profile forces it.
- Controller support in scope from Phase 1 (`docs/port/controller.md`).
- Host: Apple Silicon (arm64), macOS 26.7.1. Docker 27.4. `ps2dev/ps2dev:v2.0.0` is a native arm64
  image (the brief expected amd64 under emulation). It has EE GCC 15.2.0, plus SDL2, gsKit, audsrv,
  padman, ps2mouse, ps2kbd and bdm in the image. That settles the brief's "verify" on SDL2-for-PS2:
  the port is present.

## 2026-11-17: Phase 0 build

- `cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake` + ccache, Homebrew mingw-w64 14.0.0:
  510 steps, no errors, 2m31 wall on this Mac. `build/d2.exe`: PE32 i386 GUI, 327 MB with debug info.
  The brief's psapi workaround wasn't needed with these headers.
- **Watch:** Homebrew's mingw-w64 targets **UCRT** (imports `api-ms-win-crt-*`), whereas upstream CI on
  Ubuntu links msvcrt. D2 reads MSVC `FILE` (`_iobuf`) fields directly, and the UCRT `FILE` layout
  differs. If the Mac-built exe misbehaves at runtime on Windows/Wine, rebuild with an msvcrt mingw
  (the Ubuntu toolchain in `tools/port` or upstream CI) before suspecting anything else.
- Module gates: 59 wrong-direction pairs, 33 internal-include pairs (upstream's baseline; not ours).
- Not run yet: needs the owner's 1.14d data and Wine/CrossOver or a Windows PC.

## 2026-11-17: Phase 1 container

- `tools/port/linux-i386.sh` builds `hellstation/linux-i386:bookworm`: i686 userland under qemu-i386,
  GCC 12.2, CMake 3.25.1, SDL2 2.26.5, `LONG_BIT` 32. First image build took 3m23.
- Docker Desktop on arm64 had no x86 binfmt handler (`exec format error`). The wrapper re-registers
  `qemu-i386` through `tonistiigi/binfmt` when the probe fails; the registration doesn't survive a
  Docker restart.
- CMake 3.25 in bookworm is above the ≥3.24 the brief requires.

## 2026-11-17: first pixels on an emulated PS2

- `tools/port/ps2-smoke` (gsKit) draws a 640×480 PSMT8 texture with a 256-entry CT32 CLUT, scaled
  to the display, and cycles the palette every frame without re-uploading the indices. That's the
  presentation path planned for D2Gdi's back buffer.
- Runs in PCSX2 2.8.2 (native arm64, Metal) on OpenPS2BIOS 0.6 (MIT, no Sony firmware; sha256
  `3702baa5…1a84`, kept outside the repo in PCSX2's bios folder). Boots with
  `-fastboot -elf`; GS 640×448 NTSC, 60 FPS / 60 VPS, GPU 3%. Gradient and CLUT ordering are correct
  (the CSM1 8–15/16–23 swizzle is handled).
- The ps2dev image ships no make or cmake. `tools/port/Dockerfile.ps2` layers make, cmake, ninja,
  ccache and python3 on the pinned `v2.0.0` image as `hellstation/ps2dev:v2.0.0`.
- PCSX2 rejects a hand-written ini without `[UI] SettingsVersion = 1`.

## 2026-11-17: Phase 0 exit: main menu under Wine

- `tools/port/run-wine.sh` runs `build/d2.exe -w` (Homebrew mingw, UCRT) under Wine 11.0 (Gcenx build;
  Homebrew's own Wine casks were disabled on 2026-09-01), prefix `~/.wine-hellstation`, from a run dir
  of symlinks to `$D2DATA`. Data: the owner's 1.14d LoD set, all 11 MPQs (`MPQ\x1a` headers checked)
  plus binkw32/SmackW32/ijl11.
- Reaches the LoD main menu with the `R2109` build stamp. The UCRT `FILE` concern above didn't block
  startup; it stays on the watch list until a save/load and a log write have been exercised.
- Not yet exercised: single player, character creation, in game.
- `tools/port/winshot.sh` captures one app's window by CoreGraphics window id. Full-screen captures
  are not used: they pick up whatever else is on the owner's screen.
