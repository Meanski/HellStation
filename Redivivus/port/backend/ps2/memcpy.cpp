// memcpy for the EE, replacing newlib's.
//
// newlib's generic memcpy copies a byte at a time whenever the source or the
// destination isn't word-aligned. The game's run-length blitters (floor tiles in
// D2gfx fpDrawVisTile, sprites in D2CMP FASTCMP_DecompressRLE) call memcpy once
// per run, at arbitrary pixel offsets: about 8,000 calls a frame in Harrogath,
// most under 16 bytes, which made memcpy a third of the busy time there
// (PORTING_LOG, 2026-11-19). The EE loads and stores unaligned doublewords with
// LDL/LDR and SDL/SDR pairs (what GCC emits for a packed 64-bit field), so this
// copies eight bytes at a time at any alignment, and finishes with one
// overlapping doubleword instead of a byte loop.
//
// hs_memcpy_selftest() compares it against a byte copy over every small size and
// alignment once at startup, and logs the result.
#include <cstddef>
#include <cstdint>

#include "Redivivus/port/platform.h"

namespace {
struct __attribute__((packed, may_alias)) U64 { uint64_t v; };
__attribute__((always_inline)) inline uint64_t load(const unsigned char* p) { return ((const U64*)p)->v; }
__attribute__((always_inline)) inline void store(unsigned char* p, uint64_t v) { ((U64*)p)->v = v; }
}  // namespace

// No loop-to-memcpy pattern matching here: it would call itself.
extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) void* memcpy(void* __restrict d,
                                                                                      const void* __restrict s,
                                                                                      size_t n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    if (n < 8) {
        switch (n) {  // a run of a few pixels: no loop
        case 7: dp[6] = sp[6]; [[fallthrough]];
        case 6: dp[5] = sp[5]; [[fallthrough]];
        case 5: dp[4] = sp[4]; [[fallthrough]];
        case 4: dp[3] = sp[3]; [[fallthrough]];
        case 3: dp[2] = sp[2]; [[fallthrough]];
        case 2: dp[1] = sp[1]; [[fallthrough]];
        case 1: dp[0] = sp[0]; [[fallthrough]];
        default: break;
        }
        return d;
    }
    unsigned char* const end = dp + n;
    const unsigned char* const send = sp + n;
    if (!(((uintptr_t)dp | (uintptr_t)sp) & 7)) {  // both aligned: plain doublewords, four at a time
        while (n >= 32) {
            uint64_t a = ((const uint64_t*)sp)[0], b = ((const uint64_t*)sp)[1];
            uint64_t c = ((const uint64_t*)sp)[2], e = ((const uint64_t*)sp)[3];
            ((uint64_t*)dp)[0] = a;
            ((uint64_t*)dp)[1] = b;
            ((uint64_t*)dp)[2] = c;
            ((uint64_t*)dp)[3] = e;
            dp += 32, sp += 32, n -= 32;
        }
        while (n >= 8) {
            *(uint64_t*)dp = *(const uint64_t*)sp;
            dp += 8, sp += 8, n -= 8;
        }
    } else {
        while (n >= 16) {
            uint64_t a = load(sp), b = load(sp + 8);
            store(dp, a);
            store(dp + 8, b);
            dp += 16, sp += 16, n -= 16;
        }
        if (n >= 8) {
            store(dp, load(sp));
            dp += 8, sp += 8, n -= 8;
        }
    }
    if (n) store(end - 8, load(send - 8));  // the last 0-7 bytes: one overlapping doubleword
    return d;
}

// newlib's memset stores a word at a time; in a fight decoding sprites it was
// the hottest function (8% of samples): every large allocation is zeroed. This
// one stores doublewords, four per loop.
extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) void* memset(void* d, int c, size_t n) {
    unsigned char* p = (unsigned char*)d;
    unsigned char b = (unsigned char)c;
    if (n >= 16) {
        while ((uintptr_t)p & 7) { *p++ = b; n--; }
        uint64_t v = b;  // the R5900 has no 64-bit multiply: spread the byte by shifts
        v |= v << 8;
        v |= v << 16;
        v |= v << 32;
        uint64_t* q = (uint64_t*)p;
        for (; n >= 32; n -= 32, q += 4) { q[0] = v; q[1] = v; q[2] = v; q[3] = v; }
        for (; n >= 8; n -= 8) *q++ = v;
        p = (unsigned char*)q;
    }
    while (n--) *p++ = b;
    return d;
}

void hs_memcpy_selftest(void) {
    static unsigned char src[96], dst[96], want[96];
    for (unsigned i = 0; i < sizeof src; i++) src[i] = (unsigned char)(i * 37 + 11);
    unsigned bad = 0;
    for (unsigned so = 0; so < 8; so++)
        for (unsigned doff = 0; doff < 8; doff++)
            for (unsigned n = 0; n <= 80; n++) {
                for (unsigned i = 0; i < sizeof dst; i++) dst[i] = want[i] = 0xEE;
                for (unsigned i = 0; i < n; i++) want[doff + i] = src[so + i];
                volatile size_t vn = n;  // a real call, not a builtin expansion
                memcpy(dst + doff, src + so, vn);
                for (unsigned i = 0; i < sizeof dst; i++)
                    if (dst[i] != want[i]) { bad++; break; }
            }
    hs_log("memcpy check: %s (%u of %u cases wrong)", bad ? "FAILED" : "ok", bad, 8u * 8u * 81u);
    bad = 0;
    for (unsigned doff = 0; doff < 8; doff++)
        for (unsigned n = 0; n <= 80; n++) {
            for (unsigned i = 0; i < sizeof dst; i++) dst[i] = want[i] = 0xEE;
            for (unsigned i = 0; i < n; i++) want[doff + i] = (unsigned char)(n + 0x5A);
            volatile size_t vn = n;
            memset(dst + doff, (int)(n + 0x5A) | 0x100, vn);  // high bits of c must be ignored
            for (unsigned i = 0; i < sizeof dst; i++)
                if (dst[i] != want[i]) { bad++; break; }
        }
    hs_log("memset check: %s (%u of %u cases wrong)", bad ? "FAILED" : "ok", bad, 8u * 81u);
}
