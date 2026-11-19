// Platform-neutral input: scripted replay and the controller's virtual mouse.
//
// Backends feed these from their own devices and drain hs_events out of them;
// nothing here knows about SDL or ps2sdk. See docs/port/controller.md.
#pragma once

#include <cstdint>

#include "Redivivus/port/platform.h"

namespace hs::input {

// PC set-1 scancode for a Windows virtual key (the lParam of WM_KEY*), 0 if unmapped.
int set1_from_vk(int vk);

// ---- Scripted input -------------------------------------------------------------
// One step per line, keyed to the presented-frame count so a replay doesn't
// depend on host speed:
//   <frame> move <x> <y>          mouse to frame coordinates
//   <frame> click <x> <y> [right] move, press, release
//   <frame> key <vk>              press and release a Windows virtual key (hex or decimal)
//   <frame> text <characters...>  typed characters (rest of the line)
//   <frame> quit                  close the window
//   <frame> where                 log the objects near the player and where to click them
// '#' starts a comment. A step's events come out one per call.
//
// Live input: poll_live() reads the lines appended to a file since its last
// call, so a running game can be driven a step at a time from the host. There a
// frame written "+N" means N frames after the line is read.
class Script {
public:
    // Reads the file once; a missing file means no script (logged).
    void load(const char* path);
    // Appends the complete lines added to path since the last call.
    void poll_live(const char* path, uint32_t frame);
    // The next due event, if any. Mouse events carry absolute coordinates; for
    // others x/y are left for the caller to fill.
    bool next(uint32_t frame, hs_event* ev);

private:
    struct Step;
    bool parse(const char* line, uint32_t now, Step* out);
    void add(Step&& step);
    struct Steps;
    Steps* steps_ = nullptr;
    unsigned nextStep_ = 0, pendingStep_ = 0, pendingEvent_ = 0;
    long liveOffset_ = 0;
    bool pending_ = false;
};

// ---- Pad -> virtual mouse (controller layer 0) ----------------------------------
enum PadButton : uint32_t {
    kPadUp = 1u << 0, kPadDown = 1u << 1, kPadLeft = 1u << 2, kPadRight = 1u << 3,
    kPadCross = 1u << 4, kPadCircle = 1u << 5, kPadSquare = 1u << 6, kPadTriangle = 1u << 7,
    kPadL1 = 1u << 8, kPadR1 = 1u << 9, kPadL2 = 1u << 10, kPadR2 = 1u << 11,
    kPadL3 = 1u << 12, kPadR3 = 1u << 13, kPadStart = 1u << 14, kPadSelect = 1u << 15,
};

struct PadState {
    bool connected = false;
    uint32_t buttons = 0;          // PadButton bits, 1 = held
    int lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, -128..127, +x right, +y down
};

// Turns one pad's state, sampled once per presented frame, into mouse and key
// events. Needs no game knowledge, so it works on every screen.
class VirtualMouse {
public:
    // Sample the pad; queues the resulting events. (x, y) is the cursor in
    // frame coordinates and is moved in place.
    void update(const PadState& pad, int* x, int* y, int frameW, int frameH);
    bool next(hs_event* ev);

private:
    void push(const hs_event& e);
    static constexpr unsigned kQueue = 64;
    hs_event queue_[kQueue] = {};
    unsigned head_ = 0, tail_ = 0;
    uint32_t prev_ = 0;
    float fx_ = -1, fy_ = -1;  // sub-pixel cursor
};

}  // namespace hs::input
