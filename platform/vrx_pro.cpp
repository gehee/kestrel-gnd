// The VRX Pro (RK3568): KMS on its VOP2 and GLES on its Mali (platform/kms_gles),
// MPP's decoder and screen encoder, RGA's compositor.
#include "hal/ffmpeg/vdec_ffmpeg.hpp"
#include "hal/rockchip/mpp_encoder.hpp"
#include "hal/rockchip/rga_compositor.hpp"
#include "hal/rockchip/rga_strips.hpp"
#include "hal/rockchip/vdec_rk.hpp"
#include "kestrel_gnd_features.h"
#include "platform/box.hpp"
#include "platform/platform.hpp"

namespace platform {

const char* name() { return "vrxpro"; }

bool has_refresh_sync() { return true; }   // the VOP2's frame length

bool hardware_decoder(const std::string& name) { return name == "rkmpp"; }

std::shared_ptr<Vdec> make_decoder(VideoCodec codec, const std::string& name, std::shared_ptr<hal::FrameSink> sink,
                                   std::shared_ptr<display::Display> display, std::atomic<bool>* stop) {
    if (hardware_decoder(name)) return std::make_shared<VdecRK>(codec, sink, display, stop);
    return std::make_shared<VdecFfmpeg>(codec, name, sink, stop);
}

bool has_background_decoder() { return true; }

std::unique_ptr<Vdec> make_background_decoder(VideoCodec codec, std::shared_ptr<hal::Scanout> scanout,
                                              std::atomic<bool>* stop,
                                              std::function<void(std::shared_ptr<DecodedUnit>)> sink) {
    std::unique_ptr<VdecRK> v(new VdecRK(codec, nullptr, scanout, stop));
    v->set_output_timeout(100);   // ms, so run_frame can notice a stop
    v->set_frame_sink(std::move(sink));
    return v;
}

bool has_strip_joiner() { return RgaStripJoiner::available(); }

std::unique_ptr<hal::StripJoiner> make_strip_joiner(std::shared_ptr<hal::FrameSink> out,
                                                    std::shared_ptr<hal::Scanout> scanout) {
    if (!RgaStripJoiner::available() || !out || !scanout) return nullptr;
    return std::unique_ptr<hal::StripJoiner>(new RgaStripJoiner(std::move(out), std::move(scanout)));
}

bool screen_compositor_available() { return RgaCompositor::available(); }

std::unique_ptr<hal::ScreenEncoder> make_screen_encoder() {
#if KESTREL_SCREEN_ENC
    return std::unique_ptr<hal::ScreenEncoder>(new MppH264Encoder());
#else
    return std::unique_ptr<hal::ScreenEncoder>(new hal::NoScreenEncoder());
#endif
}

std::unique_ptr<hal::ScreenCompositor> make_screen_compositor() {
#if KESTREL_SCREEN_ENC
    return std::unique_ptr<hal::ScreenCompositor>(new RgaCompositor());
#else
    return std::unique_ptr<hal::ScreenCompositor>(new hal::NoScreenCompositor());
#endif
}

}  // namespace platform
