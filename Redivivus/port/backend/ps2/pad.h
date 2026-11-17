#pragma once

#include "Redivivus/port/input/input.h"

void hs_ps2_pad_init();                         // once, before the first read
void hs_ps2_pad_read(hs::input::PadState* out);  // disconnected -> connected = false
