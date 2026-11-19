// Phase 1 audio: the platform's voices (platform.h) mixed in software into one
// SDL stereo stream. Each voice keeps its appended frames in a ring indexed by
// absolute frame number, so a late append lands where the game expects it and
// a gap plays as silence. Linear interpolation for rate conversion.
//
// Headless runs can check the output: SDL_AUDIODRIVER=disk writes the mixed
// stream to SDL_DISKAUDIOFILE (raw S16 stereo at 48 kHz).
#include "Redivivus/port/platform.h"

#include <SDL.h>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int kVoices = 64;
constexpr int kRate = 48000;

struct Voice {
    bool used, playing;
    int channels;
    uint32_t capacity;
    int16_t* ring;       // capacity frames
    uint64_t appended;   // frames appended (absolute)
    uint64_t played;     // frames played (absolute)
    uint32_t frac;       // 16.16 position between played and played+1
    int volL, volR;      // 0..32768
    uint32_t rate;
};
Voice g_voices[kVoices];
SDL_AudioDeviceID g_dev;
int g_state = -1;  // -1 untried, 0 none, 1 open

int16_t sample(const Voice& v, uint64_t frame, int ch) {
    if (frame >= v.appended || v.appended - frame > v.capacity) return 0;
    return v.ring[(frame % v.capacity) * v.channels + (ch < v.channels ? ch : 0)];
}

void SDLCALL mix(void*, Uint8* stream, int len) {
    int frames = len / 4;
    static int32_t acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, (size_t)frames * 2 * sizeof(int32_t));
    for (Voice& v : g_voices) {
        if (!v.used || !v.playing || !v.rate) continue;
        uint32_t step = (uint32_t)(((uint64_t)v.rate << 16) / kRate);
        for (int i = 0; i < frames; i++) {
            int32_t f = (int32_t)v.frac;
            for (int ch = 0; ch < 2; ch++) {
                int32_t a = sample(v, v.played, ch), b = sample(v, v.played + 1, ch);
                int32_t s = a + (int32_t)(((int64_t)(b - a) * f) >> 16);
                acc[i * 2 + ch] += (int32_t)(((int64_t)s * (ch ? v.volR : v.volL)) >> 15);
            }
            v.frac += step;
            v.played += v.frac >> 16;
            v.frac &= 0xffff;
        }
    }
    int16_t* out = (int16_t*)stream;
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    if (frames * 4 < len) memset(stream + frames * 4, 0, (size_t)(len - frames * 4));
}

struct Guard {
    Guard() { if (g_dev) SDL_LockAudioDevice(g_dev); }
    ~Guard() { if (g_dev) SDL_UnlockAudioDevice(g_dev); }
};
Voice* voice(int v) { return v >= 0 && v < kVoices && g_voices[v].used ? &g_voices[v] : nullptr; }
}  // namespace

int hs_audio_open(void) {
    if (g_state >= 0) return g_state;
    g_state = 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        hs_log("audio: SDL audio unavailable: %s", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec want = {}, have = {};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        hs_log("audio: no device: %s", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    hs_log("audio: SDL %s, %d Hz stereo", SDL_GetCurrentAudioDriver(), have.freq);
    g_state = 1;
    return 1;
}

int hs_audio_voice_create(int channels, uint32_t capacity) {
    if (channels < 1 || channels > 2 || !capacity) return -1;
    int16_t* ring = (int16_t*)calloc((size_t)capacity * channels, sizeof(int16_t));
    if (!ring) return -1;
    Guard g;
    for (int i = 0; i < kVoices; i++) {
        if (g_voices[i].used) continue;
        Voice& v = g_voices[i];
        v = Voice{};
        v.used = true;
        v.channels = channels;
        v.capacity = capacity;
        v.ring = ring;
        v.volL = v.volR = 32768;
        v.rate = 22050;
        return i;
    }
    free(ring);
    return -1;
}

void hs_audio_voice_destroy(int id) {
    int16_t* ring = nullptr;
    {
        Guard g;
        Voice* v = voice(id);
        if (!v) return;
        ring = v->ring;
        *v = Voice{};
    }
    free(ring);
}

void hs_audio_voice_reset(int id) {
    Guard g;
    if (Voice* v = voice(id)) {
        v->playing = false;
        v->appended = v->played = 0;
        v->frac = 0;
    }
}

void hs_audio_voice_append(int id, const int16_t* frames, uint32_t count) {
    Guard g;
    Voice* v = voice(id);
    if (!v) return;
    if (v->appended < v->played) {  // these frames' time has passed: drop them
        uint64_t skip = v->played - v->appended;
        if (skip > count) skip = count;
        frames += skip * v->channels;
        count -= (uint32_t)skip;
        v->appended += skip;
    }
    for (uint32_t i = 0; i < count; i++, v->appended++)
        memcpy(&v->ring[(v->appended % v->capacity) * v->channels], &frames[i * v->channels],
               sizeof(int16_t) * v->channels);
}

void hs_audio_voice_play(int id, int playing) {
    Guard g;
    if (Voice* v = voice(id)) v->playing = playing != 0;
}

uint64_t hs_audio_voice_played(int id) {
    Guard g;
    Voice* v = voice(id);
    return v ? v->played : 0;
}

void hs_audio_voice_params(int id, int volLeft, int volRight, uint32_t rate) {
    Guard g;
    if (Voice* v = voice(id)) {
        v->volL = volLeft;
        v->volR = volRight;
        v->rate = rate;
    }
}

// No memory outside the heap worth a second sprite cache here.
int hs_l2_store(const uint32_t[8], const void*, uint32_t, const void*, uint32_t) { return 0; }
int hs_l2_fetch(const uint32_t[8], void* (*)(void*, uint32_t), void*) { return 0; }
