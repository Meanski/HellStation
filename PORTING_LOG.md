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
