#!/usr/bin/env bash
# Fail if the port binary imports a host symbol not in docs/port/host-imports.txt
# (C++ runtime, SDL and pthread symbols are always allowed).
#   tools/port/check-imports.sh [binary]   (run inside tools/port/linux-i386.sh)
set -euo pipefail
BIN=${1:-build-linux/d2}
allowed=$(grep -v '^#' docs/port/host-imports.txt | tr ' ' '\n' | grep -v '^$' | sort -u)
bad=$(i686-linux-gnu-nm -D --undefined-only "$BIN" | awk '{print $NF}' | sed 's/@.*//' | sort -u |
  grep -vE '^(_Z|__cxa|_Unwind|__gxx|__|SDL_|pthread_)' | comm -23 - <(echo "$allowed") || true)
if [ -n "$bad" ]; then
  echo "check-imports: host symbols bound without review (likely Win32 names the shim must define):" >&2
  echo "$bad" | sed 's/^/  /' >&2
  exit 1
fi
# The reverse trap: anything the binary exports could capture a host library's call.
exported=$(i686-linux-gnu-nm -D --defined-only "$BIN" | awk '{print $NF}' |
  grep -vE '^(_ZNSt|_ZSt|_ZNKSt)|^(_edata|_end|__bss_start|_IO_stdin_used|_init|_fini|__data_start|data_start|_fp_hw|__dso_handle)$' || true)
if [ -n "$exported" ]; then
  echo "check-imports: the binary exports symbols host libraries could bind to:" >&2
  echo "$exported" | head -20 | sed 's/^/  /' >&2
  exit 1
fi
echo "check-imports: ok"
