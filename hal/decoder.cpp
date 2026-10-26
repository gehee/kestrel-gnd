#include "hal/decoder.hpp"

#include "kestrel_gnd_features.h"
#include "vdec/vdec_ffmpeg.hpp"
#if KESTREL_HAL_ROCKCHIP
#include "hal/rockchip/vdec_rk.hpp"
#endif

namespace hal {

std::shared_ptr<Vdec> make_decoder(VideoCodec codec, const std::string &name,
                                   std::shared_ptr<Renderer> renderer,
                                   std::shared_ptr<DrmDevice> dev, volatile bool *signal_stop) {
#if KESTREL_HAL_ROCKCHIP
    if (decoder_backend(name) == DecoderBackend::Rockchip)
        return std::make_shared<VdecRK>(codec, renderer, dev, signal_stop);
#endif
    (void)dev;
    return std::make_shared<VdecFfmpeg>(codec, name, renderer, signal_stop);
}

std::unique_ptr<Vdec> make_background_decoder(VideoCodec codec, std::shared_ptr<DrmDevice> dev,
                                              volatile bool *signal_stop,
                                              std::function<void(std::shared_ptr<DecodedUnit>)> sink) {
#if KESTREL_HAL_ROCKCHIP
    std::unique_ptr<VdecRK> v(new VdecRK(codec, nullptr, dev, signal_stop));
    v->set_output_timeout(100);   // ms, so run_frame can notice a stop
    v->set_frame_sink(std::move(sink));
    return v;
#else
    (void)codec; (void)dev; (void)signal_stop; (void)sink;
    return nullptr;
#endif
}

}  // namespace hal
