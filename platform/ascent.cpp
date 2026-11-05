#include "platform/software.hpp"
#include "display/ar9311_screen.hpp"
#include "hal/v4l2/vdec_v4l2.hpp"
namespace platform {
SoftwareReceiver make_software_receiver(const SoftwareSetup &config, VideoCodec codec, volatile bool *stop) {
    SoftwareReceiver r;
    r.screen = display::make_ar9311_screen(config.display_device, config.force_display, stop);
    r.decoder = std::make_shared<VdecV4l2>(codec, config.decoder_device, r.screen, stop);
    return r;
}
} // namespace platform
