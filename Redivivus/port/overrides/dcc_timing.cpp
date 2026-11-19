// Time spent decoding DCC sprites (D2CMP's CODEC_DecompressAllFrames), per
// decode and per KiB produced, logged every 256 decodes. How often the game
// decodes depends on the sprite cache and on timing (the same scripted walk
// gave 45 and 205 ms frames), but the cost of a decode doesn't: this is the
// figure to compare when the decoder is changed. A port wrap
// (cmake/port.cmake, HS_PORT_WRAPS).
#include <cstddef>
#include <cstdint>
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
    if (++g_decodes % 256 == 0) {
        uint64_t us = g_ticks * 1000 / kTicksPerMs;
        hs_log("dcc: %u decodes, %u ms, %u KiB in, %u KiB out: %u us a decode, %u us a KiB out", g_decodes,
               (unsigned)(us / 1000), (unsigned)(g_inBytes >> 10), (unsigned)(g_outBytes >> 10),
               (unsigned)(us / g_decodes), (unsigned)(g_outBytes >> 10 ? us / (g_outBytes >> 10) : 0));
    }
    return r;
}
}  // namespace D2CMP::SRC::Codec
