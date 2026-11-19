// In-game panel art, resident only while its panel is open, on a small-RAM target.
//
// Entering a game, D2Client loads the art of every panel and keeps it until the
// game ends: in a Black Marsh fight 1.6 MB of DC6 belonged to closed panels
// (PORTING_LOG, 2026-11-19). Each image below is read only by its own panel's
// draw, which DRAW_UI calls when that panel's flag in GFX_bShowUIFlag is set
// (checked in Source/D2Client: no hit test or other code reads them). So on a
// target reporting 32 MB or less, DRAW_UI (wrapped) first loads the art of every
// open panel, then draws, then frees what has been closed for kIdleFrames:
//
//   skill tree background   pSpellsClassSkilltree[class] (SpellSel, static)  367 KiB
//   character panel         Panel::InvChar (stats and inventory)              276 KiB
//   trade window            gpNpcMenuShopInventoryDC6 (npcmenu)               138 KiB
//   mercenary inventory     gpInvMercBodyImage (Inv, static)                  138 KiB
//   party screen            Party::pMenu_party                                138 KiB
//   waypoint list           WaypointWayGateBackGround (Waypoint, static)      138 KiB
//   other acts' automap     Automap::pAutoMapDC6[1..3]                        388 KiB
//
// The automap's tileset 0 draws every act's floors and walls and stays; sets
// 1-3 draw the "extras" of one act each (the current level's fSaved), so only
// the current act's is kept, whether or not the automap is open.
//
// An image is reloaded only when this file freed it and its slot is still
// empty, so the game's own loads and frees (entering and leaving a game,
// AUTOMAP_LoadDC6Sprites) are never doubled: a slot the game refilled clears
// the mark. Everything runs on the main thread, between and around draws.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/memstats.h"

#if defined(HS_FAKE_I386)
#  define HS_FC
#  define HS_SC
#else
#  define HS_FC __attribute__((__fastcall__))
#  define HS_SC __attribute__((__stdcall__))
#endif

struct DC6;
struct D2GameViewStrc;
struct HsAutomapLayer {  // D2AutomapLayerStrc (D2Client/D2AutomapLayerStrc.h)
    int32_t nLayer;
    int32_t fSaved;  // which tileset (pAutoMapDC6 index) draws its extras
};

extern "C" {
HS_FC void hs_orig_DRAW_UI(D2GameViewStrc*) __asm__("hs_orig__ZN8D2Client2UI4Draw7DRAW_UIEP14D2GameViewStrc");
HS_FC DC6* hs_LoadDC6Ex(char* path, int32_t dcc) __asm__("_ZN8D2Client4Core7Archive15IMAGE_LoadDC6ExEPci");
HS_FC void hs_FreeDC6(DC6*) __asm__("_ZN8D2Client4Core7Archive17DC6_FreeAndDeleteEP3DC6");
HS_FC DC6* hs_LoadOverlay(char* path) __asm__("_ZN8D2Client2UI2ui34ARCHIVE_LoadOverlayImageUsedInGameEPc");
HS_SC void hs_LoadSkilltree(void) __asm__("_ZN8D2Client2UI8SpellSel32ARCHIVE_LoadSpellsClassSkilltreeEv");
HS_SC uint32_t hs_LanguageCode(void) __asm__("_ZN6D2Lang8StrTable8strtable20MISC_GetLanguageCodeEv");
HS_SC int32_t hs_ExpansionData(void) __asm__("_ZN3Fog20D2CheckExpansionGameEv");
HS_SC int32_t hs_ExpansionGame(void) __asm__("_ZN8D2Client4GAME4Game21CLIENT_CheckExpansionEv");

extern int32_t hs_ui_flags[38] __asm__("_ZN8D2Client2UI4Draw15GFX_bShowUIFlagE");  // D2UIFlagStrc: one BOOL per panel
extern uint8_t hs_skilltree[7 * 34] __asm__("hs_D2Client_pSpellsClassSkilltree");  // D2ClassIconWithNameStrc[7], packed
                                                                                // (pD2Archive, a 30-byte path); globalized
extern int32_t hs_player_class __asm__("nD2PlayerClassID");
extern DC6* hs_InvChar __asm__("_ZN8D2Client2UI5Panel7InvCharE");
extern DC6* hs_ShopDC6 __asm__("gpNpcMenuShopInventoryDC6");
extern DC6* hs_MercBody __asm__("hs_D2Client_gpInvMercBodyImage");  // Inv.cpp's static, globalized (cmake/port.cmake)
extern DC6* hs_PartyDC6 __asm__("_ZN8D2Client2UI5Party11pMenu_partyE");
extern DC6* hs_WaypointBack __asm__("hs_D2Client_WaypointWayGateBackGround");  // Waypoint.cpp's static, globalized
extern DC6* hs_AutomapDC6[4] __asm__("_ZN8D2Client2UI7Automap11pAutoMapDC6E");
extern uint32_t hs_MiniMapType __asm__("_ZN8D2Client2UI7Automap11MiniMapTypeE");
extern HsAutomapLayer* hs_AutomapLayer __asm__("_ZN8D2Client2UI7Automap17sgpAutomapLayerExE");
}

namespace {
constexpr unsigned kIdleFrames = 50;  // about two seconds closed before the art goes

// GFX_bShowUIFlag indices (D2Client/D2Client.globals.h).
enum : int {
    kInventory = 1, kCharStat = 2, kSkill = 4, kNpcMenu = 8, kAutomap = 10, kNpcShop = 12, kCash = 14,
    kWaypointMenu = 20, kPartyMenu = 22, kTradePrompt = 23, kStashMenu = 25, kCubeMenu = 26, kGuildVault = 28,
    kX1D = 29, kMercMenu = 36,
};
bool flag(int i) { return hs_ui_flags[i] != 0; }
bool expansion() { return hs_ExpansionData() && hs_ExpansionGame(); }

// The skill tree slot of the player's class (unaligned: the table is packed).
uint8_t* skilltree_slot() {
    int32_t c = hs_player_class;
    return c >= 0 && c < 7 ? hs_skilltree + 34 * c : nullptr;
}
DC6* get_skilltree() {
    DC6* p = nullptr;
    if (uint8_t* s = skilltree_slot()) memcpy(&p, s, sizeof p);
    return p;
}
void set_skilltree(DC6* p) {
    if (uint8_t* s = skilltree_slot()) memcpy(s, &p, sizeof p);
}

DC6* load_path(const char* fmt, const char* root) {
    char path[260];
    snprintf(path, sizeof path, fmt, root);
    return hs_LoadDC6Ex(path, 0);
}

struct Art {
    const char* name;
    DC6* (*get)();
    void (*set)(DC6*);
    bool (*needed)();
    DC6* (*load)();  // as the game loaded it
    bool freed;      // this file freed it, and the game hasn't refilled the slot
    unsigned idle;   // frames not needed
};

#define HS_SLOT(var) [] { return var; }, [](DC6* p) { var = p; }
bool inventory_open() {
    return flag(kInventory) || flag(kNpcShop) || flag(kCash) || flag(kStashMenu) || flag(kCubeMenu) ||
           flag(kGuildVault) || flag(kX1D) || flag(kTradePrompt);
}
int automap_set() { return hs_AutomapLayer ? hs_AutomapLayer->fSaved : -1; }
DC6* load_automap(int i) {
    static const char* const kNames[4] = {"MaxiMap", "Act2Map", "Act4Map", "ExTnMap"};
    char fmt[96];
    snprintf(fmt, sizeof fmt, "%%s\\UI\\AutoMap\\%s%s", kNames[i], hs_MiniMapType == 1 ? "S" : "");
    return load_path(fmt, "DATA\\GLOBAL");
}

Art g_art[] = {
    {"skill tree", get_skilltree, set_skilltree, [] { return flag(kSkill); },
     [] { hs_LoadSkilltree(); return get_skilltree(); }, false, 0},
    {"character panel", HS_SLOT(hs_InvChar), [] { return flag(kCharStat) || inventory_open(); },
     [] {
         uint32_t lang = hs_LanguageCode();
         if (lang >= 6 && lang <= 9) return load_path(expansion() ? "%s\\UI\\InvChar6" : "%s\\UI\\InvChar", "DATA\\LOCAL");
         return hs_LoadOverlay(expansion() ? (char*)"Panel\\InvChar6" : (char*)"Panel\\InvChar");
     }, false, 0},
    {"trade window", HS_SLOT(hs_ShopDC6), [] { return inventory_open() || flag(kNpcMenu); },
     [] { return load_path("%s\\ui\\panel\\buysell", "DATA\\GLOBAL"); }, false, 0},
    {"mercenary inventory", HS_SLOT(hs_MercBody), [] { return flag(kMercMenu); },
     [] { return hs_LoadOverlay((char*)"Panel\\NPCInv"); }, false, 0},
    {"party screen", HS_SLOT(hs_PartyDC6), [] { return flag(kPartyMenu); },
     [] { return hs_LoadOverlay((char*)"Menu\\party"); }, false, 0},
    {"waypoint list", HS_SLOT(hs_WaypointBack), [] { return flag(kWaypointMenu); },
     [] { return load_path("%s\\ui\\menu\\waygatebackground", "DATA\\GLOBAL"); }, false, 0},
    {"automap act 2", HS_SLOT(hs_AutomapDC6[1]), [] { return automap_set() == 1; }, [] { return load_automap(1); }, false, 0},
    {"automap act 4", HS_SLOT(hs_AutomapDC6[2]), [] { return automap_set() == 2; }, [] { return load_automap(2); }, false, 0},
    {"automap act 5", HS_SLOT(hs_AutomapDC6[3]), [] { return automap_set() == 3; }, [] { return load_automap(3); }, false, 0},
};
#undef HS_SLOT

int g_on = -1;
bool on() {
    if (g_on < 0) {
        hs_mem_info mi = {};
        hs_mem_query(&mi);
        g_on = mi.phys_total && mi.phys_total <= (32u << 20);
        if (g_on) hs_log("panel art: loaded while open, freed %u frames after closing", kIdleFrames);
    }
    return g_on;
}
unsigned g_loads, g_frees;
}  // namespace

namespace D2Client::UI::Draw {
HS_FC void DRAW_UI(D2GameViewStrc* view) {
    if (!on()) return hs_orig_DRAW_UI(view);
    for (Art& a : g_art) {
        if (a.get()) a.freed = false;  // loaded, by the game or by us
        if (!a.needed()) continue;
        a.idle = 0;
        if (a.freed && !a.get()) {
            a.set(a.load());
            a.freed = false;
            if (++g_loads <= 32) hs_log("panel art: loaded the %s", a.name);
        }
    }
    hs_orig_DRAW_UI(view);
    for (Art& a : g_art) {
        if (a.needed()) continue;  // a panel the draw just opened
        if (++a.idle < kIdleFrames) continue;
        a.idle = kIdleFrames;
        if (DC6* p = a.get()) {
            a.set(nullptr);
            hs_FreeDC6(p);
            a.freed = true;
            if (++g_frees <= 32) hs_log("panel art: freed the %s", a.name);
        }
    }
}
}  // namespace D2Client::UI::Draw
