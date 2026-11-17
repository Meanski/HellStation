// Scripted input replay (format in input.h). Used by headless smoke runs on
// every backend: SDL reads HS_INPUT_SCRIPT, the PS2 reads host:input.txt.
#include "Redivivus/port/input/input.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hs::input {

int set1_from_vk(int vk) {
    static const char* const kRow = "QWERTYUIOP";
    for (int i = 0; kRow[i]; i++) if (vk == kRow[i]) return 0x10 + i;
    static const char* const kRow2 = "ASDFGHJKL";
    for (int i = 0; kRow2[i]; i++) if (vk == kRow2[i]) return 0x1E + i;
    static const char* const kRow3 = "ZXCVBNM";
    for (int i = 0; kRow3[i]; i++) if (vk == kRow3[i]) return 0x2C + i;
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);
    switch (vk) {
    case 0x1B: return 0x01; case 0x0D: return 0x1C; case 0x20: return 0x39; case 0x09: return 0x0F;
    case 0x08: return 0x0E; case 0x10: return 0x2A; case 0x11: return 0x1D; case 0x12: return 0x38;
    default: return 0;
    }
}

struct Script::Step {
    uint32_t frame;
    std::vector<hs_event> events;
};

void Script::load(const char* path) {
    if (!path) return;
    FILE* f = std::fopen(path, "r");
    if (!f) { hs_log("input script %s: none", path); return; }
    std::vector<Step> steps;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (char* hash = std::strchr(line, '#')) *hash = 0;
        unsigned frame;
        char cmd[16];
        int used = 0;
        if (std::sscanf(line, "%u %15s %n", &frame, cmd, &used) < 2) continue;
        const char* rest = line + used;
        Step step{frame, {}};
        hs_event e{};
        if (!std::strcmp(cmd, "move") || !std::strcmp(cmd, "click")) {
            char which[16] = "";
            int x = 0, y = 0;
            std::sscanf(rest, "%d %d %15s", &x, &y, which);
            e.type = HS_EV_MOUSE_MOVE; e.x = x; e.y = y;
            step.events.push_back(e);
            if (!std::strcmp(cmd, "click")) {
                e.type = HS_EV_MOUSE_BUTTON;
                e.button = !std::strcmp(which, "right") ? HS_BUTTON_RIGHT : HS_BUTTON_LEFT;
                e.down = 1; step.events.push_back(e);
                e.down = 0; step.events.push_back(e);
            }
        } else if (!std::strcmp(cmd, "key")) {
            e.type = HS_EV_KEY;
            e.vk = (int)std::strtol(rest, nullptr, 0);
            e.scancode = set1_from_vk(e.vk);
            e.down = 1; step.events.push_back(e);
            e.down = 0; step.events.push_back(e);
        } else if (!std::strcmp(cmd, "text")) {
            for (const char* c = rest; *c && *c != '\n' && *c != '\r'; ++c) {
                hs_event ch{};
                ch.type = HS_EV_CHAR; ch.ch = (unsigned char)*c;
                step.events.push_back(ch);
            }
        } else if (!std::strcmp(cmd, "quit")) {
            e.type = HS_EV_QUIT; step.events.push_back(e);
        } else {
            hs_log("input script: unknown command '%s'", cmd);
            continue;
        }
        steps.push_back(std::move(step));
    }
    std::fclose(f);
    count_ = (unsigned)steps.size();
    steps_ = new Step[count_ ? count_ : 1];
    for (unsigned i = 0; i < count_; i++) steps_[i] = std::move(steps[i]);
    hs_log("input script %s: %u steps", path, count_);
}

bool Script::next(uint32_t frame, hs_event* ev) {
    if (!pending_ && nextStep_ < count_ && frame >= steps_[nextStep_].frame) {
        hs_log("input script: frame %u step %u", (unsigned)frame, nextStep_);
        pendingStep_ = nextStep_++;
        pendingEvent_ = 0;
        pending_ = true;
    }
    if (!pending_) return false;
    const Step& s = steps_[pendingStep_];
    *ev = s.events[pendingEvent_++];
    if (pendingEvent_ >= s.events.size()) pending_ = false;
    return true;
}

}  // namespace hs::input
