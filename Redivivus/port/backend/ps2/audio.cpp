// PS2 audio: the platform's voices (platform.h) on SPU2 hardware voices.
//
// Each voice channel is a ring of ADPCM blocks in SPU2 memory that a hardware
// voice loops over (the first block marks the loop start, the last jumps back
// to it). Appended PCM is encoded here, on the EE, 112 frames (four 16-byte
// blocks: one 64-byte DMA unit) at a time, and written at the ring position of
// its frame number, so the SPU2 plays frame N when it reaches block N/28 of the
// ring. A frame count that hasn't filled four blocks waits in `carry`. The
// played count comes from where each hardware voice is reading (NAX), polled
// through the IOP module (iop/hsaudio.c) at most every few milliseconds. Main
// RAM holds no audio: 16 bytes of SPU2 memory hold 28 frames.
//
// SPU2 memory 0x10000-0x1F0000 is ours (1.9 MB; the cores' input, output and
// effect areas sit outside it). Rings freed by a voice are kept, already
// formatted, for the next voice of the same size: the game recreates a buffer
// for nearly every sound it plays.
#include "Redivivus/port/platform.h"

#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <cstring>

#include "Redivivus/port/backend/ps2/iop/hsaudio_rpc.h"

extern unsigned char hsaudio_irx[];
extern unsigned int size_hsaudio_irx;

namespace {
constexpr int kVoices = 32;       // platform voices
constexpr int kHwVoices = 48;     // SPU2 voices, 24 per core
constexpr uint32_t kGroupFrames = 112;  // four ADPCM blocks: one 64-byte write
constexpr uint32_t kSpuBase = 0x10000, kSpuEnd = 0x1F0000;
constexpr uint32_t kPollTicksMs = 4;

// ---- RPC ---------------------------------------------------------------------
SifRpcClientData_t g_client;
uint32_t g_rpc[HSA_RPC_BUFFER / 4] __attribute__((aligned(64)));
hs_mutex* g_lock;

void call(int fn, uint32_t sendBytes, uint32_t recvBytes) {
    SifCallRpc(&g_client, fn, 0, g_rpc, (int)sendBytes, g_rpc, (int)recvBytes, nullptr, nullptr);
}

// ---- ADPCM ---------------------------------------------------------------------
const int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

struct Encoder {
    int s1, s2;  // the decoder's last two samples
};

// One 28-sample block. Picks the filter with the smallest worst residual and
// the finest shift that holds it, then quantises against what the decoder
// will reconstruct, so errors don't accumulate. The step is a power of two, so
// quantising is a shift (the EE divides slowly). Silent blocks, which the
// game writes plenty of (it pads every short sound to its whole buffer), skip
// the search: in town the encoder was 37% of the EE before both.
void encode_block(Encoder& e, const int16_t* x, int stride, uint8_t flags, uint8_t* out) {
    int any = 0;
    for (int i = 0; i < 28; i++) any |= x[i * stride];
    if (!any && !e.s1 && !e.s2) {
        memset(out, 0, 16);
        out[0] = 12;  // filter 0, shift 12: every sample decodes to 0
        out[1] = flags;
        return;
    }
    // The predictor whose error energy is least, from three correlations of the
    // block (with its history): for x[n] ~ a x[n-1] + b x[n-2], the energy is
    // R0 (1 + a^2 + b^2) - 2a R1 - 2b R2 + 2ab R1 (a, b = the filter / 64).
    // One pass of three multiplies a sample, where trying all five filters
    // cost ten (6,400 cycles a block on the EE). Samples scaled by 1/16 keep
    // the sums in 32 bits.
    int r0 = 0, r1 = 0, r2 = 0;
    {
        int p1 = e.s1 >> 4, p2 = e.s2 >> 4;
        for (int i = 0; i < 28; i++) {
            int v = x[i * stride] >> 4;
            r0 += v * v;
            r1 += v * p1;
            r2 += v * p2;
            p2 = p1;
            p1 = v;
        }
    }
    int best = 0;
    float bestE = (float)r0;
    for (int f = 1; f < 5; f++) {
        float a = kFilter[f][0] / 64.0f, b = kFilter[f][1] / 64.0f;
        float en = (float)r0 * (1.0f + a * a + b * b) - 2.0f * a * (float)r1 - 2.0f * b * (float)r2 + 2.0f * a * b * (float)r1;
        if (en < bestE) { bestE = en; best = f; }
    }
    int worstBest = 0;
    {
        int s1 = e.s1, s2 = e.s2;
        for (int i = 0; i < 28; i++) {
            int v = x[i * stride];
            int r = v - ((s1 * kFilter[best][0] + s2 * kFilter[best][1] + 32) >> 6);
            if (r < 0) r = -r;
            if (r > worstBest) worstBest = r;
            s2 = s1;
            s1 = v;
        }
    }
    int shift = 12;
    while (shift > 0 && worstBest > 7 << (12 - shift)) shift--;
    const int k = 12 - shift, half = (1 << k) >> 1;
    const int c0 = kFilter[best][0], c1 = kFilter[best][1];
    out[0] = (uint8_t)((best << 4) | shift);
    out[1] = flags;
    int s1 = e.s1, s2 = e.s2;
    for (int i = 0; i < 28; i++) {
        int pred = (s1 * c0 + s2 * c1 + 32) >> 6;
        int q = (x[i * stride] - pred + half) >> k;  // round to nearest (arithmetic shift)
        if (q > 7) q = 7;
        if (q < -8) q = -8;
        int d = (q << k) + pred;
        if (d > 32767) d = 32767;
        if (d < -32768) d = -32768;
        s2 = s1;
        s1 = d;
        uint8_t& b = out[2 + i / 2];
        if (i & 1) b = (uint8_t)(b | ((q & 15) << 4));
        else b = (uint8_t)(q & 15);
    }
    e.s1 = s1;
    e.s2 = s2;
}

// ---- SPU2 memory: first fit over a sorted extent list, plus formatted rings ----
struct Extent { uint32_t at, size; };
Extent g_free[128];
int g_freeCount;
struct Ring { uint32_t at, blocks; };
Ring g_spare[32];  // formatted rings no voice is using
int g_spareCount;

uint32_t spu_alloc(uint32_t size) {
    for (int i = 0; i < g_freeCount; i++) {
        if (g_free[i].size < size) continue;
        uint32_t at = g_free[i].at;
        g_free[i].at += size;
        g_free[i].size -= size;
        if (!g_free[i].size) {
            memmove(&g_free[i], &g_free[i + 1], sizeof(Extent) * (size_t)(g_freeCount - i - 1));
            g_freeCount--;
        }
        return at;
    }
    return 0;
}
void spu_free(uint32_t at, uint32_t size) {
    int i = 0;
    while (i < g_freeCount && g_free[i].at < at) i++;
    if (g_freeCount == (int)(sizeof g_free / sizeof g_free[0])) return;  // leak rather than corrupt
    memmove(&g_free[i + 1], &g_free[i], sizeof(Extent) * (size_t)(g_freeCount - i));
    g_free[i] = {at, size};
    g_freeCount++;
    if (i + 1 < g_freeCount && g_free[i].at + g_free[i].size == g_free[i + 1].at) {
        g_free[i].size += g_free[i + 1].size;
        memmove(&g_free[i + 1], &g_free[i + 2], sizeof(Extent) * (size_t)(g_freeCount - i - 2));
        g_freeCount--;
    }
    if (i > 0 && g_free[i - 1].at + g_free[i - 1].size == g_free[i].at) {
        g_free[i - 1].size += g_free[i].size;
        memmove(&g_free[i], &g_free[i + 1], sizeof(Extent) * (size_t)(g_freeCount - i - 1));
        g_freeCount--;
    }
}

uint8_t block_flags(uint32_t block, uint32_t blocks) {
    if (block == 0) return 0x06;            // loop start, keep looping
    if (block == blocks - 1) return 0x03;   // loop end: jump back to the start
    return 0x02;
}

// Writes `bytes` (a multiple of 64, at most 16 KiB) staged in the RPC buffer.
void spu_write(uint32_t at, uint32_t bytes) {
    g_rpc[0] = at;
    g_rpc[1] = bytes;
    call(HSA_WRITE, HSA_HEADER + bytes, 16);
}

// A fresh ring: silent blocks carrying the loop flags.
uint32_t ring_format(uint32_t blocks) {
    uint32_t at = spu_alloc(blocks * 16);
    if (!at) return 0;
    for (uint32_t b = 0; b < blocks;) {
        uint32_t n = blocks - b < 1024 ? blocks - b : 1024;
        uint8_t* p = (uint8_t*)g_rpc + HSA_HEADER;
        memset(p, 0, n * 16);
        for (uint32_t i = 0; i < n; i++) p[i * 16 + 1] = block_flags(b + i, blocks);
        spu_write(at + b * 16, n * 16);
        b += n;
    }
    return at;
}
uint32_t ring_take(uint32_t blocks) {
    for (int i = 0; i < g_spareCount; i++) {
        if (g_spare[i].blocks != blocks) continue;
        uint32_t at = g_spare[i].at;
        g_spare[i] = g_spare[--g_spareCount];
        return at;
    }
    uint32_t at = ring_format(blocks);
    while (!at && g_spareCount) {  // make room from spares of other sizes
        Ring r = g_spare[--g_spareCount];
        spu_free(r.at, r.blocks * 16);
        at = ring_format(blocks);
    }
    return at;
}
void ring_give(uint32_t at, uint32_t blocks) {
    if (g_spareCount < (int)(sizeof g_spare / sizeof g_spare[0])) g_spare[g_spareCount++] = {at, blocks};
    else spu_free(at, blocks * 16);
}

// ---- Voices ------------------------------------------------------------------------
struct Voice {
    bool used, playing;
    int channels;
    uint32_t blocks;          // ring length, a multiple of 4
    uint32_t ring[2];         // SPU2 address of each channel's ring
    int hw[2];                // hardware voice of each channel
    uint64_t appended;        // frames written to the ring (whole groups: always a multiple of 112)
    uint32_t skip;            // incoming frames still to drop (their time has passed)
    uint64_t skipTo;          // the frame the stream resumes at after them (a group boundary)
    uint64_t played;
    uint32_t lastBlock;       // where the hardware voice was at the last poll
    int16_t carry[kGroupFrames * 2];
    uint32_t carried;
    Encoder enc[2];
    int volL, volR;
    uint32_t rate;
};
Voice g_voices[kVoices];
bool g_hwUsed[kHwVoices];
uint32_t g_nax[kHwVoices];
uint32_t g_lastPoll;
bool g_polled;
int g_state = -1;

Voice* voice(int v) { return v >= 0 && v < kVoices && g_voices[v].used ? &g_voices[v] : nullptr; }

int hw_alloc_pair(int n, int out[2]) {
    for (int core = 0; core < 2; core++) {
        int got = 0;
        for (int v = core * 24; v < core * 24 + 24 && got < n; v++)
            if (!g_hwUsed[v]) out[got++] = v;
        if (got == n) {
            for (int i = 0; i < n; i++) g_hwUsed[out[i]] = true;
            return 1;
        }
    }
    return 0;
}

void push_params(Voice& v) {
    uint32_t pitch = (uint32_t)(((uint64_t)v.rate << 12) / 48000);
    if (pitch > 0x3fff) pitch = 0x3fff;
    g_rpc[0] = (uint32_t)v.channels;
    for (int c = 0; c < v.channels; c++) {
        int l = v.volL >> 1, r = v.volR >> 1;  // 0..32768 -> 0..0x4000
        if (v.channels == 2) { if (c == 0) r = 0; else l = 0; }  // a stereo pair: one side each
        if (l > 0x3fff) l = 0x3fff;
        if (r > 0x3fff) r = 0x3fff;
        g_rpc[1 + c * 3] = (uint32_t)v.hw[c];
        g_rpc[2 + c * 3] = (uint32_t)l | (uint32_t)r << 16;
        g_rpc[3 + c * 3] = pitch;
    }
    call(HSA_PARAMS, 64, 0);
}

void key(Voice& v, bool on) {
    g_rpc[0] = on;
    g_rpc[1] = (uint32_t)v.channels;
    uint32_t block = (uint32_t)((v.played / 28) % v.blocks);
    for (int c = 0; c < v.channels; c++) {
        g_rpc[2 + c * 3] = (uint32_t)v.hw[c];
        g_rpc[3 + c * 3] = v.ring[c] + block * 16;
        g_rpc[4 + c * 3] = v.ring[c];
    }
    call(HSA_KEY, 64, 0);
    v.lastBlock = block;
}

// Brings every playing voice's played count up to date.
void poll(bool force) {
    uint32_t now = hs_ticks_ms();
    if (!force && g_polled && now - g_lastPoll < kPollTicksMs) return;
    call(HSA_STATUS, 64, 48 * 8);
    memcpy(g_nax, g_rpc, sizeof g_nax);
    g_lastPoll = now;
    g_polled = true;
    for (Voice& v : g_voices) {
        if (!v.used || !v.playing) continue;
        uint32_t nax = g_nax[v.hw[0]];
        if (nax < v.ring[0] || nax >= v.ring[0] + v.blocks * 16) continue;  // not on its ring yet
        uint32_t block = (nax - v.ring[0]) / 16;
        uint32_t delta = (block + v.blocks - v.lastBlock) % v.blocks;
        v.played += (uint64_t)delta * 28;
        v.lastBlock = block;
    }
}

// Encodes and writes whole groups of frames starting at v.appended (a group boundary).
void write_groups(Voice& v, const int16_t* frames, uint32_t groups) {
    uint32_t ringFrames = v.blocks * 28;
    while (groups) {
        uint32_t block = (uint32_t)((v.appended / 28) % v.blocks);
        uint32_t n = groups * 4;                         // blocks to write
        if (n > v.blocks - block) n = v.blocks - block;  // up to the ring's end
        if (n > 1024) n = 1024;                          // one RPC
        for (int c = 0; c < v.channels; c++) {
            uint8_t* out = (uint8_t*)g_rpc + HSA_HEADER;
            for (uint32_t i = 0; i < n; i++)
                encode_block(v.enc[c], frames + (size_t)i * 28 * v.channels + c, v.channels,
                             block_flags(block + i, v.blocks), out + i * 16);
            spu_write(v.ring[c] + block * 16, n * 16);
        }
        frames += (size_t)n * 28 * v.channels;
        v.appended += n * 28;
        groups -= n / 4;
        (void)ringFrames;
    }
}
// The decoder's side of encode_block, for the start-up check.
void decode_block(Encoder& d, const uint8_t* in, int16_t* out) {
    int filter = in[0] >> 4, shift = in[0] & 15;
    if (filter > 4) filter = 0;
    for (int i = 0; i < 28; i++) {
        int nib = (in[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int q = nib >= 8 ? nib - 16 : nib;
        int s = (q << 12 >> shift) + ((d.s1 * kFilter[filter][0] + d.s2 * kFilter[filter][1] + 32) >> 6);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        d.s2 = d.s1;
        d.s1 = s;
        out[i] = (int16_t)s;
    }
}

int16_t test_tone(uint32_t i) {  // 441 Hz at 22050 Hz: 50 frames a period, by table
    static const int16_t quarter[13] = {0, 2054, 4084, 6066, 7975, 9790, 11488, 13049, 14454, 15686, 16729, 17571, 18201};
    uint32_t k = i % 50;
    if (k < 13) return quarter[k];
    if (k < 25) return quarter[25 - k];
    if (k < 38) return (int16_t)-quarter[k - 25];
    return (int16_t)-quarter[50 - k];
}
}  // namespace

// Start-up check: a tone written to SPU2 memory reads back as the tone, and a
// voice playing it (silently) advances at its pitch.
static void self_check() {
    int id = hs_audio_voice_create(1, 22050);
    if (id < 0) { hs_log("audio check: no voice"); return; }
    hs_audio_voice_params(id, 0, 0, 22050);
    static int16_t tone[4480];
    for (uint32_t i = 0; i < 4480; i++) tone[i] = test_tone(i);
    hs_audio_voice_append(id, tone, 4480);
    hs_mutex_lock(g_lock);
    Voice& v = g_voices[id];
    g_rpc[0] = v.ring[0];
    g_rpc[1] = 64 * 20;  // 80 blocks, 2240 frames
    call(HSA_READ, 64, HSA_HEADER + 64 * 20);
    Encoder d = {};
    int16_t dec[28];
    int64_t sig = 0, err = 0;
    int badFlags = 0;
    const uint8_t* in = (const uint8_t*)g_rpc + HSA_HEADER;
    for (uint32_t b = 0; b < 80; b++) {
        decode_block(d, in + b * 16, dec);
        if (in[b * 16 + 1] != block_flags(b, v.blocks)) badFlags++;
        for (int i = 0; i < 28; i++) {
            int32_t want = test_tone(b * 28 + (uint32_t)i), e = dec[i] - want;
            sig += (int64_t)want * want;
            err += (int64_t)e * e;
        }
    }
    hs_mutex_unlock(g_lock);
    int snrTenths = 0;
    for (int64_t r = err ? sig / err : 1000000000; r >= 10 && snrTenths < 990; r /= 10) snrTenths += 100;  // ~dB, in steps of 10
    hs_audio_voice_play(id, 1);
    uint32_t t0 = hs_ticks_ms();
    hs_sleep_ms(100);
    uint64_t played = hs_audio_voice_played(id);
    uint32_t ms = hs_ticks_ms() - t0;
    hs_audio_voice_destroy(id);
    {
        static uint8_t out[16 * 256];
        Encoder e = {};
        uint32_t c0, c1;
        __asm__ volatile("mfc0 %0, $9" : "=r"(c0));
        for (int r = 0; r < 4; r++)
            for (uint32_t b = 0; b < 160; b++) encode_block(e, tone + b * 28, 1, 2, out + (b % 256) * 16);
        __asm__ volatile("mfc0 %0, $9" : "=r"(c1));
        hs_log("audio check: encoder %u cycles a block", (unsigned)((c1 - c0) / 640));
    }
    hs_log("audio check: read back %s (signal/error %lld/%lld, about %d dB), flags %s; played %u frames in %u ms (%u expected)",
           err * 100 < sig ? "ok" : "BAD", (long long)(sig / 2240), (long long)(err / 2240), snrTenths / 10,
           badFlags ? "BAD" : "ok", (unsigned)played, (unsigned)ms, (unsigned)(ms * 2205 / 100));
}

int hs_audio_open(void) {
    if (g_state >= 0) return g_state;
    g_state = 0;
    g_lock = hs_mutex_create();
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    int sd = SifLoadModule("rom0:LIBSD", 0, nullptr);
    int ret = 0;
    int id = SifExecModuleBuffer(hsaudio_irx, size_hsaudio_irx, 0, nullptr, &ret);
    if (sd < 0 || id < 0) {
        hs_log("audio: loading LIBSD/hsaudio failed (%d, %d)", sd, id);
        return 0;
    }
    for (int tries = 0; tries < 200; tries++) {
        if (SifBindRpc(&g_client, HSA_SID, 0) >= 0 && g_client.server) break;
        hs_sleep_ms(5);
    }
    if (!g_client.server) {
        hs_log("audio: hsaudio RPC didn't bind");
        return 0;
    }
    call(HSA_INIT, 64, 16);
    g_free[0] = {kSpuBase, kSpuEnd - kSpuBase};
    g_freeCount = 1;
    hs_log("audio: SPU2 via hsaudio.irx, %u KiB of sound memory", (unsigned)((kSpuEnd - kSpuBase) >> 10));
    g_state = 1;
    self_check();
    return 1;
}

int hs_audio_voice_create(int channels, uint32_t capacity) {
    if (g_state != 1 || channels < 1 || channels > 2 || !capacity) return -1;
    hs_mutex_lock(g_lock);
    int id = -1;
    for (int i = 0; i < kVoices; i++)
        if (!g_voices[i].used) { id = i; break; }
    int hw[2] = {-1, -1};
    if (id >= 0 && hw_alloc_pair(channels, hw)) {
        Voice& v = g_voices[id];
        v = Voice{};
        v.channels = channels;
        v.blocks = ((capacity + 27) / 28 + 3) & ~3u;
        v.hw[0] = hw[0];
        v.hw[1] = hw[1];
        bool ok = true;
        for (int c = 0; c < channels; c++) {
            v.ring[c] = ring_take(v.blocks);
            if (!v.ring[c]) ok = false;
        }
        if (ok) {
            v.used = true;
            v.volL = v.volR = 32768;
            v.rate = 22050;
            push_params(v);
        } else {
            for (int c = 0; c < channels; c++) {
                if (v.ring[c]) ring_give(v.ring[c], v.blocks);
                g_hwUsed[hw[c]] = false;
            }
            hs_log("audio: out of SPU2 memory for a %u-frame voice", (unsigned)capacity);
            id = -1;
        }
    } else if (id >= 0) {
        hs_log("audio: out of hardware voices");
        id = -1;
    }
    hs_mutex_unlock(g_lock);
    return id;
}

void hs_audio_voice_destroy(int id) {
    hs_mutex_lock(g_lock);
    if (Voice* v = voice(id)) {
        if (v->playing) key(*v, false);
        for (int c = 0; c < v->channels; c++) {
            ring_give(v->ring[c], v->blocks);
            g_hwUsed[v->hw[c]] = false;
        }
        *v = Voice{};
    }
    hs_mutex_unlock(g_lock);
}

void hs_audio_voice_reset(int id) {
    hs_mutex_lock(g_lock);
    if (Voice* v = voice(id)) {
        if (v->playing) key(*v, false);
        v->playing = false;
        v->appended = v->played = 0;
        v->carried = 0;
        v->skip = 0;
        v->lastBlock = 0;
        v->enc[0] = v->enc[1] = Encoder{};
    }
    hs_mutex_unlock(g_lock);
}

void hs_audio_voice_append(int id, const int16_t* frames, uint32_t count) {
    hs_mutex_lock(g_lock);
    Voice* v = voice(id);
    if (!v) { hs_mutex_unlock(g_lock); return; }
    if (v->playing) poll(false);
    const int ch = v->channels;
    // Frames whose time has passed: drop up to the next group boundary at or
    // after the play position (the ring there already played something else).
    uint64_t next = v->skip ? v->skipTo - v->skip : v->appended + v->carried;  // the next incoming frame
    if (v->playing && next < v->played) {
        v->skipTo = (v->played + kGroupFrames - 1) / kGroupFrames * kGroupFrames;
        v->skip = (uint32_t)(v->skipTo - next);
        v->carried = 0;  // their group was never written: the ring there played old audio
    }
    if (v->skip) {
        uint32_t n = v->skip < count ? v->skip : count;
        frames += (size_t)n * ch;
        count -= n;
        v->skip -= n;
        if (!v->skip) v->appended = v->skipTo;
    }
    uint64_t room = v->played + (uint64_t)v->blocks * 28 - kGroupFrames;  // never lap the play position
    while (count) {
        if (v->carried || count < kGroupFrames) {
            uint32_t n = kGroupFrames - v->carried;
            if (n > count) n = count;
            memcpy(&v->carry[v->carried * ch], frames, sizeof(int16_t) * n * ch);
            v->carried += n;
            frames += n * ch;
            count -= n;
            if (v->carried == kGroupFrames) {
                if (v->appended + kGroupFrames <= room) write_groups(*v, v->carry, 1);
                else v->appended += kGroupFrames;
                v->carried = 0;
            }
            continue;
        }
        uint32_t groups = count / kGroupFrames;
        uint64_t fits = room > v->appended ? (room - v->appended) / kGroupFrames : 0;
        if (groups > fits) groups = (uint32_t)fits;
        if (!groups) {  // no room: these frames are lost (the caller wrote too far ahead)
            v->appended += (uint64_t)(count / kGroupFrames) * kGroupFrames;
            frames += (size_t)(count / kGroupFrames) * kGroupFrames * ch;
            count %= kGroupFrames;
            continue;
        }
        write_groups(*v, frames, groups);
        frames += (size_t)groups * kGroupFrames * ch;
        count -= groups * kGroupFrames;
    }
    hs_mutex_unlock(g_lock);
}

void hs_audio_voice_play(int id, int playing) {
    hs_mutex_lock(g_lock);
    if (Voice* v = voice(id)) {
        if (playing && !v->playing) {
            v->playing = true;
            key(*v, true);
        } else if (!playing && v->playing) {
            poll(true);
            v->playing = false;
            key(*v, false);
        }
    }
    hs_mutex_unlock(g_lock);
}

uint64_t hs_audio_voice_played(int id) {
    hs_mutex_lock(g_lock);
    uint64_t r = 0;
    if (Voice* v = voice(id)) {
        if (v->playing) poll(false);
        r = v->played;
    }
    hs_mutex_unlock(g_lock);
    return r;
}

void hs_audio_voice_params(int id, int volLeft, int volRight, uint32_t rate) {
    hs_mutex_lock(g_lock);
    if (Voice* v = voice(id)) {
        if (v->volL != volLeft || v->volR != volRight || v->rate != rate) {
            v->volL = volLeft;
            v->volR = volRight;
            v->rate = rate;
            push_params(*v);
        }
    }
    hs_mutex_unlock(g_lock);
}
