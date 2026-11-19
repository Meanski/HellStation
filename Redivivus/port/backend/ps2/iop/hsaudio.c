// hsaudio.irx: the SPU2 side of the PS2 audio backend (backend/ps2/audio.cpp).
//
// The EE encodes each voice's PCM to SPU2 ADPCM and owns every decision; this
// module only does what the EE can't reach: SPU2 memory writes, voice
// registers and key on/off, and reading back where each voice is playing.
// One SIF RPC server, one command per call; the reply is the call's buffer.
//
// Mixing: both cores' voices go to their core's dry output; core 0's output
// feeds core 1's input, and core 1 is what the PS2 outputs. No effects.
#include <irx.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <sifman.h>
#include <sysclib.h>
#include <thbase.h>
#include <libsd.h>

#include "hsaudio_rpc.h"

IRX_ID("hsaudio", 1, 1);

static SifRpcDataQueue_t g_queue;
static SifRpcServerData_t g_server;
static u32 g_buffer[HSA_RPC_BUFFER / 4] __attribute__((aligned(64)));

static u16 voice_entry(int voice, u16 what) { return (u16)(SD_VOICE(voice / 24, voice % 24) | what); }

static void init_spu(void) {
    int core;
    sceSdInit(0);
    for (core = 0; core < 2; core++) {
        sceSdSetParam(core | SD_PARAM_MVOLL, 0x3fff);
        sceSdSetParam(core | SD_PARAM_MVOLR, 0x3fff);
        sceSdSetParam(core | SD_PARAM_EVOLL, 0);
        sceSdSetParam(core | SD_PARAM_EVOLR, 0);
        sceSdSetSwitch(core | SD_SWITCH_VMIXL, 0xffffff);
        sceSdSetSwitch(core | SD_SWITCH_VMIXR, 0xffffff);
        sceSdSetSwitch(core | SD_SWITCH_VMIXEL, 0);
        sceSdSetSwitch(core | SD_SWITCH_VMIXER, 0);
        sceSdSetSwitch(core | SD_SWITCH_KOFF, 0xffffff);
    }
    // MMIX: bits 10-11 voices to the dry output; core 1 also takes core 0's
    // output as its input (bits 2-3, dry).
    sceSdSetParam(0 | SD_PARAM_MMIX, 0x0c00);
    sceSdSetParam(1 | SD_PARAM_MMIX, 0x0c0c);
    sceSdSetParam(0 | SD_PARAM_AVOLL, 0x7fff);
    sceSdSetParam(0 | SD_PARAM_AVOLR, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_AVOLL, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_AVOLR, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_BVOLL, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_BVOLR, 0x7fff);
}

static void* rpc(int fno, void* data, int size) {
    u32* a = (u32*)data;
    (void)size;
    switch (fno) {
    case HSA_INIT:
        init_spu();
        a[0] = 1;
        break;
    case HSA_WRITE: {  // a[0] SPU2 byte address, a[1] bytes (multiple of 64), data from byte 64
        u32 bytes = a[1];
        if (bytes && bytes <= HSA_RPC_BUFFER - HSA_HEADER) {
            sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (u8*)a + HSA_HEADER, (u32*)a[0], bytes);
            sceSdVoiceTransStatus(0, 1);
        }
        a[0] = bytes;
        break;
    }
    case HSA_PARAMS: {  // pairs of (voice, volL | volR << 16, pitch), a[0] = count
        u32 i, n = a[0];
        for (i = 0; i < n && i < 48; i++) {
            u32* p = &a[1 + i * 3];
            int v = (int)p[0];
            if (v < 0 || v >= 48) continue;
            sceSdSetParam(voice_entry(v, SD_VPARAM_VOLL), (u16)(p[1] & 0x3fff));
            sceSdSetParam(voice_entry(v, SD_VPARAM_VOLR), (u16)((p[1] >> 16) & 0x3fff));
            sceSdSetParam(voice_entry(v, SD_VPARAM_PITCH), (u16)(p[2] & 0x3fff));
        }
        break;
    }
    case HSA_KEY: {  // a[0] on, a[1] count, then (voice, start, loop start) each:
        u32 i, n = a[1];  // keyed together, so a stereo pair starts on the same sample
        u32 bits[2] = {0, 0};
        for (i = 0; i < n && i < 4; i++) {
            u32* p = &a[2 + i * 3];
            int v = (int)p[0];
            if (v < 0 || v >= 48) continue;
            bits[v / 24] |= 1u << (v % 24);
            if (!a[0]) continue;
            sceSdSetAddr(voice_entry(v, SD_VADDR_SSA), p[1]);
            sceSdSetAddr(voice_entry(v, SD_VADDR_LSAX), p[2]);
            sceSdSetParam(voice_entry(v, SD_VPARAM_ADSR1), 0x00ff);  // instant attack, full sustain
            sceSdSetParam(voice_entry(v, SD_VPARAM_ADSR2), 0x1fc0);  // hold, instant release
        }
        for (i = 0; i < 2; i++)
            if (bits[i]) sceSdSetSwitch(i | (a[0] ? SD_SWITCH_KON : SD_SWITCH_KOFF), bits[i]);
        break;
    }
    case HSA_STATUS: {  // replies with every voice's NAX, then ENVX
        int v;
        for (v = 0; v < 48; v++) {
            a[v] = sceSdGetAddr(voice_entry(v, SD_VADDR_NAX));
            a[48 + v] = sceSdGetParam(voice_entry(v, SD_VPARAM_ENVX));
        }
        break;
    }
    case HSA_READ: {  // a[0] SPU2 address, a[1] bytes (multiple of 64): reads back from byte 64
        u32 bytes = a[1];
        if (bytes && bytes <= HSA_RPC_BUFFER - HSA_HEADER) {
            sceSdVoiceTrans(0, SD_TRANS_READ | SD_TRANS_MODE_DMA, (u8*)a + HSA_HEADER, (u32*)a[0], bytes);
            sceSdVoiceTransStatus(0, 1);
        }
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
    sceSifRegisterRpc(&g_server, HSA_SID, rpc, g_buffer, NULL, NULL, &g_queue);
    sceSifRpcLoop(&g_queue);
}

int _start(int argc, char* argv[]) {
    iop_thread_t t;
    int id;
    (void)argc;
    (void)argv;
    sceSifInitRpc(0);
    memset(&t, 0, sizeof t);
    t.attr = TH_C;
    t.thread = server;
    t.stacksize = 0x1000;
    t.priority = 0x28;
    id = CreateThread(&t);
    if (id <= 0) return MODULE_NO_RESIDENT_END;
    StartThread(id, NULL);
    return MODULE_RESIDENT_END;
}
