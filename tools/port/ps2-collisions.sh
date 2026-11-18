#!/usr/bin/env bash
# Symbols that both the port (game + Win32 shim) and the EE toolchain's libraries
# define. Each one is a silent mis-link: whichever comes first wins, and ps2sdk's
# own calls (e.g. pthreads -> CreateThread) land in Win32 code. Run inside
# tools/port/ps2.sh after a build; prints C symbol names one per line.
#   tools/port/ps2-collisions.sh [build dir]
set -euo pipefail
B=${1:-build-ps2}
LIBS="$PS2SDK/ee/lib/*.a $PS2DEV/gsKit/lib/*.a $PS2DEV/ee/mips64r5900el-ps2-elf/lib/*.a $(dirname $(mips64r5900el-ps2-elf-gcc -print-libgcc-file-name))/libgcc.a"
ours=$(mips64r5900el-ps2-elf-nm --defined-only $B/lib*.a $B/CMakeFiles/hs_overrides.dir/Redivivus/port/overrides/*.obj 2>/dev/null |
  awk '$2 ~ /^[TDBRW]$/ {print $3}' | grep -v '^_Z' | sort -u)
theirs=$(for l in $LIBS; do mips64r5900el-ps2-elf-nm --defined-only "$l" 2>/dev/null; done |
  awk '$2 ~ /^[TDBRW]$/ {print $3}' | grep -v '^_Z' | sort -u)
# Our own backend is meant to define libc-level hooks (__wrap_*, atomics) and
# replaces _sbrk and memcpy (backend/ps2/memory.cpp, memcpy.cpp).
comm -12 <(echo "$ours") <(echo "$theirs") | grep -vE '^(__wrap_|__sync_|__atomic_|main$|abort$|_sbrk$|memcpy$)' || true
