#pragma once
// HAL: the display controller's frame length, for DISPLAY > Refresh Sync.
// The phase lock (display_lock.cpp) moves the vblank by making frames a few
// lines longer or shorter; this is the register that does it, on the
// display controllers that let it be written mid-frame.

#include <cstdint>
#include <memory>

namespace hal {

class FrameLength {
public:
    // What the controller holds now.
    struct Seen {
        uint32_t value = 0;   // its raw value, for the log
        int total = 0;        // the frame length in lines
        bool held = false;    // an immediate write is still in force
    };

    virtual ~FrameLength() = default;
    // Takes the controller's value as the current mode's (`vtotal` lines,
    // `sync_lines` of sync); a total a little off is ours, left by a run that
    // did not get to put it back. False if it is not that mode's: the lock
    // then stays off.
    virtual bool take_mode(int vtotal, int sync_lines, Seen *seen) = 0;
    // The frame being scanned out is `total` lines long, from now on.
    virtual void set_total(int total) = 0;
    // Back to the mode's own value, applied at a frame boundary as the
    // driver's own writes are.
    virtual void restore() = 0;
};

// The frame-length register of CRTC `crtc_index`, or nullptr where this
// build or this unit has none (logged once).
std::unique_ptr<FrameLength> map_frame_length(int crtc_index);

}  // namespace hal
