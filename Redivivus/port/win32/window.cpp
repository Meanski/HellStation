// Win32 windows and the message queue.
//
// One desktop, windows at the origin, client coordinates == screen
// coordinates == frame pixels. PeekMessage/GetMessage pump the backend and turn
// its events into the WM_* messages the game's window procedures expect.
#include <windows.h>
#include <windowsx.h>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/handles.h"
#include "Redivivus/port/win32/window.h"

namespace {

struct WndClass { WNDPROC proc; UINT style; int cbWndExtra; HINSTANCE inst; HCURSOR cursor; };
std::map<std::string, WndClass>& classes() { static std::map<std::string, WndClass> c; return c; }

HsLock g_queueLock;
std::deque<MSG>& queue() { static std::deque<MSG> q; return q; }
std::vector<HsWindow*>& windows() { static std::vector<HsWindow*> w; return w; }
HsWindow* g_focus;
HsWindow* g_capture;
int g_cursorShow = 0;  // ShowCursor counter (>= 0 shows)

struct Timer { HWND hwnd; UINT_PTR id; UINT ms; DWORD next; TIMERPROC proc; };
std::vector<Timer>& timers() { static std::vector<Timer> t; return t; }

std::string lower(const char* s) {
    std::string r = s ? s : "";
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

DWORD mouse_keys() {
    DWORD k = 0;
    if (hs_input_key_down(VK_LBUTTON)) k |= MK_LBUTTON;
    if (hs_input_key_down(VK_RBUTTON)) k |= MK_RBUTTON;
    if (hs_input_key_down(VK_MBUTTON)) k |= MK_MBUTTON;
    if (hs_input_key_down(VK_SHIFT)) k |= MK_SHIFT;
    if (hs_input_key_down(VK_CONTROL)) k |= MK_CONTROL;
    return k;
}

void post(HWND h, UINT m, WPARAM w, LPARAM l) {
    MSG msg{};
    msg.hwnd = h;
    msg.message = m;
    msg.wParam = w;
    msg.lParam = l;
    msg.time = GetTickCount();
    int x, y;
    hs_input_mouse_pos(&x, &y);
    msg.pt.x = x;
    msg.pt.y = y;
    HsLockGuard lock(g_queueLock);
    queue().push_back(msg);
}

HWND input_target() {
    if (g_capture) return (HWND)g_capture;
    if (g_focus) return (HWND)g_focus;
    return windows().empty() ? nullptr : (HWND)windows().front();
}

// Backend events -> window messages.
void pump() {
    hs_event ev;
    while (hs_input_poll(&ev)) {
        HWND h = input_target();
        LPARAM pos = MAKELPARAM(ev.x, ev.y);
        switch (ev.type) {
        case HS_EV_QUIT:
            if (h) post(h, WM_CLOSE, 0, 0);
            else PostQuitMessage(0);
            break;
        case HS_EV_MOUSE_MOVE:
            if (h) post(h, WM_MOUSEMOVE, mouse_keys(), pos);
            break;
        case HS_EV_MOUSE_BUTTON: {
            UINT m = ev.button == HS_BUTTON_LEFT ? (ev.down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
                   : ev.button == HS_BUTTON_RIGHT ? (ev.down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                   : (ev.down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
            if (h) post(h, m, mouse_keys(), pos);
            break;
        }
        case HS_EV_MOUSE_WHEEL:
            if (h) post(h, WM_MOUSEWHEEL, MAKEWPARAM(mouse_keys(), (WORD)(short)(ev.wheel * WHEEL_DELTA)), pos);
            break;
        case HS_EV_KEY: {
            bool alt = hs_input_key_down(VK_MENU);
            UINT m = ev.down ? ((alt || ev.vk == VK_MENU) ? WM_SYSKEYDOWN : WM_KEYDOWN)
                             : ((alt || ev.vk == VK_MENU) ? WM_SYSKEYUP : WM_KEYUP);
            LPARAM l = 1 | ((LPARAM)(ev.scancode & 0xFF) << 16) | (alt ? (1 << 29) : 0)
                     | (ev.repeat ? (1 << 30) : 0) | (ev.down ? 0 : (LPARAM)(3u << 30));
            if (h) post(h, m, (WPARAM)ev.vk, l);
            break;
        }
        case HS_EV_CHAR:
            if (h) post(h, WM_CHAR, (WPARAM)ev.ch, 1);
            break;
        case HS_EV_FOCUS:
            if (h) {
                post(h, WM_ACTIVATEAPP, ev.down, 0);
                post(h, WM_ACTIVATE, ev.down ? WA_ACTIVE : WA_INACTIVE, 0);
            }
            break;
        }
    }
    DWORD now = GetTickCount();
    for (Timer& t : timers()) {
        if ((int)(now - t.next) >= 0) {
            t.next = now + t.ms;
            post(t.hwnd, WM_TIMER, t.id, (LPARAM)t.proc);
        }
    }
}

bool matches(const MSG& m, HWND h, UINT lo, UINT hi) {
    if (h && m.hwnd != h) return false;
    if (lo == 0 && hi == 0) return true;
    return m.message >= lo && m.message <= hi;
}

}  // namespace

// Validated by membership, never by dereferencing: the game passes handles
// such as GetDesktopWindow() that aren't window records.
HsWindow* hs_window_from(HWND h) {
    for (HsWindow* w : windows()) if ((HWND)w == h) return w;
    return nullptr;
}

HsWindow* hs_main_window() { return windows().empty() ? nullptr : windows().front(); }

// ---- Classes and windows ---------------------------------------------------------

ATOM WINAPI RegisterClassA(const WNDCLASSA* wc) {
    classes()[lower(wc->lpszClassName)] = {wc->lpfnWndProc, wc->style, wc->cbWndExtra, wc->hInstance, wc->hCursor};
    return (ATOM)classes().size();
}
ATOM WINAPI RegisterClassExA(const WNDCLASSEXA* wc) {
    classes()[lower(wc->lpszClassName)] = {wc->lpfnWndProc, wc->style, wc->cbWndExtra, wc->hInstance, wc->hCursor};
    return (ATOM)classes().size();
}
BOOL WINAPI UnregisterClassA(LPCSTR name, HINSTANCE) { classes().erase(lower(name)); return TRUE; }

HWND WINAPI CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                            HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    auto it = classes().find(lower(cls));
    if (it == classes().end()) {
        hs_log("CreateWindowExA: unknown class \"%s\"", cls ? cls : "");
        SetLastError(ERROR_CANNOT_FIND_WND_CLASS);
        return nullptr;
    }
    HsWindow* win = new HsWindow;
    win->proc = it->second.proc;
    win->style = style;
    win->exStyle = exStyle;
    win->parent = parent;
    win->title = title ? title : "";
    win->extra.assign((size_t)(it->second.cbWndExtra > 0 ? it->second.cbWndExtra : 0), 0);
    win->rect = {x == (int)CW_USEDEFAULT ? 0 : x, y == (int)CW_USEDEFAULT ? 0 : y, 0, 0};
    win->rect.right = win->rect.left + (w == (int)CW_USEDEFAULT ? 640 : w);
    win->rect.bottom = win->rect.top + (h == (int)CW_USEDEFAULT ? 480 : h);
    windows().push_back(win);
    if (!g_focus && !parent) g_focus = win;
    hs_log("CreateWindowExA(\"%s\", \"%s\", %dx%d)", cls, win->title.c_str(), w, h);

    CREATESTRUCTA cs{};
    cs.lpCreateParams = param;
    cs.hInstance = inst;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.cy = h; cs.cx = w; cs.y = y; cs.x = x;
    cs.style = (LONG)style;
    cs.lpszName = title;
    cs.lpszClass = cls;
    cs.dwExStyle = exStyle;
    HWND hwnd = (HWND)win;
    if (!win->proc(hwnd, WM_NCCREATE, 0, (LPARAM)&cs)) { DestroyWindow(hwnd); return nullptr; }
    if (win->proc(hwnd, WM_CREATE, 0, (LPARAM)&cs) == -1) { DestroyWindow(hwnd); return nullptr; }
    return hwnd;
}

BOOL WINAPI DestroyWindow(HWND h) {
    HsWindow* w = hs_window_from(h);
    if (!w) return FALSE;
    w->proc(h, WM_DESTROY, 0, 0);
    w->proc(h, WM_NCDESTROY, 0, 0);
    for (size_t i = 0; i < windows().size(); i++) if (windows()[i] == w) windows().erase(windows().begin() + i);
    if (g_focus == w) g_focus = windows().empty() ? nullptr : windows().front();
    if (g_capture == w) g_capture = nullptr;
    w->magic = 0;  // leaked deliberately: late messages for it are dropped, not use-after-free
    return TRUE;
}

BOOL WINAPI IsWindow(HWND h) { return hs_window_from(h) != nullptr; }
BOOL WINAPI IsWindowVisible(HWND h) { HsWindow* w = hs_window_from(h); return w && w->visible; }
BOOL WINAPI IsIconic(HWND) { return FALSE; }
BOOL WINAPI IsZoomed(HWND) { return FALSE; }
BOOL WINAPI ShowWindow(HWND h, int cmd) {
    HsWindow* w = hs_window_from(h);
    if (!w) return FALSE;
    BOOL was = w->visible;
    w->visible = cmd != SW_HIDE;
    return was;
}
BOOL WINAPI UpdateWindow(HWND) { return TRUE; }
BOOL WINAPI InvalidateRect(HWND, const RECT*, BOOL) { return TRUE; }
BOOL WINAPI ValidateRect(HWND, const RECT*) { return TRUE; }

BOOL WINAPI SetWindowPos(HWND h, HWND, int x, int y, int cx, int cy, UINT flags) {
    HsWindow* w = hs_window_from(h);
    if (!w) return FALSE;
    if (!(flags & SWP_NOMOVE)) { int ow = w->rect.right - w->rect.left, oh = w->rect.bottom - w->rect.top; w->rect = {x, y, x + ow, y + oh}; }
    if (!(flags & SWP_NOSIZE)) { w->rect.right = w->rect.left + cx; w->rect.bottom = w->rect.top + cy; }
    if (flags & SWP_SHOWWINDOW) w->visible = TRUE;
    if (flags & SWP_HIDEWINDOW) w->visible = FALSE;
    return TRUE;
}
BOOL WINAPI MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL) { return SetWindowPos(h, nullptr, x, y, cx, cy, 0); }
BOOL WINAPI GetWindowRect(HWND h, LPRECT r) {
    HsWindow* w = hs_window_from(h);
    if (!w) { *r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; return h == nullptr || h == GetDesktopWindow(); }
    *r = w->rect;
    return TRUE;
}
BOOL WINAPI GetClientRect(HWND h, LPRECT r) {
    HsWindow* w = hs_window_from(h);
    if (!w) { *r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; return h == nullptr || h == GetDesktopWindow(); }
    *r = {0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    return TRUE;
}
BOOL WINAPI AdjustWindowRect(LPRECT, DWORD, BOOL) { return TRUE; }       // no decorations
BOOL WINAPI AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }
BOOL WINAPI ScreenToClient(HWND, LPPOINT) { return TRUE; }
BOOL WINAPI ClientToScreen(HWND, LPPOINT) { return TRUE; }
int WINAPI MapWindowPoints(HWND, HWND, LPPOINT, UINT) { return 0; }

LONG WINAPI GetWindowLongA(HWND h, int idx) {
    HsWindow* w = hs_window_from(h);
    if (!w) return 0;
    switch (idx) {
    case GWL_WNDPROC: return (LONG)(intptr_t)w->proc;
    case GWL_STYLE: return (LONG)w->style;
    case GWL_EXSTYLE: return (LONG)w->exStyle;
    case GWL_USERDATA: return w->userData;
    case GWL_HWNDPARENT: return (LONG)(intptr_t)w->parent;
    case GWL_ID: return 0;
    case GWL_HINSTANCE: return 0x00400000;
    default:
        if (idx >= 0 && (size_t)idx + 4 <= w->extra.size()) { LONG v; std::memcpy(&v, &w->extra[idx], 4); return v; }
        return 0;
    }
}
LONG WINAPI SetWindowLongA(HWND h, int idx, LONG v) {
    HsWindow* w = hs_window_from(h);
    if (!w) return 0;
    LONG old = GetWindowLongA(h, idx);
    switch (idx) {
    case GWL_WNDPROC: w->proc = (WNDPROC)(intptr_t)v; break;
    case GWL_STYLE: w->style = (DWORD)v; break;
    case GWL_EXSTYLE: w->exStyle = (DWORD)v; break;
    case GWL_USERDATA: w->userData = v; break;
    default:
        if (idx >= 0 && (size_t)idx + 4 <= w->extra.size()) std::memcpy(&w->extra[idx], &v, 4);
        break;
    }
    return old;
}
LRESULT WINAPI CallWindowProcA(WNDPROC proc, HWND h, UINT m, WPARAM w, LPARAM l) { return proc ? proc(h, m, w, l) : 0; }

BOOL WINAPI SetWindowTextA(HWND h, LPCSTR t) {
    HsWindow* w = hs_window_from(h);
    if (!w) return FALSE;
    w->title = t ? t : "";
    hs_video_set_title(w->title.c_str());
    return TRUE;
}
int WINAPI GetWindowTextA(HWND h, LPSTR buf, int n) {
    HsWindow* w = hs_window_from(h);
    if (!w || n <= 0) return 0;
    int c = (int)w->title.size() < n - 1 ? (int)w->title.size() : n - 1;
    std::memcpy(buf, w->title.c_str(), (size_t)c);
    buf[c] = 0;
    return c;
}

HWND WINAPI GetDesktopWindow(void) { return (HWND)(uintptr_t)0x10010; }
HWND WINAPI FindWindowA(LPCSTR, LPCSTR) { return nullptr; }
HWND WINAPI GetParent(HWND h) { HsWindow* w = hs_window_from(h); return w ? w->parent : nullptr; }
HWND WINAPI GetFocus(void) { return (HWND)g_focus; }
HWND WINAPI SetFocus(HWND h) { HWND old = (HWND)g_focus; if (hs_window_from(h)) g_focus = hs_window_from(h); return old; }
HWND WINAPI GetActiveWindow(void) { return (HWND)g_focus; }
HWND WINAPI SetActiveWindow(HWND h) { return SetFocus(h); }
HWND WINAPI GetForegroundWindow(void) { return (HWND)g_focus; }
BOOL WINAPI SetForegroundWindow(HWND h) { SetFocus(h); return TRUE; }
BOOL WINAPI BringWindowToTop(HWND) { return TRUE; }
HWND WINAPI SetCapture(HWND h) { HWND old = (HWND)g_capture; g_capture = hs_window_from(h); return old; }
BOOL WINAPI ReleaseCapture(void) { g_capture = nullptr; return TRUE; }
HWND WINAPI GetCapture(void) { return (HWND)g_capture; }
DWORD WINAPI GetWindowThreadProcessId(HWND, LPDWORD pid) { if (pid) *pid = GetCurrentProcessId(); return 1; }
BOOL WINAPI EnableWindow(HWND, BOOL) { return FALSE; }

int WINAPI GetSystemMetrics(int i) {
    switch (i) {
    case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN: return 1024;
    case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN: return 768;
    case SM_CMONITORS: return 1;
    case SM_MOUSEPRESENT: return 1;
    case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
    default: return 0;
    }
}
UINT WINAPI GetDoubleClickTime(void) { return 500; }

// ---- Messages ------------------------------------------------------------------------

BOOL WINAPI PeekMessageA(LPMSG msg, HWND h, UINT lo, UINT hi, UINT remove) {
    pump();
    HsLockGuard lock(g_queueLock);
    for (auto it = queue().begin(); it != queue().end(); ++it) {
        if (it->message == WM_QUIT || matches(*it, h, lo, hi)) {
            *msg = *it;
            if (remove & PM_REMOVE) queue().erase(it);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL WINAPI GetMessageA(LPMSG msg, HWND h, UINT lo, UINT hi) {
    while (!PeekMessageA(msg, h, lo, hi, PM_REMOVE)) hs_sleep_ms(1);
    return msg->message != WM_QUIT;
}

BOOL WINAPI WaitMessage(void) {
    for (;;) {
        pump();
        { HsLockGuard lock(g_queueLock); if (!queue().empty()) return TRUE; }
        hs_sleep_ms(1);
    }
}

BOOL WINAPI TranslateMessage(const MSG* m) {
    // Printable characters arrive from the backend as WM_CHAR already; the
    // control characters a text field needs come from their keys here.
    if (m->message != WM_KEYDOWN) return FALSE;
    switch (m->wParam) {
    case VK_RETURN: case VK_BACK: case VK_ESCAPE: case VK_TAB:
        post(m->hwnd, WM_CHAR, m->wParam, m->lParam);
        return TRUE;
    default:
        return FALSE;
    }
}

LRESULT WINAPI DispatchMessageA(const MSG* m) {
    if (m->message == WM_TIMER && m->lParam) {
        ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, GetTickCount());
        return 0;
    }
    HsWindow* w = hs_window_from(m->hwnd);
    return w ? w->proc(m->hwnd, m->message, m->wParam, m->lParam) : 0;
}

LRESULT WINAPI SendMessageA(HWND h, UINT m, WPARAM w, LPARAM l) {
    HsWindow* win = hs_window_from(h);
    return win ? win->proc(h, m, w, l) : 0;
}
BOOL WINAPI PostMessageA(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!h) { post(nullptr, m, w, l); return TRUE; }
    if (!hs_window_from(h)) return FALSE;
    post(h, m, w, l);
    return TRUE;
}
BOOL WINAPI PostThreadMessageA(DWORD, UINT m, WPARAM w, LPARAM l) { post(nullptr, m, w, l); return TRUE; }
void WINAPI PostQuitMessage(int code) { post(nullptr, WM_QUIT, (WPARAM)code, 0); }

LRESULT WINAPI DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_CLOSE) PostMessageA(h, WM_CLOSE, 0, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_SETCURSOR: return TRUE;
    case WM_ACTIVATEAPP: case WM_ACTIVATE: return 0;
    case WM_QUERYNEWPALETTE: case WM_PALETTECHANGED: return 0;
    case WM_GETMINMAXINFO: return 0;
    default: return 0;
    }
}

UINT_PTR WINAPI SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC proc) {
    for (Timer& t : timers()) if (t.hwnd == h && t.id == id) { t.ms = ms; t.next = GetTickCount() + ms; t.proc = proc; return id; }
    if (!h && !id) id = 0x1000 + timers().size();
    timers().push_back({h, id, ms, GetTickCount() + ms, proc});
    return id;
}
BOOL WINAPI KillTimer(HWND h, UINT_PTR id) {
    for (size_t i = 0; i < timers().size(); i++)
        if (timers()[i].hwnd == h && timers()[i].id == id) { timers().erase(timers().begin() + i); return TRUE; }
    return FALSE;
}

// ---- Keyboard and mouse state -------------------------------------------------------

SHORT WINAPI GetAsyncKeyState(int vk) { return hs_input_key_down(vk) ? (SHORT)0x8000 : 0; }
SHORT WINAPI GetKeyState(int vk) { return hs_input_key_down(vk) ? (SHORT)0x8000 : 0; }
BOOL WINAPI GetKeyboardState(PBYTE keys) {
    for (int i = 0; i < 256; i++) keys[i] = hs_input_key_down(i) ? 0x80 : 0;
    return TRUE;
}
UINT WINAPI MapVirtualKeyA(UINT code, UINT type) {
    if (type == MAPVK_VK_TO_CHAR) return (code >= '0' && code <= 'Z') ? code : 0;
    return code;
}
HKL WINAPI GetKeyboardLayout(DWORD) { return (HKL)(uintptr_t)0x04090409; }
int WINAPI ToAscii(UINT vk, UINT, const BYTE*, LPWORD out, UINT) {
    if (vk >= 'A' && vk <= 'Z') { *out = (WORD)(vk + 32); return 1; }
    if (vk >= '0' && vk <= '9' || vk == ' ') { *out = (WORD)vk; return 1; }
    return 0;
}

BOOL WINAPI GetCursorPos(LPPOINT p) {
    int x, y;
    hs_input_mouse_pos(&x, &y);
    p->x = x;
    p->y = y;
    return TRUE;
}
BOOL WINAPI SetCursorPos(int x, int y) { hs_input_warp_mouse(x, y); return TRUE; }
BOOL WINAPI ClipCursor(const RECT*) { return TRUE; }
BOOL WINAPI GetClipCursor(LPRECT r) { *r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; return TRUE; }
int WINAPI ShowCursor(BOOL show) {
    g_cursorShow += show ? 1 : -1;
    hs_input_show_cursor(g_cursorShow >= 0);
    return g_cursorShow;
}
HCURSOR WINAPI SetCursor(HCURSOR c) { return c; }
HCURSOR WINAPI LoadCursorA(HINSTANCE, LPCSTR) { return (HCURSOR)(uintptr_t)0x10020; }
HICON WINAPI LoadIconA(HINSTANCE, LPCSTR) { return (HICON)(uintptr_t)0x10030; }

// ---- Dialogs ---------------------------------------------------------------------------

int WINAPI MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    hs_log("MessageBox [%s]: %s", caption ? caption : "", text ? text : "");
    switch (type & 0xF) {
    case MB_YESNO: case MB_YESNOCANCEL: return IDYES;
    case MB_RETRYCANCEL: return IDCANCEL;
    case MB_ABORTRETRYIGNORE: return IDIGNORE;
    default: return IDOK;
    }
}
