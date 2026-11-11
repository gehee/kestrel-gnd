#include "hal/rockchip/rga_strips.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstdlib>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include "base/time_util.h"
#ifdef USE_RGA
#include "include/rga/im2d.hpp"

bool RgaStripJoiner::available() {
    return access("/dev/rga", R_OK | W_OK) == 0;
}

// Each set of picture buffers made its own DecodedUnit::buf_epoch, apart from
// the decoders' (counted from 1): the screen recording keeps its RGA imports
// by epoch and fd, and a new set may come back with the old fd numbers.
static std::atomic<uint32_t> s_epoch{0};

RgaStripJoiner::RgaStripJoiner(std::shared_ptr<hal::FrameSink> out, std::shared_ptr<hal::Scanout> scanout)
    : out_(std::move(out)), scanout_(std::move(scanout)), drm_fd_(scanout_ ? scanout_->scanout_fd() : -1) {
    const char* c = getenv("KESTREL_STRIP_CHECK");
    check_ = c && atoi(c) > 0;
    if (check_) printf("strips: KESTREL_STRIP_CHECK - every joined picture checked for strips of other captures\n");
}

RgaStripJoiner::~RgaStripJoiner() {
    std::lock_guard<std::mutex> lk(mtx_);
    cur_ = Picture();
    for (auto& m : imported_) {
        for (auto& i : m) releasebuffer_handle(i.second);
        m.clear();
    }
    free_slots();
}

void RgaStripJoiner::free_slots() {
    for (int i = 0; i < kSlots; i++)
        if (map_[i]) { munmap(map_[i], (size_t)pitch_ * h_ * 3 / 2); map_[i] = nullptr; }
    for (Slot& s : slot_) {
        if (s.rga) releasebuffer_handle(s.rga);
        if (s.fd >= 0) close(s.fd);
        s = Slot();
    }
}

// The whole-picture buffers, w x h NV12, made as the decoder makes its own:
// a dumb buffer on the DRM device, its prime fd kept and its handle let go
// (the renderer's import of the fd gets a handle of its own).
bool RgaStripJoiner::alloc(uint32_t w, uint32_t h) {
    free_slots();
    w_ = h_ = pitch_ = 0;
    for (Slot& s : slot_) {
        struct drm_mode_create_dumb c = {};
        c.bpp = 8;
        c.width = w;
        c.height = h * 3 / 2;
        int r;
        do r = ioctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &c); while (r == -1 && (errno == EINTR || errno == EAGAIN));
        if (r) { perror("strips: picture buffer"); free_slots(); return false; }
        struct drm_prime_handle p = {};
        p.handle = c.handle;
        p.flags = DRM_CLOEXEC | DRM_RDWR;
        p.fd = -1;
        r = ioctl(drm_fd_, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p);
        struct drm_mode_destroy_dumb d = {};
        d.handle = c.handle;
        ioctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        if (r) { perror("strips: picture buffer fd"); free_slots(); return false; }
        s.fd = p.fd;
        s.rga = importbuffer_fd(s.fd, (int)(c.pitch * h * 3 / 2));
        if (!s.rga) { printf("strips: RGA cannot import a picture buffer\n"); free_slots(); return false; }
        pitch_ = c.pitch;
    }
    w_ = w;
    h_ = h;
    epoch_out_ = 0x80000000u | (++s_epoch & 0x7fffffffu);
    printf("strips: joining %ux%u pictures in RGA (%s)\n", w, h, querystring(RGA_VERSION));
    return true;
}

// A buffer nothing holds any more (the screen and any recording done with
// the picture it last held), the oldest first.
int RgaStripJoiner::take_slot() {
    for (int i = 0; i < kSlots; i++) {
        const int k = (next_slot_ + i) % kSlots;
        if (k == cur_.slot || !slot_[k].out.expired()) continue;
        next_slot_ = (k + 1) % kSlots;
        return k;
    }
    return -1;
}

uint32_t RgaStripJoiner::import(int k, const DecodedUnit& du) {
    if (du.buf_epoch != epoch_[k]) {
        for (auto& i : imported_[k]) releasebuffer_handle(i.second);
        imported_[k].clear();
        epoch_[k] = du.buf_epoch;
    }
    auto it = imported_[k].find(du.prime_fd);
    if (it != imported_[k].end()) return it->second;
    const uint32_t ver = du.pitches[0] ? du.offsets[1] / du.pitches[0] : du.height;
    rga_buffer_handle_t h = importbuffer_fd(du.prime_fd, (int)(du.pitches[0] * ver * 3 / 2));
    if (h) imported_[k][du.prime_fd] = h;
    return h;
}

bool RgaStripJoiner::copy(int k, const DecodedUnit& du, uint32_t y, uint32_t top, uint32_t rows) {
    const uint32_t h = import(k, du);
    if (!h) { printf("strips: RGA cannot import strip %d's picture (fd %d)\n", k, du.prime_fd); return false; }
    const int ver = du.pitches[0] ? (int)(du.offsets[1] / du.pitches[0]) : (int)du.height;
    rga_buffer_t src = wrapbuffer_handle(h, (int)du.width, (int)du.height, RK_FORMAT_YCbCr_420_SP,
                                         (int)du.pitches[0], ver);
    rga_buffer_t dst = wrapbuffer_handle(slot_[cur_.slot].rga, (int)w_, (int)h_, RK_FORMAT_YCbCr_420_SP,
                                         (int)pitch_, (int)h_);
    im_rect sr = {0, (int)top, (int)du.width, (int)rows};
    im_rect dr = {0, (int)y, (int)du.width, (int)rows};
    rga_buffer_t none = {};
    im_rect nr = {};
    const uint64_t t0 = get_time_us();
    IM_STATUS st = improcess(src, dst, none, sr, dr, nr, IM_SYNC);
    if (st != IM_STATUS_SUCCESS && st != IM_STATUS_NOERROR) {
        printf("strips: RGA copy of strip %d failed: %s\n", k, imStrError(st));
        return false;
    }
    copy_us_ += get_time_us() - t0;
    copies_++;
    return true;
}

void RgaStripJoiner::give_up() {
    if (cur_.pic >= 0 && cur_.n && !cur_.given_up) incomplete_++;
    const int64_t pic = cur_.pic;
    cur_ = Picture();
    cur_.pic = pic;
    cur_.given_up = true;
}

bool RgaStripJoiner::layout(uint32_t* w, uint32_t* h) const {
    uint32_t total = 0, width = 0;
    for (int k = 0; k < cur_.n; k++) {
        if (!shown(k) || rows_pic_[k] < layout_pic_) return false;
        total += shown(k);
    }
    for (int k = 0; k < cur_.n && !width; k++)
        if (cur_.s[k].du) width = cur_.s[k].du->width;
    if (!width) return false;
    *w = width;
    *h = total;
    return true;
}

void RgaStripJoiner::strip(int k, bool last, int ov, std::shared_ptr<DecodedUnit> du) {
    if (!du || !du->has_prime_fd || k < 0 || k >= kStrips || ov < 0 || ov > 64) return;
    std::shared_ptr<DecodedUnit> whole;
    std::unique_lock<std::mutex> lk(mtx_);
    const int64_t pic = (int64_t)du->pts;
    // A picture given up on already - unless far behind: the air unit
    // counting from 0 again (its kestrel-air started again).
    if (cur_.pic >= 0 && pic < cur_.pic) {
        if (cur_.pic - pic < 64) return;
        cur_ = Picture();
        clean_ = false;
        layout_pic_ = -1;
        for (int64_t& p : rows_pic_) p = -1;
    }
    // How many strips a picture has: its last one says; one that is not the
    // last, at or past that count, says there are more.
    const int n = last ? k + 1 : std::max(n_, k + 2);
    if (n < 2 || n > kStrips) return;
    // Each strip's rows, from its own pictures. A change (or a new count)
    // and the rest's are the old layout's until they say theirs again: the
    // buffers are sized from this picture's strips on, not from a mix.
    if (rows_[k] != du->height) {
        rows_[k] = du->height;
        layout_pic_ = pic;
        // Strips below this one placed already went to rows from the old height.
        if (cur_.pic == pic && (cur_.placed >> (k + 1))) give_up();
    }
    rows_pic_[k] = pic;
    // The picture being joined was laid out for the old count (or overlap): it goes.
    if (n != n_ || ov != ov_) {
        if (cur_.pic == pic) give_up();
        n_ = n;
        ov_ = ov;
        layout_pic_ = pic;
    }
    if (pic != cur_.pic) {
        // A newer picture: the one being joined lacks a strip, and goes.
        give_up();
        cur_ = Picture();
        cur_.pic = pic;
        cur_.n = n_;
        cur_.ov = ov_;
    }
    if (cur_.given_up || cur_.s[k].du || (cur_.placed >> k & 1)) return;   // not wanted, or had
    cur_.s[k].du = std::move(du);
    if (resizing_) return;   // the strip resizing places this one once done
    uint32_t w = 0, h = 0;
    if (!layout(&w, &h)) return;
    if (w != w_ || h != h_) {
        // A new size: every buffer made again. The renderer and the screen
        // recording let go of the old pictures first - the renderer keeps
        // their framebuffers by fd number, which new buffers may come back
        // with - and so do pictures on their way to it.
        const uint64_t now = get_time_us() / 1000;
        if (alloc_failed_ms_ && now - alloc_failed_ms_ < 1000) { give_up(); return; }
        resizing_ = true;
        idle_.wait(lk, [this] { return queuing_ == 0; });
        const bool had = w_ != 0;
        lk.unlock();
        if (had) {
            out_->request_flush();
            if (scanout_) scanout_->drop_screen_pictures();
        }
        lk.lock();
        resizing_ = false;
        // The strips that came meanwhile may have changed it again.
        if (!layout(&w, &h)) return;
        // What went to the old buffers is gone with them.
        if (cur_.placed) { give_up(); return; }
        cur_.slot = -1;
        if ((w != w_ || h != h_) && !alloc(w, h)) {
            alloc_failed_ms_ = now ? now : 1;
            give_up();
            return;
        }
        alloc_failed_ms_ = 0;
    }
    whole = place();
    if (!whole) return;
    queuing_++;
    lk.unlock();
    out_->queue_frame(std::move(whole));
    lk.lock();
    if (--queuing_ == 0) idle_.notify_all();
}

std::shared_ptr<DecodedUnit> RgaStripJoiner::place() {
    if (!w_ || !h_) return nullptr;
    if (cur_.slot < 0) {
        cur_.slot = take_slot();
        if (cur_.slot < 0) {   // all of them on screen still: not this picture
            no_slot_++;
            give_up();
            return nullptr;
        }
    }
    uint32_t y = 0;
    for (int k = 0; k < cur_.n; k++) {
        Strip& s = cur_.s[k];
        if (s.du && !(cur_.placed >> k & 1)) {
            if (s.du->width != w_ || y + shown(k) > h_ || !copy(k, *s.du, y, top(k), shown(k))) {
                give_up();
                return nullptr;
            }
            s.placed_us = get_time_us();
            cur_.placed |= 1u << k;
            cur_.times[k] = s.du->slices;
            cur_.key = cur_.key && s.du->is_keyframe;
            if (k == 0) {
                cur_.recv_ts = s.du->recv_ts;
                cur_.dec_start_ts = s.du->dec_start_ts;
                cur_.enc_us = s.du->tx_encode_delay_us;
                cur_.air_us = s.du->tx_processing_delay_us;
            }
            s.du.reset();   // its decoder buffer back
        }
        y += shown(k);
    }
    return cur_.placed == (1u << cur_.n) - 1 ? finish() : nullptr;
}

// The whole picture, as a decoder would hand it out: its rows timed strip by
// strip (each strip a slice of it, base/slice_times.hpp), the air figures of
// its top.
std::shared_ptr<DecodedUnit> RgaStripJoiner::finish() {
    auto du = std::make_shared<DecodedUnit>();
    memset(du->data, 0, sizeof(du->data));
    memset(du->linesize, 0, sizeof(du->linesize));
    du->pts = (uint64_t)cur_.pic;
    du->width = w_;
    du->height = h_;
    du->recv_ts = cur_.recv_ts;
    du->dec_start_ts = cur_.dec_start_ts;
    du->dec_end_ts = cur_.s[cur_.n - 1].placed_us;
    du->tx_encode_delay_us = cur_.enc_us;
    du->tx_processing_delay_us = cur_.air_us;
    // A keyframe where every strip is one - and the first whole picture after
    // a start too: each strip's decoder took a keyframe of its own before it
    // decoded anything (radio/video_out holds slices back until then), though
    // the picture in which they all were may have been given up. The screen
    // goes live on a keyframe (OSD::notify_video_frame); with intra refresh
    // none comes again unless asked for, so without this it never did.
    du->is_keyframe = cur_.key || !clean_;
    clean_ = true;
    du->has_prime_fd = true;
    du->prime_fd = slot_[cur_.slot].fd;
    du->drm_pixel_format = DRM_FORMAT_NV12;
    du->pitches[0] = du->pitches[1] = pitch_;
    du->offsets[0] = 0;
    du->offsets[1] = pitch_ * h_;
    // The buffer is this picture's while anything holds the picture.
    auto hold = std::make_shared<int>(0);
    slot_[cur_.slot].out = hold;
    du->frame_ref = hold;
    du->buf_epoch = epoch_out_;   // not a decoder's, and new with each set of buffers
    // The strips' own times, each the slice its rows are of.
    const SliceTimes* top = cur_.times[0].get();
    if (top && top->frame_cap_us && top->height) {
        auto st = std::make_shared<SliceTimes>();
        st->height = h_;
        st->frame_cap_us = top->frame_cap_us;
        st->readout_us = top->readout_us;
        st->exposure_us = top->exposure_us;
        st->key = cur_.key;
        st->pts = cur_.pic;
        st->expected = cur_.n;
        uint32_t y = 0;
        for (int k = 0; k < cur_.n; k++) {
            const SliceTimes* t = cur_.times[k].get();
            SliceTimes::Slice& s = st->s[k];
            s.row = y;
            s.cap_us = top->frame_cap_us + (h_ ? (uint64_t)y * top->readout_us / h_ : 0);
            if (t && t->n.load() > 0) {
                s.out_us = t->s[0].out_us;
                s.here_us = t->s[0].here_us;
                s.in_us.store(t->s[0].in_us.load());
            }
            s.done_us.store(cur_.s[k].placed_us);
            y += shown(k);
        }
        st->n.store(cur_.n);
        st->complete.store(true);
        du->slices = st;
    }
    if (check_) check_picture();
    joined_++;
    const uint64_t now = get_time_us() / 1000;
    if (now - log_ms_ >= 5000) {
        printf("strips: %lu pictures of %d strips joined in 5 s (%d rows past each join cut), %lu given up "
               "(a strip missing), %lu with no buffer free; RGA %.2f ms a strip\n", joined_, cur_.n, cur_.ov,
               incomplete_, no_slot_,
               copies_ ? copy_us_ / 1000.0 / copies_ : 0.0);
        joined_ = incomplete_ = no_slot_ = 0;
        copy_us_ = copies_ = 0;
        log_ms_ = now;
    }
    cur_ = Picture();
    cur_.pic = (int64_t)du->pts;   // its strips, should any come again, are not a new picture's
    cur_.given_up = true;          // ...nor this one's (n 0: not counted as given up)
    return du;
}

void RgaStripJoiner::check_picture() {
    const int k = cur_.slot;
    if (!map_[k]) {
        void* p = mmap(nullptr, (size_t)pitch_ * h_ * 3 / 2, PROT_READ, MAP_SHARED, slot_[k].fd, 0);
        if (p == MAP_FAILED) { perror("strips: check mmap"); check_ = false; return; }
        map_[k] = (uint8_t*)p;
    }
    // Each strip's halves: the mean luma of every 8th row's every 32nd pixel.
    double l[2 * kStrips];
    const int bands = 2 * cur_.n;
    uint32_t y = 0;
    for (int s = 0; s < cur_.n; s++) {
        for (int half = 0; half < 2; half++) {
            const uint32_t from = y + half * shown(s) / 2, to = y + (half + 1) * shown(s) / 2;
            uint64_t sum = 0, cnt = 0;
            for (uint32_t r = from + 2; r + 2 < to; r += 8)
                for (uint32_t x = 16; x < w_; x += 32) { sum += map_[k][(size_t)r * pitch_ + x]; cnt++; }
            l[2 * s + half] = cnt ? (double)sum / cnt : 0;
        }
        y += shown(s);
    }
    checked_++;
    bool all_lit = true;
    double rmin = 1e9, rmax = 0;
    for (int b = 0; b < bands; b++) {
        if (prev_[b] < 8 || l[b] < 8) { all_lit = false; continue; }
        const double r = l[b] / prev_[b];
        rmin = r < rmin ? r : rmin;
        rmax = r > rmax ? r : rmax;
    }
    if (all_lit && (rmax > 1.4 || rmin < 1 / 1.4)) {
        steps_++;
        // A step in one half with another that did not move (within 4%) or
        // moved the other way. Not just less: the tone curve flattens the
        // bright parts, which then step by a third of what the dark ones do
        // (1.13 against 1.64 on the bench), but they step in the same picture.
        if ((rmax > 1.4 && rmin < 1.04) || (rmin < 1 / 1.4 && rmax > 1 / 1.04)) {
            if (++torn_ <= 20) {
                printf("strips: TORN picture %lld - each half's luma against the picture before:",
                       (long long)cur_.pic);
                for (int b = 0; b < bands; b++) printf(" %.0f/%.0f", l[b], prev_[b]);
                printf("\n");
            }
        }
    }
    for (int b = 0; b < bands; b++) prev_[b] = l[b];
    const uint64_t now = get_time_us() / 1000;
    static uint64_t log_ms;
    if (now - log_ms >= 5000) {
        printf("strips: check - %lu pictures, %lu brightness steps, %lu torn\n", checked_, steps_, torn_);
        log_ms = now;
    }
}

void RgaStripJoiner::drop() {
    std::lock_guard<std::mutex> lk(mtx_);
    give_up();
    cur_ = Picture();
    clean_ = false;
}

#else   // no RGA: strips cannot be joined, so they are never asked for

bool RgaStripJoiner::available() { return false; }
RgaStripJoiner::RgaStripJoiner(std::shared_ptr<hal::FrameSink> out, std::shared_ptr<hal::Scanout> scanout)
    : out_(std::move(out)), scanout_(std::move(scanout)), drm_fd_(-1) {}
RgaStripJoiner::~RgaStripJoiner() {}
void RgaStripJoiner::strip(int, bool, int, std::shared_ptr<DecodedUnit>) {}
void RgaStripJoiner::drop() {}

#endif
