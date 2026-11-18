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

## 2026-11-17: Phase 1: the whole generated tree compiles for i686 Linux

- **Toolchain:** the Phase 1 image is now Debian trixie on the host's own arch, with `g++-i686-linux-gnu`
  14.2 (compiling runs at native speed) and the i386 runtime multiarch (`libsdl2-dev:i386`), so the
  32-bit binary runs in the same container through qemu-i386.
- **Header strategy, decided by measurement:** the non-`_WIN32` half of `d2_platform.h` is a stale
  skeleton. The generated code uses SDK spellings it doesn't provide or provides differently
  (`tagPALETTEENTRY`, `LARGE_INTEGER::u`, `_RTL_CRITICAL_SECTION`, `BITMAPINFO`), and it's generated,
  so it can't be fixed here. The `_WIN32` half is proven against mingw-w64's headers by the shipping
  build. So port builds define `_WIN32` + `_X86_` and compile against **mingw-w64's Win32 API headers**
  (`-idirafter`, public domain, build dependency only), with the host C library underneath.
  `Redivivus/port/prelude.h` reconciles the two: calling conventions (real attributes on i386),
  `__declspec`, the C library owning `FILE`/`timeval`, Winsock owning `fd_set`, 16-bit `wchar_t`
  (`-fshort-wchar`, game wide-CRT calls routed to `wcrt16`), and the MSVC CRT names glibc lacks
  (`win32/msvcrt.h`).
- **Result:** `tools/port/probe-compile.sh`: 462/462 TUs (Source/, Storm, crashy) pass `-fsyntax-only`.
  The first probe on the non-`_WIN32` path failed every TU.
- **Watch on PS2:** `_X86_` gives the x86 `CONTEXT`/`NT_TIB` layouts on MIPS, which is what the code
  reads.

## 2026-11-17: Phase 1: one code path for every port target

- The first full compile failed in two TUs on `d2_platform.h`'s x86 SEH dispatcher: inline asm that
  calls `_RtlUnwind@16`, a PE-only spelling the ELF assembler rejects. It's enabled by `__i386__`.
- A global `#undef __i386__` in the prelude doesn't work: mingw's `winnt.h` `#error`s without an
  architecture and its `Interlocked*` intrinsics disappear.
- Decision: `Redivivus/port/include/x86intrin.h` interposes the one header `d2_platform.h` includes
  between the SDK headers and its x86-only blocks. It forwards to the real header, then retires
  `__i386__`. SDK headers keep their architecture; everything after (the SEH block, the tree's
  `__i386__` paths) takes the generic paths the PS2 build takes, so Phase 1 tests PS2 code paths on
  an easy-to-debug host. Calling-convention attributes are fixed earlier by the prelude, so the i386
  ABI is unchanged. A per-thread `%fs` TIB is no longer needed and was removed.

## 2026-11-17: Phase 1: the LoD main menu renders on i686 Linux

`build-linux/d2 -w` (the generated tree unchanged + `Redivivus/port/`) boots in the Phase 1 container,
opens all 11 LoD MPQs through the virtual `C:\Diablo II`, creates its 800×600 window and DIB section,
and presents the animated main menu through D2Gdi → GDI shim → SDL (headless frame dumps, stable for 4
minutes). Steps and findings, in order:

- **Link:** 162 undefined symbols, all resolved: 116 real, 42 stub-ok (no counterpart on port
  targets), 4 stub-todo (DirectSound, FindFirstFileW). `docs/port/symbols.tsv` has each one with its file.
- **The link-name trap:** Win32 functions that share a name with a host-library function (Winsock vs
  BSD sockets: `socket`, `select`, `send`, `gethostbyname`, …; also `rand`, `fopen`) never showed up as
  undefined. The linker bound the game's stdcall calls to glibc's cdecl functions with other structures.
  The first crash (`NET_GetLocalIp`) came from this. Fixed by defining them in the shim; MSVC `rand`
  (own LCG, `RAND_MAX` 0x7FFF) and path-translating `fopen` come in through `msvcrt.h`.
  **Guard:** `tools/port/check-imports.sh` runs post-link and fails on any host import not in
  `docs/port/host-imports.txt`. It caught a mis-typed `select` immediately.
- **The reverse trap:** SDL's own `socket()` call bound to *our* stdcall `socket` (a non-PIE executable
  exports what shared libraries reference) and smashed the stack inside SDL init. Fixed with
  `-fvisibility=hidden` for game, shim and backend; check-imports now also fails on exported symbols.
- **Storm needs Windows' VirtualAlloc guarantees:** (1) reservations are cheap: zero-filling a whole
  reservation up front stalled Storm's async I/O worker for minutes under qemu. Now `hs_vm_reserve`
  (lazy mmap), decommit = zeroed pages. (2) Reservations are 64 KiB-aligned: `SMemFree` finds a heap's
  descriptor from the top 16 bits of a block address (`uLink << 16`), so 4 KiB alignment crashed.
  **PS2 note:** there's no virtual memory there; Storm's reservation sizes will need tuning to real RAM.
- **Handles:** the game passes non-window HWNDs (e.g. `GetDesktopWindow()`); window handles are
  validated by registry membership, never by dereferencing.
- **Diagnostics built on the way:** symbolized crash/long-wait/critical-section stacks
  (`tools/port/symbolize.sh`), frame-pointer walking from the signal context (glibc `backtrace()` fails
  inside handlers under qemu), stack scanning for frames interrupted in libraries, all-thread sampling on
  SIGINT, `HS_TRACE`/`HS_TRACE_WAITS`. Non-PIE link so addresses are file addresses (as on the PS2).
- **Not yet ported from upstream's `d2_patch.cpp`:** the LRU single-node unlink fix (Blizzard's bug; its
  callers are in the same TU, so `--wrap` can't reach them; it needs a weakened-symbol override),
  the `R2109` version stamp, and the Patch_D2/levels check.
- **Watch:** Storm's lists treat negative pointers as terminators (`(intptr_t)p <= 0`), which assumes
  every node is below 2 GB. That holds here (stack 0x408…, heap low) and on the PS2, but anything
  that moves allocations above 0x80000000 would break it.

## 2026-11-17: Phase 1: in game, Rogue Encampment (Act 1), on i686 Linux

`tools/port/scripts/rogue-encampment.txt` drives the build headlessly: main menu → Single Player → new
**LoD** Barbarian "Hellstation" → OK → Rogue Encampment, next to Warriv, with rain, the campfire and the
full HUD. It stayed stable in town for thousands of frames (14,600 presented frames in the run).

- **Scripted input:** `HS_INPUT_SCRIPT` steps keyed to the presented-frame count (deterministic at any
  host speed), injected through the same event path as real input. This is the smoke test and the
  future controller harness.
- **`D2.LNG`:** the game loads its language module as a data file for accelerators and error strings,
  both of which have fallbacks. The owner's 1.14d bundle has no `D2.LNG`, and port targets have no PE
  resources anyway; `LOAD_LIBRARY_AS_DATAFILE` loads now yield a resource-less module.
- **First generator defect found:** `PALETTE_LoadFromFile` computes the end of its path buffer from
  another local's address (a VC8 frame-layout artefact), so under GCC entering a game halts (Fog line
  57). An upstream issue is drafted in `docs/port/upstream/palette-frame-artefact.md`.
- **Overrides mechanism** (`HS_PORT_OVERRIDES`, `docs/port/overrides.md`): weaken the generated
  definition in the module archive (`objcopy`), rename and globalize the file-statics it needs, and link
  the hand-written replacement as objects into the executable. A per-module stamp source makes the table
  a build input. The palette fix and upstream's LRU single-node unlink fix (portable now: no x86
  `CALL` rewriting) both use it.

## 2026-11-17: Phase 2: the game builds for the PS2 and boots in PCSX2 (blocked in OpenPS2BIOS)

- **Compile:** all 476 TUs for `mips64r5900el-ps2-elf` (GCC 15.2, n32 ILP32) with the same prelude.
  On non-x86 the SDK headers are shown `__i386__` (the x86 layouts the game was decompiled against)
  and the interposer retires it. x86 asm bodies the SDK and `d2_platform.h`'s early part define
  (`%fs` TEB read, INT3, CPUID) are renamed aside and restored as portable definitions; `NtCurrentTeb`
  is a fake TEB/PEB with an empty module list. `time_t` is the C library's; `int32_t` is forced to
  `int` for game code (EE GCC makes it `long`, which collapses `d2_platform.h`'s overloads); RDTSC is a
  counter; `d2_crt_impl.cpp`'s x87 `__ftol2` is excluded (declared, never called).
- **Link:** `cmake/ps2-ee.cmake` on ps2dev's own toolchain file minus `-O2`, plus the WHOLE_ARCHIVE
  link feature CMake lacks for "Generic". The R5900 has no LL/SC, so `__sync_*`/`__atomic_*` are
  library calls; `backend/ps2/atomics.cpp` implements them with interrupts masked (one core).
- **Size:** first link was 27.3 MB, mostly 16.6 MB of `.rodata`. `d2_enums.h` gives every TU its own
  copy of thousands of namespace-scope `static const` constants. `-ffunction-sections
  -fdata-sections --gc-sections` (all port targets) cut the image to **10.3 MB** (text 6.7 MB, rodata
  0.2 MB, bss 2.5 MB), leaving about 21 MB of a stock 32 MB PS2 for the heap. No Extended RAM needed so
  far (and OpenPS2BIOS mis-places the stack at 0xFFFF8000 when it's enabled).
- **Backend:** `backend/posix` (files, threads) is now shared by Linux and the EE (newlib + ps2sdk
  pthread glue); `backend/ps2` adds gsKit PSMT8+CLUT presentation, logging and the atomics.
  Data reaches the EE through PCSX2's `host:` (needs `[EmuCore] HostFs = true`).
- **Debugging without a console:** OpenPS2BIOS doesn't route EE printf, and file I/O is unavailable
  during static construction (fio is set up by a constructor). `hs_log` writes into a 64 KiB RAM
  ring, and `tools/port/pine-log.py` reads it out of a paused/crashed PCSX2 over PINE. abort/terminate
  log their callers; `--wrap=malloc…` logs large allocations with callers.
- **Found with those:** (1) the game asks `CreateThread` for multi-MB stacks (virtual on Windows, real
  RAM on the EE); EE thread stacks are fixed at 1 MiB. (2) **The link-name trap, EE edition:**
  ps2sdk's kernel `CreateThread(ee_thread_t*)` was bound to our Win32 `CreateThread` (pthread glue's
  thread creation landed in Win32 code), and likewise for Exit/Resume/Suspend/TerminateThread, the
  BSD socket calls and newlib's internal `_mkdir`. `include/hs_ps2_renames.h` renames them (`hs_w32_`)
  for game and shim code; `tools/port/ps2-collisions.sh` runs post-link and fails on any new collision.
- **Blocked:** static construction now gets as far as the game's own threads, then OpenPS2BIOS halts
  with "unsupported syscall=0161ed98" at `SignalSema` (ps2sdk's stub is `li v1,66; syscall`).
  OpenPS2BIOS reads `v1` correctly, so `v1` was changed between those two instructions: an interrupt
  (or a reschedule from one) returning without restoring `v1`. This is a BIOS-level fault, reportable
  to TamaribaEmu/OpenPS2BIOS. Play! 0.72 loads the ELF but shows nothing and doesn't map `host:` to the
  ELF's folder.

## 2026-11-17: Phase 2: the PS2 build reaches the game's init, opens every MPQ, loads its first string table

With the owner-selected Sony BIOS (SCPH-70004 v2.00) in PCSX2. Found and fixed, in order:

- **Static-initialisation order:** ps2sdk's `PTHREAD_MUTEX_INITIALIZER` is `(ptr)-1`, not a constant
  expression, so every `std::mutex` in the shim is initialised at run time by its file's static
  constructor, *after* upstream's game initialisers (which run first) have already called
  `VirtualAlloc`. Zero-mutex → `EINVAL` → `std::system_error` → libstdc++'s exception pool looped
  until the heap was gone. Glibc's initializer is all-zero, which hid this on Linux. Shim locks are
  now `HsLock` (constant-initialised, creates its mutex atomically on first use); file-scope shim
  objects the game can reach early are function-local.
- **Kernel semaphore exhaustion:** the EE kernel has 256 semaphores; ps2sdk's pthread mutexes take one
  each, and the game creates hundreds of critical sections at start-up (356 semaphores requested).
  Failed creations and double deletes left libkernel's priority-0 helper thread spinning, which
  starved `main` (the EE never time-slices). EE locks are now semaphore-free: owner id +
  compare-and-swap, with waiters sleeping briefly; condition waits sleep in slices and re-check.
- **Critical sections the shim didn't create** (zeroed or copied by the game) are no longer destroyed
  on `DeleteCriticalSection` (tagged in `SpinCount`).
- **`-w` by default** on the EE: without it the game looks for a Direct3D device and exits.
- **`_sbrk`:** replaced (fixed heap end read once on the main thread); `HS_PS2_EXTENDED_RAM`
  continues the heap at 0x02000000-0x08000000 for PCSX2 Extended RAM bring-up.
- **Diagnostics:** wrappers for `__cxa_throw` (type + thrower), pthread mutex failures, and a
  call-free recorder for kernel `CreateSema`/`DeleteSema` (runs before libc exists), read over PINE.

**Where it stands:** `WinMain("-w")` runs; every LoD MPQ opens through `host:`; the 800×600 window and
DIB section are created; `string.tbl` (from `d2data.mpq`) loads and passes its CRC. Then loading the
next table (`patch_d2.mpq` / `d2exp.mpq`) returns NULL before any allocation, while Linux loads all
three. Next: why Storm can't find or decompress files in those archives on the EE. Suspect: an
unaligned read in a decompressor or the hash/block table code (PCSX2 doesn't fault on these).

**Memory at this point:** image 10.3 MB + ~19.7 MB of Storm/Fog heap reservations before the main
menu. On the EE every reservation is real RAM; fitting 32 MB means tuning those pools (Phase 3).
`docs/port/overrides.md` notes the temporary `STRTABLE_CalculateCRC` diagnostic override.

## 2026-11-17: Phase 2 milestone: the LoD main menu on an emulated PS2

PCSX2 2.8.2 (owner's SCPH-70004 BIOS, Extended RAM for bring-up), `d2.elf` from this tree: the 1.14d
LoD main menu renders through D2Gdi → GDI shim → gsKit PSMT8+CLUT, animated (snow, torches), at 25
FPS with emulation at 100% speed, PAL 640×512. Data read from the owner's MPQs over `host:`.

The last blocker was a **second generator defect**: `Storm::SMem::SMemCalculateAllocationLayout`
emits `(n ? 7 : 0) + n` for the original `(n + 7) & ~7`, so every Storm heap block is 15 bytes over its
request and none is 8-aligned. Storm's archive records landed at odd addresses (0x…2F3); on MIPS their
32-bit fields are unaligned loads (PCSX2 returns wrong data rather than faulting), so every lookup
except in the first archive failed (`patchstring.tbl` "not found"). The function is static and
reached through its section, so it can't be redirected at link time: on MIPS its entry is patched at
the first `SMemAlloc` to jump to a corrected copy (`overrides/storm_align.cpp`), and `SMemAlloc` is
wrapped to round requests (new `HS_PORT_WRAPS` mechanism: rename the definition in one object,
interpose everywhere else). x86 doesn't care about alignment, which is why Linux never saw it.

Memory: the heap ran past 32 MB into Extended RAM before the menu (DC6 buffers at 0x23…). Fitting a
retail PS2 means trimming Storm/Fog pool reservations (~20 MB at the menu) and the 6.7 MB of
unoptimised code: Phase 3.

## 2026-11-17: in game on PS2; -O0 was the frame-rate problem

- The Rogue Encampment smoke script now runs on the EE as well (`run-pcsx2.sh --script`, staged as
  `host:input.txt`). The script player moved to `Redivivus/port/input/script.cpp`, shared with SDL.
- Controller layer 0 is in: `input/virtual_mouse.cpp` (neutral) + `backend/ps2/pad.cpp` (rom0
  SIO2MAN/PADMAN, libpad). Left stick coarse cursor, right stick fine, ✕/R2 left click, ○/L2 right
  click, Start Esc, △ I, □ S, R1 C, Select Tab, L1 Shift, L3 R, R3 Alt, D-pad belt 1-4. The pad
  comes up digital (mode 4); analogue is re-requested every 30 reads until it reports mode 7.
- Measured in PCSX2 (Extended RAM, EE at 100% speed), standing in town next to Warriv:
  - default build (no -O, -g): 10 FPS;
  - `-O2 -fno-inline -fno-ipa-{cp,sra,icf,modref,pure-const} -fno-strict-aliasing -fwrapv
    -fno-delete-null-pointer-checks`: 25 FPS, the game's own cap.
- Plain -O2 doesn't link: inlining removes the statics the override mechanism weakens and patches
  (SMemCalculateAllocationLayout, LRUCACHE_PopOldest), and template members a TU instantiated only
  inline are never emitted for other TUs (TSHashTable). No inlining and no IPA cloning keep every
  function a real, patchable symbol. The other flags cover UB the decompiled code may lean on.
- In-game hs.log cost: Storm reserves 64 KiB blocks continually (4,247 log lines per run), each
  written to host: one byte per syscall. Reserve logging is now per 4 MiB of growth, and log writes
  go one per contiguous ring run.

## 2026-11-17: units teleporting in town = PCSX2 recompiler LWR bug

- Symptom: NPCs and the player sometimes covered a whole walk (4-11 subtiles) in one frame, at
  the same spots every time. Linux x86, same script: 0 jumps. PS2: 43 per run.
- Chain, each link measured: no server corrections (packet 0x15 never fired); frame pacing clean
  (1 server tick per frame); the jump is one `PATH_AdvanceUnitAlongPath` step;
  `PATH_ComputeDirectMovementVector` decided the target was within one step and set the velocity
  to the whole remaining distance (seen as `DRLGPATH_NormalizeFixedPointPair` input (0x80000,
  -0x30000)). Its velocity fields sit at unaligned offsets in packed `D2DynamicPathStrc` (0x72,
  0x76) and load through LWL/LWR, then `slt` compares 64-bit registers. Every snapping walk had a
  negative velocity component.
- Probe (`backend/ps2/lwlr_probe.cpp`, packed int -4263): recompiler `lwl;lwr` gives
  0x00000000ffffef59, interpreter gives 0xffffffffffffef59. `lwr;lwl` and `lwl;lwr;sll 0` are
  correct on both. PCSX2's EE recompiler zeroes the upper half on a non-aligned LWR.
- Fix: every EE object goes through `tools/port/ee-cc-launcher.sh` -> `ee-lwr-fixup.py`, which
  appends `sll rt,rt,0` after each LWR (a delay-slot LWR is hoisted above an ordinary branch;
  branch-likely or a conflict fails the build). After: 0 snaps, 1 jump (the spawn, as on Linux).
- Real hardware is expected to be unaffected (interpreter follows MIPS III) but is unverified.
  Draft report: `docs/port/upstream/pcsx2-lwr-sign-extension.md`.
- `-mstrict-align` doesn't stop GCC emitting LWL/LWR for the R5900.

## 2026-11-17: EE build optimised by default

- `cmake/ps2-ee.cmake` now builds `-O2 -g -fno-inline -fno-ipa-{cp,sra,icf,modref,pure-const}
  -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks` (was: no -O). Town in PCSX2: 10 FPS
  -> 25 FPS (the game's cap), steady 40 ms frames.
- The investigation's diagnostics (pacing trace, warp/jump detector, path-step wraps, function
  instrumentation, a pre-empted D2PathPointStrc that turned out not to matter) are removed. The LWR
  probe stays as a one-line start-up `CPU check:`.
- `-DHS_EE_LWR_FIXUP=OFF` rebuilds without the fix-up, to reproduce the bug.
- Correction to the layer-0 bindings above: □ sends T (the skill tree in 1.14d); S is the skill speed
  bar. Fixed in `input/virtual_mouse.cpp`.

## 2026-11-17: Redivivus as a snapshot import

- History restarted from one commit importing jaenster/redivivus at dd289669c82a (credited there,
  in the README and in LICENSE); HellStation's commits replay on top, tree unchanged.
- Dropped upstream's project scaffolding (CI, issue templates, plugin config, byte-match oracle,
  progress tracking, citation, contributing guide, its README and build docs); `UPSTREAM` lists
  them so syncs skip them. The build needs none of them.
- Upstream changes now arrive through `tools/sync-redivivus.sh` (three-way apply of the upstream
  diff since `UPSTREAM`'s revision); the pre-commit hook accepts generated-code changes only with
  `HS_UPSTREAM_SYNC=1`.

## 2026-11-18: first-boot diagnostics removed (#12)

- Linux town smoke re-verified first (`run-linux.sh --script tools/port/scripts/rogue-encampment.txt`,
  tree at `dea0a2cb`): new LoD Barbarian in the Rogue Encampment next to Warriv, rain, campfire and
  HUD, 17,040 presented frames in 900 s under qemu with no halt or crash.
- Removed: `overrides/diag_d2lang.cpp` (`STRTABLE_CalculateCRC` logging), `overrides/diag_d2hell.cpp`
  (`D2FILE_AllocAndLoadD2File` logging, same body as the generated one; plus the
  `SFileOpenArchive` wrap), and `backend/ps2/alloc_trace.cpp` (the malloc/calloc/realloc/memalign,
  `__cxa_throw`, pthread mutex and `CreateSema`/`DeleteSema` link wraps), with their entries in
  `cmake/port.cmake` and `docs/port/overrides.md`. Logging of large allocations goes with them; #5
  measures the heap on its own terms.
- After removal: the generated `STRTABLE_CalculateCRC` and `D2FILE_AllocAndLoadD2File` are strong
  definitions again in both port builds, and `d2.elf` has no `__wrap_` symbols. Linux smoke: same
  result (town, 14,400 presented frames in 900 s, no halt). PCSX2 `town-walk.txt`: all 13 steps
  (town, walks, inventory open/close), `CPU check` line present, 40 ms frames standing in town
  (0 frames over 50 ms per 250); hitches of up to 180 ms while walking and opening the inventory
  (125 frames over 50 ms in frames 3500-3750).

## 2026-11-18: memory measured at the menu and in town (#5, part 1)

**Tooling.** A `mem` line every 250 presented frames (`win32/system.cpp`, via `win32/memstats.h`),
and one line per Fog pool manager every 500 (`port/fog_pools.cpp`, which reads Fog's
`pGlobalPoolSystem` block table and overflow buckets). Fields, in KiB, `now/peak`:
`crt` is the C heap (taken from the program break, live malloc bytes, and the retail limit);
`vm` is VirtualAlloc regions, reserved and committed (high-water per region), split by how they
were asked for: `reserve` (MEM_RESERVE alone: Storm's heaps), `commit` (MEM_COMMIT alone: Fog's
pool blocks), `both` (lookup tables); `w32` is HeapAlloc & co. On the EE every VirtualAlloc region
is real RAM inside the C heap. Backend side: `hs_mem_query` (`platform.h`). The old cumulative
"total reserved" log only counted up (300 MB by the town) and is gone.

**Retail budget.** `_end` is 0x842080 with the `-O2` build: image 7,432 KiB (text+data+bss from
0x100000; it was 10.3 MB at -O0). 32 MiB less the 1 MiB kernel area, the image and the 128 KiB main
stack leaves the heap **24,184 KiB**.

**Measured** (PCSX2, Extended RAM, `town-walk.txt`; KiB):

| | Main menu (f2500) | Rogue Encampment (f4500) |
|-|-|-|
| C heap in use | 28,168 | 46,889 |
| C heap taken (peak) | 32,917 | 64,201 (60,293 with the alignment change below) |
| Fog "Client Pool System": committed / slots in use | n/a | 18,304 / 14,473 (79%) |
| Fog "Global Pool System": committed / slots in use | 8,128 / 4,343 (53%) | 7,424 / 5,564 (74%) |
| Fog overflow (straight to malloc) | 6,847 in 16 allocs | 7,301 in 2,940 allocs |
| Storm heaps: reserved / committed | 1,856 / 232 | 2,240 / 284 |
| Other commit regions (not in a live Fog block table) | 4,804 | 4,804 (same at boot: unattributed) |
| Other C heap (3 game threads x 1 MiB stack, DIB, gsKit texture, Fog block headers, libc) | ~6,900 | ~6,900 |

- **The town needs about twice the retail heap.** Live allocations alone are 46.9 MB against
  24.2 MB; Fog's slots in use plus its overflow are 27.3 MB, over the budget with zero waste.
- **The Client pool is the working set** (sprite and tile caches for the area): 14.5 MB of
  power-of-two slots. The menu's big overflow allocs are its full-screen DC6 backgrounds
  (477 KiB each). The town's 2,940 overflow allocs are mostly data tables.
- **The game doesn't size anything from RAM:** `GlobalMemoryStatus` feeds only the FPS overlay and
  the out-of-memory log (D2Client `DRAW_UI_FPSStats`, Fog `OutOfMemoryHandler`).
- **Taken vs in use:** about 14 MB in town is free space malloc holds, either fragmentation or
  the town-load spike; the current counters can't tell which (peak in-use isn't tracked).

**Change (an experiment that's kept):** VirtualAlloc gives 64 KiB alignment only to MEM_RESERVE-alone
regions (Storm, which finds a heap's descriptor from an address's top 16 bits); the rest get page
alignment (Fog finds blocks by binary search over start addresses, `FindBlockContainingAddress`).
Peak taken in town: 64,201 -> 60,293 KiB. Town walk and inventory still run, 25 FPS.

**Verdict so far: LoD vs classic isn't decided by this.** The memory is in the area working set
(Client pool) and data tables, which a classic town also needs; classic itself is not measured.
Either target needs real memory work (cache caps, table trimming, streaming) to fit 24 MB. Still to
measure: Act 5 town and a busy fight.

## 2026-11-18: does it fit in 32 MB? Feasibility check (#5, part 2)

- **Stock RAM today:** PCSX2 without Extended RAM runs out of heap during start-up (break at
  0x1F7B000), before the first menu frame; the game exits cleanly with Fog error 8.
- **The game sizes its caches from `GlobalMemoryStatus`** (D2Client `UI/Draw.cpp`): avail = total
  - 11 MB; sound cache 10% (3-5 MB); a 6 MB tile LRU; the sprite cache gets a third of the rest,
  clamped to 24-64 MB; below 64 MB avail it also turns on "sprite cache conversion" (D2CMP
  `GFX_InitCelDataCache`). It refuses to start under 22.5 MB. The shim said 256 MB (sprite limit
  64 MB). The EE now reports 32 MB (`hs_mem_info.phys_total`), a retail PS2 even under Extended
  RAM. Town: in use 46.9 -> **41.8 MB**, peak taken 61.1 -> **54.0 MB**, Client pool committed
  18.3 -> 13.2 MB; sprite cache 5.2 MB used of its 24 MB limit. Town walk and pacing unchanged.
- **Client pool by slot size (before the change):** 8.3 MB in 64-256 KiB slots, 5.4 MB in
  8-32 KiB slots: decoded sprites and tiles. Fog rounds every request up to a power of two.
- **Heap cap (`-DHS_PS2_HEAP_LIMIT_KB`) at 46 MB:** the menu is fine, the town load fails
  (a 512 KiB Fog block). The 54 MB town-load peak is needed as the game loads today, not idle
  fragmentation.
- Also fixed: `run-pcsx2.sh` edited PCSX2's settings before closing the old instance, which then
  wrote its own settings back (Extended RAM stuck off); and it appended a `HostFs` line per run.

**Where that leaves it:** town peak 54 MB (41.8 MB live) against a 24.2 MB heap: 2.2x. The
levers found, with rough sizes: sprite and tile cache caps (~3 + ~3 MB in town, more in fights),
thread stacks (~2 MB), Fog pool slack and power-of-two rounding (~4-7 MB, needs replacing Fog's
allocator), the town-load spike (up to ~12 MB, cause not yet found), data tables in Fog overflow
(7.3 MB, partly trimmable), a smaller image (-Os, ~1 MB), 4.8 MB unattributed. Closing the gap
needs nearly all of them, several invasive, and smaller caches cost EE time re-decoding sprites.
Act 5 and fights (not measured) will need more than the town.
- **Classic not measured:** with only the classic archives (no `d2exp.mpq`/`d2x*.mpq`, links in a
  scratch folder) the EE halts before the menu in `Bnclient::GetBattlenetRealmsList`
  (`BNGatewayAccess.cpp:221`, Fog halt line 392), a path the LoD set-up doesn't take. Unfixed.

## 2026-11-18: the bounded attempt at 32 MB (#5, part 3)

Gate set beforehand: the town walk loads with the C heap capped at 35 MB. Result: **missed by
about 2.4 MB** (fails mid-load at 33.2 MB live; uncapped peak 37.4 MB). Town peak over the day:
61.1 -> 37.4 MB taken.

| Change | Town in use | Peak taken |
|-|-|-|
| Start (game told 256 MB) | 46.9 MB | 61.1 MB |
| Tell the game 32 MB | 41.8 | 54.0 |
| Two-ended heap (regions from the top, malloc from the bottom) | 41.8 | 44.4 |
| Thread stacks 1 MiB -> 128 KiB | 39.1 | 41.5 |
| Sprite cache capped at 2 MB (was 5.2 MB used of 24) | 35.1 | 37.4 |

- **Fragmentation was the "load spike":** at a 46 MB cap the town failed with only 34.7 MB live; free
  space was in pieces smaller than Fog's 512 KiB block. `backend/ps2/memory.cpp` now carves
  VirtualAlloc regions down from the top of RAM with their own coalescing free list, and malloc's
  break grows up from `_end`. Without `HS_PS2_EXTENDED_RAM` the top stops at a retail 32 MB.
- **Thread stacks:** ps2sdk's pthread glue ignores `pthread_attr_setstack` (it mallocs its own);
  each thread now paints its stack (bounds from `ReferThreadStatus`) and the report shows the
  deepest use: 16 KiB (the unpainted margin) over the town walk. 128 KiB each.
- **Sprite cache cap** (`overrides/sprite_cache_cap.cpp`, a wrap of D2CMP `GFX_InitCelDataCache`,
  only when the backend reports <= 32 MB). Cost: standing in town went from 0 to 22-28 frames over
  50 ms per 250; walking peaks at 220 ms.
- **The 4.8 MB "unattributed" regions are Storm's large blocks** (`SMEM_AllocateHeapBlock`): mostly
  the MPQ hash and block tables, 3,886 KiB for the 11 archives (read from their headers);
  `d2data.mpq` and `d2exp.mpq` each have 65,536 hash slots for about 10,000 files.
- The tile LRU (6 MB limit) is 18% used in town and allocates as it fills: capping it saves nothing
  there.

**Levers left, rough sizes:** Storm heaps reserve 2.2 MB and commit 0.3 MB (~1.9 MB); MPQs repacked
with right-sized hash tables (~1.5 MB) and music/video archives opened on demand (~0.4 MB); Fog
pool slack, 3.5 MB in the Client pool alone with the cache cap (allocator replacement); the 7.3 MB
of data tables in Fog overflow (unknown how much is trimmable); a smaller image (~1 MB). From
37.4 MB to the 24.2 MB budget is 13.2 MB, before sound (its cache is 3-5 MB) and before Act 5 and
fights, which are not measured.

## 2026-11-18: Act 5 measured; the project stops

- **Harrogath, Hell**, with a level 96 Sorceress (a save downloaded from a community armory, kept
  out of the repo; its start byte set to Hell Act 5 and the header checksum recomputed). With all of
  the day's cuts: **peak taken 40.3 MB, 38.1 MB live** after walking around town. The same build
  peaks at 37.4 MB in the Rogue Encampment. **15 FPS, every frame over 50 ms**, at 100% emulated
  speed. A walk out of town to a fight didn't arrive (the scripted walk stuck on a building), so
  fights are not measured.
- The two "cheap" cuts proposed weren't: Storm's 64 KiB heap windows are fixed (`SMEM_AllocateHeap`
  reserves 0x10000, its block sizes are 16-bit) and a failed commit inside one returns NULL to its
  callers. Shrinking an MPQ's hash table needs every file name, which the archives may not carry.
- **Decision (owner):** stop. Act 5 needs ~16 MB more than a retail PS2's 24.2 MB heap after every
  cheap cut, before sound (3-5 MB) and fights. The remaining levers (Fog's allocator, the data
  tables, the MPQ tables) roughly match the gap with no margin. The frame rate is already 15 FPS
  in Act 5, and memory cuts so far have cost speed. **LoD vs classic** was not decided by
  measurement (the classic-only data set halts before the menu); the memory is in area graphics
  and tables, which classic also needs.

## 2026-11-18: picked up again as a long shot; 640x480 in game

The owner decided to keep going after all, aiming at the original goal (a disc in a retail PS2),
low-risk levers first.

- **640x480 in game** on small-RAM targets: the shim's registry seeds `Resolution = 0` (D2Client
  `UI/EscMenu.cpp`: 0 = 640x480, 1 = 800x600, the default) when the backend reports <= 32 MB. The
  menus stay 800x600 (LoD). Rogue Encampment: peak 37.4 -> 36.8 MB, standing back to 0 frames over
  50 ms per 250 (was 22-28 with the sprite cap), walking 6-16 (was 22-137). **Harrogath (Act 5,
  Hell): 1 frame over 50 ms per 250, about 23-25 FPS, against every frame over 50 ms at 800x600**;
  peak 37.0 MB. (The 800x600 run had a panel open and had walked; the comparison is close, not
  exact.) Mostly a speed win; memory barely moves.
- **-Os, tried and dropped:** text 4,877 -> 4,315 KB (-562 KiB; the build already doesn't
  inline). The -Os build behaves differently: opening character select takes 14.5 MB more than
  the -O2 build (39.7 vs 28.5 MB in use) and its OK button does nothing. Likely undefined behaviour
  in decompiled code that -Os exposes; not worth half a megabyte.
- bss is 2.5 MB of the game's static state, spread over many tables of 256 KiB or less: no cheap
  target.

**Where it stands:** Act 1 and Act 5 towns peak at about 37 MB against a 24.2 MB heap, at full
speed in both. The levers left are the medium and high ones in the entries above.

## 2026-11-18: Fog's pools exact-size above 256 bytes (#5, first medium lever)

`overrides/fog_exact_alloc.cpp` wraps the seven Fog memory entry points the game imports
(`Alloc`, `AllocClientMemory`, `AllocServerMemory`, `ReAllocServerMemory`, `FreeClientMemory`,
`FreeServerMemory`, `FreeMemoryPool`). On a target reporting 32 MB or less, requests over 256 bytes
get exactly their size from malloc behind a 16-byte header. Each header sits on its pool manager's
list, so `FreeMemoryPool` frees them along with Fog's blocks. Smaller requests, which pack densely,
stay in Fog's pools. Frees and reallocs recognise a header by a check word tied to its address.
Managers with a pool id (data tables during loading) keep Fog's own malloc path. Linux and Windows
are unchanged.

| Peak taken (MB) | Before | After |
|-|-|-|
| Rogue Encampment, existing character | 36.8 | **27.5** |
| New-character screen (every class's animations) | not separated | 34.1 |
| Harrogath, Act 5 Hell, level 96 character | 37.0 | **28.2** |

- **Fog's Client pool held 13.2 MB of blocks in town; now 0.2 MB, plus 8 MB exact-size.** The
  pools' cost was rounding up to a power of two and half-empty 64/512 KiB blocks, as measured.
- **Speed unchanged:** Rogue Encampment and Harrogath at 25-26 FPS, 1-2 frames over 50 ms per 250.
- **Alignment:** every block malloc gave was 16-byte aligned (the report counts any that aren't).
- **Load-time allocations, by source line** (a temporary log, removed): whole-file reads
  (`.\SRC\Archive.cpp:207`, 14.5 MB over 64 calls, up to 575 KiB each, the menus' DC6 backgrounds
  among them) and sprite decoding (`.\SRC\Codec.cpp:1377`, 4 MB over 65). The new-character screen
  has 22 MB of them live at once; it's now the highest point of a session.
- Not checked: whether the game relies on fresh pool slots being zero (malloc doesn't zero; Fog's
  first use of a block did). Nothing misbehaved in either town.

**Where it stands:** Act 1 and Act 5 towns peak at 27.5 and 28.2 MB against a 24.2 MB heap: 3-4 MB
short, before sound and fights. The new-character screen needs about 10 MB more than that. Next:
the MPQ tables (3.9 MB), and the character-creation spike.
