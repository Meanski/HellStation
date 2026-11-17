#include "Redivivus/port/win32/teb.h"
#include "Redivivus/port/platform.h"
#include <cstring>

namespace {
thread_local HsTeb* t_teb;
}

HsTeb* hs_teb_current() {
    if (!t_teb) hs_teb_attach_current_thread();
    return t_teb;
}

void hs_teb_attach_current_thread() {
    if (t_teb) return;
    HsTeb* teb = new HsTeb;
    std::memset(teb, 0, sizeof *teb);
    teb->tib.ExceptionList = (EXCEPTION_REGISTRATION_RECORD*)(intptr_t)-1;  // end of chain
    teb->tib.Self = &teb->tib;
    t_teb = teb;
}

// NtCurrentTeb() for game code. Fog walks TEB->PEB(+0x30)->Ldr(+0x0C)->
// InMemoryOrderModuleList(+0x14) to enumerate loaded modules; there are none to
// report, so the list head points at itself.
namespace {
struct FakeLdr { char pad[0x14]; void* flink; void* blink; char tail[0x20]; };
struct FakePeb { char pad[0x0C]; FakeLdr* ldr; char tail[0x200]; };
struct FakeTeb { char pad[0x30]; FakePeb* peb; char tail[0x1000]; };
FakeLdr g_ldr;
FakePeb g_peb;
FakeTeb g_teb;
}  // namespace
extern "C" struct _TEB* hs_NtCurrentTeb(void) {
    if (!g_teb.peb) {
        g_ldr.flink = g_ldr.blink = &g_ldr.flink;
        g_peb.ldr = &g_ldr;
        g_teb.peb = &g_peb;
    }
    return (struct _TEB*)&g_teb;
}
