#!/usr/bin/env bash
# Turn the HS-FRAME lines of a crash log into function and file:line.
#   tools/port/symbolize.sh <log> [binary]   (run inside tools/port/linux-i386.sh)
set -euo pipefail
LOG=$1; BIN=${2:-build-linux/d2}
# Each "HellStation backtrace"/"fatal signal" header starts a new stack.
awk '/HellStation backtrace/ {print "== " $0; next} /HS-FRAME/ {print $2}' "$LOG" | while read -r a b; do
  if [ "$a" = "==" ]; then echo "$a $b"; else
    i686-linux-gnu-addr2line -f -C -p -e "$BIN" "0x$a" | sed 's|/src/||; s| (discriminator [0-9]*)||'; fi
done
