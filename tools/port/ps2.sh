#!/usr/bin/env bash
# Run a command in the pinned ps2dev toolchain container with the repo at /src.
# Usage: tools/port/ps2.sh <cmd...>
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
IMAGE=ps2dev/ps2dev:v2.0.0
exec docker run --rm -v "$ROOT:/src" -v hellstation-ccache-ps2:/ccache -e CCACHE_DIR=/ccache -w /src "$IMAGE" "$@"
