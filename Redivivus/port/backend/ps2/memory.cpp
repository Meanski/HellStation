// EE memory: one heap for malloc and the Win32 VirtualAlloc regions, without an MMU.
//
// RAM from _end to the top (EndOfHeap(), read once on the main thread during C
// library start-up) is a single pool, managed here in the TLSF style: blocks
// carry an 8-byte header (previous block, size and two flags) so payloads are
// 16-byte aligned; free blocks sit on segregated lists, 16 classes per power of
// two, found through two bitmaps in constant time; a freed block merges with
// free neighbours at once. malloc and its family (the whole newlib set, so no
// libc member is linked) and the region calls (hs_vm_reserve: memalign plus
// zeroing) share it.
//
// History: newlib's malloc in one heap with the regions left the town unable to
// load with 11 MB free in pieces (2026-11-18); splitting the RAM between malloc
// growing up and regions carved down fixed that, but then neither side could use
// the other's free memory, and fights died with 1.25 MB free in 25 gaps on one
// side and malloc's free lists on the other (PORTING_LOG, 2026-11-19).
//
// Without HS_PS2_EXTENDED_RAM the top is held to a retail 32 MB even when
// PCSX2's Extended RAM offers more. -DHS_PS2_HEAP_LIMIT_KB caps it lower still.
// Threads aren't time-sliced, but an interrupt that wakes a higher-priority
// thread (the vertical-blank handler does) pre-empts whoever is running, so the
// pool is changed only with interrupts off (Locked). Logging happens after.
#include <kernel.h>
#include <malloc.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "Redivivus/port/platform.h"

extern "C" char _ftext[], _end[];
extern "C" void hs_ee_stack_usage(size_t* total, size_t* deepest);  // backend/posix/threads.cpp

namespace {
constexpr uintptr_t kRetailTop = 0x02000000 - 0x20000;  // 32 MB less the main stack (128 KiB)

constexpr size_t kHdr = 8;        // prevPhys + sizeFlags; blocks start at 8 mod 16
constexpr size_t kMinBlock = 16;  // a header and the two free-list links
constexpr unsigned kSlLog = 4, kSlCount = 1u << kSlLog;
constexpr unsigned kSmallLog = 8;  // blocks under 256 bytes: one class per 16 bytes
constexpr unsigned kFlCount = 20;  // first level 0 (small), 1 (256 B) ... 17 (16 MB) and spare
constexpr uint32_t kFree = 1, kPrevFree = 2;

struct Block {
    Block* prevPhys;     // the block below; valid while it is free (kPrevFree)
    uint32_t sizeFlags;  // whole block size, a multiple of 16, | kFree | kPrevFree
    Block* nextFree;     // the payload's first words, while this block is free
    Block* prevFree;
    size_t size() const { return sizeFlags & ~(uint32_t)15; }
    bool isFree() const { return sizeFlags & kFree; }
    Block* next() { return (Block*)((char*)this + size()); }
    void* payload() { return (char*)this + kHdr; }
};
static_assert(offsetof(Block, nextFree) == kHdr, "the payload starts after the header");

char* g_base;  // the pool: [g_base, g_top)
char* g_top;
Block* g_sentinel;  // a used block of size 0 at the top: nothing merges past it
Block* g_heads[kFlCount][kSlCount];
uint32_t g_flMap, g_slMap[kFlCount];
size_t g_used, g_peak, g_vmLive;

struct Locked {  // interrupts off: no pre-emption, and nesting is harmless
    int was = DIntr();
    ~Locked() { if (was) EIntr(); }
};

Block* from_payload(void* p) { return (Block*)((char*)p - kHdr); }
unsigned fls(uint32_t x) { return 31u - (unsigned)__builtin_clz(x); }

void mapping(size_t s, unsigned& fl, unsigned& sl) {
    if (s < (1u << kSmallLog)) {
        fl = 0;
        sl = (unsigned)(s >> 4);
    } else {
        unsigned f = fls((uint32_t)s);
        sl = (unsigned)(s >> (f - kSlLog)) ^ kSlCount;
        fl = f - (kSmallLog - 1);
    }
}
void insert(Block* b) {
    unsigned fl, sl;
    mapping(b->size(), fl, sl);
    Block* head = g_heads[fl][sl];
    b->nextFree = head;
    b->prevFree = nullptr;
    if (head) head->prevFree = b;
    g_heads[fl][sl] = b;
    g_flMap |= 1u << fl;
    g_slMap[fl] |= 1u << sl;
}
void remove(Block* b) {
    unsigned fl, sl;
    mapping(b->size(), fl, sl);
    if (b->nextFree) b->nextFree->prevFree = b->prevFree;
    if (b->prevFree) b->prevFree->nextFree = b->nextFree;
    else {
        g_heads[fl][sl] = b->nextFree;
        if (!b->nextFree) {
            g_slMap[fl] &= ~(1u << sl);
            if (!g_slMap[fl]) g_flMap &= ~(1u << fl);
        }
    }
}
// A free block of at least s bytes, or null. Above the small sizes, the best
// fit in s's own class first (a short scan: big free blocks are few), then the
// first non-empty class above it, whose blocks all fit (TLSF's good fit alone
// passes over same-class blocks that fit and splits bigger ones instead).
Block* find(size_t s) {
    if (s >= (1u << kSmallLog)) {
        unsigned fl0, sl0;
        mapping(s, fl0, sl0);
        Block* best = nullptr;
        int scanned = 0;
        for (Block* b = fl0 < kFlCount ? g_heads[fl0][sl0] : nullptr; b && scanned < 64; b = b->nextFree, scanned++)
            if (b->size() >= s && (!best || b->size() < best->size())) best = b;
        if (best) return best;
    }
    if (s >= (1u << kSmallLog)) s += (1u << (fls((uint32_t)s) - kSlLog)) - 1;
    unsigned fl, sl;
    mapping(s, fl, sl);
    if (fl >= kFlCount) return nullptr;
    uint32_t slMap = g_slMap[fl] & (~0u << sl);
    if (!slMap) {
        uint32_t flMap = fl + 1 < 32 ? g_flMap & (~0u << (fl + 1)) : 0;
        if (!flMap) return nullptr;
        fl = (unsigned)__builtin_ctz(flMap);
        slMap = g_slMap[fl];
    }
    return g_heads[fl][(unsigned)__builtin_ctz(slMap)];
}

void init() {
    if (g_top) return;
    char* top = (char*)EndOfHeap();
#if !defined(HS_PS2_EXTENDED_RAM)
    if ((uintptr_t)top > kRetailTop) top = (char*)kRetailTop;
#endif
#if defined(HS_PS2_HEAP_LIMIT_KB)
    if (top > _end + ((size_t)HS_PS2_HEAP_LIMIT_KB << 10)) top = _end + ((size_t)HS_PS2_HEAP_LIMIT_KB << 10);
#endif
    g_base = (char*)(((uintptr_t)_end + 15) & ~(uintptr_t)15);
    g_top = (char*)((uintptr_t)top & ~(uintptr_t)15);
    Block* first = (Block*)(g_base + kHdr);
    g_sentinel = (Block*)(g_top - kHdr);
    first->prevPhys = nullptr;
    first->sizeFlags = (uint32_t)((char*)g_sentinel - (char*)first) | kFree;
    g_sentinel->prevPhys = first;
    g_sentinel->sizeFlags = kPrevFree;  // size 0, in use
    insert(first);
}

size_t block_size(size_t n) {
    size_t bs = (n + kHdr + 15) & ~(size_t)15;
    return bs < kMinBlock ? kMinBlock : bs;
}
// The largest free block: what the next big request can get.
size_t largest_free() {
    if (!g_flMap) return 0;
    unsigned fl = fls(g_flMap);
    size_t best = 0;
    for (Block* b = g_heads[fl][fls(g_slMap[fl])]; b; b = b->nextFree)
        if (b->size() > best) best = b->size();
    return best > kHdr ? best - kHdr : 0;
}
size_t pool_size() { return (size_t)(g_top - g_base); }
size_t taken() { return pool_size() - largest_free(); }  // all but the biggest hole: fragmentation shows here

void* g_caller;  // who called the allocator last (for the failure log)

void out_of_memory(const char* who, size_t n) {
    // Every failure up to 20, then every 100th: a caller that retries shows up.
    static unsigned s_count;
    if (++s_count > 20 && s_count % 100) return;
    size_t used, largest;
    {
        Locked l;
        used = g_used;
        largest = largest_free();
    }
    hs_log("heap: failure %u: %s(%u KiB) from %p failed: %u KiB in use, %u KiB free, largest free block %u KiB, %u KiB in all",
           s_count, who, (unsigned)(n >> 10), g_caller, (unsigned)(used >> 10), (unsigned)((pool_size() - used) >> 10),
           (unsigned)(largest >> 10), (unsigned)(pool_size() >> 10));
}

// Give the tail of used block b past bs bytes back, merging it with a free
// block above.
void trim(Block* b, size_t bs) {
    size_t rest = b->size() - bs;
    if (rest < kMinBlock) return;
    b->sizeFlags = (uint32_t)bs | (b->sizeFlags & kPrevFree);
    g_used -= rest;
    Block* r = (Block*)((char*)b + bs);
    r->sizeFlags = (uint32_t)rest | kFree;
    Block* n = r->next();
    if (n->isFree()) {
        remove(n);
        r->sizeFlags += (uint32_t)n->size();
        n = r->next();
    }
    n->prevPhys = r;
    n->sizeFlags |= kPrevFree;
    insert(r);
}
// Take free block b off its list and mark it used.
void take(Block* b) {
    remove(b);
    b->sizeFlags &= ~kFree;
    b->next()->sizeFlags &= ~kPrevFree;
    g_used += b->size();
    if (g_used > g_peak) g_peak = g_used;
}

void* heap_alloc(size_t n) {
    {
        Locked l;
        init();
        Block* b = n <= pool_size() ? find(block_size(n)) : nullptr;
        if (b) {
            take(b);
            trim(b, block_size(n));
            return b->payload();
        }
    }
    out_of_memory("malloc", n);
    return nullptr;
}
void heap_free(void* p) {
    if (!p) return;
    Locked l;
    Block* b = from_payload(p);
    g_used -= b->size();
    b->sizeFlags |= kFree;
    if (b->sizeFlags & kPrevFree) {
        Block* below = b->prevPhys;
        remove(below);
        below->sizeFlags += (uint32_t)b->size();
        b = below;
    }
    Block* n = b->next();
    if (n->isFree()) {
        remove(n);
        b->sizeFlags += (uint32_t)n->size();
        n = b->next();
    }
    n->prevPhys = b;
    n->sizeFlags |= kPrevFree;
    insert(b);
}
void* carve_aligned(Block* b, size_t align, size_t bs);
void* heap_memalign(size_t align, size_t n) {
    if (align <= 16) return heap_alloc(n);
    if (align & (align - 1)) return nullptr;
    {
        Locked l;
        init();
        Block* b = n <= pool_size() ? find(block_size(n) + align + kMinBlock) : nullptr;  // room for a leading gap
        if (b) return carve_aligned(b, align, block_size(n));
    }
    out_of_memory("memalign", n);
    return nullptr;
}
void* carve_aligned(Block* b, size_t align, size_t bs) {
    take(b);
    uintptr_t p = (uintptr_t)b->payload();
    uintptr_t a = (p + align - 1) & ~(uintptr_t)(align - 1);
    if (a != p && a - p < kMinBlock) a += align;
    if (a != p) {  // the gap below becomes a free block of its own
        size_t gap = a - p;
        Block* nb = (Block*)(a - kHdr);
        nb->sizeFlags = (uint32_t)(b->size() - gap);  // used, and the block below it is free
        b->sizeFlags = (uint32_t)gap | kFree | (b->sizeFlags & kPrevFree);
        nb->prevPhys = b;
        nb->sizeFlags |= kPrevFree;
        g_used -= gap;
        insert(b);
        b = nb;
    }
    trim(b, bs);
    return b->payload();
}
void* heap_realloc(void* p, size_t n) {
    if (!p) return heap_alloc(n);
    if (!n) { heap_free(p); return nullptr; }
    Block* b = from_payload(p);
    size_t bs = block_size(n), cur;
    {
        Locked l;
        cur = b->size();
        if (bs <= cur) { trim(b, bs); return p; }
        Block* nx = b->next();
        if (nx->isFree() && cur + nx->size() >= bs) {  // grow into the free block above
            remove(nx);
            b->sizeFlags += (uint32_t)nx->size();
            b->next()->sizeFlags &= ~kPrevFree;
            g_used += nx->size();
            if (g_used > g_peak) g_peak = g_used;
            trim(b, bs);
            return p;
        }
    }
    void* q = heap_alloc(n);
    if (!q) return nullptr;
    memcpy(q, p, cur - kHdr);
    heap_free(p);
    return q;
}
size_t heap_usable(void* p) { return p ? from_payload(p)->size() - kHdr : 0; }
}  // namespace

// ---- The newlib allocator interface, all of it ----------------------------------
extern "C" {
void* malloc(size_t n) {
    g_caller = __builtin_return_address(0);
    return heap_alloc(n);
}
void free(void* p) { heap_free(p); }
void* realloc(void* p, size_t n) { return heap_realloc(p, n); }
void* calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) return nullptr;
    void* p = heap_alloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void* memalign(size_t align, size_t n) {
    g_caller = __builtin_return_address(0);
    return heap_memalign(align, n);
}
void* aligned_alloc(size_t align, size_t n) { return heap_memalign(align, n); }
int posix_memalign(void** out, size_t align, size_t n) {
    void* p = heap_memalign(align, n);
    if (!p) return 12;  // ENOMEM
    *out = p;
    return 0;
}
void* valloc(size_t n) { return heap_memalign(4096, n); }
void* pvalloc(size_t n) { return heap_memalign(4096, (n + 4095) & ~(size_t)4095); }
size_t malloc_usable_size(void* p) { return heap_usable(p); }
struct mallinfo mallinfo(void) {
    Locked l;
    init();
    struct mallinfo mi = {};
    mi.arena = pool_size();
    mi.uordblks = g_used;
    mi.fordblks = pool_size() - g_used;
    mi.usmblks = g_peak;
    return mi;
}
void malloc_stats(void) {}
int mallopt(int, int) { return 0; }
int malloc_trim(size_t) { return 0; }

void* _malloc_r(struct _reent*, size_t n) { return heap_alloc(n); }
void _free_r(struct _reent*, void* p) { heap_free(p); }
void* _realloc_r(struct _reent*, void* p, size_t n) { return heap_realloc(p, n); }
void* _calloc_r(struct _reent*, size_t count, size_t size) { return calloc(count, size); }
void* _memalign_r(struct _reent*, size_t align, size_t n) { return heap_memalign(align, n); }
void* _valloc_r(struct _reent*, size_t n) { return valloc(n); }
void* _pvalloc_r(struct _reent*, size_t n) { return pvalloc(n); }
size_t _malloc_usable_size_r(struct _reent*, void* p) { return heap_usable(p); }
struct mallinfo _mallinfo_r(struct _reent*) { return mallinfo(); }
void _malloc_stats_r(struct _reent*) {}
int _mallopt_r(struct _reent*, int, int) { return 0; }
int _malloc_trim_r(struct _reent*, size_t) { return 0; }

// Nothing should grow a program break any more: everything is in the pool.
void* _sbrk(ptrdiff_t incr) {
    if (incr == 0) return _end;
    out_of_memory("sbrk", (size_t)incr);
    return (void*)-1;
}
}  // extern "C"

// ---- Regions (VirtualAlloc) -------------------------------------------------------
extern "C" void* hs_vm_reserve(size_t size, size_t align) {
    g_caller = __builtin_return_address(0);
    size = (size + 15) & ~(size_t)15;
    void* p = heap_memalign(align < 16 ? 16 : align, size);
    if (!p) return nullptr;
    g_vmLive += size;
    if (size >= (1u << 20)) hs_log("hs_vm_reserve(%u KiB) -> %p", (unsigned)(size >> 10), p);
    return memset(p, 0, size);
}

extern "C" void hs_vm_decommit(void* p, size_t size) { memset(p, 0, size); }

extern "C" void hs_vm_release(void* p, size_t size) {
    size = (size + 15) & ~(size_t)15;
    g_vmLive -= size;
    heap_free(p);
}

extern "C" void hs_mem_query(hs_mem_info* out) {
    {
        Locked l;
        init();
        out->heap_taken = taken();
        out->heap_in_use = g_used;
        out->heap_limit = pool_size();
    }
    out->image = (size_t)(_end - _ftext);
    static size_t s_takenPeak;
    if (out->heap_taken > s_takenPeak) s_takenPeak = out->heap_taken;
    out->heap_peak = s_takenPeak;
    // A retail PS2, even when PCSX2's Extended RAM is lending more: the game sizes
    // its sprite, tile and sound caches from this (D2Client UI/Draw.cpp).
    out->phys_total = 32u << 20;
    hs_ee_stack_usage(&out->stacks, &out->stack_deepest);
}
