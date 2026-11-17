// 16-bit wide CRT (see wcrt16.h). Compiled with -fshort-wchar like game code.
// Characters are UTF-16 code units; case mapping and classification cover
// ASCII and Latin-1, which is what the game's tables and fonts use.
#include "Redivivus/port/win32/wcrt16.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
inline wchar_t lower(wchar_t c) {
    if ((c >= L'A' && c <= L'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (wchar_t)(c + 32);
    return c;
}
inline wchar_t upper(wchar_t c) {
    if ((c >= L'a' && c <= L'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (wchar_t)(c - 32);
    return c;
}
}  // namespace

extern "C" {

size_t hs_wcslen(const wchar_t* s) { const wchar_t* p = s; while (*p) ++p; return (size_t)(p - s); }
wchar_t* hs_wcscpy(wchar_t* d, const wchar_t* s) { wchar_t* r = d; while ((*d++ = *s++)) {} return r; }
wchar_t* hs_wcsncpy(wchar_t* d, const wchar_t* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}
wchar_t* hs_wcscat(wchar_t* d, const wchar_t* s) { hs_wcscpy(d + hs_wcslen(d), s); return d; }
wchar_t* hs_wcsncat(wchar_t* d, const wchar_t* s, size_t n) {
    wchar_t* e = d + hs_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; ++i) e[i] = s[i];
    e[i] = 0;
    return d;
}
int hs_wcscmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned short)*a - (int)(unsigned short)*b;
}
int hs_wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b || !*a) return (int)(unsigned short)*a - (int)(unsigned short)*b;
    }
    return 0;
}
int hs_wcsicmp(const wchar_t* a, const wchar_t* b) {
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return (int)(unsigned short)lower(*a) - (int)(unsigned short)lower(*b);
}
int hs_wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        wchar_t x = lower(*a), y = lower(*b);
        if (x != y || !x) return (int)(unsigned short)x - (int)(unsigned short)y;
    }
    return 0;
}
wchar_t* hs_wcschr(const wchar_t* s, wchar_t c) {
    for (;; ++s) {
        if (*s == c) return (wchar_t*)s;
        if (!*s) return nullptr;
    }
}
wchar_t* hs_wcsrchr(const wchar_t* s, wchar_t c) {
    const wchar_t* r = nullptr;
    for (;; ++s) {
        if (*s == c) r = s;
        if (!*s) return (wchar_t*)r;
    }
}
wchar_t* hs_wcsstr(const wchar_t* s, const wchar_t* sub) {
    size_t n = hs_wcslen(sub);
    if (!n) return (wchar_t*)s;
    for (; *s; ++s)
        if (hs_wcsncmp(s, sub, n) == 0) return (wchar_t*)s;
    return nullptr;
}

static std::string narrow(const wchar_t* s, const wchar_t** stop = nullptr) {
    std::string r;
    const wchar_t* p = s;
    for (; *p && (unsigned short)*p < 0x80; ++p) r += (char)*p;
    if (stop) *stop = p;
    return r;
}
long hs_wcstol(const wchar_t* s, wchar_t** end, int base) {
    std::string n = narrow(s);
    char* e = nullptr;
    long v = std::strtol(n.c_str(), &e, base);
    if (end) *end = (wchar_t*)(s + (e - n.c_str()));
    return v;
}
unsigned long hs_wcstoul(const wchar_t* s, wchar_t** end, int base) {
    std::string n = narrow(s);
    char* e = nullptr;
    unsigned long v = std::strtoul(n.c_str(), &e, base);
    if (end) *end = (wchar_t*)(s + (e - n.c_str()));
    return v;
}

// MSVC wide printf: %s and %c take wide arguments, %S and %C narrow ones; %hs/%ls
// force narrow/wide. Numeric conversions are delegated to the narrow snprintf.
int hs_vswprintf(wchar_t* d, size_t cap, const wchar_t* fmt, va_list ap) {
    std::u16string out;
    auto put = [&](char16_t c) { out.push_back(c); };
    for (const wchar_t* p = fmt; *p; ++p) {
        if (*p != L'%') { put((char16_t)*p); continue; }
        std::string spec = "%";
        ++p;
        while (*p && std::strchr("-+ #0", (char)*p)) spec += (char)*p++;
        if (*p == L'*') { spec += std::to_string(va_arg(ap, int)); ++p; }
        while (*p >= L'0' && *p <= L'9') spec += (char)*p++;
        if (*p == L'.') {
            spec += '.'; ++p;
            if (*p == L'*') { spec += std::to_string(va_arg(ap, int)); ++p; }
            while (*p >= L'0' && *p <= L'9') spec += (char)*p++;
        }
        int size = 0;  // 'h' = -1, 'l' = 1, 'll'/'I64' = 2
        for (;;) {
            if (*p == L'h') { size = -1; ++p; }
            else if (*p == L'l') { size = size == 1 ? 2 : 1; ++p; }
            else if (p[0] == L'I' && p[1] == L'6' && p[2] == L'4') { size = 2; p += 3; }
            else break;
        }
        wchar_t conv = *p;
        if (!conv) break;
        char buf[128];
        switch (conv) {
        case L'%': put(u'%'); break;
        case L's': case L'S': {
            bool wide = (conv == L's') ? size != -1 : size == 1;
            if (wide) {
                const wchar_t* s = va_arg(ap, const wchar_t*);
                if (!s) s = L"(null)";
                for (; *s; ++s) put((char16_t)*s);
            } else {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                for (; *s; ++s) put((char16_t)(unsigned char)*s);
            }
            break;
        }
        case L'c': case L'C': put((char16_t)va_arg(ap, int)); break;
        case L'd': case L'i': case L'u': case L'x': case L'X': case L'o': {
            spec += size == 2 ? "ll" : "";
            spec += (char)conv;
            if (size == 2) std::snprintf(buf, sizeof buf, spec.c_str(), va_arg(ap, long long));
            else std::snprintf(buf, sizeof buf, spec.c_str(), va_arg(ap, int));
            for (char* q = buf; *q; ++q) put((char16_t)*q);
            break;
        }
        case L'f': case L'e': case L'E': case L'g': case L'G': {
            spec += (char)conv;
            std::snprintf(buf, sizeof buf, spec.c_str(), va_arg(ap, double));
            for (char* q = buf; *q; ++q) put((char16_t)*q);
            break;
        }
        case L'p': {
            std::snprintf(buf, sizeof buf, "%08X", (unsigned)(uintptr_t)va_arg(ap, void*));
            for (char* q = buf; *q; ++q) put((char16_t)*q);
            break;
        }
        default: put(u'%'); put((char16_t)conv); break;
        }
    }
    if (cap) {
        size_t n = out.size() < cap ? out.size() : cap - 1;
        std::memcpy(d, out.data(), n * sizeof(wchar_t));
        d[n] = 0;
    }
    return out.size() < cap ? (int)out.size() : -1;
}
int hs_swprintf(wchar_t* d, size_t n, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = hs_vswprintf(d, n, fmt, ap);
    va_end(ap);
    return r;
}
int hs_wsprintfW_unbounded(wchar_t* d, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = hs_vswprintf(d, 1024, fmt, ap);  // wsprintf's documented maximum
    va_end(ap);
    return r;
}

wchar_t hs_towlower(wchar_t c) { return lower(c); }
wchar_t hs_towupper(wchar_t c) { return upper(c); }
int hs_iswspace(wchar_t c) { return c == L' ' || (c >= 9 && c <= 13) || c == 0xA0; }
int hs_iswdigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
int hs_iswalpha(wchar_t c) { return lower(c) != upper(c); }

}  // extern "C"
