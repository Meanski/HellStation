// Win32 file API over the backend's file layer.
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/paths.h"

namespace {

struct HsFile : HsObject {
    static constexpr Kind kKind = File;
    int fd = -1;
    std::string path;
    HsFile() : HsObject(File) {}
    ~HsFile() override { if (fd >= 0) hs_file_close(fd); }
};

struct HsFind : HsObject {
    static constexpr Kind kKind = Find;
    struct Entry { std::string name; bool dir; int64_t size; };
    std::vector<Entry> entries;
    size_t next = 0;
    HsFind() : HsObject(Find) {}
};

DWORD win_error(int e) {
    switch (e) {
    case HS_ERR_NONE: return ERROR_SUCCESS;
    case HS_ERR_NOT_FOUND: return ERROR_FILE_NOT_FOUND;
    case HS_ERR_PATH_NOT_FOUND: return ERROR_PATH_NOT_FOUND;
    case HS_ERR_ACCESS: return ERROR_ACCESS_DENIED;
    case HS_ERR_EXISTS: return ERROR_FILE_EXISTS;
    default: return ERROR_GEN_FAILURE;
    }
}

// Windows wildcard match (* and ?), case-insensitive.
bool wild_match(const char* p, const char* s) {
    for (; *p; ++p, ++s) {
        if (*p == '*') {
            while (p[1] == '*') ++p;
            if (!p[1]) return true;
            for (; *s; ++s) if (wild_match(p + 1, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '?' && std::tolower((unsigned char)*p) != std::tolower((unsigned char)*s)) return false;
    }
    return !*s;
}

void fill_find_data(const HsFind::Entry& e, LPWIN32_FIND_DATAA fd) {
    std::memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    fd->nFileSizeLow = (DWORD)e.size;
    fd->nFileSizeHigh = (DWORD)((uint64_t)e.size >> 32);
    std::strncpy(fd->cFileName, e.name.c_str(), MAX_PATH - 1);
}

}  // namespace

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE) {
    bool write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    std::string target = hs_win_to_target(name, write);
    int acc = HS_OPEN_READ | (write ? HS_OPEN_WRITE : 0);
    int disp = disposition == CREATE_NEW ? HS_CREATE_NEW
             : disposition == CREATE_ALWAYS ? HS_CREATE_ALWAYS
             : disposition == OPEN_ALWAYS ? HS_OPEN_ALWAYS
             : disposition == TRUNCATE_EXISTING ? HS_TRUNCATE_EXISTING
             : HS_OPEN_EXISTING;
    int fd = hs_file_open(target.c_str(), acc, disp);
    if (fd < 0 && write && !(access & GENERIC_READ)) fd = hs_file_open(target.c_str(), HS_OPEN_WRITE, disp);
    if (fd < 0) {
        SetLastError(win_error(hs_errno()));
        return INVALID_HANDLE_VALUE;
    }
    HsFile* f = new HsFile;
    f->fd = fd;
    f->path = target;
    SetLastError(ERROR_SUCCESS);
    return hs_handle_new(f);
}

BOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return FALSE;
    if (ov) {
        // Overlapped reads complete synchronously at the given offset.
        int64_t off = (int64_t)ov->Offset | ((int64_t)ov->OffsetHigh << 32);
        if (hs_file_seek(f->fd, off, 0) < 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    }
    int64_t r = hs_file_read(f->fd, buf, n);
    if (r < 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    if (read) *read = (DWORD)r;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    return TRUE;
}

BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return FALSE;
    if (ov) {
        int64_t off = (int64_t)ov->Offset | ((int64_t)ov->OffsetHigh << 32);
        if (hs_file_seek(f->fd, off, 0) < 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    }
    int64_t r = hs_file_write(f->fd, buf, n);
    if (r < 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    if (written) *written = (DWORD)r;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    return TRUE;
}

BOOL WINAPI GetOverlappedResult(HANDLE, LPOVERLAPPED ov, LPDWORD n, BOOL) {
    if (n) *n = (DWORD)ov->InternalHigh;
    return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return INVALID_SET_FILE_POINTER;
    int64_t off = hi ? (((int64_t)*hi << 32) | (uint32_t)lo) : (int64_t)lo;
    int64_t r = hs_file_seek(f->fd, off, (int)method);
    if (r < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return INVALID_SET_FILE_POINTER; }
    if (hi) *hi = (LONG)(r >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)r;
}

BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newPos, DWORD method) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return FALSE;
    int64_t r = hs_file_seek(f->fd, dist.QuadPart, (int)method);
    if (r < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return FALSE; }
    if (newPos) newPos->QuadPart = r;
    return TRUE;
}

DWORD WINAPI GetFileSize(HANDLE h, LPDWORD hi) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return INVALID_FILE_SIZE;
    int64_t s = hs_file_size(f->fd);
    if (hi) *hi = (DWORD)((uint64_t)s >> 32);
    return (DWORD)s;
}

BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
    HsFile* f = hs_handle_as<HsFile>(h);
    if (!f) return FALSE;
    size->QuadPart = hs_file_size(f->fd);
    return TRUE;
}

BOOL WINAPI SetEndOfFile(HANDLE h) {
    HS_STUB("no-op; files are never shrunk by the game in single player");
    return hs_handle_as<HsFile>(h) != nullptr;
}

BOOL WINAPI FlushFileBuffers(HANDLE h) { return hs_handle_as<HsFile>(h) != nullptr; }

DWORD WINAPI GetFileType(HANDLE h) { return hs_handle_as<HsFile>(h) ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN; }

DWORD WINAPI GetFileAttributesA(LPCSTR name) {
    int isDir = 0;
    if (hs_file_stat(hs_win_to_target(name, false).c_str(), &isDir, nullptr, nullptr) != 0) {
        SetLastError(win_error(hs_errno()));
        return INVALID_FILE_ATTRIBUTES;
    }
    return isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL WINAPI SetFileAttributesA(LPCSTR, DWORD) { return TRUE; }

BOOL WINAPI DeleteFileA(LPCSTR name) {
    if (hs_file_delete(hs_win_to_target(name, true).c_str()) != 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES) {
    if (hs_file_mkdir(hs_win_to_target(name, true).c_str()) != 0) {
        SetLastError(hs_errno() == HS_ERR_EXISTS ? ERROR_ALREADY_EXISTS : win_error(hs_errno()));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to) {
    if (hs_file_rename(hs_win_to_target(from, true).c_str(), hs_win_to_target(to, true).c_str()) != 0) {
        SetLastError(win_error(hs_errno()));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists) {
    int in = hs_file_open(hs_win_to_target(from, false).c_str(), HS_OPEN_READ, HS_OPEN_EXISTING);
    if (in < 0) { SetLastError(win_error(hs_errno())); return FALSE; }
    int out = hs_file_open(hs_win_to_target(to, true).c_str(), HS_OPEN_WRITE, failIfExists ? HS_CREATE_NEW : HS_CREATE_ALWAYS);
    if (out < 0) { hs_file_close(in); SetLastError(win_error(hs_errno())); return FALSE; }
    char buf[65536];
    int64_t r;
    while ((r = hs_file_read(in, buf, sizeof buf)) > 0) hs_file_write(out, buf, (uint32_t)r);
    hs_file_close(in);
    hs_file_close(out);
    return TRUE;
}

HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
    std::string pat = pattern ? pattern : "*";
    size_t slash = pat.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? "." : pat.substr(0, slash);
    std::string mask = slash == std::string::npos ? pat : pat.substr(slash + 1);
    if (mask == "*.*") mask = "*";

    HsFind* f = new HsFind;
    struct Ctx { HsFind* f; const std::string* mask; } ctx{f, &mask};
    auto add = [](void* c, const char* name, int isDir, int64_t size) {
        Ctx* x = (Ctx*)c;
        if (wild_match(x->mask->c_str(), name)) x->f->entries.push_back({name, isDir != 0, size});
    };
    // The game folder is an overlay: list both halves, writable copy first.
    std::string writable = hs_win_to_target(dir.c_str(), true);
    std::string readable = hs_win_to_target(dir.c_str(), false);
    hs_dir_list(writable.c_str(), add, &ctx);
    if (readable != writable) hs_dir_list(readable.c_str(), add, &ctx);
    if (f->entries.empty()) {
        delete f;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    fill_find_data(f->entries[0], data);
    f->next = 1;
    return hs_handle_new(f);
}

BOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
    HsFind* f = hs_handle_as<HsFind>(h);
    if (!f) return FALSE;
    if (f->next >= f->entries.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    fill_find_data(f->entries[f->next++], data);
    return TRUE;
}

BOOL WINAPI FindClose(HANDLE h) {
    if (!hs_handle_as<HsFind>(h)) return FALSE;
    return CloseHandle(h);
}

DWORD WINAPI GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n) {
    static const char kExe[] = "C:\\Diablo II\\Game.exe";
    if (!buf || !n) return 0;
    DWORD len = (DWORD)sizeof kExe - 1;
    DWORD c = len < n ? len : n - 1;
    std::memcpy(buf, kExe, c);
    buf[c] = 0;
    return c;
}

DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD n, LPSTR buf, LPSTR* filePart) {
    char full[MAX_PATH];
    if (!_fullpath(full, name, sizeof full)) return 0;
    DWORD len = (DWORD)std::strlen(full);
    if (!buf || n <= len) return len + 1;
    std::memcpy(buf, full, len + 1);
    if (filePart) {
        char* s = std::strrchr(buf, '\\');
        *filePart = s ? s + 1 : buf;
    }
    return len;
}
