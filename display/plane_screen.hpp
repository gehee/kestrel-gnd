#pragma once
#include "hal/vdec.hpp"
#include "display/ascent_mode.hpp"
#include "base/damage.hpp"
#include <memory>
#include <stdexcept>

namespace display {
class ScreenError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
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
    struct VideoStats {
        uint64_t stamp_us = 0;
        int width = 0, height = 0, fps = 0;
        float latency_ms = -1;
    };
    virtual VideoStats video_stats() const { return {}; }
    virtual bool supports_mode(AscentMode mode) const { return mode == AscentMode{}; }
    virtual void run() = 0;
    virtual uint64_t presented() const = 0;
    // Completed flips which replaced the UI buffer, independently of video.
    virtual uint64_t overlay_presented() const { return 0; }
    virtual bool failed() const = 0;
};
} // namespace display
