#!/usr/bin/env bash
# Alignment sweep: every misaligned load or store is an address-error crash on
# a real PS2 (PCSX2 reads wrong data quietly instead), so the Linux UBSan build
# plays the PS2's path and reports each site once in <build>/run/hs.log.
#   tools/port/sanitize-sweep.sh [hunts]       (default 10)
# Needs build-linux-asan, configured with -DREDIVIVUS_PORT_SANITIZE=ON (AGENTS.md)
# and -g1 (line tables; the full debug link runs out of Docker's memory), and a
# save in build-linux-asan/run/Diablo II/Save/ with a Sorceress as drive.py
# expects (copy it from a PS2 run folder's hsroot). Runs at the PS2's 640x480
# (HS_PHYS_MB=32): load the save, Act 1 waypoint to the Black Marsh, fight.
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$ROOT"
B=build-linux-asan; N=${1:-10}
HS_PHYS_MB=32 tools/port/run-linux.sh --build $B --seconds 3600 --script tools/port/scripts/load-save-normal.txt \
  > $B/run-linux.out 2>&1 &
game=$!
# The script's last step (index 4) closes the character panel in town.
for _ in $(seq 360); do grep -q 'input script: frame [0-9]* step 4' $B/run/hs.log 2>/dev/null && break; sleep 5; done
grep -q 'step 4' $B/run/hs.log || { echo "the game never reached town; see $B/run/hs.log" >&2; exit 1; }
sleep 20
HSB=$B python3 tools/port/drive.py wp > $B/wp.out 2>&1
HSB=$B python3 tools/port/drive.py hunt "$N" > $B/hunt.out 2>&1
echo "attacks: $(grep -c '^attacked' $B/hunt.out || true); sanitizer reports: $(grep -c 'runtime error' $B/run/hs.log || true)"
grep 'runtime error' $B/run/hs.log || true
# The game runs in a container started by run-linux.sh: stop that one only (its
# command names this build's live input), not other users of the image.
docker ps --no-trunc --format '{{.ID}} {{.Command}}' | grep "/src/$B/run/live.txt" | awk '{print $1}' |
  xargs -r docker stop >/dev/null || true
wait $game 2>/dev/null || true
