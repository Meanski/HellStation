// Windows path -> target path. The game sees a C: drive whose root is
// hs_root_dir(), and its own folder C:\Diablo II, which overlays the writable
// copy (hs_game_dir()) on the read-only game data (hs_data_dir()).
#pragma once
#include <string>

std::string hs_win_to_target(const char* winPath, bool forWrite);
const char* hs_game_dir_win();        // "C:\\Diablo II"
