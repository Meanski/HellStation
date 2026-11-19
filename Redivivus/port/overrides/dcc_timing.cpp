// Time spent decoding DCC sprites (D2CMP's CODEC_DecompressAllFrames), per
// decode and per KiB produced, logged every 256 decodes. How often the game
// decodes depends on the sprite cache and on timing (the same scripted walk
// gave 45 and 205 ms frames), but the cost of a decode doesn't: this is the
// figure to compare when the decoder is changed. A port wrap
// (cmake/port.cmake, HS_PORT_WRAPS).
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
}  // namespace

namespace D2CMP::SRC::Codec {
HS_CODEC_CC void* CODEC_DecompressAllFrames(uint32_t* pdwCompressedData, D2PoolManagerStrc* pMemory, int32_t nReadSize,
                                            uint8_t* pCelHeader, D2CelDecodeStateStrc* pDecodeState, int32_t nStartFrame,
                                            int32_t nFrameCount, size_t* pnSize, char* szFileName) {
    uint32_t t0 = now();
    void* r = hs_orig_CODEC_DecompressAllFrames(pdwCompressedData, pMemory, nReadSize, pCelHeader, pDecodeState,
                                                nStartFrame, nFrameCount, pnSize, szFileName);
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
    }
    return r;
}
}  // namespace D2CMP::SRC::Codec
