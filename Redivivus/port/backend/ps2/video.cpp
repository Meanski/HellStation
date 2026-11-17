// PS2 backend: GS presentation through gsKit, and input (pad as virtual mouse,
// plus the scripted replay from host:input.txt when it exists).
//
// The game's 8-bit frame is uploaded as a PSMT8 texture (1024 wide, the GS
// buffer-width constraint for 8-bit textures, covering the 800-pixel frame)
// with a 256-entry CT32 CLUT, and drawn scaled to the display mode. Palette and
// gamma changes cost only the 1 KiB CLUT upload. Same path as tools/port/ps2-smoke.
#include "Redivivus/port/platform.h"
#include "Redivivus/port/backend/ps2/pad.h"
#include "Redivivus/port/input/input.h"

#include <cstring>
#include <dmaKit.h>
#include <gsKit.h>
#include <malloc.h>

namespace {
constexpr int kTexW = 1024;
constexpr int kTexH = 1024;
GSGLOBAL* g_gs;
GSTEXTURE g_tex;
int g_frameW, g_frameH;
uint32_t g_frameNo;      // presented frames: the clock for scripts and the pad
uint32_t g_padFrame = ~0u;
hs::input::Script g_script;
hs::input::VirtualMouse g_mouse;
int g_mouseX = 400, g_mouseY = 300;
bool g_keys[256];

// CSM1 CLUT: entries 8-15 and 16-23 of each 32-entry block are swapped.
unsigned clut_slot(unsigned i) { return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1); }
}  // namespace


extern "C" {

int hs_video_open(int width, int height, const char*) {
    if (!g_gs) {
        g_gs = gsKit_init_global();
        g_gs->PSM = GS_PSM_CT24;
        g_gs->PSMZ = GS_PSMZ_16S;
        g_gs->ZBuffering = GS_SETTING_OFF;
        g_gs->DoubleBuffering = GS_SETTING_ON;
        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);
        gsKit_init_screen(g_gs);
        gsKit_mode_switch(g_gs, GS_ONESHOT);

        std::memset(&g_tex, 0, sizeof g_tex);
        g_tex.Width = kTexW;
        g_tex.Height = kTexH;
        g_tex.PSM = GS_PSM_T8;
        g_tex.ClutPSM = GS_PSM_CT32;
        g_tex.Filter = GS_FILTER_LINEAR;
        g_tex.Mem = (u32*)memalign(128, gsKit_texture_size_ee(kTexW, kTexH, GS_PSM_T8));
        g_tex.Clut = (u32*)memalign(128, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));
        std::memset(g_tex.Mem, 0, gsKit_texture_size_ee(kTexW, kTexH, GS_PSM_T8));
        g_tex.Vram = gsKit_vram_alloc(g_gs, gsKit_texture_size(kTexW, kTexH, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
        g_tex.VramClut = gsKit_vram_alloc(g_gs, gsKit_texture_size(16, 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
        hs_log("GS: display %dx%d, frame texture %dx%d PSMT8", g_gs->Width, g_gs->Height, kTexW, kTexH);
    }
    g_frameW = width;
    g_frameH = height;
    return 0;
}

void hs_video_set_title(const char*) {}

void hs_video_present(const uint8_t* pixels, int width, int height, int pitch, const uint32_t* palette) {
    if (!g_gs || width != g_frameW || height != g_frameH) hs_video_open(width, height, nullptr);
    if (width > kTexW || height > kTexH) return;
    uint8_t* dst = (uint8_t*)g_tex.Mem;
    for (int y = 0; y < height; y++) std::memcpy(dst + y * kTexW, pixels + (ptrdiff_t)y * pitch, (size_t)width);
    u32* clut = (u32*)g_tex.Clut;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = palette[i];  // 0x00RRGGBB -> GS ABGR, alpha 0x80 = opaque
        clut[clut_slot(i)] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (0x80u << 24);
    }
    gsKit_texture_upload(g_gs, &g_tex);
    gsKit_clear(g_gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_prim_sprite_texture(g_gs, &g_tex, 0, 0, 0, 0, g_gs->Width, g_gs->Height, width, height, 1,
                              GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    gsKit_queue_exec(g_gs);
    gsKit_sync_flip(g_gs);
    // Frame pacing, logged every 250 presented frames: the longest gap and the slow frames.
    static uint32_t last, worst, slow;
    uint32_t now = hs_ticks_ms(), dt = last ? now - last : 0;
    last = now;
    if (dt > worst) worst = dt;
    if (dt > 50) slow++;
    if (++g_frameNo % 250 == 0) {
        hs_log("frame %u: worst gap %u ms, %u frames over 50 ms", (unsigned)g_frameNo, (unsigned)worst, (unsigned)slow);
        worst = slow = 0;
    }
}

// Called from the game's message pump. The pad is sampled once per presented
// frame, so cursor speed follows the game's frame rate, not the polling rate.
int hs_input_poll(hs_event* ev) {
    static bool started;
    if (!started) {
        started = true;
        hs_ps2_pad_init();
        g_script.load("host:input.txt");
    }
    bool got = g_script.next(g_frameNo, ev);
    if (!got && g_frameW && g_padFrame != g_frameNo) {
        g_padFrame = g_frameNo;
        hs::input::PadState pad;
        hs_ps2_pad_read(&pad);
        g_mouse.update(pad, &g_mouseX, &g_mouseY, g_frameW, g_frameH);
    }
    if (!got) got = g_mouse.next(ev);
    if (!got) return 0;
    if (ev->type == HS_EV_MOUSE_MOVE || ev->type == HS_EV_MOUSE_BUTTON) { g_mouseX = ev->x; g_mouseY = ev->y; }
    if (ev->type == HS_EV_MOUSE_BUTTON) g_keys[ev->button == HS_BUTTON_LEFT ? 0x01 : ev->button == HS_BUTTON_RIGHT ? 0x02 : 0x04] = ev->down;
    if (ev->type == HS_EV_KEY) g_keys[ev->vk & 0xFF] = ev->down;
    ev->x = g_mouseX;
    ev->y = g_mouseY;
    return 1;
}
int hs_input_key_down(int vk) { return g_keys[vk & 0xFF]; }
void hs_input_mouse_pos(int* x, int* y) { *x = g_mouseX; *y = g_mouseY; }
void hs_input_warp_mouse(int x, int y) { g_mouseX = x; g_mouseY = y; }
void hs_input_show_cursor(int) {}  // D2 draws its own cursor into the frame

}  // extern "C"
