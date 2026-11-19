// The ports draw through D2Gdi alone (win32/gdi.cpp presents its frames), yet
// D2GFX_Initialize's renderer selector table names DirectDraw's, Glide's and
// Direct3D's function tables, and that one reference kept all three renderers
// in the link (about 120 KiB of code and data on the EE). cmake/port.cmake
// renames those references in libD2gfx.a to the empty tables here, which the
// game never selects (the video mode is always the windowed GDI one), so
// --gc-sections drops the renderers.
#include "Source/D2gfx/D2gfx.public.h"

extern "C" {
D2RendererFunctionsStrc hs_dropped_ddraw = {};
D2RendererFunctionsStrc hs_dropped_glide = {};
D2RendererFunctionsStrc hs_dropped_d3d = {};
}
