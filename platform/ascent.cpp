#include "platform/software.hpp"
#include "display/ar9311_screen.hpp"
#include "hal/v4l2/vdec_v4l2.hpp"
namespace platform {
std::shared_ptr<Vdec> make_software_decoder(const SoftwareSetup& config,VideoCodec codec,std::shared_ptr<hal::FrameSink> sink,volatile bool* stop) {
    return std::make_shared<VdecV4l2>(codec,config.decoder_device,std::move(sink),stop);
}
SoftwareReceiver make_software_receiver(const SoftwareSetup &config, VideoCodec codec, volatile bool *stop) {
    SoftwareReceiver r;
    r.screen = display::make_ar9311_screen(config.display_device, config.force_display, stop);
    r.decoder = make_software_decoder(config,codec,r.screen,stop);
    return r;
}
} // namespace platform
