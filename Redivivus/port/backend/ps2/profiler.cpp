// A sampling profiler for the EE, for debugging builds only (-DHS_PROFILE).
//
// EE timer 1 interrupts at about 998 Hz (bus clock / 256, compare 577: not a
// multiple of the frame rate) and the handler records the address it arrived
// at, bucketed by 16 bytes. (A vertical-blank sampler, the first version, was
// phase-locked to the frames and credited whatever ran at fixed points of a
// frame with whole frames of time.) ps2sdk's timer library uses timer 2, the
// kernel's alarms timer 3. Every 250 presented frames the
// busiest buckets are logged and the counts start over, so each report covers
// one stretch of play. memcpy is wrapped (-Wl,--wrap=memcpy) and its callers
// counted, by calls and by bytes, reported with the samples. Symbolize
// the addresses offline:
//   tools/port/ps2.sh mips64r5900el-ps2-elf-addr2line -f -C -e build-ps2/d2.elf ADDR...
#if defined(HS_PROFILE)
#include <kernel.h>
#include <timer.h>

#include <cstdint>
#include <cstring>

#include "Redivivus/port/platform.h"

extern "C" char _ftext[], _etext[];

namespace {
constexpr unsigned kSlots = 4096;  // open addressing, power of two
struct Slot { uint32_t bucket, count; };
Slot g_slots[kSlots];
volatile uint32_t g_total, g_outside, g_dropped;

// memcpy callers: return address -> calls, bytes.
struct Caller { uint32_t ra, calls, bytes; };
constexpr unsigned kCallers = 2048;
Caller g_callers[kCallers];
uint32_t g_copyCalls, g_copyBytes;
// memset callers, the same way (-Wl,--wrap=memset).
constexpr unsigned kSetters = 512;
Caller g_setters[kSetters];
uint32_t g_setCalls, g_setBytes;

void count_caller(Caller* table, unsigned size, uint32_t ra, size_t n) {
    for (unsigned i = 0, h = (ra * 2654435761u) & (size - 1); i < 32; i++, h = (h + 1) & (size - 1)) {
        if (table[h].ra == ra || !table[h].calls) {
            table[h].ra = ra;
            table[h].calls++;
            table[h].bytes += (uint32_t)n;
            break;
        }
    }
}

void report_callers(Caller* c, bool byBytes, unsigned size = kCallers, const char* what = "memcpy") {
    for (int n = 0; n < 12; n++) {
        unsigned best = 0;
        for (unsigned i = 1; i < size; i++)
            if ((byBytes ? c[i].bytes : c[i].calls) > (byBytes ? c[best].bytes : c[best].calls)) best = i;
        if (!c[best].calls) break;
        hs_log("prof   %s from %08x: %u calls, %u KiB", what, (unsigned)c[best].ra, (unsigned)c[best].calls,
               (unsigned)(c[best].bytes >> 10));
        c[best].calls = c[best].bytes = 0;
    }
}

int on_timer(int, void*, void* addr) {
    *T1_MODE = *T1_MODE | (1u << 10);  // clear the compare flag (write 1)
    uint32_t a = (uint32_t)(uintptr_t)addr;
    g_total++;
    if (a < (uint32_t)(uintptr_t)_ftext || a >= (uint32_t)(uintptr_t)_etext) { g_outside++; return 0; }
    uint32_t b = a >> 4;
    for (unsigned i = 0, h = (b * 2654435761u) & (kSlots - 1); i < 64; i++, h = (h + 1) & (kSlots - 1)) {
        if (g_slots[h].bucket == b) { g_slots[h].count++; return 0; }
        if (!g_slots[h].count) { g_slots[h].bucket = b; g_slots[h].count = 1; return 0; }
    }
    g_dropped++;
    return 0;
}
}  // namespace

extern "C" void* __real_memcpy(void*, const void*, size_t);
extern "C" void* __wrap_memcpy(void* d, const void* s, size_t n) {
    uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
    g_copyCalls++;
    g_copyBytes += (uint32_t)n;
    count_caller(g_callers, kCallers, ra, n);
    return __real_memcpy(d, s, n);
}
extern "C" void* __real_memset(void*, int, size_t);
extern "C" void* __wrap_memset(void* d, int c, size_t n) {
    g_setCalls++;
    g_setBytes += (uint32_t)n;
    count_caller(g_setters, kSetters, (uint32_t)(uintptr_t)__builtin_return_address(0), n);
    return __real_memset(d, c, n);
}

void hs_prof_start(void) {
    *T1_MODE = 0;  // stop while setting up
    *T1_COUNT = 0;
    *T1_COMP = 577;
    AddIntcHandler2(INTC_TIM1, on_timer, 0, nullptr);
    EnableIntc(INTC_TIM1);
    // bus clock / 256, reset at compare, count, interrupt at compare, clear the flag
    *T1_MODE = Tn_MODE(2, 0, 0, 0, 1, 1, 1, 0, 1, 0);
    hs_log("profiler: sampling at ~998 Hz on timer 1, text %p-%p", (void*)_ftext, (void*)_etext);
}

void hs_prof_report(unsigned frame) {
    static Slot copy[kSlots];
    static Caller callers[kCallers];
    __real_memcpy(callers, g_callers, sizeof callers);
    memset(g_callers, 0, sizeof g_callers);
    uint32_t copyCalls = g_copyCalls, copyBytes = g_copyBytes;
    g_copyCalls = g_copyBytes = 0;
    DIntr();
    __real_memcpy(copy, g_slots, sizeof copy);
    memset(g_slots, 0, sizeof g_slots);
    uint32_t total = g_total, outside = g_outside, dropped = g_dropped;
    g_total = g_outside = g_dropped = 0;
    EIntr();
    hs_log("prof f%u: %u samples, %u outside the text, %u dropped", frame, (unsigned)total, (unsigned)outside,
           (unsigned)dropped);
    for (int n = 0; n < 24; n++) {
        unsigned best = 0;
        for (unsigned i = 1; i < kSlots; i++)
            if (copy[i].count > copy[best].count) best = i;
        if (!copy[best].count) break;
        hs_log("prof   %08x %u", (unsigned)(copy[best].bucket << 4), (unsigned)copy[best].count);
        copy[best].count = 0;
    }
    hs_log("prof memcpy: %u calls, %u KiB; by calls, then by bytes:", (unsigned)copyCalls, (unsigned)(copyBytes >> 10));
    static Caller second[kCallers];
    __real_memcpy(second, callers, sizeof second);
    report_callers(callers, false);
    report_callers(second, true);
    static Caller setters[kSetters];
    __real_memcpy(setters, g_setters, sizeof setters);
    __real_memset(g_setters, 0, sizeof g_setters);
    hs_log("prof memset: %u calls, %u KiB; by bytes:", (unsigned)g_setCalls, (unsigned)(g_setBytes >> 10));
    g_setCalls = g_setBytes = 0;
    report_callers(setters, true, kSetters, "memset");
}
#else
void hs_prof_start(void) {}
void hs_prof_report(unsigned) {}
#endif
