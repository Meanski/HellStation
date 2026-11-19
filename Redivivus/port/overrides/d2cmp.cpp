// Overrides for D2CMP (see docs/port/overrides.md). Each replaces a generated
// function whose definition cmake/port.cmake weakens in libD2CMP.a.
#include "Source/D2CMP/Pallete.h"
#include "Source/D2CMP/SRC/LRUCache.h"
#include "Source/D2Hell/D2Hell.h"
#include "Source/Fog/Fog.h"
#include "Redivivus/port/platform.h"

#include <cstdio>

// D2CMP and D2Win each keep a 236 KiB item palette table (nine sets of 105
// rows of 256 bytes) and fill it from the same eight files into the same sets,
// 21 rows each (D2Win's D2COMP_LoadAllItemPalettes, D2CMP's
// PALETTE_InitItemPalettes); nothing else writes either. D2CMP reads only those
// rows, so it shares D2Win's table, and with nothing left referring to D2CMP's
// own (file-static in Source/D2CMP/Pallete.cpp) --gc-sections drops it.
extern uint8_t gaItemPaletteTransformData[241920];  // Source/D2Win/Src/D2Comp.cpp
// LRUCACHE_PopOldest is `static` in Source/D2CMP/SRC/LRUCache.cpp; globalized likewise.
extern D2LRUCacheNodeStrc* hs_D2CMP_LRUCACHE_PopOldest(D2LRUCacheStrc* pCache) __asm__("hs_D2CMP_LRUCACHE_PopOldest");

namespace D2CMP::Pallete {

// Generated body builds "Data\Global\Items\Palette\<name>.dat" but finds the end
// of the string from `&szInlineNameBuf + 3`, a stack-frame artefact that only
// lands on the path buffer with VC8's frame layout; under GCC the ".dat" goes
// astray and the lookup halts. Same behaviour, string built directly.
void __fastcall PALETTE_LoadFromFile(char* szPaletteName, int32_t nPaletteSet) {
    char szPath[260];
    std::snprintf(szPath, sizeof szPath, "Data\\Global\\Items\\Palette\\%s.dat", szPaletteName);
    const uint32_t nPaletteDataSize = 0x1500;
    D2MPQFileStrc* pFile = nullptr;
    if (!D2Hell::CheckIfFileExists(nullptr, szPath, &pFile, 1)) {
        Fog::Src::ErrorManager::ERROR_UnrecoverableInternalError_Halt("", Fog::Src::ErrorManager::GetInstructionPointer(), 0x39);
        exit(-1);
    }
    D2Hell::D2FILE_ReadFile(nullptr, pFile, gaItemPaletteTransformData + nPaletteSet * 0x6900, nPaletteDataSize);
    D2Hell::D2FILE_CloseFile(nullptr, pFile);
}

// As generated, on the shared table.
uint8_t* __stdcall PALETTE_GetItemPalette(int32_t nPaletteSet, int32_t nRow) {
    if (nPaletteSet < 9 && nRow < 0x15 && nPaletteSet && (nPaletteSet <= 2 || nPaletteSet > 4))
        return gaItemPaletteTransformData + (nPaletteSet * 0x69 + nRow) * 0x100;
    return nullptr;
}

}  // namespace D2CMP::Pallete

namespace D2CMP::SRC::LRUCache {

// Blizzard's bug, carried over from upstream's Redivivus/d2_patch.cpp (which
// fixes it by rewriting x86 CALLs, impossible on MIPS): unlinking the only node
// of a list takes the tail branch and writes through its null pPrev (address
// 0x0C). D2BS ships the same fix as GameCrashFix. Otherwise the original body.
void LRUCACHE_Unlink(D2LRUCacheNodeStrc* pNode, D2LRUCacheStrc* pCache) {
    if (!pCache->pLRUHead) return;
    if (!pNode) {
        Fog::Src::ErrorManager::ERROR_UnrecoverableInternalError_Halt("", Fog::Src::ErrorManager::GetInstructionPointer(), 0x172);
        exit(-1);
    }
    if (pNode == pCache->pLRUHead) {
        hs_D2CMP_LRUCACHE_PopOldest(pCache);
        return;
    }
    if (pNode == pCache->pLRUTail) {
        if (!pNode->pPrev) {  // the fix: the only node in the list
            pCache->pLRUHead = nullptr;
            pCache->pLRUTail = nullptr;
        } else {
            pCache->pLRUTail = pNode->pPrev;
            pCache->pLRUTail->pNext = nullptr;
        }
    } else {
        pNode->pPrev->pNext = pNode->pNext;
        pNode->pNext->pPrev = pNode->pPrev;
    }
    pNode->pNext = nullptr;
    pNode->pPrev = nullptr;
}

}  // namespace D2CMP::SRC::LRUCache
