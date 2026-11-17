#!/usr/bin/env python3
"""Compiler launcher for EE builds (cmake/ps2-ee.cmake).

Compiles to assembly, applies tools/port/ee-lwr-fixup.py, then assembles.
Anything other than a plain `-c <src> -o <obj>` compile runs unchanged.
    ee-cc-launcher.sh <compiler> <args...>
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    cc, args = argv[0], argv[1:]
    if "-c" not in args or "-o" not in args:
        os.execvp(cc, argv)
    obj = args[args.index("-o") + 1]
    raw, fixed = obj + ".raw.s", obj + ".fixed.s"
    to_asm = ["-S" if a == "-c" else a for a in args]
    to_asm[to_asm.index("-o") + 1] = raw
    rc = subprocess.call([cc] + to_asm)
    if rc:
        return rc
    rc = subprocess.call([sys.executable, os.path.join(HERE, "ee-lwr-fixup.py"), raw, fixed])
    if rc:
        return rc
    target = [a for a in args if a.startswith(("-m", "-G", "-g"))]
    rc = subprocess.call([cc] + target + ["-c", "-x", "assembler", fixed, "-o", obj])
    for f in (raw, fixed):
        try:
            os.remove(f)
        except OSError:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
