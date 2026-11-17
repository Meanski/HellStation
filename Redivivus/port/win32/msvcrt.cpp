// MSVC CRT functions the host C library lacks (see msvcrt.h).
#include "Redivivus/port/win32/msvcrt.h"
#include "Redivivus/port/platform.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "Redivivus/port/win32/paths.h"

namespace {
template <class U>
char* utoa_radix(U v, char* buf, int radix, bool neg) {
    char tmp[72];
    int n = 0;
    if (radix < 2 || radix > 36) radix = 10;
    do {
        unsigned d = (unsigned)(v % (U)radix);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (U)radix;
    } while (v);
    char* p = buf;
    if (neg) *p++ = '-';
    while (n) *p++ = tmp[--n];
    *p = 0;
    return buf;
}
std::string narrow16(const wchar_t* s) {
    std::string r;
    for (; *s; ++s) r += (char)(*s < 0x80 ? *s : '?');
    return r;
}
}  // namespace

extern "C" {

FILE* _wfopen(const wchar_t* name, const wchar_t* mode) {
    return std::fopen(narrow16(name).c_str(), narrow16(mode).c_str());
}

long _time32(long* t) {
    long v = (long)std::time(nullptr);
    if (t) *t = v;
    return v;
}
long long _time64(long long* t) {
    long long v = (long long)std::time(nullptr);
    if (t) *t = v;
    return v;
}

int strncpy_s(char* dst, size_t dstSize, const char* src, size_t count) {
    if (!dst || !dstSize) return EINVAL;
    if (!src) { dst[0] = 0; return EINVAL; }
    const size_t truncate = (size_t)-1;  // _TRUNCATE
    size_t n = 0;
    while (n < count && src[n]) n++;
    if (n >= dstSize) {
        if (count == truncate) {
            std::memcpy(dst, src, dstSize - 1);
            dst[dstSize - 1] = 0;
            return 80;  // STRUNCATE
        }
        dst[0] = 0;
        return ERANGE;
    }
    std::memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
int strcpy_s(char* dst, size_t dstSize, const char* src) { return strncpy_s(dst, dstSize, src, (size_t)-2); }
int strcat_s(char* dst, size_t dstSize, const char* src) {
    if (!dst || !dstSize) return EINVAL;
    size_t len = strnlen(dst, dstSize);
    if (len == dstSize) return EINVAL;
    return strncpy_s(dst + len, dstSize - len, src, (size_t)-2);
}

char* _itoa(int v, char* b, int r) {
    bool neg = r == 10 && v < 0;
    return utoa_radix<unsigned>(neg ? 0u - (unsigned)v : (unsigned)v, b, r, neg);
}
char* _ltoa(long v, char* b, int r) { return _itoa((int)v, b, r); }
char* _ultoa(unsigned long v, char* b, int r) { return utoa_radix<unsigned long>(v, b, r, false); }
char* _uitoa(unsigned v, char* b, int r) { return utoa_radix<unsigned>(v, b, r, false); }
char* _i64toa(long long v, char* b, int r) {
    bool neg = r == 10 && v < 0;
    return utoa_radix<unsigned long long>(neg ? 0ull - (unsigned long long)v : (unsigned long long)v, b, r, neg);
}
char* _ui64toa(unsigned long long v, char* b, int r) { return utoa_radix<unsigned long long>(v, b, r, false); }

char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)std::tolower((unsigned char)*p); return s; }
char* _strupr(char* s) { for (char* p = s; *p; ++p) *p = (char)std::toupper((unsigned char)*p); return s; }

char* _fullpath(char* absPath, const char* relPath, size_t maxLength) {
    // Paths stay in the game's Windows spelling; file APIs translate them.
    std::string r = relPath ? relPath : ".";
    bool absolute = r.size() > 1 && r[1] == ':';
    if (!absolute) r = std::string("C:\\Diablo II\\") + r;
    if (!absPath) return strdup(r.c_str());
    if (r.size() + 1 > maxLength) return nullptr;
    std::memcpy(absPath, r.c_str(), r.size() + 1);
    return absPath;
}

int _localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : EINVAL; }
int _localtime64_s(struct tm* out, const long long* t) {
    time_t v = (time_t)*t;
    return localtime_r(&v, out) ? 0 : EINVAL;
}

int _access(const char* path, int) {
    int isDir = 0;
    return hs_file_stat(hs_win_to_target(path, false).c_str(), &isDir, nullptr, nullptr) == 0 ? 0 : -1;
}
int _mkdir(const char* path) { return hs_file_mkdir(hs_win_to_target(path, true).c_str()); }

// MSVC rotate intrinsics (declared by the SDK; x86 gets them inline). The names
// are parenthesised because the SDK also defines them as function-like macros.
unsigned int __cdecl (_rotl)(unsigned int v, int s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
unsigned int __cdecl (_rotr)(unsigned int v, int s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

namespace { thread_local unsigned t_randSeed = 1; }
int hs_msvc_rand(void) {
    t_randSeed = t_randSeed * 214013u + 2531011u;
    return (int)((t_randSeed >> 16) & 0x7FFF);
}
void hs_msvc_srand(unsigned seed) { t_randSeed = seed; }

#undef fopen
FILE* hs_fopen(const char* path, const char* mode) {
    bool write = mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a') || std::strchr(mode, '+'));
    return std::fopen(hs_win_to_target(path, write).c_str(), mode);
}

}  // extern "C"
