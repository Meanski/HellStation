// POSIX backend: pthreads (glibc on Linux, ps2sdk's pthread glue on the PS2).
#include "Redivivus/port/platform.h"

#include <atomic>
#include <cerrno>
#include <ctime>
#include <pthread.h>
#if defined(__linux__)
#  include <sys/mman.h>
#else
#  include <malloc.h>
#  include <cstring>
#endif
#include <csignal>

struct hs_mutex { pthread_mutex_t m; };
struct hs_cond { pthread_cond_t c; };
struct hs_thread { pthread_t t; void (*fn)(void*); void* arg; };

extern "C" {

#if defined(__linux__)
void* hs_vm_reserve(size_t size, size_t align) {
    if (align < 4096) align = 4096;
    size_t span = size + align;
    void* p = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    uintptr_t base = (uintptr_t)p, aligned = (base + align - 1) & ~(uintptr_t)(align - 1);
    if (aligned > base) munmap(p, aligned - base);                       // trim the head
    if (base + span > aligned + size) munmap((void*)(aligned + size), base + span - aligned - size);  // and tail
    return (void*)aligned;
}
void hs_vm_decommit(void* p, size_t size) { madvise(p, size, MADV_DONTNEED); }
void hs_vm_release(void* p, size_t size) { munmap(p, size); }
#else
// No virtual memory: a reservation is real, zeroed memory up front.
void* hs_vm_reserve(size_t size, size_t align) {
    static size_t total;
    void* p = memalign(align < 16 ? 16 : align, size);
    total += p ? size : 0;
    hs_log("hs_vm_reserve(%u KiB) -> %p, total %u KiB", (unsigned)(size >> 10), p, (unsigned)(total >> 10));
    if (p) std::memset(p, 0, size);
    else hs_log("hs_vm_reserve: out of memory for %u bytes", (unsigned)size);
    return p;
}
void hs_vm_decommit(void* p, size_t size) { std::memset(p, 0, size); }
void hs_vm_release(void* p, size_t) { free(p); }
#endif

hs_mutex* hs_mutex_create(void) {
    hs_mutex* m = new hs_mutex;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
void hs_mutex_destroy(hs_mutex* m) { pthread_mutex_destroy(&m->m); delete m; }
void hs_mutex_lock(hs_mutex* m) { pthread_mutex_lock(&m->m); }
int hs_mutex_trylock(hs_mutex* m) { return pthread_mutex_trylock(&m->m) == 0; }
void hs_mutex_unlock(hs_mutex* m) { pthread_mutex_unlock(&m->m); }

hs_cond* hs_cond_create(void) {
    hs_cond* c = new hs_cond;
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
    return c;
}
void hs_cond_destroy(hs_cond* c) { pthread_cond_destroy(&c->c); delete c; }
int hs_cond_wait(hs_cond* c, hs_mutex* m, uint32_t timeoutMs) {
    if (timeoutMs == 0xFFFFFFFFu) { pthread_cond_wait(&c->c, &m->m); return 0; }
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == ETIMEDOUT ? 1 : 0;
}
void hs_cond_broadcast(hs_cond* c) { pthread_cond_broadcast(&c->c); }

// Every thread registers itself so a stall can be sampled (hs_dump_all_threads).
static pthread_mutex_t g_regLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_threads[64];
static int g_threadCount;
static thread_local uintptr_t t_stackTop;
uintptr_t hs_current_stack_top(void) { return t_stackTop; }
void hs_register_current_thread(void) {
#if defined(__linux__)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void* lo;
        size_t size;
        if (pthread_attr_getstack(&a, &lo, &size) == 0) t_stackTop = (uintptr_t)lo + size;
        pthread_attr_destroy(&a);
    }
#endif
    pthread_mutex_lock(&g_regLock);
    if (g_threadCount < 64) g_threads[g_threadCount++] = pthread_self();
    pthread_mutex_unlock(&g_regLock);
}
void hs_dump_all_threads(int sig) {
#if defined(__linux__)
    for (int i = 0; i < g_threadCount; i++)
        if (!pthread_equal(g_threads[i], pthread_self())) pthread_kill(g_threads[i], sig);
#else
    (void)sig;
#endif
}

static void* trampoline(void* p) {
    hs_register_current_thread();
    hs_thread* t = (hs_thread*)p;
    t->fn(t->arg);
    return nullptr;
}
hs_thread* hs_thread_start(void (*fn)(void*), void* arg, uint32_t stackSize) {
    hs_thread* t = new hs_thread{{}, fn, arg};
    pthread_attr_t a;
    pthread_attr_init(&a);
#if defined(__linux__)
    if (stackSize) pthread_attr_setstacksize(&a, stackSize < 65536 ? 65536 : stackSize);
#else
    // Windows stack sizes are virtual reservations the game asks for freely
    // (megabytes); on the EE a stack is real RAM, so every thread gets 1 MiB.
    (void)stackSize;
    pthread_attr_setstacksize(&a, 1024 * 1024);
#endif
    int rc = pthread_create(&t->t, &a, trampoline, t);
    pthread_attr_destroy(&a);
    if (rc != 0) { delete t; return nullptr; }
    return t;
}
void hs_thread_detach(hs_thread* t) { pthread_detach(t->t); }
void hs_thread_exit(void) { pthread_exit(nullptr); }

uint32_t hs_thread_id(void) {
    static std::atomic<uint32_t> next{1};
    thread_local uint32_t id = 0;
    if (!id) id = next++ * 4;  // Windows thread ids are multiples of 4
    return id;
}

}  // extern "C"
