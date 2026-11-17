#!/usr/bin/env bash
# Record one application's frontmost window region (never the whole screen).
#   tools/port/winrec.sh <owner name, e.g. PCSX2> <out.mov> <seconds>
set -euo pipefail
PROC=$1; OUT=$2; SECS=$3
SRC=$(mktemp -t winrec).swift
trap 'rm -f "$SRC"' EXIT
cat > "$SRC" <<'SWIFT'
import CoreGraphics
let want = CommandLine.arguments[1].lowercased()
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
for w in list where (w[kCGWindowLayer as String] as? Int) == 0 {
  if let o = w[kCGWindowOwnerName as String] as? String, o.lowercased().contains(want),
     let b = w[kCGWindowBounds as String] as? [String: Any] {
    let x = b["X"] as! Double, y = b["Y"] as! Double, wd = b["Width"] as! Double, h = b["Height"] as! Double
    print("\(Int(x)),\(Int(y)),\(Int(wd)),\(Int(h))"); break
  }
}
SWIFT
RECT=$(swift "$SRC" "$PROC")
[ -n "$RECT" ] || { echo "winrec: no on-screen window for $PROC" >&2; exit 1; }
screencapture -x -v -V "$SECS" -R "$RECT" "$OUT"
