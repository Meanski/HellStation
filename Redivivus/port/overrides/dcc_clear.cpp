// The DCC decoder's second pass clears only the blocks it uses.
//
// CODEC_DecompressAllFrames decodes each direction in two passes over one
// block table, gaCodecFrameDecodeBuffer (5,625 entries of 20 bytes). Pass 1
// clears the blocks the direction covers (nBlockCount * 20 bytes, Codec.cpp
// line 1113); pass 2 clears the whole table (0x1b774 bytes, line 1323), on the
// same frame bounds and the same block iterator, so it never reads past
// nBlockCount either. In a busy fight that second clear was 4 MB of memset a
// frame: the largest memset caller by far (PORTING_LOG, 2026-11-19).
//
// The port wrap on `memset` in Codec.cpp's object (cmake/port.cmake) sends that
// file's memset calls here: a clear of the whole table becomes a clear of what
// the last partial clear of it covered, i.e. this direction's pass 1. The
// decoder runs under D2CMP's codec critical section, so the two clears are
// always of the same direction. Everything else passes straight through.
// overrides/dcc_timing.cpp decodes the first 200 directions a second time with
// the whole clear (hs_dcc_whole_clear) and logs whether the outputs match.
#include <cstddef>
#include <cstdint>
#include <cstring>

extern uint8_t hs_codec_decode_buffer[] __asm__("gaCodecFrameDecodeBuffer");

namespace {
constexpr size_t kWholeTable = 0x1b774;
size_t g_used = kWholeTable;  // bytes of the table the current direction uses
}  // namespace

bool hs_dcc_whole_clear;  // the game's own clear, for the check

extern "C" void* hs_orig_memset(void* d, int c, size_t n) {
    if (d == hs_codec_decode_buffer && c == 0) {
        if (n == kWholeTable) {
            if (!hs_dcc_whole_clear) n = g_used;
        }
        else g_used = n;
    }
    return memset(d, c, n);
}
