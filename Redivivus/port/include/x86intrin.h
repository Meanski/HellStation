// HellStation interposer: one code path for every port target.
//
// Source/D2Headers/d2_platform.h includes <x86intrin.h> (when __i386__ is set)
// after it has pulled in the Windows SDK headers and before its x86-only
// blocks: a real fs:[0] SEH registration whose inline asm calls _RtlUnwind@16
// (a PE-only spelling the ELF assembler rejects), and the tree's other
// __i386__ paths. Port builds put this directory ahead of the system headers,
// so this file forwards to the real header and then retires __i386__: the SDK
// headers above keep their architecture, and everything after takes the
// generic paths, which are the ones the PS2 build takes. The calling-convention
// attributes were fixed by the prelude, so the i386 ABI is unchanged.
#if !defined(HS_FAKE_I386)
#  include_next <x86intrin.h>
#else
// Not x86: RDTSC for the decompiler's pseudo-op is a monotonic counter.
extern "C" unsigned long long hs_rdtsc(void);
#  define __rdtsc() hs_rdtsc()
#endif

#if defined(HELLSTATION_PORT)
#  undef __i386__
#  undef __i386
#  undef i386
#endif
