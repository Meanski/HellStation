// Allocation tracing for the EE (linked with --wrap=malloc,calloc,realloc,memalign):
// every request of 256 KiB or more is logged with its caller, and failures are
// logged, so a heap exhaustion before main() can be read back from the RAM ring.
#include "Redivivus/port/platform.h"
#include <stddef.h>

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);

static void note(const char* fn, size_t n, void* p, void* caller) {
    if (n >= (256u << 10) || !p) hs_log("%s(%u KiB) -> %p from %p", fn, (unsigned)(n >> 10), p, caller);
}
void* __wrap_malloc(size_t n) { void* p = __real_malloc(n); note("malloc", n, p, __builtin_return_address(0)); return p; }
void* __wrap_calloc(size_t c, size_t n) { void* p = __real_calloc(c, n); note("calloc", c * n, p, __builtin_return_address(0)); return p; }
void* __wrap_realloc(void* o, size_t n) { void* p = __real_realloc(o, n); note("realloc", n, p, __builtin_return_address(0)); return p; }
void* __wrap_memalign(size_t a, size_t n) { void* p = __real_memalign(a, n); note("memalign", n, p, __builtin_return_address(0)); return p; }
}
