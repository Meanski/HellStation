// Shim-internal window record (HWND points at one of these).
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct HsWindow {
    static constexpr uint32_t kMagic = 0x48535744;  // 'HSWD'
    uint32_t magic = kMagic;
    WNDPROC proc = nullptr;
    DWORD style = 0, exStyle = 0;
    HWND parent = nullptr;
    RECT rect{};
    LONG userData = 0;
    BOOL visible = FALSE;
    std::string title;
    std::vector<unsigned char> extra;  // cbWndExtra bytes
};

HsWindow* hs_window_from(HWND h);
HsWindow* hs_main_window();
