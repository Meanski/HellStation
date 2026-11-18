// Fog's pool allocator, exact-size above 256 bytes on a small-RAM target.
//
// Fog rounds every request up to a power of two and carves slots from 64 KiB
// and 512 KiB blocks that it keeps while any slot in them lives. In the Rogue
// Encampment that was 3.5 MB of the Client pool's committed blocks unused, plus
// the rounding (PORTING_LOG, 2026-11-18). On the PS2 (the game is told 32 MB),
// requests over kExactAbove bytes skip the pools and get exactly their size
// behind a 16-byte header: from malloc, or from 64 KiB the region allocator.
// Smaller ones, which pack densely, stay in Fog's pools.
//
// Each header sits on its manager's list, so the manager's teardown
// (FreeMemoryPool, e.g. at the end of a game) frees them as Fog frees its own.
// Frees and reallocs recognise a header by a check word tied to its address;
// anything else goes to Fog unchanged. Fog's own malloc "overflow" (managers
// with a pool id, AsyncData's buffers, growth inside Fog's ReAlloc) stays Fog's.
//
// Wrapped entry points: every call from outside Fog/Memory.cpp (the game's
// FOG_* imports). Calls inside that object reach Fog's originals, which is
// what Fog's internals need. Off on Linux and Windows (they report more RAM).
// The EE never pre-empts a thread, so the lists need no lock there.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Source/Fog/D2MemoryManagerStrc.h"
#include "Source/Fog/D2PoolManagerStrc.h"
#include "Source/Fog/Memory.data.h"
#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/memstats.h"

#if defined(HS_FAKE_I386)
#  define HS_FOG_CC
#else
#  define HS_FOG_CC __attribute__((__fastcall__))
#endif

#define HS_FOG_ORIG(sym) __asm__("hs_orig_" #sym)
extern "C" {
HS_FOG_CC void* hs_orig_Alloc(D2PoolManagerStrc*, size_t, char*, int32_t)
    HS_FOG_ORIG(_ZN3Fog6Memory5AllocEP17D2PoolManagerStrcjPci);
HS_FOG_CC void* hs_orig_AllocClient(size_t, char*, int32_t, int32_t)
    HS_FOG_ORIG(_ZN3Fog6Memory17AllocClientMemoryEjPcii);
HS_FOG_CC void* hs_orig_AllocServer(D2PoolManagerStrc*, size_t, char*, int32_t, int32_t)
    HS_FOG_ORIG(_ZN3Fog6Memory17AllocServerMemoryEP17D2PoolManagerStrcjPcii);
HS_FOG_CC void* hs_orig_ReAllocServer(D2PoolManagerStrc*, void*, size_t, char*, int, int)
    HS_FOG_ORIG(_ZN3Fog6Memory19ReAllocServerMemoryEP17D2PoolManagerStrcPvjPcii);
HS_FOG_CC int hs_orig_FreeClient(void*, char*, int32_t, int32_t)
    HS_FOG_ORIG(_ZN3Fog6Memory16FreeClientMemoryEPvPcii);
HS_FOG_CC int hs_orig_FreeServer(D2PoolManagerStrc*, void*, char*, int32_t, int32_t)
    HS_FOG_ORIG(_ZN3Fog6Memory16FreeServerMemoryEP17D2PoolManagerStrcPvPcii);
void hs_orig_FreeMemoryPool(D2PoolManagerStrc*) HS_FOG_ORIG(_ZN3Fog6Memory14FreeMemoryPoolEP17D2PoolManagerStrc);
}

namespace {
constexpr size_t kExactAbove = 256;  // Fog's 512-byte class and up
constexpr size_t kRegionFrom = 64u << 10;  // and up: the region allocator (see raw_alloc)
constexpr uint32_t kCheck = 0x46E6A11Cu;
constexpr int kManagers = 8;

struct Hdr {  // 16 bytes, so the user block keeps malloc's alignment
    Hdr* next;
    Hdr* prev;
    uint32_t check;  // (uintptr_t)this ^ kCheck
    uint32_t size;
};
static_assert(sizeof(Hdr) == 16 || sizeof(void*) == 8, "header must be 16 bytes on 32-bit targets");

Hdr g_lists[kManagers];  // circular, one sentinel per pool manager
size_t g_live, g_peak, g_count, g_under16;  // g_under16: blocks malloc placed below 16-byte alignment

int g_on = -1;  // decided at the first call: a small-RAM target
bool on() {
    if (g_on < 0) {
        hs_mem_info mi = {};
        hs_mem_query(&mi);
        g_on = mi.phys_total && mi.phys_total <= (32u << 20);
        for (Hdr& h : g_lists) h.next = h.prev = &h;
        if (g_on) hs_log("fog: exact-size allocations above %u bytes", (unsigned)kExactAbove);
    }
    return g_on;
}

D2MemoryManagerStrc& sys() { return Fog::Memory::pGlobalPoolSystem; }
int manager_index(D2PoolManagerStrc* m) {
    if (!m) return 0;
    ptrdiff_t i = m - sys().pManagers;
    return i >= 0 && i < kManagers ? (int)i : -1;
}
uint32_t check_of(Hdr* h) { return (uint32_t)(uintptr_t)h ^ kCheck; }

void link(Hdr* h, int list) {
    Hdr* head = &g_lists[list];
    h->next = head->next;
    h->prev = head;
    head->next->prev = h;
    head->next = h;
    h->check = check_of(h);
}
void unlink(Hdr* h) {
    h->prev->next = h->next;
    h->next->prev = h->prev;
    h->check = 0;
}

// A block of ours, or null. Reads the 16 bytes before p: on the EE all RAM is
// readable, and the check word ties the header to its own address.
Hdr* ours(void* p) {
    if (!p || g_on != 1 || ((uintptr_t)p & 3)) return nullptr;
    Hdr* h = (Hdr*)p - 1;
    return h->check == check_of(h) && h->next->prev == h && h->prev->next == h ? h : nullptr;
}

// Big blocks (whole-file reads, sprite decodes: up to 575 KiB, mostly short-
// lived) come from the region allocator at the top of RAM, which coalesces its
// gaps; left in malloc's heap they cut it into holes the small objects
// couldn't use (1 MB at the first retail-cap run). Zeroed there by the backend.
bool in_region(size_t n) { return n >= kRegionFrom; }

Hdr* raw_alloc(size_t n) {
    if (in_region(n)) return (Hdr*)hs_vm_reserve(sizeof(Hdr) + n, 16);
    Hdr* h = (Hdr*)malloc(sizeof(Hdr) + n);
    // Fog served most objects from fresh, zeroed blocks; malloc reuses freed
    // memory, so objects are zeroed as a precaution.
    if (h) memset(h + 1, 0, n);
    return h;
}
void raw_free(Hdr* h) {
    if (in_region(h->size)) hs_vm_release(h, sizeof(Hdr) + h->size);
    else free(h);
}
void counted(Hdr* h, size_t n) {
    h->size = (uint32_t)n;
    g_live += n;
    g_count++;
    if ((uintptr_t)(h + 1) & 15) g_under16++;
    if (g_live > g_peak) g_peak = g_live;
}

// Takes a request when it's ours to serve; null means "Fog's".
void* take(D2PoolManagerStrc* m, size_t n) {
    if (!on() || n <= kExactAbove || !sys().bInit) return nullptr;
    int list = manager_index(m);
    if (list < 0) return nullptr;
    // A manager with a pool id (the global one while a game loads) sends
    // everything to malloc itself, exact-size already; only its big blocks are
    // taken, to keep them out of malloc's heap.
    if (((m && m->nPoolId) || (!m && sys().pManagers[0].nPoolId)) && !in_region(n)) return nullptr;
    Hdr* h = raw_alloc(n);
    if (!h) return nullptr;  // let Fog try (and report) instead
    counted(h, n);
    link(h, list);
    return h + 1;
}
void give_back(Hdr* h) {
    g_live -= h->size;
    g_count--;
    unlink(h);
    raw_free(h);
}
// The slot size of a block in one of manager m's pools (Fog's
// FindBlockContainingAddress over its sorted block table), or 0.
size_t pool_slot_size(D2PoolManagerStrc* m, void* p) {
    if (!m) m = sys().pManagers;
    int lo = 0, hi = (int)m->nBlocks;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((char*)p >= (char*)m->pBlocks[mid].pCommit) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return 0;
    D2PoolBlockStrc* block = m->pBlocks[lo - 1].pBlock;
    if (!block || !block->pPool || (char*)p >= (char*)block->pCommit + block->pPool->nSize) return 0;
    return block->pPool->nBlockSize;
}
D2PoolManagerStrc* server_manager(D2PoolManagerStrc* m) { return Fog::Memory::gpServerMemHeap ? m : nullptr; }
}  // namespace

void hs_mem_report_fog_exact(void) {
    if (g_on == 1)
        hs_log("  fog exact-size: %u allocs, %u KiB live, %u KiB peak, %u ever under 16-byte aligned", (unsigned)g_count,
               (unsigned)(g_live >> 10), (unsigned)(g_peak >> 10), (unsigned)g_under16);
}

namespace Fog::Memory {
HS_FOG_CC void* Alloc(D2PoolManagerStrc* m, size_t n, char* file, int32_t line) {
    if (void* p = take(m, n)) return p;
    return hs_orig_Alloc(m, n, file, line);
}
HS_FOG_CC void* AllocClientMemory(size_t n, char* file, int32_t line, int32_t z) {
    if (void* p = take(nullptr, n)) return p;
    return hs_orig_AllocClient(n, file, line, z);
}
HS_FOG_CC void* AllocServerMemory(D2PoolManagerStrc* m, size_t n, char* file, int32_t line, int32_t z) {
    if (void* p = take(server_manager(m), n)) return p;
    return hs_orig_AllocServer(m, n, file, line, z);
}
HS_FOG_CC void* ReAllocServerMemory(D2PoolManagerStrc* m, void* p, size_t n, char* file, int line, int z) {
    Hdr* h = ours(p);
    if (!h) {
        // A pooled block outgrowing its slot: Fog would move it to a bigger size
        // class, whose blocks are 512 KiB from 1 KiB up. Move it to an exact one.
        if (p && g_on == 1 && n > kExactAbove) {
            D2PoolManagerStrc* pm = server_manager(m);
            size_t slot = pool_slot_size(pm, p);
            if (slot && n > slot) {
                if (void* q = take(pm, n)) {
                    memcpy(q, p, slot);
                    hs_orig_FreeServer(m, p, file, line, z);
                    return q;
                }
            }
        }
        return hs_orig_ReAllocServer(m, p, n, file, line, z);
    }
    if (n <= h->size && !in_region(h->size)) {  // a malloc block shrinks in place (a region's size must stay whole)
        g_live -= h->size - n;
        h->size = (uint32_t)n;
        return p;
    }
    Hdr* r = raw_alloc(n);
    if (!r) return nullptr;  // the old block is untouched, as Fog would leave it
    counted(r, n);
    r->next = h->next;  // into the old block's list, just after it
    r->prev = h;
    h->next->prev = r;
    h->next = r;
    r->check = check_of(r);
    memcpy(r + 1, h + 1, h->size < n ? h->size : n);
    give_back(h);
    return r + 1;
}
HS_FOG_CC int FreeClientMemory(void* p, char* file, int32_t line, int32_t z) {
    if (Hdr* h = ours(p)) { give_back(h); return 1; }
    return hs_orig_FreeClient(p, file, line, z);
}
HS_FOG_CC int FreeServerMemory(D2PoolManagerStrc* m, void* p, char* file, int32_t line, int32_t z) {
    if (Hdr* h = ours(p)) { give_back(h); return 1; }
    return hs_orig_FreeServer(m, p, file, line, z);
}
void FreeMemoryPool(D2PoolManagerStrc* m) {
    int list = g_on == 1 && sys().bInit ? manager_index(m) : -1;
    if (list >= 0) {
        Hdr* head = &g_lists[list];
        while (head->next != head) give_back(head->next);
    }
    hs_orig_FreeMemoryPool(m);
}
}  // namespace Fog::Memory
