#pragma once
#include "hal/vdec.hpp"
#include "hal/i420_overlay.hpp"
#include <memory>

namespace display {
// Presentation contract for a board with a single scanout plane. The LVGL
// frontend publishes immutable overlays; the decoder publishes I420 pictures.
// Their threads never draw into a buffer that the display is scanning out.
class SoftwareScreen : public hal::FrameSink {
  public:
    virtual void overlay(std::shared_ptr<const hal::I420Overlay>) = 0;
    virtual void run() = 0;
    virtual uint64_t presented() const = 0;
    virtual bool failed() const = 0;
};
} // namespace display
