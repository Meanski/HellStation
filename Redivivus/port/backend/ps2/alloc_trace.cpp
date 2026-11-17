// Allocation tracing for the EE (linked with --wrap=malloc,calloc,realloc,memalign):
// every request of 256 KiB or more is logged with its caller, and failures are
// logged, so a heap exhaustion before main() can be read back from the RAM ring.
#include "Redivivus/port/platform.h"
#include <stddef.h>
#include <kernel.h>
#include <unistd.h>

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);

static void note(const char* fn, size_t n, void* p, void* caller) {
    static bool first = true;
    if (first) {
        first = false;
        hs_log("heap: first %s(%u) -> %p; sbrk(0)=%p EndOfHeap=%p", fn, (unsigned)n, p, sbrk(0), EndOfHeap());
    }
    static unsigned failures, calls;
    static unsigned long long bytes;
    calls++;
    if (p) bytes += n;
    static void* callers[64];
    static unsigned counts[64];
    static unsigned long long callerBytes[64];
    for (int i = 0; i < 64; i++) {
        if (callers[i] == caller || !callers[i]) { callers[i] = caller; counts[i]++; callerBytes[i] += n; break; }
    }
    if (!p && failures == 0)
        hs_log("heap: FIRST FAILURE %s(%u) after %u calls, %u KiB requested in total; sbrk(0)=%p",
               fn, (unsigned)n, calls, (unsigned)(bytes >> 10), sbrk(0));
    if (!p && failures == 0)
        for (int i = 0; i < 64 && callers[i]; i++)
            if (counts[i] > 100 || callerBytes[i] > (512u << 10))
                hs_log("  caller %p: %u calls, %u KiB", callers[i], counts[i], (unsigned)(callerBytes[i] >> 10));
    if (!p && ++failures > 20) return;
    if (n >= (256u << 10) || !p) hs_log("%s(%u KiB) -> %p from %p", fn, (unsigned)(n >> 10), p, caller);
}
void* __wrap_malloc(size_t n) { void* p = __real_malloc(n); note("malloc", n, p, __builtin_return_address(0)); return p; }
void* __wrap_calloc(size_t c, size_t n) { void* p = __real_calloc(c, n); note("calloc", c * n, p, __builtin_return_address(0)); return p; }
void* __wrap_realloc(void* o, size_t n) { void* p = __real_realloc(o, n); note("realloc", n, p, __builtin_return_address(0)); return p; }
void* __wrap_memalign(size_t a, size_t n) { void* p = __real_memalign(a, n); note("memalign", n, p, __builtin_return_address(0)); return p; }
}

// Every C++ throw (linked with --wrap=__cxa_throw): type and thrower, rate-limited.
#include <typeinfo>
extern "C" void __real___cxa_throw(void*, std::type_info*, void (*)(void*)) __attribute__((noreturn));
extern "C" void __wrap___cxa_throw(void* obj, std::type_info* tinfo, void (*dest)(void*)) {
    static unsigned n;
    if (++n <= 8 || n % 10000 == 0)
        hs_log("throw #%u: %s from %p", n, tinfo ? tinfo->name() : "?", __builtin_return_address(0));
    __real___cxa_throw(obj, tinfo, dest);
}

// Failing pthread mutex calls (linked with --wrap=pthread_mutex_lock/unlock).
#include <pthread.h>
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {
    int rc = __real_pthread_mutex_lock(m);
    static unsigned n;
    if (rc && ++n <= 6) hs_log("pthread_mutex_lock(%p) = %d from %p", (void*)m, rc, __builtin_return_address(0));
    return rc;
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* m) {
    int rc = __real_pthread_mutex_unlock(m);
    static unsigned n;
    if (rc && ++n <= 6) hs_log("pthread_mutex_unlock(%p) = %d from %p, self %p", (void*)m, rc, __builtin_return_address(0), (void*)pthread_self());
    return rc;
}

// Kernel semaphore lifetimes (linked with --wrap=CreateSema,--wrap=DeleteSema).
// CreateSema runs during crt0's kernel setup, before the C library exists, so
// events are only recorded here (op, id, caller) and read back over PINE.
#include <kernel.h>
extern "C" s32 __real_CreateSema(ee_sema_t*);
extern "C" s32 __real_DeleteSema(s32);
extern "C" {
__attribute__((used)) volatile unsigned hs_sema_events[3 * 256];
__attribute__((used)) volatile unsigned hs_sema_count;
}
static void sema_event(unsigned op, int id, void* caller) {
    unsigned i = hs_sema_count++;
    if (i < 256) { hs_sema_events[3 * i] = op; hs_sema_events[3 * i + 1] = (unsigned)id; hs_sema_events[3 * i + 2] = (unsigned)(uintptr_t)caller; }
}
extern "C" s32 __wrap_CreateSema(ee_sema_t* s) {
    s32 id = __real_CreateSema(s);
    sema_event(1, id, __builtin_return_address(0));
    return id;
}
extern "C" s32 __wrap_DeleteSema(s32 id) {
    sema_event(2, id, __builtin_return_address(0));
    return __real_DeleteSema(id);
}
