// The goggle's gallery, drawn. gallery.hpp has the logic (which key does what,
// the animation, where each tile sits); this draws that, on the OSD plane, in
// place of the HUD, and runs the decoder that plays a recording.
//
// How the live picture comes into it: it is not drawn here at all. It stays on
// the video plane, which the display controller scales, and the gallery moves
// that plane to the live tile's rectangle (DrmDevice::stage_tile) and leaves
// that rectangle clear in this frame, so the picture shows through the hole.
// Everything else - the backdrop, the recordings' thumbnails, text - is on the
// OSD plane around it.

#include "osd.hpp"
#include "dvr_library.hpp"
#include "hud_theme.hpp"
#include "gallery_stats.hpp"
#include "renderer.hpp"
#include "utils/time_util.h"

#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace {

// A recording's thumbnail (a JPEG, see dvr_library) as BGRA, the byte order the
// OSD's shader expects of every texture it draws.
bool decode_thumbnail(const std::string& path, std::vector<uint8_t>& bgra, int& w, int& h) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    AVCodecContext* cc = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    bool ok = false;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs < 0) break;
        const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
        if (!dec) break;
        cc = avcodec_alloc_context3(dec);
        if (!cc || avcodec_parameters_to_context(cc, fmt->streams[vs]->codecpar) < 0 ||
            avcodec_open2(cc, dec, nullptr) < 0) break;
        bool got = false;
        while (!got && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) == 0)
                got = avcodec_receive_frame(cc, frame) == 0;
            av_packet_unref(pkt);
        }
        if (!got) { avcodec_send_packet(cc, nullptr); got = avcodec_receive_frame(cc, frame) == 0; }
        if (!got || frame->width < 2 || frame->height < 2) break;
        w = frame->width;
        h = frame->height;
        sws = sws_getContext(w, h, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) break;
        bgra.assign((size_t)w * h * 4, 0);
        uint8_t* dst[4] = { bgra.data(), nullptr, nullptr, nullptr };
        int dst_ls[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dst_ls);
        ok = true;
    } while (0);
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return ok;
}

// The largest rectangle of aspect `aspect` that fits inside r (centre-based).
Gallery::Rect fit(const Gallery::Rect& r, float aspect) {
    if (aspect <= 0) return r;
    float w = r.w, h = r.w / aspect;
    if (h > r.h) { h = r.h; w = r.h * aspect; }
    return { r.x, r.y, w, h };
}

constexpr size_t kMaxThumbs = 24;

} // namespace

// The recording's thumbnail as a texture, or 0 if it has none (yet). Loaded the
// first time a tile near the screen asks, one per frame at most (`may_load`),
// and the stalest dropped past kMaxThumbs. A thumbnail that is still being made
// (dvr_library does that, one at a time) is looked for again a little later.
GLuint OSD::gallery_thumb(const std::string& name, int& w, int& h, bool may_load, bool& loaded) {
    loaded = false;
    auto it = gallery_thumbs_.find(name);
    if (it != gallery_thumbs_.end() && it->second.tex) {
        it->second.used = gallery_frame_;
        w = it->second.w;
        h = it->second.h;
        return it->second.tex;
    }
    if (!may_load) return 0;
    const uint64_t now = get_time_us();
    if (it != gallery_thumbs_.end() && now < it->second.next_try_us) return 0;

    GalleryThumb& th = gallery_thumbs_[name];
    th.next_try_us = now + 700000;
    std::vector<uint8_t> px;
    int tw = 0, thh = 0;
    struct stat st;
    const std::string path = dvr_lib::thumb_path(name);
    if (stat(path.c_str(), &st) != 0 || !decode_thumbnail(path, px, tw, thh)) return 0;

    if (gallery_thumbs_.size() > kMaxThumbs) {
        auto victim = gallery_thumbs_.end();
        for (auto v = gallery_thumbs_.begin(); v != gallery_thumbs_.end(); ++v)
            if (v->first != name && v->second.tex &&
                (victim == gallery_thumbs_.end() || v->second.used < victim->second.used))
                victim = v;
        if (victim != gallery_thumbs_.end()) {
            glDeleteTextures(1, &victim->second.tex);
            gallery_thumbs_.erase(victim);
        }
    }
    GalleryThumb& slot = gallery_thumbs_[name];   // (the erase may have moved things)
    glGenTextures(1, &slot.tex);
    glBindTexture(GL_TEXTURE_2D, slot.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, thh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    slot.w = tw;
    slot.h = thh;
    slot.used = gallery_frame_;
    w = tw;
    h = thh;
    loaded = true;
    return slot.tex;
}

// Keep the recording decoder in step with what the gallery wants playing. On
// the render thread, and not under osd_mutex: stopping a player joins its
// decode thread, which signals a render.
void OSD::service_clip_player(const Gallery::Snapshot& s) {
    const std::string want = gallery_.play_name();
    if (want != clip_playing_) {
        if (clip_player_) { clip_player_->stop(); clip_player_.reset(); }
        clip_playing_ = want;
        clip_have_frame_ = false;
        if (!want.empty()) {
            const std::string path = dvr_lib::dir() + "/" + want;
            clip_player_.reset(new BgVideoPlayer(path.c_str(), dev, false));
            if (s.items)
                for (const auto& it : *s.items)
                    if (it.name == want) clip_player_->set_duration_ms((int)(it.duration * 1000));
            clip_player_->on_new_frame = [this]() { signal_render(prof::kWakeOther); };
            clip_player_->start();
        }
    }
    if (!clip_player_) return;
    clip_player_->set_paused(gallery_.play_paused());
    const int seek = gallery_.take_seek_ms();
    if (seek) clip_player_->seek_by_ms(seek);
    if (clip_player_->ended() && !clip_player_->seek_pending()) gallery_.playback_ended();
    if (import_drm_frame(clip_player_.get(), clip_ext_tex_)) clip_have_frame_ = true;
}

// The picture the display is showing, as a texture of its own: the same decoded frame the
// video plane has, imported by file descriptor (no copy). The import holds its own
// reference to the memory, so the picture itself is let go of as soon as this returns.
bool OSD::import_live_picture() {
    if (!pfn_eglCreateImageKHR || !pfn_glEGLImageTargetTexture2DOES || !live_ext_tex_) return false;
    std::shared_ptr<DecodedUnit> pic = dev->shown_picture();
    if (!pic || !pic->has_prime_fd || pic->prime_fd < 0 || !pic->drm_pixel_format || !pic->pitches[0])
        return live_ext_have_;
    if (live_ext_have_ && pic->pts == live_ext_pts_ && pic->buf_epoch == live_ext_epoch_) return true;
    EGLint attribs[32]; int ai = 0;
    attribs[ai++] = EGL_WIDTH;                    attribs[ai++] = (EGLint)pic->width;
    attribs[ai++] = EGL_HEIGHT;                   attribs[ai++] = (EGLint)pic->height;
    attribs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;     attribs[ai++] = (EGLint)pic->drm_pixel_format;
    attribs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;    attribs[ai++] = pic->prime_fd;
    attribs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[ai++] = (EGLint)pic->offsets[0];
    attribs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attribs[ai++] = (EGLint)pic->pitches[0];
    attribs[ai++] = EGL_DMA_BUF_PLANE1_FD_EXT;    attribs[ai++] = pic->prime_fd;
    attribs[ai++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT; attribs[ai++] = (EGLint)pic->offsets[1];
    attribs[ai++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;  attribs[ai++] = (EGLint)pic->pitches[1];
    const uint64_t mod = pic->modifiers[0];
    if (mod != DRM_FORMAT_MOD_INVALID && mod != DRM_FORMAT_MOD_LINEAR) {
        attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attribs[ai++] = (EGLint)(mod & 0xFFFFFFFF);
        attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attribs[ai++] = (EGLint)(mod >> 32);
        attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT; attribs[ai++] = (EGLint)(mod & 0xFFFFFFFF);
        attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT; attribs[ai++] = (EGLint)(mod >> 32);
    }
    attribs[ai++] = EGL_NONE;
    EGLImageKHR img = pfn_eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[OSD] live picture import failed: 0x%x\n", eglGetError()); }
        return live_ext_have_;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, live_ext_tex_);
    pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
    pfn_eglDestroyImageKHR(display, img);
    live_ext_have_ = true;
    live_ext_pts_ = pic->pts;
    live_ext_epoch_ = pic->buf_epoch;
    return true;
}

// Top up the history the stats screen reads: what has been added to the latency ring and
// the flip log since last time, copied - not the whole window, which at 100 fps for three
// minutes is 18000 pictures and used to be copied under the lock the video path takes on
// every frame. Old entries are let go of from the front, in chunks.
void OSD::stats_history_update(uint64_t now) {
    const uint64_t keep_us = 190ULL * 1000000ULL;
    const uint64_t cutoff = now > keep_us ? now - keep_us : 0;
    static std::vector<StatsFrame> fresh;
    static std::vector<DrmDevice::FlipSample> fresh_flips;
    fresh.clear();
    fresh_flips.clear();

    // Not looked at for longer than the history reaches: start over.
    if (stats_frames_t_ && now > stats_frames_t_ + keep_us) {
        stats_frames_.clear(); stats_frames_head_ = 0; stats_frames_t_ = 0;
        stats_flips_.clear();  stats_flips_head_ = 0;  stats_flips_t_ = 0;
    }
    pthread_mutex_lock(&osd_mutex);
    const size_t cnt = osd_vars.latency_ring.size();
    for (size_t i = 0; i < cnt; i++) {                // newest first, until what is already held
        const LatencyFrame& f = osd_vars.latency_ring.at(cnt - 1 - i);
        if (f.t_us == 0 || f.t_us <= stats_frames_t_ || f.t_us < cutoff) break;
        StatsFrame sf;
        sf.t_us = f.t_us;
        sf.stage[0] = f.capture_ms; sf.stage[1] = f.processing_ms; sf.stage[2] = f.net_ms;
        sf.stage[3] = f.reassemble_ms; sf.stage[4] = f.dec_ms; sf.stage[5] = f.disp_ms;
        sf.video_mbps = f.video_mbps; sf.link_mbps = f.rf_mbps;
        sf.lost = f.lost; sf.snr = f.snr; sf.mcs = f.mcs; sf.key = f.key;
        fresh.push_back(sf);
    }
    pthread_mutex_unlock(&osd_mutex);
    if (!fresh.empty()) {
        stats_frames_t_ = fresh.front().t_us;         // newest first
        stats_frames_.insert(stats_frames_.end(), fresh.rbegin(), fresh.rend());
    }
    dev->append_flip_log_after(stats_flips_t_, fresh_flips);
    if (!fresh_flips.empty()) {
        stats_flips_t_ = fresh_flips.back().t_us;
        stats_flips_.reserve(stats_flips_.size() + fresh_flips.size());
        for (const auto& f : fresh_flips) stats_flips_.push_back({ f.t_us, f.gap_us });
    }
    // Forget what is older than any window shows.
    while (stats_frames_head_ < stats_frames_.size() && stats_frames_[stats_frames_head_].t_us < cutoff) stats_frames_head_++;
    while (stats_flips_head_ < stats_flips_.size() && stats_flips_[stats_flips_head_].t_us < cutoff) stats_flips_head_++;
    if (stats_frames_head_ > 4096 && stats_frames_head_ * 2 > stats_frames_.size()) {
        stats_frames_.erase(stats_frames_.begin(), stats_frames_.begin() + (long)stats_frames_head_);
        stats_frames_head_ = 0;
    }
    if (stats_flips_head_ > 4096 && stats_flips_head_ * 2 > stats_flips_.size()) {
        stats_flips_.erase(stats_flips_.begin(), stats_flips_.begin() + (long)stats_flips_head_);
        stats_flips_head_ = 0;
    }
}

// Rebuild the stats screen's numbers for the window, from the history above. Every frame
// for the 10 s window; the longer ones move a column in 250 ms and 750 ms, so a few
// rebuilds a second is smooth (the chart scrolls by the fraction in between) and each is a
// pass over the whole window. And once a window changes, at once.
void OSD::stats_refresh(int window_idx, uint64_t now) {
    static const int kWin[3] = { 10, 60, 180 };
    const int ws = kWin[std::max(0, std::min(2, window_idx))];
    const uint64_t every = ws <= 10 ? 0 : ws <= 60 ? 50000 : 250000;
    if (stats_view_window_ == ws && now - stats_view_us_ < every) return;
    const uint64_t pt_start = prof::enabled() ? get_time_us() : 0;
    stats_history_update(now);
    const uint64_t pt_copied = prof::enabled() ? get_time_us() : 0;

    // Only the part of the history this window reaches into.
    const uint64_t win_us = (uint64_t)ws * 1000000ULL;
    const uint64_t t0 = now > win_us + win_us / StatsView::kCols ? now - win_us - win_us / StatsView::kCols : 0;
    const StatsFrame* fb = stats_frames_.data() + stats_frames_head_;
    const size_t fn = stats_frames_.size() - stats_frames_head_;
    const StatsFrame* f0 = std::lower_bound(fb, fb + fn, t0, [](const StatsFrame& f, uint64_t t) { return f.t_us < t; });
    const StatsFlip* lb = stats_flips_.data() + stats_flips_head_;
    const size_t ln = stats_flips_.size() - stats_flips_head_;
    const StatsFlip* l0 = std::lower_bound(lb, lb + ln, t0, [](const StatsFlip& f, uint64_t t) { return f.t_us < t; });
    build_stats(f0, (size_t)(fb + fn - f0), l0, (size_t)(lb + ln - l0), now, ws, dev->flips_are_frames, stats_view_);
    stats_view_window_ = ws;
    stats_view_us_ = now;
    if (stats_shown_us_ == 0 || now - stats_shown_us_ >= 500000 || stats_shown_.window_s != ws) {
        stats_shown_ = stats_view_;
        stats_shown_us_ = now;
    }
    if (prof::enabled()) {
        const uint64_t pt_end = get_time_us();
        static uint64_t n = 0, sum_copy = 0, max_copy = 0, sum_build = 0, max_total = 0, last_print = 0;
        n++;
        sum_copy += pt_copied - pt_start; max_copy = std::max(max_copy, pt_copied - pt_start);
        sum_build += pt_end - pt_copied;
        max_total = std::max(max_total, pt_end - pt_start);
        if (pt_end - last_print > 2000000) {
            if (last_print)
                printf("PROF stats rebuild %ds window (2 s): n=%llu pictures %zu flips %zu | history update avg %.2f max %.2f ms | build avg %.2f ms | total max %.2f ms\n",
                       ws, (unsigned long long)n, (size_t)(fb + fn - f0), (size_t)(lb + ln - l0),
                       sum_copy / 1000.0 / n, max_copy / 1000.0, sum_build / 1000.0 / n, max_total / 1000.0);
            n = sum_copy = max_copy = sum_build = max_total = 0;
            last_print = pt_end;
        }
    }
}

// KESTREL_PROF: every two seconds, how the last ten seconds of display flips were
// spaced - the evidence behind the smoothness figure.
void OSD::stats_prof(uint64_t now) {
    static uint64_t last = 0;
    if (now - last < 2000000) return;
    last = now;
    static std::vector<DrmDevice::FlipSample> raw;
    dev->copy_flip_log(now > 10000000 ? now - 10000000 : 0, raw);
    static const float edge[8] = { 10, 14, 20, 30, 45, 70, 120, 1e9f };
    int h[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (const auto& f : raw) {
        const float ms = (float)f.gap_us / 1000.0f;
        int b = 0;
        while (ms >= edge[b]) b++;
        h[b]++;
    }
    if (getenv("KESTREL_PROF_RAWFLIPS")) {
        printf("PROF raw gaps(us):");
        const size_t n0 = raw.size() > 40 ? raw.size() - 40 : 0;
        for (size_t i = n0; i < raw.size(); i++) printf(" %u", (unsigned)raw[i].gap_us);
        printf("\n");
    }
    {
        uint32_t a[6];
        for (int i = 0; i < 6; i++) a[i] = dev->osd_age_hist_[i].exchange(0);
        {
        static uint64_t f0 = 0, t0p = 0;
        const uint64_t f = latency_frames_total_.load(), tn = get_time_us();
        if (t0p) printf("PROF video pictures rendered: %.1f/s\n", (double)(f - f0) * 1e6 / (double)(tn - t0p));
        f0 = f; t0p = tn;
    }
    printf("PROF OSD frame wait before commit (2 s): <5:%u <10:%u <20:%u <35:%u <60:%u more:%u ms\n", a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    {
        static uint64_t cons0 = 0, sup0 = 0;
        const uint64_t c = dev->osd_consumed_, s = dev->osd_superseded_;
        printf("PROF OSD frames shown %llu, never shown (replaced before a flip carried them) %llu (2 s)\n",
               (unsigned long long)(c - cons0), (unsigned long long)(s - sup0));
        cons0 = c; sup0 = s;
    }
    printf("PROF frame start since last flip (2 s): <1:%d <2:%d <4:%d <8:%d <12:%d <17:%d more:%d ms\n",
           since_flip_hist_[0], since_flip_hist_[1], since_flip_hist_[2], since_flip_hist_[3], since_flip_hist_[4], since_flip_hist_[5], since_flip_hist_[6]);
    for (int& c : since_flip_hist_) c = 0;
    printf("PROF gallery frame intervals (2 s): <14:%d <19:%d <25:%d <40:%d <70:%d more:%d\n",
           gallery_dt_hist_[0], gallery_dt_hist_[1], gallery_dt_hist_[2], gallery_dt_hist_[3], gallery_dt_hist_[4], gallery_dt_hist_[5]);
    for (int& c : gallery_dt_hist_) c = 0;
    printf("PROF flips 10 s: n=%zu gaps <10:%d <14:%d <20:%d <30:%d <45:%d <70:%d <120:%d more:%d  gallery=%d frames-mode=%d\n",
           raw.size(), h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], gallery_.active() ? 1 : 0, dev->flips_are_frames ? 1 : 0);
    fflush(stdout);
}

// The frame just drawn is what is on the screen: copy it, then open the gallery.
// Called from render_gl after the HUD (or the idle screen) is drawn and before
// it is swapped out, so the copy is exactly what the pilot is looking at - and
// the gallery's first frame, with the live tile still filling the screen with
// this copy in it, is the same picture. The video itself stays on its own plane.
void OSD::gallery_capture_frame(int W, int H) {
    gallery_capture_ = false;
    if (gallery_.active()) return;
    if (!gallery_snap_tex_) glGenTextures(1, &gallery_snap_tex_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gallery_snap_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, W, H, 0);
    gallery_snap_valid_ = (glGetError() == GL_NO_ERROR);
    // Everything the first frames draw is made now, not in the middle of the
    // zoom, where building it made the first steps of the motion jump.
    gallery_prewarm();
    gallery_.open(DvrRecorder::instance().current_file());
    signal_render(prof::kWakeAnim);
}

void OSD::gallery_prewarm() {
    static const char* const words[] = {
        "GALLERY", "LIVE", "LAST FRAME", "NO VIDEO", "NO PREVIEW YET", "READING THE CARD",
        "NO RECORDINGS YET", "Receiving video", "Video stopped", "No video yet", "<", ">",
        "LEFT / RIGHT  BROWSE        ENTER  OPEN        BACK  LIVE",
        "LEFT / RIGHT  SEEK 5 S        ENTER  PAUSE        BACK  GALLERY",
        "PAUSED", "ENDED - ENTER TO REPLAY", "1 / 1", "0:00",
        "LEFT / RIGHT  BROWSE      ENTER  PLAY      DOWN  DELETE      BACK  LIVE",
        "CANCEL", "DELETE",
        "STATS", "Latency, frame pacing and link, live", "LATENCY", "SMOOTH", "FPS", "LOST",
        "MCS / SNR", "VIDEO", "LINK USE", "STALLS", "WORST", "latency distribution",
        "Cap", "Enc", "Net", "Rsm", "Dec", "Disp", "ms", "I", "F", "Mb", "link", "p50", "p99", "now",
        "0", "20", "40", "60", "80", "100", "-10 s", "-1 min", "-3 min", "WAITING FOR VIDEO",
        "pacing: n/a (needs vsync)", "--",
        "UP / DOWN  WINDOW 10 S      BACK  GALLERY",
        "UP / DOWN  WINDOW 1 MIN      BACK  GALLERY",
        "UP / DOWN  WINDOW 3 MIN      BACK  GALLERY",
    };
    for (const char* w : words) ensure_text_texture(w);
}

// One frame of the gallery. False once it has closed (the HUD draws instead).
bool OSD::render_gallery(int W, int H) {
    const uint64_t now = get_time_us();
    gallery_frame_++;
    static uint64_t prev_frame_us = 0;
    const float sdt = prev_frame_us ? std::min(0.05f, (float)(now - prev_frame_us) / 1e6f) : 1.0f / 60.0f;
    if (prof::enabled() && prev_frame_us) {
        const uint64_t dt = now - prev_frame_us;
        const int b = dt < 14000 ? 0 : dt < 19000 ? 1 : dt < 25000 ? 2 : dt < 40000 ? 3 : dt < 70000 ? 4 : 5;
        gallery_dt_hist_[b]++;
    }
    if (prof::enabled()) {
        const uint64_t lf = last_flip_seen_us_.load(std::memory_order_relaxed);
        const uint64_t d = now > lf ? now - lf : 0;
        since_flip_hist_[d < 1000 ? 0 : d < 2000 ? 1 : d < 4000 ? 2 : d < 8000 ? 3 : d < 12000 ? 4 : d < 17000 ? 5 : 6]++;
    }
    prev_frame_us = now;
    // The chart scrolls with time, and a frame's start time is not when it reaches the
    // screen: it jitters by a few milliseconds, which at the chart's speed is a pixel
    // back and forth. It runs on a clock that advances by the average frame interval
    // and is only gently pulled toward the real time.
    if (stats_clock_us_ == 0 || (now > stats_clock_us_ ? now - stats_clock_us_ : stats_clock_us_ - now) > 150000) {
        stats_clock_us_ = now;
        stats_dt_us_ = 16667.0;
    } else {
        stats_dt_us_ += ((double)sdt * 1e6 - stats_dt_us_) * 0.04;
        const uint64_t nxt = stats_clock_us_ + (uint64_t)(stats_dt_us_ + 0.08 * ((double)now - (double)stats_clock_us_ - stats_dt_us_));
        stats_clock_us_ = std::max(stats_clock_us_ + 1, nxt);
    }
    const uint64_t stats_clock = stats_clock_us_;
    const bool animating = gallery_.update(now);
    const Gallery::Snapshot s = gallery_.snapshot();
    service_clip_player(s);
    if (!gallery_.active() || !s.items) return false;

    const auto& items = *s.items;
    const int n = (int)items.size();
    const int live_i = Gallery::live_index(items);
    int stats_i = -1;
    for (int i = 0; i < n; i++) if (items[i].stats) stats_i = i;
    const bool stats_sel = stats_i >= 0 && s.sel == stats_i;
    const float u = (float)H / 1080.0f;                  // 1.0 at 1080p
    const float k = s.k, pt = s.play_t;
    const float strip_a = k * (1.0f - pt);               // what belongs to the strip alone
    const HudTheme& th = hud_theme_current();
    const float ink[3]    = { th.text[0],  th.text[1],  th.text[2]  };
    const float quiet[3]  = { th.quiet[0], th.quiet[1], th.quiet[2] };
    const float accent[3] = { th.accent[0], th.accent[1], th.accent[2] };
    const float ground[3] = { th.ground[0], th.ground[1], th.ground[2] };
    const float data[3]   = { th.data[0],  th.data[1],  th.data[2]  };
    const float tile_bg[3] = { ground[0] * 0.55f, ground[1] * 0.55f, ground[2] * 0.55f };

    // What the live picture is doing. Read the same way the HUD reads it.
    VideoState vs;
    uint64_t last_frame_us;
    pthread_mutex_lock(&osd_mutex);
    vs = video_state_;
    last_frame_us = last_fpv_frame_us_;
    pthread_mutex_unlock(&osd_mutex);
    const bool live_pic = (vs == VideoState::LIVE);
    const bool live_fresh = live_pic && now - last_frame_us < 1500000ULL;

    int px, py, pw, ph;
    dev->picture_rect(W, H, px, py, pw, ph);
    const Gallery::Rect full{ px + pw * 0.5f, py + ph * 0.5f, (float)pw, (float)ph };

    // ---- GL state: pixels, y up, premultiplied alpha ----
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(shader_program);
    const float mvp[16] = { 2.0f / W, 0, 0, 0,  0, 2.0f / H, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);
    if (a_alpha_factor_ != -1) glVertexAttrib1f(a_alpha_factor_, 1.0f);

    auto submit = [&](const float* verts, GLint pos_loc, GLint uv_loc) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 20 * sizeof(float), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(uv_loc);
        glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };
    // The charts' coloured triangles, gathered here and drawn in one go: x, y, then the
    // premultiplied colour, per vertex, as a single strip (flush() in the stats code builds
    // it). Anything drawn the ordinary way first draws what is gathered, so the order of
    // drawing is the order of the code.
    std::vector<float> fbatch;
    auto gflush = [&]() {
        if (fbatch.empty()) return;
        if (!gallery_flat_shader_) { fbatch.clear(); return; }
        if (!gallery_flat_vbo_) glGenBuffers(1, &gallery_flat_vbo_);
        glUseProgram(gallery_flat_shader_);
        glUniformMatrix4fv(flat_mvp_, 1, GL_FALSE, mvp);
        glBindBuffer(GL_ARRAY_BUFFER, gallery_flat_vbo_);
        glBufferData(GL_ARRAY_BUFFER, fbatch.size() * sizeof(float), fbatch.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray(flat_pos_);
        glVertexAttribPointer(flat_pos_, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(flat_col_);
        glVertexAttribPointer(flat_col_, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, (GLsizei)(fbatch.size() / 6));
        glDisableVertexAttribArray(flat_pos_);
        glDisableVertexAttribArray(flat_col_);
        glUseProgram(shader_program);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);
        fbatch.clear();
    };
    // Everything below is laid out from the top left with y down; these turn it
    // into GL's y up at the last step.
    auto fill = [&](float x, float y, float w, float h, const float* c, float a) {
        if (a <= 0.003f || w <= 0 || h <= 0) return;
        gflush();
        const float gy = H - (y + h);
        const float v[] = { x, gy, 0, 0, 0,  x + w, gy, 0, 1, 0,  x, gy + h, 0, 0, 1,  x + w, gy + h, 0, 1, 1 };
        // The shader multiplies (colour, alpha) by alpha: pre-scale the colour so
        // what lands is (c * a, a), a plain premultiplied fill.
        const float sa = std::sqrt(a);
        glUniform1i(u_is_text_, 0);
        glUniform1i(u_use_shading_, 0);
        glUniform4f(u_color_, c[0] * sa, c[1] * sa, c[2] * sa, 1.0f);
        glUniform1f(u_alpha_, sa);
        submit(v, a_pos_, a_uv_);
    };
    auto tex_quad = [&](GLuint tex, const Gallery::Rect& r, float a) {
        if (!tex || a <= 0.003f) return;
        gflush();
        const float x = r.x - r.w * 0.5f, gy = H - (r.y + r.h * 0.5f);
        const float v[] = { x, gy, 0, 0, 1,  x + r.w, gy, 0, 1, 1,  x, gy + r.h, 0, 0, 0,  x + r.w, gy + r.h, 0, 1, 0 };
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(u_tex_, 0);
        glUniform1i(u_is_text_, 1);
        glUniform1i(u_use_shading_, 0);
        glUniform4f(u_color_, 1, 1, 1, 1);
        glUniform1f(u_alpha_, a);
        submit(v, a_pos_, a_uv_);
    };
    auto ext_quad = [&](const Gallery::Rect& r, GLuint ext_tex) {
        if (!bg_ext_shader_ || !ext_tex) return;
        gflush();
        const float x = r.x - r.w * 0.5f, gy = H - (r.y + r.h * 0.5f);
        const float v[] = { x, gy, 0, 0, 1,  x + r.w, gy, 0, 1, 1,  x, gy + r.h, 0, 0, 0,  x + r.w, gy + r.h, 0, 1, 0 };
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_tex);
        glUseProgram(bg_ext_shader_);
        glUniformMatrix4fv(glGetUniformLocation(bg_ext_shader_, "mvp"), 1, GL_FALSE, mvp);
        glUniform1i(glGetUniformLocation(bg_ext_shader_, "tex"), 0);
        submit(v, glGetAttribLocation(bg_ext_shader_, "pos"), glGetAttribLocation(bg_ext_shader_, "uv"));
        glUseProgram(shader_program);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);
    };
    // The copy of the screen, premultiplied as the OSD plane holds it, drawn
    // over the rectangle r (centre-based, y down) that the whole screen maps to.
    auto pm_quad = [&](GLuint tex, const Gallery::Rect& r, float a) {
        if (!gallery_snap_shader_ || !tex) return;
        gflush();
        const float x = r.x - r.w * 0.5f, gy = H - (r.y + r.h * 0.5f);
        const float v[] = { x, gy, 0, 0, 0,  x + r.w, gy, 0, 1, 0,  x, gy + r.h, 0, 0, 1,  x + r.w, gy + r.h, 0, 1, 1 };
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUseProgram(gallery_snap_shader_);
        glUniformMatrix4fv(glGetUniformLocation(gallery_snap_shader_, "mvp"), 1, GL_FALSE, mvp);
        glUniform1i(glGetUniformLocation(gallery_snap_shader_, "tex"), 0);
        glUniform1f(glGetUniformLocation(gallery_snap_shader_, "alpha"), a);
        submit(v, glGetAttribLocation(gallery_snap_shader_, "pos"), glGetAttribLocation(gallery_snap_shader_, "uv"));
        glUseProgram(shader_program);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);
    };
    auto snap_quad = [&](const Gallery::Rect& r, float a) { pm_quad(gallery_snap_tex_, r, a); };
    // align: 0 left, 1 centre, 2 right of x. `baseline` is where the letters sit.
    auto text = [&](const std::string& str, float x, float baseline, float font_px, int align,
                    const float* c, float a) {
        if (str.empty() || a <= 0.003f) return;
        gflush();
        ensure_text_texture(str.c_str());
        auto it = text_cache.find(str.c_str());
        if (it == text_cache.end()) return;
        const float scale = font_px * 1.6f;                 // the texture is drawn at 40 px in 64
        const float tw = it->second.text_w / 64.0f * scale;
        const float tx = align == 1 ? x - tw * 0.5f : align == 2 ? x - tw : x;
        const float qw = scale * (float)it->second.w / (float)it->second.h;
        const float gy = H - (baseline + 0.25f * scale);
        const float v[] = { tx, gy, 0, 0, 1,  tx + qw, gy, 0, 1, 1,  tx, gy + scale, 0, 0, 0,  tx + qw, gy + scale, 0, 1, 0 };
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->second.tex);
        glUniform1i(u_tex_, 0);
        glUniform1i(u_is_text_, 1);
        glUniform1i(u_use_shading_, 0);
        glUniform4f(u_color_, c[0], c[1], c[2], 1.0f);
        glUniform1f(u_alpha_, a);
        submit(v, a_pos_, a_uv_);
    };
    auto text_width = [&](const std::string& str, float font_px) {
        return this->text_width(str.c_str(), font_px * 1.6f);
    };
    auto pill = [&](const std::string& str, float x, float y, float font_px, const float* bg, float bg_a,
                    const float* fg, float a) {
        const float pad = 0.5f * font_px, w = text_width(str, font_px) + 2 * pad, h = font_px * 1.7f;
        fill(x, y, w, h, bg, bg_a * a);
        text(str, x + pad, y + h * 0.5f + font_px * 0.36f, font_px, 0, fg, a);
        return w;
    };
    auto ring = [&](const Gallery::Rect& r, float t, const float* c, float a) {
        const float x = r.x - r.w * 0.5f, y = r.y - r.h * 0.5f;
        fill(x - t, y - t, r.w + 2 * t, t, c, a);
        fill(x - t, y + r.h, r.w + 2 * t, t, c, a);
        fill(x - t, y, t, r.h, c, a);
        fill(x + r.w, y, t, r.h, c, a);
    };

    // ---- the stats screen and its tile ----
    // The screen is laid out in a 680 x 383 box and scaled to whatever rectangle it is
    // drawn in, so one piece of code draws it full screen and while it grows out of
    // (or shrinks back into) its tile on the strip.
    //
    // It is drawn in two layers. What only changes when its numbers do (twice a second)
    // or when a scale moves - the text, the axes, the legend, the histogram - goes into
    // a texture of its own that is used as it is until then; what moves with time - the
    // charts - is drawn every frame, as one batch of coloured triangles. A frame that
    // drew all of it, every time, took ~10 ms, and one that is not done well before the
    // next video flip misses it and reaches the screen a whole frame late.
    static const float kStageCol[6][3] = {
        {0.31f, 0.56f, 0.84f}, {1.00f, 0.42f, 0.42f}, {0.10f, 0.83f, 0.64f},
        {0.71f, 0.44f, 0.91f}, {0.95f, 0.79f, 0.30f}, {1.00f, 0.54f, 0.30f} };
    static const char* const kStageName[6] = { "Cap", "Enc", "Air+Net", "Rsm", "Dec", "Disp" };
    static const char* const kWinTime[3]   = { "-10 s", "-1 min", "-3 min" };
    static const char* const kWinHint[3]   = {
        "UP / DOWN  WINDOW 10 S      BACK  GALLERY",
        "UP / DOWN  WINDOW 1 MIN      BACK  GALLERY",
        "UP / DOWN  WINDOW 3 MIN      BACK  GALLERY" };
    float stats_fr = 0.0f;       // how far the chart has scrolled past its last rebuild, in columns
    const float lime[3]  = { 0.70f, 1.00f, 0.00f };
    const float amber[3] = { 0.95f, 0.79f, 0.30f };
    const float red[3]   = { 1.00f, 0.30f, 0.30f };
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    const float panel[3] = { ground[0] * 0.45f, ground[1] * 0.45f, ground[2] * 0.45f };
    const float* const kPaceCol[4] = { lime, amber, red, quiet };

    // Shapes are built as positions in vb, then given a colour and appended to the batch
    // (see gflush) by flush(): rectangles (six vertices each) and strips alike.
    std::vector<float> vb;
    auto flush = [&](GLenum mode, const float* c, float a) {
        if (a > 0.003f && vb.size() >= 15) {
            const float r = c[0] * a, g = c[1] * a, bl = c[2] * a;      // premultiplied
            auto push = [&](const float* p) {
                fbatch.push_back(p[0]); fbatch.push_back(p[1]);
                fbatch.push_back(r); fbatch.push_back(g); fbatch.push_back(bl); fbatch.push_back(a);
            };
            // Joined to what the batch holds by degenerate triangles: the last vertex again,
            // and the first of the new piece.
            auto join = [&](const float* p) {
                if (fbatch.empty()) return;
                const size_t n = fbatch.size();
                for (int k = 0; k < 6; k++) fbatch.push_back(fbatch[n - 6 + k]);
                push(p);
            };
            if (mode == GL_TRIANGLES) {         // rectangles: TL TR BL TR BR BL
                for (size_t i = 0; i + 30 <= vb.size(); i += 30) {
                    const float* q = &vb[i];
                    join(q);
                    push(q); push(q + 5); push(q + 10); push(q + 20);
                }
            } else {                            // one triangle strip
                join(&vb[0]);
                for (size_t i = 0; i + 5 <= vb.size(); i += 5) push(&vb[i]);
            }
        }
        vb.clear();
    };
    auto vtx = [&](float x, float y_down) {
        vb.push_back(x); vb.push_back((float)H - y_down); vb.push_back(0); vb.push_back(0); vb.push_back(0);
    };
    auto rect_v = [&](float x, float y, float w, float h) {
        vtx(x, y); vtx(x + w, y); vtx(x, y + h); vtx(x + w, y); vtx(x + w, y + h); vtx(x, y + h);
    };
    auto dashed_h = [&](float xa, float xb, float y, float t, float dash, float gap) {
        for (float x = xa; x < xb; x += dash + gap) rect_v(x, y - t * 0.5f, std::min(dash, xb - x), t);
    };
    auto dashed_v = [&](float x, float ya, float yb, float t, float dash, float gap) {
        for (float y = ya; y < yb; y += dash + gap) rect_v(x - t * 0.5f, y, t, std::min(dash, yb - y));
    };

    // Positions in the screen's box, shared by the two layers.
    const float kx0 = 42, kx1 = 418, kchY = 86, kchH = 128, kbase = kchY + kchH;   // latency chart
    const float klb = 352, klh = 58;                                              // link lane
    const float khx = 464, khy = 156, khw = 208, khh = 56;                        // histogram

    // ---- the layer that changes twice a second ----
    auto stats_layer_static = [&](float ox, float oy, float m, float A) {
        const StatsView& sh = stats_shown_;
        const int wi = sh.window_s >= 180 ? 2 : sh.window_s >= 60 ? 1 : 0;
        auto X = [&](float a) { return ox + a * m; };
        auto Yd = [&](float a) { return oy + a * m; };
        const float fs = m * 0.8f;                  // font px per font unit of the 680 x 383 box
        char b[96];
        fill(ox, oy, 680.0f * m, 383.0f * m, panel, A);

        // header: the median, and the figures that say how the picture is doing
        text("LATENCY", X(10), Yd(16), 11 * fs, 0, quiet, A);
        snprintf(b, sizeof(b), "%.1f", sh.p50);
        text(b, X(10), Yd(46), 30 * fs, 0, ink, A);
        text("ms", X(10) + text_width(b, 30 * fs) + 6 * m, Yd(46), 13 * fs, 0, quiet, A);
        snprintf(b, sizeof(b), "p50 %.1f   p99 %.1f", sh.p50, sh.p99);
        text(b, X(10), Yd(62), 11 * fs, 0, quiet, A);
        auto chip = [&](float x, const char* label, const char* val) {
            text(label, X(x), Yd(16), 11 * fs, 0, quiet, A);
            text(val, X(x), Yd(40), 16 * fs, 0, ink, A);
        };
        snprintf(b, sizeof(b), "%.1f", sh.fps_now);        // now, not the window's average
        chip(150, "FPS", b);
        text("SMOOTH", X(200), Yd(16), 11 * fs, 0, quiet, A);
        fill(X(200), Yd(28), 9 * m, 9 * m, !sh.pacing ? quiet : sh.good_pct >= 99.0f ? lime : sh.good_pct >= 95.0f ? amber : red, A);
        if (sh.pacing) snprintf(b, sizeof(b), "%.1f%%", sh.good_pct); else snprintf(b, sizeof(b), "--");
        text(b, X(214), Yd(40), 16 * fs, 0, ink, A);
        snprintf(b, sizeof(b), "%u", (unsigned)sh.lost_total);
        chip(280, "LOST", b);
        snprintf(b, sizeof(b), "%d / %d", sh.mcs_now, (int)sh.snr_now);
        chip(328, "MCS / SNR", b);
        snprintf(b, sizeof(b), "%.1f", sh.video_now);
        chip(396, "VIDEO", b);

        // the latency chart's axes
        const float mx = stats_ymax_;
        auto Yl = [&](float ms) { return Yd(kbase - std::min(ms, mx) / mx * kchH); };
        // A line and a label every 5 ms, the tens a little stronger.
        const int ms_top = (int)(mx + 0.01f);
        for (int ms = 5; ms <= ms_top; ms += 5)
            if (ms % 10) dashed_h(X(kx0), X(kx1), Yl((float)ms), 0.8f * m, 3 * m, 5 * m);
        flush(GL_TRIANGLES, data, 0.12f * A);
        for (int ms = 10; ms <= ms_top; ms += 10)
            dashed_h(X(kx0), X(kx1), Yl((float)ms), 1.0f * m, 3 * m, 5 * m);
        flush(GL_TRIANGLES, data, 0.25f * A);
        for (int ms = 0; ms <= ms_top; ms += 5) {
            snprintf(b, sizeof(b), "%d", ms);
            text(b, X(kx0 - 5), Yl((float)ms) + 3.2f * m, 8.5f * fs, 2, quiet, A);
        }
        text("I", X(kx0 - 5), Yd(227), 11 * fs, 2, quiet, A);
        text("F", X(kx0 - 5), Yd(248), 11 * fs, 2, quiet, A);
        if (sh.pacing)
            snprintf(b, sizeof(b), "pacing: on time %.1f%%   p99 gap %.1f ms   stutters %d", sh.good_pct, sh.p99_gap_ms, sh.stutters);
        else
            snprintf(b, sizeof(b), "pacing: n/a (needs vsync)");
        text(b, X(kx0), Yd(270), 11 * fs, 0, quiet, A);

        // the link lane: what it means, said in words - the scale in Mbps (a line at the
        // top, one halfway) and a key with the numbers as they are now
        const float lmx = stats_lmax_;
        auto Yk = [&](float mbps) { return Yd(klb - std::min(mbps, lmx) / lmx * klh); };
        {
            // A step every 5 Mbps (every 10 or 20 once the scale is too big for the lane to
            // hold that many labels), the round ones a little stronger.
            const int step = lmx <= 40.5f ? 5 : lmx <= 80.5f ? 10 : 20;
            for (int mb = step; mb <= (int)(lmx + 0.5f); mb += step)
                if (mb % 20) dashed_h(X(kx0), X(kx1), Yk((float)mb), 0.8f * m, 3 * m, 5 * m);
            flush(GL_TRIANGLES, quiet, 0.2f * A);
            for (int mb = step; mb <= (int)(lmx + 0.5f); mb += step)
                if (mb % 20 == 0) dashed_h(X(kx0), X(kx1), Yk((float)mb), 1.0f * m, 3 * m, 5 * m);
            flush(GL_TRIANGLES, quiet, 0.4f * A);
            text("Mbps", X(10), Yd(287), 11 * fs, 0, quiet, A);
            for (int mb = 0; mb <= (int)(lmx + 0.5f); mb += step) {
                snprintf(b, sizeof(b), "%d", mb);
                text(b, X(kx0 - 5), Yk((float)mb) + 3.2f * m, 9 * fs, 2, quiet, A);
            }
        }
        {
            float kx = kx0;
            auto key = [&](const char* label, const char* val, const float* col, int shape) {
                // shape 0: a filled swatch (an area), 1: a line, 2: a tick
                if (shape == 0)      fill(X(kx), Yd(279), 10 * m, 8 * m, col, 0.55f * A);
                else if (shape == 1) fill(X(kx), Yd(282.5f), 10 * m, 1.6f * m, col, A);
                else                 fill(X(kx + 4), Yd(278), 2.4f * m, 9 * m, col, A);
                text(label, X(kx + 14), Yd(287), 11 * fs, 0, ink, A);
                const float w = text_width(label, 11 * fs) / m;
                if (val[0]) text(val, X(kx + 14 + w + 4), Yd(287), 11 * fs, 0, quiet, A);
                kx += 14 + w + 4 + (val[0] ? text_width(val, 11 * fs) / m : 0.0f) + 14;
            };
            snprintf(b, sizeof(b), "%.1f", sh.video_now);
            key("video rate", b, data, 0);
            snprintf(b, sizeof(b), "%.1f", sh.link_now);
            key("link capacity", b, white, 1);
            key("lost packets", "", red, 2);
        }
        {
            // The two lines are named where they end, the lower one pushed down if they meet.
            const float yl = Yk(std::max(sh.link_now, 0.0f)) + 4 * m;
            const float yv = std::max(Yk(std::max(sh.video_now, 0.0f)) + 4 * m, yl + 12 * m);
            text("link", X(kx1 + 4), yl, 11 * fs, 0, white, A);
            text("video", X(kx1 + 4), std::min(yv, Yd(klb) + 4 * m), 11 * fs, 0, data, A);
        }
        text(kWinTime[wi], X(kx0), Yd(370), 11 * fs, 0, quiet, A);
        text("now", X(kx1), Yd(370), 11 * fs, 2, quiet, A);

        // right column: the distribution, a few figures, the key
        text("latency distribution", X(khx), Yd(khy - 4), 11 * fs, 0, quiet, A);
        {
            uint32_t hm = 1;
            for (int i = 0; i < StatsView::kBins; i++) hm = std::max(hm, sh.hist[i]);
            const float bw = khw / (float)StatsView::kBins;
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < StatsView::kBins; i++) {
                    const bool tail = (float)((i + 1) * 2) > sh.p99;
                    if (!sh.hist[i] || tail != (pass == 1)) continue;
                    const float bh = (float)sh.hist[i] / (float)hm * khh;
                    rect_v(X(khx + (float)i * bw), Yd(khy + khh - bh), (bw - 1.0f) * m, bh * m);
                }
                flush(GL_TRIANGLES, pass ? amber : data, 0.65f * A);
            }
            rect_v(X(khx), Yd(khy + khh), khw * m, 0.8f * m);
            // a tick and a label every 5 ms
            for (int ms = 0; ms <= 60; ms += 5)
                rect_v(X(khx + (float)ms / 60.0f * khw) - 0.5f * m, Yd(khy + khh), 1.0f * m, (ms % 20 ? 3.0f : 5.0f) * m);
            flush(GL_TRIANGLES, quiet, 0.6f * A);
            for (int ms = 0; ms <= 60; ms += 5) {
                snprintf(b, sizeof(b), "%d", ms);
                text(b, X(khx + (float)ms / 60.0f * khw), Yd(khy + khh + 13), 9.5f * fs, ms == 60 ? 2 : ms == 0 ? 0 : 1, quiet, A);
            }
        }
        auto readout = [&](float x, float y, const char* label, const char* val) {
            text(label, X(x), Yd(y), 11 * fs, 0, quiet, A);
            text(val, X(x), Yd(y + 22), 19 * fs, 0, ink, A);
        };
        snprintf(b, sizeof(b), "%.0f%%", sh.link_use);
        readout(464, 246, "LINK USE", b);
        snprintf(b, sizeof(b), "%u", (unsigned)sh.lost_total);
        readout(560, 246, "LOST", b);
        snprintf(b, sizeof(b), "%d", sh.stutters);
        readout(464, 284, "STALLS", b);
        snprintf(b, sizeof(b), "%.0f ms", sh.worst_ms);
        readout(560, 284, "WORST", b);
        // The stages, each with its median over the window in ms.
        text("stage medians, ms", X(464), Yd(324), 11 * fs, 0, quiet, A);
        // Only the stages this link reports: one that reads zero all the time is left out.
        int shown = 0;
        for (int i = 0; i < 6; i++) {
            if (sh.stage_p50[i] < 0.05f) continue;
            const float lx = 464 + (float)(shown % 2) * 104, ly = 338 + (float)(shown / 2) * 14;
            fill(X(lx), Yd(ly - 9), 9 * m, 9 * m, kStageCol[i], A);
            snprintf(b, sizeof(b), "%s (%.1f)", kStageName[i], sh.stage_p50[i]);
            text(b, X(lx + 13), Yd(ly), 11 * fs, 0, ink, A);
            shown++;
        }
        text(kWinHint[wi], X(676), Yd(380), 11 * fs, 2, quiet, A);
        gflush();
    };

    // ---- the layer that moves with time: every frame, one batch ----
    auto stats_layer_dynamic = [&](float ox, float oy, float m, float A) {
        const StatsView& v = stats_view_;
        const int C = StatsView::kCols, Cd = C - 1;
        auto X = [&](float a) { return ox + a * m; };
        auto Yd = [&](float a) { return oy + a * m; };
        const float fs = m * 0.8f;
        char b[96];
        // The columns sit on a time grid: the chart scrolls by the part of a column that
        // has passed since the view was built, and the newest column - still filling -
        // is left out.
        const float colw = (kx1 - kx0) / (float)C;
        auto cxu = [&](int i) { return kx1 - ((float)C - 1.5f - (float)i + stats_fr) * colw; };
        auto colx = [&](int i) { return X(cxu(i)); };
        // A cell is cut at the chart's ends rather than drawn past them.
        auto rect_c = [&](float xl, float xr, float y, float h) {
            xl = std::max(xl, X(kx0)); xr = std::min(xr, X(kx1));
            if (xr > xl) rect_v(xl, y, xr - xl, h);
        };
        const float mx = stats_ymax_;
        auto Yl = [&](float ms) { return Yd(kbase - std::min(ms, mx) / mx * kchH); };
        const float lmx = stats_lmax_;
        auto Yk = [&](float mbps) { return Yd(klb - std::min(mbps, lmx) / lmx * klh); };

        // the link's MCS over the top, a band per run (named after the geometry)
        struct Label { float x; int mcs; } mlab[16];
        int nlab = 0;
        {
            int i = 0;
            while (i < Cd) {
                if (!v.have[i]) { i++; continue; }
                int j = i;
                while (j + 1 < Cd && (!v.have[j + 1] || v.mcs[j + 1] == v.mcs[i])) j++;
                const float xa = i == 0 ? kx0 : std::max(kx0, cxu(i) - 0.5f * colw);     // the ends are the chart's
                const float xb = j == Cd - 1 ? kx1 : std::min(kx1, cxu(j) + 0.5f * colw);
                if (xb > xa) {
                    // The colour follows the rate, not the run, so a band keeps its colour as the chart scrolls.
                    rect_v(X(xa), Yd(70), (xb - xa - 0.6f) * m, 12 * m);
                    flush(GL_TRIANGLES, (v.mcs[i] & 1) ? amber : lime, 0.22f * A);
                    if (xb - xa > 36 && nlab < 16) mlab[nlab++] = { xa, v.mcs[i] };
                }
                i = j + 1;
            }
        }
        // six stages stacked, the total over them
        {
            float acc[StatsView::kCols];
            for (int i = 0; i < C; i++) acc[i] = 0;
            for (int k2 = 0; k2 < 6; k2++) {
                for (int i = 0; i < Cd; i++) {
                    const float lo = acc[i], hi = lo + (v.have[i] ? v.stage[k2][i] : 0.0f);
                    // Held out to the chart's edges: its ends do not move with the columns.
                    if (i == 0)      { vtx(X(kx0), Yl(lo)); vtx(X(kx0), Yl(hi)); }
                    vtx(colx(i), Yl(lo)); vtx(colx(i), Yl(hi));
                    if (i == Cd - 1) { vtx(X(kx1), Yl(lo)); vtx(X(kx1), Yl(hi)); }
                    acc[i] = hi;
                }
                flush(GL_TRIANGLE_STRIP, kStageCol[k2], 0.8f * A);
            }
        }
        for (int i = 0; i < Cd; i++) {
            const float y = v.have[i] ? Yl(v.total[i]) : Yd(kbase);
            if (i == 0)      { vtx(X(kx0), y - 0.6f * m); vtx(X(kx0), y + 0.6f * m); }
            vtx(colx(i), y - 0.6f * m); vtx(colx(i), y + 0.6f * m);
            if (i == Cd - 1) { vtx(X(kx1), y - 0.6f * m); vtx(X(kx1), y + 0.6f * m); }
        }
        flush(GL_TRIANGLE_STRIP, white, 0.9f * A);
        dashed_h(X(kx0), X(kx1), Yl(stats_p50_), 1.2f * m, 7 * m, 4 * m);
        flush(GL_TRIANGLES, data, A);
        dashed_h(X(kx0), X(kx1), Yl(stats_p99_), 1.2f * m, 7 * m, 4 * m);
        flush(GL_TRIANGLES, amber, A);
        // the worst moment, if it stands out from the rest
        const bool worst = v.worst_col >= 0 && v.tmax[v.worst_col] > std::max(2.2f * v.p50, v.p50 + 15.0f);
        float worst_x = 0;
        if (worst) {
            worst_x = colx(v.worst_col);
            dashed_v(worst_x, Yd(82), Yd(kbase), 1.2f * m, 4 * m, 3 * m);
            flush(GL_TRIANGLES, red, A);
        }
        // keyframes, and how evenly the pictures reached the screen
        // (Columns wider than the gap between keyframes all hold one: a solid bar says nothing.)
        {
            int keys = 0, have_cols = 0;
            for (int i = 0; i < C; i++) { keys += v.key[i] ? 1 : 0; have_cols += v.have[i] ? 1 : 0; }
            if (keys * 10 < have_cols * 7)
                for (int i = 0; i < C; i++) if (v.key[i]) rect_c(colx(i) - 1.0f * m, colx(i) + 1.0f * m, Yd(220), 7 * m);
        }
        flush(GL_TRIANGLES, quiet, A);
        const float cw = colw * m;
        for (int cls = 0; cls < 4; cls++) {
            for (int i = 0; i < (cls == StatsView::kNoData ? Cd : C); i++)     // the filling column shows what has happened in it, never a blank
                if (v.pace[i] == cls) rect_c(colx(i) - cw * 0.5f, colx(i) + cw * 0.5f + 0.4f, Yd(234), 18 * m);
            flush(GL_TRIANGLES, kPaceCol[cls], (cls == 0 ? 0.75f : cls == 3 ? 0.18f : 1.0f) * A);
        }
        // the link: the video's rate against what the link carries, and the packets it lost
        for (int i = 0; i < Cd; i++) {
            const float y = v.have[i] ? Yk(v.video[i]) : Yd(klb);
            if (i == 0)      { vtx(X(kx0), y); vtx(X(kx0), Yd(klb)); }
            vtx(colx(i), y); vtx(colx(i), Yd(klb));
            if (i == Cd - 1) { vtx(X(kx1), y); vtx(X(kx1), Yd(klb)); }
        }
        flush(GL_TRIANGLE_STRIP, data, 0.5f * A);
        for (int i = 0; i < Cd; i++) {
            const float y = v.have[i] ? Yk(v.link[i]) : Yd(klb);
            if (i == 0)      { vtx(X(kx0), y - 0.5f * m); vtx(X(kx0), y + 0.5f * m); }
            vtx(colx(i), y - 0.5f * m); vtx(colx(i), y + 0.5f * m);
            if (i == Cd - 1) { vtx(X(kx1), y - 0.5f * m); vtx(X(kx1), y + 0.5f * m); }
        }
        flush(GL_TRIANGLE_STRIP, white, 0.7f * A);
        for (int i = 0; i < C; i++) if (v.lost[i]) rect_c(colx(i) - 1.5f * m, colx(i) + 1.5f * m, Yd(klb - 8), 8 * m);
        flush(GL_TRIANGLES, red, A);
        // the p50 / p99 marks on the histogram
        {
            const float px50 = std::min(khw, stats_p50_ / 60.0f * khw), px99 = std::min(khw, stats_p99_ / 60.0f * khw);
            dashed_v(X(khx + px50), Yd(khy), Yd(khy + khh), 1.2f * m, 4 * m, 3 * m);
            flush(GL_TRIANGLES, data, A);
            dashed_v(X(khx + px99), Yd(khy), Yd(khy + khh), 1.2f * m, 4 * m, 3 * m);
            flush(GL_TRIANGLES, amber, A);
        }
        gflush();
        // what is written over the data
        for (int i = 0; i < nlab; i++) {
            snprintf(b, sizeof(b), "MCS %d", mlab[i].mcs);
            text(b, X(mlab[i].x + 5), Yd(80), 11 * fs, 0, quiet, A);
        }
        text("p50", X(kx1 + 4), Yl(stats_p50_) + 4 * m, 11 * fs, 0, data, A);
        text("p99", X(kx1 + 4), Yl(stats_p99_) + 4 * m, 11 * fs, 0, amber, A);
        if (worst) {
            snprintf(b, sizeof(b), "worst %.0f ms", v.tmax[v.worst_col]);
            const bool left = worst_x > X(kx1 - 70);
            text(b, left ? worst_x - 4 * m : worst_x + 4 * m, Yd(kchY + 11), 11 * fs, left ? 2 : 0, red, A);
        }
    };

    // The screen: the cached layer if it is up to date and the screen is where it was drawn
    // (settled), else drawn afresh (while it grows out of its tile); the charts over it.
    auto draw_stats_screen = [&](float ox, float oy, float m, float A) {
        if (A <= 0.003f) return;
        // The slow layer is always drawn for the full-screen layout and used at any size: on
        // the strip the screen is the same one, scaled into its tile.
        const float fox = (float)px, foy = (float)py, fm = (float)pw / 680.0f;
        bool cached = false;
        if (gallery_flat_shader_) {
            StatsLayerKey key;
            key.win = stats_shown_.window_s;
            key.shown_us = stats_shown_us_;
            key.ymax_q = (int)std::lround(stats_ymax_ * 4.0f);
            key.lmax_q = (int)std::lround(stats_lmax_ * 4.0f);
            key.W = W; key.H = H;
            key.ox_q = (int)std::lround(fox * 4.0f); key.oy_q = (int)std::lround(foy * 4.0f);
            key.m_q = (int)std::lround(fm * 1000.0f);
            key.theme_q = (int)((ink[0] + 2 * ink[1] + 3 * ink[2] + 5 * quiet[0] + 7 * ground[0] + 11 * data[1]) * 1000.0f);
            if (!stats_layer_valid_ || !(key == stats_layer_key_)) {
                if (!stats_fbo_) glGenFramebuffers(1, &stats_fbo_);
                if (!stats_layer_tex_) glGenTextures(1, &stats_layer_tex_);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, stats_layer_tex_);
                if (stats_layer_w_ != W || stats_layer_h_ != H) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    stats_layer_w_ = W; stats_layer_h_ = H;
                }
                gflush();
                glBindFramebuffer(GL_FRAMEBUFFER, stats_fbo_);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stats_layer_tex_, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                    glViewport(0, 0, W, H);
                    glClearColor(0, 0, 0, 0);
                    glClear(GL_COLOR_BUFFER_BIT);
                    stats_layer_static(fox, foy, fm, 1.0f);
                    stats_layer_key_ = key;
                    stats_layer_valid_ = true;
                } else {
                    stats_layer_valid_ = false;
                    static bool warned = false;
                    if (!warned) { warned = true; fprintf(stderr, "stats layer: framebuffer incomplete\n"); }
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, W, H);
                glUseProgram(shader_program);
            }
            cached = stats_layer_valid_;
        }
        if (cached) {
            // The whole-screen texture, scaled and placed so its screen box lands on (ox, oy, m).
            const float sc = m / fm;
            pm_quad(stats_layer_tex_, Gallery::Rect{ ox + (W * 0.5f - fox) * sc, oy + (H * 0.5f - foy) * sc, W * sc, H * sc }, A);
        } else stats_layer_static(ox, oy, m, A);
        stats_layer_dynamic(ox, oy, m, A);
        // The live picture in its slot, drawn from the same frames the video plane shows.
        {
            const Gallery::Rect pip{ ox + (464.0f + 104.0f) * m, oy + (10.0f + 58.5f) * m, 208.0f * m, 117.0f * m };
            if (live_pic && import_live_picture()) {
                ext_quad(pip, live_ext_tex_);
                if (A < 0.999f) fill(pip.x - pip.w * 0.5f, pip.y - pip.h * 0.5f, pip.w, pip.h, panel, 1.0f - A);
            } else {
                fill(pip.x - pip.w * 0.5f, pip.y - pip.h * 0.5f, pip.w, pip.h, tile_bg, A);
            }
            ring(pip, 3 * m, accent, A);
            if (live_pic) pill(live_fresh ? "LIVE" : "LAST FRAME", pip.x - pip.w * 0.5f + 4 * m, pip.y - pip.h * 0.5f + 4 * m,
                               8 * m, live_fresh ? accent : quiet, 0.95f, ground, A);
        }
    };

    // Whichever the selection is doing: the tile, or the screen growing out of it.
    auto draw_stats = [&]() {
        if (stats_i < 0) return;
        const Gallery::Rect r = Gallery::tile_rect(s, stats_i, W, H, full);
        if (r.x + r.w * 0.5f < 0 || r.x - r.w * 0.5f > W) return;
        {
            const StatsView& v = stats_view_;
            stats_fr = v.col_us ? std::max(0.0f, std::min(3.0f, (float)((double)((int64_t)stats_clock - (int64_t)v.grid_us) / (double)v.col_us))) : 0.0f;
            // The scales follow what the data needs, eased, so a new worst moment does
            // not make the whole chart jump.
            const float ytgt = std::max(40.0f, std::min(100.0f, std::ceil(stats_shown_.p99 * 1.3f / 10.0f) * 10.0f));
            float ltgt = 40.0f;
            for (int i = 0; i < StatsView::kCols - 1; i++)
                if (v.have[i]) ltgt = std::max(ltgt, std::max(v.video[i], v.link[i]));
            ltgt = std::ceil(ltgt / 20.0f) * 20.0f;
            const float ease = 1.0f - std::exp(-sdt / 0.25f);
            stats_p50_ = stats_p50_ <= 0 ? stats_shown_.p50 : stats_p50_ + (stats_shown_.p50 - stats_p50_) * ease;
            stats_p99_ = stats_p99_ <= 0 ? stats_shown_.p99 : stats_p99_ + (stats_shown_.p99 - stats_p99_) * ease;
            stats_ymax_ = stats_ymax_ <= 0 ? ytgt : stats_ymax_ + (ytgt - stats_ymax_) * ease;
            stats_lmax_ = stats_lmax_ <= 0 ? ltgt : stats_lmax_ + (ltgt - stats_lmax_) * ease;
        }
        const bool zoomed = stats_sel && pt > 0.003f;
        const float dist = std::min(1.0f, std::fabs((float)stats_i - s.scroll));
        const float a = zoomed ? 1.0f : strip_a * (1.0f - 0.45f * dist);
        draw_stats_screen(r.x - r.w * 0.5f, r.y - r.h * 0.5f, r.w / 680.0f, a);
        gflush();
        if (stats_sel && !zoomed) ring(r, 4 * u, accent, a);
    };

    // A deleted recording's thumbnail leaves the GPU with it.
    if (s.gap > 0.003f) {
        for (auto it = gallery_thumbs_.begin(); it != gallery_thumbs_.end();) {
            bool present = false;
            for (const auto& item : items) if (item.name == it->first) { present = true; break; }
            if (present) { ++it; continue; }
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = gallery_thumbs_.erase(it);
        }
    }

    // ---- backdrop: fades in as the picture shrinks ----
    fill(0, 0, (float)W, (float)H, ground, std::min(1.0f, k * 1.4f));

    // ---- the recordings ----
    int thumb_loads = 1;     // thumbnails decoded this frame, at most
    auto draw_clip = [&](int i) {
        const Gallery::Rect r = Gallery::tile_rect(s, i, W, H, full);
        const float dist = std::min(1.0f, std::fabs((float)i - s.scroll));
        const bool zooming = (i == s.sel && pt > 0);
        const float a = zooming ? 1.0f : strip_a * (1.0f - 0.45f * dist);
        if (r.x + r.w * 0.5f < 0 || r.x - r.w * 0.5f > W || a <= 0.003f) return;
        const float x = r.x - r.w * 0.5f, y = r.y - r.h * 0.5f;
        const float black[3] = { 0, 0, 0 };
        fill(x, y, r.w, r.h, zooming ? black : tile_bg, zooming ? 1.0f : a);

        const bool near_screen = std::fabs((float)i - s.scroll) < 2.2f;
        int tw = 0, thh = 0;
        bool loaded = false;
        const GLuint tex = near_screen ? gallery_thumb(items[i].name, tw, thh, thumb_loads > 0, loaded) : 0;
        if (loaded) thumb_loads--;
        const bool playing_here = zooming && s.mode == Gallery::Mode::Playing && clip_have_frame_ &&
                                  clip_player_ && clip_player_->width() > 0;
        if (playing_here) {
            ext_quad(fit(r, (float)clip_player_->width() / (float)clip_player_->height()), clip_ext_tex_);
        } else if (tex) {
            tex_quad(tex, fit(r, (float)tw / (float)thh), a);
        } else {
            text("NO PREVIEW YET", r.x, r.y + 5 * u, 22 * u * (r.w / (0.6f * W)) * 1.4f, 1, quiet, a);
        }
        if (!zooming && r.w > 0) {
            // length, bottom right
            const std::string d = Gallery::duration_label(items[i].duration);
            const float fs = 22 * u;
            const float pw_ = text_width(d, fs) + fs;
            fill(x + r.w - pw_ - 8 * u, y + r.h - fs * 1.7f - 8 * u, pw_, fs * 1.7f, ground, 0.8f * a);
            text(d, x + r.w - pw_ * 0.5f - 8 * u, y + r.h - 8 * u - fs * 0.55f, fs, 1, ink, a);
        }
        if (i == s.sel && !zooming) ring(r, 4 * u, accent, a);
    };
    // ---- the live tile ----
    // Its hole is cleared, so it has to be drawn before anything that must cover
    // it: a recording filling the screen is drawn over the live tile, and drawn
    // after it. Otherwise the tile (a neighbour of the recording on the strip)
    // cuts a window through the recording.
    // The live tile stays where the strip has it. The stats screen draws the picture again
    // in its own slot (from the same decoded frames), so the video plane never has to move.
    const bool stats_zoomed = stats_sel && pt > 0.003f;
    const Gallery::Rect lr = Gallery::tile_rect(s, live_i, W, H, full);
    auto draw_live = [&]() {
        const bool live_selected = (s.sel == live_i);
        const float lx = lr.x - lr.w * 0.5f, ly = lr.y - lr.h * 0.5f;
        DrmDevice::Tile t;
        t.active = true;
        t.x = (int)std::lround(lx);
        t.y = (int)std::lround(ly);
        t.w = (int)std::lround(lr.w);
        t.h = (int)std::lround(lr.h);
        int x0, y0, x1, y1;
        const bool visible = DrmDevice::tile_on_screen(t, W, H, x0, y0, x1, y1);
        if (live_pic) {
            frame_tile_ = t;
            if (visible) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(x0, H - y1, x1 - x0, y1 - y0);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);   // the hole the picture shows through
                glDisable(GL_SCISSOR_TEST);
            }
        } else if (visible) {
            fill(lx, ly, lr.w, lr.h, tile_bg, 1.0f);   // nothing on the video plane: a base for the copy
        }
        if (gallery_snap_valid_) {
            // What was on the screen when Back was pressed - the HUD over the
            // picture, or the idle screen - shrinks and slides with the tile:
            // the whole screen, mapped by the same move that takes the picture's
            // full-screen rectangle to the tile, and cut to the tile.
            if (visible && full.w > 0 && full.h > 0) {
                const float sx = lr.w / full.w, sy = lr.h / full.h;
                const Gallery::Rect dest{ lr.x + (W * 0.5f - full.x) * sx, lr.y + (H * 0.5f - full.y) * sy,
                                          W * sx, H * sy };
                glEnable(GL_SCISSOR_TEST);
                glScissor(x0, H - y1, x1 - x0, y1 - y0);
                snap_quad(dest, 1.0f);
                glDisable(GL_SCISSOR_TEST);
            }
        } else if (!live_pic) {
            text("NO VIDEO", lr.x, lr.y + 8 * u, 30 * u * (lr.w / (0.6f * W)) * 1.4f, 1, quiet, k);
        }
        if (live_selected && pt <= 0) ring(lr, 4 * u, accent, k);
        // the badge: only on a tile, not on the picture filling the screen
        const std::string tag = !live_pic ? "" : (live_fresh ? "LIVE" : "LAST FRAME");
        if (!tag.empty() && strip_a > 0.003f)
            pill(tag, lx + 8 * u, ly + 8 * u, 22 * u, live_fresh ? accent : quiet, 0.95f, ground, strip_a);
    };

    // A recording or the stats screen filling the screen covers the live tile (and its
    // hole), so the tile goes first.
    const bool stats_visible = stats_i >= 0 && (stats_zoomed || std::fabs((float)stats_i - s.scroll) < 1.6f);
    if (stats_visible) stats_refresh(s.stats_window, stats_clock);
    const bool recording_zoomed = pt > 0.003f && !stats_sel;
    const bool live_first = recording_zoomed || stats_zoomed;
    if (live_first) draw_live();
    for (int i = 0; i < live_i; i++)
        if (i != s.sel) draw_clip(i);
    if (s.sel < live_i) draw_clip(s.sel);
    draw_stats();
    if (!live_first) draw_live();

    // ---- words around the strip ----
    // They come in once the picture has mostly shrunk away from where they go,
    // so they never sit over it.
    const float words_a = stats_zoomed ? 0.0f : std::max(0.0f, std::min(1.0f, (k - 0.5f) * 2.0f)) * (1.0f - pt);
    if (words_a > 0.003f) {
        const float m = 48 * u;
        text("GALLERY", m, 70 * u, 34 * u, 0, ink, words_a);
        if (s.loading) text("READING THE CARD", W * 0.5f, 70 * u, 24 * u, 1, quiet, words_a);


        // what is selected
        const Gallery::Item& it = items[s.sel];
        std::string l1, l2;
        if (it.live) {
            l1 = "LIVE";
            l2 = live_fresh ? "Receiving video" : (live_pic ? "Video stopped" : "No video yet");
        } else if (it.stats) {
            l1 = "STATS";
            l2 = "Latency, frame pacing and link, live";
        } else {
            l1 = Gallery::stamp_label(it.name);
            if (l1.empty()) l1 = it.name;
            char sz[48];
            snprintf(sz, sizeof(sz), "  -  %.0f MB", it.size / 1048576.0);
            l2 = Gallery::duration_label(it.duration) + sz;
        }
        text(l1, W * 0.5f, 0.82f * H, 40 * u, 1, ink, words_a);
        text(l2, W * 0.5f, 0.82f * H + 40 * u, 28 * u, 1, quiet, words_a);
        if (n == 1 && !s.loading)
            text("NO RECORDINGS YET", W * 0.5f, 0.82f * H + 76 * u, 24 * u, 1, quiet, words_a);

        const float cy = 0.42f * H;
        if (s.sel > 0)        text("<", 0.025f * W, cy + 14 * u, 52 * u, 0, ink, words_a);
        if (s.sel < n - 1)    text(">", 0.975f * W, cy + 14 * u, 52 * u, 2, ink, words_a);
    }

    // ---- "delete this recording?" ----
    if (s.dialog > 0.003f && s.sel >= 0 && s.sel < live_i) {
        const float a = s.dialog;
        const float black[3] = { 0, 0, 0 };
        const float danger[3] = { 0.93f, 0.33f, 0.28f };
        const float panel_bg[3] = { ground[0] * 0.45f, ground[1] * 0.45f, ground[2] * 0.45f };
        fill(0, 0, (float)W, (float)H, black, 0.6f * a);                 // the strip steps back
        // two buttons and nothing else; the lit one is the answer, and it starts on Cancel
        const float bw = 0.15f * W, bh = 66 * u, gx = 0.02f * W, pad = 0.03f * W;
        const float pw = 2 * bw + gx + 2 * pad, ph = bh + 2 * pad * 0.8f;
        const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f, by = py + (ph - bh) * 0.5f;
        fill(px, py, pw, ph, panel_bg, 0.98f * a);
        ring(Gallery::Rect{ W * 0.5f, H * 0.5f, pw, ph }, 2 * u, quiet, a);
        const float kx = W * 0.5f - gx * 0.5f - bw, dx = W * 0.5f + gx * 0.5f;
        const bool del = s.delete_chosen;
        fill(kx, by, bw, bh, del ? panel_bg : accent, a);
        ring(Gallery::Rect{ kx + bw * 0.5f, by + bh * 0.5f, bw, bh }, 2 * u, del ? quiet : accent, a);
        text("CANCEL", kx + bw * 0.5f, by + bh * 0.5f + 11 * u, 30 * u, 1, del ? ink : ground, a);
        fill(dx, by, bw, bh, del ? danger : panel_bg, a);
        ring(Gallery::Rect{ dx + bw * 0.5f, by + bh * 0.5f, bw, bh }, 2 * u, del ? danger : quiet, a);
        text("DELETE", dx + bw * 0.5f, by + bh * 0.5f + 11 * u, 30 * u, 1, del ? ground : ink, a);
    }

    // ---- playback ----
    if (pt > 0.003f && s.sel >= 0 && s.sel < live_i) {
        const Gallery::Item& it = items[s.sel];
        const float a = std::max(0.0f, (pt - 0.5f) * 2.0f);
        const float m = 48 * u;
        std::string title = Gallery::stamp_label(it.name);
        if (title.empty()) title = it.name;
        pill(title, m, 36 * u, 26 * u, ground, 0.7f, ink, a);

        const float dur_ms = (float)std::max(1.0, it.duration * 1000.0);
        const float pos_ms = clip_player_ ? (float)clip_player_->position_ms() : 0.0f;
        const float frac = std::max(0.0f, std::min(1.0f, pos_ms / dur_ms));
        const float bx = m + 90 * u, bw = W - 2 * m - 180 * u, by = H - 90 * u;
        fill(m - 12 * u, by - 40 * u, W - 2 * m + 24 * u, 80 * u, ground, 0.7f * a);
        fill(bx, by - 3 * u, bw, 6 * u, quiet, 0.5f * a);
        fill(bx, by - 3 * u, bw * frac, 6 * u, accent, a);
        text(Gallery::duration_label(pos_ms / 1000.0), bx - 14 * u, by + 9 * u, 26 * u, 2, ink, a);
        text(Gallery::duration_label(it.duration), bx + bw + 14 * u, by + 9 * u, 26 * u, 0, quiet, a);
        const char* state = s.ended ? "ENDED - ENTER TO REPLAY" : (s.paused ? "PAUSED" : "");
        if (state[0]) text(state, W * 0.5f, H * 0.5f, 40 * u, 1, ink, a);
        text("LEFT / RIGHT  SEEK 5 S        ENTER  PAUSE        BACK  GALLERY",
             W * 0.5f, by - 56 * u, 24 * u, 1, quiet, a);
    }

    glUseProgram(shader_program);
    if (a_alpha_factor_ != -1) glVertexAttrib1f(a_alpha_factor_, 1.0f);

    // Another frame at once while anything moves; a thumbnail or the card read
    // still coming is picked up on the next ordinary refresh.
    // The stats chart moves with time: redraw at the display's rate while it is on screen.
    const bool busy = animating || stats_visible;
    static const bool no_phase_lock = getenv("KESTREL_NO_PHASELOCK") != nullptr;   // for A/B tests
    phase_lock_.store(busy && !no_phase_lock, std::memory_order_relaxed);
    if (busy) signal_render(prof::kWakeAnim);
    if (prof::enabled()) stats_prof(now);

    // Profiling (KESTREL_PROF): how evenly an animation was drawn, and how many
    // of its frames the display never got.
    if (prof::enabled()) {
        static uint64_t last_us = 0, n = 0, late = 0, max_dt = 0, sum_dt = 0, cons0 = 0, sup0 = 0;
        if (busy) {
            if (n == 0) { cons0 = dev->osd_consumed_; sup0 = dev->osd_superseded_; }
            else {
                const uint64_t dt = now - last_us;
                sum_dt += dt;
                if (dt > max_dt) max_dt = dt;
                if (dt > 20000) late++;
            }
            n++;
            last_us = now;
        } else if (n > 0) {
            printf("GALLERY anim %llu frames, interval avg %.1f max %.1f ms, %llu over 20 ms; display took %llu, dropped %llu\n",
                   (unsigned long long)n, n > 1 ? sum_dt / 1000.0 / (n - 1) : 0.0, max_dt / 1000.0,
                   (unsigned long long)late, (unsigned long long)(dev->osd_consumed_ - cons0),
                   (unsigned long long)(dev->osd_superseded_ - sup0));
            fflush(stdout);
            n = late = max_dt = sum_dt = 0;
        }
    }
    return true;
}
