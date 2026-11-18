// On a small-RAM target (the PS2 reports a retail 32 MB) the sprite cache and the
// tile LRU get fixed caps. The game sizes the sprite cache as a third of free RAM clamped to 24-64 MB
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
extern "C" HS_D2CMP_CC void hs_orig_GFX_InitLRUCache(D2PoolManagerStrc* pMemory, uint32_t dwBytes)
    __asm__("hs_orig__ZN5D2CMP3SRC11SpriteCache16GFX_InitLRUCacheEP17D2PoolManagerStrcj");

namespace {
constexpr uint32_t kSmallRamSpriteCache = 2u << 20;
// The game gives the LRU a flat 6 MB (D2Client UI/Draw.cpp). It fills as the
// player explores: 1.2-1.3 MB standing in a town, and walking Harrogath's lower
// town took it past the retail heap. Capped, that walk drops to 15 FPS: the cap
// costs speed (PORTING_LOG, 2026-11-19).
constexpr uint32_t kSmallRamTileLru = 2048u << 10;
bool small_ram() {
    hs_mem_info mi = {};
    hs_mem_query(&mi);
    return mi.phys_total && mi.phys_total <= (32u << 20);
}
}

namespace D2CMP::SRC::SpriteCache {
void __stdcall GFX_InitCelDataCache(D2PoolManagerStrc* pMemory, uint32_t dwCacheBytes, uint32_t dwUnitCacheBytes,
                                    uint32_t dwAvailMemOverride) {
    if (small_ram() && dwCacheBytes > kSmallRamSpriteCache) {
        hs_log("sprite cache: %u KiB asked, capped at %u KiB", (unsigned)(dwCacheBytes >> 10), (unsigned)(kSmallRamSpriteCache >> 10));
        dwCacheBytes = kSmallRamSpriteCache;
    }
    hs_orig_GFX_InitCelDataCache(pMemory, dwCacheBytes, dwUnitCacheBytes, dwAvailMemOverride);
}
void __stdcall GFX_InitLRUCache(D2PoolManagerStrc* pMemory, uint32_t dwBytes) {
    if (small_ram() && dwBytes > kSmallRamTileLru) {
        hs_log("tile LRU: %u KiB asked, capped at %u KiB", (unsigned)(dwBytes >> 10), (unsigned)(kSmallRamTileLru >> 10));
        dwBytes = kSmallRamTileLru;
    }
    hs_orig_GFX_InitLRUCache(pMemory, dwBytes);
}
}  // namespace D2CMP::SRC::SpriteCache
