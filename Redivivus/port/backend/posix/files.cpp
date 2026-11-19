// POSIX backend (Linux and the PS2's newlib): files with Windows-style case-insensitive lookup, and
// the game folder as an overlay of a writable root over the read-only data dir.
#include "Redivivus/port/platform.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#if !defined(__linux__)  // newlib: off_t is already 64-bit where it matters
#  define lseek64 lseek
#  define fstat64 fstat
#  define stat64 stat
#  define off64_t off_t
#endif

namespace {
thread_local int t_err;

int map_errno(int e) {
    switch (e) {
    case ENOENT: return HS_ERR_NOT_FOUND;
    case ENOTDIR: return HS_ERR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return HS_ERR_ACCESS;
    case EEXIST: return HS_ERR_EXISTS;
    default: return HS_ERR_OTHER;
    }
}
int fail() { t_err = map_errno(errno); return -1; }

// The PS2 disc (tools/port/make-iso.py): every file sits in the root under its
// 8.3 name in upper case, with ISO9660's ";1". A path below cdrom0: is a file
// name, maybe with separators in front; anything in a subdirectory isn't there.
bool on_disc(const std::string& path) { return path.compare(0, 7, "cdrom0:") == 0; }
std::string disc_name(const std::string& path) {
    std::string name = path.substr(7);
    while (!name.empty() && (name[0] == '/' || name[0] == '\\')) name.erase(0, 1);
    if (name.empty()) return "cdrom0:\\";
    if (name.find_first_of("/\\") != std::string::npos) return "cdrom0:\\NOT-ON-THE-DISC;1";
    for (char& c : name) c = (char)toupper((unsigned char)c);
    return "cdrom0:\\" + name + ";1";
}
// The CD driver's getstat is unreliable: a file's size comes from seeking to its end.
int disc_stat(const std::string& path, int64_t* size) {
    if (path == "cdrom0:\\") { if (size) *size = 0; return 1; }  // the root: a directory
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { errno = ENOENT; return -1; }
    off64_t end = lseek64(fd, 0, SEEK_END);
    close(fd);
    if (size) *size = end < 0 ? 0 : end;
    return 0;
}

// Reading from the disc. A drive spends its time seeking, and the game reads
// archives a 4 KiB sector at a time: loading town was 33,500 reads (PCSX2,
// emulating DVD seeks: still loading after ten minutes). Disc files read
// through four 32 KiB windows, least recently used out: in the same load that
// would have been 3,200 reads. Positions are kept here, so a seek costs nothing
// until data is needed.
constexpr uint32_t kWindow = 32u << 10;
constexpr int kWindows = 4;
struct DiscFile { bool on; int64_t pos, size; };
DiscFile g_discFiles[64];
struct Window { int fd; int64_t block; uint32_t bytes; uint32_t used; uint8_t* data; };
Window g_windows[kWindows];
uint32_t g_windowClock;
// Several threads read archives (the main thread, Storm's I/O thread), and a
// read blocks in an IOP call, which lets the others run: one lock for all of it.
hs_mutex* disc_lock() {
    static hs_mutex* m = hs_mutex_create();
    return m;
}
struct DiscGuard {
    DiscGuard() { hs_mutex_lock(disc_lock()); }
    ~DiscGuard() { hs_mutex_unlock(disc_lock()); }
};
DiscFile* disc_file(int fd) { return fd >= 0 && fd < 64 && g_discFiles[fd].on ? &g_discFiles[fd] : nullptr; }
Window* window_for(int fd, int64_t block) {
    Window* lru = &g_windows[0];
    for (Window& w : g_windows) {
        if (w.data && w.fd == fd && w.block == block) { w.used = ++g_windowClock; return &w; }
        if (!w.data || w.used < lru->used) lru = &w;
    }
    if (!lru->data && !(lru->data = (uint8_t*)malloc(kWindow))) return nullptr;
    lru->fd = -1;
    if (lseek64(fd, (off64_t)(block * kWindow), SEEK_SET) < 0) return nullptr;
    uint32_t got = 0;
    while (got < kWindow) {
        ssize_t r = read(fd, lru->data + got, kWindow - got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    lru->fd = fd;
    lru->block = block;
    lru->bytes = got;
    lru->used = ++g_windowClock;
    return lru;
}
int64_t disc_read(DiscFile& f, int fd, void* buf, uint32_t n) {
    uint32_t done = 0;
    while (done < n && f.pos < f.size) {
        Window* w = window_for(fd, f.pos / kWindow);
        if (!w) return done ? (int64_t)done : -1;
        uint32_t off = (uint32_t)(f.pos % kWindow);
        if (off >= w->bytes) break;
        uint32_t take = w->bytes - off;
        if (take > n - done) take = n - done;
        memcpy((char*)buf + done, w->data + off, take);
        done += take;
        f.pos += take;
    }
    return done;
}

// The memory card's getstat answers for paths that don't exist: whether a file
// or a folder is there comes from opening it.
bool on_card(const std::string& path) { return path.compare(0, 4, "mc0:") == 0; }
int card_stat(const std::string& path, int* isDir, int64_t* size) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        off64_t end = lseek64(fd, 0, SEEK_END);
        close(fd);
        if (isDir) *isDir = 0;
        if (size) *size = end < 0 ? 0 : end;
        return 0;
    }
    if (DIR* d = opendir(path.c_str())) {
        closedir(d);
        if (isDir) *isDir = 1;
        if (size) *size = 0;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

// Walk `path` component by component, matching each case-insensitively when
// the exact spelling doesn't exist. Components that don't exist at all are
// kept as given (so creating a new file works).
std::string resolve(const std::string& path) {
    if (on_disc(path)) return disc_name(path);
    struct stat st;
    if (on_card(path)) {  // case-insensitive lookups need a working stat: take the path as given
        return path;
    }
    if (path.empty() || stat(path.c_str(), &st) == 0) return path;
    std::string out = path[0] == '/' ? "/" : "";
    size_t i = path[0] == '/' ? 1 : 0;
    bool missing = false;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string comp = path.substr(i, j - i);
        i = j + 1;
        if (comp.empty()) continue;
        std::string dir = out.empty() ? "." : out;
        std::string candidate = out + (out.empty() || out.back() == '/' ? "" : "/") + comp;
        if (!missing && stat(candidate.c_str(), &st) != 0) {
            bool found = false;
            if (DIR* d = opendir(dir.c_str())) {
                while (dirent* e = readdir(d)) {
                    if (strcasecmp(e->d_name, comp.c_str()) == 0) {
                        candidate = out + (out.empty() || out.back() == '/' ? "" : "/") + e->d_name;
                        found = true;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) missing = true;
        }
        out = candidate;
    }
    return out;
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}
}  // namespace
extern "C" const char* hs_default_root_dir(void);  // per backend
namespace {
}  // namespace

extern "C" {

const char* hs_root_dir(void) {
    static std::string root = [] {
        std::string r = env_or("HS_ROOT", hs_default_root_dir());
        std::string acc;
        for (size_t i = 0; i <= r.size(); i++) {  // mkdir -p
            if (i == r.size() || r[i] == '/') { if (!acc.empty()) mkdir(acc.c_str(), 0755); }
            if (i < r.size()) acc += r[i];
        }
        return r;
    }();
    return root.c_str();
}

const char* hs_game_dir(void) {
    static std::string dir = [] {
        std::string d = std::string(hs_root_dir()) + "/Diablo II";
        mkdir(d.c_str(), 0755);
        return d;
    }();
    return dir.c_str();
}

int hs_errno(void) { return t_err; }

int hs_path_resolve(const char* path, char* out, size_t outSize) {
    std::string r = resolve(path);
    if (r.size() + 1 > outSize) return -1;
    std::memcpy(out, r.c_str(), r.size() + 1);
    return 0;
}

int hs_file_open(const char* path, int access, int disposition) {
    std::string p = resolve(path);
    int flags = (access & HS_OPEN_WRITE) ? ((access & HS_OPEN_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case HS_CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case HS_CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case HS_OPEN_ALWAYS: flags |= O_CREAT; break;
    case HS_TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
    }
#ifndef O_CLOEXEC
#  define O_CLOEXEC 0
#endif
    int fd = open(p.c_str(), flags | O_CLOEXEC, 0644);
#if defined(_EE)
    // Creating a file in a folder that isn't there: make the folders. Windows
    // always has the user's Documents tree the game logs and saves under; a
    // fresh memory card has nothing.
    if (fd < 0 && (flags & O_CREAT)) {
        for (size_t i = p.find('/'); i != std::string::npos; i = p.find('/', i + 1))
            if (i > 0 && p[i - 1] != ':') mkdir(p.substr(0, i).c_str(), 0755);
        fd = open(p.c_str(), flags | O_CLOEXEC, 0644);
    }
    // The memory card (mc0:) refuses O_TRUNC, even with O_CREAT on a new file:
    // replace the file instead.
    if (fd < 0 && (flags & O_TRUNC) && p.compare(0, 3, "mc0") == 0) {
        unlink(p.c_str());
        fd = open(p.c_str(), (flags & ~O_TRUNC) | O_CREAT | O_CLOEXEC, 0644);
    }
#endif
    if (fd < 0) return fail();
    struct stat st;
    if (!on_card(p) && !on_disc(p) && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); t_err = HS_ERR_ACCESS; return -1; }
    if (on_disc(p) && fd < 64) {  // read through the windows (above)
        DiscGuard g;
        off64_t end = lseek64(fd, 0, SEEK_END);
        lseek64(fd, 0, SEEK_SET);
        g_discFiles[fd] = {true, 0, end < 0 ? 0 : (int64_t)end};
        for (Window& w : g_windows)
            if (w.fd == fd) w.fd = -1;
    }
    t_err = 0;
    return fd;
}

int hs_file_close(int fd) {
    if (DiscFile* f = disc_file(fd)) {
        DiscGuard g;
        f->on = false;
        for (Window& w : g_windows)
            if (w.fd == fd) w.fd = -1;
    }
    return close(fd) == 0 ? 0 : fail();
}

int64_t hs_file_read(int fd, void* buf, uint32_t n) {
    if (DiscFile* f = disc_file(fd)) {
        DiscGuard g;
        int64_t r = disc_read(*f, fd, buf, n);
        if (r < 0) { errno = EIO; return fail(); }
        return r;
    }
    uint32_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char*)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return fail(); }
        if (r == 0) break;
        done += (uint32_t)r;
    }
    return done;
}

int64_t hs_file_write(int fd, const void* buf, uint32_t n) {
    uint32_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char*)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return fail(); }
        done += (uint32_t)r;
    }
    return done;
}

int64_t hs_file_seek(int fd, int64_t offset, int whence) {
    if (DiscFile* f = disc_file(fd)) {
        DiscGuard g;
        int64_t to = whence == 0 ? offset : whence == 1 ? f->pos + offset : f->size + offset;
        if (to < 0) { errno = EINVAL; return fail(); }
        f->pos = to;
        return to;
    }
    off64_t r = lseek64(fd, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    return r < 0 ? fail() : r;
}

int64_t hs_file_size(int fd) {
    if (DiscFile* f = disc_file(fd)) return f->size;
#if defined(_EE)
    // fstat on a file open on the disc (cdrom0:) reports nothing useful: Storm
    // read every archive there as empty. Seeking to the end works everywhere.
    off64_t cur = lseek64(fd, 0, SEEK_CUR);
    off64_t end = cur < 0 ? -1 : lseek64(fd, 0, SEEK_END);
    if (end < 0) return fail();
    lseek64(fd, cur, SEEK_SET);
    return (int64_t)end;
#else
    struct stat64 st;
    return fstat64(fd, &st) == 0 ? (int64_t)st.st_size : fail();
#endif
}

int hs_file_stat(const char* path, int* isDir, int64_t* size, int64_t* mtime) {
    if (on_disc(path)) {
        int r = disc_stat(disc_name(path), size);
        if (r < 0) return fail();
        if (isDir) *isDir = r == 1;
        if (mtime) *mtime = 0;
        return 0;
    }
    if (on_card(path)) {
        int dir = 0;
        if (card_stat(path, &dir, size) < 0) return fail();
        if (isDir) *isDir = dir;
        if (mtime) *mtime = 0;
        return 0;
    }
    struct stat64 st;
    if (stat64(resolve(path).c_str(), &st) != 0) return fail();
    if (isDir) *isDir = S_ISDIR(st.st_mode);
    if (size) *size = st.st_size;
    if (mtime) *mtime = st.st_mtime;
    return 0;
}

// A missing file is reported without calling unlink: on the PS2 under PCSX2's host device,
// unlinking a path that doesn't exist left a directory of that name, which then blocked the
// file (the automap's "<char>.ma0".."ma3", deleted before they're first written).
int hs_file_delete(const char* path) {
    std::string p = resolve(path);
    if (on_card(p)) {
        int dir = 0;
        if (card_stat(p, &dir, nullptr) < 0) return fail();
        if (dir) { t_err = HS_ERR_ACCESS; return -1; }
        return unlink(p.c_str()) == 0 ? 0 : fail();
    }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return fail();
    if (S_ISDIR(st.st_mode)) { t_err = HS_ERR_ACCESS; return -1; }  // DeleteFile on a directory fails
    return unlink(p.c_str()) == 0 ? 0 : fail();
}
int hs_file_mkdir(const char* path) { return mkdir(resolve(path).c_str(), 0755) == 0 ? 0 : fail(); }
int hs_file_rename(const char* from, const char* to) {
    return rename(resolve(from).c_str(), resolve(to).c_str()) == 0 ? 0 : fail();
}

int hs_dir_list(const char* dir, void (*fn)(void*, const char*, int, int64_t), void* ctx) {
    std::string d = resolve(dir);
    DIR* h = opendir(d.c_str());
    if (!h) return fail();
    while (dirent* e = readdir(h)) {
        struct stat64 st;
        std::string full = d + "/" + e->d_name;
        if (stat64(full.c_str(), &st) != 0) continue;
        fn(ctx, e->d_name, S_ISDIR(st.st_mode), st.st_size);
    }
    closedir(h);
    return 0;
}

// ---- Time ---------------------------------------------------------------------
#ifndef CLOCK_MONOTONIC
#  define CLOCK_MONOTONIC CLOCK_REALTIME
#endif
uint32_t hs_ticks_ms(void) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
uint64_t hs_perf_counter(void) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t hs_perf_frequency(void) { return 1000000000ull; }
void hs_sleep_ms(uint32_t ms) {
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
// A yield must let every other thread run, as Windows' Sleep(0) does for the
// game's equal-priority threads. On the EE a yield only rotates the caller's
// own priority level, and the game's threads run below the main thread: the
// main menu's "wait until the menu sounds stop" loop (D2Launch, Sleep(0) until
// the sound thread clears the flag) spun forever. A short sleep lets them in.
#if defined(_EE)
void hs_yield(void) { usleep(100); }
#else
void hs_yield(void) { std::this_thread::yield(); }
#endif
int64_t hs_time_now(void) { return (int64_t)std::time(nullptr); }

}  // extern "C"
