#!/usr/bin/env bash
# Run the Phase 1 build (build-linux/d2) inside the container against $D2DATA.
#   tools/port/run-linux.sh [--build DIR] [--frames DIR] [--seconds N] [--script FILE] [game args...]
# Headless: SDL's dummy video driver; with --frames, every 60th presented frame
# is written to DIR as PPM (converted to PNG on exit). --seconds stops the game
# after N seconds (default: run until it exits). --build picks the build folder
# (default build-linux); its run/ folder is the C: root, run/hs.log gets the log
# (and sanitizer reports), and run/live.txt takes live input (tools/port/live.sh,
# drive.py).
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
FRAMES=""; SECS=0; SCRIPT=""; BUILD=build-linux
while [ $# -gt 0 ]; do
  case "$1" in
    --frames) FRAMES=$2; shift 2 ;;
    --seconds) SECS=$2; shift 2 ;;
    --script) SCRIPT=$2; shift 2 ;;
    --build) BUILD=$2; shift 2 ;;
    *) break ;;
  esac
done
[ $# -eq 0 ] && set -- -w
mkdir -p "$ROOT/$BUILD/run"
: > "$ROOT/$BUILD/run/live.txt"
: > "$ROOT/$BUILD/run/hs.log"
[ -n "$FRAMES" ] && { rm -rf "$ROOT/$FRAMES"; mkdir -p "$ROOT/$FRAMES"; }
exec env HS_FRAME_DUMP_EVERY="${HS_FRAME_DUMP_EVERY:-60}" "$ROOT/tools/port/linux-i386.sh" sh -c '
  export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
  B='"$BUILD"'
  export HS_ROOT=/src/$B/run HS_LIVE_INPUT=/src/$B/run/live.txt HS_TRACE=${HS_TRACE:-1} HS_TRACE_WAITS=${HS_TRACE_WAITS:-}
  [ -n "'"$FRAMES"'" ] && export HS_FRAME_DUMP=/src/'"$FRAMES"'
  [ -n "'"$SCRIPT"'" ] && export HS_INPUT_SCRIPT=/src/'"$SCRIPT"'
  if [ '"$SECS"' -gt 0 ]; then timeout -s INT '"$SECS"' ./$B/d2 "$@" 2>>/src/$B/run/hs.log; rc=$?; else ./$B/d2 "$@" 2>>/src/$B/run/hs.log; rc=$?; fi
  if [ -n "'"$FRAMES"'" ]; then for f in /src/'"$FRAMES"'/*.ppm; do [ -e "$f" ] && convert "$f" "${f%.ppm}.png" && rm "$f"; done; fi
  exit $rc' d2 "$@"
