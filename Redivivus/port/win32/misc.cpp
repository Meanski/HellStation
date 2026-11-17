// The long tail of Win32 the game links against.
//
// Real where the game depends on the answer (strings, rectangles, .ini files,
// file times, special folders, wide-char file calls, file mappings, Winsock
// start-up). Elsewhere a documented "not available" answer the game already
// handles: process/thread inspection (anti-cheat and crash reporting),
// services, certificates, clipboard, IME, and the licensed video codecs.
#include <windows.h>
#include <winsock2.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/paths.h"
#include "Redivivus/port/win32/window.h"

namespace {
std::string narrow(LPCWSTR s) {
    std::string r;
    if (s) for (; *s; ++s) r += (char)(*s < 0x100 ? *s : '?');
    return r;
}
void widen(const char* s, LPWSTR out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = (WCHAR)(unsigned char)s[i];
    if (n) out[i] = 0;
}
}  // namespace

// ---- CRT and string helpers -----------------------------------------------------------

extern "C" int _purecall(void) { hs_fatal("pure virtual function call"); }
extern "C" char* _strrev(char* s) {
    size_t n = std::strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char c = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = c; }
    return s;
}
// d2_platform.h declares CRT_Floor variadic (Ghidra's unknown-signature form);
// its two call sites pass one double. Defined under the exact symbol name.
extern "C" double hs_crt_floor(double x) __asm__("CRT_Floor");
extern "C" double hs_crt_floor(double x) { return std::floor(x); }

extern "C" void D2CRASH_ReportHalt(int nLine, unsigned nAddress) {
    hs_log("Fog halt: line %d at %08X", nLine, nAddress);
    hs_backtrace("Fog halt");
}

LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) { return std::strcpy(d, s); }
int WINAPI lstrlenA(LPCSTR s) { return s ? (int)std::strlen(s) : 0; }
int WINAPI wvsprintfA(LPSTR out, LPCSTR fmt, va_list ap) { return std::vsnprintf(out, 1024, fmt, ap); }
int WINAPIV wsprintfA(LPSTR out, LPCSTR fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = std::vsnprintf(out, 1024, fmt, ap);
    va_end(ap);
    return r;
}
DWORD WINAPI CharLowerBuffA(LPSTR s, DWORD n) {
    for (DWORD i = 0; i < n; i++) s[i] = (char)std::tolower((unsigned char)s[i]);
    return n;
}
DWORD WINAPI FormatMessageA(DWORD, LPCVOID, DWORD id, DWORD, LPSTR buf, DWORD n, va_list*) {
    return (DWORD)std::snprintf(buf, n, "Error %lu", (unsigned long)id);
}

BOOL WINAPI SetRect(LPRECT r, int l, int t, int rt, int b) { r->left = l; r->top = t; r->right = rt; r->bottom = b; return TRUE; }
BOOL WINAPI CopyRect(LPRECT d, const RECT* s) { *d = *s; return TRUE; }
BOOL WINAPI PtInRect(const RECT* r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
BOOL WINAPI IntersectRect(LPRECT d, const RECT* a, const RECT* b) {
    d->left = a->left > b->left ? a->left : b->left;
    d->top = a->top > b->top ? a->top : b->top;
    d->right = a->right < b->right ? a->right : b->right;
    d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (d->left >= d->right || d->top >= d->bottom) { *d = {0, 0, 0, 0}; return FALSE; }
    return TRUE;
}

// ---- .ini files -------------------------------------------------------------------------

DWORD WINAPI GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file) {
    auto put = [&](const std::string& v) {
        DWORD c = (DWORD)v.size() < n ? (DWORD)v.size() : (n ? n - 1 : 0);
        if (n) { std::memcpy(out, v.data(), c); out[c] = 0; }
        return c;
    };
    std::string fallback = def ? def : "";
    int fd = file ? hs_file_open(hs_win_to_target(file, false).c_str(), HS_OPEN_READ, HS_OPEN_EXISTING) : -1;
    if (fd < 0 || !section || !key) { if (fd >= 0) hs_file_close(fd); return put(fallback); }
    std::string text;
    char buf[4096];
    int64_t r;
    while ((r = hs_file_read(fd, buf, sizeof buf)) > 0) text.append(buf, (size_t)r);
    hs_file_close(fd);
    bool inSection = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') {
            inSection = _stricmp(line.substr(1, line.find(']') - 1).c_str(), section) == 0;
        } else if (inSection) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && _stricmp(line.substr(0, eq).c_str(), key) == 0) return put(line.substr(eq + 1));
        }
    }
    return put(fallback);
}
UINT WINAPI GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file) {
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof buf, file);
    return buf[0] ? (UINT)std::strtol(buf, nullptr, 10) : (UINT)def;
}

// ---- File times, folders, volumes --------------------------------------------------------

LONG WINAPI CompareFileTime(const FILETIME* a, const FILETIME* b) {
    uint64_t x = ((uint64_t)a->dwHighDateTime << 32) | a->dwLowDateTime;
    uint64_t y = ((uint64_t)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}
BOOL WINAPI FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st) {
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    time_t s = (time_t)(t / 10000000ULL - 11644473600ULL);
    struct tm tmv;
    gmtime_r(&s, &tmv);
    st->wYear = (WORD)(tmv.tm_year + 1900); st->wMonth = (WORD)(tmv.tm_mon + 1); st->wDayOfWeek = (WORD)tmv.tm_wday;
    st->wDay = (WORD)tmv.tm_mday; st->wHour = (WORD)tmv.tm_hour; st->wMinute = (WORD)tmv.tm_min;
    st->wSecond = (WORD)tmv.tm_sec; st->wMilliseconds = (WORD)((t / 10000) % 1000);
    return TRUE;
}
// Days since 1970-01-01 for a proleptic Gregorian date (no timegm on newlib).
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    int64_t secs = days_from_civil(st->wYear, st->wMonth, st->wDay) * 86400 + st->wHour * 3600 + st->wMinute * 60 + st->wSecond;
    uint64_t t = ((uint64_t)secs + 11644473600ULL) * 10000000ULL + st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}
BOOL WINAPI FileTimeToLocalFileTime(const FILETIME* in, LPFILETIME out) { *out = *in; return TRUE; }
BOOL WINAPI GetFileTime(HANDLE, LPFILETIME c, LPFILETIME a, LPFILETIME w) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (c) *c = now; if (a) *a = now; if (w) *w = now;
    return TRUE;
}
BOOL WINAPI SetFileTime(HANDLE, const FILETIME*, const FILETIME*, const FILETIME*) { return TRUE; }
DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) {
    std::memset(tz, 0, sizeof *tz);
    return TIME_ZONE_ID_UNKNOWN;
}
int WINAPI GetDateFormatA(LCID, DWORD, const SYSTEMTIME* st, LPCSTR, LPSTR out, int n) {
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    char buf[32];
    int len = std::snprintf(buf, sizeof buf, "%02u/%02u/%04u", st->wMonth, st->wDay, st->wYear);
    if (!n) return len + 1;
    std::snprintf(out, (size_t)n, "%s", buf);
    return len + 1;
}
int WINAPI GetLocaleInfoA(LCID, LCTYPE type, LPSTR out, int n) {
    const char* v = "";
    switch (type & 0xFFFF) {
    case LOCALE_SDECIMAL: v = "."; break;
    case LOCALE_STHOUSAND: v = ","; break;
    case LOCALE_SENGLANGUAGE: v = "English"; break;
    case LOCALE_SABBREVLANGNAME: v = "ENU"; break;
    case LOCALE_IDEFAULTANSICODEPAGE: v = "1252"; break;
    default: break;
    }
    int len = (int)std::strlen(v) + 1;
    if (!n) return len;
    std::snprintf(out, (size_t)n, "%s", v);
    return len;
}

DWORD WINAPI GetTempPathA(DWORD n, LPSTR buf) {
    static const char kTemp[] = "C:\\Temp\\";
    CreateDirectoryA("C:\\Temp", nullptr);
    if (n < sizeof kTemp) return sizeof kTemp;
    std::memcpy(buf, kTemp, sizeof kTemp);
    return sizeof kTemp - 1;
}
DWORD WINAPI GetTempPathW(DWORD n, LPWSTR buf) {
    char tmp[MAX_PATH];
    DWORD r = GetTempPathA(sizeof tmp, tmp);
    if (n <= r) return r + 1;
    widen(tmp, buf, n);
    return r;
}
UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out) {
    static UINT counter = 1;
    UINT id = unique ? unique : counter++;
    std::snprintf(out, MAX_PATH, "%s\\%.3s%04X.tmp", dir, prefix ? prefix : "", id & 0xFFFF);
    return id;
}
UINT WINAPI GetWindowsDirectoryA(LPSTR buf, UINT n) {
    static const char kWin[] = "C:\\Windows";
    if (n < sizeof kWin) return sizeof kWin;
    std::memcpy(buf, kWin, sizeof kWin);
    return sizeof kWin - 1;
}
HRESULT WINAPI SHGetFolderPathA(HWND, int csidl, HANDLE, DWORD, LPSTR out) {
    const char* p;
    switch (csidl & 0xFF) {
    case CSIDL_PERSONAL: p = "C:\\Users\\player\\Documents"; break;
    case CSIDL_APPDATA: p = "C:\\Users\\player\\AppData\\Roaming"; break;
    case CSIDL_LOCAL_APPDATA: p = "C:\\Users\\player\\AppData\\Local"; break;
    case CSIDL_COMMON_APPDATA: p = "C:\\ProgramData"; break;
    case CSIDL_PROGRAM_FILES: p = "C:\\Program Files"; break;
    default: p = "C:\\Users\\player"; break;
    }
    std::snprintf(out, MAX_PATH, "%s", p);
    if (csidl & CSIDL_FLAG_CREATE) {  // mkdir -p in the Windows namespace
        std::string acc;
        for (const char* c = p;; ++c) {
            if (*c == '\\' || !*c) { if (acc.size() > 2) CreateDirectoryA(acc.c_str(), nullptr); }
            if (!*c) break;
            acc += *c;
        }
    }
    return S_OK;
}
UINT WINAPI GetDriveTypeA(LPCSTR) { return DRIVE_FIXED; }
DWORD WINAPI GetLogicalDriveStringsA(DWORD n, LPSTR buf) {
    static const char kDrives[] = "C:\\\0";
    if (n < sizeof kDrives) return sizeof kDrives;
    std::memcpy(buf, kDrives, sizeof kDrives);
    return sizeof kDrives - 1;
}
BOOL WINAPI GetDiskFreeSpaceA(LPCSTR, LPDWORD spc, LPDWORD bps, LPDWORD freeClusters, LPDWORD totalClusters) {
    if (spc) *spc = 8; if (bps) *bps = 512;
    if (freeClusters) *freeClusters = 0x40000; if (totalClusters) *totalClusters = 0x80000;  // 1 GB free
    return TRUE;
}
BOOL WINAPI GetVolumeInformationA(LPCSTR, LPSTR name, DWORD nameLen, LPDWORD serial, LPDWORD maxComp, LPDWORD flags,
                                  LPSTR fs, DWORD fsLen) {
    if (name && nameLen) std::snprintf(name, nameLen, "HELLSTATION");
    if (serial) *serial = 0x48530001;
    if (maxComp) *maxComp = 255;
    if (flags) *flags = FILE_CASE_PRESERVED_NAMES;
    if (fs && fsLen) std::snprintf(fs, fsLen, "NTFS");
    return TRUE;
}
BOOL WINAPI RemoveDirectoryA(LPCSTR path) {
    if (hs_file_delete(hs_win_to_target(path, true).c_str()) != 0) { SetLastError(ERROR_DIR_NOT_EMPTY); return FALSE; }
    return TRUE;
}

// Wide-character file calls: the game's paths are Latin-1.
HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    return CreateFileA(narrow(name).c_str(), a, s, sa, d, f, t);
}
DWORD WINAPI GetFileAttributesW(LPCWSTR name) { return GetFileAttributesA(narrow(name).c_str()); }
BOOL WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(narrow(name).c_str(), sa); }
BOOL WINAPI CopyFileW(LPCWSTR a, LPCWSTR b, BOOL f) { return CopyFileA(narrow(a).c_str(), narrow(b).c_str(), f); }
HANDLE WINAPI FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    WIN32_FIND_DATAA a;
    HANDLE h = FindFirstFileA(narrow(pattern).c_str(), &a);
    if (h == INVALID_HANDLE_VALUE) return h;
    std::memset(data, 0, sizeof *data);
    data->dwFileAttributes = a.dwFileAttributes;
    data->nFileSizeLow = a.nFileSizeLow;
    data->nFileSizeHigh = a.nFileSizeHigh;
    widen(a.cFileName, data->cFileName, MAX_PATH);
    HS_STUB("FindNextFileW is not provided; W enumeration returns the first match only");
    return h;
}

// File mappings: a read-only snapshot of the file's bytes. The game maps files
// to read them; a writable view is logged, because writes would not persist.
namespace {
struct HsMapping : HsObject {
    static constexpr Kind kKind = Mapping;
    HANDLE file;
    explicit HsMapping(HANDLE f) : HsObject(Mapping), file(f) {}
    ~HsMapping() override { CloseHandle(file); }
};
}  // namespace
HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD protect, DWORD, DWORD, LPCSTR) {
    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY)) HS_STUB("writable file mapping is a snapshot; writes don't reach the file");
    HANDLE dup = nullptr;
    DuplicateHandle(nullptr, file, nullptr, &dup, 0, FALSE, 0);
    return hs_handle_new(new HsMapping(dup));
}
LPVOID WINAPI MapViewOfFile(HANDLE mapping, DWORD, DWORD offHi, DWORD offLo, SIZE_T n) {
    HsMapping* m = hs_handle_as<HsMapping>(mapping);
    if (!m) return nullptr;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(m->file, &size)) return nullptr;
    int64_t off = ((int64_t)offHi << 32) | offLo;
    if (!n) n = (SIZE_T)(size.QuadPart - off);
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LARGE_INTEGER pos;
    pos.QuadPart = off;
    DWORD got = 0;
    SetFilePointerEx(m->file, pos, nullptr, FILE_BEGIN);
    ReadFile(m->file, p, (DWORD)n, &got, nullptr);
    return p;
}
BOOL WINAPI UnmapViewOfFile(LPCVOID p) { return VirtualFree((LPVOID)p, 0, MEM_RELEASE); }

// ---- Modules and resources ---------------------------------------------------------------
// There are no PE resources on a port target; every lookup misses.

HMODULE WINAPI LoadLibraryW(LPCWSTR name) { return LoadLibraryA(narrow(name).c_str()); }
HRSRC WINAPI FindResourceA(HMODULE, LPCSTR, LPCSTR) { return nullptr; }
HGLOBAL WINAPI LoadResource(HMODULE, HRSRC) { return nullptr; }
LPVOID WINAPI LockResource(HGLOBAL) { return nullptr; }
DWORD WINAPI SizeofResource(HMODULE, HRSRC) { return 0; }
BOOL WINAPI FreeResource(HGLOBAL) { return TRUE; }
int WINAPI LoadStringA(HINSTANCE, UINT, LPSTR buf, int n) { if (buf && n) buf[0] = 0; return 0; }
HACCEL WINAPI LoadAcceleratorsA(HINSTANCE, LPCSTR) { return nullptr; }
int WINAPI TranslateAcceleratorA(HWND, HACCEL, LPMSG) { return 0; }
HANDLE WINAPI LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return nullptr; }
BOOL WINAPI VerQueryValueA(LPCVOID, LPCSTR, LPVOID*, PUINT) { return FALSE; }
BOOL WINAPI FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }

// ---- Processes, threads, toolhelp ---------------------------------------------------------
// Inspection of other processes and threads: used by crash reporting and
// cheat detection. Nothing to inspect here; every call reports failure.

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD) { SetLastError(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }
BOOL WINAPI Process32First(HANDLE, LPPROCESSENTRY32) { return FALSE; }
BOOL WINAPI Process32Next(HANDLE, LPPROCESSENTRY32) { return FALSE; }
BOOL WINAPI Process32FirstW(HANDLE, LPPROCESSENTRY32W) { return FALSE; }
BOOL WINAPI Process32NextW(HANDLE, LPPROCESSENTRY32W) { return FALSE; }
BOOL WINAPI Thread32First(HANDLE, LPTHREADENTRY32) { return FALSE; }
BOOL WINAPI Thread32Next(HANDLE, LPTHREADENTRY32) { return FALSE; }
BOOL WINAPI Module32FirstW(HANDLE, LPMODULEENTRY32W) { return FALSE; }
BOOL WINAPI Module32NextW(HANDLE, LPMODULEENTRY32W) { return FALSE; }
BOOL WINAPI K32GetModuleInformation(HANDLE, HMODULE, LPMODULEINFO, DWORD) { return FALSE; }
DWORD WINAPI K32GetModuleFileNameExA(HANDLE, HMODULE m, LPSTR buf, DWORD n) { return GetModuleFileNameA(m, buf, n); }
HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
DWORD WINAPI GetProcessId(HANDLE) { return GetCurrentProcessId(); }
BOOL WINAPI GetExitCodeProcess(HANDLE, LPDWORD code) { *code = 0; return TRUE; }
BOOL WINAPI ReadProcessMemory(HANDLE, LPCVOID src, LPVOID dst, SIZE_T n, SIZE_T* got) {
    std::memcpy(dst, src, n);
    if (got) *got = n;
    return TRUE;
}
BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR,
                           LPSTARTUPINFOA, LPPROCESS_INFORMATION) {
    hs_log("CreateProcessA(%s, %s): not available", app ? app : "", cmd ? cmd : "");
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
BOOL WINAPI CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                           LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
HINSTANCE WINAPI ShellExecuteA(HWND, LPCSTR op, LPCSTR file, LPCSTR, LPCSTR, INT) {
    hs_log("ShellExecuteA(%s, %s): not available", op ? op : "", file ? file : "");
    return (HINSTANCE)(uintptr_t)SE_ERR_NOASSOC;
}
HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
DWORD WINAPI SuspendThread(HANDLE) { return (DWORD)-1; }
BOOL WINAPI GetThreadContext(HANDLE, LPCONTEXT) { return FALSE; }
BOOL WINAPI SetThreadContext(HANDLE, const CONTEXT*) { return FALSE; }
void WINAPI RtlCaptureContext(PCONTEXT c) { std::memset(c, 0, sizeof *c); }
PVOID WINAPI AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    HS_STUB("no structured exceptions on this target");
    return (PVOID)(uintptr_t)1;
}
void WINAPI RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR*) {
    hs_backtrace("RaiseException");
    hs_fatal("RaiseException(%08lX)", (unsigned long)code);
}
// ExitThread lives in sync.cpp, next to the thread objects it completes.

// ---- Services, security, certificates -----------------------------------------------------

SC_HANDLE WINAPI OpenSCManagerA(LPCSTR, LPCSTR, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
SC_HANDLE WINAPI OpenServiceA(SC_HANDLE, LPCSTR, DWORD) { return nullptr; }
BOOL WINAPI CloseServiceHandle(SC_HANDLE) { return TRUE; }
SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerA(LPCSTR, LPHANDLER_FUNCTION) { return 0; }
BOOL WINAPI SetServiceStatus(SERVICE_STATUS_HANDLE, LPSERVICE_STATUS) { return FALSE; }
BOOL WINAPI StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA*) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }
BOOL WINAPI AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY, BYTE, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, PSID* sid) {
    *sid = (PSID) new char[16]();
    return TRUE;
}
PVOID WINAPI FreeSid(PSID sid) { delete[] (char*)sid; return nullptr; }
BOOL WINAPI CheckTokenMembership(HANDLE, PSID, PBOOL member) { *member = FALSE; return TRUE; }
// Authenticode: the game checks signatures of its own files for Battle.net.
// Nothing here is signed; report "no signature", which single player ignores.
LONG WINAPI WinVerifyTrust(HWND, GUID*, LPVOID) { return TRUST_E_NOSIGNATURE; }
BOOL WINAPI CryptQueryObject(DWORD, const void*, DWORD, DWORD, DWORD, DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**) { return FALSE; }
BOOL WINAPI CryptMsgClose(HCRYPTMSG) { return TRUE; }
BOOL WINAPI CryptMsgGetParam(HCRYPTMSG, DWORD, DWORD, void*, DWORD*) { return FALSE; }
BOOL WINAPI CertCloseStore(HCERTSTORE, DWORD) { return TRUE; }
PCCERT_CONTEXT WINAPI CertFindCertificateInStore(HCERTSTORE, DWORD, DWORD, DWORD, const void*, PCCERT_CONTEXT) { return nullptr; }
BOOL WINAPI CertFreeCertificateContext(PCCERT_CONTEXT) { return TRUE; }
DWORD WINAPI CertGetNameStringW(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR out, DWORD n) { if (out && n) out[0] = 0; return 1; }
void WINAPI CoTaskMemFree(LPVOID p) { std::free(p); }

// ---- Desktop, display, clipboard, IME ------------------------------------------------------

BOOL WINAPI SystemParametersInfoA(UINT, UINT, PVOID, UINT) { return FALSE; }
LONG WINAPI ChangeDisplaySettingsA(DEVMODEA*, DWORD) { return DISP_CHANGE_SUCCESSFUL; }
HMONITOR WINAPI MonitorFromWindow(HWND, DWORD) { return (HMONITOR)(uintptr_t)0x10040; }
BOOL WINAPI GetMonitorInfoA(HMONITOR, LPMONITORINFO mi) {
    mi->rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}
BOOL WINAPI GetWindowPlacement(HWND h, WINDOWPLACEMENT* wp) {
    wp->flags = 0;
    wp->showCmd = SW_SHOWNORMAL;
    wp->ptMinPosition = {-1, -1};
    wp->ptMaxPosition = {-1, -1};
    GetWindowRect(h, &wp->rcNormalPosition);
    return TRUE;
}
BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT) { return TRUE; }
UINT_PTR WINAPI SHAppBarMessage(DWORD, PAPPBARDATA) { return 0; }
BOOL WINAPI OpenClipboard(HWND) { return FALSE; }
BOOL WINAPI CloseClipboard(void) { return TRUE; }
BOOL WINAPI EmptyClipboard(void) { return FALSE; }
HANDLE WINAPI GetClipboardData(UINT) { return nullptr; }
HANDLE WINAPI SetClipboardData(UINT, HANDLE) { return nullptr; }
BOOL WINAPI IsClipboardFormatAvailable(UINT) { return FALSE; }

HIMC WINAPI ImmGetContext(HWND) { return nullptr; }
BOOL WINAPI ImmReleaseContext(HWND, HIMC) { return TRUE; }
BOOL WINAPI ImmIsIME(HKL) { return FALSE; }
BOOL WINAPI ImmGetOpenStatus(HIMC) { return FALSE; }
BOOL WINAPI ImmSetOpenStatus(HIMC, BOOL) { return FALSE; }
BOOL WINAPI ImmGetConversionStatus(HIMC, LPDWORD a, LPDWORD b) { if (a) *a = 0; if (b) *b = 0; return FALSE; }
BOOL WINAPI ImmSetConversionStatus(HIMC, DWORD, DWORD) { return FALSE; }
LONG WINAPI ImmGetCompositionStringA(HIMC, DWORD, LPVOID, DWORD) { return 0; }
DWORD WINAPI ImmGetCandidateListA(HIMC, DWORD, LPCANDIDATELIST, DWORD) { return 0; }
DWORD WINAPI ImmGetCandidateListCountA(HIMC, LPDWORD n) { if (n) *n = 0; return 0; }
BOOL WINAPI ImmSimulateHotKey(HWND, DWORD) { return FALSE; }

// ---- Winsock: single player only; the network stays down --------------------------------------

namespace { thread_local int t_wsaError; }
int WSAAPI WSAStartup(WORD version, LPWSADATA data) {
    std::memset(data, 0, sizeof *data);
    data->wVersion = version;
    data->wHighVersion = MAKEWORD(2, 2);
    std::snprintf(data->szDescription, sizeof data->szDescription, "HellStation (no network)");
    return 0;
}
int WSAAPI WSACleanup(void) { return 0; }
int WSAAPI WSAGetLastError(void) { return t_wsaError; }
void WSAAPI WSASetLastError(int e) { t_wsaError = e; }
int WSAAPI closesocket(SOCKET) { return 0; }
int WSAAPI ioctlsocket(SOCKET, LONG, u_long*) { t_wsaError = WSAENETDOWN; return SOCKET_ERROR; }
int WSAAPI __WSAFDIsSet(SOCKET s, fd_set* set) {
    for (u_int i = 0; i < set->fd_count; i++) if (set->fd_array[i] == s) return 1;
    return 0;
}

// ---- Licensed codecs: absent, so cinematics and JPEG screenshots are skipped -------------------

extern "C" {
void* WINAPI BinkOpen(void*, uint32_t) { return nullptr; }
void WINAPI BinkClose(void*) {}
int32_t WINAPI BinkDoFrame(void*) { return 0; }
void WINAPI BinkNextFrame(void*) {}
int32_t WINAPI BinkWait(void*) { return 0; }
int32_t WINAPI BinkCopyToBuffer(void*, void*, int32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t WINAPI BinkSetSoundSystem(void* (WINAPI*)(uint32_t), uint32_t) { return 0; }
void* WINAPI BinkOpenDirectSound(uint32_t) { return nullptr; }
uint32_t WINAPI BinkDDSurfaceType(void*) { return 0; }
void* WINAPI SmackOpen(const char*, uint32_t, uint32_t) { return nullptr; }
void WINAPI SmackClose(void*) {}
int32_t WINAPI SmackDoFrame(void*) { return 0; }
void WINAPI SmackNextFrame(void*) {}
int32_t WINAPI SmackWait(void*) { return 0; }
void WINAPI SmackToBuffer(void*, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t) {}
int32_t WINAPI ijlInit(void*) { return -1; }
int32_t WINAPI ijlFree(void*) { return 0; }
int32_t WINAPI ijlWrite(void*, int32_t) { return -1; }
}

// ---- DirectDraw / DirectSound: not present; the game uses GDI and (for now) no sound ------------

extern "C" HRESULT WINAPI DirectDrawCreate(GUID*, struct IDirectDraw1**, struct IUnknown*) { return E_FAIL; }
namespace _Wrappers {
extern "C" HRESULT WINAPI DirectDrawEnumerateA(BOOL(WINAPI*)(GUID*, char*, char*, void*), void*) { return 0; }
}
extern "C" int32_t WINAPI DirectSoundCreate(const GUID*, struct IDirectSound**, struct IUnknown*) {
    HS_STUB("no audio backend yet: the game runs silent");
    return (int32_t)0x88780078;  // DSERR_NODRIVER
}
extern "C" int32_t WINAPI DirectSoundEnumerateA(BOOL(WINAPI*)(int, char*, char*, DWORD), void*) { return 0; }

// ---- Odds and ends ------------------------------------------------------------------------------

HANDLE WINAPI OpenEventA(DWORD, BOOL, LPCSTR name) {
    (void)name;
    SetLastError(ERROR_FILE_NOT_FOUND);  // no named objects: nothing else is running
    return nullptr;
}
HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR, DWORD) {
    if (existing) return existing;
    if (file != INVALID_HANDLE_VALUE) return nullptr;
    return CreateEventA(nullptr, TRUE, FALSE, nullptr);  // a port nothing ever completes on
}
BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, LPDWORD n, PULONG_PTR key, LPOVERLAPPED* ov, DWORD timeout) {
    WaitForSingleObject(port, timeout);
    if (n) *n = 0;
    if (key) *key = 0;
    if (ov) *ov = nullptr;
    SetLastError(WAIT_TIMEOUT);
    return FALSE;
}
DWORD WINAPI GdiSetBatchLimit(DWORD) { return 1; }
COLORREF WINAPI GetPixel(HDC, int, int) { return 0; }
BOOL WINAPI GetCharWidthA(HDC, UINT first, UINT last, LPINT w) { for (UINT i = first; i <= last; i++) w[i - first] = 8; return TRUE; }
LONG WINAPI RegDeleteKeyA(HKEY, LPCSTR) { return ERROR_SUCCESS; }
LONG WINAPI RegEnumValueA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_NO_MORE_ITEMS; }

// ---- Winsock name and socket calls -------------------------------------------------------------
// These share names with the host C library's BSD sockets, which have a
// different calling convention and different structures. Defining them here
// keeps the game's (stdcall, Winsock-layout) calls off the host's versions.
// Single player has no network: names resolve to loopback, sockets fail.

namespace {
int net_down() { t_wsaError = WSAENETDOWN; return SOCKET_ERROR; }
}  // namespace
int WSAAPI gethostname(char* name, int n) { std::snprintf(name, (size_t)n, "hellstation"); return 0; }
struct hostent* WSAAPI gethostbyname(const char*) {
    static u_long loopback = 0x0100007F;  // 127.0.0.1 in network order
    static char* addrs[] = {(char*)&loopback, nullptr};
    static char* aliases[] = {nullptr};
    static char name[] = "hellstation";
    static struct hostent h = {name, aliases, AF_INET, 4, addrs};
    return &h;
}
u_short WSAAPI htons(u_short v) { return (u_short)((v << 8) | (v >> 8)); }
u_short WSAAPI ntohs(u_short v) { return htons(v); }
u_long WSAAPI htonl(u_long v) { return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }
u_long WSAAPI ntohl(u_long v) { return htonl(v); }
unsigned long WSAAPI inet_addr(const char* s) {
    unsigned a, b, c, d;
    if (!s || std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return INADDR_NONE;
    return (unsigned long)(a | (b << 8) | (c << 16) | (d << 24));
}
char* WSAAPI inet_ntoa(struct in_addr in) {
    static char buf[16];
    unsigned long v;
    std::memcpy(&v, &in, 4);
    std::snprintf(buf, sizeof buf, "%lu.%lu.%lu.%lu", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
    return buf;
}
SOCKET WSAAPI socket(int, int, int) { t_wsaError = WSAENETDOWN; return INVALID_SOCKET; }
int WSAAPI bind(SOCKET, const struct sockaddr*, int) { return net_down(); }
int WSAAPI connect(SOCKET, const struct sockaddr*, int) { return net_down(); }
int WSAAPI listen(SOCKET, int) { return net_down(); }
SOCKET WSAAPI accept(SOCKET, struct sockaddr*, int*) { t_wsaError = WSAENETDOWN; return INVALID_SOCKET; }
int WSAAPI send(SOCKET, const char*, int, int) { return net_down(); }
int WSAAPI recv(SOCKET, char*, int, int) { return net_down(); }
int WSAAPI setsockopt(SOCKET, int, int, const char*, int) { return net_down(); }
int WSAAPI getsockname(SOCKET, struct sockaddr*, int*) { return net_down(); }
int WSAAPI getpeername(SOCKET, struct sockaddr*, int*) { return net_down(); }
int WSAAPI select(int, fd_set*, fd_set*, fd_set*, const TIMEVAL* timeout) {
    if (timeout) hs_sleep_ms((uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000));
    return 0;
}
