#!/usr/bin/env bash
# Compile TUs for the PS2 EE with the port's game flags. Run inside tools/port/ps2.sh.
#   tools/port/check-tu-ps2.sh <file.cpp>...     (HEAD=n errors shown per file)
set -uo pipefail
MINGW=${HS_MINGW_INCLUDE:-/usr/share/mingw-w64/include}
RSP=/tmp/hs-incs-ps2.rsp
if [ ! -s "$RSP" ]; then
  T=$(mktemp)
  { find Source 3rdparty/Storm 3rdparty/crashy -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/'; echo "-I."; } > "$T"
  mv -f "$T" "$RSP"
fi
rc=0
for f in "$@"; do
  out=$(mips64r5900el-ps2-elf-g++ -std=gnu++17 -c -o /dev/null -w -fms-extensions -fwrapv -fshort-wchar -fvisibility=hidden \
    -D_WIN32 -DHELLSTATION_PORT -D_EE -U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int" -U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int -U__UINT_LEAST32_TYPE__ "-D__UINT_LEAST32_TYPE__=unsigned int" -I$PS2SDK/ee/include -I$PS2SDK/common/include \
    -IRedivivus/port/include -idirafter "$MINGW" \
    -include Redivivus/port/prelude.h -include Source/D2Headers/d2_platform.h -include Redivivus/d2_hand.h \
    @$RSP "$f" 2>&1)
  n=$(printf '%s' "$out" | grep -c "error" || true)
  printf '%-60s %s\n' "$f" "$([ "$n" = 0 ] && echo ok || echo "$n errors")"
  [ "$n" = 0 ] || { rc=1; printf '%s\n' "$out" | grep -E "error" | sort | uniq -c | sort -rn | head -${HEAD:-8}; }
done
exit $rc
