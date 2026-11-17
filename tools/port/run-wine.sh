#!/usr/bin/env bash
# Phase 0: run the mingw d2.exe under Wine against the owner's game data.
# A run directory symlinks $D2DATA's files and the built exe side by side,
# so nothing is copied into or out of the data folder.
#   tools/port/run-wine.sh [extra d2 args...]      (defaults to -w)
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
D2DATA=${D2DATA:-$HOME/d2data}
RUN=${D2RUN:-$HOME/d2run}
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-hellstation}
export WINEDEBUG=${WINEDEBUG:--all}
mkdir -p "$RUN"
for f in "$D2DATA"/*; do ln -sf "$f" "$RUN/"; done
ln -sf "$ROOT/build/d2.exe" "$RUN/d2.exe"
cd "$RUN"
[ $# -eq 0 ] && set -- -w
exec wine d2.exe "$@"
