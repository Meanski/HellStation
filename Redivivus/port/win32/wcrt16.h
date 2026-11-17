// 16-bit wide-character CRT for game translation units.
//
// Game TUs build with -fshort-wchar (MSVC's 2-byte wchar_t); the host C
// library's wcs* functions assume 4 bytes. These macros route the game's wide
// CRT calls to implementations in wcrt16.cpp that work on 2-byte units. Shim
// code is compiled without -fshort-wchar and never sees these macros.
#pragma once
#include <cstddef>
#include <cstdarg>

static_assert(sizeof(wchar_t) == 2, "game translation units must build with -fshort-wchar");

extern "C" {
size_t   hs_wcslen(const wchar_t* s);
wchar_t* hs_wcscpy(wchar_t* d, const wchar_t* s);
wchar_t* hs_wcsncpy(wchar_t* d, const wchar_t* s, size_t n);
wchar_t* hs_wcscat(wchar_t* d, const wchar_t* s);
wchar_t* hs_wcsncat(wchar_t* d, const wchar_t* s, size_t n);
int      hs_wcscmp(const wchar_t* a, const wchar_t* b);
int      hs_wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
int      hs_wcsicmp(const wchar_t* a, const wchar_t* b);
int      hs_wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* hs_wcschr(const wchar_t* s, wchar_t c);
wchar_t* hs_wcsrchr(const wchar_t* s, wchar_t c);
wchar_t* hs_wcsstr(const wchar_t* s, const wchar_t* sub);
long     hs_wcstol(const wchar_t* s, wchar_t** end, int base);
unsigned long hs_wcstoul(const wchar_t* s, wchar_t** end, int base);
int      hs_swprintf(wchar_t* d, size_t n, const wchar_t* fmt, ...);
int      hs_vswprintf(wchar_t* d, size_t n, const wchar_t* fmt, va_list ap);
int      hs_wsprintfW_unbounded(wchar_t* d, const wchar_t* fmt, ...);
wchar_t  hs_towlower(wchar_t c);
wchar_t  hs_towupper(wchar_t c);
int      hs_iswspace(wchar_t c);
int      hs_iswdigit(wchar_t c);
int      hs_iswalpha(wchar_t c);
}

#define wcslen   hs_wcslen
#define wcscpy   hs_wcscpy
#define wcsncpy  hs_wcsncpy
#define wcscat   hs_wcscat
#define wcsncat  hs_wcsncat
#define wcscmp   hs_wcscmp
#define wcsncmp  hs_wcsncmp
#define _wcsicmp hs_wcsicmp
#define wcsicmp  hs_wcsicmp
#define _wcsnicmp hs_wcsnicmp
#define wcschr   hs_wcschr
#define wcsrchr  hs_wcsrchr
#define wcsstr   hs_wcsstr
#define wcstol   hs_wcstol
#define wcstoul  hs_wcstoul
#define swprintf hs_swprintf
#define vswprintf hs_vswprintf
#define towlower hs_towlower
#define towupper hs_towupper
#define iswspace hs_iswspace
#define iswdigit hs_iswdigit
#define iswalpha hs_iswalpha

namespace std {
using ::hs_wcslen; using ::hs_wcscpy; using ::hs_wcsncpy; using ::hs_wcscat; using ::hs_wcsncat;
using ::hs_wcscmp; using ::hs_wcsncmp; using ::hs_wcschr; using ::hs_wcsrchr; using ::hs_wcsstr;
using ::hs_wcstol; using ::hs_wcstoul; using ::hs_swprintf; using ::hs_vswprintf;
using ::hs_towlower; using ::hs_towupper; using ::hs_iswspace; using ::hs_iswdigit; using ::hs_iswalpha;
}
