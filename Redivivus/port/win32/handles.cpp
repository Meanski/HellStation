#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/platform.h"
#include <cstdlib>

HsObject* hs_handle_get(HANDLE h, HsObject::Kind kind) {
    HsObject* o = (HsObject*)h;
    if (!o || h == INVALID_HANDLE_VALUE || o->magic != HsObject::kMagic || o->kind != kind) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return o;
}

HANDLE hs_handle_new(HsObject* o) { return (HANDLE)o; }

bool hs_trace_enabled() {
    static int on = -1;
    if (on < 0) { const char* v = getenv("HS_TRACE"); on = v && *v && *v != '0'; }
    return on == 1;
}

void hs_stub_once(const char* name, const char* note) {
    hs_log("STUB %s: %s", name, note ? note : "");
}

BOOL WINAPI CloseHandle(HANDLE h) {
    HsObject* o = (HsObject*)h;
    if (!o || h == INVALID_HANDLE_VALUE || o->magic != HsObject::kMagic) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (--o->refs == 0) delete o;
    return TRUE;
}

BOOL WINAPI DuplicateHandle(HANDLE, HANDLE src, HANDLE, LPHANDLE dst, DWORD, BOOL, DWORD) {
    HsObject* o = (HsObject*)src;
    if (!o || o->magic != HsObject::kMagic) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ++o->refs;
    if (dst) *dst = src;
    return TRUE;
}

namespace { thread_local DWORD t_lastError; }
DWORD WINAPI GetLastError(void) { return t_lastError; }
void WINAPI SetLastError(DWORD e) { t_lastError = e; }
