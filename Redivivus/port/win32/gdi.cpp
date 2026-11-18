// Win32 GDI: just enough to present the software renderer's frame.
//
// D2Gdi draws into an 8-bit DIB section, sets its colour table, and
// StretchBlts it to the window DC (Source/D2Gdi/Renderer/Windowed.cpp,
// EndScene2). Here that blit becomes hs_video_present(): indices + palette,
// which the backend scales and shows. Text and font calls are logged stubs.
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/memstats.h"
#include "Redivivus/port/win32/window.h"

namespace {

struct GdiObj {
    static constexpr uint32_t kMagic = 0x48534744;  // 'HSGD'
    enum Type { Bitmap, Dc, Palette, Font, Brush, Pen } type;
    uint32_t magic = kMagic;
    explicit GdiObj(Type t) : type(t) {}
    virtual ~GdiObj() { magic = 0; }
};

struct HsBitmap : GdiObj {
    int width = 0, height = 0, bpp = 0, pitch = 0;
    bool topDown = false;
    uint8_t* bits = nullptr;
    uint32_t palette[256] = {};  // 0x00RRGGBB
    HsBitmap() : GdiObj(Bitmap) {}
    ~HsBitmap() override { std::free(bits); }
};

struct HsDc : GdiObj {
    HWND window = nullptr;     // window/screen DC
    bool memory = false;       // CreateCompatibleDC
    HsBitmap* bitmap = nullptr;
    HsDc() : GdiObj(Dc) {}
};

struct HsSimple : GdiObj { explicit HsSimple(Type t) : GdiObj(t) {} };

template <class T> T* as(void* h, GdiObj::Type t) {
    GdiObj* o = (GdiObj*)h;
    return (o && o->magic == GdiObj::kMagic && o->type == t) ? static_cast<T*>(o) : nullptr;
}

void present(HsBitmap* b) {
    if (!b || !b->bits || b->bpp != 8) {
        if (b) HS_STUB("present of a non-8-bit bitmap ignored");
        return;
    }
    const uint8_t* first = b->topDown ? b->bits : b->bits + (size_t)(b->height - 1) * b->pitch;
    hs_video_present(first, b->width, b->height, b->topDown ? b->pitch : -b->pitch, b->palette);
    static unsigned s_frames;
    if (++s_frames % 250 == 0) hs_mem_report(s_frames);
}

void set_colors(HsBitmap* b, UINT start, UINT n, const RGBQUAD* c) {
    for (UINT i = 0; i < n && start + i < 256; i++)
        b->palette[start + i] = ((uint32_t)c[i].rgbRed << 16) | ((uint32_t)c[i].rgbGreen << 8) | c[i].rgbBlue;
}

}  // namespace

HDC WINAPI GetDC(HWND h) {
    HsDc* dc = new HsDc;
    dc->window = h;
    return (HDC)dc;
}
HDC WINAPI GetWindowDC(HWND h) { return GetDC(h); }
int WINAPI ReleaseDC(HWND, HDC dc) { delete as<HsDc>(dc, GdiObj::Dc); return 1; }
HDC WINAPI CreateCompatibleDC(HDC) {
    HsDc* dc = new HsDc;
    dc->memory = true;
    return (HDC)dc;
}
HDC WINAPI CreateDCA(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA*) { return GetDC(nullptr); }
BOOL WINAPI DeleteDC(HDC dc) { delete as<HsDc>(dc, GdiObj::Dc); return TRUE; }

HBITMAP WINAPI CreateDIBSection(HDC, const BITMAPINFO* bmi, UINT usage, void** bits, HANDLE, DWORD) {
    const BITMAPINFOHEADER& h = bmi->bmiHeader;
    HsBitmap* b = new HsBitmap;
    b->width = h.biWidth;
    b->height = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    b->topDown = h.biHeight < 0;
    b->bpp = h.biBitCount;
    b->pitch = ((b->width * b->bpp / 8) + 3) & ~3;
    b->bits = (uint8_t*)memalign(16, (size_t)b->pitch * b->height);
    std::memset(b->bits, 0, (size_t)b->pitch * b->height);
    if (b->bpp == 8 && usage == DIB_RGB_COLORS) set_colors(b, 0, h.biClrUsed ? h.biClrUsed : 256, bmi->bmiColors);
    if (bits) *bits = b->bits;
    hs_log("CreateDIBSection %dx%d %dbpp %s", b->width, b->height, b->bpp, b->topDown ? "top-down" : "bottom-up");
    return (HBITMAP)b;
}

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ obj) {
    HsDc* dc = as<HsDc>(hdc, GdiObj::Dc);
    if (!dc) return nullptr;
    if (HsBitmap* b = as<HsBitmap>(obj, GdiObj::Bitmap)) {
        HsBitmap* old = dc->bitmap;
        dc->bitmap = b;
        return (HGDIOBJ)old;
    }
    return obj;  // fonts, brushes, pens: nothing to track
}

UINT WINAPI SetDIBColorTable(HDC hdc, UINT start, UINT n, const RGBQUAD* colors) {
    HsDc* dc = as<HsDc>(hdc, GdiObj::Dc);
    if (!dc || !dc->bitmap) return 0;
    set_colors(dc->bitmap, start, n, colors);
    return n;
}

BOOL WINAPI StretchBlt(HDC dst, int, int, int, int, HDC src, int, int, int, int, DWORD) {
    HsDc* s = as<HsDc>(src, GdiObj::Dc);
    HsDc* d = as<HsDc>(dst, GdiObj::Dc);
    if (s && d && !d->memory) present(s->bitmap);
    return TRUE;
}
BOOL WINAPI BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    return StretchBlt(dst, x, y, w, h, src, sx, sy, w, h, rop);
}
int WINAPI StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void* bits, const BITMAPINFO* bmi, UINT, DWORD) {
    const BITMAPINFOHEADER& h = bmi->bmiHeader;
    if (h.biBitCount != 8) { HS_STUB("non-8-bit StretchDIBits ignored"); return 0; }
    HsBitmap b;
    b.width = h.biWidth;
    b.height = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    b.topDown = h.biHeight < 0;
    b.bpp = 8;
    b.pitch = (b.width + 3) & ~3;
    b.bits = (uint8_t*)bits;
    set_colors(&b, 0, h.biClrUsed ? h.biClrUsed : 256, bmi->bmiColors);
    present(&b);
    b.bits = nullptr;  // not ours
    return b.height;
}
int WINAPI SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT, UINT, const void* bits,
                             const BITMAPINFO* bmi, UINT usage) {
    return StretchDIBits(dc, x, y, (int)w, (int)h, sx, sy, (int)w, (int)h, bits, bmi, usage, SRCCOPY);
}
BOOL WINAPI GdiFlush(void) { return TRUE; }

static HsBitmap* new_bitmap(int w, int h, int bpp) {
    HsBitmap* b = new HsBitmap;
    b->width = w;
    b->height = h;
    b->bpp = bpp;
    b->pitch = ((w * bpp / 8) + 3) & ~3;
    b->bits = (uint8_t*)std::calloc((size_t)b->pitch * (h ? h : 1), 1);
    return b;
}
HBITMAP WINAPI CreateBitmap(int w, int h, UINT, UINT bpp, const void* bits) {
    HsBitmap* b = new_bitmap(w, h, (int)bpp);
    if (bits) std::memcpy(b->bits, bits, (size_t)b->pitch * h);
    return (HBITMAP)b;
}
HBITMAP WINAPI CreateCompatibleBitmap(HDC, int w, int h) { return (HBITMAP)new_bitmap(w, h, 32); }

BOOL WINAPI DeleteObject(HGDIOBJ obj) {
    GdiObj* o = (GdiObj*)obj;
    if (o && o->magic == GdiObj::kMagic && o->type != GdiObj::Dc) delete o;
    return TRUE;
}
HGDIOBJ WINAPI GetStockObject(int) { static HsSimple s(GdiObj::Brush); return (HGDIOBJ)&s; }
int WINAPI GetObjectA(HANDLE obj, int n, LPVOID out) {
    HsBitmap* b = as<HsBitmap>(obj, GdiObj::Bitmap);
    if (!b || n < (int)sizeof(BITMAP)) return 0;
    BITMAP* bm = (BITMAP*)out;
    std::memset(bm, 0, sizeof *bm);
    bm->bmWidth = b->width;
    bm->bmHeight = b->height;
    bm->bmWidthBytes = b->pitch;
    bm->bmPlanes = 1;
    bm->bmBitsPixel = (WORD)b->bpp;
    bm->bmBits = b->bits;
    return sizeof(BITMAP);
}

// Palettes: the frame's palette travels with the DIB, so these only need to exist.
HPALETTE WINAPI CreatePalette(const LOGPALETTE*) { return (HPALETTE) new HsSimple(GdiObj::Palette); }
HPALETTE WINAPI SelectPalette(HDC, HPALETTE p, BOOL) { return p; }
UINT WINAPI RealizePalette(HDC) { return 0; }
UINT WINAPI SetPaletteEntries(HPALETTE, UINT, UINT n, const PALETTEENTRY*) { return n; }
UINT WINAPI GetSystemPaletteEntries(HDC, UINT, UINT n, LPPALETTEENTRY e) {
    if (e) std::memset(e, 0, n * sizeof *e);
    return n;
}
UINT WINAPI SetSystemPaletteUse(HDC, UINT) { return SYSPAL_STATIC; }
int WINAPI GetDeviceCaps(HDC, int i) {
    switch (i) {
    case BITSPIXEL: return 32;
    case PLANES: return 1;
    case HORZRES: return GetSystemMetrics(SM_CXSCREEN);
    case VERTRES: return GetSystemMetrics(SM_CYSCREEN);
    case RASTERCAPS: return RC_BITBLT | RC_STRETCHBLT | RC_DI_BITMAP;
    case VREFRESH: return 60;
    case SIZEPALETTE: return 0;
    default: return 0;
    }
}

// Text: used only by error and launcher paths that never show on this target.
HFONT WINAPI CreateFontA(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR) {
    HS_STUB("fonts are not rendered; returns a dummy font");
    return (HFONT) new HsSimple(GdiObj::Font);
}
HFONT WINAPI CreateFontIndirectA(const LOGFONTA*) { return CreateFontA(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, nullptr); }
int WINAPI DrawTextA(HDC, LPCSTR text, int, LPRECT, UINT) {
    HS_STUB("GDI text is not drawn");
    (void)text;
    return 16;
}
BOOL WINAPI TextOutA(HDC, int, int, LPCSTR, int) { HS_STUB("GDI text is not drawn"); return TRUE; }
BOOL WINAPI GetTextExtentPoint32A(HDC, LPCSTR, int n, LPSIZE s) { s->cx = n * 8; s->cy = 16; return TRUE; }
BOOL WINAPI GetTextMetricsA(HDC, LPTEXTMETRICA tm) { std::memset(tm, 0, sizeof *tm); tm->tmHeight = 16; tm->tmAveCharWidth = 8; return TRUE; }
COLORREF WINAPI SetTextColor(HDC, COLORREF c) { return c; }
COLORREF WINAPI SetBkColor(HDC, COLORREF c) { return c; }
int WINAPI SetBkMode(HDC, int m) { return m; }
HBRUSH WINAPI CreateSolidBrush(COLORREF) { return (HBRUSH) new HsSimple(GdiObj::Brush); }
int WINAPI FillRect(HDC, const RECT*, HBRUSH) { return 1; }
