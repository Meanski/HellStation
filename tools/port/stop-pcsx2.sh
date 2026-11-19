#!/usr/bin/env bash
# Stop PCSX2 without its "are you sure?" dialog: SIGTERM, then SIGKILL if it
# hasn't gone in 10 s. Matches the process name exactly (pgrep -x), so unlike
# `pkill -f` it can't hit a shell whose command line mentions PCSX2.
pkill -x PCSX2 2>/dev/null || exit 0
for _ in $(seq 50); do pgrep -x PCSX2 >/dev/null || exit 0; sleep 0.2; done
pkill -9 -x PCSX2 2>/dev/null || true
