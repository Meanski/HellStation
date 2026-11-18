// MPQ hash tables, compacted on the EE.
//
// Storm keeps each archive's whole hash table in memory: 16 bytes a slot, and
// d2data.mpq and d2exp.mpq have 65,536 slots for about 10,000 files each. The
// 11 LoD archives' tables are 4.0 MB, of which about 0.3 MB is in use
// (PORTING_LOG, 2026-11-19). The first lookup in an archive replaces its table
// with only the slots that aren't empty, in slot order, followed by each one's
// slot number; the full table is freed.
//
// Storm reads the table in exactly two places: SFILE_SearchHashTable (name ->
// slot) and SFILE_GetBlockTableEntry (slot -> block entry, pHashTable[slot]).
// The search is replaced: it returns an index into the compact table, which
// GetBlockTableEntry then reads unchanged. Both are file-statics reached inside
// SFile.cpp, so on MIPS the search's first instruction is patched to jump here
// (as overrides/storm_align.cpp does). The probe is the original's: from the
// name's start slot along the run of non-empty slots (deleted ones included),
// wrapping at the end, the same locale and platform preference. SFileCloseArchive
// frees pHashTable with SMemFree, so the compact table is an SMemAlloc block.
// Other targets keep Storm's own search.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "3rdparty/Storm/D2ArchiveStrc.h"
#include "Redivivus/port/platform.h"

#if defined(__mips__)
#if defined(HS_FAKE_I386)
#  define HS_STORM_CC
#else
#  define HS_STORM_CC __attribute__((__stdcall__))
#endif

namespace Storm::Source::SMem {
HS_STORM_CC int32_t SMemFree(void* pObject, char* szFile, int32_t nLine, DWORD dwFlags);
HS_STORM_CC void* SMemAlloc(uint32_t nSize, char* szFile, uint32_t nLine, uint32_t nFlags);
}  // namespace Storm::Source::SMem

extern "C" void FlushCache(int operation);  // ps2sdk kernel.h can't sit beside the Win32 headers
extern "C" HS_STORM_CC uint32_t hs_Storm_SFILE_Hash(int32_t nHashKind, char* szText);
extern "C" void hs_Storm_SFILE_SearchHashTable(void);  // address only

namespace {
constexpr uint32_t kEmpty = 0xFFFFFFFFu, kDeleted = 0xFFFFFFFEu;

struct Compact {
    D2ArchiveStrc* archive;
    D2MPQHashEntryStrc* table;  // what pHashTable was set to; a different value means a new table
    uint32_t n;
};
constexpr int kMaxArchives = 32;
Compact g_compact[kMaxArchives];
size_t g_saved;

uint32_t* slots_of(const Compact& c) { return (uint32_t*)(c.table + c.n); }

Compact* find(D2ArchiveStrc* a) {
    for (Compact& c : g_compact)
        if (c.archive == a && c.table == a->pHashTable) return &c;
    return nullptr;
}
Compact* entry_for(D2ArchiveStrc* a) {
    for (Compact& c : g_compact)
        if (!c.archive || c.archive == a) return &c;
    for (Compact& c : g_compact)  // more archives than entries: one whose table moved on (closed)
        if (c.archive->pHashTable != c.table) return &c;
    return nullptr;
}

// SMemAlloc and SMemFree take Storm's heap lock, and on the EE a contended lock
// yields to another thread, which may search (and so compact) the same archive.
// So: re-check after allocating, and point the archive at the compact table
// before freeing the full one.
Compact* compact(D2ArchiveStrc* a) {
    if (Compact* c = find(a)) return c;
    uint32_t size = a->pMpqHeader->dwHashTableSize;
    D2MPQHashEntryStrc* full = a->pHashTable;
    uint32_t n = 0;
    for (uint32_t i = 0; i < size; i++) n += full[i].dwBlockIndex != kEmpty;
    size_t bytes = (size_t)n * (sizeof(D2MPQHashEntryStrc) + sizeof(uint32_t));
    auto* table = (D2MPQHashEntryStrc*)Storm::Source::SMem::SMemAlloc(bytes ? (uint32_t)bytes : 16u, (char*)"mpq_compact_index", __LINE__, 0);
    if (!table) return find(a);
    Compact* e = a->pHashTable == full ? entry_for(a) : nullptr;
    if (!e) {  // compacted by another thread meanwhile, or no entry free
        hs_log("mpq index: %s: %s", a->szArchivePath, a->pHashTable != full ? "compacted by another thread meanwhile" : "no entry free");
        Storm::Source::SMem::SMemFree(table, (char*)"mpq_compact_index", __LINE__, 0);
        return find(a);
    }
    uint32_t* slots = (uint32_t*)(table + n);
    for (uint32_t i = 0, k = 0; i < size; i++)
        if (full[i].dwBlockIndex != kEmpty) { table[k] = full[i]; slots[k] = i; k++; }
    *e = Compact{a, table, n};
    a->pHashTable = table;
    Storm::Source::SMem::SMemFree(full, (char*)"mpq_compact_index", __LINE__, 0);
    g_saved += (size_t)size * sizeof(D2MPQHashEntryStrc) - bytes;
    hs_log("mpq index: %s: %u of %u slots kept (%u KiB freed in all)", a->szArchivePath, (unsigned)n, (unsigned)size,
           (unsigned)(g_saved >> 10));
    return e;
}

// The original search over a full table: the fallback if compacting failed.
uint32_t search_full(D2ArchiveStrc* a, uint16_t locale, char platform, uint32_t start, uint32_t ha, uint32_t hb) {
    uint32_t mask = a->pMpqHeader->dwHashTableSize - 1, best = kEmpty, slot = start & mask;
    uint32_t first = slot;
    do {
        D2MPQHashEntryStrc& e = a->pHashTable[slot];
        if (e.dwBlockIndex == kEmpty) return best;
        if (e.dwHashA == ha && e.dwHashB == hb && e.dwBlockIndex != kDeleted) {
            if (e.wLocale == locale && (char)e.wPlatform == platform) return slot;
            if ((!e.wLocale || e.wLocale == locale) && ((char)e.wPlatform == '\0' || (char)e.wPlatform == platform)) best = slot;
        }
        slot = (slot + 1) & mask;
    } while (slot != first);
    return best;
}

uint32_t search(D2ArchiveStrc* a, int16_t nLocale, char platform, char* name) {
    uint32_t start = hs_Storm_SFILE_Hash(0, name);
    uint32_t ha = hs_Storm_SFILE_Hash(1, name), hb = hs_Storm_SFILE_Hash(2, name);
    uint16_t locale = (uint16_t)nLocale;
    Compact* c = compact(a);
    if (!c) return search_full(a, locale, platform, start, ha, hb);
    uint32_t mask = a->pMpqHeader->dwHashTableSize - 1, slot = start & mask, n = c->n;
    const uint32_t* slots = slots_of(*c);
    uint32_t lo = 0, hi = n;  // the first kept slot >= the start slot
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (slots[mid] < slot) lo = mid + 1; else hi = mid;
    }
    uint32_t i = lo == n ? 0 : lo, best = kEmpty;
    for (uint32_t walked = 0; walked < n; walked++) {
        if (slots[i] != slot) return best;  // an empty slot ends the run
        D2MPQHashEntryStrc& e = c->table[i];
        if (e.dwHashA == ha && e.dwHashB == hb && e.dwBlockIndex != kDeleted) {
            if (e.wLocale == locale && (char)e.wPlatform == platform) return i;
            if ((!e.wLocale || e.wLocale == locale) && ((char)e.wPlatform == '\0' || (char)e.wPlatform == platform)) best = i;
        }
        slot = (slot + 1) & mask;
        if (++i == n) i = 0;
    }
    return best;  // every slot full: the whole table was walked
}
__attribute__((constructor)) void patch_search() {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)&hs_Storm_SFILE_SearchHashTable;
    uint32_t target = (uint32_t)(uintptr_t)&search;
    p[0] = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);  // j search
    p[1] = 0;                                             // nop (delay slot)
    FlushCache(0);  // write back the data cache
    FlushCache(2);  // invalidate the instruction cache
}
}  // namespace
#endif  // __mips__
