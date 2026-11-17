// Per-thread Windows state: TLS slots and the thread id. (Port builds take the
// generic SEH path, so nothing reads a TEB through a segment register.)
#pragma once
#include <windows.h>

struct HsTeb {
    NT_TIB tib;                 // ExceptionList, StackBase/Limit, Self at +0x18
    void*  reserved[64];        // read by nothing we know of; room for the 0x2c TLS pointer etc.
    void*  tlsSlots[64];        // TlsAlloc/TlsGetValue storage for this thread
    DWORD  threadId;
};

void hs_teb_attach_current_thread();
HsTeb* hs_teb_current();
