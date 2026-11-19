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
// file. Off on targets reporting more than 32 MB. Checking every sound means
// opening 4,700 files, which from a disc took 29 s of seeks: the result is kept
// in the writable root (hs_stream.txt, on the PS2 the memory card) and reused
// while sounds.txt has the same number of rows.
//
// The main menu's own sounds (D2Sound's SOUND_LoadWavFile, wrapped) load whole
// at the menu's start and stay: with the class-select voice lines that's
// 3.9 MB, and creating a character ran out of memory with them (PORTING_LOG,
// 2026-11-20). On a small-RAM target the voice lines (sfx\cursor\intro\) are
// loaded when one is played instead (SOUND_PlaySoundEffect, wrapped), and the
// other voice lines are let go once no effect buffer is playing them: one
// class's line at a time, at most 616 KiB.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Redivivus/port/platform.h"

#if defined(HS_FAKE_I386)
#  define HS_FC
#else
#  define HS_FC __attribute__((__fastcall__))
#endif

struct D2MPQFileStrc;
extern "C" {
HS_FC void hs_orig_SOUNDHDR_Init(uint32_t bBackground) __asm__("hs_orig__ZN8D2Client5Sound8SoundHdr13SOUNDHDR_InitEj");
HS_FC void hs_orig_SOUND_LoadWavFile(char* path) __asm__("hs_orig__ZN7D2Sound3Src11D2SoundFast17SOUND_LoadWavFileEPc");
HS_FC void hs_orig_SOUND_PlaySoundEffect(uint8_t* path)
    __asm__("hs_orig__ZN7D2Sound3Src11D2SoundFast21SOUND_PlaySoundEffectEPh");
HS_FC int hs_FreeClient(void* p, const char* file, int32_t line, int32_t z)
    __asm__("_ZN3Fog6Memory16FreeClientMemoryEPvPcii");
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

// The cache: "hs_stream 1 <rows>", then "<id> <stereo>" per streamed sound.
std::string cache_path() { return std::string(hs_root_dir()) + "/hs_stream.txt"; }
bool load_cache(unsigned rows, unsigned* switched) {
    int fd = hs_file_open(cache_path().c_str(), HS_OPEN_READ, HS_OPEN_EXISTING);
    if (fd < 0) { hs_log("sound: no stream cache at %s (%d)", cache_path().c_str(), hs_errno()); return false; }
    std::string text;
    char buf[1024];
    for (int64_t n; (n = hs_file_read(fd, buf, sizeof buf)) > 0;) text.append(buf, (size_t)n);
    hs_file_close(fd);
    unsigned version = 0, cachedRows = 0;
    int used = 0;
    if (sscanf(text.c_str(), "hs_stream %u %u%n", &version, &cachedRows, &used) != 2 || version != 1 ||
        cachedRows != rows) {
        hs_log("sound: stream cache out of date (%u bytes, version %u, %u rows)", (unsigned)text.size(), version, cachedRows);
        return false;
    }
    const char* p = text.c_str() + used;
    int id, stereo, n;
    *switched = 0;
    while (sscanf(p, "%d %d%n", &id, &stereo, &n) == 2) {
        p += n;
        if (uint8_t* row = hs_SoundRow(id)) {
            row[kStream] = 1;
            row[kStereo] = stereo != 0;
            ++*switched;
        }
    }
    return true;
}
void save_cache(unsigned rows, const std::string& lines) {
    int fd = hs_file_open(cache_path().c_str(), HS_OPEN_WRITE, HS_CREATE_ALWAYS);
    if (fd < 0) { hs_log("sound: can't write %s (%d)", cache_path().c_str(), hs_errno()); return; }
    char head[64];
    int n = snprintf(head, sizeof head, "hs_stream 1 %u\n", rows);
    hs_file_write(fd, head, (uint32_t)n);
    hs_file_write(fd, lines.data(), (uint32_t)lines.size());
    hs_file_close(fd);
}
}  // namespace

namespace D2Client::Sound::SoundHdr {
HS_FC void SOUNDHDR_Init(uint32_t bBackground) {
    hs_orig_SOUNDHDR_Init(bBackground);
    if (!small_ram()) return;
    uint32_t t0 = hs_ticks_ms();
    unsigned switched = 0, rows = 0;
    while (hs_SoundRow((int32_t)rows + 1)) rows++;
    if (load_cache(rows, &switched)) {
        hs_log("sound: %u of %u sounds stream (from %s), %u ms", switched, rows, cache_path().c_str(),
               (unsigned)(hs_ticks_ms() - t0));
        return;
    }
    std::string lines;
    uint64_t bytes = 0;
    for (int32_t id = 1;; id++) {
        uint8_t* row = hs_SoundRow(id);
        if (!row) break;
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
                char line[32];
                snprintf(line, sizeof line, "%d %d\n", (int)id, ch == 2);
                lines += line;
            }
        }
        hs_FileClose(f);
    }
    hs_log("sound: %u of %u sounds over %u KiB stream instead of loading (%u KiB of files), %u ms", switched, rows,
           (unsigned)(kStreamFrom >> 10), (unsigned)(bytes >> 10), (unsigned)(hs_ticks_ms() - t0));
    save_cache(rows, lines);
    (void)kLoop;
}
}  // namespace D2Client::Sound::SoundHdr

// D2SoundFast's menu sound cache and effect buffers (statics, globalized).
struct WavEntry {  // D2SoundWavCacheEntryStrc
    char szFileName[80];
    int* pWavData;
    uint32_t nDataSize;
    uint32_t bStereo;
    WavEntry* pNext;
};
extern "C" WavEntry* hs_wav_cache __asm__("hs_D2Sound_gpWavFileCacheListHead");
extern "C" uint8_t* hs_effect_buffers[8] __asm__("hs_D2Sound_gpaSoundEffectBuffers");  // D2SoundSmpStrc*

namespace {
bool is_voice_line(const char* path) { return path && strstr(path, "cursor\\intro\\"); }
// An effect buffer playing from inside e's data (D2SoundSmpStrc: pPcmData at
// 0x14, bIsPlaying at 0x1C).
bool playing(const WavEntry* e) {
    const uint8_t* lo = (const uint8_t*)e->pWavData;
    for (uint8_t* smp : hs_effect_buffers) {
        if (!smp) continue;
        const uint8_t* pcm;
        int32_t on;
        memcpy(&pcm, smp + 0x14, sizeof pcm);
        memcpy(&on, smp + 0x1C, sizeof on);
        if (on && pcm >= lo && pcm < lo + e->nDataSize) return true;
    }
    return false;
}
}  // namespace

namespace D2Sound::Src::D2SoundFast {
HS_FC void SOUND_LoadWavFile(char* path) {
    if (is_voice_line(path) && small_ram()) return;  // loaded when played (below)
    hs_orig_SOUND_LoadWavFile(path);
}
HS_FC void SOUND_PlaySoundEffect(uint8_t* path) {
    const char* name = (const char*)path;
    if (is_voice_line(name) && small_ram()) {
        bool cached = false;
        for (WavEntry** link = &hs_wav_cache; *link;) {
            WavEntry* e = *link;
            if (!strcmp(e->szFileName, name)) { cached = true; link = &e->pNext; continue; }
            if (is_voice_line(e->szFileName) && !playing(e)) {
                *link = e->pNext;
                if (e->pWavData) hs_FreeClient(e->pWavData, "sound_stream_long.cpp", __LINE__, 0);
                hs_FreeClient(e, "sound_stream_long.cpp", __LINE__, 0);
                continue;
            }
            link = &e->pNext;
        }
        if (!cached) hs_orig_SOUND_LoadWavFile((char*)name);
    }
    hs_orig_SOUND_PlaySoundEffect(path);
}
}  // namespace D2Sound::Src::D2SoundFast
