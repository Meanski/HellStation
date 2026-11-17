// MSVC CRT functions the game calls that the host C library doesn't provide.
// Declared for game translation units; implemented in msvcrt.cpp on top of
// the host C library. Names and semantics follow the MSVC 2005 CRT.
#pragma once
#include <cstddef>
#include <cstdio>
#include <ctime>

extern "C" {
typedef long hs_time32_t;
FILE*  _wfopen(const wchar_t* name, const wchar_t* mode);
long   _time32(long* t);
int    strncpy_s(char* dst, size_t dstSize, const char* src, size_t count);
int    strcpy_s(char* dst, size_t dstSize, const char* src);
int    strcat_s(char* dst, size_t dstSize, const char* src);
char*  _itoa(int value, char* buf, int radix);
char*  _ltoa(long value, char* buf, int radix);
char*  _ultoa(unsigned long value, char* buf, int radix);
char*  _uitoa(unsigned int value, char* buf, int radix);
char*  _strlwr(char* s);
char*  _strupr(char* s);
char*  _fullpath(char* absPath, const char* relPath, size_t maxLength);
int    _localtime_s(struct tm* out, const time_t* t);
long long _time64(long long* t);
int    _localtime64_s(struct tm* out, const long long* t);
char*  _i64toa(long long value, char* buf, int radix);
char*  _ui64toa(unsigned long long value, char* buf, int radix);
int    _access(const char* path, int mode);
int    _mkdir(const char* path);
}
