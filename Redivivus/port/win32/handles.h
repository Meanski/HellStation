// Kernel-object model behind every Win32 HANDLE the shim hands out.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

struct HsObject {
    enum Kind : uint32_t { File, Event, Mutex, Semaphore, Thread, Find, Process, Console, Mapping };
    static constexpr uint32_t kMagic = 0x48534F42;  // 'HSOB'
    uint32_t magic = kMagic;
    Kind kind;
    std::atomic<int> refs{1};
    explicit HsObject(Kind k) : kind(k) {}
    virtual ~HsObject() { magic = 0; }
};

// Validates a HANDLE and its kind; null on mismatch (and SetLastError(ERROR_INVALID_HANDLE)).
HsObject* hs_handle_get(HANDLE h, HsObject::Kind kind);
template <class T> T* hs_handle_as(HANDLE h) { return static_cast<T*>(hs_handle_get(h, T::kKind)); }
HANDLE hs_handle_new(HsObject* o);

// Log the first call to a function that is not (fully) implemented yet.
void hs_stub_once(const char* name, const char* note);
#define HS_STUB(note) do { static bool s_logged; if (!s_logged) { s_logged = true; hs_stub_once(__func__, note); } } while (0)

// HS_TRACE=1 in the environment: log file opens and long waits.
bool hs_trace_enabled();

// A lock usable from the very first instruction of the program. The game's
// static initialisers call into the shim before the shim's own file-scope
// objects are constructed (constructor order), and on the EE a std::mutex is
// dynamically initialised (ps2sdk's PTHREAD_MUTEX_INITIALIZER is (ptr)-1, not a
// constant expression), so a std::mutex there is still zero when first locked.
// HsLock is constant-initialised and creates its mutex on first use.
#include "Redivivus/port/platform.h"
class HsLock {
    std::atomic<hs_mutex*> m_{nullptr};
    hs_mutex* get() {
        hs_mutex* m = m_.load(std::memory_order_acquire);
        if (m) return m;
        hs_mutex* fresh = hs_mutex_create();
        if (m_.compare_exchange_strong(m, fresh, std::memory_order_acq_rel)) return fresh;
        hs_mutex_destroy(fresh);
        return m;
    }
public:
    constexpr HsLock() = default;
    void lock() { hs_mutex_lock(get()); }
    void unlock() { hs_mutex_unlock(get()); }
};
struct HsLockGuard {
    HsLock& l;
    explicit HsLockGuard(HsLock& lock) : l(lock) { l.lock(); }
    ~HsLockGuard() { l.unlock(); }
    HsLockGuard(const HsLockGuard&) = delete;
    HsLockGuard& operator=(const HsLockGuard&) = delete;
};
