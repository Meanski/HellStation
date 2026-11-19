// "where": the objects around the player, for driving the game from a script.
//
// A scripted or live "where" step (Redivivus/port/input/input.h) logs every
// object, warp and monster in the player's room and the rooms near it: its
// type, class (the row of objects.txt, levels' warps, monstats.txt), offset from
// the player in game coordinates, and the frame point to click to reach it. The
// point is the isometric projection of the offset (a subtile is 32x16 pixels:
// x = 16 (dx - dy), y = 8 (dx + dy)) from where the player stands on screen.
// Debugging aid only: nothing calls it unless a script asks.
#include <cstdint>
#include <cstdlib>

#include "Redivivus/port/platform.h"

struct HsPoint { int32_t x, y; };  // POINT (LONG is 32-bit on every port target)
static_assert(sizeof(HsPoint) == 8, "POINT is two 32-bit fields");

#if defined(HS_FAKE_I386)
#  define HS_CC
#else
#  define HS_CC __attribute__((__stdcall__))
#endif

extern "C" {
HS_CC void* hs_where_player(void) __asm__("_ZN8D2Client4UNIT6Player16GetPlayerUnit_UIEv");
HS_CC void* hs_where_room(void* unit) __asm__("_ZN8D2Common5Units13UNITS_GetRoomEP10D2UnitStrc");
HS_CC void hs_where_coords(void* unit, HsPoint* p) __asm__("_ZN8D2Common5Units9GetCoordsEP10D2UnitStrcP8tagPOINT");
}

namespace {
// D2Common/D2UnitStrc.h and D2RoomStrc.h (32-bit layouts on every target).
constexpr size_t kUnitType = 0x00, kUnitClass = 0x04, kUnitRoomNext = 0xE8;
constexpr size_t kRoomNear = 0x00, kRoomNearCount = 0x24, kRoomUnits = 0x74;
template <typename T> T field(const void* base, size_t off) { return *(const T*)((const char*)base + off); }

// Where the player stands in the 640x480 in-game frame (calibrated on a hovered waypoint).
constexpr int kPlayerX = 320, kPlayerY = 232;
}  // namespace

extern "C" void hs_debug_where(void) {
    void* player = hs_where_player();
    if (!player) { hs_log("where: no player"); return; }
    void* room = hs_where_room(player);
    if (!room) { hs_log("where: the player has no room"); return; }
    HsPoint me;
    hs_where_coords(player, &me);
    hs_log("where: player at %d,%d", (int)me.x, (int)me.y);
    void** nearRooms = field<void**>(room, kRoomNear);
    uint32_t count = field<uint32_t>(room, kRoomNearCount);
    if (!nearRooms) count = 0;
    unsigned shown = 0;
    for (uint32_t r = 0; r <= count && shown < 80; r++) {
        void* rm = r == count ? room : nearRooms[r];
        if (!rm || (r < count && rm == room)) continue;
        for (void* u = field<void*>(rm, kRoomUnits); u && shown < 80; u = field<void*>(u, kUnitRoomNext)) {
            int type = field<int32_t>(u, kUnitType);
            if (type != 1 && type != 2 && type != 5) continue;  // monsters, objects, warps
            HsPoint p;
            hs_where_coords(u, &p);
            int dx = (int)(p.x - me.x), dy = (int)(p.y - me.y);
            if (std::abs(dx) > 60 || std::abs(dy) > 60) continue;
            int sx = kPlayerX + 16 * (dx - dy), sy = kPlayerY + 8 * (dx + dy);
            hs_log("where: %s %d at %+d,%+d -> click %d %d%s", type == 1 ? "monster" : type == 2 ? "object" : "warp",
                   field<int32_t>(u, kUnitClass), dx, dy, sx, sy,
                   sx >= 0 && sx < 640 && sy >= 0 && sy < 480 ? "" : " (off screen)");
            shown++;
        }
    }
    hs_log("where: %u units", shown);
}
