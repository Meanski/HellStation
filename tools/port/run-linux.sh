#!/usr/bin/env bash
# Run the Phase 1 build (build-linux/d2) inside the container against $D2DATA.
#   tools/port/run-linux.sh [--frames DIR] [--seconds N] [game args...]
# Headless: SDL's dummy video driver; with --frames, every 60th presented frame
# is written to DIR as PPM (converted to PNG on exit). --seconds stops the game
# after N seconds (default: run until it exits).
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
FRAMES=""; SECS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --frames) FRAMES=$2; shift 2 ;;
    --seconds) SECS=$2; shift 2 ;;
    *) break ;;
  esac
done
[ $# -eq 0 ] && set -- -w
mkdir -p "$ROOT/build-linux/run"
[ -n "$FRAMES" ] && { rm -rf "$ROOT/$FRAMES"; mkdir -p "$ROOT/$FRAMES"; }
exec "$ROOT/tools/port/linux-i386.sh" sh -c '
  export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
  export HS_ROOT=/src/build-linux/run HS_TRACE=${HS_TRACE:-1} HS_TRACE_WAITS=${HS_TRACE_WAITS:-}
  [ -n "'"$FRAMES"'" ] && export HS_FRAME_DUMP=/src/'"$FRAMES"'
  if [ '"$SECS"' -gt 0 ]; then timeout -s INT '"$SECS"' ./build-linux/d2 "$@"; rc=$?; else ./build-linux/d2 "$@"; rc=$?; fi
  if [ -n "'"$FRAMES"'" ]; then for f in /src/'"$FRAMES"'/*.ppm; do [ -e "$f" ] && convert "$f" "${f%.ppm}.png" && rm "$f"; done; fi
  exit $rc' d2 "$@"
