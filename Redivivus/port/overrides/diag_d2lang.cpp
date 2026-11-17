// TEMPORARY diagnostic override (remove once the PS2 string-table halt is
// understood): logs STRTABLE_CalculateCRC's arguments and the .tbl header bytes,
// then computes the CRC exactly as the generated body does.
#include "Source/D2Lang/StrTable/strtable.h"
#include "Source/Fog/Fog.h"
#include "Redivivus/port/platform.h"

namespace D2Lang::StrTable::strtable {
void STRTABLE_UpdateCRC(uint16_t* pwCRC, uint8_t byData);

void __stdcall STRTABLE_CalculateCRC(uint16_t* pwCRC, uint8_t* pBuffer, uint32_t nOffset, uint32_t nEndOffset) {
    hs_log("STRTABLE_CalculateCRC(buf %p, start %u, end %u) hdr %02x %02x %02x %02x | %02x %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x",
           pBuffer, nOffset, nEndOffset, pBuffer[0], pBuffer[1], pBuffer[2], pBuffer[3], pBuffer[8], pBuffer[9],
           pBuffer[10], pBuffer[11], pBuffer[12], pBuffer[13], pBuffer[14], pBuffer[15], pBuffer[16], pBuffer[17],
           pBuffer[18], pBuffer[19], pBuffer[20]);
    if (nOffset >= nEndOffset) {
        Fog::Src::ErrorManager::ERROR_UnrecoverableInternalError_Halt("", Fog::Src::ErrorManager::GetInstructionPointer(), 0x3ea);
        exit(-1);
    }
    *pwCRC = 0xffff;
    do STRTABLE_UpdateCRC(pwCRC, pBuffer[nOffset++]); while (nOffset < nEndOffset);
}
}  // namespace D2Lang::StrTable::strtable
