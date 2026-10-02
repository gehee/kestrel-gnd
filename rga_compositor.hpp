#ifndef RGA_COMPOSITOR_HPP
#define RGA_COMPOSITOR_HPP

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "screen_tap.hpp"

// Rebuilds one refresh of the screen in the encoder's input buffer: the
// decoded picture where the video plane showed it, the OSD over it - with the
// RK3568's RGA2, off the display path entirely. What Caddx's own ground app
// does to record its screen, on the same kernel.
//
// The blend is RGA's three-channel mode: the NV12 picture in, the OSD in as
// BGRA (DRM's ARGB8888 is B,G,R,A in memory), "dst over" - the OSD on top -
// and NV12 out. The OSD is already premultiplied, as the display blends it
// ("pixel blend mode" 1), so RGA is not asked to premultiply it again: with
// IM_ALPHA_BLEND_PRE_MUL, as the stock app passes, a 50% white came out at
// Y 98 where the screen shows 153. Measured on the goggle: 4.8 ms median, 5.6
// ms p99 per 1080p frame, and 60 a second next to live video cost it nothing
// measurable (pictures shown and flip timing unchanged at 60 and 100 fps).
//
// Colour: RGA's own conversions, BT.601 limited range both ways (an opaque
// red OSD pixel comes out Y 82, Cb 90, Cr 240). The video makes a
// YUV->RGB->YUV round trip through the blend with the same matrix, so it
// comes out as it went in, and the OSD's RGB is converted the way the display
// shows video. They cannot be set explicitly here: librga refuses a colour
// mode for what it counts as an NV12-to-NV12 operation. The encoder tags the
// file BT.601 to match (MppH264Encoder::init).
class RgaCompositor {
public:
    ~RgaCompositor() { deinit(); }

    // Whether RGA is there to use at all (/dev/rga and the library).
    static bool available();

    // screen_w x screen_h: the size of the recording, the screen's.
    // enc_fds: the encoder's NV12 input buffers, screen-sized, stride = width.
    bool init(int screen_w, int screen_h, const std::vector<int>& enc_fds);
    void deinit();

    // Composite pair into encoder buffer enc_idx. osd_fd/osd_stride: the OSD
    // fb's DMA-BUF and pitch in bytes (-1 if there is no OSD). us: RGA time.
    bool compose(const ScreenPair& pair, int osd_fd, uint32_t osd_stride, int enc_idx, uint32_t* us);

private:
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> enc_;                     // rga handles of the encoder inputs
    std::map<int, uint32_t> osd_;                   // OSD dma-buf fd -> handle
    std::map<std::pair<uint32_t, int>, uint32_t> dec_;   // (decoder epoch, fd) -> handle
    uint32_t dec_epoch_ = 0;
    // Screen-sized NV12 the picture is scaled into when it does not fill the
    // screen 1:1 (a 720p air mode, Picture Size < 100%), black around it.
    int scratch_fd_ = -1;
    uint32_t scratch_ = 0;
    uint8_t* scratch_map_ = nullptr;
    int sx_ = -1, sy_ = -1, sw_ = -1, sh_ = -1;     // the rectangle drawn into it last
    uint32_t import_decoded(const DecodedUnit& du);
    bool ensure_scratch();
    void clear_scratch();
};

#endif
