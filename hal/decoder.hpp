#pragma once
// HAL: video decoders. The live decoder behind the Vdec interface, and the
// hardware decoder the menu's background clip plays through.

#include <functional>
#include <memory>
#include <string>

#include "common.hpp"      // VideoCodec
#include "kestrel_gnd_features.h"
#include "vdec/vdec.hpp"

class Renderer;
class DrmDevice;
struct DecodedUnit;

namespace hal {

// Which decoder a --decoder-name / decoder_name setting ends up with in this
// build. "rkmpp" is the Rockchip hardware decoder where this build has it;
// any other name, or rkmpp without it, goes to ffmpeg under that name.
enum class DecoderBackend { Rockchip, Ffmpeg };
inline DecoderBackend decoder_backend(const std::string &name) {
    return KESTREL_HAL_ROCKCHIP && name == "rkmpp" ? DecoderBackend::Rockchip : DecoderBackend::Ffmpeg;
}

std::shared_ptr<Vdec> make_decoder(VideoCodec codec, const std::string &name,
                                   std::shared_ptr<Renderer> renderer,
                                   std::shared_ptr<DrmDevice> dev, volatile bool *signal_stop);

// The background clip's decoder: hardware, no renderer, each picture to `sink`
// as a DMA-BUF. False / nullptr where this build has none; the menu then
// shows its still background.
inline bool has_background_decoder() { return KESTREL_HAL_ROCKCHIP; }
std::unique_ptr<Vdec> make_background_decoder(VideoCodec codec, std::shared_ptr<DrmDevice> dev,
                                              volatile bool *signal_stop,
                                              std::function<void(std::shared_ptr<DecodedUnit>)> sink);

}  // namespace hal
