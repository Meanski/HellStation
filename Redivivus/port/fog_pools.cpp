// Fog's pool allocator, seen from outside: for each pool manager, what its
// blocks have committed, how much of that holds live slots, and the "overflow"
// allocations (sizes past the largest class, or managers with a pool id) that
// Fog passes straight to malloc. Read-only; part of the memory report
// (win32/memstats.h).
//
// The block table is walked without the pools' locks. On the EE threads never
// pre-empt each other and the report runs on the main thread; on Linux a
// concurrent allocation can make one line slightly off, never unsafe to read
// except across a block-table realloc, which only happens while pools grow.
#include <malloc.h>
#include <stdio.h>

#include "Source/Fog/D2MemoryManagerStrc.h"
#include "Source/Fog/D2PoolManagerStrc.h"
#include "Source/Fog/Memory.data.h"
#include "Source/D2CMP/SRC/SpriteCache.h"
#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/memstats.h"

void hs_mem_report_fog(void) {
    D2MemoryManagerStrc& sys = Fog::Memory::pGlobalPoolSystem;
    if (!sys.bInit) return;
    auto kib = [](size_t n) { return (unsigned)((n + 512) >> 10); };
    {
        namespace SC = D2CMP::SRC::SpriteCache;
        size_t lruSize = 0;
        int misses = 0;
        ULONG_PTR hits = 0;
        uint32_t lruPercent = 0;
        SC::SPRITECACHE_GetStats(&lruSize, &misses, &hits, &lruPercent);
        hs_log("  game caches: sprite %u/%u KiB, celdata %u/%u KiB, lru %u KiB at %d%% (in use/limit)",
               kib(SC::SPRITECACHE_GetMemUsage()), kib(SC::SPRITECACHE_GetCacheSize()),
               kib((size_t)SC::CELDATACACHE_GetMemUsage()), kib(SC::CELDATACACHE_GetMemLimit()),
               kib(lruSize), (int)lruPercent);
    }
    size_t managers = sys.nManagers < 8 ? sys.nManagers : 8;
    for (size_t i = 0; i < managers; i++) {
        D2PoolManagerStrc& m = sys.pManagers[i];
        if (!m.nPools) continue;
        size_t committed = 0, used = 0, blocks = m.nBlocks;
        size_t classCommitted[40] = {}, classUsed[40] = {};
        for (size_t b = 0; b < blocks && m.pBlocks; b++) {
            D2PoolBlockStrc* block = m.pBlocks[b].pBlock;
            if (!block || !block->pPool) continue;
            committed += block->pPool->nSize;
            used += block->nBlocks * block->pPool->nBlockSize;
            ptrdiff_t c = block->pPool - m.pPools;
            if (c >= 0 && c < 40) {
                classCommitted[c] += block->pPool->nSize;
                classUsed[c] += block->nBlocks * block->pPool->nBlockSize;
            }
        }
        size_t overflow = 0, overflowCount = 0;
        EnterCriticalSection(&m.pSync);
        for (int k = 0; k < 1023; k++)
            for (BYTE* node = m.pOverflow[k]; node; node = *(BYTE**)node) {
                overflow += malloc_usable_size(node);
                overflowCount++;
            }
        LeaveCriticalSection(&m.pSync);
        hs_log("  fog \"%.31s\": %u blocks, committed %u KiB, slots in use %u KiB (%u%%), overflow %u allocs %u KiB",
               m.szName, (unsigned)blocks, kib(committed), kib(used),
               committed ? (unsigned)(used * 100 / committed) : 0u, (unsigned)overflowCount, kib(overflow));
        // Size classes holding 256 KiB or more: slot size, committed/in use (KiB).
        char line[256];
        int len = 0;
        for (size_t c = 0; c < m.nPools && c < 40; c++) {
            if (classCommitted[c] < (256u << 10) || len > (int)sizeof line - 40) continue;
            len += snprintf(line + len, sizeof line - len, " %u:%u/%u", (unsigned)m.pPools[c].nBlockSize,
                            kib(classCommitted[c]), kib(classUsed[c]));
        }
        if (len) hs_log("    classes (slot:committed/used KiB):%s", line);
    }
    hs_mem_report_fog_exact();
    hs_mem_report_storm_exact();
}
