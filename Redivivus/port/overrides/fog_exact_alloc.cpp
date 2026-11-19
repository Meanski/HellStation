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
#include <cstdio>
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
HS_FOG_CC void* hs_orig_AllocAndLoad(D2PoolManagerStrc*, char*, uint32_t*, char*, int32_t)
    HS_FOG_ORIG(_ZN6D2Hell25D2FILE_AllocAndLoadD2FileEP17D2PoolManagerStrcPcPjS2_i);
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
    uint32_t size : 24;  // requests of 16 MB and up stay Fog's
    uint32_t site : 8;   // index into g_sites: who asked
};
static_assert(sizeof(Hdr) == 16 || sizeof(void*) == 8, "header must be 16 bytes on 32-bit targets");

Hdr g_lists[kManagers];  // circular, one sentinel per pool manager
size_t g_live, g_peak, g_count, g_under16;  // g_under16: blocks malloc placed below 16-byte alignment

// Live bytes per call site (the file and line the game passes Fog), for the
// memory report: which tables and caches own the exact-size heap. Slot 0
// collects sites past the table's 255.
struct Site {
    const char* file;
    int32_t line;
    uint32_t count;
    size_t live, peak;
};
Site g_sites[256];
unsigned g_reports;

uint8_t site_of(const char* file, int32_t line) {
    unsigned h = ((unsigned)(uintptr_t)file ^ (unsigned)line * 2654435761u) >> 24;
    for (unsigned i = 0; i < 255; i++) {
        unsigned k = 1 + (h + i) % 255;
        Site& s = g_sites[k];
        if (s.file == file && s.line == line) return (uint8_t)k;
        if (!s.file && !s.line) {
            s.file = file ? file : "?";
            s.line = line;
            return (uint8_t)k;
        }
    }
    return 0;
}
// Whole-file buffers of 32 KiB and up still live, by name (D2FILE_AllocAndLoadD2File below).
struct BigFile {
    Hdr* h;
    char name[40];
};
BigFile g_files[96];
void file_forget(Hdr* h) {
    for (BigFile& f : g_files)
        if (f.h == h) f.h = nullptr;
}

void site_add(Hdr* h, size_t n) {
    Site& s = g_sites[h->site];
    s.live += n;
    s.count++;
    if (s.live > s.peak) s.peak = s.live;
}
void site_sub(Hdr* h, size_t n) {
    Site& s = g_sites[h->site];
    s.live -= n;
    s.count--;
}

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
void counted(Hdr* h, size_t n, uint8_t site) {
    h->size = (uint32_t)n;
    h->site = site;
    site_add(h, n);
    g_live += n;
    g_count++;
    if ((uintptr_t)(h + 1) & 15) g_under16++;
    if (g_live > g_peak) g_peak = g_live;
}

// Takes a request when it's ours to serve; null means "Fog's".
void* take(D2PoolManagerStrc* m, size_t n, const char* file, int32_t line) {
    if (!on() || n <= kExactAbove || n >= (16u << 20) || !sys().bInit) return nullptr;
    int list = manager_index(m);
    if (list < 0) return nullptr;
    // A manager with a pool id (the global one while a game loads) sends
    // everything to malloc itself, exact-size already; only its big blocks are
    // taken, to keep them out of malloc's heap.
    if (((m && m->nPoolId) || (!m && sys().pManagers[0].nPoolId)) && !in_region(n)) return nullptr;
    Hdr* h = raw_alloc(n);
    if (!h) return nullptr;  // let Fog try (and report) instead
    counted(h, n, site_of(file, line));
    link(h, list);
    return h + 1;
}
void give_back(Hdr* h) {
    g_live -= h->size;
    g_count--;
    site_sub(h, h->size);
    if (h->size >= (32u << 10)) file_forget(h);
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
    // The biggest owners, every fourth report: live KiB, blocks, peak KiB.
    if (g_on != 1 || g_reports++ % 4) return;
    int top[12];
    int ntop = 0;
    for (int k = 0; k < 256; k++) {
        if (!g_sites[k].live) continue;
        int i = ntop;
        if (ntop < 12) ntop++;
        else if (g_sites[top[11]].live >= g_sites[k].live) continue;
        else i = 11;
        while (i > 0 && g_sites[top[i - 1]].live < g_sites[k].live) { top[i] = top[i - 1]; i--; }
        top[i] = k;
    }
    for (int i = 0; i < ntop; i++) {
        const Site& s = g_sites[top[i]];
        const char* f = s.file ? s.file : "(sites past 255)";
        for (const char* c = f; *c; c++)
            if (*c == '/' || *c == '\\') f = c + 1;
        hs_log("    %6u KiB in %4u from %s:%d (peak %u KiB)", (unsigned)(s.live >> 10), (unsigned)s.count, f, (int)s.line,
               (unsigned)(s.peak >> 10));
    }
    char line[200];
    int len = 0;
    for (const BigFile& b : g_files) {
        if (!b.h) continue;
        len += snprintf(line + len, sizeof line - len, " %s %u", b.name, (unsigned)(b.h->size >> 10));
        if (len > 150) { hs_log("    files:%s", line); len = 0; }
    }
    if (len) hs_log("    files:%s", line);
}

namespace Fog::Memory {
HS_FOG_CC void* Alloc(D2PoolManagerStrc* m, size_t n, char* file, int32_t line) {
    if (void* p = take(m, n, file, line)) return p;
    return hs_orig_Alloc(m, n, file, line);
}
HS_FOG_CC void* AllocClientMemory(size_t n, char* file, int32_t line, int32_t z) {
    if (void* p = take(nullptr, n, file, line)) return p;
    return hs_orig_AllocClient(n, file, line, z);
}
HS_FOG_CC void* AllocServerMemory(D2PoolManagerStrc* m, size_t n, char* file, int32_t line, int32_t z) {
    if (void* p = take(server_manager(m), n, file, line)) return p;
    return hs_orig_AllocServer(m, n, file, line, z);
}
HS_FOG_CC void* ReAllocServerMemory(D2PoolManagerStrc* m, void* p, size_t n, char* file, int line, int z) {
    if (!p) {  // as Fog: a null block is an allocation
        if (void* q = take(server_manager(m), n, file, line)) return q;
        return hs_orig_ReAllocServer(m, p, n, file, line, z);
    }
    Hdr* h = ours(p);
    if (!h) {
        // A pooled block outgrowing its slot: Fog would move it to a bigger size
        // class, whose blocks are 512 KiB from 1 KiB up. Move it to an exact one.
        if (g_on == 1 && n > kExactAbove) {
            D2PoolManagerStrc* pm = server_manager(m);
            size_t slot = pool_slot_size(pm, p);
            if (slot && n > slot) {
                if (void* q = take(pm, n, file, line)) {
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
        site_sub(h, h->size);
        h->size = (uint32_t)n;
        site_add(h, n);
        return p;
    }
    if (n >= (16u << 20)) return nullptr;
    Hdr* r = raw_alloc(n);
    if (!r) return nullptr;  // the old block is untouched, as Fog would leave it
    counted(r, n, site_of(file, line));
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

// D2Hell's whole-file loader allocates every buffer from one line of its own
// (Archive.cpp:207) and drops its caller's file and line; the wrapper puts the
// caller's on the block, and logs the first big loads by name.
namespace D2Hell {
HS_FOG_CC void* D2FILE_AllocAndLoadD2File(D2PoolManagerStrc* m, char* name, uint32_t* size, char* file, int32_t line) {
    uint32_t n = 0;
    void* p = hs_orig_AllocAndLoad(m, name, &n, file, line);
    if (size) *size = n;
    if (Hdr* h = ours(p)) {
        site_sub(h, h->size);
        h->site = site_of(file, line);
        site_add(h, h->size);
        if (h->size >= (32u << 10))
            for (BigFile& f : g_files)
                if (!f.h) {
                    const char* b = name ? name : "?";
                    for (const char* c = b; *c; c++)
                        if (*c == '\\' || *c == '/') b = c + 1;
                    f.h = h;
                    snprintf(f.name, sizeof f.name, "%s", b);
                    break;
                }
    }
    static unsigned logged;
    if (p && n >= (32u << 10) && logged < 400) {
        logged++;
        hs_log("file: %u KiB %s for %s:%d", (unsigned)(n >> 10), name ? name : "?", file ? file : "?", (int)line);
    }
    return p;
}
}  // namespace D2Hell
