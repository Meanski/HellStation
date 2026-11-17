// Controller layer 0: the pad as a mouse plus D2's own hotkeys
// (docs/port/controller.md). Until layer 1 exists both sticks drive the cursor:
// the left one coarse, the right one fine.
#include "Redivivus/port/input/input.h"

#include <cmath>

namespace hs::input {
namespace {

// Radial dead zone, then a squared response curve: precise near the centre, fast at the rim.
float response(int x, int y, float* dx, float* dy) {
    constexpr float kInner = 28.f, kOuter = 120.f;
    float mag = std::sqrt((float)(x * x + y * y));
    if (mag <= kInner) { *dx = *dy = 0; return 0; }
    float t = (mag - kInner) / (kOuter - kInner);
    if (t > 1) t = 1;
    *dx = x / mag;
    *dy = y / mag;
    return t * t;
}

struct Binding { uint32_t button; int vk; };
// Keys D2 already binds by default; nothing here is game-state aware.
constexpr Binding kKeys[] = {
    {kPadStart, 0x1B},     // Esc: game menu / back
    {kPadTriangle, 'I'},   // inventory
    {kPadSquare, 'S'},     // skill tree
    {kPadSelect, 0x09},    // Tab: automap
    {kPadL1, 0x10},        // Shift: stand still
    {kPadR1, 'C'},         // character sheet
    {kPadL3, 'R'},         // toggle run/walk
    {kPadR3, 0x12},        // Alt: show items (held)
    {kPadUp, '1'}, {kPadRight, '2'}, {kPadDown, '3'}, {kPadLeft, '4'},  // belt
};

}  // namespace

void VirtualMouse::push(const hs_event& e) {
    if (tail_ - head_ >= kQueue) return;  // a stalled consumer drops input rather than growing
    queue_[tail_++ % kQueue] = e;
}

bool VirtualMouse::next(hs_event* ev) {
    if (head_ == tail_) return false;
    *ev = queue_[head_++ % kQueue];
    return true;
}

void VirtualMouse::update(const PadState& pad, int* x, int* y, int frameW, int frameH) {
    uint32_t held = pad.connected ? pad.buttons : 0;
    if (fx_ < 0) { fx_ = (float)*x; fy_ = (float)*y; }

    // Cursor speed in frame pixels per presented frame (25-30 fps on the EE).
    float dx, dy, ox = 0, oy = 0;
    if (pad.connected) {
        float s = response(pad.lx, pad.ly, &dx, &dy) * 22.f;
        ox += dx * s; oy += dy * s;
        s = response(pad.rx, pad.ry, &dx, &dy) * 6.f;
        ox += dx * s; oy += dy * s;
    }
    hs_event e{};
    if (ox != 0 || oy != 0) {
        fx_ += ox; fy_ += oy;
        if (fx_ < 0) fx_ = 0;
        if (fy_ < 0) fy_ = 0;
        if (fx_ > frameW - 1) fx_ = (float)(frameW - 1);
        if (fy_ > frameH - 1) fy_ = (float)(frameH - 1);
        int nx = (int)fx_, ny = (int)fy_;
        if (nx != *x || ny != *y) {
            *x = nx; *y = ny;
            e.type = HS_EV_MOUSE_MOVE;
            e.x = nx; e.y = ny;
            push(e);
        }
    } else {
        fx_ = (float)*x; fy_ = (float)*y;  // follow warps by the game
    }

    const uint32_t was = prev_, changed = held ^ was;
    prev_ = held;
    if (!changed) return;
    auto button = [&](uint32_t mask, int which) {
        // Two pad buttons share each mouse button: report only the first press / last release.
        if (!(was & mask) == !(held & mask)) return;
        e = {};
        e.type = HS_EV_MOUSE_BUTTON;
        e.x = *x; e.y = *y;
        e.button = which;
        e.down = (held & mask) != 0;
        push(e);
    };
    button(kPadCross | kPadR2, HS_BUTTON_LEFT);
    button(kPadCircle | kPadL2, HS_BUTTON_RIGHT);
    for (const Binding& b : kKeys) {
        if (!(changed & b.button)) continue;
        e = {};
        e.type = HS_EV_KEY;
        e.x = *x; e.y = *y;
        e.vk = b.vk;
        e.scancode = set1_from_vk(b.vk);
        e.down = (held & b.button) != 0;
        push(e);
    }
}

}  // namespace hs::input
