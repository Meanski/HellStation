#!/usr/bin/env bash
# Compile individual TUs with the port's game flags (fast iteration without a
# full build). Run inside tools/port/linux-i386.sh.  tools/port/check-tu.sh <file.cpp>...
set -uo pipefail
find Source 3rdparty/Storm 3rdparty/crashy -name '*.h' -printf '%h\n' | sort -u | sed 's/^/-I/' > /tmp/hs-incs.rsp
echo "-I." >> /tmp/hs-incs.rsp
rc=0
for f in "$@"; do
  out=$(i686-linux-gnu-g++ -std=c++17 -c -o /dev/null -w -fms-extensions -fwrapv -fshort-wchar \
    -D_WIN32 -DHELLSTATION_PORT -IRedivivus/port/include -idirafter /usr/i686-w64-mingw32/include \
    -include Redivivus/port/prelude.h -include Source/D2Headers/d2_platform.h -include Redivivus/d2_hand.h \
    @/tmp/hs-incs.rsp "$f" 2>&1)
  n=$(printf '%s' "$out" | grep -c "error" || true)
  printf '%-60s %s\n' "$f" "$([ "$n" = 0 ] && echo ok || echo "$n errors")"
  [ "$n" = 0 ] || { rc=1; printf '%s\n' "$out" | grep -E "error" | head -${HEAD:-8}; }
done
exit $rc
