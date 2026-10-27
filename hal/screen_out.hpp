#pragma once
// HAL: the screen as a video stream, for screen recording and the phone view.
// The compositor rebuilds one refresh of the screen (the picture where the
// video plane showed it, the OSD over it) into the encoder's input buffers,
// and the encoder turns those into H.264/H.265.
//
// Every build gets an encoder and a compositor; where the HAL has none, they
// refuse init() and the recorder reports screen recording unavailable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ScreenPair;

namespace hal {

using EncodedSink = std::function<void(const uint8_t *, size_t, bool)>;

class ScreenEncoder {
public:
    // Input buffers the compositor writes and the encoder reads, so the next
    // frame can be composited while the last is still being encoded.
    static constexpr int kInputBuffers = 6;

    virtual ~ScreenEncoder() = default;
    // Before init(): how many input buffers (0 for an encoder that only reads
    // another's, encode_external()), and H.265 instead of H.264.
    virtual void set_input_count(int n) = 0;
    virtual void set_hevc(bool on) = 0;
    virtual bool hevc() const = 0;
    // NV12 in. bt601: tag the stream BT.601 limited range. max_bps > 0:
    // variable rate around bitrate_bps, never above max_bps.
    virtual bool init(int width, int height, int fps, int bitrate_bps, bool bt601 = false, int max_bps = 0) = 0;
    virtual bool ready() const = 0;
    // SPS/PPS (and VPS) as Annex-B, from init().
    virtual const std::vector<uint8_t> &header() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual int  input_count() const = 0;
    virtual int  input_dmabuf_fd(int i = 0) const = 0;
    // Input buffer i itself, for a second encoder of the same size to read
    // with encode_external(): one composite, two streams.
    virtual void *input_buffer(int i) const = 0;
    virtual bool encode_buffer(int i, const EncodedSink &sink) = 0;
    virtual bool encode_external(void *buffer, const EncodedSink &sink) = 0;
    // The next frame encoded is an IDR.
    virtual void force_idr() = 0;
    virtual void deinit() = 0;
};

class ScreenCompositor {
public:
    virtual ~ScreenCompositor() = default;
    // screen_w x screen_h: the recording's size. enc_fds: the encoder's NV12
    // input buffers, screen-sized, stride = width.
    virtual bool init(int screen_w, int screen_h, const std::vector<int> &enc_fds) = 0;
    virtual void deinit() = 0;
    // pair into encoder buffer enc_idx. osd_fd/osd_stride: the OSD fb's
    // DMA-BUF and pitch (-1: no OSD). us: the time it took.
    virtual bool compose(const ScreenPair &pair, int osd_fd, uint32_t osd_stride, int enc_idx, uint32_t *us) = 0;
};

// Whether this unit can composite the screen at all (the hardware and its
// library are there).
bool screen_compositor_available();
std::unique_ptr<ScreenEncoder> make_screen_encoder();
std::unique_ptr<ScreenCompositor> make_screen_compositor();

}  // namespace hal
