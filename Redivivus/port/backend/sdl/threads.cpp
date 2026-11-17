// Phase 1 backend: pthreads.
#include "Redivivus/port/platform.h"

#include <atomic>
#include <cerrno>
#include <ctime>
#include <pthread.h>

struct hs_mutex { pthread_mutex_t m; };
struct hs_cond { pthread_cond_t c; };
struct hs_thread { pthread_t t; void (*fn)(void*); void* arg; };

extern "C" {

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

static void* trampoline(void* p) {
    hs_thread* t = (hs_thread*)p;
    t->fn(t->arg);
    return nullptr;
}
hs_thread* hs_thread_start(void (*fn)(void*), void* arg, uint32_t stackSize) {
    hs_thread* t = new hs_thread{{}, fn, arg};
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stackSize) pthread_attr_setstacksize(&a, stackSize < 65536 ? 65536 : stackSize);
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
