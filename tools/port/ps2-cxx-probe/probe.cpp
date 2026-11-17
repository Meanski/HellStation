// PS2 runtime probe: the C++/newlib/pthread features the port relies on, each
// logged to host:probe.log as it passes. Build: make; run the ELF in PCSX2.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <string>

static int g_fd = -1;
static void say(const char* s) {
    if (g_fd < 0) g_fd = open("host:probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_fd >= 0) { write(g_fd, s, strlen(s)); write(g_fd, "\n", 1); }
}
static int g_ctor101, g_ctorPlain, g_ctorOpen = -99;
static thread_local int t_inCtor;
static int g_ctorTls = -1;
__attribute__((constructor(101))) static void early() { g_ctor101 = 1; g_ctorOpen = open("host:probe-ctor.log", O_WRONLY | O_CREAT | O_TRUNC, 0644); }
__attribute__((constructor)) static void plain() { g_ctorPlain = 1; t_inCtor = 3; g_ctorTls = t_inCtor; }
static thread_local int t_counter = 5;
static std::string g_str = "static std::string ok";
static void* worker(void*) { t_counter = 9; say("thread: running"); return nullptr; }

int main() {
    say("main: entered");
    char b0[96];
    snprintf(b0, sizeof b0, "ctor101 ran=%d plain ran=%d open-in-ctor=%d tls-in-ctor=%d", g_ctor101, g_ctorPlain, g_ctorOpen, g_ctorTls);
    say(b0);
    say(g_str.c_str());
    char buf[64];
    snprintf(buf, sizeof buf, "snprintf %d %s", 42, "ok");
    say(buf);
    say(t_counter == 5 ? "thread_local ok" : "thread_local BAD");
    std::atomic<int> a{1};
    a.fetch_add(2);
    int expect = 3;
    a.compare_exchange_strong(expect, 7);
    say(a.load() == 7 ? "atomics ok" : "atomics BAD");
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_join(t, nullptr);
    say(t_counter == 5 ? "thread_local per-thread ok" : "thread_local per-thread BAD");
    say("main: done");
    for (;;) {}
}
