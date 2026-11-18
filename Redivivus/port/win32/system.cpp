// Win32 memory, module, registry and system-information calls.
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <mutex>
#include <string>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/memstats.h"

// ---- Memory -------------------------------------------------------------------------
// Heaps are the C allocator. Blocks carry their size in front so HeapSize and
// HeapReAlloc work, and are 16-byte aligned like Windows heap blocks (MIPS
// needs 8 for doubles; the game's own pools assume at least that).

namespace {
constexpr size_t kHdr = 16;
// Live bytes in Win32 heap blocks and the most there ever were.
std::atomic<size_t> g_heapLive, g_heapPeak;
void heap_count(ptrdiff_t delta) {
    size_t now = g_heapLive.fetch_add((size_t)delta, std::memory_order_relaxed) + (size_t)delta;
    size_t peak = g_heapPeak.load(std::memory_order_relaxed);
    while (now > peak && !g_heapPeak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
}
void* heap_alloc(size_t n, bool zero) {
    void* p = memalign(16, n + kHdr);
    if (n >= (256u << 10)) hs_log("HeapAlloc(%u KiB) -> %p", (unsigned)(n >> 10), p);
    if (!p) return nullptr;
    heap_count((ptrdiff_t)n);
    *(size_t*)p = n;
    if (zero) std::memset((char*)p + kHdr, 0, n);
    return (char*)p + kHdr;
}
void heap_free(void* p) {
    if (!p) return;
    heap_count(-(ptrdiff_t)*(size_t*)((char*)p - kHdr));
    std::free((char*)p - kHdr);
}
size_t heap_size(const void* p) { return *(const size_t*)((const char*)p - kHdr); }
const HANDLE kProcessHeap = (HANDLE)(uintptr_t)0x00150000;
}  // namespace

HANDLE WINAPI GetProcessHeap(void) { return kProcessHeap; }
HANDLE WINAPI HeapCreate(DWORD, SIZE_T, SIZE_T) { return kProcessHeap; }
BOOL WINAPI HeapDestroy(HANDLE) { return TRUE; }
LPVOID WINAPI HeapAlloc(HANDLE, DWORD flags, SIZE_T n) { return heap_alloc(n, flags & HEAP_ZERO_MEMORY); }
BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID p) { heap_free(p); return TRUE; }
SIZE_T WINAPI HeapSize(HANDLE, DWORD, LPCVOID p) { return p ? heap_size(p) : (SIZE_T)-1; }
LPVOID WINAPI HeapReAlloc(HANDLE, DWORD flags, LPVOID p, SIZE_T n) {
    if (!p) return heap_alloc(n, flags & HEAP_ZERO_MEMORY);
    size_t old = heap_size(p);
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n > old) return nullptr;
        heap_count((ptrdiff_t)n - (ptrdiff_t)old);
        *(size_t*)((char*)p - kHdr) = n;
        return p;
    }
    void* q = heap_alloc(n, false);
    if (!q) return nullptr;
    std::memcpy(q, p, old < n ? old : n);
    if ((flags & HEAP_ZERO_MEMORY) && n > old) std::memset((char*)q + old, 0, n - old);
    heap_free(p);
    return q;
}
BOOL WINAPI HeapValidate(HANDLE, DWORD, LPCVOID) { return TRUE; }
SIZE_T WINAPI HeapCompact(HANDLE, DWORD) { return 0x100000; }

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T n) { return heap_alloc(n, flags & GMEM_ZEROINIT); }
HGLOBAL WINAPI GlobalFree(HGLOBAL p) { heap_free(p); return nullptr; }
LPVOID WINAPI GlobalLock(HGLOBAL p) { return p; }
BOOL WINAPI GlobalUnlock(HGLOBAL) { return TRUE; }
SIZE_T WINAPI GlobalSize(HGLOBAL p) { return p ? heap_size(p) : 0; }
HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n) { return heap_alloc(n, flags & LMEM_ZEROINIT); }
HLOCAL WINAPI LocalFree(HLOCAL p) { heap_free(p); return nullptr; }

// VirtualAlloc: a reservation is zero-filled address space (64 KiB-aligned
// where Storm needs it: see below) from the platform
// (lazily backed on Phase 1); committing inside it is free; decommit returns
// pages to zero, as Windows guarantees for a later recommit.
namespace {
HsLock g_vmLock;
// Regions by how they were asked for, which separates the game's allocators:
// Storm reserves a heap and commits into it as it grows; Fog asks for whole
// committed pool blocks; MEM_RESERVE|MEM_COMMIT is everything else (mapped
// files, the odd one-off). Without virtual memory (the EE) a reservation is
// real RAM from the start, so reserved minus committed is RAM the game never used.
enum VmKind { kVmReserved, kVmCommitted, kVmBoth, kVmKinds };
const char* const kVmNames[kVmKinds] = {"reserve", "commit", "both"};
struct VmRegion { size_t size, committed; VmKind kind; };
struct VmTotals { size_t live, peak, committedLive, committedPeak; };
VmTotals g_vm[kVmKinds], g_vmAll;
std::map<uintptr_t, VmRegion>& vm_regions() { static std::map<uintptr_t, VmRegion> m; return m; }
size_t page_round(size_t n) { return (n + 4095) & ~(size_t)4095; }
void vm_count(VmTotals& t, ptrdiff_t reserved, ptrdiff_t committed) {
    t.live += reserved;
    t.committedLive += committed;
    if (t.live > t.peak) t.peak = t.live;
    if (t.committedLive > t.committedPeak) t.committedPeak = t.committedLive;
}
void vm_account(VmKind kind, ptrdiff_t reserved, ptrdiff_t committed) {
    vm_count(g_vm[kind], reserved, committed);
    vm_count(g_vmAll, reserved, committed);
}
// A commit inside a reservation raises that region's high-water mark; a later
// decommit doesn't lower it (the RAM is held either way on the EE).
void vm_commit_inside(uintptr_t addr, size_t n) {
    HsLockGuard l(g_vmLock);
    auto& m = vm_regions();
    auto it = m.upper_bound(addr);
    if (it == m.begin()) return;
    --it;
    VmRegion& r = it->second;
    if (addr >= it->first + r.size) return;
    size_t end = addr - it->first + n;
    if (end > r.size) end = r.size;
    if (end > r.committed) {
        vm_account(r.kind, 0, (ptrdiff_t)(end - r.committed));
        r.committed = end;
    }
}
}  // namespace

LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T n, DWORD type, DWORD) {
    if (addr && !(type & MEM_RESERVE)) {  // commit inside an existing reservation
        if (type & MEM_COMMIT) vm_commit_inside((uintptr_t)addr, n);
        return addr;
    }
    if (addr) HS_STUB("VirtualAlloc at a fixed address is not honoured; returning a new region");
    size_t size = page_round(n ? n : 1);
    VmKind kind = !(type & MEM_COMMIT) ? kVmReserved : (type & MEM_RESERVE) ? kVmBoth : kVmCommitted;
    // Windows' 64 KiB allocation granularity matters only to Storm, which reserves
    // first (MEM_RESERVE alone) and finds a heap's descriptor from an address's top
    // 16 bits. Fog finds pool blocks by binary search over their start addresses.
    // Without virtual memory (the EE), each 64 KiB alignment can leave up to 64 KiB
    // of the C heap unusable in front of the region, so the rest get page alignment.
    void* p = hs_vm_reserve(size, kind == kVmReserved ? 0x10000 : 0x1000);
    if (!p) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    size_t committed = (type & MEM_COMMIT) ? size : 0;
    HsLockGuard l(g_vmLock);
    vm_regions()[(uintptr_t)p] = VmRegion{size, committed, kind};
    vm_account(kind, (ptrdiff_t)size, (ptrdiff_t)committed);
    return p;
}
BOOL WINAPI VirtualFree(LPVOID p, SIZE_T n, DWORD type) {
    if (type & MEM_RELEASE) {
        size_t size = 0;
        {
            HsLockGuard l(g_vmLock);
            auto it = vm_regions().find((uintptr_t)p);
            if (it == vm_regions().end()) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
            const VmRegion& r = it->second;
            size = r.size;
            vm_account(r.kind, -(ptrdiff_t)r.size, -(ptrdiff_t)r.committed);
            vm_regions().erase(it);
        }
        hs_vm_release(p, size);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) {
        uintptr_t start = (uintptr_t)p & ~(uintptr_t)4095;
        hs_vm_decommit((void*)start, page_round(n + ((uintptr_t)p - start)));
    }
    return TRUE;
}

void hs_mem_report(unsigned frame) {
    auto kib = [](size_t n) { return (unsigned)((n + 512) >> 10); };
    hs_mem_info c = {};
    hs_mem_query(&c);
    VmTotals v[kVmKinds], all;
    size_t regions;
    {
        HsLockGuard l(g_vmLock);
        for (int k = 0; k < kVmKinds; k++) v[k] = g_vm[k];
        all = g_vmAll;
        regions = vm_regions().size();
    }
    // KiB, "now/peak". crt = the C heap (on the EE it holds the VM regions too);
    // vm = VirtualAlloc regions, reserved and committed; w32 = HeapAlloc & co.
    hs_log("mem f%u: image %u | crt taken %u/%u in-use %u limit %u | vm %u regions res %u/%u com %u/%u"
           " | %s %u/%u com %u/%u | %s %u/%u | %s %u/%u | w32 %u/%u",
           frame, kib(c.image), kib(c.heap_taken), kib(c.heap_peak), kib(c.heap_in_use), kib(c.heap_limit),
           (unsigned)regions, kib(all.live), kib(all.peak), kib(all.committedLive), kib(all.committedPeak),
           kVmNames[kVmReserved], kib(v[kVmReserved].live), kib(v[kVmReserved].peak),
           kib(v[kVmReserved].committedLive), kib(v[kVmReserved].committedPeak),
           kVmNames[kVmCommitted], kib(v[kVmCommitted].live), kib(v[kVmCommitted].peak),
           kVmNames[kVmBoth], kib(v[kVmBoth].live), kib(v[kVmBoth].peak),
           kib(g_heapLive.load()), kib(g_heapPeak.load()));
    if (frame % 500 == 0) hs_mem_report_fog();
}
BOOL WINAPI VirtualProtect(LPVOID, SIZE_T, DWORD newProt, PDWORD oldProt) {
    if (oldProt) *oldProt = PAGE_READWRITE;
    (void)newProt;
    return TRUE;
}
SIZE_T WINAPI VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n) {
    HS_STUB("reports every address as committed read-write");
    if (n < sizeof *mbi) return 0;
    std::memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = (PVOID)((uintptr_t)p & ~(uintptr_t)4095);
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 4096;
    mbi->State = MEM_COMMIT;
    mbi->Protect = PAGE_READWRITE;
    mbi->Type = MEM_PRIVATE;
    return sizeof *mbi;
}

BOOL WINAPI IsBadReadPtr(const void* p, UINT_PTR) { return p == nullptr; }
BOOL WINAPI IsBadWritePtr(LPVOID p, UINT_PTR) { return p == nullptr; }
BOOL WINAPI IsBadCodePtr(FARPROC p) { return p == nullptr; }
BOOL WINAPI IsBadStringPtrA(LPCSTR p, UINT_PTR) { return p == nullptr; }

void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS ms) {
    std::memset(ms, 0, sizeof *ms);
    ms->dwLength = sizeof *ms;
    ms->dwTotalPhys = 256u << 20;
    ms->dwAvailPhys = 192u << 20;
    ms->dwTotalPageFile = 512u << 20;
    ms->dwAvailPageFile = 384u << 20;
    ms->dwTotalVirtual = 2047u << 20;
    ms->dwAvailVirtual = 1536u << 20;
}
BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms) {
    MEMORYSTATUS s;
    GlobalMemoryStatus(&s);
    ms->dwMemoryLoad = 25;
    ms->ullTotalPhys = s.dwTotalPhys;
    ms->ullAvailPhys = s.dwAvailPhys;
    ms->ullTotalPageFile = s.dwTotalPageFile;
    ms->ullAvailPageFile = s.dwAvailPageFile;
    ms->ullTotalVirtual = s.dwTotalVirtual;
    ms->ullAvailVirtual = s.dwAvailVirtual;
    return TRUE;
}

void WINAPI GetSystemInfo(LPSYSTEM_INFO si) {
    std::memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0x7FFEFFFF;
    si->dwActiveProcessorMask = 1;
    si->dwNumberOfProcessors = 1;
    si->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
    si->dwAllocationGranularity = 65536;
    si->wProcessorLevel = 6;
}

// ---- Modules ------------------------------------------------------------------------
// One module: the game. Every other library the game loads (renderer DLLs,
// Bink, Smacker, the sound backends) is reported absent, and the game falls
// back the way it does on a machine without them.

namespace { const HMODULE kGameModule = (HMODULE)(uintptr_t)0x00400000; }

HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
    if (!name) return kGameModule;
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE WINAPI GetModuleHandleW(LPCWSTR name) { return name ? nullptr : kGameModule; }
HMODULE WINAPI LoadLibraryA(LPCSTR name) {
    hs_log("LoadLibraryA(\"%s\"): not available on this target", name ? name : "");
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
// LOAD_LIBRARY_AS_DATAFILE is how the game opens its language module (D2.LNG)
// for resources only: accelerators and error strings, both of which have
// fallbacks. There are no PE resources on a port target, so a data-file load
// yields a resource-less module and every resource lookup misses.
HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags) {
    if (flags & LOAD_LIBRARY_AS_DATAFILE) {
        hs_log("LoadLibraryExA(\"%s\", AS_DATAFILE): resource-less module", name ? name : "");
        return (HMODULE)(uintptr_t)0x00410000;
    }
    return LoadLibraryA(name);
}
BOOL WINAPI FreeLibrary(HMODULE) { return TRUE; }
FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR name) {
    hs_log("GetProcAddress(\"%s\"): not available on this target",
           ((uintptr_t)name >> 16) ? name : "<ordinal>");
    SetLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}
BOOL WINAPI DisableThreadLibraryCalls(HMODULE) { return TRUE; }

// RDTSC substitute on non-x86 targets (Redivivus/port/include/x86intrin.h).
extern "C" unsigned long long hs_rdtsc(void) { return hs_perf_counter(); }

// ---- Process ------------------------------------------------------------------------

void WINAPI ExitProcess(UINT code) {
    hs_log("ExitProcess(%u)", code);
    std::exit((int)code);
}
BOOL WINAPI TerminateProcess(HANDLE, UINT code) { ExitProcess(code); return TRUE; }
void WINAPI OutputDebugStringA(LPCSTR s) { if (s) hs_log("[debug] %s", s); }
BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
void WINAPI GetStartupInfoA(LPSTARTUPINFOA si) {
    std::memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
}
DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n) {
    (void)name;
    if (buf && n) buf[0] = 0;
    SetLastError(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    (void)f;
    HS_STUB("no structured exceptions on this target; crashes go to the signal handler");
    return nullptr;
}
UINT WINAPI SetErrorMode(UINT) { return 0; }

// ---- Versions and locale ---------------------------------------------------------------

DWORD WINAPI GetVersion(void) { return 0x0A280105; }  // Windows XP SP2 shape: 5.1, build 2600
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA v) {
    v->dwMajorVersion = 5;
    v->dwMinorVersion = 1;
    v->dwBuildNumber = 2600;
    v->dwPlatformId = VER_PLATFORM_WIN32_NT;
    std::strcpy(v->szCSDVersion, "Service Pack 2");
    if (v->dwOSVersionInfoSize >= sizeof(OSVERSIONINFOEXA)) {
        OSVERSIONINFOEXA* x = (OSVERSIONINFOEXA*)v;
        x->wServicePackMajor = 2;
        x->wProductType = VER_NT_WORKSTATION;
    }
    return TRUE;
}
UINT WINAPI GetACP(void) { return 1252; }
UINT WINAPI GetOEMCP(void) { return 437; }
LCID WINAPI GetUserDefaultLCID(void) { return 0x0409; }
LCID WINAPI GetSystemDefaultLCID(void) { return 0x0409; }
LANGID WINAPI GetUserDefaultLangID(void) { return 0x0409; }
LANGID WINAPI GetSystemDefaultLangID(void) { return 0x0409; }
LCID WINAPI GetThreadLocale(void) { return 0x0409; }
BOOL WINAPI GetComputerNameA(LPSTR buf, LPDWORD n) {
    static const char kName[] = "HELLSTATION";
    if (*n < sizeof kName) { *n = sizeof kName; return FALSE; }
    std::memcpy(buf, kName, sizeof kName);
    *n = sizeof kName - 1;
    return TRUE;
}
BOOL WINAPI GetUserNameA(LPSTR buf, LPDWORD n) {
    static const char kName[] = "player";
    if (*n < sizeof kName) { *n = sizeof kName; return FALSE; }
    std::memcpy(buf, kName, sizeof kName);
    *n = sizeof kName;
    return TRUE;
}

// Code page 1252 <-> UTF-16, which covers the game's Latin-1 text.
int WINAPI MultiByteToWideChar(UINT, DWORD, LPCCH src, int srcLen, LPWSTR dst, int dstLen) {
    if (srcLen < 0) srcLen = (int)std::strlen(src) + 1;
    if (!dstLen) return srcLen;
    int n = srcLen < dstLen ? srcLen : dstLen;
    for (int i = 0; i < n; i++) dst[i] = (WCHAR)(unsigned char)src[i];
    return n;
}
int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWCH src, int srcLen, LPSTR dst, int dstLen, LPCCH, LPBOOL used) {
    if (srcLen < 0) { srcLen = 0; while (src[srcLen]) srcLen++; srcLen++; }
    if (!dstLen) return srcLen;
    int n = srcLen < dstLen ? srcLen : dstLen;
    bool lossy = false;
    for (int i = 0; i < n; i++) {
        WCHAR c = src[i];
        if (c > 0xFF) { lossy = true; c = '?'; }
        dst[i] = (char)c;
    }
    if (used) *used = lossy;
    return n;
}

// ---- Registry -------------------------------------------------------------------------
// An in-memory registry, seeded with what Diablo II reads at startup. Values the
// game writes live for the session. (A persistent .ini can come later.)

namespace {
struct RegKey { std::string path; };
HsLock g_regLock;
struct RegValue { DWORD type; std::string data; };  // REG_SZ data excludes the terminator
std::map<std::string, RegValue>& reg_values() {
    static std::map<std::string, RegValue> v = {
        {"software\\blizzard entertainment\\diablo ii\\installpath", {REG_SZ, "C:\\Diablo II"}},
        {"software\\blizzard entertainment\\diablo ii\\save path", {REG_SZ, "C:\\Diablo II\\Save\\"}},
    };
    return v;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string key_base(HKEY k) {
    if (k == HKEY_LOCAL_MACHINE || k == HKEY_CURRENT_USER || k == HKEY_CLASSES_ROOT || k == HKEY_USERS) return "";
    return ((RegKey*)k)->path;
}
}  // namespace

LONG WINAPI RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD, REGSAM, PHKEY out) {
    std::string base = key_base(k);
    *out = (HKEY) new RegKey{lower(base.empty() ? std::string(sub ? sub : "") : base + "\\" + (sub ? sub : ""))};
    return ERROR_SUCCESS;
}
LONG WINAPI RegOpenKeyA(HKEY k, LPCSTR sub, PHKEY out) { return RegOpenKeyExA(k, sub, 0, 0, out); }
LONG WINAPI RegCreateKeyExA(HKEY k, LPCSTR sub, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD disp) {
    if (disp) *disp = REG_OPENED_EXISTING_KEY;
    return RegOpenKeyExA(k, sub, 0, 0, out);
}
LONG WINAPI RegCreateKeyA(HKEY k, LPCSTR sub, PHKEY out) { return RegOpenKeyExA(k, sub, 0, 0, out); }
LONG WINAPI RegCloseKey(HKEY k) {
    if (!key_base(k).empty()) delete (RegKey*)k;
    return ERROR_SUCCESS;
}
LONG WINAPI RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size) {
    HsLockGuard l(g_regLock);
    auto it = reg_values().find(key_base(k) + "\\" + lower(name ? name : ""));
    if (it == reg_values().end()) return ERROR_FILE_NOT_FOUND;
    const std::string& v = it->second.data;
    bool isString = it->second.type == REG_SZ || it->second.type == REG_EXPAND_SZ;
    DWORD need = (DWORD)v.size() + (isString ? 1 : 0);
    if (type) *type = it->second.type;
    if (data) {
        if (!size || *size < need) { if (size) *size = need; return ERROR_MORE_DATA; }
        std::memcpy(data, v.data(), v.size());
        if (isString) data[v.size()] = 0;
    }
    if (size) *size = need;
    return ERROR_SUCCESS;
}
LONG WINAPI RegSetValueExA(HKEY k, LPCSTR name, DWORD, DWORD type, const BYTE* data, DWORD size) {
    HsLockGuard l(g_regLock);
    std::string v((const char*)data, size);
    if ((type == REG_SZ || type == REG_EXPAND_SZ) && !v.empty() && v.back() == '\0') v.pop_back();
    reg_values()[key_base(k) + "\\" + lower(name ? name : "")] = RegValue{type, v};
    return ERROR_SUCCESS;
}
LONG WINAPI RegDeleteValueA(HKEY k, LPCSTR name) {
    HsLockGuard l(g_regLock);
    reg_values().erase(key_base(k) + "\\" + lower(name ? name : ""));
    return ERROR_SUCCESS;
}
LONG WINAPI RegFlushKey(HKEY) { return ERROR_SUCCESS; }
