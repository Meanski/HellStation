#!/usr/bin/env bash
# Syntax-check translation units for the Linux port without building, and rank
# the errors. Run inside tools/port/linux-i386.sh.
#   tools/port/probe-compile.sh [N files per module (default all)] [out dir]
set -uo pipefail
N=${1:-0}; OUT=${2:-build-probe}
mkdir -p "$OUT"
find Source 3rdparty/Storm 3rdparty/crashy -name '*.h' -printf '%h\n' | sort -u | sed 's/^/-I/' > "$OUT/incs.rsp"
echo "-I." >> "$OUT/incs.rsp"
if [ "$N" -gt 0 ]; then
  for d in Source/*/ 3rdparty/Storm/ 3rdparty/crashy/; do find "$d" -name '*.cpp' | sort | head -n "$N"; done
else
  find Source 3rdparty/Storm 3rdparty/crashy -name '*.cpp' | sort
fi > "$OUT/files.txt"
CXX=${CXX:-i686-linux-gnu-g++}
xargs -P "$(nproc)" -I{} sh -c '
  f={}; o="'"$OUT"'/err/$(echo "$f" | tr / _).txt"; mkdir -p "$(dirname "$o")"
  '"$CXX"' -std=c++17 -fsyntax-only -w -fms-extensions -fwrapv -fshort-wchar -fmax-errors=200 \
    -D_WIN32 -DHELLSTATION_PORT -idirafter /usr/i686-w64-mingw32/include \
    -include Redivivus/port/prelude.h -include Source/D2Headers/d2_platform.h -include Redivivus/d2_hand.h \
    @'"$OUT"'/incs.rsp "$f" > "$o" 2>&1 || true' < "$OUT/files.txt"
total=$(wc -l < "$OUT/files.txt"); bad=$(grep -l "error:" "$OUT"/err/*.txt 2>/dev/null | wc -l)
echo "TUs: $total  failing: $bad"
cat "$OUT"/err/*.txt | grep -oE "error: .*" | sed -E "s/[0-9]+//g" | sort | uniq -c | sort -rn | head -${TOP:-60}
