// PS2 backend: process entry and logging.
//
// Logs go to host:hs.log (next to the ELF; OpenPS2BIOS doesn't route EE console
// output to PCSX2's log), the EE serial port and stdout.
// Game data is read through the host: device (PCSX2's host file system, or
// ps2client over Ethernet on hardware) from a data/ folder beside the ELF; booted
// from a disc (tools/port/make-iso.py), from the disc, with saves and settings
// on the memory card in slot 1.
#include "Redivivus/port/platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <kernel.h>
#include <libmc.h>
#include <loadfile.h>
#include <sifrpc.h>
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
        // One host round trip per contiguous run of the ring, not one per byte.
        while (start < end) {
            unsigned off = start % sizeof hs_log_ring, run = end - start;
            if (run > sizeof hs_log_ring - off) run = sizeof hs_log_ring - off;
            (void)!write(fd, &hs_log_ring[off], run);
            start += run;
        }
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

bool g_fromDisc;  // set by main() from where the ELF was loaded
const char* hs_data_dir(void) { return g_fromDisc ? "cdrom0:" : "host:data"; }
const char* hs_default_root_dir(void) { return g_fromDisc ? "mc0:/HELLSTAT" : "host:hsroot"; }

}  // extern "C"

// The earliest point we control: runs before every other static constructor
// (upstream's static-init table runs at priority 200).
__attribute__((constructor(101))) static void hs_early_marker(void) {
    hs_log("HellStation (ps2): static constructors starting");
}

void hs_install_terminate_handler();

extern "C" void hs_lwlr_probe();  // lwlr_probe.cpp (temporary)
void hs_memcpy_selftest(void);    // memcpy.cpp

int hs_ps2_load_sio2man();  // pad.cpp

// Saves and settings live on the memory card in slot 1. A card that has never
// been formatted holds nothing, and nothing can be written to it, so it is
// formatted, as the PS2's browser would; a formatted card is never touched.
static void prepare_memory_card() {
    if (mcInit(MC_TYPE_MC) < 0) { hs_log("memory card: mcInit failed"); return; }
    int type = 0, free = 0, format = 0, ret = 0;
    mcGetInfo(0, 0, &type, &free, &format);
    mcSync(0, nullptr, &ret);
    hs_log("memory card: slot 1 type %d, %s, %d KiB free (result %d)", type, format ? "formatted" : "unformatted", free, ret);
    if (type == MC_TYPE_PS2 && !format) {
        mcFormat(0, 0);
        mcSync(0, nullptr, &ret);
        hs_log("memory card: formatted slot 1 (result %d)", ret);
    }
}

int main(int argc, char** argv) {
    // Booted from a disc, argv[0] is cdrom0:\HS.ELF;1 (SYSTEM.CNF's BOOT2).
    g_fromDisc = argc > 0 && argv[0] && !__builtin_strncmp(argv[0], "cdrom0:", 7);
    if (g_fromDisc) {
        SifInitRpc(0);
        int sio = hs_ps2_load_sio2man();
        int mc = SifLoadModule("rom0:MCMAN", 0, nullptr);
        int mcs = SifLoadModule("rom0:MCSERV", 0, nullptr);
        hs_log("HellStation (ps2): booted from the disc; memory card modules %d %d %d", sio, mc, mcs);
        prepare_memory_card();
    }
    hs_install_terminate_handler();
    hs_lwlr_probe();
    hs_memcpy_selftest();
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
