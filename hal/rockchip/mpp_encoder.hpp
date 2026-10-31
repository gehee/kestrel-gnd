#ifndef MPP_ENCODER_HPP
#define MPP_ENCODER_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

#include "hal/screen_out.hpp"

// Hardware H.264 encoder on the RK3568 VEPU, driven through Rockchip MPP.
//
// Why not ffmpeg: this board's libavcodec offers exactly one H.264 encoder,
// h264_v4l2m2m, and there are no /dev/video* nodes for it to bind to - it fails
// with "Could not find a valid device". The hardware encoder is reachable only
// through MPP (/dev/mpp_service), which we already link for decoding.
//
// Verified on hardware before this was written: mpp_init(MPP_CTX_ENC,
// MPP_VIDEO_CodingAVC) succeeds and MPP_ENC_GET_HDR_SYNC returns a valid 38-byte
// SPS/PPS (67 4D 40 28 -> Main profile, level 4.0).
class MppH264Encoder : public hal::ScreenEncoder {
    public:
        MppH264Encoder() = default;
        ~MppH264Encoder() override;

        // NV12 in, H.264 out. Returns false if the hardware refuses the config.
        // The input buffers are DMA-BUFs RGA composites the screen into.
        // bt601: tag the stream BT.601 limited range in its VUI, for input
        // converted from RGB with that matrix (the RGA screen recorder), so
        // players do not guess BT.709 for HD and shift the colours.
        // max_bps > 0: variable rate, bitrate_bps on average and never above
        // max_bps (the web view's cap); otherwise constant rate.
        bool init(int width, int height, int fps, int bitrate_bps, bool bt601 = false, int max_bps = 0) override;
        bool ready() const override { return ctx_ != nullptr; }

        // SPS/PPS as Annex-B, fetched at init. minimp4 needs these before any
        // slice, exactly as the FPV recording path does.
        const std::vector<uint8_t>& header() const override { return header_; }

        int width()  const override { return w_; }
        int height() const override { return h_; }

        // The input buffers as DMA-BUFs: RGA composites the screen straight
        // into them, so a frame is never copied - RGA writes one, the encoder
        // reads it. There are kInputBuffers so RGA can write the next frame
        // while the encoder is still reading the last one (and a follower,
        // another encoder, reads one too).
        // Before init(): how many to allocate (0 for an encoder that only
        // reads another's, encode_external()).
        void set_input_count(int n) override { n_inputs_ = n; }
        // Before init(): H.265 instead of H.264 (the VEPU does both).
        void set_hevc(bool on) override { hevc_ = on; }
        bool hevc() const override { return hevc_; }
        int  input_count() const override { return (int)bufs_.size(); }
        int  input_dmabuf_fd(int i = 0) const override;
        // Encode whatever is already in input buffer i (no memcpy in).
        bool encode_buffer(int i, const hal::EncodedSink& sink) override;
        bool encode_in_place(const std::function<void(const uint8_t*, size_t, bool)>& sink) {
            return encode_buffer(0, sink);
        }
        // The next frame encoded is an IDR (a viewer waiting to start on one).
        void force_idr() override;
        // Input buffer i itself (an MppBuffer), for a second encoder of the
        // same size to read with encode_external(): one composite, two streams.
        void* input_buffer(int i) const override { return i >= 0 && i < (int)bufs_.size() ? bufs_[i] : nullptr; }
        bool encode_external(void* mpp_buffer, const hal::EncodedSink& sink) override;

        void deinit() override;

    private:
        void*  ctx_        = nullptr;   // MppCtx
        void*  mpi_        = nullptr;   // MppApi*
        void*  frm_grp_    = nullptr;   // MppBufferGroup
        std::vector<void*> bufs_;        // MppBuffers for the input frames
        int    w_ = 0, h_ = 0, fps_ = 0;
        size_t frame_size_ = 0;
        std::vector<uint8_t> header_;
        int64_t pts_ = 0;
        int    n_inputs_ = kInputBuffers;
        bool   hevc_ = false;
};

#endif
