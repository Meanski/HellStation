// The HellStation platform interface.
//
// The seam between the Win32 shim (Redivivus/port/win32/, compiled against the
// Windows SDK headers like the game) and a backend (Redivivus/port/backend/<b>/,
// plain target code: SDL2 + POSIX in Phase 1, ps2sdk in Phase 2). Plain C types
// only, so both sides can include it. A backend implements every function here.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Process ----------------------------------------------------------------
// Backend main() calls this once the platform is up; it runs the game.
int hs_game_main(int argc, char** argv);
// Read-only directory holding the owner's game data (MPQs, DLLs).
const char* hs_data_dir(void);
void hs_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void hs_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

// ---- Virtual memory -----------------------------------------------------------
// Reserve `size` bytes of zero-filled, read-write address space aligned to
// `align` (a power of two; Win32 VirtualAlloc needs 64 KiB), lazily backed where
// the target can. Decommit returns pages to zero; release frees.
void* hs_vm_reserve(size_t size, size_t align);
void  hs_vm_decommit(void* p, size_t size);
void  hs_vm_release(void* p, size_t size);

// ---- Memory footprint ---------------------------------------------------------
// What the target's C heap holds, for the memory report (win32/system.cpp).
// Zero means "not known on this target".
typedef struct hs_mem_info {
    size_t image;       // loaded program: text, data and bss
    size_t heap_taken;  // bytes the C heap has taken from the system (program break)
    size_t heap_peak;   // most it ever held
    size_t heap_in_use; // bytes in live malloc blocks
    size_t heap_limit;  // what a retail machine leaves the heap (PS2: _end to EndOfHeap)
    size_t phys_total;  // RAM to report to the game (GlobalMemoryStatus); it sizes caches from it
    size_t stacks;      // game thread stacks, where they're real RAM (EE), and the deepest use
    size_t stack_deepest;
} hs_mem_info;
void hs_mem_query(hs_mem_info* out);

// ---- Files ------------------------------------------------------------------
// Paths given to the backend are already translated to target spelling
// (see win32/paths.cpp). Lookup is case-insensitive per component, as on
// Windows. A file id is >= 0; -1 means failure (hs_errno() says why).
enum { HS_OPEN_READ = 1, HS_OPEN_WRITE = 2 };
enum { HS_CREATE_NEW = 1, HS_CREATE_ALWAYS = 2, HS_OPEN_EXISTING = 3, HS_OPEN_ALWAYS = 4, HS_TRUNCATE_EXISTING = 5 };
enum { HS_ERR_NONE = 0, HS_ERR_NOT_FOUND = 2, HS_ERR_PATH_NOT_FOUND = 3, HS_ERR_ACCESS = 5, HS_ERR_EXISTS = 80, HS_ERR_OTHER = 31 };
int      hs_file_open(const char* path, int access, int disposition);
int      hs_file_close(int fd);
int64_t  hs_file_read(int fd, void* buf, uint32_t n);
int64_t  hs_file_write(int fd, const void* buf, uint32_t n);
int64_t  hs_file_seek(int fd, int64_t offset, int whence);   // whence: 0 set, 1 cur, 2 end
int64_t  hs_file_size(int fd);
int      hs_file_stat(const char* path, int* isDir, int64_t* size, int64_t* mtime);
int      hs_file_delete(const char* path);
int      hs_file_mkdir(const char* path);
int      hs_file_rename(const char* from, const char* to);
int      hs_errno(void);
// Resolve `path` case-insensitively against what exists; returns the real
// spelling in `out` (or the input, if nothing matches). 0 on success.
int      hs_path_resolve(const char* path, char* out, size_t outSize);
// Directory listing for FindFirstFile: calls fn for each entry name.
int      hs_dir_list(const char* dir, void (*fn)(void* ctx, const char* name, int isDir, int64_t size), void* ctx);

// Where the Windows C: drive and the game folder live on the target.
const char* hs_root_dir(void);   // writable: C:\ maps here
const char* hs_game_dir(void);   // C:\Diablo II -> here (overlay: root first, then data)

// ---- Time -------------------------------------------------------------------
uint32_t hs_ticks_ms(void);           // monotonic, wraps like GetTickCount
uint64_t hs_perf_counter(void);       // monotonic high resolution
uint64_t hs_perf_frequency(void);
void     hs_sleep_ms(uint32_t ms);
void     hs_yield(void);
int64_t  hs_time_now(void);           // seconds since 1970, UTC

// ---- Threads and synchronisation --------------------------------------------
// Recursive mutex + condition variable are the primitives; Win32 events,
// semaphores and critical sections are built from them in the shim.
typedef struct hs_mutex hs_mutex;
typedef struct hs_cond hs_cond;
hs_mutex* hs_mutex_create(void);              // recursive
void      hs_mutex_destroy(hs_mutex* m);
void      hs_mutex_lock(hs_mutex* m);
int       hs_mutex_trylock(hs_mutex* m);      // 1 if acquired
void      hs_mutex_unlock(hs_mutex* m);
hs_cond*  hs_cond_create(void);
void      hs_cond_destroy(hs_cond* c);
// Waits with m held; returns 0 when signalled (or spuriously), 1 on timeout.
// timeoutMs == 0xFFFFFFFF waits forever.
int       hs_cond_wait(hs_cond* c, hs_mutex* m, uint32_t timeoutMs);
void      hs_cond_broadcast(hs_cond* c);

typedef struct hs_thread hs_thread;
// Starts fn(arg) on a new thread with the given stack size (0 = default).
hs_thread* hs_thread_start(void (*fn)(void*), void* arg, uint32_t stackSize);
void       hs_thread_detach(hs_thread* t);
uint32_t   hs_thread_id(void);                // stable small id for this thread
void       hs_thread_exit(void) __attribute__((noreturn));  // end the calling thread

// Log the calling thread's stack (symbolised where the target can).
void hs_backtrace(const char* why);

// ---- Video ------------------------------------------------------------------
// The game renders an 8-bit indexed frame; the backend presents it scaled to
// its display with the given palette (256 entries, 0x00RRGGBB).
int  hs_video_open(int width, int height, const char* title);
void hs_video_present(const uint8_t* pixels, int width, int height, int pitch,
                      const uint32_t* palette);
void hs_video_set_title(const char* title);

// ---- Input -------------------------------------------------------------------
// Events in game coordinates (the frame's pixel space). Keys are Windows
// virtual-key codes, so the shim can forward them as WM_KEY* unchanged.
enum hs_event_type {
    HS_EV_NONE = 0, HS_EV_QUIT, HS_EV_MOUSE_MOVE, HS_EV_MOUSE_BUTTON, HS_EV_MOUSE_WHEEL,
    HS_EV_KEY, HS_EV_CHAR, HS_EV_FOCUS,
    HS_EV_DEBUG_WHERE  // scripted "where": log the objects around the player (overrides/debug_where.cpp)
};
enum { HS_BUTTON_LEFT = 1, HS_BUTTON_RIGHT = 2, HS_BUTTON_MIDDLE = 3 };
typedef struct hs_event {
    int type;
    int x, y;          // mouse position (all mouse events)
    int button;        // HS_BUTTON_*
    int down;          // button/key pressed (1) or released (0); focus gained/lost
    int vk;            // HS_EV_KEY: Windows virtual-key code
    int scancode;      // HS_EV_KEY: PC set-1 scancode (for lParam)
    int repeat;        // HS_EV_KEY: auto-repeat
    int ch;            // HS_EV_CHAR: character (Latin-1)
    int wheel;         // HS_EV_MOUSE_WHEEL: +/- notches
} hs_event;
// Pumps the platform and returns the next event; 0 when none is pending.
int  hs_input_poll(hs_event* ev);
int  hs_input_key_down(int vk);            // current state, for GetAsyncKeyState
void hs_input_mouse_pos(int* x, int* y);
void hs_input_warp_mouse(int x, int y);
void hs_input_show_cursor(int show);

#ifdef __cplusplus
}
#endif
