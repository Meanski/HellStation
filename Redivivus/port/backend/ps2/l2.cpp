// The EE side of the sprite decode cache in the IOP's memory (iop/l2.c).
#include "Redivivus/port/platform.h"

#include <sifrpc.h>
#include <cstring>

#include "Redivivus/port/backend/ps2/iop/hsaudio_rpc.h"

int hs_ps2_load_iop_module(void);  // audio.cpp

namespace {
SifRpcClientData_t g_client;
uint32_t g_rpc[HSL_RPC_BUFFER / 4] __attribute__((aligned(64)));
hs_mutex* g_lock;
int g_state = -1;
constexpr uint32_t kChunk = HSL_RPC_BUFFER - HSL_HEADER;
constexpr uint32_t kMaxEntry = 256u << 10;
// Each stored entry's size, so a fetch asks for no more than it will get (a
// reply's size is part of what the SIF transfer costs). Direct-mapped by key
// hash; a wrong guess only costs a bigger or a second transfer.
struct SizeHint { uint32_t tag, size; };
SizeHint g_hints[1024];
uint32_t key_hash(const uint32_t key[8]) {
    uint32_t h = 0;
    for (int i = 0; i < 8; i++) h = (h ^ key[i]) * 16777619u;
    return h | 1;
}

bool ready() {
    if (g_state >= 0) return g_state == 1;
    g_state = 0;
    g_lock = hs_mutex_create();
    if (!hs_ps2_load_iop_module()) return false;
    for (int tries = 0; tries < 200; tries++) {
        if (SifBindRpc(&g_client, HSL_SID, 0) >= 0 && g_client.server) break;
        hs_sleep_ms(5);
    }
    g_state = g_client.server ? 1 : 0;
    return g_state == 1;
}

// A store that fits one call is sent without waiting for the IOP; the next
// call waits for it (the buffer is in use until then).
void wait_idle() {
    while (SifCheckStatRpc(&g_client)) hs_yield();
}

// One PUT of bytes at off, gathered from the two parts. async: don't wait for
// the answer (and don't learn it).
bool put(const uint32_t key[8], uint32_t total, uint32_t off, const void* a, uint32_t na, const void* b, uint32_t nb,
         uint32_t bytes, bool async) {
    wait_idle();
    memcpy(g_rpc, key, HSL_KEY_WORDS * 4);
    g_rpc[8] = total;
    g_rpc[9] = off;
    g_rpc[10] = bytes;
    uint8_t* out = (uint8_t*)g_rpc + HSL_HEADER;
    for (uint32_t i = 0; i < bytes;) {  // copy [off, off+bytes) of a ++ b
        uint32_t at = off + i;
        if (at < na) {
            uint32_t n = na - at < bytes - i ? na - at : bytes - i;
            memcpy(out + i, (const uint8_t*)a + at, n);
            i += n;
        } else {
            uint32_t n = bytes - i;
            memcpy(out + i, (const uint8_t*)b + (at - na), n);
            i += n;
        }
    }
    SifCallRpc(&g_client, HSL_PUT, async ? SIF_RPC_M_NOWAIT : 0, g_rpc, HSL_HEADER + ((bytes + 63) & ~63u), g_rpc, 16,
               nullptr, nullptr);
    return async || g_rpc[0] == 1;
}
}  // namespace

int hs_l2_store(const uint32_t key[8], const void* a, uint32_t na, const void* b, uint32_t nb) {
    uint32_t total = na + nb;
    if (!total || total > kMaxEntry || !ready()) return 0;
    uint32_t h = key_hash(key);
    g_hints[h % 1024] = {h, total};
    hs_mutex_lock(g_lock);
    bool ok = true;
    for (uint32_t off = 0; ok && off < total; off += kChunk) {
        uint32_t bytes = total - off < kChunk ? total - off : kChunk;
        ok = put(key, total, off, a, na, b, nb, bytes, total <= kChunk);
    }
    hs_mutex_unlock(g_lock);
    return ok;
}

int hs_l2_fetch(const uint32_t key[8], void* (*alloc)(void* ctx, uint32_t size), void* ctx) {
    if (!ready()) return 0;
    hs_mutex_lock(g_lock);
    uint8_t* dst = nullptr;
    uint32_t total = 0, off = 0;
    uint32_t h = key_hash(key);
    if (g_hints[h % 1024].tag != h) {  // never stored (or its hint was replaced): most likely a miss
        hs_mutex_unlock(g_lock);
        return 0;
    }
    uint32_t hint = g_hints[h % 1024].size;
    for (;;) {
        wait_idle();
        memcpy(g_rpc, key, HSL_KEY_WORDS * 4);
        g_rpc[9] = off;
        uint32_t want = total ? total - off : hint - off;
        if (want > kChunk || want == 0) want = kChunk;
        g_rpc[10] = want;
        SifCallRpc(&g_client, HSL_GET, 0, g_rpc, HSL_HEADER, g_rpc, HSL_HEADER + ((want + 63) & ~63u), nullptr, nullptr);
        uint32_t t = g_rpc[0], n = g_rpc[1];
        if (!t || (total && t != total) || !n) break;  // a miss, or overwritten while we read
        if (!dst) {
            total = t;
            dst = (uint8_t*)alloc(ctx, total);
            if (!dst) break;
        }
        memcpy(dst + off, (uint8_t*)g_rpc + HSL_HEADER, n);
        off += n;
        if (off >= total) break;
    }
    hs_mutex_unlock(g_lock);
    return dst && off >= total ? 1 : (dst ? -1 : 0);  // -1: allocated but incomplete
}
