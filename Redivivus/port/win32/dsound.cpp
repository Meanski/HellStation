// DirectSound, as D2Sound and Storm use it, on the backend's audio voices.
//
// The game sees ordinary secondary buffers: rings of PCM it Locks, fills and
// Unlocks, then plays while it reads the play cursor and refills behind it
// (D2SoundSmp's sound thread, every 50 ms; Storm's music streams the same way).
// Both write the ring strictly in order, so a buffer here keeps no ring at all:
// each Unlock appends what was written to the buffer's backend voice, and the
// play cursor is where the voice's played-frame count lands in the ring. That
// keeps the 128-256 KiB rings the game asks for (25 of them, ~4.3 MB) out of
// main RAM: the PS2 backend holds the queued audio in the SPU2's own memory.
//
// A write that doesn't continue where the last one ended (a new sound loaded
// into a reused buffer) restarts the voice's stream at that offset; so does
// SetCurrentPosition. The primary buffer has no voice: its Lock hands out
// scratch memory and its Play does nothing.
//
// Not provided: 3D buffers and EAX (QueryInterface answers E_NOINTERFACE). The
// game only asks for them with 3D sound switched on in its options, and then
// shuts sound down cleanly when they're missing.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"

namespace {
constexpr HRESULT kOk = 0, kNoInterface = (HRESULT)0x80004002, kInvalidParam = (HRESULT)0x80070057,
                  kNoDriver = (HRESULT)0x88780078, kOutOfMemory = (HRESULT)0x8007000E,
                  kUnsupported = (HRESULT)0x80004001;
constexpr DWORD kStatusPlaying = 1, kStatusLooping = 4;
constexpr DWORD kFlagPrimary = 0x1;

// The SDK's structures, in the layouts the game was compiled against.
struct WaveFormat {  // WAVEFORMATEX
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct BufferDesc {  // DSBUFFERDESC (the 20-byte DirectX 5 form, or larger)
    DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
    WaveFormat* lpwfxFormat;
};
struct BufferCaps {  // DSBCAPS
    DWORD dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead;
};

struct Buffer;
struct BufferVtbl {
    HRESULT(WINAPI* QueryInterface)(Buffer*, const GUID*, void**);
    ULONG(WINAPI* AddRef)(Buffer*);
    ULONG(WINAPI* Release)(Buffer*);
    HRESULT(WINAPI* GetCaps)(Buffer*, BufferCaps*);
    HRESULT(WINAPI* GetCurrentPosition)(Buffer*, DWORD*, DWORD*);
    HRESULT(WINAPI* GetFormat)(Buffer*, WaveFormat*, DWORD, DWORD*);
    HRESULT(WINAPI* GetVolume)(Buffer*, LONG*);
    HRESULT(WINAPI* GetPan)(Buffer*, LONG*);
    HRESULT(WINAPI* GetFrequency)(Buffer*, DWORD*);
    HRESULT(WINAPI* GetStatus)(Buffer*, DWORD*);
    HRESULT(WINAPI* Initialize)(Buffer*, void*, const BufferDesc*);
    HRESULT(WINAPI* Lock)(Buffer*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD);
    HRESULT(WINAPI* Play)(Buffer*, DWORD, DWORD, DWORD);
    HRESULT(WINAPI* SetCurrentPosition)(Buffer*, DWORD);
    HRESULT(WINAPI* SetFormat)(Buffer*, const WaveFormat*);
    HRESULT(WINAPI* SetVolume)(Buffer*, LONG);
    HRESULT(WINAPI* SetPan)(Buffer*, LONG);
    HRESULT(WINAPI* SetFrequency)(Buffer*, DWORD);
    HRESULT(WINAPI* Stop)(Buffer*);
    HRESULT(WINAPI* Unlock)(Buffer*, void*, DWORD, void*, DWORD);
    HRESULT(WINAPI* Restore)(Buffer*);
};

struct Buffer {
    const BufferVtbl* vtbl;  // first: the game calls through it
    LONG refs;
    bool primary;
    DWORD flags, size;        // size: the ring's bytes
    WORD channels, bits;
    DWORD rate, frequency;    // the format's rate; SetFrequency's (0: the format's)
    LONG volume, pan;         // hundredths of a dB
    int voice;                // backend voice, or -1
    bool playing, looping;
    DWORD origin;             // ring offset of the voice's frame 0
    DWORD expect;             // ring offset where the next in-order write starts
    bool fresh;               // nothing appended since the stream restarted
    bool locked;              // holds the shared scratch, between Lock and Unlock
    DWORD lockedAt, lockedBytes;
};

hs_mutex* g_lock;
// One scratch for every Lock, held from Lock to Unlock (the game's writers
// already take turns: D2Sound under its sample lock, Storm under its stream
// lock). Per-buffer scratch had kept up to 64 KiB for each of 25 buffers.
hs_mutex* g_scratchLock;
uint8_t* g_scratch;
DWORD g_scratchSize;
void lock() { hs_mutex_lock(g_lock); }
void unlock() { hs_mutex_unlock(g_lock); }

DWORD frame_bytes(const Buffer* b) { return (DWORD)b->channels * (b->bits / 8); }

int linear(LONG hundredthsDb) {  // DirectSound attenuation -> 0..32768
    if (hundredthsDb >= 0) return 32768;
    if (hundredthsDb <= -10000) return 0;
    return (int)(32768.0 * pow(10.0, hundredthsDb / 2000.0));  // rare: on volume changes
}
void push_params(Buffer* b) {
    if (b->voice < 0) return;
    int v = linear(b->volume);
    int left = v, right = v;
    if (b->pan > 0) left = (int)((int64_t)v * linear(-b->pan) >> 15);
    if (b->pan < 0) right = (int)((int64_t)v * linear(b->pan) >> 15);
    hs_audio_voice_params(b->voice, left, right, b->frequency ? b->frequency : b->rate);
}
void restart(Buffer* b, DWORD at) {
    if (b->voice >= 0) hs_audio_voice_reset(b->voice);
    b->origin = b->expect = at % (b->size ? b->size : 1);
    b->fresh = true;
    if (b->playing && b->voice >= 0) hs_audio_voice_play(b->voice, 1);
}
DWORD play_cursor(Buffer* b) {
    if (b->voice < 0 || !b->size) return 0;
    uint64_t bytes = hs_audio_voice_played(b->voice) * frame_bytes(b);
    return (DWORD)((b->origin + bytes) % b->size);
}

// Converts written ring bytes to 16-bit frames and appends them.
void append(Buffer* b, const void* data, DWORD bytes) {
    if (b->voice < 0 || !bytes) return;
    uint32_t frames = bytes / frame_bytes(b);
    if (b->bits == 16) {
        hs_audio_voice_append(b->voice, (const int16_t*)data, frames);
        return;
    }
    int16_t tmp[1024];
    const uint8_t* s = (const uint8_t*)data;
    uint32_t samples = frames * b->channels, done = 0;
    while (done < samples) {
        uint32_t n = samples - done < 1024 ? samples - done : 1024;
        n -= n % b->channels;
        for (uint32_t i = 0; i < n; i++) tmp[i] = (int16_t)((s[done + i] - 128) << 8);
        hs_audio_voice_append(b->voice, tmp, n / b->channels);
        done += n;
    }
}

HRESULT WINAPI B_QueryInterface(Buffer*, const GUID*, void** out) {
    if (out) *out = nullptr;
    return kNoInterface;
}
ULONG WINAPI B_AddRef(Buffer* b) { return (ULONG)++b->refs; }
ULONG WINAPI B_Release(Buffer* b) {
    LONG n = --b->refs;
    if (n > 0) return (ULONG)n;
    if (b->voice >= 0) hs_audio_voice_destroy(b->voice);
    if (b->locked) hs_mutex_unlock(g_scratchLock);
    free(b);
    return 0;
}
HRESULT WINAPI B_GetCaps(Buffer* b, BufferCaps* caps) {
    if (!caps) return kInvalidParam;
    caps->dwFlags = b->flags;
    caps->dwBufferBytes = b->size;
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;
    return kOk;
}
HRESULT WINAPI B_GetCurrentPosition(Buffer* b, DWORD* play, DWORD* write) {
    lock();
    DWORD p = play_cursor(b);
    unlock();
    if (play) *play = p;
    if (write) *write = b->size ? (p + frame_bytes(b) * 64) % b->size : 0;  // a little ahead, like hardware
    return kOk;
}
HRESULT WINAPI B_GetFormat(Buffer* b, WaveFormat* f, DWORD bytes, DWORD* written) {
    WaveFormat w = {1, b->channels, b->rate, b->rate * frame_bytes(b), (WORD)frame_bytes(b), b->bits, 0};
    if (f) memcpy(f, &w, bytes < sizeof w ? bytes : sizeof w);
    if (written) *written = sizeof w;
    return kOk;
}
HRESULT WINAPI B_GetVolume(Buffer* b, LONG* v) { if (v) *v = b->volume; return kOk; }
HRESULT WINAPI B_GetPan(Buffer* b, LONG* p) { if (p) *p = b->pan; return kOk; }
HRESULT WINAPI B_GetFrequency(Buffer* b, DWORD* f) { if (f) *f = b->frequency ? b->frequency : b->rate; return kOk; }
HRESULT WINAPI B_GetStatus(Buffer* b, DWORD* s) {

    if (s) *s = b->playing ? kStatusPlaying | (b->looping ? kStatusLooping : 0) : 0;
    return kOk;
}
HRESULT WINAPI B_Initialize(Buffer*, void*, const BufferDesc*) { return kOk; }
HRESULT WINAPI B_Lock(Buffer* b, DWORD at, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2, DWORD flags) {
    if (!p1 || !n1) return kInvalidParam;
    if (flags & 2) { at = 0; bytes = b->size; }                      // DSBLOCK_ENTIREBUFFER
    if (flags & 1) { lock(); at = play_cursor(b); unlock(); }        // DSBLOCK_FROMWRITECURSOR
    if (!b->size || at >= b->size) return kInvalidParam;
    // One region: the game's writes never cross the ring's end (they pass no
    // second pointer); a request that would is cut at the end.
    if (bytes > b->size - at) bytes = b->size - at;
    hs_mutex_lock(g_scratchLock);
    if (bytes > g_scratchSize) {  // grows to the largest write: a whole stereo ring, 256 KiB
        void* p = realloc(g_scratch, bytes);
        if (!p) {
            hs_mutex_unlock(g_scratchLock);
            return kOutOfMemory;
        }
        g_scratch = (uint8_t*)p;
        g_scratchSize = bytes;
    }
    b->locked = true;
    b->lockedAt = at;
    b->lockedBytes = bytes;
    *p1 = g_scratch;
    *n1 = bytes;
    if (p2) *p2 = nullptr;
    if (n2) *n2 = 0;
    return kOk;
}
HRESULT WINAPI B_Unlock(Buffer* b, void* p1, DWORD n1, void*, DWORD) {
    if (!b->locked || p1 != g_scratch) return kInvalidParam;
    if (n1 > b->lockedBytes) n1 = b->lockedBytes;
    if (b->voice >= 0) {
        lock();
        if (b->lockedAt != b->expect && !b->fresh) restart(b, b->lockedAt);
        if (b->fresh) b->origin = b->lockedAt;
        append(b, p1, n1);
        b->fresh = false;
        b->expect = (b->lockedAt + n1) % b->size;
        unlock();
    }
    b->locked = false;
    b->lockedBytes = 0;
    hs_mutex_unlock(g_scratchLock);
    return kOk;
}
HRESULT WINAPI B_Play(Buffer* b, DWORD, DWORD, DWORD flags) {
    b->looping = (flags & 1) != 0;  // DSBPLAY_LOOPING
    if (b->primary || b->voice < 0) { b->playing = true; return kOk; }
    lock();
    b->playing = true;
    hs_audio_voice_play(b->voice, 1);
    unlock();
    return kOk;
}
HRESULT WINAPI B_SetCurrentPosition(Buffer* b, DWORD at) {
    if (b->voice < 0) return kOk;
    lock();
    restart(b, at);
    unlock();
    return kOk;
}
HRESULT WINAPI B_SetFormat(Buffer*, const WaveFormat* f) { return f ? kOk : kInvalidParam; }  // the primary's mix format
HRESULT WINAPI B_SetVolume(Buffer* b, LONG v) {
    lock();
    b->volume = v;
    push_params(b);
    unlock();
    return kOk;
}
HRESULT WINAPI B_SetPan(Buffer* b, LONG p) {
    lock();
    b->pan = p;
    push_params(b);
    unlock();
    return kOk;
}
HRESULT WINAPI B_SetFrequency(Buffer* b, DWORD f) {
    lock();
    b->frequency = f;
    push_params(b);
    unlock();
    return kOk;
}
HRESULT WINAPI B_Stop(Buffer* b) {
    lock();
    b->playing = false;
    if (b->voice >= 0) hs_audio_voice_play(b->voice, 0);
    unlock();
    return kOk;
}
HRESULT WINAPI B_Restore(Buffer*) { return kOk; }

const BufferVtbl kBufferVtbl = {
    B_QueryInterface, B_AddRef,  B_Release, B_GetCaps,  B_GetCurrentPosition, B_GetFormat,          B_GetVolume,
    B_GetPan,         B_GetFrequency, B_GetStatus, B_Initialize, B_Lock, B_Play, B_SetCurrentPosition, B_SetFormat,
    B_SetVolume,      B_SetPan,  B_SetFrequency, B_Stop, B_Unlock, B_Restore,
};

// ---- IDirectSound ---------------------------------------------------------------------

struct Device;
struct DeviceVtbl {
    HRESULT(WINAPI* QueryInterface)(Device*, const GUID*, void**);
    ULONG(WINAPI* AddRef)(Device*);
    ULONG(WINAPI* Release)(Device*);
    HRESULT(WINAPI* CreateSoundBuffer)(Device*, const BufferDesc*, Buffer**, void*);
    HRESULT(WINAPI* GetCaps)(Device*, DWORD*);
    HRESULT(WINAPI* DuplicateSoundBuffer)(Device*, Buffer*, Buffer**);
    HRESULT(WINAPI* SetCooperativeLevel)(Device*, HWND, DWORD);
    HRESULT(WINAPI* Compact)(Device*);
    HRESULT(WINAPI* GetSpeakerConfig)(Device*, DWORD*);
    HRESULT(WINAPI* SetSpeakerConfig)(Device*, DWORD);
    HRESULT(WINAPI* Initialize)(Device*, const GUID*);
};
struct Device {
    const DeviceVtbl* vtbl;
    LONG refs;
};

HRESULT WINAPI D_QueryInterface(Device*, const GUID*, void** out) {
    if (out) *out = nullptr;
    return kNoInterface;
}
ULONG WINAPI D_AddRef(Device* d) { return (ULONG)++d->refs; }
ULONG WINAPI D_Release(Device* d) {
    LONG n = --d->refs;
    if (n <= 0) free(d);
    return n > 0 ? (ULONG)n : 0;
}
HRESULT WINAPI D_CreateSoundBuffer(Device*, const BufferDesc* desc, Buffer** out, void*) {
    if (!desc || !out) return kInvalidParam;
    *out = nullptr;
    Buffer* b = (Buffer*)calloc(1, sizeof(Buffer));
    if (!b) return kOutOfMemory;
    b->vtbl = &kBufferVtbl;
    b->refs = 1;
    b->voice = -1;
    b->fresh = true;
    b->flags = desc->dwFlags;
    b->primary = (desc->dwFlags & kFlagPrimary) != 0;
    b->channels = 2;
    b->bits = 16;
    b->rate = 22050;
    if (b->primary) {
        b->size = 4096;  // what the game clears through Lock; nothing plays from it
    } else {
        const WaveFormat* f = desc->lpwfxFormat;
        if (!f || f->wFormatTag != 1 || (f->nChannels != 1 && f->nChannels != 2) ||
            (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) || !desc->dwBufferBytes) {
            free(b);
            return kInvalidParam;
        }
        b->channels = f->nChannels;
        b->bits = f->wBitsPerSample;
        b->rate = f->nSamplesPerSec;
        b->size = desc->dwBufferBytes - desc->dwBufferBytes % frame_bytes(b);
        // Room for a whole ring ahead of the play cursor, and a little more.
        b->voice = hs_audio_voice_create(b->channels, b->size / frame_bytes(b) + 1024);
        if (b->voice < 0) {
            free(b);
            return kOutOfMemory;
        }
        push_params(b);
    }
    *out = b;
    return kOk;
}
HRESULT WINAPI D_GetCaps(Device*, DWORD* caps) {  // DSCAPS: dwSize, dwFlags, then counts
    if (!caps || caps[0] < 8) return kInvalidParam;
    DWORD size = caps[0];
    memset(caps, 0, size);
    caps[0] = size;
    // PRIMARYMONO|PRIMARYSTEREO|PRIMARY8BIT|PRIMARY16BIT|SECONDARYMONO|SECONDARYSTEREO|SECONDARY8BIT|SECONDARY16BIT
    caps[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400 | 0x800;
    return kOk;
}
HRESULT WINAPI D_DuplicateSoundBuffer(Device*, Buffer*, Buffer** out) {
    if (out) *out = nullptr;
    return kUnsupported;
}
HRESULT WINAPI D_SetCooperativeLevel(Device*, HWND, DWORD) { return kOk; }
HRESULT WINAPI D_Compact(Device*) { return kOk; }
HRESULT WINAPI D_GetSpeakerConfig(Device*, DWORD* c) { if (c) *c = 4; return kOk; }  // DSSPEAKER_STEREO
HRESULT WINAPI D_SetSpeakerConfig(Device*, DWORD) { return kOk; }
HRESULT WINAPI D_Initialize(Device*, const GUID*) { return kOk; }

const DeviceVtbl kDeviceVtbl = {
    D_QueryInterface, D_AddRef, D_Release, D_CreateSoundBuffer, D_GetCaps, D_DuplicateSoundBuffer,
    D_SetCooperativeLevel, D_Compact, D_GetSpeakerConfig, D_SetSpeakerConfig, D_Initialize,
};
}  // namespace

extern "C" int32_t WINAPI DirectSoundCreate(const GUID*, struct IDirectSound** out, struct IUnknown*) {
    if (!out) return kInvalidParam;
    *out = nullptr;
    if (!hs_audio_open()) {
        HS_STUB("no audio device: the game runs silent");
        return kNoDriver;
    }
    if (!g_lock) g_lock = hs_mutex_create();
    if (!g_scratchLock) {
        g_scratchLock = hs_mutex_create();
        g_scratchSize = 256u << 10;  // the largest write, a whole stereo ring: once, so it never grows in pieces
        g_scratch = (uint8_t*)malloc(g_scratchSize);
        if (!g_scratch) g_scratchSize = 0;
    }
    Device* d = (Device*)calloc(1, sizeof(Device));
    if (!d) return kOutOfMemory;
    d->vtbl = &kDeviceVtbl;
    d->refs = 1;
    *out = (struct IDirectSound*)d;
    hs_log("dsound: device created");
    return kOk;
}
// d2_platform.h's declaration (the GUID pointer as an int, the context as a DWORD).
extern "C" int32_t WINAPI DirectSoundEnumerateA(BOOL(WINAPI* cb)(int, char*, char*, DWORD), void* ctx) {
    if (cb && hs_audio_open()) cb(0, (char*)"Primary Sound Driver", (char*)"", (DWORD)(uintptr_t)ctx);
    return kOk;
}
