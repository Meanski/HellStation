// Atomic builtins for the R5900, which has no LL/SC: GCC lowers __sync_* and
// __atomic_* to these library calls. One core, so masking interrupts around the
// operation makes it atomic against every thread switch. (Types follow GCC's
// built-in prototypes: unsigned int, not the EE's long-based uint32_t.)
#include <kernel.h>
#include <stdint.h>

namespace {
struct IrqGuard {
    int was = DIntr();
    ~IrqGuard() { if (was) EIntr(); }
};
}  // namespace

extern "C" {

unsigned int __sync_val_compare_and_swap_4(volatile void* p, unsigned int expected, unsigned int desired) {
    IrqGuard g;
    unsigned int old = *(volatile unsigned int*)p;
    if (old == expected) *(volatile unsigned int*)p = desired;
    return old;
}
bool __sync_bool_compare_and_swap_4(volatile void* p, unsigned int expected, unsigned int desired) {
    return __sync_val_compare_and_swap_4(p, expected, desired) == expected;
}
unsigned int __sync_add_and_fetch_4(volatile void* p, unsigned int v) { IrqGuard g; return *(volatile unsigned int*)p += v; }
unsigned int __sync_sub_and_fetch_4(volatile void* p, unsigned int v) { IrqGuard g; return *(volatile unsigned int*)p -= v; }
unsigned int __sync_fetch_and_add_4(volatile void* p, unsigned int v) { IrqGuard g; unsigned int o = *(volatile unsigned int*)p; *(volatile unsigned int*)p = o + v; return o; }
unsigned int __sync_fetch_and_sub_4(volatile void* p, unsigned int v) { IrqGuard g; unsigned int o = *(volatile unsigned int*)p; *(volatile unsigned int*)p = o - v; return o; }
unsigned int __sync_lock_test_and_set_4(volatile void* p, unsigned int v) { IrqGuard g; unsigned int o = *(volatile unsigned int*)p; *(volatile unsigned int*)p = v; return o; }
void __sync_synchronize(void) { __asm__ __volatile__("sync" ::: "memory"); }

unsigned int __atomic_fetch_add_4(volatile void* p, unsigned int v, int) { return __sync_fetch_and_add_4(p, v); }
unsigned int __atomic_fetch_sub_4(volatile void* p, unsigned int v, int) { return __sync_fetch_and_sub_4(p, v); }
unsigned int __atomic_exchange_4(volatile void* p, unsigned int v, int) { return __sync_lock_test_and_set_4(p, v); }
bool __atomic_compare_exchange_4(volatile void* p, void* expected, unsigned int desired, bool, int, int) {
    IrqGuard g;
    unsigned int cur = *(volatile unsigned int*)p;
    if (cur == *(unsigned int*)expected) { *(volatile unsigned int*)p = desired; return true; }
    *(unsigned int*)expected = cur;
    return false;
}
bool __atomic_compare_exchange_8(volatile void* p, void* expected, unsigned long long desired, bool, int, int) {
    IrqGuard g;
    unsigned long long cur = *(volatile unsigned long long*)p;
    if (cur == *(unsigned long long*)expected) { *(volatile unsigned long long*)p = desired; return true; }
    *(unsigned long long*)expected = cur;
    return false;
}

}  // extern "C"
