// Storm's SMemAlloc doesn't round request sizes, so after one odd-sized block
// every later block in that heap starts at an odd address: Storm's own archive
// records came back at 0x...2F3, 0x...55E. x86 doesn't care; on MIPS each 32-bit
// field read from such a record is an unaligned load, and lookups in every
// archive but the first silently failed. Every Storm allocation is rounded up
// to 8 bytes here (a port wrap: see docs/port/overrides.md).
#include <cstdint>

#if defined(HS_FAKE_I386)
#  define HS_STORM_CC
#else
#  define HS_STORM_CC __attribute__((__stdcall__))
#endif

extern "C" HS_STORM_CC void* hs_orig_SMemAlloc(uint32_t nSize, char* szFile, uint32_t nLine, uint32_t nFlags)
    __asm__("hs_orig__ZN5Storm6Source4SMem9SMemAllocEjPcjj");


// SMemCalculateAllocationLayout (static in SMem.cpp) lost its rounding in
// generation: it computes `(n ? 7 : 0) + n` where the original is `(n + 7) & ~7`,
// so every Storm block is 15 bytes over its request and never 8-aligned. Its
// callers reach it through its section (-ffunction-sections), so no link-time
// redirect is possible; on MIPS its first instruction is patched to jump to the
// corrected copy below, before Storm lays out a single block. (x86 doesn't care.)
#if defined(__mips__)
// ps2sdk's kernel.h can't be included alongside the Win32 headers; one declaration.
extern "C" void FlushCache(int operation);
extern "C" uint32_t hs_Storm_ProtectMemory;
extern "C" uint32_t hs_Storm_DebugMemory;
extern "C" void hs_Storm_SMemCalculateAllocationLayout(void);  // address only

static void storm_layout_fixed(uint32_t nSize, int* pnOutProtected, int* pnOutTotalSize, int* pnOutPadding, int* pnOutDebug) {
    int bProtected = !hs_Storm_ProtectMemory && nSize <= 0xfe87 ? 0 : 1;
    *pnOutProtected = bProtected;
    int bDebug = (!hs_Storm_DebugMemory || bProtected) ? 0 : 1;
    *pnOutDebug = bDebug;
    if (bProtected) nSize = 4;
    int nHeader = 8 + (bDebug ? 2 : 0) + (int)nSize;
    int nAligned = (nHeader + 7) & ~7;
    *pnOutTotalSize = nAligned;
    *pnOutPadding = nAligned - nHeader;
}

static void storm_patch_layout() {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)&hs_Storm_SMemCalculateAllocationLayout;
    uint32_t target = (uint32_t)(uintptr_t)&storm_layout_fixed;
    p[0] = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);  // j storm_layout_fixed
    p[1] = 0;                                             // nop (delay slot)
    FlushCache(0);  // write back the data cache
    FlushCache(2);  // invalidate the instruction cache
}
#endif

namespace Storm::Source::SMem {
void* __stdcall SMemAlloc(uint32_t nSize, char* szFile, uint32_t nLine, uint32_t nFlags) {
#if defined(__mips__)
    static bool s_patched;
    if (!s_patched) { s_patched = true; storm_patch_layout(); }
#endif
    return hs_orig_SMemAlloc((nSize + 7u) & ~7u, szFile, nLine, nFlags);
}
}  // namespace Storm::Source::SMem
