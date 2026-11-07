#pragma once
#include "display/plane_screen.hpp"
#include "hal/vdec.hpp"
#include "base/video_codec.hpp"
#include <string>
namespace platform {
struct SoftwareSetup {
    std::string display_device = "/dev/dri/card0", decoder_device = "/dev/video0";
    int display_width=1920, display_height=1080, display_hz=60;
    bool force_display = false;
};
struct SoftwareReceiver {
    std::shared_ptr<display::PlaneScreen> screen;
    std::shared_ptr<Vdec> decoder;
};
SoftwareReceiver make_software_receiver(const SoftwareSetup &, VideoCodec, std::atomic<bool> *stop);
std::shared_ptr<Vdec> make_software_decoder(const SoftwareSetup&,VideoCodec,std::shared_ptr<hal::FrameSink>,std::atomic<bool>* stop);
} // namespace platform
