// The hsaudio.irx RPC interface, shared by the IOP module and the EE backend.
#pragma once

#define HSA_SID 0x48534131  // "HSA1"
#define HSA_HEADER 64                       // command words; data starts here (64-byte aligned for DMA)
#define HSA_RPC_BUFFER (HSA_HEADER + 16 * 1024)  // per call: up to 16 KiB of ADPCM

enum {
    HSA_INIT = 1,
    HSA_WRITE = 2,
    HSA_PARAMS = 3,
    HSA_KEY = 4,
    HSA_STATUS = 5,
    HSA_READ = 6,
};

// The disc read cache (disc.c): its own server, so a read waiting on the drive
// doesn't hold up audio.
#define HSD_SID 0x48534431  // "HSD1"
#define HSD_HEADER 64
#define HSD_RPC_BUFFER (HSD_HEADER + 32 * 1024)  // up to 32 KiB a read
enum {
    HSD_FIND = 1,
    HSD_READ = 2,
};
