# Draft: PCSX2 EE recompiler LWR clobbers the upper 32 bits on a non-aligned address

For the owner to file at PCSX2/pcsx2 (GitHub issues). Not filed.

## Summary

On the EE recompiler, `LWR rt, off(base)` with `(base + off) & 3 != 0` zeroes bits 63..32 of
`rt`. The MIPS III definition, and PCSX2's own EE interpreter, leave those bits as they were. After
the usual `LWL` + `LWR` pair, they hold the sign extension `LWL` wrote. GCC builds every unaligned
32-bit load (packed structs) from that pair and then compares the 64-bit register (`slt`, `bltz`).
So under the recompiler, a negative value read through a packed struct compares as a large positive
one.

## Reproduction

A packed `{ uint16_t pad; int v = -4263; }` (the bytes of `v` at offset 2..5), and three sequences
on the same address:

| Sequence | Recompiler | Interpreter (`EnableEE = false`) | Expected |
|-|-|-|-|
| `lwl r,5(p)` (r = 0 before) | `0xffffffffffff0000` | not run | `0xffffffffffff0000` |
| `lwl r,5(p)` ; `lwr r,2(p)` | `0x00000000ffffef59` | `0xffffffffffffef59` | `0xffffffffffffef59` |
| `lwr r,2(p)` ; `lwl r,5(p)` | `0xffffffffffffef59` | not run | `0xffffffffffffef59` |

The probe is `Redivivus/port/backend/ps2/lwlr_probe.cpp` in this repository (it runs at every start
and logs a `CPU check:` line), built with the ps2dev v2.0.0 toolchain, run as an ELF with the owner's
own BIOS. PCSX2 v2.8.2 (macOS, Apple Silicon).

## Impact seen

Diablo II's movement code reads a unit's velocity from a packed struct and compares it with the
remaining distance. Every walk with a negative velocity component finished in a single step: units
teleported to their destination. The same build is correct under the interpreter.

## Workaround in this project

`tools/port/ee-lwr-fixup.py` appends `sll rt,rt,0` (canonical 32→64 sign extension) after each `LWR`
in compiled code. It's harmless on hardware.

## Not verified

Real hardware. The interpreter's behaviour matches the MIPS III manual, so hardware is expected to
match it, but no console run has been done.
