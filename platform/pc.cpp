// A PC, or any box with KMS and a GPU and nothing else of its own: KMS and
// GLES (platform/kms_gles), ffmpeg's decoders, no background clip decoder and
// no screen encoder.
#include "hal/ffmpeg/vdec_ffmpeg.hpp"
#include "platform/box.hpp"
#include "platform/platform.hpp"

namespace platform {

const char* name() { return "pc"; }

bool has_refresh_sync() { return false; }

bool hardware_decoder(const std::string&) { return false; }

std::shared_ptr<Vdec> make_decoder(VideoCodec codec, const std::string& name, std::shared_ptr<hal::FrameSink> sink,
                                   std::shared_ptr<display::Display>, std::atomic<bool>* stop) {
    return std::make_shared<VdecFfmpeg>(codec, name, sink, stop);
}

bool has_background_decoder() { return false; }

std::unique_ptr<Vdec> make_background_decoder(VideoCodec, std::shared_ptr<hal::Scanout>, std::atomic<bool>*,
                                              std::function<void(std::shared_ptr<DecodedUnit>)>) {
    return nullptr;
}

bool has_strip_joiner() { return false; }

std::unique_ptr<hal::StripJoiner> make_strip_joiner(std::shared_ptr<hal::FrameSink>, std::shared_ptr<hal::Scanout>) {
    return nullptr;
}

bool screen_compositor_available() { return false; }

std::unique_ptr<hal::ScreenEncoder> make_screen_encoder() {
    return std::unique_ptr<hal::ScreenEncoder>(new hal::NoScreenEncoder());
}

std::unique_ptr<hal::ScreenCompositor> make_screen_compositor() {
    return std::unique_ptr<hal::ScreenCompositor>(new hal::NoScreenCompositor());
}

}  // namespace platform
