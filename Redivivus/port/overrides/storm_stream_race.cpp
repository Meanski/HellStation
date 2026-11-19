// Storm: a streamed sound ended while one of its reads is in flight.
//
// Storm's I/O thread finishes a stream read, checks the request's stream is
// still there, then calls SFILE_HandleAsyncReadError, which takes Storm's
// stream lock and writes the data through the stream's DirectSound buffer.
// SFileDdaEnd, on another thread, ends a stream under that lock: it sets the
// in-flight request's stream to null and frees the stream record. If it runs
// between the check and the lock, the handler calls through a null buffer and
// the EE jumps to address 0 (PORTING_LOG, 2026-11-20). On Windows the window
// is a few instructions; on the EE threads switch exactly at that lock wait,
// and with every long sound streamed it happened within minutes.
//
// SFILE_HandleAsyncReadError is static in SFile.cpp and reached through its
// section, so on MIPS its first two instructions are moved to a trampoline and
// replaced with a jump to the guard below: it takes the same (recursive) lock,
// returns if the stream is gone, and otherwise runs the original holding it,
// so the stream can't be ended under it. The function and the lock are
// globalized for this (cmake/port.cmake).
#include <cstdint>

#if defined(__mips__)
extern "C" void FlushCache(int operation);
extern "C" void hs_Storm_HandleAsyncReadError(void);  // address only
extern "C" CRITICAL_SECTION hs_Storm_StreamCs;

namespace {
constexpr unsigned kStreamField = 0x28;  // D2StormAsyncOpStrc::pStream
uint32_t g_trampoline[4] __attribute__((aligned(16)));

void guarded(void* op) {
    EnterCriticalSection(&hs_Storm_StreamCs);
    if (*(void**)((char*)op + kStreamField)) ((void (*)(void*))(uintptr_t)g_trampoline)(op);
    LeaveCriticalSection(&hs_Storm_StreamCs);
}

__attribute__((constructor(150))) void patch() {
    volatile uint32_t* fn = (volatile uint32_t*)(uintptr_t)&hs_Storm_HandleAsyncReadError;
    // The first two instructions (addiu sp / sd s2: nothing position-dependent),
    // then a jump back to the third.
    g_trampoline[0] = fn[0];
    g_trampoline[1] = fn[1];
    g_trampoline[2] = 0x08000000u | ((((uint32_t)(uintptr_t)(fn + 2)) >> 2) & 0x03FFFFFFu);  // j fn+8
    g_trampoline[3] = 0;                                                                      // nop
    uint32_t target = (uint32_t)(uintptr_t)&guarded;
    fn[0] = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);  // j guarded
    fn[1] = 0;                                             // nop (delay slot)
    FlushCache(0);  // write back the data cache
    FlushCache(2);  // invalidate the instruction cache
}
}  // namespace
#endif
