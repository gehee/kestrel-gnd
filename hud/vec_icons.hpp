#pragma once

// Vector HUD icons.
//
// GLES2 can only sample a texture, so an icon has to become pixels somewhere.
// The question is only *when*: baking a 24x24 PNG into the binary fixes the
// raster at authoring time and leaves the HUD upsampling it, which is what
// made the old icons soft. These are drawn with Cairo at the pixel size the
// HUD is about to use, so they are exact at any ui_scale - the same trick the
// text path already uses for glyphs.
//
// Geometry is written in a 32-unit design space and each icon declares the
// box its ink actually occupies, so the renderer can hand back a tight
// texture plus its aspect ratio rather than padding everything to a square.

#include <GLES2/gl2.h>

enum class VecIcon {
    Drone,      // ring quad, top-down
    Goggles,    // twin-lens FPV goggle
    Distance,   // span marker, |<->|
};

// Rasterize (or return a cached) icon whose ink is `px_h` pixels tall.
// `*out_aspect` receives the ink's width/height so the caller can size the
// quad without distorting it. Returns 0 on failure.
GLuint vec_icon_texture(VecIcon which, int px_h, float* out_aspect);

// Drop every cached texture (GL context teardown).
void vec_icon_cache_clear();
