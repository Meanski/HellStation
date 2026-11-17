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
