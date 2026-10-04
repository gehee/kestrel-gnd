#include "bg_video.hpp"
#ifdef USE_RKMPP
#include "vdec/vdec_rk.hpp"   // full VdecRK / DrmDevice / DecodedUnit
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <algorithm>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <drm_fourcc.h>
}

// ─────────────────────────────────────────────────────────────────────────────
// Background video player — direct MPP hardware decode (reuses VdecRK).
//
// Pipeline:  mp4 file ──avformat demux──▶ h264/hevc_mp4toannexb BSF ──▶ split
// into NALs ──▶ VdecRK::feed_packet_to_decoder ──▶ (hardware decode) ──▶ frame
// sink ──▶ front_drm_ ──▶ OSD GL thread imports the NV12 DMA-BUF as a texture.
//
// This is the same decoder the live FPV feed uses; only the output routing
// differs (a frame sink into the GL-texture path instead of the video plane),
// because the idle→live transition is a GL effect that needs the frame as a
// texture. The old ffmpeg software path only reached ~21fps at 1080p60.
// ─────────────────────────────────────────────────────────────────────────────

BgVideoPlayer::BgVideoPlayer(const char* path, std::shared_ptr<DrmDevice> dev, bool loop)
    : path_(path), loop_(loop), dev_(std::move(dev)) {
    pthread_mutex_init(&mu_, nullptr);
}

BgVideoPlayer::~BgVideoPlayer() {
    stop();
    pthread_mutex_destroy(&mu_);
    free(front_);
    free(back_);
}

void BgVideoPlayer::start() {
    if (running_) return; // already running — don't spawn a second thread
    vdec_stop_ = false;
    running_ = true;
    pthread_create(&thread_, nullptr, thread_func, this);
}

void BgVideoPlayer::stop() {
    if (!running_) return;
    running_   = false;                 // stops the feed loop
    vdec_stop_ = true;                  // stops VdecRK::run_frame
    pthread_join(thread_, nullptr);     // decode_loop joins the vdec thread + tears down
}

// --- GL thread: zero-copy DRM path ---
bool BgVideoPlayer::get_latest_drm_frame(BgDrmFrame& out) {
    pthread_mutex_lock(&mu_);
    if (!new_frame_ || !front_drm_.valid) {
        pthread_mutex_unlock(&mu_);
        return false;
    }
    new_frame_ = false;
    out = front_drm_; // shared_ptr copy — keeps the MPP buffer (and DRM fd) alive
    pthread_mutex_unlock(&mu_);
    return true;
}

// --- GL thread: CPU/BGRA path (unused with hardware decode; kept for safety) ---
bool BgVideoPlayer::upload_latest_frame(GLuint tex) {
    pthread_mutex_lock(&mu_);
    if (!new_frame_ || !front_) {
        pthread_mutex_unlock(&mu_);
        return false;
    }
    new_frame_ = false;
    int w = w_, h = h_;
    pthread_mutex_unlock(&mu_);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, front_);
    return true;
}

void* BgVideoPlayer::thread_func(void* arg) {
    static_cast<BgVideoPlayer*>(arg)->decode_loop();
    return nullptr;
}

#ifdef USE_RKMPP

// Feed one Annex-B NAL (with its start code) to the decoder, computing its type.
static void bg_feed_nal(VdecRK* vdec, const uint8_t* nal, int len,
                        int64_t pts, bool is_h265) {
    if (len < 4) return;
    int sc = (nal[2] == 1) ? 3 : 4;   // 00 00 01 -> 3,  00 00 00 01 -> 4
    if (len <= sc) return;
    uint8_t hdr = nal[sc];
    uint8_t nal_type = is_h265 ? ((hdr >> 1) & 0x3F) : (hdr & 0x1F);
    vdec->feed_packet_to_decoder((void*)nal, len, pts, 0, nal_type, 0, 0, false);
}

// Split an Annex-B buffer into NAL units (each kept with its start code) and
// feed them one at a time — matching how the live feed pipeline feeds VdecRK.
static void bg_feed_annexb(VdecRK* vdec, const uint8_t* buf, int size,
                           int64_t pts, bool is_h265) {
    int i = 0, nal_begin = -1;
    while (i + 3 <= size) {
        bool sc3 = (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1);
        bool sc4 = (i + 4 <= size && buf[i] == 0 && buf[i + 1] == 0 &&
                    buf[i + 2] == 0 && buf[i + 3] == 1);
        if (sc3 || sc4) {
            if (nal_begin >= 0)
                bg_feed_nal(vdec, buf + nal_begin, i - nal_begin, pts, is_h265);
            nal_begin = i;
            i += sc4 ? 4 : 3;
        } else {
            i++;
        }
    }
    if (nal_begin >= 0 && nal_begin < size)
        bg_feed_nal(vdec, buf + nal_begin, size - nal_begin, pts, is_h265);
}

// A recording's pictures can have several slices (the air unit sends two). The
// decoder wants a picture's slices as ONE packet with one timestamp: fed one
// slice at a time, the second half of every picture came out wrong - the live
// path (ar8030_source.cpp, "feed them as a single access unit") has the same
// rule. So the picture's slice NALs are gathered into one packet; parameter sets
// and the rest go in alone, where the decoder caches or drops them.
static void bg_feed_access_unit(VdecRK* vdec, const uint8_t* buf, int size,
                                int64_t pts, bool is_h265) {
    std::vector<uint8_t> au;
    uint8_t au_type = 0;
    auto nal_header_type = [&](const uint8_t* nal, int len) -> int {
        const int sc = (nal[2] == 1) ? 3 : 4;
        if (len <= sc) return -1;
        const uint8_t hdr = nal[sc];
        return is_h265 ? ((hdr >> 1) & 0x3F) : (hdr & 0x1F);
    };
    auto take = [&](const uint8_t* nal, int len) {
        if (len < 4) return;
        const int type = nal_header_type(nal, len);
        if (type < 0) return;
        const bool vcl = is_h265 ? (type <= 31) : (type >= 1 && type <= 5);
        if (!vcl) { bg_feed_nal(vdec, nal, len, pts, is_h265); return; }
        if (au.empty()) au_type = (uint8_t)type;
        au.insert(au.end(), nal, nal + len);
    };
    int i = 0, nal_begin = -1;
    while (i + 3 <= size) {
        const bool sc3 = (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1);
        const bool sc4 = (i + 4 <= size && buf[i] == 0 && buf[i + 1] == 0 &&
                          buf[i + 2] == 0 && buf[i + 3] == 1);
        if (sc3 || sc4) {
            if (nal_begin >= 0) take(buf + nal_begin, i - nal_begin);
            nal_begin = i;
            i += sc4 ? 4 : 3;
        } else {
            i++;
        }
    }
    if (nal_begin >= 0 && nal_begin < size) take(buf + nal_begin, size - nal_begin);
    if (!au.empty())
        vdec->feed_packet_to_decoder(au.data(), (int)au.size(), pts, 0, au_type, 0, 0, false);
}

void BgVideoPlayer::decode_loop() {
    // Lower priority so the GL/OSD thread isn't starved during VPU init.
    setpriority(PRIO_PROCESS, 0, 5);

    // ---- Open the file (retry until success or stop) ----
    AVFormatContext* fmt = nullptr;
    while (running_) {
        AVDictionary* fmt_opts = nullptr;
        av_dict_set_int(&fmt_opts, "probesize",       65536,  0);
        av_dict_set_int(&fmt_opts, "analyzeduration", 500000, 0);
        int open_ret = avformat_open_input(&fmt, path_.c_str(), nullptr, &fmt_opts);
        av_dict_free(&fmt_opts);
        if (open_ret == 0) { avformat_find_stream_info(fmt, nullptr); break; }
        fprintf(stderr, "[BgVideo] cannot open '%s', retrying in 2s\n", path_.c_str());
        sleep(2);
    }
    if (!running_) { if (fmt) avformat_close_input(&fmt); return; }

    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    if (vs < 0) {
        fprintf(stderr, "[BgVideo] no video stream in '%s'\n", path_.c_str());
        avformat_close_input(&fmt);
        return;
    }

    AVCodecParameters* cp = fmt->streams[vs]->codecpar;
    VideoCodec  vcodec;
    const char* bsf_name;
    bool        is_h265;
    if (cp->codec_id == AV_CODEC_ID_H264)      { vcodec = VideoCodec::H264; bsf_name = "h264_mp4toannexb"; is_h265 = false; }
    else if (cp->codec_id == AV_CODEC_ID_HEVC) { vcodec = VideoCodec::H265; bsf_name = "hevc_mp4toannexb"; is_h265 = true;  }
    else {
        fprintf(stderr, "[BgVideo] unsupported codec %d (need h264/h265)\n", cp->codec_id);
        avformat_close_input(&fmt);
        return;
    }

    // ---- mp4 (length-prefixed) -> Annex-B (start codes + in-band SPS/PPS) ----
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    AVBSFContext* bsf_ctx = nullptr;
    if (!bsf || av_bsf_alloc(bsf, &bsf_ctx) < 0) {
        fprintf(stderr, "[BgVideo] %s BSF unavailable\n", bsf_name);
        avformat_close_input(&fmt);
        return;
    }
    avcodec_parameters_copy(bsf_ctx->par_in, cp);
    bsf_ctx->time_base_in = fmt->streams[vs]->time_base;
    av_bsf_init(bsf_ctx);

    // ---- Create the hardware decoder (no renderer; output via the sink) ----
    // VdecRK::run_frame loops while !*signal_stop, so pass the stop flag.
    // NOTE: VdecRK runs in immediate (decode-order) output mode for low FPV
    // latency, and MPP does not reorder, so the background clip MUST be encoded
    // without B-frames (IPPP) — otherwise B-frames present in decode order and
    // appear to play backwards. background.mp4 is encoded -bf 0 accordingly.
    vdec_.reset(new VdecRK(vcodec, nullptr, dev_, &vdec_stop_));
    vdec_->set_output_timeout(100); // ms, so run_frame can notice stop()

    vdec_->set_frame_sink([this](std::shared_ptr<DecodedUnit> du) {
        BgDrmFrame bf;
        bf.fd        = du->prime_fd;
        bf.width     = (int)du->width;
        bf.height    = (int)du->height;
        bf.stride_y  = du->pitches[0];
        bf.stride_uv = du->pitches[1];
        bf.offset_uv = du->offsets[1];
        bf.drm_fmt   = du->drm_pixel_format;   // DRM_FORMAT_NV12
        bf.modifier  = du->modifiers[0];       // 0 == linear (MPP NV12 is linear)
        bf.ref       = du->frame_ref;          // keeps the MPP buffer alive until import
        bf.valid     = true;

        frames_++;
        pthread_mutex_lock(&mu_);
        w_ = bf.width; h_ = bf.height;
        back_drm_ = std::move(bf);
        std::swap(front_drm_, back_drm_);
        is_drm_    = true;
        new_frame_ = true;
        pthread_mutex_unlock(&mu_);

        if (on_new_frame) on_new_frame();
    });

    fprintf(stderr, "[BgVideo] hardware decode via VdecRK (%s)\n", is_h265 ? "h265" : "h264");
    pthread_create(&vdec_thread_, nullptr, Vdec::run_frame_thread, vdec_.get());

    // ---- Feed loop: demux -> annexb -> NALs -> VdecRK, paced to source fps ----
    AVRational fr = av_guess_frame_rate(fmt, fmt->streams[vs], nullptr);
    double fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 60.0;
    uint64_t frame_interval_ns = (uint64_t)(1e9 / (fps > 1.0 ? fps : 60.0));
    uint64_t next_frame_ns = 0;
    int64_t  feed_pts = 0;

    uint64_t fps_count = 0, fps_window_ns = 0;

    AVPacket* pkt = av_packet_alloc();

    // Recording playback paces on the pictures' own times, from a point on the
    // wall clock that is set again after anything that breaks the run: the
    // start, a pause, a seek. A seek lands on a keyframe and plays from there;
    // running the decoder on to the exact target as fast as it will take pictures
    // overflowed its input and dropped some, and what followed was garbled
    // until the next keyframe.
    const AVRational stream_tb = fmt->streams[vs]->time_base;
    auto picture_ms = [&](const AVPacket* p) -> int64_t {
        const int64_t v = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        return v == AV_NOPTS_VALUE ? -1 : av_rescale_q(v, stream_tb, AVRational{1, 1000});
    };
    uint64_t base_wall_ns = 0;
    int64_t  base_pic_ms = 0;
    bool     resync = true;
    bool     show_one = false;    // a seek while paused still puts its picture on screen

    while (running_) {
        if (!loop_) {
            const int delta = seek_delta_ms_.exchange(0);
            if (delta != 0) {
                ended_ = false;
                int64_t dur = duration_ms_ > 0 ? (int64_t)duration_ms_
                              : (fmt->duration > 0 ? fmt->duration / 1000 : INT64_MAX);
                int64_t want = delta <= INT_MIN / 2 ? 0 : (int64_t)pos_ms_ + delta;
                if (want > dur - 500) want = dur - 500;   // leave something to play
                if (want < 0) want = 0;
                // Back to the keyframe at or before the target; forward to the
                // one at or after it, so a short jump still moves when the
                // keyframes are further apart than it. With none ahead, a
                // forward jump has nowhere to go: playback carries on.
                const int64_t ts = av_rescale_q(want, AVRational{1, 1000}, stream_tb);
                if (delta > 0) {
                    if (av_seek_frame(fmt, vs, ts, 0) < 0) continue;
                } else if (av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD) < 0) {
                    av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);   // no index: from the start
                }
                av_bsf_flush(bsf_ctx);
                pos_ms_ = (int)want;
                ended_ = false;
                resync = true;
                show_one = true;
            }
            if ((paused_ || ended_) && !show_one) {   // the last picture stays on screen
                usleep(10000);
                resync = true;
                continue;
            }
        }

        int ret = av_read_frame(fmt, pkt);
        if (ret == AVERROR_EOF) {
            if (!loop_) { ended_ = true; continue; }   // idles above until a seek or stop()
            av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
            av_bsf_flush(bsf_ctx);
            next_frame_ns = 0;
            continue;
        }
        if (ret < 0) break;
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;

        if (!loop_) {
            const int64_t pm = picture_ms(pkt);
            if (pm >= 0) {
                if (resync) { base_wall_ns = now_ns; base_pic_ms = pm; resync = false; }
                const uint64_t due = base_wall_ns + (uint64_t)std::max<int64_t>(0, pm - base_pic_ms) * 1000000ULL;
                if (due > now_ns && due - now_ns < 1000000000ULL) {
                    struct timespec tgt;
                    tgt.tv_sec  = due / 1000000000ULL;
                    tgt.tv_nsec = due % 1000000000ULL;
                    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tgt, nullptr);
                } else if (now_ns > due + 200000000ULL) {
                    resync = true;           // far behind: carry on from here, not in a burst
                }
                pos_ms_ = (int)pm;
            }
        } else {
        // Steady frame-rate pacing on the feed (robust to B-frame reordering).
        if (next_frame_ns == 0) next_frame_ns = now_ns;
        next_frame_ns += frame_interval_ns;
        if (next_frame_ns > now_ns) {
            struct timespec tgt;
            tgt.tv_sec  = next_frame_ns / 1000000000ULL;
            tgt.tv_nsec = next_frame_ns % 1000000000ULL;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tgt, nullptr);
        } else {
            next_frame_ns = now_ns; // fell behind — resync, don't accumulate drift
        }
        }

        // Convert this access unit to Annex-B and feed its NALs.
        if (av_bsf_send_packet(bsf_ctx, pkt) == 0) {
            AVPacket* out = av_packet_alloc();
            while (av_bsf_receive_packet(bsf_ctx, out) == 0) {
                if (loop_) bg_feed_annexb(vdec_.get(), out->data, out->size, feed_pts++, is_h265);
                else       bg_feed_access_unit(vdec_.get(), out->data, out->size, feed_pts++, is_h265);
                av_packet_unref(out);
            }
            av_packet_free(&out);
        }
        av_packet_unref(pkt);
        show_one = false;

        // FPS meter every 5s (feed rate; the sink/display rate follows it).
        if (!loop_) continue;
        if (fps_window_ns == 0) fps_window_ns = now_ns;
        fps_count++;
        uint64_t elapsed = now_ns - fps_window_ns;
        if (elapsed >= 5000000000ULL) {
            fprintf(stderr, "[BgVideo] feed fps=%.2f (%llu frames / %.2fs)\n",
                    (double)fps_count * 1e9 / (double)elapsed,
                    (unsigned long long)fps_count, elapsed / 1e9);
            fps_count = 0; fps_window_ns = now_ns;
        }
    }

    // ---- Teardown ----
    running_   = false;
    vdec_stop_ = true;                      // ensure VdecRK::run_frame exits
    if (vdec_thread_) { pthread_join(vdec_thread_, nullptr); vdec_thread_ = 0; }
    vdec_.reset();
    av_packet_free(&pkt);
    av_bsf_free(&bsf_ctx);
    avformat_close_input(&fmt);
}

#else  // !USE_RKMPP

// The zero-copy DRM background-video path is built on the RK3588 MPP decoder
// (VdecRK) and has no software/VAAPI equivalent wired up. On non-rkmpp builds
// the player is a no-op: the OSD falls back to its static background, while the
// live FPV feed still decodes via the ffmpeg/VAAPI path.
void BgVideoPlayer::decode_loop() {
    static bool warned = false;
    if (!warned) {
        fprintf(stderr, "[BgVideo] disabled: built without USE_RKMPP "
                        "(no MPP hardware decoder)\n");
        warned = true;
    }
    running_ = false;
}

#endif // USE_RKMPP
