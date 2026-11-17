// POSIX backend (Linux and the PS2's newlib): files with Windows-style case-insensitive lookup, and
// the game folder as an overlay of a writable root over the read-only data dir.
#include "Redivivus/port/platform.h"

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

// Walk `path` component by component, matching each case-insensitively when
// the exact spelling doesn't exist. Components that don't exist at all are
// kept as given (so creating a new file works).
std::string resolve(const std::string& path) {
    struct stat st;
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
    if (fd < 0) return fail();
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); t_err = HS_ERR_ACCESS; return -1; }
    t_err = 0;
    return fd;
}

int hs_file_close(int fd) { return close(fd) == 0 ? 0 : fail(); }

int64_t hs_file_read(int fd, void* buf, uint32_t n) {
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
    off64_t r = lseek64(fd, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    return r < 0 ? fail() : r;
}

int64_t hs_file_size(int fd) {
    struct stat64 st;
    return fstat64(fd, &st) == 0 ? (int64_t)st.st_size : fail();
}

int hs_file_stat(const char* path, int* isDir, int64_t* size, int64_t* mtime) {
    struct stat64 st;
    if (stat64(resolve(path).c_str(), &st) != 0) return fail();
    if (isDir) *isDir = S_ISDIR(st.st_mode);
    if (size) *size = st.st_size;
    if (mtime) *mtime = st.st_mtime;
    return 0;
}

int hs_file_delete(const char* path) { return unlink(resolve(path).c_str()) == 0 ? 0 : fail(); }
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
void hs_yield(void) { std::this_thread::yield(); }
int64_t hs_time_now(void) { return (int64_t)std::time(nullptr); }

}  // extern "C"
