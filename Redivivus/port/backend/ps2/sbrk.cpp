// Program break for the EE, replacing libcglue's _sbrk.
//
// The heap runs from _end to EndOfHeap() (read once, on the main thread during C
// library start-up). With HS_PS2_EXTENDED_RAM (bring-up on PCSX2's 128 MB
// "Extended RAM" / devkit memory), it then continues at 0x02000000-0x08000000,
// past the main thread's stack at the top of the first 32 MB; newlib's malloc
// copes with the discontiguous break. Retail hardware has 32 MB: build without it.
#include <kernel.h>
#include <stddef.h>
#include "Redivivus/port/platform.h"

extern "C" char _end[];

extern "C" void* _sbrk(ptrdiff_t incr) {
    static char* s_ptr = _end;
    static char* s_end;
    static bool s_extended;
    if (!s_end) s_end = (char*)EndOfHeap();
    if (incr == 0) return s_ptr;
    char* next = s_ptr + incr;
    if (next > s_end) {
#if defined(HS_PS2_EXTENDED_RAM)
        if (!s_extended) {
            s_extended = true;
            hs_log("heap: first 32 MB exhausted at %p; continuing in extended RAM", s_ptr);
            s_ptr = (char*)0x02000000;
            s_end = (char*)0x08000000;
            next = s_ptr + incr;
            if (next > s_end) return (void*)-1;
            char* ret = s_ptr;
            s_ptr = next;
            return ret;
        }
#endif
        return (void*)-1;
    }
    if (next < _end) return (void*)-1;
    char* ret = s_ptr;
    s_ptr = next;
    return ret;
}
