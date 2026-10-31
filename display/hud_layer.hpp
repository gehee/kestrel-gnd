#pragma once
// The HUD's layer on the display: the buffers the HUD draws its frames into,
// and how each frame reaches the screen. The platform makes it for its display
// and its way of drawing (platform/gles_hud_layer: a GBM surface the GPU draws
// into, each buffer a framebuffer on the display's HUD plane). On the HUD's
// thread only.
#include "display/display.hpp"

namespace display {

class HudLayer {
public:
    virtual ~HudLayer() = default;
    // Before the first frame, on the HUD's thread: its buffers, at the
    // screen's size. False (and logged) if they cannot be made.
    virtual bool init() = 0;
    // A frame starts: the buffers the display has let go of are reused, and
    // the drawing goes to the next one.
    virtual void begin() = 0;
    // The frame drawn since begin(), to the display once it is drawn, with the
    // picture's rectangle that goes with it (a gallery tile, or none).
    virtual void present(const Tile& tile) = 0;
};

}  // namespace display
