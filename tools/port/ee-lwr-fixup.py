#!/usr/bin/env python3
"""Re-canonicalise every LWR result in GCC's EE assembly: lwr rt,... -> + sll rt,rt,0.

PCSX2's EE recompiler zeroes the upper 32 bits of rt on a LWR whose address
isn't word aligned; the MIPS III spec (and PCSX2's own interpreter) leave them
as LWL set them, i.e. sign-extended. GCC builds every unaligned 32-bit load
from an LWL/LWR pair and then compares the 64-bit register (slt, bltz), so on
the recompiler a negative value read from a packed struct compares as a huge
positive one. `sll rt,rt,0` is MIPS64's canonical 32->64 sign extension: a
no-op on hardware, the fix on the emulator. Evidence: PORTING_LOG.md,
2026-11-17; draft report: docs/port/upstream/pcsx2-lwr-sign-extension.md.

An LWR in the delay slot of an ordinary branch always executes, so it moves
above the branch (with its SLL) and a NOP takes the slot, provided the branch
neither reads rt nor links into a register the load uses. A branch-likely
slot, or either conflict, fails the build instead of producing a binary that
runs wrong.

Hand-written inline asm (between GCC's #APP / #NO_APP markers) is left as written.

usage: ee-lwr-fixup.py in.s out.s
"""
import re
import sys

INSN = re.compile(r"^\s+([a-z][a-z0-9.]*)\b\s*(.*)$")
BRANCH = re.compile(r"^(b|bal|j|jal|jr|jalr|b(eq|ne|gez|gtz|lez|ltz|c[01][ft])[a-z]*|bgezal[l]?|bltzal[l]?)$")


def main(src: str, dst: str) -> int:
    noreorder = False
    inline_asm = False
    prev = None  # mnemonic of the previous instruction in this block
    prev_idx = -1  # its index in out
    prev_args = ""
    out = []
    fixed = 0
    with open(src) as f:
        for n, line in enumerate(f, 1):
            out.append(line)
            s = line.strip()
            if s == "#APP":
                inline_asm = True
                continue
            if s == "#NO_APP":
                inline_asm = False
                continue
            if inline_asm:
                continue
            if s.startswith(".set"):
                if "noreorder" in s:
                    noreorder = True
                elif re.search(r"\breorder\b", s):
                    noreorder = False
                continue
            if not s or s.startswith((".", "#")) or s.endswith(":"):
                if s.endswith(":"):
                    prev = None  # a label starts a new basic block
                continue
            m = INSN.match(line)
            if not m:
                continue
            op, args = m.group(1), m.group(2)
            if op == "lwr":
                rt = args.split(",")[0].strip()
                if rt not in ("$0", "$zero"):
                    if noreorder and prev and BRANCH.match(prev):
                        base = re.search(r"\((\$\w+)\)", args)
                        regs = set(re.findall(r"\$\w+", prev_args))
                        links = prev in ("jal", "jalr", "bal") or prev.endswith(("al", "all"))
                        likely = prev.endswith("l") and prev not in ("jal", "bal") and not prev.endswith("al")
                        uses_ra = rt in ("$31", "$ra") or (base and base.group(1) in ("$31", "$ra"))
                        if likely or rt in regs or (links and uses_ra):
                            sys.stderr.write(f"{src}:{n}: lwr in a delay slot of '{prev} {prev_args}' can't be moved\n")
                            return 1
                        lwr_line = out.pop()
                        out.insert(prev_idx, lwr_line)
                        out.insert(prev_idx + 1, f"\tsll\t{rt},{rt},0\n")
                        out.append("\tnop\n")
                        fixed += 1
                        prev = "nop"
                        prev_idx = len(out) - 1
                        prev_args = ""
                        continue
                    out.append(f"\tsll\t{rt},{rt},0\n")
                    fixed += 1
            prev = op
            prev_idx = len(out) - 1
            prev_args = args
    with open(dst, "w") as f:
        f.writelines(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
