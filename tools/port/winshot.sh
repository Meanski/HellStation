#!/usr/bin/env bash
# Capture one application's frontmost window only (never the whole screen).
#   tools/port/winshot.sh <owner name, e.g. PCSX2 or wine> <out.png>
set -euo pipefail
PROC=$1; OUT=$2
SRC=$(mktemp -t winshot).swift
trap 'rm -f "$SRC"' EXIT
cat > "$SRC" <<'SWIFT'
import CoreGraphics
let want = CommandLine.arguments[1].lowercased()
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
for w in list where (w[kCGWindowLayer as String] as? Int) == 0 {
  if let o = w[kCGWindowOwnerName as String] as? String, o.lowercased().contains(want) {
    print(w[kCGWindowNumber as String] as! Int); break
  }
}
SWIFT
WID=$(swift "$SRC" "$PROC")
[ -n "$WID" ] || { echo "winshot: no on-screen window for $PROC" >&2; exit 1; }
screencapture -x -o -l "$WID" "$OUT"
