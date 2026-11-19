// Phase 1 backend: SDL2 video and input.
//
// The 8-bit frame is expanded through the palette into an ARGB streaming
// texture and drawn letterboxed into the window. HS_FRAME_DUMP=<dir> also
// writes every Nth presented frame (HS_FRAME_DUMP_EVERY, default 60) as a PPM,
// which is how headless runs produce screenshots.
#include "Redivivus/port/platform.h"
#include "Redivivus/port/input/input.h"

#include <SDL2/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

SDL_Window* g_window;
SDL_Renderer* g_renderer;
SDL_Texture* g_texture;
int g_frameW, g_frameH;
std::vector<uint32_t> g_rgb;
bool g_keys[256];
int g_mouseX, g_mouseY;
std::mutex g_videoLock;  // SDL video calls stay on one thread at a time
uint32_t g_frameNo;

int vk_from_scancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    default: return 0;
    }
}

// PC set-1 scancode for the lParam of WM_KEY*: the subset the game reads.
using hs::input::set1_from_vk;

void to_frame_coords(int wx, int wy, int* fx, int* fy) {
    if (!g_renderer || !g_frameW) { *fx = wx; *fy = wy; return; }
    float lx, ly;
    SDL_RenderWindowToLogical(g_renderer, wx, wy, &lx, &ly);
    int x = (int)lx, y = (int)ly;
    *fx = x < 0 ? 0 : x >= g_frameW ? g_frameW - 1 : x;
    *fy = y < 0 ? 0 : y >= g_frameH ? g_frameH - 1 : y;
}

void dump_frame() {
    static const char* dir = std::getenv("HS_FRAME_DUMP");
    if (!dir) return;
    static uint32_t every = std::getenv("HS_FRAME_DUMP_EVERY") ? (uint32_t)std::atoi(std::getenv("HS_FRAME_DUMP_EVERY")) : 60;
    if (std::getenv("HS_FRAME_DUMP_LAST")) every = 1;  // keep only the newest frame (overwritten)
    if (!every || g_frameNo % every) return;
    char path[512];
    if (std::getenv("HS_FRAME_DUMP_LAST")) std::snprintf(path, sizeof path, "%s/last.ppm", dir);
    else std::snprintf(path, sizeof path, "%s/frame-%06u.ppm", dir, g_frameNo);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", g_frameW, g_frameH);
    for (uint32_t c : g_rgb) {
        unsigned char px[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

// ---- Scripted input (HS_INPUT_SCRIPT=<file>, HS_LIVE_INPUT=<file>; format in port/input/input.h) ----
hs::input::Script g_script;

bool next_scripted(hs_event* ev) {
    static bool loaded;
    if (!loaded) { loaded = true; g_script.load(std::getenv("HS_INPUT_SCRIPT")); }
    // Live input (tools/port/live.sh, drive.py): steps appended to a file while
    // the game runs, read about once a second, as on the PS2.
    static const char* live = std::getenv("HS_LIVE_INPUT");
    static uint32_t livePolled = ~0u;
    if (live && *live && g_frameNo / 25 != livePolled) {
        livePolled = g_frameNo / 25;
        g_script.poll_live(live, g_frameNo);
    }
    if (!g_script.next(g_frameNo, ev)) return false;
    if (ev->type == HS_EV_MOUSE_MOVE || ev->type == HS_EV_MOUSE_BUTTON) { g_mouseX = ev->x; g_mouseY = ev->y; }
    if (ev->type == HS_EV_MOUSE_BUTTON && ev->button == HS_BUTTON_LEFT) g_keys[0x01] = ev->down;
    if (ev->type == HS_EV_MOUSE_BUTTON && ev->button == HS_BUTTON_RIGHT) g_keys[0x02] = ev->down;
    if (ev->type == HS_EV_KEY) g_keys[ev->vk & 0xFF] = ev->down;
    ev->x = g_mouseX; ev->y = g_mouseY;
    return true;
}

}  // namespace

extern "C" {

int hs_video_open(int width, int height, const char* title) {
    std::lock_guard<std::mutex> l(g_videoLock);
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        hs_log("SDL video init failed: %s", SDL_GetError());
        return -1;
    }
    if (!g_window) {
        g_window = SDL_CreateWindow(title ? title : "HellStation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    width * 2 > 1600 ? width : width * 2, height * 2 > 1200 ? height : height * 2,
                                    SDL_WINDOW_RESIZABLE);
        if (!g_window) { hs_log("SDL_CreateWindow failed: %s", SDL_GetError()); return -1; }
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_PRESENTVSYNC);
        if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }
    if (width != g_frameW || height != g_frameH) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_RenderSetLogicalSize(g_renderer, width, height);
        g_frameW = width;
        g_frameH = height;
        g_rgb.assign((size_t)width * height, 0);
    }
    return 0;
}

void hs_video_set_title(const char* title) {
    std::lock_guard<std::mutex> l(g_videoLock);
    if (g_window) SDL_SetWindowTitle(g_window, title);
}

void hs_video_present(const uint8_t* pixels, int width, int height, int pitch, const uint32_t* palette) {
    if (width != g_frameW || height != g_frameH) hs_video_open(width, height, nullptr);
    std::lock_guard<std::mutex> l(g_videoLock);
    if (!g_texture) return;
    for (int y = 0; y < height; y++) {
        const uint8_t* src = pixels + (ptrdiff_t)y * pitch;
        uint32_t* dst = &g_rgb[(size_t)y * width];
        for (int x = 0; x < width; x++) dst[x] = 0xFF000000u | palette[src[x]];
    }
    SDL_UpdateTexture(g_texture, nullptr, g_rgb.data(), width * 4);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    ++g_frameNo;
    dump_frame();
}

int hs_input_poll(hs_event* ev) {
    std::lock_guard<std::mutex> l(g_videoLock);
    if (next_scripted(ev)) return 1;
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        std::memset(ev, 0, sizeof *ev);
        ev->x = g_mouseX;
        ev->y = g_mouseY;
        switch (e.type) {
        case SDL_QUIT:
            ev->type = HS_EV_QUIT;
            return 1;
        case SDL_MOUSEMOTION:
            to_frame_coords(e.motion.x, e.motion.y, &g_mouseX, &g_mouseY);
            ev->type = HS_EV_MOUSE_MOVE;
            ev->x = g_mouseX; ev->y = g_mouseY;
            return 1;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            to_frame_coords(e.button.x, e.button.y, &g_mouseX, &g_mouseY);
            ev->type = HS_EV_MOUSE_BUTTON;
            ev->x = g_mouseX; ev->y = g_mouseY;
            ev->down = e.type == SDL_MOUSEBUTTONDOWN;
            ev->button = e.button.button == SDL_BUTTON_LEFT ? HS_BUTTON_LEFT
                       : e.button.button == SDL_BUTTON_RIGHT ? HS_BUTTON_RIGHT : HS_BUTTON_MIDDLE;
            if (ev->button == HS_BUTTON_LEFT) g_keys[0x01] = ev->down;
            if (ev->button == HS_BUTTON_RIGHT) g_keys[0x02] = ev->down;
            if (ev->button == HS_BUTTON_MIDDLE) g_keys[0x04] = ev->down;
            return 1;
        case SDL_MOUSEWHEEL:
            ev->type = HS_EV_MOUSE_WHEEL;
            ev->wheel = e.wheel.y;
            return 1;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int vk = vk_from_scancode(e.key.keysym.scancode);
            if (!vk) continue;
            ev->type = HS_EV_KEY;
            ev->vk = vk;
            ev->down = e.type == SDL_KEYDOWN;
            ev->repeat = e.key.repeat;
            ev->scancode = set1_from_vk(vk);
            g_keys[vk & 0xFF] = ev->down;
            return 1;
        }
        case SDL_TEXTINPUT: {
            unsigned char c = (unsigned char)e.text.text[0];
            if (c >= 0x80) {  // two-byte UTF-8 -> Latin-1
                unsigned char c2 = (unsigned char)e.text.text[1];
                if ((c & 0xE0) != 0xC0) continue;
                c = (unsigned char)(((c & 0x1F) << 6) | (c2 & 0x3F));
            }
            ev->type = HS_EV_CHAR;
            ev->ch = c;
            return 1;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                ev->type = HS_EV_FOCUS;
                ev->down = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
                return 1;
            }
            continue;
        default:
            continue;
        }
    }
    return 0;
}

int hs_input_key_down(int vk) { return g_keys[vk & 0xFF]; }
void hs_input_mouse_pos(int* x, int* y) { *x = g_mouseX; *y = g_mouseY; }
void hs_input_warp_mouse(int x, int y) {
    g_mouseX = x;
    g_mouseY = y;
    std::lock_guard<std::mutex> l(g_videoLock);
    if (!g_window || !g_renderer) return;
    // Logical -> window coordinates through the current scale and letterbox.
    int ww, wh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    float sx = (float)ww / g_frameW, sy = (float)wh / g_frameH, s = sx < sy ? sx : sy;
    int ox = (int)((ww - g_frameW * s) / 2), oy = (int)((wh - g_frameH * s) / 2);
    SDL_WarpMouseInWindow(g_window, ox + (int)(x * s), oy + (int)(y * s));
}
void hs_input_show_cursor(int show) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }

}  // extern "C"
