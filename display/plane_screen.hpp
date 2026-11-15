#pragma once
#include "hal/vdec.hpp"
#include "display/ascent_mode.hpp"
#include "base/damage.hpp"
#include <cstdint>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>

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
    // Picture Size: the UI drawn smaller than the screen, its frames (w x h,
    // what overlay() takes from then on) at x, y. w 0: the whole screen.
    struct UiInset {
        int x = 0, y = 0, w = 0, h = 0;
        bool operator==(const UiInset& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    };
    virtual bool supports_ui_inset() const { return false; }
    virtual void set_ui_inset(const UiInset&) {}
    // What shows around a smaller UI: art (the whole screen's ARGB8888,
    // screen-sized) under dim_argb (its alpha in the top byte, 0 none), or
    // nothing (transparent: the video plane) without art. Kept for as long
    // as it shows.
    virtual void set_ui_margin(std::shared_ptr<const std::vector<uint8_t>> /*art*/, uint32_t /*dim_argb*/) {}
    // Recorded playback may intentionally stop feeding while paused. Keep its
    // last picture until an explicit decoder flush; live reception times out.
    virtual void hold_video(bool) {}
    // Which air unit the pictures come from (its radio MAC, hex): a screen
    // that follows the camera keeps each one's camera rate.
    virtual void set_source(const std::string&) {}
    struct VideoStats {
        uint64_t stamp_us = 0;
        int width = 0, height = 0, fps = 0;
        float latency_ms = -1;
        uint64_t pts_us = 0; // timestamp of the last completed video flip
        // The last decoded picture this pipeline could not scan out (its size
        // or format): dropped, not shown, not a display failure.
        uint64_t rejected_us = 0;
        int rejected_width = 0, rejected_height = 0;
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
