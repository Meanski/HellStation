// PS2 pad adapter: port 1 through the BIOS's SIO2MAN/PADMAN and libpad, into
// the neutral hs::input::PadState. Analogue (DualShock) mode is forced on.
#include "Redivivus/port/backend/ps2/pad.h"

#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>

namespace {
alignas(64) char g_padArea[256];
bool g_open;
int g_mode = -1;      // last logged PAD_MODECURID
unsigned g_retry;     // reads until the next analogue request

int stick(unsigned char v) { return (int)v - 128; }
}  // namespace

// SIO2MAN serves both the pads and the memory cards; it loads once.
int hs_ps2_load_sio2man() {
    static int r = 1;
    if (r == 1) {
        SifInitRpc(0);
        r = SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    }
    return r;
}

void hs_ps2_pad_init() {
    SifInitRpc(0);
    int a = hs_ps2_load_sio2man();
    int b = SifLoadModule("rom0:PADMAN", 0, nullptr);
    if (a < 0 || b < 0) { hs_log("pad: loading SIO2MAN/PADMAN failed (%d, %d)", a, b); return; }
    if (padInit(0) != 1 || padPortOpen(0, 0, g_padArea) == 0) { hs_log("pad: padman init failed"); return; }
    g_open = true;
    hs_log("pad: port 1 open");
}

void hs_ps2_pad_read(hs::input::PadState* out) {
    *out = {};
    if (!g_open) return;
    int state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        if (state == PAD_STATE_DISCONN) g_mode = -1;
        return;
    }
    // Analogue mode is requested until the pad reports it: the request is
    // dropped if it arrives while the pad is still negotiating.
    if (state == PAD_STATE_STABLE) {
        int mode = padInfoMode(0, 0, PAD_MODECURID, 0);
        if (mode != g_mode) { hs_log("pad: mode id %d%s", mode, mode == 7 ? " (analogue)" : ""); g_mode = mode; }
        if (mode != 7 && g_retry-- == 0) {
            padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
            g_retry = 30;
        }
    }
    padButtonStatus raw;
    if (padRead(0, 0, &raw) == 0) return;
    unsigned b = 0xFFFFu ^ raw.btns;  // active low
    using namespace hs::input;
    static const struct { unsigned pad; uint32_t ours; } kMap[] = {
        {PAD_UP, kPadUp}, {PAD_DOWN, kPadDown}, {PAD_LEFT, kPadLeft}, {PAD_RIGHT, kPadRight},
        {PAD_CROSS, kPadCross}, {PAD_CIRCLE, kPadCircle}, {PAD_SQUARE, kPadSquare}, {PAD_TRIANGLE, kPadTriangle},
        {PAD_L1, kPadL1}, {PAD_R1, kPadR1}, {PAD_L2, kPadL2}, {PAD_R2, kPadR2},
        {PAD_L3, kPadL3}, {PAD_R3, kPadR3}, {PAD_START, kPadStart}, {PAD_SELECT, kPadSelect},
    };
    out->connected = true;
    for (const auto& m : kMap) if (b & m.pad) out->buttons |= m.ours;
    if (g_mode == 7) {  // sticks are only valid in analogue mode
        out->lx = stick(raw.ljoy_h); out->ly = stick(raw.ljoy_v);
        out->rx = stick(raw.rjoy_h); out->ry = stick(raw.rjoy_v);
    }
}
