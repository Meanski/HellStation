// The class-select screen's animations, loaded per class on a small-RAM target.
//
// Creating a character, the menu loads every class's five animations and their
// overlays at once (D2Launch IMAGE_LoadPredefinedImagesEx): 12.3 MB of DC6 for
// the seven classes, the highest point of a session (31.7 MB against a retail
// 24.2 MB heap; PORTING_LOG, 2026-11-19). Idle and mouse-over (rows 0 and 1)
// are needed for every class at all times, 2.5 MB together. Walking forward,
// standing selected and walking back (rows 2-4, with their overlays) are needed
// only for the class the cursor is on or that is selected.
//
// So on a target reporting 32 MB or less:
//   - IMAGE_LoadDC6 (wrapped) skips the row 2-4 files, leaving their slots null;
//   - D2WinAnimImage (the control's constructor, wrapped) swaps in a draw
//     function that loads a class's row 2-4 images as soon as its control
//     leaves row 0 (the mouse-over is detected inside the draw, so it checks
//     again after drawing), and frees them once the control is back in row 0.
// A press can only reach row 2 from row 1, so the images are in place before any
// code reads them. The menu's own teardown frees every slot; IMAGE_FreeImage
// ignores the null ones.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Redivivus/port/platform.h"

#if defined(HS_FAKE_I386)
#  define HS_FC
#else
#  define HS_FC __attribute__((__fastcall__))
#endif

struct DC6;
struct HsAnimFrame {  // FORMS_AnimFrameStrc (D2Win/D2WinAnimImage.h)
    DC6** ppBaseImage;
    DC6** ppOverlayImage;
    int32_t nDrawMode;
    int32_t nFrameDelay;
};
static_assert(sizeof(HsAnimFrame) == 16 || sizeof(void*) == 8, "FORMS_AnimFrameStrc is 16 bytes");

// The seven class tables (D2Launch MainMenus.cpp, global scope).
extern HsAnimFrame gaCharSelectAnimFramesAmazon[5], gaCharSelectAnimFramesAssassin[5],
    gaCharSelectAnimFramesBarbarian[5], gaCharSelectAnimFramesDruid[5], gaCharSelectAnimFramesNecromancer[5],
    gaCharSelectAnimFramesPaladin[5], gaCharSelectAnimFramesSorceress[5];

extern "C" {
HS_FC DC6* hs_orig_IMAGE_LoadDC6(char* path, int32_t flags)
    __asm__("hs_orig__ZN5D2Win3Src12D2WinArchive13IMAGE_LoadDC6EPci");
HS_FC void hs_IMAGE_FreeImage(DC6* p) __asm__("_ZN5D2Win3Src12D2WinArchive15IMAGE_FreeImageEP3DC6");
HS_FC void* hs_orig_D2WinAnimImage(int32_t x, int32_t y, int32_t w, int32_t h, DC6* pDC6, int32_t delay, void* onPress,
                                   HsAnimFrame* frames, uint32_t drawMode, void* onStateChange)
    __asm__("hs_orig__ZN5D2Win14D2WinAnimImageEiiiiP3DC6iPFiP12D2ControlMsgEP19FORMS_AnimFrameStrcjPFvPvE");
HS_FC int32_t hs_D2Win_Draw(void* pAnimImage) __asm__("_ZN5D2Win4DrawEP14D2WinAnimImage");
}

namespace {
constexpr size_t kFpDraw = 0x1C, kAnimFrames = 0x40, kAnimState = 0x50;  // D2ControlStrc / D2WinAnimImage
const char* const kRowAnim[5] = {nullptr, nullptr, "fw", "nu3", "bw"};

struct ClassArt {
    HsAnimFrame* table;
    const char* dir;     // under data\global\ui\FrontEnd
    const char* prefix;  // file names start with it
    bool loaded;
};
ClassArt g_classes[] = {
    {gaCharSelectAnimFramesAmazon, "amazon", "am", false},
    {gaCharSelectAnimFramesAssassin, "assassin", "as", false},
    {gaCharSelectAnimFramesBarbarian, "barbarian", "ba", false},
    {gaCharSelectAnimFramesDruid, "druid", "dz", false},
    {gaCharSelectAnimFramesNecromancer, "necromancer", "ne", false},
    {gaCharSelectAnimFramesPaladin, "paladin", "pa", false},
    {gaCharSelectAnimFramesSorceress, "sorceress", "so", false},
};

int g_on = -1;
bool on() {
    if (g_on < 0) {
        hs_mem_info mi = {};
        hs_mem_query(&mi);
        g_on = mi.phys_total && mi.phys_total <= (32u << 20);
    }
    return g_on;
}

template <typename T> T& field(void* base, size_t off) { return *(T*)((char*)base + off); }

ClassArt* class_of(HsAnimFrame* table) {
    for (ClassArt& c : g_classes)
        if (c.table == table) return &c;
    return nullptr;
}

// A row 2-4 file of a class's art: "<prefix>(fw|nu3|bw)[s]" in its directory.
bool deferred(const char* path) {
    const char* name = strrchr(path, '\\');
    if (!name || !strstr(path, "FrontEnd")) return false;
    name++;
    for (ClassArt& c : g_classes) {
        if (strncmp(name, c.prefix, 2) != 0) continue;
        char dir[32];
        snprintf(dir, sizeof dir, "\\%s\\", c.dir);
        if (!strstr(path, dir)) continue;
        for (int row = 2; row < 5; row++) {
            size_t n = strlen(kRowAnim[row]);
            if (strncmp(name + 2, kRowAnim[row], n) == 0 && (name[2 + n] == '\0' || (name[2 + n] == 's' && name[3 + n] == '\0')))
                return true;
        }
    }
    return false;
}

void load_rows(ClassArt& c) {
    char path[128];
    for (int row = 2; row < 5; row++) {
        HsAnimFrame& f = c.table[row];
        snprintf(path, sizeof path, "DATA\\GLOBAL\\ui\\FrontEnd\\%s\\%s%s", c.dir, c.prefix, kRowAnim[row]);
        if (f.ppBaseImage && !*f.ppBaseImage) *f.ppBaseImage = hs_orig_IMAGE_LoadDC6(path, 0);
        if (f.ppOverlayImage && !*f.ppOverlayImage) {
            strncat(path, "s", sizeof path - strlen(path) - 1);
            *f.ppOverlayImage = hs_orig_IMAGE_LoadDC6(path, 0);
        }
    }
    c.loaded = true;
}
void free_rows(ClassArt& c) {
    for (int row = 2; row < 5; row++) {
        HsAnimFrame& f = c.table[row];
        if (f.ppBaseImage && *f.ppBaseImage) { hs_IMAGE_FreeImage(*f.ppBaseImage); *f.ppBaseImage = nullptr; }
        if (f.ppOverlayImage && *f.ppOverlayImage) { hs_IMAGE_FreeImage(*f.ppOverlayImage); *f.ppOverlayImage = nullptr; }
    }
    c.loaded = false;
}
void settle(void* image) {
    ClassArt* c = class_of(field<HsAnimFrame*>(image, kAnimFrames));
    if (!c) return;
    bool idle = field<int32_t>(image, kAnimState) == 0;
    if (!idle && !c->loaded) load_rows(*c);
    if (idle && c->loaded) free_rows(*c);
}

HS_FC int32_t draw_lazily(void* image) {
    settle(image);
    int32_t r = hs_D2Win_Draw(image);
    settle(image);  // the mouse-over is detected inside the draw
    return r;
}
}  // namespace

extern "C" {
HS_FC DC6* hs_IMAGE_LoadDC6(char* path, int32_t flags) __asm__("_ZN5D2Win3Src12D2WinArchive13IMAGE_LoadDC6EPci");
HS_FC DC6* hs_IMAGE_LoadDC6(char* path, int32_t flags) {
    if (on() && path && deferred(path)) {
        // A fresh menu: whatever this class had loaded was freed by the menu's teardown.
        for (ClassArt& c : g_classes) c.loaded = false;
        return nullptr;
    }
    return hs_orig_IMAGE_LoadDC6(path, flags);
}

HS_FC void* hs_D2WinAnimImage(int32_t x, int32_t y, int32_t w, int32_t h, DC6* pDC6, int32_t delay, void* onPress,
                              HsAnimFrame* frames, uint32_t drawMode, void* onStateChange)
    __asm__("_ZN5D2Win14D2WinAnimImageEiiiiP3DC6iPFiP12D2ControlMsgEP19FORMS_AnimFrameStrcjPFvPvE");
HS_FC void* hs_D2WinAnimImage(int32_t x, int32_t y, int32_t w, int32_t h, DC6* pDC6, int32_t delay, void* onPress,
                              HsAnimFrame* frames, uint32_t drawMode, void* onStateChange) {
    void* image = hs_orig_D2WinAnimImage(x, y, w, h, pDC6, delay, onPress, frames, drawMode, onStateChange);
    if (image && on() && class_of(frames)) field<void*>(image, kFpDraw) = (void*)&draw_lazily;
    return image;
}
}  // extern "C"
