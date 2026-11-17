// Phase 1 backend: process entry and logging.
#include "Redivivus/port/platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
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

const char* hs_data_dir(void) {
    const char* d = std::getenv("D2DATA");
    return d && *d ? d : ".";
}

void hs_backtrace(const char* why) {
    void* frames[64];
    int n = backtrace(frames, 64);
    std::fprintf(stderr, "HellStation backtrace (%s):\n", why ? why : "");
    backtrace_symbols_fd(frames, n, 2);
}

int main(int argc, char** argv) {
    hs_log("HellStation (linux-sdl) starting, data dir: %s", hs_data_dir());
    return hs_game_main(argc, argv);
}
