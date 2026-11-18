// Storm's small allocations, exact-size on a small-RAM target.
//
// Storm keeps a heap per allocating source line (SMemAlloc hashes file:line),
// and each heap reserves a 64 KiB window that it commits a page at a time. On
// the EE a reservation is real RAM: in town Storm held 2.2 MB of windows with
// 0.3 MB committed (PORTING_LOG, 2026-11-18). On a target reporting 32 MB or
// less, requests below Storm's large-block size get exactly their size from
// malloc behind an 8-byte header (Storm's blocks are 8-aligned; so are these).
// Large requests keep Storm's own regions, which are already exact.
//
// Wraps SMemFree and SMemReAlloc; SMemAlloc is wrapped in storm_align.cpp,
// which asks hs_storm_exact_alloc first. Frees and reallocs recognise a header
// by a check word tied to its address; anything else goes to Storm unchanged.
// Storm's flags: 8 zero-fills (on alloc, and the grown tail on realloc); 0x10
// on realloc means "only in place", which every caller follows with an
// alloc-copy-free when it fails. The EE never pre-empts a thread, so no lock.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/memstats.h"

#if defined(HS_FAKE_I386)
#  define HS_STORM_CC
#else
#  define HS_STORM_CC __attribute__((__stdcall__))
#endif
typedef unsigned long hs_dword;  // DWORD as the EE and i686 headers spell it

extern "C" HS_STORM_CC int32_t hs_orig_SMemFree(void*, char*, int32_t, hs_dword)
    __asm__("hs_orig__ZN5Storm6Source4SMem8SMemFreeEPvPcim");
extern "C" HS_STORM_CC uint32_t* hs_orig_SMemReAlloc(uint32_t*, uint32_t, char*, int32_t, uint32_t)
    __asm__("hs_orig__ZN5Storm6Source4SMem11SMemReAllocEPjjPcij");

namespace {
constexpr uint32_t kLargeBlock = 0xfe88;  // Storm's own regions from here up (SMemCalculateAllocationLayout)
constexpr uint32_t kCheck = 0x5709A11Cu;
constexpr uint32_t kZero = 8, kInPlaceOnly = 0x10;

struct Hdr {  // 8 bytes, so the user block keeps malloc's 8-alignment
    uint32_t check;  // (uintptr_t)this ^ kCheck
    uint32_t size;
};
static_assert(sizeof(Hdr) == 8, "8-byte header");

size_t g_live, g_peak, g_count;
int g_on = -1;
bool on() {
    if (g_on < 0) {
        hs_mem_info mi = {};
        hs_mem_query(&mi);
        g_on = mi.phys_total && mi.phys_total <= (32u << 20);
        if (g_on) hs_log("storm: exact-size allocations below %u bytes", (unsigned)kLargeBlock);
    }
    return g_on;
}
uint32_t check_of(Hdr* h) { return (uint32_t)(uintptr_t)h ^ kCheck; }

Hdr* ours(void* p) {
    if (!p || g_on != 1 || ((uintptr_t)p & 7)) return nullptr;
    Hdr* h = (Hdr*)p - 1;
    return h->check == check_of(h) && h->size < kLargeBlock ? h : nullptr;
}
void count(intptr_t bytes, int blocks) {
    g_live += bytes;
    g_count += blocks;
    if (g_live > g_peak) g_peak = g_live;
}
}  // namespace

// Null means "Storm's": not a small-RAM target, a large block, or malloc failed.
extern "C" void* hs_storm_exact_alloc(uint32_t n, uint32_t flags) {
    if (!on() || n >= kLargeBlock) return nullptr;
    Hdr* h = (Hdr*)malloc(sizeof(Hdr) + n);
    if (!h) return nullptr;
    h->check = check_of(h);
    h->size = n;
    if (flags & kZero) memset(h + 1, 0, n);
    count((intptr_t)n, 1);
    return h + 1;
}

void hs_mem_report_storm_exact(void) {
    if (g_on == 1)
        hs_log("  storm exact-size: %u allocs, %u KiB live, %u KiB peak", (unsigned)g_count,
               (unsigned)(g_live >> 10), (unsigned)(g_peak >> 10));
}

namespace Storm::Source::SMem {
HS_STORM_CC int32_t SMemFree(void* p, char* file, int32_t line, hs_dword flags) {
    if (Hdr* h = ours(p)) {
        count(-(intptr_t)h->size, -1);
        h->check = 0;
        free(h);
        return 1;
    }
    return hs_orig_SMemFree(p, file, line, flags);
}
HS_STORM_CC uint32_t* SMemReAlloc(uint32_t* p, uint32_t n, char* file, int32_t line, uint32_t flags) {
    Hdr* h = ours(p);
    if (!h) {
        if (!p) {  // as Storm: a null block is an allocation
            if (void* q = hs_storm_exact_alloc(n, flags)) return (uint32_t*)q;
        }
        return hs_orig_SMemReAlloc(p, n, file, line, flags);
    }
    uint32_t old = h->size;
    if (n <= old) {  // shrinking stays in place (the tail is kept, as malloc would)
        count(-(intptr_t)(old - n), 0);
        h->size = n;
        return p;
    }
    if (flags & kInPlaceOnly) return nullptr;  // the caller allocates, copies and frees
    void* q = hs_storm_exact_alloc(n, 0);
    if (!q) {  // too big for us now: hand it to Storm
        q = hs_orig_SMemReAlloc(nullptr, n, file, line, 0);
        if (!q) return nullptr;
    }
    memcpy(q, p, old);
    if (flags & kZero) memset((char*)q + old, 0, n - old);
    SMemFree(p, file, line, 0);
    return (uint32_t*)q;
}
}  // namespace Storm::Source::SMem
