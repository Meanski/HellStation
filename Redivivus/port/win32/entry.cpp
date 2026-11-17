// Process entry for port builds: bring up the Windows-shaped process state the
// game expects (a TEB for the main thread, the command line), then call the
// game's own WinMain.
#include <windows.h>
#include <string>
#include "Redivivus/port/platform.h"
#include "Redivivus/port/win32/teb.h"

namespace {
std::string g_commandLine;
}

extern "C" LPSTR WINAPI GetCommandLineA(void) { return (LPSTR)g_commandLine.c_str(); }

int hs_game_main(int argc, char** argv) {
    hs_teb_attach_current_thread();

    // WinMain's lpCmdLine excludes the program name; GetCommandLineA includes it.
    std::string args;
    for (int i = 1; i < argc; i++) {
        if (i > 1) args += ' ';
        args += argv[i];
    }
    g_commandLine = std::string("d2.exe") + (args.empty() ? "" : " ") + args;

    hs_log("HellStation: WinMain(\"%s\")", args.c_str());
    int rc = WinMain(GetModuleHandleA(nullptr), nullptr, (LPSTR)args.c_str(), SW_SHOWNORMAL);
    hs_log("HellStation: WinMain returned %d", rc);
    return rc;
}
