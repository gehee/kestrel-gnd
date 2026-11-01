#pragma once

// Vector HUD icons.
//
// The canvas draws images, so an icon has to become pixels somewhere.
// The question is only *when*: baking a 24x24 PNG into the binary fixes the
// raster at authoring time and leaves the HUD upsampling it, which is what
// made the old icons soft. These are drawn with Cairo at the pixel size the
// HUD is about to use, so they are exact at any ui_scale - the same trick the
// text path already uses for glyphs.
//
// Geometry is written in a 32-unit design space and each icon declares the
// box its ink actually occupies, so the renderer can hand back a tight
// texture plus its aspect ratio rather than padding everything to a square.

#include "render/canvas.hpp"

enum class VecIcon {
    Drone,      // ring quad, top-down
    Goggles,    // twin-lens FPV goggle
    Distance,   // span marker, |<->|
};

// Rasterize (or return a cached) icon whose ink is `px_h` pixels tall.
// `*out_aspect` receives the ink's width/height so the caller can size the
// quad without distorting it. An image of `canvas`'s; 0 on failure.
render::ImageId vec_icon_texture(render::Canvas& canvas, VecIcon which, int px_h, float* out_aspect);

// Drop every cached image (the canvas going away).
void vec_icon_cache_clear(render::Canvas& canvas);
