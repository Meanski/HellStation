// D2CMP's GFXHASH_CalcDirectionIndex, with a null sprite entry read as "no data".
//
// SPRITECACHE_LoadDCC returns null when a sprite can't be loaded, and its
// callers pass that straight to GFXHASH_FindEntryByGfxData, which reads
// through it here (pCelEntry->dwCacheFlags). Entering a game on the PS2 does
// that a few times (PORTING_LOG, 2026-11-20): PCSX2 lets the null read through,
// real hardware would fault. Here a null entry reads as an entry whose
// "no data" flag (dwCacheFlags bit 0) is set, a case the function already
// handles, and the first ones are logged with the unit asking. The rest is the
// generated function unchanged. An override (cmake/port.cmake): the generated
// definition is weakened.
#include "Source/D2CMP/SRC/GfxHash.h"
#include "Source/D2CMP/SRC/CelCmp.h"
#include "Source/D2Client/D2GfxDataStrc.h"
#include "Redivivus/port/platform.h"

namespace D2CMP::SRC::GfxHash {

uint32_t __fastcall GFXHASH_CalcDirectionIndex(D2GfxDataStrc * pGfxData, uint32_t * pnDirectionDivisor, D2CelFileCacheEntryStrc * pCelEntry) {
    // HellStation: a sprite that failed to load (SPRITECACHE_LoadDCC returned
    // null) arrives here as a null entry; read it as an entry with no data.
    static D2CelFileCacheEntryStrc s_noData = [] { D2CelFileCacheEntryStrc e = {}; e.dwCacheFlags = 1; return e; }();
    if (!pCelEntry) {
        static unsigned s_logged;
        if (++s_logged <= 16)
            hs_log("gfxhash: no sprite for unit type %d class %d mode %d (direction index read as empty)",
                   (int)pGfxData->eUnitType, (int)pGfxData->nClassId, (int)pGfxData->nMode);
        pCelEntry = &s_noData;
    }
    uint32_t dwDirectionMask;
    int nDirFrameTable;
    uint32_t nDirFrameIdx = 0;
    *pnDirectionDivisor = 0;
    if (pGfxData->eUnitType != UNIT_PLAYER) {
      eD2UnitType nType;
      if (pGfxData->eUnitType != UNIT_MONSTER) {
        *pnDirectionDivisor = 1;
        nType = pGfxData->eUnitType;
        if (nType == UNIT_OBJECT) {
          return nDirFrameIdx / *pnDirectionDivisor;
        }
        if (nType != UNIT_COUNT || !(pGfxData->dwFlags & 2)) {
          if (nType == UNIT_ITEM) {
            return (uint32_t)pGfxData->nDirection / *pnDirectionDivisor;
          }
          if (nType != UNIT_MISSILE) {
            return nDirFrameIdx / *pnDirectionDivisor;
          }
        }
        if (nType == UNIT_ITEM) {
          return (uint32_t)pGfxData->nDirection / *pnDirectionDivisor;
        }
        if (pCelEntry->dwCacheFlags & 1) {
          return nDirFrameIdx / *pnDirectionDivisor;
        }

        dwDirectionMask = pCelEntry->nDirections;
        nDirFrameTable = pGfxData->nDirection;
      } else {
        if (pCelEntry->dwCacheFlags & 1) {
          return 0;
        }

        *pnDirectionDivisor = 4 + (pCelEntry->nDirections != 8 ? -3 : 0);
        dwDirectionMask = pCelEntry->nDirections;
        nDirFrameTable = pGfxData->nDirection;
      }

      nDirFrameIdx = CelCmp::CELCMP_GetDirectionFrameTableIndex(nDirFrameTable, dwDirectionMask);
    } else {
      *pnDirectionDivisor = 8;
      if (!pGfxData->nMode || pGfxData->nMode == 0x11 || pGfxData->dwRenderFlags & 2) {
        *pnDirectionDivisor = 1;
      }
      if (!(pCelEntry->dwCacheFlags & 1)) {
        dwDirectionMask = pCelEntry->nDirections;
        nDirFrameTable = pGfxData->nDirection;
        nDirFrameIdx = CelCmp::CELCMP_GetDirectionFrameTableIndex(nDirFrameTable, dwDirectionMask);
      }
    }
    if (*pnDirectionDivisor) {
      return nDirFrameIdx / *pnDirectionDivisor;
    }

    return 0;
}

}  // namespace D2CMP::SRC::GfxHash
