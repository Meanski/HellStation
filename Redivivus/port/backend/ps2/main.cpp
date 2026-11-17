// PS2 backend: process entry and logging.
//
// Logs go to host:hs.log (next to the ELF; OpenPS2BIOS doesn't route EE console
// output to PCSX2's log), the EE serial port and stdout.
// Game data is read through the host: device (PCSX2's host file system, or
// ps2client over Ethernet on hardware) from a data/ folder beside the ELF.
#include "Redivivus/port/platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <kernel.h>
#include <sio.h>
#include <fcntl.h>
#include <unistd.h>

// Every log line also lands in this ring, so a crash before main() (file I/O
// isn't up during static construction) or a hang can be read back out of
// emulated RAM with tools/port/pine-log.py.
extern "C" {
__attribute__((used)) char hs_log_ring[65536];
__attribute__((used)) volatile unsigned hs_log_ring_pos;
}

namespace {
void ring_append(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) hs_log_ring[(hs_log_ring_pos + i) % sizeof hs_log_ring] = s[i];
    hs_log_ring_pos += (unsigned)n;
}
void emit(const char* prefix, const char* fmt, va_list ap) {
    char buf[512];
    int n = std::snprintf(buf, sizeof buf, "%s", prefix);
    std::vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    size_t len = __builtin_strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = 0;
    ring_append(buf, len + 1);
    // File I/O only works once main() is running; until then the ring holds it.
    static int fd = -1;
    static unsigned flushed = 0;
    if (fd < 0) fd = open("host:hs.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        unsigned end = hs_log_ring_pos, start = end - flushed > sizeof hs_log_ring ? end - sizeof hs_log_ring : flushed;
        for (unsigned i = start; i < end; i++) (void)!write(fd, &hs_log_ring[i % sizeof hs_log_ring], 1);
        flushed = end;
    }
    sio_puts(buf);
    std::fputs(buf, stdout);
}
}  // namespace

extern "C" {

void hs_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("", fmt, ap);
    va_end(ap);
}

void hs_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("HellStation FATAL: ", fmt, ap);
    va_end(ap);
    for (;;) SleepThread();
}

void hs_backtrace(const char* why) { hs_log("HellStation backtrace (%s): not available on the EE", why ? why : ""); }

const char* hs_data_dir(void) { return "host:data"; }
const char* hs_default_root_dir(void) { return "host:hsroot"; }

}  // extern "C"

// The earliest point we control: runs before every other static constructor
// (upstream's static-init table runs at priority 200).
__attribute__((constructor(101))) static void hs_early_marker(void) {
    hs_log("HellStation (ps2): static constructors starting");
}

void hs_install_terminate_handler();

int main(int argc, char** argv) {
    hs_install_terminate_handler();
    hs_log("HellStation (ps2) starting, data dir: %s", hs_data_dir());
    // The software (GDI) renderer is the one the port presents; it's the game's
    // windowed mode, -w. A loader may pass its own arguments instead.
    static char arg0[] = "d2.elf", argW[] = "-w";
    static char* defaultArgs[] = {arg0, argW, nullptr};
    if (argc < 2) { argc = 2; argv = defaultArgs; }
    return hs_game_main(argc, argv);
}

// abort() and std::terminate are silent on the EE: log who got here (into the
// RAM ring, readable post-mortem) before halting.
#include <exception>
#include <typeinfo>
#include <cxxabi.h>

#include <malloc.h>
extern "C" void abort(void) {
    hs_log("abort() called from %p", __builtin_return_address(0));
    struct mallinfo mi = mallinfo();
    hs_log("  heap: arena %u KiB, in use %u KiB, free %u KiB", (unsigned)(mi.arena >> 10),
           (unsigned)(mi.uordblks >> 10), (unsigned)(mi.fordblks >> 10));
    hs_fatal("abort");
}

namespace {
void on_terminate() {
    const char* what = "(no active exception)";
    if (std::type_info* t = abi::__cxa_current_exception_type()) what = t->name();
    hs_log("std::terminate: uncaught %s", what);
    try {
        if (std::current_exception()) std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        hs_log("  what(): %s", e.what());
    } catch (...) {
    }
    hs_fatal("terminate");
}
}  // namespace
// Installed from main(): std::set_terminate takes a libstdc++ mutex, and on
// Sony's kernel pthread mutexes aren't usable during static construction
// (EINVAL, pthread_self() == 0), which turned into an endless throw loop.
void hs_install_terminate_handler() { std::set_terminate(on_terminate); }
