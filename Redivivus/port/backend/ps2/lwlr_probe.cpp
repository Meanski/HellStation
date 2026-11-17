// Start-up self-check: does an unaligned 32-bit load (GCC's LWL + LWR pair)
// come back sign-extended in the 64-bit register, as MIPS III requires?
// PCSX2's EE recompiler gets it wrong (docs/port/upstream/pcsx2-lwr-sign-extension.md);
// the build compensates (tools/port/ee-lwr-fixup.py), and this line records
// which kind of CPU the run had.
#include "Redivivus/port/platform.h"

#include <cstdint>

namespace {
struct __attribute__((packed)) Probe { uint16_t pad; int v; };
volatile Probe g_probe = {0, -4263};  // bytes of v at offset 2..5
}  // namespace

extern "C" void hs_lwlr_probe() {
    long long r;
    const volatile Probe* p = &g_probe;
    // Written in asm so the build's own fix-up (which only rewrites compiled code) can't hide it.
    __asm__ volatile("lwl %0, 5(%1)\n\tlwr %0, 2(%1)" : "=&r"(r) : "r"(p) : "memory");
    hs_log("CPU check: unaligned LWL/LWR of -4263 -> %#llx (%s)", (unsigned long long)r,
           r == -4263 ? "spec behaviour" : "upper half lost: PCSX2 recompiler bug, compensated by the build");
}
