// EE only: Win32/MSVC names the game and the Win32 shim define that the PS2
// toolchain's own libraries also define (the EE kernel's CreateThread and
// friends, ps2sdk's BSD sockets, newlib's _mkdir syscall stub). Unrenamed, the
// linker silently binds ps2sdk's internal calls to the Win32 functions; e.g.
// libpthreadglue's CreateThread(ee_thread_t*) landed in Win32 CreateThread.
// Renaming on our side keeps every toolchain symbol intact.
//
// The list is tools/port/ps2-collisions.sh's output; the build re-runs that
// scan after linking and fails if a new collision appears.
#pragma once
#define CreateThread hs_w32_CreateThread
#define ExitThread hs_w32_ExitThread
#define ResumeThread hs_w32_ResumeThread
#define SuspendThread hs_w32_SuspendThread
#define TerminateThread hs_w32_TerminateThread
#define _mkdir hs_w32__mkdir
#define accept hs_w32_accept
#define bind hs_w32_bind
#define connect hs_w32_connect
#define gethostbyname hs_w32_gethostbyname
#define getpeername hs_w32_getpeername
#define getsockname hs_w32_getsockname
#define listen hs_w32_listen
#define recv hs_w32_recv
#define select hs_w32_select
#define send hs_w32_send
#define setsockopt hs_w32_setsockopt
#define socket hs_w32_socket
