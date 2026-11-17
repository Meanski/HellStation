/*
 * PS2 toolchain and presentation smoke test.
 *
 * Draws a 640x480 8-bit indexed frame (the shape of D2Gdi's back buffer) as a
 * PSMT8 texture with a 256-entry CT32 CLUT, scaled to the display mode, and
 * cycles the palette every frame without re-uploading the indices. That is
 * the presentation path the port will use: index upload once per game frame,
 * palette and gamma through the CLUT.
 */
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <fcntl.h>
#include <unistd.h>

#define FB_W 640
#define FB_H 480

/* The GS stores a 256-entry CSM1 CLUT with entries 8-15 and 16-23 of every
 * 32-entry block swapped; write logical index i at its stored position. */
static unsigned clut_slot(unsigned i) {
    return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
}

static u32 rgb(unsigned r, unsigned g, unsigned b) {
    return (r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16) | (0x80u << 24);
}

static void fill_palette(u32 *clut, unsigned phase) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned k = (i + phase) & 0xff;
        unsigned r = k < 128 ? k * 2 : 255;          /* black -> red -> yellow -> white */
        unsigned g = k < 128 ? 0 : (k - 128) * 2;
        unsigned b = k < 192 ? 0 : (k - 192) * 4;
        clut[clut_slot(i)] = rgb(r, g, b);
    }
}

int main(void) {
    int lf = open("host:smoke.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lf >= 0) { write(lf, "smoke: host write ok\n", 21); close(lf); }
    GSGLOBAL *gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT24;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF,
                D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    GSTEXTURE tex;
    memset(&tex, 0, sizeof tex);
    tex.Width = FB_W;
    tex.Height = FB_H;
    tex.PSM = GS_PSM_T8;
    tex.ClutPSM = GS_PSM_CT32;
    tex.Filter = GS_FILTER_NEAREST;
    tex.Mem = memalign(128, gsKit_texture_size_ee(FB_W, FB_H, GS_PSM_T8));
    tex.Clut = memalign(128, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));
    tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(FB_W, FB_H, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
    tex.VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(16, 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

    /* Indices: a plasma-ish gradient plus a 32px checker, so scaling and
     * CLUT ordering mistakes are both visible. */
    u8 *px = (u8 *)tex.Mem;
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            px[y * FB_W + x] = (u8)((x + y) / 4 + (((x >> 5) ^ (y >> 5)) & 1) * 64);

    fill_palette(tex.Clut, 0);
    gsKit_texture_upload(gs, &tex);

    printf("HellStation ps2-smoke: GS %dx%d, texture %dx%d PSMT8 + CT32 CLUT\n",
           gs->Width, gs->Height, FB_W, FB_H);

    for (unsigned frame = 0;; frame++) {
        fill_palette(tex.Clut, frame);
        gsKit_texture_send_inline(gs, tex.Clut, 16, 16, tex.VramClut, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
        gsKit_prim_sprite_texture(gs, &tex, 0, 0, 0, 0, gs->Width, gs->Height, FB_W, FB_H, 1,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        if (frame % 300 == 0) printf("ps2-smoke: frame %u\n", frame);
    }
}
