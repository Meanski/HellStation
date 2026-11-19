// Time spent decoding DCC sprites (D2CMP's CODEC_DecompressAllFrames), per
// decode and per KiB produced, logged every 256 decodes. How often the game
// decodes depends on the sprite cache and on timing (the same scripted walk
// gave 45 and 205 ms frames), but the cost of a decode doesn't: this is the
// figure to compare when the decoder is changed. A port wrap
// (cmake/port.cmake, HS_PORT_WRAPS).
//
// It's also where decodes meet the second-level cache (platform.h hs_l2_*: on
// the PS2, the IOP's spare memory): a decode is looked up there first, by its
// file, frame range and compressed bytes, and a hit is copied back as the
// decoder would have left it (its output, allocated from the same pool with
// the same tag; the per-direction offsets it writes into the decode state; the
// size). A miss decodes and stores the result. The first 64 hits are decoded
// anyway and compared byte for byte.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Redivivus/port/platform.h"

struct D2PoolManagerStrc;
struct D2CelDecodeStateStrc;
#if defined(HS_FAKE_I386)
#  define HS_CODEC_CC
#else
#  define HS_CODEC_CC __attribute__((__fastcall__))
#endif
extern "C" HS_CODEC_CC void* hs_orig_CODEC_DecompressAllFrames(uint32_t*, D2PoolManagerStrc*, int32_t, uint8_t*,
                                                              D2CelDecodeStateStrc*, int32_t, int32_t, size_t*, char*)
    __asm__("hs_orig__ZN5D2CMP3SRC5Codec25CODEC_DecompressAllFramesEPjP17D2PoolManagerStrciPhP20D2CelDecodeStateStrciiS2_Pc");

extern "C" HS_CODEC_CC int hs_FreeServerMemory(D2PoolManagerStrc*, void*, char*, int32_t, int32_t)
    __asm__("_ZN3Fog6Memory16FreeServerMemoryEP17D2PoolManagerStrcPvPcii");
extern bool hs_dcc_whole_clear;  // overrides/dcc_clear.cpp
extern "C" HS_CODEC_CC void* hs_AllocServerMemory(D2PoolManagerStrc*, size_t, char*, int32_t, int32_t)
    __asm__("_ZN3Fog6Memory17AllocServerMemoryEP17D2PoolManagerStrcjPcii");

namespace {
#if defined(_EE)
constexpr uint64_t kTicksPerMs = 294912;  // the EE's Count register runs at the CPU clock
uint32_t now() {
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#else
constexpr uint64_t kTicksPerMs = 1;
uint32_t now() { return hs_ticks_ms(); }
#endif
unsigned g_decodes;
// Which files were decoded since the last line: the most-decoded name says
// what keeps falling out of the sprite cache.
struct Name { char s[32]; unsigned n; };
Name g_names[48];
void count_name(const char* f) {
    if (!f) return;
    const char* b = f;
    for (const char* c = f; *c; c++)
        if (*c == '\\' || *c == '/') b = c + 1;
    Name* free_slot = nullptr;
    for (Name& n : g_names) {
        if (n.n && !strncmp(n.s, b, sizeof n.s - 1)) { n.n++; return; }
        if (!n.n && !free_slot) free_slot = &n;
    }
    if (free_slot) { snprintf(free_slot->s, sizeof free_slot->s, "%s", b); free_slot->n = 1; }
}
void log_names() {
    char line[220];
    int len = 0;
    for (int k = 0; k < 6; k++) {
        Name* best = nullptr;
        for (Name& n : g_names)
            if (n.n && (!best || n.n > best->n)) best = &n;
        if (!best) break;
        len += snprintf(line + len, sizeof line - len, " %s %u", best->s, best->n);
        best->n = 0;
    }
    for (Name& n : g_names) n.n = 0;
    if (len) hs_log("dcc: most decoded:%s", line);
}
uint64_t g_ticks, g_inBytes, g_outBytes;

uint32_t fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
bool make_key(uint32_t key[8], const uint32_t* data, int32_t readSize, int32_t start, int32_t count, const char* name) {
    if (!name || !data || readSize <= 0 || count <= 0 || count > 64) return false;
    key[0] = fnv(name, strlen(name));
    key[1] = (uint32_t)start;
    key[2] = (uint32_t)count;
    key[3] = (uint32_t)readSize;
    size_t n = readSize < 64 ? (size_t)readSize : 64;
    key[4] = fnv(data, n);
    key[5] = fnv((const uint8_t*)data + readSize - n, n);
    key[6] = fnv((const uint8_t*)data + readSize / 2, readSize / 2 < 32 ? 0 : 32);
    key[7] = 1;  // layout version: output, then the direction offsets
    return true;
}
struct HitCtx { D2PoolManagerStrc* pool; void* out; uint32_t size; };
void* alloc_like_decoder(void* c, uint32_t size) {
    HitCtx* h = (HitCtx*)c;
    h->size = size;
    h->out = hs_AllocServerMemory(h->pool, size, (char*)".\\SRC\\Codec.cpp", 0x561, 0);
    return h->out;
}
unsigned g_l2Hits, g_l2Misses, g_l2Checked, g_l2Differ;
uint64_t g_l2HitTicks, g_l2HitBytes, g_l2StoreTicks;
}  // namespace

namespace D2CMP::SRC::Codec {
HS_CODEC_CC void* CODEC_DecompressAllFrames(uint32_t* pdwCompressedData, D2PoolManagerStrc* pMemory, int32_t nReadSize,
                                            uint8_t* pCelHeader, D2CelDecodeStateStrc* pDecodeState, int32_t nStartFrame,
                                            int32_t nFrameCount, size_t* pnSize, char* szFileName) {
    uint32_t key[8];
    bool keyed = pnSize && pDecodeState && make_key(key, pdwCompressedData, nReadSize, nStartFrame, nFrameCount, szFileName);
    uint32_t* dirs = keyed ? (uint32_t*)((char*)pDecodeState + 0xc + nStartFrame * 4) : nullptr;
    static unsigned checkLimit = 64;
    if (keyed) {
        HitCtx hit = {pMemory, nullptr, 0};
        uint32_t th = now();
        int got = hs_l2_fetch(key, alloc_like_decoder, &hit);
        if (got == 1) { g_l2HitTicks += (uint32_t)(now() - th); g_l2HitBytes += hit.size; }
        uint32_t dirBytes = (uint32_t)nFrameCount * 4;
        if (got != 0) {
            // A hit: the entry is the output, then the direction offsets (the
            // block is dirBytes longer than the output the game is told
            // about). A broken one (overwritten while read) goes back and the
            // sprite is decoded.
            uint8_t* out = (uint8_t*)hit.out;
            if (got == 1 && hit.size > dirBytes) {
                uint32_t outBytes = hit.size - dirBytes;
                memcpy(dirs, out + outBytes, dirBytes);
                *pnSize = outBytes;
                if (++g_l2Hits % 512 == 0)
                    hs_log("dcc cache: %u hits, %u misses; a hit %u us (%u KiB), a store %u us", g_l2Hits, g_l2Misses,
                           (unsigned)(g_l2HitTicks * 1000 / kTicksPerMs / g_l2Hits),
                           (unsigned)((g_l2HitBytes >> 10) / g_l2Hits),
                           (unsigned)(g_l2Misses ? g_l2StoreTicks * 1000 / kTicksPerMs / g_l2Misses : 0));
                if (g_l2Checked < checkLimit) {  // decode it anyway and compare
                    size_t n2 = 0;
                    uint32_t dirsHit[64];
                    memcpy(dirsHit, dirs, dirBytes);
                    void* r2 = hs_orig_CODEC_DecompressAllFrames(pdwCompressedData, pMemory, nReadSize, pCelHeader,
                                                                 pDecodeState, nStartFrame, nFrameCount, &n2, szFileName);
                    g_l2Checked++;
                    bool same = r2 && n2 == outBytes && !memcmp(r2, out, outBytes) && !memcmp(dirsHit, dirs, dirBytes);
                    if (!same) {
                        g_l2Differ++;
                        // Which is wrong? Decode a third time with the game's own whole-table
                        // clear (dcc_clear.cpp) and see which it matches; and where they differ.
                        size_t n3 = 0;
                        uint32_t dirsFresh[64];
                        memcpy(dirsFresh, dirs, dirBytes);
                        hs_dcc_whole_clear = true;
                        void* r3 = hs_orig_CODEC_DecompressAllFrames(pdwCompressedData, pMemory, nReadSize, pCelHeader,
                                                                     pDecodeState, nStartFrame, nFrameCount, &n3, szFileName);
                        hs_dcc_whole_clear = false;
                        uint32_t nDiff = 0, first = 0xffffffff;
                        uint32_t n = r2 && n2 < outBytes ? (uint32_t)n2 : outBytes;
                        for (uint32_t i = 0; r2 && i < n; i++)
                            if (((uint8_t*)r2)[i] != out[i]) { if (!nDiff) first = i; nDiff++; }
                        bool wholeIsFresh = r3 && r2 && n3 == n2 && !memcmp(r3, r2, n2);
                        bool wholeIsCached = r3 && n3 == outBytes && !memcmp(r3, out, outBytes);
                        hs_log("dcc cache differs: %s frames %d+%d, sizes %u/%u, %u bytes differ from %u, dirs %s; the "
                               "whole-clear decode matches %s",
                               szFileName ? szFileName : "?", (int)nStartFrame, (int)nFrameCount, outBytes, (unsigned)n2,
                               nDiff, first, memcmp(dirsHit, dirsFresh, dirBytes) ? "differ" : "same",
                               wholeIsFresh && wholeIsCached ? "both" : wholeIsFresh ? "the fresh decode"
                               : wholeIsCached ? "the cached copy" : "neither");
                        if (r3) hs_FreeServerMemory(pMemory, r3, (char*)"dcc_timing.cpp", __LINE__, 0);
                        memcpy(dirs, dirsFresh, dirBytes);
                    }
                    if (g_l2Checked == checkLimit || (!same && g_l2Differ <= 4))
                        hs_log("dcc cache check: %u of %u hits identical to a fresh decode%s%s", g_l2Checked - g_l2Differ,
                               g_l2Checked, same ? "" : "; differs: ", same ? "" : (szFileName ? szFileName : "?"));
                    if (!same && r2) {  // trust the decoder: keep its output
                        hs_FreeServerMemory(pMemory, out, (char*)"dcc_timing.cpp", __LINE__, 0);
                        *pnSize = n2;
                        return r2;
                    }
                    if (r2) hs_FreeServerMemory(pMemory, r2, (char*)"dcc_timing.cpp", __LINE__, 0);
                    memcpy(dirs, dirsHit, dirBytes);
                    *pnSize = outBytes;
                }
                return out;
            }
            if (out) hs_FreeServerMemory(pMemory, out, (char*)"dcc_timing.cpp", __LINE__, 0);
        }
    }
    uint32_t t0 = now();
    void* r = hs_orig_CODEC_DecompressAllFrames(pdwCompressedData, pMemory, nReadSize, pCelHeader, pDecodeState,
                                                nStartFrame, nFrameCount, pnSize, szFileName);
    if (keyed && r) {
        g_l2Misses++;
        uint32_t ts = now();
        hs_l2_store(key, r, (uint32_t)*pnSize, dirs, (uint32_t)nFrameCount * 4);
        g_l2StoreTicks += (uint32_t)(now() - ts);
    }
    g_ticks += (uint32_t)(now() - t0);
    g_inBytes += nReadSize > 0 ? (uint32_t)nReadSize : 0;
    g_outBytes += pnSize ? *pnSize : 0;
    count_name(szFileName);
    // Check the shortened clear (dcc_clear.cpp): decode again with the game's
    // whole-table clear and compare. Costs a second decode, 200 times.
    static unsigned checked, differ;
    static uint64_t ticksShort, ticksWhole;
    if (r && pnSize && checked < 200) {
        size_t n2 = 0;
        hs_dcc_whole_clear = true;
        uint32_t t1 = now();
        void* r2 = hs_orig_CODEC_DecompressAllFrames(pdwCompressedData, pMemory, nReadSize, pCelHeader, pDecodeState,
                                                     nStartFrame, nFrameCount, &n2, szFileName);
        ticksWhole += (uint32_t)(now() - t1);
        ticksShort += (uint32_t)(t1 - t0);  // includes the timing wrapper's bookkeeping, a few hundred cycles
        hs_dcc_whole_clear = false;
        if (!r2 || n2 != *pnSize || memcmp(r, r2, n2)) differ++;
        if (r2) hs_FreeServerMemory(pMemory, r2, (char*)"dcc_timing.cpp", __LINE__, 0);
        if (++checked == 200 || differ == 1)
            hs_log("dcc clear check: %u of %u decodes identical with the whole-table clear; %u us against %u us a decode",
                   checked - differ, checked, (unsigned)(ticksShort * 1000 / kTicksPerMs / checked),
                   (unsigned)(ticksWhole * 1000 / kTicksPerMs / checked));
    }
    if (++g_decodes % 256 == 0) {
        uint64_t us = g_ticks * 1000 / kTicksPerMs;
        hs_log("dcc: %u decodes, %u ms, %u KiB in, %u KiB out: %u us a decode, %u us a KiB out", g_decodes,
               (unsigned)(us / 1000), (unsigned)(g_inBytes >> 10), (unsigned)(g_outBytes >> 10),
               (unsigned)(us / g_decodes), (unsigned)(g_outBytes >> 10 ? us / (g_outBytes >> 10) : 0));
        log_names();
        if (g_l2Hits + g_l2Misses)
            hs_log("dcc cache: %u hits, %u misses since start; a hit %u us (%u KiB), a store %u us", g_l2Hits, g_l2Misses,
                   (unsigned)(g_l2Hits ? g_l2HitTicks * 1000 / kTicksPerMs / g_l2Hits : 0),
                   (unsigned)(g_l2Hits ? (g_l2HitBytes >> 10) / g_l2Hits : 0),
                   (unsigned)(g_l2Misses ? g_l2StoreTicks * 1000 / kTicksPerMs / g_l2Misses : 0));
    }
    return r;
}
}  // namespace D2CMP::SRC::Codec
