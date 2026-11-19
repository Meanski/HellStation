#!/usr/bin/env bash
# Drive a running PS2 build from the host: append input steps to the run
# folder's live.txt, which the game reads about once a second
# (Redivivus/port/input/input.h has the step format).
#   tools/port/live.sh [--build build-ps2] <step>...
# Each step is a quoted line such as "click 320 230", "click 400 300 right" or
# "key 0x09"; it runs 5 frames after the game reads it, and each following one
# 15 frames later. A step that starts with "+N" keeps its own offset, counted
# from when the game reads the file (not from the previous step), so give
# cumulative offsets: "+5 move 300 40" "+17 click 300 40 right" "+45 ...".
# Hover is resolved while drawing: move first, click in a later frame, or the
# click misses objects. "where" logs nearby units with their click positions.
#   tools/port/live.sh "click 320 230" "key 0x09"
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=build-ps2
if [ "${1:-}" = "--build" ]; then BUILD=$2; shift 2; fi
LIVE="$ROOT/$BUILD/run/live.txt"
[ -e "$LIVE" ] || { echo "no $LIVE: start the game first (tools/port/run-pcsx2.sh, or run-linux.sh --build ...)" >&2; exit 1; }
offset=5
for step in "$@"; do
  case "$step" in
    +*) echo "$step" >> "$LIVE" ;;
    *)  echo "+$offset $step" >> "$LIVE"; offset=$((offset + 15)) ;;
  esac
done
