#!/usr/bin/env bash
# Run a command in the Phase 2 build container (pinned ps2dev toolchain) with
# the repo at /src. Usage: tools/port/ps2.sh <cmd...>
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
IMAGE=hellstation/ps2dev:v2.0.0
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" -f "$ROOT/tools/port/Dockerfile.ps2" "$ROOT/tools/port"
fi
exec docker run --rm -v "$ROOT:/src" -v hellstation-ccache-ps2:/ccache -w /src "$IMAGE" "$@"
