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
