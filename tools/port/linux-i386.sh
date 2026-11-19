#!/usr/bin/env bash
# Run a command in the Phase 1 container (i686 cross toolchain + i386 runtime) with the repo at /src and
# game data (read-only) at /d2data. Usage: tools/port/linux-i386.sh <cmd...>
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
IMAGE=hellstation/linux-i386:trixie
D2DATA=${D2DATA:-$HOME/d2data}
# Docker Desktop on arm64 loses binfmt registrations on restart; re-register qemu-i386 if needed.
if ! docker run --rm --platform linux/386 debian:bookworm true >/dev/null 2>&1; then
  docker run --privileged --rm tonistiigi/binfmt --install 386 >/dev/null
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" -f "$ROOT/tools/port/Dockerfile.linux-i386" "$ROOT/tools/port"
fi
mounts=(-v "$ROOT:/src" -v hellstation-ccache-i386:/ccache)
[ -d "$D2DATA" ] && mounts+=(-v "$D2DATA:/d2data:ro")
exec docker run --rm "${mounts[@]}" -e D2DATA=/d2data -e HS_FRAME_DUMP_EVERY -e HS_TRACE -e HS_TRACE_WAITS -e HS_PHYS_MB "$IMAGE" "$@"
