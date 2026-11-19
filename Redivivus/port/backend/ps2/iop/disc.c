// The disc read cache, on the IOP: its 2 MB has about 1.5 MB free, the EE's
// 32 MB has none to spare.
//
// The game reads its archives a 4 KiB sector at a time, all over the disc, and
// a drive spends its time seeking: from the disc, loading town was 33,500 reads
// with no cache and 6,900 with 128 KiB on the EE (PORTING_LOG, 2026-11-20); a
// simulation on the logged reads gave 875 with 1 MB. So the EE (backend/ps2/
// disc.cpp) asks this module for file bytes by the file's first sector, and it
// serves them from sixteen 64 KiB windows of the disc, least recently used out,
// read with sceCdRead. Its own RPC server and thread, so a read waiting on the
// drive doesn't hold up audio.
#include <irx.h>
#include <sifcmd.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>
#include <cdvdman.h>

#include "hsaudio_rpc.h"

#define WINDOW_SECTORS 32  // 64 KiB
#define WINDOWS 16         // 1 MiB

typedef struct {
    u32 first;  // first sector, or 0xffffffff for none
    u32 used;
    u8* data;
} Window;

static Window g_windows[WINDOWS];
static u32 g_clock;
static SifRpcDataQueue_t g_queue;
static SifRpcServerData_t g_server;
static u32 g_buffer[HSD_RPC_BUFFER / 4] __attribute__((aligned(64)));

static int read_sectors(u32 lsn, u32 count, void* to) {
    sceCdRMode mode;
    int tries;
    mode.trycount = 16;
    mode.spindlctrl = SCECdSpinNom;
    mode.datapattern = SCECdSecS2048;
    mode.pad = 0;
    for (tries = 0; tries < 100; tries++) {
        if (sceCdRead(lsn, count, to, &mode)) {
            sceCdSync(0);
            return 1;
        }
        DelayThread(1000);  // the drive is busy: try again
    }
    return 0;
}

static Window* window_for(u32 sector) {
    u32 first = sector - sector % WINDOW_SECTORS;
    Window* lru = &g_windows[0];
    int i;
    for (i = 0; i < WINDOWS; i++) {
        if (g_windows[i].first == first) {
            g_windows[i].used = ++g_clock;
            return &g_windows[i];
        }
        if (g_windows[i].used < lru->used) lru = &g_windows[i];
    }
    lru->first = 0xffffffff;
    if (!read_sectors(first, WINDOW_SECTORS, lru->data)) return 0;
    lru->first = first;
    lru->used = ++g_clock;
    return lru;
}

static int g_ready = -1;  // windows allocated: -1 not yet, 0 failed, 1 yes
static int ensure_windows(void) {
    int i;
    u8* mem;
    if (g_ready >= 0) return g_ready;
    mem = (u8*)AllocSysMemory(0, WINDOWS * WINDOW_SECTORS * 2048, NULL);
    g_ready = mem != 0;
    for (i = 0; mem && i < WINDOWS; i++) {
        g_windows[i].first = 0xffffffff;
        g_windows[i].used = 0;
        g_windows[i].data = mem + i * WINDOW_SECTORS * 2048;
    }
    return g_ready;
}

static void* rpc(int fno, void* data, int size) {
    u32* a = (u32*)data;
    (void)size;
    switch (fno) {
    case HSD_FIND: {  // a[1..] the file's name ("\\NAME.EXT;1"): replies a[0] first sector, a[1] size (0: not found)
        sceCdlFILE f;
        if (!ensure_windows()) { a[0] = a[1] = 0; break; }  // no cache: the EE reads the file itself
        ((char*)data)[HSD_HEADER - 1] = 0;
        if (sceCdSearchFile(&f, (const char*)&a[1])) {
            a[0] = f.lsn;
            a[1] = f.size;
        } else {
            a[0] = a[1] = 0;
        }
        break;
    }
    case HSD_READ: {  // a[0] file's first sector, a[1] byte offset, a[2] bytes: data from byte HSD_HEADER
        u32 lsn = a[0], offset = a[1], bytes = a[2], done = 0;
        u8* out = (u8*)data + HSD_HEADER;
        if (bytes > HSD_RPC_BUFFER - HSD_HEADER) bytes = HSD_RPC_BUFFER - HSD_HEADER;
        while (done < bytes) {
            u32 at = offset + done;
            u32 sector = lsn + at / 2048;
            Window* w = window_for(sector);
            u32 inWindow, take;
            if (!w) break;
            inWindow = (sector - w->first) * 2048 + at % 2048;
            take = WINDOW_SECTORS * 2048 - inWindow;
            if (take > bytes - done) take = bytes - done;
            memcpy(out + done, w->data + inWindow, take);
            done += take;
        }
        a[0] = done;
        break;
    }
    default:
        break;
    }
    return data;
}

static void server(void* arg) {
    (void)arg;
    sceSifSetRpcQueue(&g_queue, GetThreadId());
    sceSifRegisterRpc(&g_server, HSD_SID, rpc, g_buffer, NULL, NULL, &g_queue);
    sceSifRpcLoop(&g_queue);
}

// Called from _start: starts the server. The windows are allocated at the first
// request, so a game run from the host never takes the IOP's memory. 0 on failure.
int hs_disc_start(void) {
    iop_thread_t t;
    int id;
    memset(&t, 0, sizeof t);
    t.attr = TH_C;
    t.thread = server;
    t.stacksize = 0x1000;
    t.priority = 0x29;
    id = CreateThread(&t);
    if (id <= 0) return 0;
    StartThread(id, NULL);
    return 1;
}
