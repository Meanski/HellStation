// On a small-RAM target (the PS2 reports a retail 32 MB) the sprite cache gets a
// fixed cap. The game sizes it as a third of free RAM clamped to 24-64 MB
// (D2Client UI/Draw.cpp), sized for a PC with a swap file; in the Rogue
// Encampment it held 5.2 MB. Smaller means more re-decoding of sprites, so the
// cap is set by measurement (PORTING_LOG). A port wrap: see docs/port/overrides.md.
#include <cstdint>
#include "Redivivus/port/platform.h"

#if defined(HS_FAKE_I386)
#  define HS_D2CMP_CC
#else
#  define HS_D2CMP_CC __attribute__((__stdcall__))
#endif

struct D2PoolManagerStrc;
extern "C" HS_D2CMP_CC void hs_orig_GFX_InitCelDataCache(D2PoolManagerStrc* pMemory, uint32_t dwCacheBytes,
                                                         uint32_t dwUnitCacheBytes, uint32_t dwAvailMemOverride)
    __asm__("hs_orig__ZN5D2CMP3SRC11SpriteCache20GFX_InitCelDataCacheEP17D2PoolManagerStrcjjj");

namespace {
constexpr uint32_t kSmallRamSpriteCache = 2u << 20;
}

namespace D2CMP::SRC::SpriteCache {
void __stdcall GFX_InitCelDataCache(D2PoolManagerStrc* pMemory, uint32_t dwCacheBytes, uint32_t dwUnitCacheBytes,
                                    uint32_t dwAvailMemOverride) {
    hs_mem_info mi = {};
    hs_mem_query(&mi);
    if (mi.phys_total && mi.phys_total <= (32u << 20) && dwCacheBytes > kSmallRamSpriteCache) {
        hs_log("sprite cache: %u KiB asked, capped at %u KiB", (unsigned)(dwCacheBytes >> 10), (unsigned)(kSmallRamSpriteCache >> 10));
        dwCacheBytes = kSmallRamSpriteCache;
    }
    hs_orig_GFX_InitCelDataCache(pMemory, dwCacheBytes, dwUnitCacheBytes, dwAvailMemOverride);
}
}  // namespace D2CMP::SRC::SpriteCache
