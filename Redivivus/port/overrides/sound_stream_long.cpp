// Long sounds stream from the archive instead of loading whole, on a small-RAM target.
//
// The client keeps every sound it plays as a whole WAV in main RAM, under a
// budget (SoundHdr's cache, capped in sprite_cache_cap.cpp) that a file bigger
// than the whole budget gets past: SOUNDHDR_EvictLeastRecentlyUsed computes
// its target unsigned and reports success at once. Town ambience alone is a
// 656 KiB file. Sounds.txt already has a Stream column, which plays a sound
// through Storm's streaming (SFileDdaBeginEx into the channel's own buffer, a
// chunk at a time, looping if the row loops), as the game does for music.
//
// So after the client loads sounds.txt (SOUNDHDR_Init, wrapped), every sound
// whose file is over kStreamFrom bytes is switched to Stream, with its Stereo
// column set from the file's header: a streamed sound plays into a channel
// buffer of the row's channel count, where a loaded one's count comes from the
// file. Off on targets reporting more than 32 MB.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Redivivus/port/platform.h"

#if defined(HS_FAKE_I386)
#  define HS_FC
#else
#  define HS_FC __attribute__((__fastcall__))
#endif

struct D2MPQFileStrc;
extern "C" {
HS_FC void hs_orig_SOUNDHDR_Init(uint32_t bBackground) __asm__("hs_orig__ZN8D2Client5Sound8SoundHdr13SOUNDHDR_InitEj");
HS_FC uint8_t* hs_SoundRow(int32_t id) __asm__("_ZN8D2Client5Sound8SoundHdr27TXT_sounds_GetLineBySoundIdEi");
HS_FC void hs_SoundPath(int32_t id, char* out) __asm__("_ZN8D2Client5Sound8SoundHdr23SOUNDHDR_BuildSoundPathEiPc");
HS_FC int32_t hs_FileOpen(char* path, D2MPQFileStrc** out) __asm__("_ZN3Fog6Source5SFile22ARCHIVE_OpenD2FileWrapEPcPP13D2MPQFileStrc");
HS_FC void hs_FileClose(D2MPQFileStrc* f) __asm__("_ZN3Fog6Source5SFile23ERRORMANAGER_FCloseFileEP13D2MPQFileStrc");
HS_FC uint32_t hs_FileSize(D2MPQFileStrc* f, uint32_t* high) __asm__("_ZN3Fog6Source5SFile25ERRORMANAGER_FGetFileSizeEP13D2MPQFileStrcPm");
HS_FC int hs_FileRead(D2MPQFileStrc* f, void* buf, uint32_t n, uint32_t* read, void* overlapped, void* completion,
                      int flags) __asm__("_ZN3Fog6Source5SFile22ERRORMANAGER_FReadFileEP13D2MPQFileStrcPvjPjP11_OVERLAPPEDP26D2StormAsyncCompletionStrci");
}

namespace {
constexpr uint32_t kStreamFrom = 128u << 10;  // a mono channel's whole buffer
// D2SoundsTxt (D2Client.h), packed: Filename at 0, then these.
constexpr size_t kLoop = 0x3e, kStream = 0x4f, kStereo = 0x50;

bool small_ram() {
    hs_mem_info mi = {};
    hs_mem_query(&mi);
    return mi.phys_total && mi.phys_total <= (32u << 20);
}

// The channel count from a RIFF/WAVE header: the "fmt " chunk's second field.
int channels_of(const uint8_t* h, uint32_t n) {
    if (n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    for (uint32_t at = 12; at + 8 <= n;) {
        uint32_t size;
        memcpy(&size, h + at + 4, 4);
        if (!memcmp(h + at, "fmt ", 4)) {
            if (at + 8 + 4 > n) return 0;
            uint16_t ch;
            memcpy(&ch, h + at + 8 + 2, 2);
            return ch;
        }
        at += 8 + size + (size & 1);
    }
    return 0;
}
}  // namespace

namespace D2Client::Sound::SoundHdr {
HS_FC void SOUNDHDR_Init(uint32_t bBackground) {
    hs_orig_SOUNDHDR_Init(bBackground);
    if (!small_ram()) return;
    uint32_t t0 = hs_ticks_ms();
    unsigned switched = 0, rows = 0;
    uint64_t bytes = 0;
    for (int32_t id = 1;; id++) {
        uint8_t* row = hs_SoundRow(id);
        if (!row) break;
        rows++;
        if (!row[0] || row[kStream]) continue;
        char path[260];
        hs_SoundPath(id, path);
        D2MPQFileStrc* f = nullptr;
        if (!hs_FileOpen(path, &f) || !f) continue;
        uint32_t size = hs_FileSize(f, nullptr);
        if (size > kStreamFrom) {
            uint8_t head[64];
            uint32_t got = 0;
            hs_FileRead(f, head, sizeof head, &got, nullptr, nullptr, 0);
            int ch = channels_of(head, got);
            if (ch == 1 || ch == 2) {
                row[kStream] = 1;
                row[kStereo] = ch == 2;
                switched++;
                bytes += size;
            }
        }
        hs_FileClose(f);
    }
    hs_log("sound: %u of %u sounds over %u KiB stream instead of loading (%u KiB of files), %u ms", switched, rows,
           (unsigned)(kStreamFrom >> 10), (unsigned)(bytes >> 10), (unsigned)(hs_ticks_ms() - t0));
    (void)kLoop;
}
}  // namespace D2Client::Sound::SoundHdr
