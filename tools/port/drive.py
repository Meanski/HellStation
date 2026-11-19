#!/usr/bin/env python3
"""Drive a running PS2 build from the host with live.sh and the in-game `where` command.

  HSB=build-ps2-os tools/port/drive.py wp        # walk to the Act 1 town waypoint, take it to the Black Marsh
  HSB=build-ps2-os tools/port/drive.py hunt 20   # teleport to the nearest monsters and cast at them, 20 rounds

Start the game first, e.g. tools/port/run-pcsx2.sh --build build-ps2-os --seconds 1200 \
  --script tools/port/scripts/load-save-normal.txt (needs a save with a Sorceress: Teleport on the
right button, an attack on the left). Prints the last memory lines after each round.
"""
import math, os, re, subprocess, sys, time
ROOT = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip(); B = os.environ.get("HSB", "build-ps2-os"); LOG = ROOT + "/" + B + "/run/hs.log"
def log(): return open(LOG, errors="replace").read()
def send(*steps): subprocess.run([ROOT + "/tools/port/live.sh", "--build", B, *steps], check=True, stdout=subprocess.DEVNULL)
def where(delay=5, timeout=150):
    n = log().count("where: player"); send(f"+{delay} where"); t = time.time()
    while log().count("where: player") <= n:
        if time.time() - t > timeout: sys.exit("where: no answer (game stalled?)")
        time.sleep(1)
    time.sleep(1.5)
    block = log().rsplit("where: player", 1)[1]
    units = []
    for m in re.finditer(r"where: (\w+) (\d+) at ([+-]\d+),([+-]\d+) -> click (-?\d+) (-?\d+)", block):
        units.append((m[1], int(m[2]), int(m[3]), int(m[4]), int(m[5]), int(m[6])))
    return units
def toward(sx, sy, r):
    dx, dy = sx - 320, sy - 232; d = math.hypot(dx, dy) or 1
    k = min(1.0, r / d); return int(320 + dx * k), int(232 + dy * k)
def walk_to(kind, cls, button="", r=190, hop=80):
    for _ in range(12):
        u = [x for x in where() if x[0] == kind and x[1] == cls]
        if not u: sys.exit(f"no {kind} {cls} in range")
        _, _, dx, dy, sx, sy = min(u, key=lambda x: abs(x[2]) + abs(x[3]))
        if 40 <= sx <= 600 and 40 <= sy <= 420: return sx, sy
        x, y = toward(sx, sy, r)
        send(f"+5 move {x} {y}", f"+17 click {x} {y}{button}"); time.sleep(hop / 25)
    sys.exit("could not reach it")
def mem(): return [l for l in log().splitlines() if l.startswith(("mem f", "heap:", "sprite cache: gave", "sprite cache: held", "VirtualAlloc"))][-3:]
cmd = sys.argv[1]
if cmd == "wp":
    sx, sy = walk_to("object", 119)
    send(f"+5 move {sx} {sy-4}", f"+17 click {sx} {sy-4}", "+90 move 125 219", "+102 click 125 219")
    time.sleep(12); where(); print("\n".join(mem()))
elif cmd == "hunt":
    for i in range(int(sys.argv[2]) if len(sys.argv) > 2 else 3):
        mons = [x for x in where() if x[0] == "monster" and x[1] != 338]
        if not mons: print("no monsters near"); break
        _, cls, dx, dy, sx, sy = min(mons, key=lambda x: abs(x[2]) + abs(x[3]))
        if 40 <= sx <= 600 and 40 <= sy <= 420:
            steps = []
            for k in range(4): steps += [f"+{5+k*30} move {sx+k} {sy-24}", f"+{17+k*30} click {sx+k} {sy-24}"]
            send(*steps); time.sleep(6); print(f"attacked monster {cls} at {sx},{sy}")
        else:
            x, y = toward(sx, sy, 190)
            send(f"+5 move {x} {y}", f"+17 click {x} {y} right"); time.sleep(3); print(f"teleported toward monster {cls} ({dx},{dy})")
        print("  " + "\n  ".join(l[:120] for l in mem()))
