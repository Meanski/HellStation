# HellStation port plan

Diablo II: LoD 1.14d single player on the PS2, from the Redivivus tree (base snapshot `dd289669`,
2026-11-15). Hand-written code only; generated code (`Source/`, `3rdparty/Storm`, `3rdparty/crashy`)
is never edited. Items marked **(verify)** are unconfirmed; whoever confirms one replaces the
marker with what was checked and how.

## Scope decisions

| Decision | Choice | Why |
|-|-|-|
| Game | **LoD 1.14d** (all 11 MPQs incl. `d2exp`, `d2xmusic`, `d2xtalk`, `d2xvideo`) | Owner's call. Classic is a fallback only if the Phase 1 memory profile proves LoD can't reach 32 MB. |
| Input | Mouse/keyboard via USB **and** DualShock 2, controller designed in from Phase 1 | Owner's call. See [controller.md](controller.md). |
| Renderer | D2Gdi software renderer, presented by the backend | 8-bit palettised frame → SDL texture / GS PSMT8 + CLUT. |
| Sound | Fake DirectSound COM under D2Sound | Keeps D2Sound's mixing intact. |
| Net | Winsock stubbed | Single player only. |
| Cinematics | Skipped | Bink/Smacker are closed; FFmpeg later, maybe. |
| Upstream | Snapshot import + `tools/sync-redivivus.sh` | HellStation's history is its own; credit stays in the import commit, README and LICENSE. |

## Phases and exit criteria

**Phase 0: baseline.** Stock mingw `d2.exe` builds on the Mac (done 2026-11-17, see `PORTING_LOG.md`).
Exit: also reaches the main menu with the owner's 1.14d data under Wine (done 2026-11-17).

**Phase 1: i686 Linux + SDL2** (container: `tools/port/linux-i386.sh`).
1. `REDIVIVUS_PORT` option and `cmake/linux-i686-sdl.cmake`; loosen the `NOT WIN32` guard behind it.
2. `Redivivus/port/prelude.h`, forced include; build until compile errors are only real Win32 gaps.
3. `tools/port/gen_link_data.py` produces `Redivivus/port/link_data.cpp` from `d2_link_support.s`.
4. Link with `--unresolved-symbols=report-all`; seed `docs/port/symbols.tsv`; shim area by area.
5. Files + MPQ → first frame (D2Gdi via SDL) → input (mouse/keyboard, then pad layer 0) → sound →
   Act 1 → save/load.
6. Sanitizer config (`-fsanitize=alignment,undefined`) on every smoke run.
7. Memory profile: peak RSS and Fog pool stats in Rogue Encampment, Harrogath, Worldstone Keep,
   a busy Act 5 fight. Write it down. **This decides LoD vs classic and the trimming budget.**

Exit: boots, plays Act 1 to Andariel and into Act 5 town, saves and loads, with sound and pad
layer 1; UBSan alignment clean on the smoke path; memory profile in the log.

**Phase 2: PS2 bring-up** (container: `tools/port/ps2.sh`, `ps2dev/ps2dev:v2.0.0`, EE GCC 15.2;
the image ships SDL2, gsKit, audsrv, padman, ps2mouse, ps2kbd, bdm).
1. `cmake/ps2-ee.cmake`; same shim API on ps2sdk (fileio/bdm, kernel semas/threads with yields).
2. Try the SDL2 PS2 backend first to reuse Phase 1; keep a hand-written gsKit present path ready.
3. Data from USB mass storage (bdm), or `host:` via ps2link during development.
4. Order: ELF boots in Play! → MPQs open → first frame on GS → menus → in game.
5. Bring-up may use PCSX2's extended RAM **(verify the option exists and the ps2sdk linkfile can use it)**.

Exit: main menu in PCSX2 at 32 MB, then in game, then on real hardware.

**Phase 3: fit and feel.** Memory trimming to 32 MB with LoD, EE performance (scratchpad, VU0 if
needed), 480i/480p modes, controller layer 2 polish, optional cinematics.

## Fitting LoD into 32 MB

Levers, in the order to try them, each one measured before and after:
1. Strip code that can't run: renderers other than GDI, Battle.net/realm, launcher, multiplayer UI,
   cinematic decoders. Link-time removal (`--gc-sections`, per-module), not source edits.
2. Shrink DCC/DC6 caches in D2CMP/Fog through their size parameters/globals, set from hand-written code.
3. Free act data on act change.
4. Stream music and speech from storage through audsrv/SPU2 RAM (2 MB) instead of EE RAM.
5. MPQ file-table residency: load hash/block tables lazily, read file data on demand.
6. Only then: classic-only fallback, with the numbers that forced it.

## Top risks

1. Memory (above).
2. Unaligned loads: DS1/DCC/`.d2s`/packets/MPQ tables cast from byte buffers. Find on Linux with UBSan.
3. Non-IEEE single-precision FPU, software `double`: NaN tests, div-by-zero, precision-dependent RNG.
4. Upstream regeneration churn: stay out of generated code; sync upstream often.
5. Non-time-sliced EE threads: busy waits starve; the shim yields.
6. EE performance: 294 MHz in-order, 8 KB D-cache.

## Alternatives considered

- **D2Decomp** (psi29a/d2decomped, GPL-3.0, C++23, SDL3): a cross-platform 1.14d-referenced
  *reimplementation* that already plays all five acts on Linux/macOS/Windows with full gamepad
  support. Not our base: it's a reimplementation rather than the original code, it pulls in Qt,
  OpenAL, StormLib and FFmpeg, and its licence is GPL. It's the best working reference for
  controller UX on D2 and for behaviour questions. Design reference only, no code.
- **D2Vita** (Franckrst/D2Vita, GPL-3.0): runs the original x86 binary through box86 on the Vita. A
  JIT on a 32 MB, 294 MHz EE is not viable. Its control scheme is a useful reference.
