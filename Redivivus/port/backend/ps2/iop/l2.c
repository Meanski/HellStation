// A second level for the game's sprite cache, in the IOP's spare memory.
//
// Busy fights are slow because the game decodes the same sprites over and over:
// its 2 MB sprite cache can't hold a scene's player, mercenary and overlay
// directions (PORTING_LOG, 2026-11-19/20). The EE (overrides/dcc_timing.cpp)
// keeps a copy of each decode here, keyed by what was decoded, and on the next
// decode of the same thing copies it back instead of decoding: a transfer over
// SIF instead of milliseconds of bit-reading on the EE.
//
// Storage is one ring: entries are written at the head and older ones are
// overwritten (first in, first out). It takes what the IOP has free at the
// first store, less a reserve, up to 1.25 MB. Its own RPC server and thread.
#include <irx.h>
#include <sifcmd.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>

#include "hsaudio_rpc.h"

#define ENTRIES 1024
#define RESERVE (192 * 1024)
#define MAX_POOL (1280 * 1024)

typedef struct {
    u32 key[HSL_KEY_WORDS];
    u32 at, size;  // in the pool; size 0 = empty
} Entry;

static Entry g_entries[ENTRIES];  // FIFO: the oldest at g_tail
static u32 g_head, g_tail, g_count;
static u8* g_pool;
static u32 g_poolSize, g_ringHead;
static int g_state = -1;
static SifRpcDataQueue_t g_queue;
static SifRpcServerData_t g_server;
static u32 g_buffer[HSL_RPC_BUFFER / 4] __attribute__((aligned(64)));

static int ensure_pool(void) {
    u32 freeBytes;
    if (g_state >= 0) return g_state;
    freeBytes = QueryMaxFreeMemSize();
    g_poolSize = freeBytes > RESERVE ? freeBytes - RESERVE : 0;
    if (g_poolSize > MAX_POOL) g_poolSize = MAX_POOL;
    g_poolSize &= ~63u;
    g_pool = g_poolSize >= 256 * 1024 ? (u8*)AllocSysMemory(0, g_poolSize, NULL) : 0;
    g_state = g_pool != 0;
    return g_state;
}

static int same_key(const u32* a, const u32* b) {
    int i;
    for (i = 0; i < HSL_KEY_WORDS; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static Entry* find(const u32* key) {
    u32 i, n;
    for (n = 0, i = g_tail; n < g_count; n++, i = (i + 1) % ENTRIES)
        if (g_entries[i].size && same_key(g_entries[i].key, key)) return &g_entries[i];
    return 0;
}

// Room for size bytes at the ring's head, dropping every entry it overlaps
// (after a wrap the overlapped ones needn't be the oldest), then the oldest
// while the entry table is full.
static Entry* make_room(const u32* key, u32 size) {
    Entry* e;
    u32 i, n, at = g_ringHead;
    if (at + ((size + 63) & ~63u) > g_poolSize) at = 0;  // wrap: the space at the end goes unused this lap
    for (n = 0, i = g_tail; n < g_count; n++, i = (i + 1) % ENTRIES) {
        Entry* old = &g_entries[i];
        if (old->size && old->at < at + size && at < old->at + old->size) old->size = 0;
    }
    while (g_count && (!g_entries[g_tail].size || g_count == ENTRIES)) {
        g_entries[g_tail].size = 0;
        g_tail = (g_tail + 1) % ENTRIES;
        g_count--;
    }
    e = &g_entries[g_head];
    g_head = (g_head + 1) % ENTRIES;
    g_count++;
    memcpy(e->key, key, sizeof e->key);
    e->at = at;
    e->size = size;
    g_ringHead = at + ((size + 63) & ~63u);  // entries start 64-byte aligned: word copies and whole DMA blocks
    return e;
}

static void* rpc(int fno, void* data, int size) {
    u32* a = (u32*)data;
    u8* payload = (u8*)data + HSL_HEADER;
    (void)size;
    switch (fno) {
    case HSL_PUT: {  // key, a[8] total size, a[9] offset, a[10] bytes; data from HSL_HEADER
        u32 total = a[8], off = a[9], bytes = a[10];
        Entry* e;
        if (!ensure_pool() || total > g_poolSize / 4 || off + bytes > total) { a[0] = 0; break; }
        if (off == 0) {
            Entry* old = find(a);
            if (old) old->size = 0;  // stored again: the new copy replaces it
            e = make_room(a, total);
        } else {
            e = find(a);
        }
        if (!e || e->size != total) { a[0] = 0; break; }
        memcpy(g_pool + e->at + off, payload, bytes);
        a[0] = 1;
        break;
    }
    case HSL_GET: {  // key, a[9] offset, a[10] most bytes: replies a[0] total (0 = miss), a[1] bytes
        u32 off = a[9], most = a[10];
        Entry* e = g_state == 1 ? find(a) : 0;
        if (!e || off >= e->size) { a[0] = a[1] = 0; break; }
        if (most > e->size - off) most = e->size - off;
        if (most > HSL_RPC_BUFFER - HSL_HEADER) most = HSL_RPC_BUFFER - HSL_HEADER;
        memcpy(payload, g_pool + e->at + off, most);
        a[0] = e->size;
        a[1] = most;
        break;
    }
    case HSL_INFO:
        a[0] = g_state == 1 ? g_poolSize : 0;
        a[1] = g_count;
        break;
    default:
        break;
    }
    return data;
}

static void server(void* arg) {
    (void)arg;
    sceSifSetRpcQueue(&g_queue, GetThreadId());
    sceSifRegisterRpc(&g_server, HSL_SID, rpc, g_buffer, NULL, NULL, &g_queue);
    sceSifRpcLoop(&g_queue);
}

int hs_l2_start(void) {
    iop_thread_t t;
    int id;
    memset(&t, 0, sizeof t);
    t.attr = TH_C;
    t.thread = server;
    t.stacksize = 0x1000;
    t.priority = 0x2a;
    id = CreateThread(&t);
    if (id <= 0) return 0;
    StartThread(id, NULL);
    return 1;
}
