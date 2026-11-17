#include "Redivivus/port/win32/paths.h"
#include "Redivivus/port/platform.h"
#include <windows.h>
#include <cctype>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_cwdLock;
std::string g_cwd = "C:\\Diablo II";

// Collapse a Windows path to an absolute, backslash-separated, drive-qualified form.
std::string absolute_win(const char* in) {
    std::string p(in ? in : "");
    for (char& c : p) if (c == '/') c = '\\';
    std::string abs;
    if (p.size() >= 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':') {
        abs = "C:" + p.substr(2);                       // every drive is C:
        if (abs.size() == 2 || abs[2] != '\\') abs.insert(2, "\\");
    } else if (!p.empty() && p[0] == '\\') {
        abs = "C:" + p;
    } else {
        std::lock_guard<std::mutex> l(g_cwdLock);
        abs = g_cwd + "\\" + p;
    }
    // Normalise . and .. and repeated separators.
    std::string out = "C:";
    size_t i = 2;
    std::string parts[256];
    int n = 0;
    while (i < abs.size()) {
        while (i < abs.size() && abs[i] == '\\') i++;
        size_t j = abs.find('\\', i);
        if (j == std::string::npos) j = abs.size();
        std::string c = abs.substr(i, j - i);
        i = j;
        if (c.empty() || c == ".") continue;
        if (c == "..") { if (n) n--; continue; }
        if (n < 256) parts[n++] = c;
    }
    for (int k = 0; k < n; k++) out += "\\" + parts[k];
    if (n == 0) out += "\\";
    return out;
}

bool exists(const std::string& target) {
    int isDir = 0;
    return hs_file_stat(target.c_str(), &isDir, nullptr, nullptr) == 0;
}
}  // namespace

const char* hs_game_dir_win() { return "C:\\Diablo II"; }

std::string hs_win_to_target(const char* winPath, bool forWrite) {
    std::string abs = absolute_win(winPath);
    std::string rel = abs.substr(3);  // after "C:\"
    for (char& c : rel) if (c == '\\') c = '/';

    static const std::string kGame = "diablo ii";
    std::string lower = rel;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower == kGame || lower.compare(0, kGame.size() + 1, kGame + "/") == 0) {
        std::string tail = rel.size() > kGame.size() ? rel.substr(kGame.size() + 1) : "";
        std::string writable = std::string(hs_game_dir()) + (tail.empty() ? "" : "/" + tail);
        if (forWrite || exists(writable)) return writable;
        std::string data = std::string(hs_data_dir()) + (tail.empty() ? "" : "/" + tail);
        if (exists(data)) return data;
        return writable;
    }
    return std::string(hs_root_dir()) + (rel.empty() ? "" : "/" + rel);
}

DWORD WINAPI GetCurrentDirectoryA(DWORD n, LPSTR buf) {
    std::lock_guard<std::mutex> l(g_cwdLock);
    if (!buf || n <= g_cwd.size()) return (DWORD)g_cwd.size() + 1;
    std::memcpy(buf, g_cwd.c_str(), g_cwd.size() + 1);
    return (DWORD)g_cwd.size();
}

BOOL WINAPI SetCurrentDirectoryA(LPCSTR path) {
    std::string abs = absolute_win(path);
    int isDir = 0;
    if (hs_file_stat(hs_win_to_target(abs.c_str(), false).c_str(), &isDir, nullptr, nullptr) != 0 || !isDir) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    std::lock_guard<std::mutex> l(g_cwdLock);
    g_cwd = abs;
    return TRUE;
}
