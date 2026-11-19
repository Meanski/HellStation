// PS2 backend: GS presentation through gsKit, and input (pad as virtual mouse,
// plus the scripted replay from host:input.txt when it exists).
//
// The game's 8-bit frame is uploaded as a PSMT8 texture (1024 wide, the GS
// buffer-width constraint for 8-bit textures, covering the 800-pixel frame)
// with a 256-entry CT32 CLUT, and drawn scaled to the display mode. Palette and
// gamma changes cost only the 1 KiB CLUT upload. Same path as tools/port/ps2-smoke.
//
// Buffers flip at vertical blank from an interrupt handler instead of with
// gsKit_sync_flip, which spins until the next blank: that rounded every frame up
// to whole fields (20 ms on PAL), so 48 ms of work in Harrogath's lower town
// became 60-80 ms frames (PORTING_LOG, 2026-11-19). Now a present draws into the
// back buffer and marks it ready; the handler displays it at the next blank; the
// next present waits only if that blank hasn't come yet.
#include "Redivivus/port/platform.h"
#include "Redivivus/port/backend/ps2/pad.h"
#include "Redivivus/port/input/input.h"

#include <cstring>
#include <kernel.h>
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

volatile bool g_flipPending;  // a drawn buffer waits for the next vertical blank
int g_flipSema = -1;

int on_vblank(int) {
    if (g_flipPending) {
        gsKit_display_buffer(g_gs);  // show the buffer the last present drew
        g_flipPending = false;
        iSignalSema(g_flipSema);
    }
    ExitHandler();
    return 0;
}

// CSM1 CLUT: entries 8-15 and 16-23 of each 32-entry block are swapped.
unsigned clut_slot(unsigned i) { return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1); }
}  // namespace

void hs_prof_start(void);            // profiler.cpp: no-ops without -DHS_PROFILE
void hs_prof_report(unsigned frame);

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
        ee_sema_t sema = {};
        sema.init_count = 0;
        sema.max_count = 1;
        g_flipSema = CreateSema(&sema);
        gsKit_add_vsync_handler(on_vblank);
        hs_prof_start();
    }
    g_frameW = width;
    g_frameH = height;
    return 0;
}

void hs_video_set_title(const char*) {}

void hs_video_present(const uint8_t* pixels, int width, int height, int pitch, const uint32_t* palette) {
    if (!g_gs || width != g_frameW || height != g_frameH) hs_video_open(width, height, nullptr);
    if (width > kTexW || height > kTexH) return;
    // The previous frame must be on screen before its buffer becomes the back buffer.
    static bool drawnOnce;
    while (g_flipPending) WaitSema(g_flipSema);  // a stale signal just loops once more
    if (drawnOnce) gsKit_switch_context(g_gs);
    drawnOnce = true;
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
    g_flipPending = true;  // on_vblank shows it
    // Frame pacing, logged every 250 presented frames: the longest gap and the slow frames.
    static uint32_t last, worst, slow, windowStart;
    uint32_t now = hs_ticks_ms(), dt = last ? now - last : 0;
    last = now;
    if (dt > worst) worst = dt;
    if (dt > 50) slow++;
    if (++g_frameNo % 250 == 0) {
        uint32_t avg10 = windowStart ? (now - windowStart) * 10 / 250 : 0;  // tenths of a ms
        hs_log("frame %u: avg %u.%u ms, worst gap %u ms, %u frames over 50 ms", (unsigned)g_frameNo,
               (unsigned)(avg10 / 10), (unsigned)(avg10 % 10), (unsigned)worst, (unsigned)slow);
        worst = slow = 0;
        windowStart = now;
    }
    if (g_frameNo % 250 == 0) hs_prof_report(g_frameNo);
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
    // Live input from the host, about once a second (tools/port/live.sh).
    static uint32_t livePolled = ~0u;
    if (g_frameNo / 25 != livePolled) {
        livePolled = g_frameNo / 25;
        g_script.poll_live("host:live.txt", g_frameNo);
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
