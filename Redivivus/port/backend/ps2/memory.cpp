// EE memory layout: the C heap and the Win32 VirtualAlloc regions, without an MMU.
//
// RAM from _end to the top (EndOfHeap(), read once on the main thread during C
// library start-up) is used from both ends:
//   - malloc's program break (_sbrk, replacing libcglue's) grows up from _end;
//   - VirtualAlloc regions (hs_vm_reserve) are carved down from the top, and
//     freed regions go on a free list of their own (coalesced, first fit).
// The game's long-lived 64-512 KiB regions (Fog pool blocks, Storm heaps) and its
// many small short-lived mallocs then never cut up each other's space. In one
// heap they did: the town failed to load with 11 MB free in pieces too small
// for a 512 KiB Fog block (PORTING_LOG, 2026-11-18).
//
// Without HS_PS2_EXTENDED_RAM the top is held to a retail 32 MB even when
// PCSX2's Extended RAM offers more. -DHS_PS2_HEAP_LIMIT_KB caps it lower still.
// The EE never pre-empts a thread and nothing here yields, so no lock is needed.
#include <kernel.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "Redivivus/port/platform.h"

extern "C" char _ftext[], _end[];
extern "C" void hs_ee_stack_usage(size_t* total, size_t* deepest);  // backend/posix/threads.cpp

namespace {
constexpr uintptr_t kRetailTop = 0x02000000 - 0x20000;  // 32 MB less the main stack (128 KiB)

char* g_brk = _end;  // malloc's break, grows up
char* g_top;         // end of usable RAM
char* g_vmLow;       // regions occupy [g_vmLow, g_top), grow down
size_t g_vmLive, g_peak;

struct Run { char* p; size_t n; };
constexpr int kMaxRuns = 512;
Run g_free[kMaxRuns];  // free gaps inside [g_vmLow, g_top), sorted by address, coalesced
int g_nFree;

void init() {
    if (g_top) return;
    g_top = (char*)EndOfHeap();
#if !defined(HS_PS2_EXTENDED_RAM)
    if ((uintptr_t)g_top > kRetailTop) g_top = (char*)kRetailTop;
#endif
#if defined(HS_PS2_HEAP_LIMIT_KB)
    if (g_top > _end + ((size_t)HS_PS2_HEAP_LIMIT_KB << 10)) g_top = _end + ((size_t)HS_PS2_HEAP_LIMIT_KB << 10);
#endif
    g_vmLow = g_top;
}

size_t taken() { return (size_t)(g_brk - _end) + (size_t)(g_top - g_vmLow); }
void note_peak() { if (taken() > g_peak) g_peak = taken(); }

void out_of_memory(const char* who, size_t n) {
    static bool s_logged;
    if (s_logged) return;
    s_logged = true;
    hs_log("heap: %s(%u KiB) failed: break %u KiB, regions %u KiB (%u KiB live, %d free runs), %u KiB in all",
           who, (unsigned)(n >> 10), (unsigned)((g_brk - _end) >> 10), (unsigned)((g_top - g_vmLow) >> 10),
           (unsigned)(g_vmLive >> 10), g_nFree, (unsigned)((g_top - _end) >> 10));
}

void remove_run(int i) {
    memmove(&g_free[i], &g_free[i + 1], (size_t)(g_nFree - i - 1) * sizeof(Run));
    g_nFree--;
}
void add_run(char* p, size_t n) {
    if (!n) return;
    int i = 0;
    while (i < g_nFree && g_free[i].p < p) i++;
    if (i > 0 && g_free[i - 1].p + g_free[i - 1].n == p) {  // joins the run below
        g_free[i - 1].n += n;
        if (i < g_nFree && g_free[i - 1].p + g_free[i - 1].n == g_free[i].p) { g_free[i - 1].n += g_free[i].n; remove_run(i); }
    } else if (i < g_nFree && p + n == g_free[i].p) {        // joins the run above
        g_free[i].p = p;
        g_free[i].n += n;
    } else {
        if (g_nFree == kMaxRuns) {
            static bool s_logged;
            if (!s_logged) { s_logged = true; hs_log("heap: region free list full; a %u KiB gap is lost", (unsigned)(n >> 10)); }
            return;
        }
        memmove(&g_free[i + 1], &g_free[i], (size_t)(g_nFree - i) * sizeof(Run));
        g_free[i] = Run{p, n};
        g_nFree++;
    }
    // A gap at the bottom of the region area goes back to the space between the ends.
    while (g_nFree && g_free[0].p == g_vmLow) { g_vmLow += g_free[0].n; remove_run(0); }
}
}  // namespace

extern "C" void* _sbrk(ptrdiff_t incr) {
    init();
    if (incr == 0) return g_brk;
    char* next = g_brk + incr;
    if (next > g_vmLow || next < _end) { out_of_memory("sbrk", (size_t)incr); return (void*)-1; }
    char* ret = g_brk;
    g_brk = next;
    note_peak();
    return ret;
}

extern "C" void* hs_vm_reserve(size_t size, size_t align) {
    init();
    if (align < 16) align = 16;
    size = (size + 15) & ~(size_t)15;
    char* p = nullptr;
    for (int i = 0; i < g_nFree && !p; i++) {  // first fit in a freed gap
        Run r = g_free[i];
        char* a = (char*)(((uintptr_t)r.p + align - 1) & ~(uintptr_t)(align - 1));
        if (a + size > r.p + r.n) continue;
        remove_run(i);
        add_run(r.p, (size_t)(a - r.p));
        add_run(a + size, (size_t)(r.p + r.n - (a + size)));
        p = a;
    }
    if (!p) {  // carve a new one below the others
        char* a = (char*)(((uintptr_t)g_vmLow - size) & ~(uintptr_t)(align - 1));
        if ((uintptr_t)g_vmLow < size || a < g_brk) { out_of_memory("hs_vm_reserve", size); return nullptr; }
        char* oldLow = g_vmLow;
        g_vmLow = a;
        add_run(a + size, (size_t)(oldLow - (a + size)));  // the alignment gap, if any
        p = a;
    }
    g_vmLive += size;
    note_peak();
    if (size >= (1u << 20)) hs_log("hs_vm_reserve(%u KiB) -> %p", (unsigned)(size >> 10), p);
    return memset(p, 0, size);
}

extern "C" void hs_vm_decommit(void* p, size_t size) { memset(p, 0, size); }

extern "C" void hs_vm_release(void* p, size_t size) {
    size = (size + 15) & ~(size_t)15;
    g_vmLive -= size;
    add_run((char*)p, size);
}

extern "C" void hs_mem_query(hs_mem_info* out) {
    init();
    struct mallinfo mi = mallinfo();
    out->image = (size_t)(_end - _ftext);
    out->heap_taken = taken();
    out->heap_peak = g_peak;
    out->heap_in_use = (size_t)mi.uordblks + g_vmLive;
    out->heap_limit = (size_t)(g_top - _end);
    // A retail PS2, even when PCSX2's Extended RAM is lending more: the game sizes
    // its sprite, tile and sound caches from this (D2Client UI/Draw.cpp).
    out->phys_total = 32u << 20;
    hs_ee_stack_usage(&out->stacks, &out->stack_deepest);
}
