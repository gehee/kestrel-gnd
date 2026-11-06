#pragma once
#include "display/plane_screen.hpp"
#include "hal/vdec.hpp"
#include "base/video_codec.hpp"
#include <string>
namespace platform {
struct SoftwareSetup {
    std::string display_device = "/dev/dri/card0", decoder_device = "/dev/video0";
    bool force_display = false;
};
struct SoftwareReceiver {
    std::shared_ptr<display::PlaneScreen> screen;
    std::shared_ptr<Vdec> decoder;
};
SoftwareReceiver make_software_receiver(const SoftwareSetup &, VideoCodec, volatile bool *stop);
} // namespace platform
