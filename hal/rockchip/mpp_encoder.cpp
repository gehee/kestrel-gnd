#include "hal/rockchip/mpp_encoder.hpp"

// The RK3568 VEPU, reachable only through Rockchip MPP. Built only with
// KESTREL_HAL=rockchip; other builds get the HAL's no-encoder stand-in.

#include <cstdio>
#include <cstring>

#include <rockchip/rk_mpi.h>

MppH264Encoder::~MppH264Encoder() { deinit(); }

bool MppH264Encoder::init(int width, int height, int fps, int bitrate_bps, bool bt601, int max_bps) {
    deinit();
    w_ = width; h_ = height; fps_ = (fps > 0) ? fps : 30;

    MppCtx  ctx = nullptr;
    MppApi *mpi = nullptr;
    if (mpp_create(&ctx, &mpi)) {
        printf("MppEnc: mpp_create failed\n");
        return false;
    }
    if (mpp_init(ctx, MPP_CTX_ENC, hevc_ ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC)) {
        printf("MppEnc: mpp_init(ENC, %s) failed\n", hevc_ ? "H.265" : "H.264");
        mpp_destroy(ctx);
        return false;
    }

    MppEncCfg cfg = nullptr;
    if (mpp_enc_cfg_init(&cfg)) {
        printf("MppEnc: mpp_enc_cfg_init failed\n");
        mpp_destroy(ctx);
        return false;
    }

    // A partial config is rejected with -6: bps_max/bps_min and both the in and
    // out fps triples are required, not optional.
    mpp_enc_cfg_set_s32(cfg, "prep:width",        w_);
    mpp_enc_cfg_set_s32(cfg, "prep:height",       h_);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",   w_);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",   h_);
    mpp_enc_cfg_set_s32(cfg, "prep:format",       MPP_FMT_YUV420SP);
    if (bt601) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", MPP_FRAME_SPC_SMPTE170M);
        mpp_enc_cfg_set_s32(cfg, "prep:colorprim",  MPP_FRAME_PRI_SMPTE170M);
        mpp_enc_cfg_set_s32(cfg, "prep:colortrc",   MPP_FRAME_TRC_SMPTE170M);
        // Full range: the screen recording carries the air unit's picture,
        // which uses 0..255 (and the OSD blended in the same, rga_compositor).
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", MPP_FRAME_RANGE_JPEG);
    }

    const bool vbr = max_bps > bitrate_bps;
    mpp_enc_cfg_set_s32(cfg, "rc:mode",           vbr ? MPP_ENC_RC_MODE_VBR : MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target",     bitrate_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",        vbr ? max_bps : bitrate_bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",        vbr ? bitrate_bps / 4 : bitrate_bps * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex",    0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",     fps_);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm",  1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex",   0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",    fps_);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",            fps_ * 2);

    if (hevc_) {
        mpp_enc_cfg_set_s32(cfg, "codec:type",    MPP_VIDEO_CodingHEVC);
        mpp_enc_cfg_set_s32(cfg, "h265:profile",  1);    // Main
        // general_level_idc: 4.1 is 1080p60, 5.1 above it.
        mpp_enc_cfg_set_s32(cfg, "h265:level",    fps_ > 60 ? 153 : 123);
    } else {
    mpp_enc_cfg_set_s32(cfg, "codec:type",        MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile",      77);   // Main
    // 4.0 covers 1080p30 only; a capture at the display's own rate needs more
    // macroblocks per second than that, and a player may refuse a stream
    // that claims less than it carries. 4.2 is 1080p60, 5.1 above it.
    mpp_enc_cfg_set_s32(cfg, "h264:level",        fps_ > 60 ? 51 : fps_ > 30 ? 42 : 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en",     1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc",    0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8",     0);
    }

    MPP_RET ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret) {
        printf("MppEnc: MPP_ENC_SET_CFG failed (%d)\n", ret);
        mpp_destroy(ctx);
        return false;
    }

    // SPS/PPS up front. GET_HDR_SYNC needs a packet backed by real storage -
    // handing it an empty packet segfaults inside MPP.
    header_.clear();
    {
        static thread_local uint8_t hdrbuf[4096];
        MppPacket pkt = nullptr;
        mpp_packet_init(&pkt, hdrbuf, sizeof(hdrbuf));
        mpp_packet_set_length(pkt, 0);
        if (!mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, pkt)) {
            size_t len = mpp_packet_get_length(pkt);
            const uint8_t* p = (const uint8_t*)mpp_packet_get_pos(pkt);
            header_.assign(p, p + len);
        }
        if (pkt) mpp_packet_deinit(&pkt);
    }

    frame_size_ = (size_t)w_ * h_ * 3 / 2;    // NV12
    MppBufferGroup grp = nullptr;
    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION)) {
        printf("MppEnc: buffer group alloc failed\n");
        mpp_destroy(ctx);
        return false;
    }
    for (int i = 0; i < n_inputs_; i++) {
        MppBuffer fbuf = nullptr;
        if (mpp_buffer_get(grp, &fbuf, frame_size_)) {
            printf("MppEnc: frame buffer alloc failed\n");
            for (void* b : bufs_) mpp_buffer_put((MppBuffer)b);
            bufs_.clear();
            mpp_buffer_group_put(grp);
            mpp_destroy(ctx);
            return false;
        }
        bufs_.push_back(fbuf);
    }

    ctx_ = ctx; mpi_ = mpi; frm_grp_ = grp; pts_ = 0;
    printf("MppEnc: %s %dx%d @%dfps %d kbps%s NV12 dmabuf-in, %zu byte header\n",
           hevc_ ? "H.265" : "H.264", w_, h_, fps_, bitrate_bps / 1000, vbr ? " (variable, capped)" : "", header_.size());
    return true;
}

int MppH264Encoder::input_dmabuf_fd(int i) const {
    if (i < 0 || i >= (int)bufs_.size()) return -1;
    return mpp_buffer_get_fd((MppBuffer)bufs_[i]);
}

bool MppH264Encoder::encode_buffer(int i, const std::function<void(const uint8_t*, size_t, bool)>& sink) {
    if (!ctx_ || i < 0 || i >= (int)bufs_.size()) return false;
    return encode_external(bufs_[i], sink);
}

// MPP buffers belong to no encoder: any context reads any of them (by its
// DMA-BUF), so another encoder's input of the same size is encoded as is.
bool MppH264Encoder::encode_external(void* mpp_buffer, const std::function<void(const uint8_t*, size_t, bool)>& sink) {
    if (!ctx_ || !mpp_buffer) return false;
    MppApi* mpi = (MppApi*)mpi_;
    MppCtx  ctx = (MppCtx)ctx_;
    MppBuffer fbuf = (MppBuffer)mpp_buffer;

    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame)) return false;
    mpp_frame_set_width(frame, w_);
    mpp_frame_set_height(frame, h_);
    mpp_frame_set_hor_stride(frame, w_);
    mpp_frame_set_ver_stride(frame, h_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, fbuf);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_pts(frame, pts_++);

    if (mpi->encode_put_frame(ctx, frame)) {
        mpp_frame_deinit(&frame);
        return false;
    }
    mpp_frame_deinit(&frame);

    MppPacket packet = nullptr;
    while (!mpi->encode_get_packet(ctx, &packet) && packet) {
        size_t len = mpp_packet_get_length(packet);
        const uint8_t* p = (const uint8_t*)mpp_packet_get_pos(packet);
        if (len && p) {
            // Keyframe if the payload carries an IDR NAL (type 5).
            bool key = false;
            for (size_t i = 0; i + 4 < len && i < 128; i++) {
                if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
                    const int t = hevc_ ? (p[i+3] >> 1) & 0x3F : p[i+3] & 0x1F;
                    if (hevc_ ? (t >= 16 && t <= 21) : t == 5) { key = true; break; }
                }
            }
            sink(p, len, key);
        }
        // One packet per frame unless the encoder splits frames into
        // partitions (not enabled here). Asking again after the last one
        // does not return "none" - it waits out MPP's output timeout, which
        // measured 16.5 ms per frame on top of a 9 ms encode, and capped
        // screen recording near 38 fps.
        const bool last = !mpp_packet_is_partition(packet) || mpp_packet_is_eoi(packet);
        mpp_packet_deinit(&packet);
        packet = nullptr;
        if (last) break;
    }
    return true;
}

void MppH264Encoder::force_idr() {
    if (ctx_) ((MppApi*)mpi_)->control((MppCtx)ctx_, MPP_ENC_SET_IDR_FRAME, nullptr);
}

void MppH264Encoder::deinit() {
    for (void* b : bufs_) mpp_buffer_put((MppBuffer)b);
    bufs_.clear();
    if (frm_grp_)   { mpp_buffer_group_put((MppBufferGroup)frm_grp_); frm_grp_ = nullptr; }
    if (ctx_)       { mpp_destroy((MppCtx)ctx_); ctx_ = nullptr; }
    mpi_ = nullptr;
    header_.clear();
}

