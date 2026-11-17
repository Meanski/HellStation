#!/usr/bin/env bash
# Boot the PS2 build in PCSX2 (macOS host) and capture its window.
#   tools/port/run-pcsx2.sh [--seconds N] [--extra-ram] [--shot out.png]
# Stages build-ps2/run/: the stripped ELF plus data/ -> $D2DATA (the owner's
# game files, never copied), which the ELF reads through PCSX2's host: device.
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SECS=60; EXTRA=false; SHOT="$ROOT/build-ps2/run/pcsx2.png"
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECS=$2; shift 2 ;;
    --extra-ram) EXTRA=true; shift ;;
    --shot) SHOT=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
RUN="$ROOT/build-ps2/run"
mkdir -p "$RUN/hsroot"
ln -sfn "${D2DATA:-$HOME/d2data}" "$RUN/data"
"$ROOT/tools/port/ps2.sh" mips64r5900el-ps2-elf-strip -o build-ps2/run/d2.elf build-ps2/d2.elf

INI="$HOME/Library/Application Support/PCSX2/inis/PCSX2.ini"
if grep -q "^\[EmuCore/CPU\]" "$INI"; then
  sed -i '' "/^\[EmuCore\/CPU\]/,/^\[/ s/^ExtraMemory = .*/ExtraMemory = $EXTRA/" "$INI"
  grep -q "^ExtraMemory" "$INI" || sed -i '' "s/^\[EmuCore\/CPU\]/[EmuCore\/CPU]\nExtraMemory = $EXTRA/" "$INI"
else
  printf '\n[EmuCore/CPU]\nExtraMemory = %s\n' "$EXTRA" >> "$INI"
fi

# EE/IOP console output (sio_puts, printf) into the log file.
python3 - "$INI" <<'PY'
import sys, re
p = sys.argv[1]; s = open(p).read()
# [EmuCore] HostFs: PCSX2 refuses host: file access (beyond loading the ELF) without it.
for key in ("HostFs", "EnablePINE"):
    if re.search(r"^" + key + r" = ", s, re.M): s = re.sub(r"^" + key + r" = .*$", key + " = true", s, flags=re.M)
    elif "[EmuCore]\n" in s: s = s.replace("[EmuCore]\n", "[EmuCore]\n" + key + " = true\n", 1)
    else: s += "\n[EmuCore]\n" + key + " = true\n"
if False: s = re.sub(r"^HostFs = .*$", "HostFs = true", s, flags=re.M)
else: s = s.replace("[EmuCore]\n", "[EmuCore]\nHostFs = true\n", 1) if "[EmuCore]\n" in s else s + "\n[EmuCore]\nHostFs = true\n"
want = {"EnableSystemConsole": "true", "EnableFileLogging": "true", "EnableEEConsole": "true", "EnableIOPConsole": "true"}
if "[Logging]" not in s: s += "\n[Logging]\n"
for k, v in want.items():
    if re.search(r"^" + k + r" = ", s, re.M): s = re.sub(r"^" + k + r" = .*$", k + " = " + v, s, flags=re.M)
    else: s = s.replace("[Logging]\n", "[Logging]\n" + k + " = " + v + "\n", 1)
open(p, "w").write(s)
PY

pkill -f 'MacOS/PCSX2' 2>/dev/null || true
/Applications/PCSX2.app/Contents/MacOS/PCSX2 -fastboot -earlyconsolelog -logfile "$RUN/pcsx2.log" -elf "$RUN/d2.elf" >/dev/null 2>&1 &
sleep "$SECS"
osascript -e 'tell application "PCSX2" to activate' >/dev/null 2>&1 || true
sleep 2
"$ROOT/tools/port/winshot.sh" PCSX2 "$SHOT" && echo "screenshot: $SHOT"
# The in-memory log, read over PINE (works even if the EE crashed before main()).
syms=$("$ROOT/tools/port/ps2.sh" mips64r5900el-ps2-elf-nm build-ps2/d2.elf | grep -E " hs_log_ring(_pos)?$")
ring=$(echo "$syms" | awk '$3=="hs_log_ring"{print $1}'); pos=$(echo "$syms" | awk '$3=="hs_log_ring_pos"{print $1}')
python3 "$ROOT/tools/port/pine-log.py" "$ring" "$pos" > "$RUN/ring.log" 2>&1 || true
echo "in-memory log: $RUN/ring.log ($(wc -l < "$RUN/ring.log") lines)"
