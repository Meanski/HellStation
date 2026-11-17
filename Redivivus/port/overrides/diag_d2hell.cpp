// TEMPORARY diagnostic override (remove with diag_d2lang.cpp): logs each
// D2FILE_AllocAndLoadD2File request, whether the file was found and its size,
// then behaves exactly like the generated body.
#include "Source/D2Hell/D2HellRoot.h"
#include "Source/D2Hell/D2Hell.h"
#include "Source/Fog/Memory.h"
#include "Redivivus/port/platform.h"

namespace D2Hell {
void* __fastcall D2FILE_AllocAndLoadD2File(D2PoolManagerStrc* pMemory, char* szFilename, uint32_t* pSize, char* szFile, int32_t nLine) {
    struct { DWORD dwFileSizeOut; D2MPQFileStrc* pFile; } frame;
    frame.dwFileSizeOut = (DWORD)(uintptr_t)pMemory;
    BOOL bFound = CheckIfFileExists(pMemory, szFilename, &frame.pFile, 0);
    void* pBuffer = nullptr;
    DWORD dwFileSize = 0;
    if (bFound) {
        dwFileSize = D2FILE_GetFileSize(pMemory, frame.pFile, &frame.dwFileSizeOut);
        pBuffer = Fog::Memory::AllocClientMemory(dwFileSize + 800, (char*)".\\SRC\\Archive.cpp", 0xcf, 0);
        if (pBuffer) {
            D2FILE_ReadFile(pMemory, frame.pFile, pBuffer, dwFileSize);
            D2FILE_CloseFile(pMemory, frame.pFile);
            if (pSize) *pSize = dwFileSize;
        }
    }
    static unsigned n;
    if (++n <= 60 || !pBuffer)
        hs_log("load %s: found=%d size=%u buf=%p", szFilename, (int)bFound, (unsigned)dwFileSize, pBuffer);
    (void)szFile; (void)nLine;
    return pBuffer;
}
}  // namespace D2Hell

// SFileOpenArchive results, via --wrap=_ZN5Storm6Source5SFile16SFileOpenArchiveEPcPijPP13D2ArchiveStrc (calls from D2Hell into Storm).
struct D2ArchiveStrc;
extern "C" uint32_t __real__ZN5Storm6Source5SFile16SFileOpenArchiveEPcPijPP13D2ArchiveStrc(char*, int*, uint32_t, D2ArchiveStrc**);
extern "C" uint32_t __wrap__ZN5Storm6Source5SFile16SFileOpenArchiveEPcPijPP13D2ArchiveStrc(char* name, int* prio, uint32_t flags, D2ArchiveStrc** out) {
    uint32_t r = __real__ZN5Storm6Source5SFile16SFileOpenArchiveEPcPijPP13D2ArchiveStrc(name, prio, flags, out);
    hs_log("SFileOpenArchive(%s, prio %d, flags %x) = %u, archive %p", name, prio ? *prio : -1, (unsigned)flags, (unsigned)r, out ? (void*)*out : nullptr);
    return r;
}
