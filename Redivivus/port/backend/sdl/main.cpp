// Phase 1 backend: process entry and logging.
#include "Redivivus/port/platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <execinfo.h>
#include <initializer_list>
#include <csignal>
#include <ucontext.h>
#include <unistd.h>


void hs_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

void hs_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("HellStation FATAL: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::abort();
}

extern "C" const char* hs_default_root_dir(void) {
    static std::string r = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.hellstation/c";
    return r.c_str();
}

const char* hs_data_dir(void) {
    const char* d = std::getenv("D2DATA");
    return d && *d ? d : ".";
}

void hs_backtrace(const char* why) {
    void* frames[64];
    int n = backtrace(frames, 64);
    std::fprintf(stderr, "HellStation backtrace (%s):\n", why ? why : "");
    for (int i = 0; i < n; i++) std::fprintf(stderr, "HS-FRAME %08lx\n", (unsigned long)(uintptr_t)frames[i]);
}

// Fatal signals: the faulting address, the instruction pointer and the stack,
// as raw addresses for tools/port/symbolize.sh (non-PIE link, so they're
// file addresses).
// Walk the frame-pointer chain from an interrupted context. Unlike glibc's
// backtrace() this works inside signal handlers under qemu-user, and it needs
// nothing but -fno-omit-frame-pointer (the -O0 default on i386).
extern "C" char __executable_start[], etext[];
static bool in_our_code(uintptr_t a) { return a >= (uintptr_t)__executable_start && a < (uintptr_t)etext; }
extern "C" uintptr_t hs_current_stack_top(void);

static void walk_frames(void* ctx, const char* header) {
    char line[96];
    int n = std::snprintf(line, sizeof line, "\nHellStation backtrace (%s):\n", header);
    (void)!write(2, line, (size_t)n);
#if defined(__i386__)
    ucontext_t* uc = (ucontext_t*)ctx;
    uintptr_t ip = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
    uintptr_t* bp = (uintptr_t*)uc->uc_mcontext.gregs[REG_EBP];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_ESP];
    n = std::snprintf(line, sizeof line, "HS-FRAME %08lx\n", (unsigned long)ip);
    (void)!write(2, line, (size_t)n);
    if (!in_our_code(ip)) {
        // Interrupted in a library without frame pointers: scan the stack for
        // words pointing into our code (return-address candidates, may include noise).
        uintptr_t* p = (uintptr_t*)sp;
        uintptr_t top = hs_current_stack_top();
        int found = 0;
        for (int i = 0; i < 4096 && found < 24 && (!top || (uintptr_t)&p[i] + sizeof(uintptr_t) <= top); i++) {
            if (in_our_code(p[i])) {
                n = std::snprintf(line, sizeof line, "HS-FRAME %08lx\n", (unsigned long)(p[i] - 1));
                (void)!write(2, line, (size_t)n);
                found++;
            }
        }
        return;
    }
    for (int depth = 0; depth < 64 && bp; depth++) {
        uintptr_t b = (uintptr_t)bp;
        if (b < sp || b > sp + (16u << 20) || (b & 3)) break;   // must stay on this stack
        uintptr_t ret = bp[1];
        if (!ret) break;
        n = std::snprintf(line, sizeof line, "HS-FRAME %08lx\n", (unsigned long)(ret - 1));
        (void)!write(2, line, (size_t)n);
        sp = b;
        bp = (uintptr_t*)bp[0];
    }
#else
    (void)ctx;
#endif
}

extern "C" void hs_register_current_thread(void);
extern "C" void hs_dump_all_threads(int sig);

// SIGUSR1: print this thread's stack and carry on (stall sampling).
static void on_sample_signal(int, siginfo_t*, void* ctx) { walk_frames(ctx, "thread sample"); }

static void on_fatal_signal(int sig, siginfo_t* si, void* ctx) {
    if (sig == SIGINT || sig == SIGTERM) {
        hs_dump_all_threads(SIGUSR1);
        usleep(300000);
    }
    char line[96];
    int n = std::snprintf(line, sizeof line, "\nHellStation: fatal signal %d, fault-addr=%08lx", sig,
                          (unsigned long)(uintptr_t)si->si_addr);
    (void)!write(2, line, (size_t)n);
    walk_frames(ctx, sig == SIGINT || sig == SIGTERM ? "interrupted" : "fatal signal");
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, char** argv) {
    { void* warm[2]; backtrace(warm, 2); }  // load the unwinder now, not inside a signal handler
    hs_register_current_thread();
    struct sigaction ss = {};
    ss.sa_sigaction = on_sample_signal;
    ss.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &ss, nullptr);
    struct sigaction sa = {};
    sa.sa_sigaction = on_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGINT, SIGTERM}) sigaction(sig, &sa, nullptr);
    hs_log("HellStation (linux-sdl) starting, data dir: %s", hs_data_dir());
    return hs_game_main(argc, argv);
}
