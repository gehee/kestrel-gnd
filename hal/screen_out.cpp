#include "hal/screen_out.hpp"

#include "kestrel_gnd_features.h"
#if KESTREL_HAL_ROCKCHIP
#include "hal/rockchip/mpp_encoder.hpp"
#include "hal/rockchip/rga_compositor.hpp"
#endif

namespace hal {

namespace {

// A HAL with no screen encoder: it takes the settings and refuses init(), as
// the recorder already handles for an encoder the hardware turns down.
class NoScreenEncoder : public ScreenEncoder {
public:
    void set_input_count(int) override {}
    void set_hevc(bool on) override { hevc_ = on; }
    bool hevc() const override { return hevc_; }
    bool init(int, int, int, int, bool, int) override { return false; }
    bool ready() const override { return false; }
    const std::vector<uint8_t> &header() const override { return header_; }
    int width() const override { return 0; }
    int height() const override { return 0; }
    int  input_count() const override { return 0; }
    int  input_dmabuf_fd(int) const override { return -1; }
    void *input_buffer(int) const override { return nullptr; }
    bool encode_buffer(int, const EncodedSink &) override { return false; }
    bool encode_external(void *, const EncodedSink &) override { return false; }
    void force_idr() override {}
    void deinit() override {}
private:
    bool hevc_ = false;
    std::vector<uint8_t> header_;
};

class NoScreenCompositor : public ScreenCompositor {
public:
    bool init(int, int, const std::vector<int> &) override { return false; }
    void deinit() override {}
    bool compose(const ScreenPair &, int, uint32_t, int, uint32_t *) override { return false; }
};

}  // namespace

bool screen_compositor_available() {
#if KESTREL_HAL_ROCKCHIP
    return RgaCompositor::available();
#else
    return false;
#endif
}

std::unique_ptr<ScreenEncoder> make_screen_encoder() {
#if KESTREL_HAL_ROCKCHIP && KESTREL_SCREEN_ENC
    return std::unique_ptr<ScreenEncoder>(new MppH264Encoder());
#else
    return std::unique_ptr<ScreenEncoder>(new NoScreenEncoder());
#endif
}

std::unique_ptr<ScreenCompositor> make_screen_compositor() {
#if KESTREL_HAL_ROCKCHIP && KESTREL_SCREEN_ENC
    return std::unique_ptr<ScreenCompositor>(new RgaCompositor());
#else
    return std::unique_ptr<ScreenCompositor>(new NoScreenCompositor());
#endif
}

}  // namespace hal
