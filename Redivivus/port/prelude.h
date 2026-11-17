// HellStation portability prelude.
//
// Force-included ahead of Source/D2Headers/d2_platform.h in port builds
// (-DREDIVIVUS_PORT=ON). The generated tree is written against the Windows SDK
// as mingw-w64 spells it, so port builds keep _WIN32 defined and compile
// against mingw-w64's Win32 API headers (a build dependency, public domain),
// with the target's own C library underneath. The Win32 functions those
// headers declare are implemented in Redivivus/port/win32/.
//
// This file only reconciles the two header worlds: compiler keywords, the
// handful of types both the C library and the Windows SDK define, and the
// CRT behaviours the game relies on with MSVC semantics (16-bit wchar_t).
#pragma once

#if !defined(_WIN32) || !defined(HELLSTATION_PORT)
#error "prelude.h needs -D_WIN32 -DHELLSTATION_PORT (set by cmake/port.cmake)"
#endif

// --- Compiler surface ------------------------------------------------------

// On i386 the real attributes keep the Windows ABI (callee-cleanup stdcall,
// ecx/edx fastcall, ecx this), so code that casts between conventions behaves
// as on Windows. Other targets have one convention.
#if defined(__i386__)
#  define __stdcall  __attribute__((__stdcall__))
#  define __fastcall __attribute__((__fastcall__))
#  define __thiscall __attribute__((__thiscall__))
#  define __cdecl    __attribute__((__cdecl__))
#else
#  define __stdcall
#  define __fastcall
#  define __thiscall
#  define __cdecl
#endif

#define __declspec(x) __hs_declspec_##x
#define __hs_declspec_noreturn    __attribute__((__noreturn__))
#define __hs_declspec_noinline    __attribute__((__noinline__))
#define __hs_declspec_nothrow     __attribute__((__nothrow__))
#define __hs_declspec_naked       __attribute__((__naked__))
#define __hs_declspec_deprecated  __attribute__((__deprecated__))
#define __hs_declspec_selectany   __attribute__((__weak__))
#define __hs_declspec_thread      __thread
#define __hs_declspec_align(n)    __attribute__((__aligned__(n)))
#define __hs_declspec_dllimport
#define __hs_declspec_dllexport
#define __hs_declspec_novtable
#define __hs_declspec_restrict
#define __hs_declspec_noalias
#define __hs_declspec_uuid(x)
#define __hs_declspec_property(...)

// The x86 CONTEXT, NT_TIB and friends the generated code reads, on every target.
#ifndef _X86_
#  define _X86_ 1
#endif

// --- C library first, then claim the types the SDK would redefine ----------

// Winsock owns fd_set/select with the Windows layout the game was compiled
// against; keep the C library's out of game translation units.
#define _SYS_SELECT_H 1
#define __FD_SET_DEFINED


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <cerrno>
#include <sys/time.h>
#include <strings.h>

// stdio: the C library owns FILE. d2_platform.h would otherwise declare
// FILE as MSVC's struct _iobuf, which glibc and newlib can't honour; code
// that reads _iobuf fields is handled in the shim, case by case.
#define _FILE_DEFINED
struct _iobuf;

// time_t: the C library's (64-bit on newlib); the game spells out __time32_t /
// __time64_t where the width matters.
#define _TIME_T_DEFINED

// timeval: identical layout on every ILP32 target; let the C library's stand.
#define _TIMEVAL_DEFINED

// Non-x86 targets (the PS2's EE): the Windows SDK headers only work for a known
// architecture, and the layouts the game was decompiled against are x86's. Show
// them __i386__; Redivivus/port/include/x86intrin.h retires it again right
// after the SDK headers, so game code takes the generic paths.
#if !defined(__i386__) && !defined(__x86_64__)
#  define __i386__ 1
#  define HS_FAKE_I386 1
#endif

// --- MSVC CRT semantics ----------------------------------------------------

// wchar_t is 16-bit on Windows, and the game's literals and buffers assume it
// (game TUs build with -fshort-wchar). The C library's wide functions are
// 32-bit, so wide CRT calls from game code go to 16-bit implementations.
#include "Redivivus/port/win32/wcrt16.h"
#include "Redivivus/port/win32/msvcrt.h"

