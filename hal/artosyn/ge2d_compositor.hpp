#pragma once
#include "hal/artosyn/ar9311_ge2d.h"
#include <vector>

namespace hal {
// Non-owning DMA-BUF views. The caller keeps every fd and allocation alive,
// keeps input contents stable, and owns the output until compose() returns.
struct Ge2dOverlay {
    ar_ge2d_image image{};
    int x = 0, y = 0;
};

// Clip moving HUD panels at the picture edges, retaining their source origin.
// Exposed separately so layout/cropping can be tested without receiver hardware.
ar_ge2d_compose ge2d_request(const ar_ge2d_image &video, const ar_ge2d_image &output,
                             const std::vector<Ge2dOverlay> &overlays);

class Ge2dCompositor {
public:
    explicit Ge2dCompositor(const char *device = "/dev/ar9311-ge2d");
    ~Ge2dCompositor();
    Ge2dCompositor(const Ge2dCompositor &) = delete;
    Ge2dCompositor &operator=(const Ge2dCompositor &) = delete;
    // Blocking hardware composition. Throws std::system_error on device errors.
    // An unsuccessful output must never be queued to the encoder.
    void compose(const ar_ge2d_image &video, const ar_ge2d_image &output,
                 const std::vector<Ge2dOverlay> &overlays);
private:
    int fd_ = -1;
};
} // namespace hal
