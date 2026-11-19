// The EE side of the disc read cache (iop/disc.c): file bytes by the file's
// first sector, served from 1 MB of IOP memory. backend/posix/files.cpp uses
// it for files on cdrom0: when it's there.
#include "Redivivus/port/platform.h"

#include <sifrpc.h>
#include <cstring>

#include "Redivivus/port/backend/ps2/iop/hsaudio_rpc.h"

int hs_ps2_load_iop_module(void);  // audio.cpp

namespace {
SifRpcClientData_t g_client;
uint32_t g_rpc[HSD_RPC_BUFFER / 4] __attribute__((aligned(64)));
hs_mutex* g_lock;
int g_state = -1;

bool ready() {
    if (g_state >= 0) return g_state == 1;
    g_state = 0;
    g_lock = hs_mutex_create();
    if (!hs_ps2_load_iop_module()) return false;
    for (int tries = 0; tries < 200; tries++) {
        if (SifBindRpc(&g_client, HSD_SID, 0) >= 0 && g_client.server) break;
        hs_sleep_ms(5);
    }
    if (!g_client.server) {
        hs_log("disc: the IOP cache didn't answer; reading the disc directly");
        return false;
    }
    g_state = 1;
    hs_log("disc: 1 MiB read cache on the IOP");
    return true;
}
}  // namespace

// The first sector and size of a file in the disc's root ("\\NAME.EXT;1"), or 0.
extern "C" int hs_disc_cache_find(const char* isoName, uint32_t* lsn, uint32_t* size) {
    if (!ready()) return 0;
    hs_mutex_lock(g_lock);
    memset(g_rpc, 0, HSD_HEADER);
    strncpy((char*)&g_rpc[1], isoName, HSD_HEADER - 5);
    SifCallRpc(&g_client, HSD_FIND, 0, g_rpc, HSD_HEADER, g_rpc, 16, nullptr, nullptr);
    *lsn = g_rpc[0];
    *size = g_rpc[1];
    hs_mutex_unlock(g_lock);
    return *lsn != 0;
}

// Reads n bytes at offset of the file starting at lsn; returns the bytes read.
extern "C" int64_t hs_disc_cache_read(uint32_t lsn, int64_t offset, void* buf, uint32_t n) {
    hs_mutex_lock(g_lock);
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done;
        if (chunk > HSD_RPC_BUFFER - HSD_HEADER) chunk = HSD_RPC_BUFFER - HSD_HEADER;
        g_rpc[0] = lsn;
        g_rpc[1] = (uint32_t)(offset + done);
        g_rpc[2] = chunk;
        SifCallRpc(&g_client, HSD_READ, 0, g_rpc, 16, g_rpc, HSD_HEADER + ((chunk + 63) & ~63u), nullptr, nullptr);
        uint32_t got = g_rpc[0];
        if (got > chunk) got = chunk;
        memcpy((char*)buf + done, (char*)g_rpc + HSD_HEADER, got);
        done += got;
        if (got < chunk) break;
    }
    hs_mutex_unlock(g_lock);
    return done;
}
