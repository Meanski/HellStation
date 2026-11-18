// Fog's pool allocator, exact-size above 256 bytes on a small-RAM target.
//
// Fog rounds every request up to a power of two and carves slots from 64 KiB
// and 512 KiB blocks that it keeps while any slot in them lives. In the Rogue
// Encampment that was 3.5 MB of the Client pool's committed blocks unused, plus
// the rounding (PORTING_LOG, 2026-11-18). On the PS2 (the game is told 32 MB),
// requests over kExactAbove bytes skip the pools and get exactly their size
// from malloc behind a 16-byte header; smaller ones, which pack densely, stay
// in Fog's pools.
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

// Takes a request when it's ours to serve; null means "Fog's".
void* take(D2PoolManagerStrc* m, size_t n) {
    if (!on() || n <= kExactAbove || !sys().bInit) return nullptr;
    int list = manager_index(m);
    if (list < 0 || (m && m->nPoolId) || (!m && sys().pManagers[0].nPoolId)) return nullptr;
    Hdr* h = (Hdr*)malloc(sizeof(Hdr) + n);
    if (!h) return nullptr;  // let Fog try (and report) instead
    h->size = (uint32_t)n;
    link(h, list);
    g_live += n;
    g_count++;
    if ((uintptr_t)(h + 1) & 15) g_under16++;
    if (g_live > g_peak) g_peak = g_live;
    return h + 1;
}
void give_back(Hdr* h) {
    g_live -= h->size;
    g_count--;
    unlink(h);
    free(h);
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
    if (!h) return hs_orig_ReAllocServer(m, p, n, file, line, z);
    Hdr* next = h->next;  // the list node moves with the block
    unlink(h);
    Hdr* r = (Hdr*)realloc(h, sizeof(Hdr) + n);
    bool ok = r != nullptr;
    if (!ok) r = h;  // the old block is untouched: relink it and fail as Fog would
    r->next = next;
    r->prev = next->prev;
    next->prev->next = r;
    next->prev = r;
    r->check = check_of(r);
    if (!ok) return nullptr;
    g_live = g_live - r->size + n;
    r->size = (uint32_t)n;
    if (g_live > g_peak) g_peak = g_live;
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
