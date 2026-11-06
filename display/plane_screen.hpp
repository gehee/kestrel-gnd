#pragma once
#include "hal/vdec.hpp"
#include "base/damage.hpp"
#include <memory>

namespace display {
// Video DMA-BUF and ARGB UI have independent producers. Hardware blends them
// at scanout; the presenter retains each buffer until flip completion.
class PlaneScreen : public hal::FrameSink {
  public:
    // Copies into a free UI scanout buffer on the caller's thread. Never waits
    // for video or a vblank; a busy ring drops the update rather than video.
    virtual void overlay(const uint32_t *argb, unsigned width, unsigned height) = 0;
    virtual void overlay_damage(const uint32_t *argb, unsigned width, unsigned height,
                                const std::vector<base::Damage>&) { overlay(argb, width, height); }
    // Recorded playback may intentionally stop feeding while paused. Keep its
    // last picture until an explicit decoder flush; live reception times out.
    virtual void hold_video(bool) {}
    virtual void run() = 0;
    virtual uint64_t presented() const = 0;
    virtual bool failed() const = 0;
};
} // namespace display
