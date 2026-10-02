#include "rga_compositor.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>

#include "renderer.hpp"   // DecodedUnit
#include "utils/time_util.h"

#ifdef USE_RGA
#include "include/rga/im2d.hpp"

bool RgaCompositor::available() {
    return access("/dev/rga", R_OK | W_OK) == 0;
}

bool RgaCompositor::init(int screen_w, int screen_h, const std::vector<int>& enc_fds) {
    deinit();
    w_ = screen_w; h_ = screen_h;
    const int size = w_ * h_ * 3 / 2;
    for (int fd : enc_fds) {
        rga_buffer_handle_t h = importbuffer_fd(fd, size);
        if (!h) {
            printf("RGA: cannot import encoder buffer fd %d\n", fd);
            deinit();
            return false;
        }
        enc_.push_back(h);
    }
    printf("RGA: compositing %dx%d into %zu encoder buffers (%s)\n", w_, h_, enc_.size(),
           querystring(RGA_VERSION));
    return true;
}

void RgaCompositor::deinit() {
    for (uint32_t h : enc_) releasebuffer_handle(h);
    enc_.clear();
    for (auto& o : osd_) releasebuffer_handle(o.second);
    osd_.clear();
    for (auto& d : dec_) releasebuffer_handle(d.second);
    dec_.clear();
    if (scratch_) { releasebuffer_handle(scratch_); scratch_ = 0; }
    if (scratch_map_) { munmap(scratch_map_, (size_t)w_ * h_ * 3 / 2); scratch_map_ = nullptr; }
    if (scratch_fd_ >= 0) { close(scratch_fd_); scratch_fd_ = -1; }
    sx_ = sy_ = sw_ = sh_ = -1;
}

// Decoder buffers are imported once each. A new epoch means the decoder
// replaced its buffers (a resolution change) and fd numbers may have been
// reused for different memory, so everything imported before goes.
uint32_t RgaCompositor::import_decoded(const DecodedUnit& du) {
    if (du.buf_epoch != dec_epoch_) {
        for (auto& d : dec_) releasebuffer_handle(d.second);
        dec_.clear();
        dec_epoch_ = du.buf_epoch;
    }
    auto key = std::make_pair(du.buf_epoch, du.prime_fd);
    auto it = dec_.find(key);
    if (it != dec_.end()) return it->second;
    const uint32_t ver_stride = du.pitches[0] ? du.offsets[1] / du.pitches[0] : du.height;
    rga_buffer_handle_t h = importbuffer_fd(du.prime_fd, (int)(du.pitches[0] * ver_stride * 3 / 2));
    if (h) dec_[key] = h;
    return h;
}

bool RgaCompositor::ensure_scratch() {
    if (scratch_) return true;
    const size_t size = (size_t)w_ * h_ * 3 / 2;
    // RGA2's MMU addresses the first 4 GB only; the goggle has 1 GB, but ask
    // for it anyway where the heap exists.
    const char* heaps[] = { "/dev/dma_heap/system-uncached-dma32", "/dev/dma_heap/system-dma32",
                            "/dev/dma_heap/system" };
    for (const char* heap : heaps) {
        int hfd = open(heap, O_RDWR | O_CLOEXEC);
        if (hfd < 0) continue;
        struct dma_heap_allocation_data d = {};
        d.len = size;
        d.fd_flags = O_RDWR | O_CLOEXEC;
        int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d);
        close(hfd);
        if (r < 0) continue;
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, (int)d.fd, 0);
        if (p == MAP_FAILED) { close((int)d.fd); continue; }
        scratch_fd_ = (int)d.fd;
        scratch_map_ = (uint8_t*)p;
        scratch_ = importbuffer_fd(scratch_fd_, (int)size);
        if (scratch_) return true;
        munmap(p, size); scratch_map_ = nullptr;
        close(scratch_fd_); scratch_fd_ = -1;
    }
    printf("RGA: no scratch buffer - a picture that does not fill the screen cannot be recorded\n");
    return false;
}

// Video black: Y 16, chroma 128 - what the screen shows where no plane is.
void RgaCompositor::clear_scratch() {
    memset(scratch_map_, 16, (size_t)w_ * h_);
    memset(scratch_map_ + (size_t)w_ * h_, 128, (size_t)w_ * h_ / 2);
}

bool RgaCompositor::compose(const ScreenPair& pair, int osd_fd, uint32_t osd_stride, int enc_idx,
                            uint32_t* us) {
    if (enc_idx < 0 || enc_idx >= (int)enc_.size()) return false;
    const uint64_t t0 = get_time_us();
    im_rect full = {0, 0, w_, h_};

    rga_buffer_t dst = wrapbuffer_handle(enc_[enc_idx], w_, h_, RK_FORMAT_YCbCr_420_SP, w_, h_);

    // The video layer, as the plane showed it.
    rga_buffer_t src;
    const DecodedUnit* du = pair.video.get();
    const bool one_to_one = du && pair.vx == 0 && pair.vy == 0 && pair.vw == w_ && pair.vh == h_ &&
                            (int)du->width == w_ && (int)du->height == h_;
    if (du && one_to_one) {
        uint32_t h = import_decoded(*du);
        if (!h) { printf("RGA: cannot import decoded picture fd %d\n", du->prime_fd); return false; }
        const int ver_stride = du->pitches[0] ? (int)(du->offsets[1] / du->pitches[0]) : (int)du->height;
        src = wrapbuffer_handle(h, (int)du->width, (int)du->height, RK_FORMAT_YCbCr_420_SP,
                                (int)du->pitches[0], ver_stride);
    } else {
        if (!ensure_scratch()) return false;
        const bool rect_ok = du && pair.vw > 0 && pair.vh > 0;
        if (!rect_ok || pair.vx != sx_ || pair.vy != sy_ || pair.vw != sw_ || pair.vh != sh_) {
            clear_scratch();
            sx_ = rect_ok ? pair.vx : -1; sy_ = rect_ok ? pair.vy : -1;
            sw_ = rect_ok ? pair.vw : -1; sh_ = rect_ok ? pair.vh : -1;
        }
        rga_buffer_t scratch = wrapbuffer_handle(scratch_, w_, h_, RK_FORMAT_YCbCr_420_SP, w_, h_);
        if (rect_ok) {
            uint32_t h = import_decoded(*du);
            if (!h) { printf("RGA: cannot import decoded picture fd %d\n", du->prime_fd); return false; }
            const int ver_stride = du->pitches[0] ? (int)(du->offsets[1] / du->pitches[0]) : (int)du->height;
            rga_buffer_t pic = wrapbuffer_handle(h, (int)du->width, (int)du->height, RK_FORMAT_YCbCr_420_SP,
                                                 (int)du->pitches[0], ver_stride);
            im_rect sr = {0, 0, (int)du->width, (int)du->height};
            im_rect dr = {pair.vx, pair.vy, pair.vw, pair.vh};
            rga_buffer_t none = {};
            im_rect nr = {};
            IM_STATUS st = improcess(pic, scratch, none, sr, dr, nr, IM_SYNC);
            if (st != IM_STATUS_SUCCESS && st != IM_STATUS_NOERROR) {
                printf("RGA: scaling the picture failed: %s\n", imStrError(st));
                return false;
            }
        }
        src = scratch;
    }

    IM_STATUS st;
    if (osd_fd >= 0) {
        auto it = osd_.find(osd_fd);
        uint32_t oh;
        if (it != osd_.end()) oh = it->second;
        else {
            oh = importbuffer_fd(osd_fd, (int)(osd_stride * h_));
            if (!oh) { printf("RGA: cannot import OSD buffer fd %d\n", osd_fd); return false; }
            osd_[osd_fd] = oh;
        }
        rga_buffer_t pat = wrapbuffer_handle(oh, w_, h_, RK_FORMAT_BGRA_8888, (int)(osd_stride / 4), h_);
        st = improcess(src, dst, pat, full, full, full, IM_ALPHA_BLEND_DST_OVER | IM_SYNC);
    } else {
        rga_buffer_t none = {};
        im_rect nr = {};
        st = improcess(src, dst, none, full, full, nr, IM_SYNC);
    }
    if (st != IM_STATUS_SUCCESS && st != IM_STATUS_NOERROR) {
        printf("RGA: composite failed: %s\n", imStrError(st));
        return false;
    }
    if (us) *us = (uint32_t)(get_time_us() - t0);
    return true;
}

#else   // !USE_RGA - no librga on this build: screen recording uses writeback

bool RgaCompositor::available() { return false; }
bool RgaCompositor::init(int, int, const std::vector<int>&) { return false; }
void RgaCompositor::deinit() {}
uint32_t RgaCompositor::import_decoded(const DecodedUnit&) { return 0; }
bool RgaCompositor::ensure_scratch() { return false; }
void RgaCompositor::clear_scratch() {}
bool RgaCompositor::compose(const ScreenPair&, int, uint32_t, int, uint32_t*) { return false; }

#endif  // USE_RGA
