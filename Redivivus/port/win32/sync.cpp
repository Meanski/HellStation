// Win32 synchronisation, threads and time over the platform primitives.
//
// Every blocking call goes through hs_cond_wait or hs_sleep_ms, so on targets
// whose scheduler doesn't time-slice (the PS2's EE kernel) waits always yield.
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/teb.h"

namespace {

// One lock + condition for all waitable objects: simple, correct for
// WaitForMultipleObjects, and contention is negligible for D2's few threads.
hs_mutex* g_lock() { static hs_mutex* m = hs_mutex_create(); return m; }
hs_cond* g_cond() { static hs_cond* c = hs_cond_create(); return c; }

struct Waitable : HsObject {
    explicit Waitable(Kind k) : HsObject(k) {}
    virtual bool signalled(DWORD tid) const = 0;
    virtual void acquire(DWORD tid) = 0;   // consume the signal (auto-reset, count, ownership)
};

struct HsEvent : Waitable {
    static constexpr Kind kKind = Event;
    bool manual, state;
    HsEvent(bool m, bool s) : Waitable(Event), manual(m), state(s) {}
    bool signalled(DWORD) const override { return state; }
    void acquire(DWORD) override { if (!manual) state = false; }
};

struct HsSemaphore : Waitable {
    static constexpr Kind kKind = Semaphore;
    LONG count, max;
    HsSemaphore(LONG c, LONG m) : Waitable(Semaphore), count(c), max(m) {}
    bool signalled(DWORD) const override { return count > 0; }
    void acquire(DWORD) override { --count; }
};

struct HsMutexObj : Waitable {
    static constexpr Kind kKind = Mutex;
    DWORD owner = 0;
    int depth = 0;
    HsMutexObj() : Waitable(Mutex) {}
    bool signalled(DWORD tid) const override { return depth == 0 || owner == tid; }
    void acquire(DWORD tid) override { owner = tid; ++depth; }
};

struct HsThreadObj : Waitable {
    static constexpr Kind kKind = Thread;
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID param = nullptr;
    DWORD id = 0;
    DWORD exitCode = STILL_ACTIVE;
    bool done = false;
    bool suspended = false;
    hs_thread* native = nullptr;
    HsThreadObj() : Waitable(Thread) {}
    bool signalled(DWORD) const override { return done; }
    void acquire(DWORD) override {}
};

Waitable* as_waitable(HANDLE h) {
    HsObject* o = (HsObject*)h;
    if (!o || h == INVALID_HANDLE_VALUE || o->magic != HsObject::kMagic) return nullptr;
    switch (o->kind) {
    case HsObject::Event: case HsObject::Semaphore: case HsObject::Mutex: case HsObject::Thread:
        return static_cast<Waitable*>(o);
    default:
        return nullptr;
    }
}

DWORD wait_objects(DWORD n, const HANDLE* hs, BOOL all, DWORD timeout) {
    std::vector<Waitable*> ws(n);
    for (DWORD i = 0; i < n; i++) {
        ws[i] = as_waitable(hs[i]);
        if (!ws[i]) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    }
    DWORD tid = GetCurrentThreadId();
    uint32_t start = hs_ticks_ms();
    hs_mutex_lock(g_lock());
    for (;;) {
        if (all) {
            bool ok = true;
            for (Waitable* w : ws) ok = ok && w->signalled(tid);
            if (ok) {
                for (Waitable* w : ws) w->acquire(tid);
                hs_mutex_unlock(g_lock());
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < n; i++) {
                if (ws[i]->signalled(tid)) {
                    ws[i]->acquire(tid);
                    hs_mutex_unlock(g_lock());
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        uint32_t left = 0xFFFFFFFFu;
        if (timeout != INFINITE) {
            uint32_t spent = hs_ticks_ms() - start;
            if (spent >= timeout) { hs_mutex_unlock(g_lock()); return WAIT_TIMEOUT; }
            left = timeout - spent;
        }
        uint32_t slice = left < 2000 ? left : 2000;
        if (hs_cond_wait(g_cond(), g_lock(), slice) && hs_trace_enabled() && hs_ticks_ms() - start >= 2000 &&
            (hs_ticks_ms() - start) % 10000 < 2000) {
            hs_log("wait: thread %lu blocked %us on %lu object(s), first kind %d", (unsigned long)tid,
                   (unsigned)((hs_ticks_ms() - start) / 1000), (unsigned long)n, (int)ws[0]->kind);
            static thread_local bool s_dumped;
            if (!s_dumped) { s_dumped = true; hs_backtrace("long wait"); }
        }
    }
}

void wake_all() { hs_cond_broadcast(g_cond()); }

}  // namespace

// ---- Events, semaphores, mutexes ---------------------------------------------

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) {
    return hs_handle_new(new HsEvent(manual != 0, initial != 0));
}
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR) {
    return CreateEventA(sa, manual, initial, nullptr);
}
BOOL WINAPI SetEvent(HANDLE h) {
    if (hs_trace_enabled() && getenv("HS_TRACE_WAITS")) {
        static std::atomic<int> n{0};
        if (n++ < 40) hs_log("SetEvent(%p) thread %lu", h, (unsigned long)GetCurrentThreadId());
    }
    HsEvent* e = hs_handle_as<HsEvent>(h);
    if (!e) return FALSE;
    hs_mutex_lock(g_lock()); e->state = true; wake_all(); hs_mutex_unlock(g_lock());
    return TRUE;
}
BOOL WINAPI ResetEvent(HANDLE h) {
    HsEvent* e = hs_handle_as<HsEvent>(h);
    if (!e) return FALSE;
    hs_mutex_lock(g_lock()); e->state = false; hs_mutex_unlock(g_lock());
    return TRUE;
}
BOOL WINAPI PulseEvent(HANDLE h) {
    HsEvent* e = hs_handle_as<HsEvent>(h);
    if (!e) return FALSE;
    hs_mutex_lock(g_lock()); e->state = true; wake_all(); hs_mutex_unlock(g_lock());
    hs_yield();
    hs_mutex_lock(g_lock()); e->state = false; hs_mutex_unlock(g_lock());
    return TRUE;
}

HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG max, LPCSTR) {
    return hs_handle_new(new HsSemaphore(initial, max));
}
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LPLONG prev) {
    HsSemaphore* s = hs_handle_as<HsSemaphore>(h);
    if (!s) return FALSE;
    hs_mutex_lock(g_lock());
    if (prev) *prev = s->count;
    bool ok = s->count + n <= s->max;
    if (ok) { s->count += n; wake_all(); }
    hs_mutex_unlock(g_lock());
    if (!ok) SetLastError(ERROR_TOO_MANY_POSTS);
    return ok;
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL owned, LPCSTR) {
    HsMutexObj* m = new HsMutexObj;
    if (owned) m->acquire(GetCurrentThreadId());
    return hs_handle_new(m);
}
BOOL WINAPI ReleaseMutex(HANDLE h) {
    HsMutexObj* m = hs_handle_as<HsMutexObj>(h);
    if (!m) return FALSE;
    hs_mutex_lock(g_lock());
    bool ok = m->depth > 0 && m->owner == GetCurrentThreadId();
    if (ok && --m->depth == 0) { m->owner = 0; wake_all(); }
    hs_mutex_unlock(g_lock());
    if (!ok) SetLastError(ERROR_NOT_OWNER);
    return ok;
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD timeout) {
    if (hs_trace_enabled() && getenv("HS_TRACE_WAITS")) {
        static std::atomic<int> n{0};
        if (n++ < 40) hs_log("WaitForSingleObject(%p, %lu) thread %lu", h, (unsigned long)timeout, (unsigned long)GetCurrentThreadId());
    }
    return wait_objects(1, &h, FALSE, timeout);
}
DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD timeout, BOOL) { return wait_objects(1, &h, FALSE, timeout); }
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD timeout) {
    return wait_objects(n, hs, all, timeout);
}

// ---- Critical sections -----------------------------------------------------------
// The game's CRITICAL_SECTION storage holds our recursive mutex in LockSemaphore.

// Sections the shim initialised carry this tag in SpinCount, so Delete never
// destroys a pointer it didn't create (the game deletes sections it zeroed or
// copied; on the EE that freed an unrelated kernel semaphore and stalled the
// libkernel helper thread).
constexpr ULONG_PTR kHsCsTag = 0x48534353;  // 'HSCS'

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    std::memset(cs, 0, sizeof *cs);
    cs->LockCount = -1;
    cs->LockSemaphore = (HANDLE)hs_mutex_create();
    cs->SpinCount = kHsCsTag;
}
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD) {
    InitializeCriticalSection(cs);
    return TRUE;
}
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) {
    if (cs->SpinCount != kHsCsTag) {
        static int n;
        if (cs->LockSemaphore && ++n <= 8) hs_log("DeleteCriticalSection(%p): not one of ours, left alone", (void*)cs);
        return;
    }
    if (cs->LockSemaphore) hs_mutex_destroy((hs_mutex*)cs->LockSemaphore);
    cs->LockSemaphore = nullptr;
    cs->SpinCount = 0;
}
static hs_mutex* cs_mutex(LPCRITICAL_SECTION cs) {
    // A zero-filled section that was never initialised (static storage the
    // game initialises lazily, or not at all): give it a mutex on first use.
    if (!cs->LockSemaphore) {
        static hs_mutex* guard = hs_mutex_create();
        hs_mutex_lock(guard);
        if (!cs->LockSemaphore) { cs->LockSemaphore = (HANDLE)hs_mutex_create(); cs->SpinCount = kHsCsTag; }
        hs_mutex_unlock(guard);
    }
    return (hs_mutex*)cs->LockSemaphore;
}
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs) {
    if (hs_trace_enabled()) {
        uint32_t start = hs_ticks_ms();
        bool reported = false;
        while (!hs_mutex_trylock(cs_mutex(cs))) {
            if (!reported && hs_ticks_ms() - start > 2000) {
                reported = true;
                hs_log("EnterCriticalSection(%p): thread %lu blocked >2s, owner %lu, recursion %ld", (void*)cs,
                       (unsigned long)GetCurrentThreadId(), (unsigned long)(uintptr_t)cs->OwningThread, (long)cs->RecursionCount);
                hs_backtrace("critical section wait");
            }
            hs_sleep_ms(1);
        }
        cs->OwningThread = (HANDLE)(uintptr_t)GetCurrentThreadId();
        cs->RecursionCount++;
        return;
    }
    hs_mutex_lock(cs_mutex(cs));
    cs->OwningThread = (HANDLE)(uintptr_t)GetCurrentThreadId();
    cs->RecursionCount++;
}
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    if (!hs_mutex_trylock(cs_mutex(cs))) return FALSE;
    cs->OwningThread = (HANDLE)(uintptr_t)GetCurrentThreadId();
    cs->RecursionCount++;
    return TRUE;
}
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    if (--cs->RecursionCount == 0) cs->OwningThread = nullptr;
    hs_mutex_unlock(cs_mutex(cs));
}

// ---- Threads ----------------------------------------------------------------------

namespace {
thread_local HsThreadObj* t_self;
void finish_thread(HsThreadObj* t, DWORD rc) {
    hs_mutex_lock(g_lock());
    t->exitCode = rc;
    t->done = true;
    wake_all();
    hs_mutex_unlock(g_lock());
    CloseHandle(t);
}
void thread_entry(void* p) {
    HsThreadObj* t = (HsThreadObj*)p;
    t_self = t;
    hs_teb_attach_current_thread();
    hs_teb_current()->threadId = t->id;
    finish_thread(t, t->start(t->param));
}
std::atomic<DWORD> g_nextThreadId{0x100};
}  // namespace

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
                           DWORD flags, LPDWORD id) {
    HsThreadObj* t = new HsThreadObj;
    t->start = start;
    t->param = param;
    t->id = g_nextThreadId += 4;
    t->refs = 2;  // the caller's handle and the running thread
    if (id) *id = t->id;
    hs_log("CreateThread: id %lu entry %p param %p%s", (unsigned long)t->id, (void*)start, param,
           (flags & CREATE_SUSPENDED) ? " (suspended)" : "");
    if (flags & CREATE_SUSPENDED) {
        t->suspended = true;
        return hs_handle_new(t);
    }
    t->native = hs_thread_start(thread_entry, t, (uint32_t)stack);
    if (!t->native) { delete t; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    hs_thread_detach(t->native);
    return hs_handle_new(t);
}

DWORD WINAPI ResumeThread(HANDLE h) {
    HsThreadObj* t = hs_handle_as<HsThreadObj>(h);
    if (!t) return (DWORD)-1;
    if (!t->suspended) return 0;
    t->suspended = false;
    t->native = hs_thread_start(thread_entry, t, 0);
    if (t->native) hs_thread_detach(t->native);
    return 1;
}

uintptr_t __cdecl _beginthreadex(void* sa, unsigned stack, unsigned(__stdcall* start)(void*), void* arg,
                                 unsigned flags, unsigned* id) {
    return (uintptr_t)CreateThread((LPSECURITY_ATTRIBUTES)sa, stack, (LPTHREAD_START_ROUTINE)start, arg, flags, (LPDWORD)id);
}

namespace {
struct BeginThreadThunk { void(__cdecl* fn)(void*); void* arg; };
DWORD WINAPI begin_thread_trampoline(LPVOID p) {
    BeginThreadThunk k = *(BeginThreadThunk*)p;
    delete (BeginThreadThunk*)p;
    k.fn(k.arg);
    return 0;
}
}  // namespace

uintptr_t __cdecl _beginthread(void(__cdecl* start)(void*), unsigned stack, void* arg) {
    HANDLE h = CreateThread(nullptr, stack, begin_thread_trampoline, new BeginThreadThunk{start, arg}, 0, nullptr);
    if (h) CloseHandle(h);
    return h ? (uintptr_t)h : (uintptr_t)-1;
}

BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD code) {
    HsThreadObj* t = hs_handle_as<HsThreadObj>(h);
    if (!t) return FALSE;
    hs_mutex_lock(g_lock());
    *code = t->exitCode;
    hs_mutex_unlock(g_lock());
    return TRUE;
}

void WINAPI ExitThread(DWORD code) {
    if (t_self) finish_thread(t_self, code);
    hs_thread_exit();
}

BOOL WINAPI SetThreadPriority(HANDLE, int) { return TRUE; }
int WINAPI GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
BOOL WINAPI TerminateThread(HANDLE, DWORD) { HS_STUB("threads can't be killed; ignored"); return FALSE; }

DWORD WINAPI GetCurrentThreadId(void) {
    HsTeb* teb = hs_teb_current();
    if (!teb->threadId) teb->threadId = hs_thread_id();
    return teb->threadId;
}
HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD WINAPI GetCurrentProcessId(void) { return 0x1000; }
HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

// ---- Thread-local storage ---------------------------------------------------------

namespace { std::atomic<DWORD> g_tlsNext{0}; }
DWORD WINAPI TlsAlloc(void) {
    DWORD i = g_tlsNext++;
    return i < 64 ? i : TLS_OUT_OF_INDEXES;
}
BOOL WINAPI TlsFree(DWORD) { return TRUE; }
LPVOID WINAPI TlsGetValue(DWORD i) {
    SetLastError(ERROR_SUCCESS);
    return i < 64 ? hs_teb_current()->tlsSlots[i] : nullptr;
}
BOOL WINAPI TlsSetValue(DWORD i, LPVOID v) {
    if (i >= 64) return FALSE;
    hs_teb_current()->tlsSlots[i] = v;
    return TRUE;
}

// ---- Time ---------------------------------------------------------------------------

DWORD WINAPI GetTickCount(void) { return hs_ticks_ms(); }
DWORD WINAPI timeGetTime(void) { return hs_ticks_ms(); }
MMRESULT WINAPI timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
MMRESULT WINAPI timeEndPeriod(UINT) { return TIMERR_NOERROR; }
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)hs_perf_counter(); return TRUE; }
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = (LONGLONG)hs_perf_frequency(); return TRUE; }
void WINAPI Sleep(DWORD ms) { if (ms == 0) hs_yield(); else hs_sleep_ms(ms); }
DWORD WINAPI SleepEx(DWORD ms, BOOL) { Sleep(ms); return 0; }
BOOL WINAPI SwitchToThread(void) { hs_yield(); return TRUE; }

namespace {
// The game passes SYSTEMTIMEs inside packed structs (an odd address seen in the
// UBSan build): filled locally and copied out, as a MIPS word store would fault.
void fill_systemtime(LPSYSTEMTIME out, bool local) {
    time_t now = (time_t)hs_time_now();
    struct tm tmv;
    if (local) localtime_r(&now, &tmv); else gmtime_r(&now, &tmv);
    SYSTEMTIME st;
    st.wYear = (WORD)(tmv.tm_year + 1900);
    st.wMonth = (WORD)(tmv.tm_mon + 1);
    st.wDayOfWeek = (WORD)tmv.tm_wday;
    st.wDay = (WORD)tmv.tm_mday;
    st.wHour = (WORD)tmv.tm_hour;
    st.wMinute = (WORD)tmv.tm_min;
    st.wSecond = (WORD)tmv.tm_sec;
    st.wMilliseconds = 0;
    memcpy(out, &st, sizeof st);
}
}  // namespace
void WINAPI GetLocalTime(LPSYSTEMTIME st) { fill_systemtime(st, true); }
void WINAPI GetSystemTime(LPSYSTEMTIME st) { fill_systemtime(st, false); }
void WINAPI GetSystemTimeAsFileTime(LPFILETIME ft) {
    uint64_t t = (uint64_t)(hs_time_now() + 11644473600LL) * 10000000ULL;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
