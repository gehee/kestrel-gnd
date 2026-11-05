// AR9311 fixed-mode scanout, using the DRM ABI validated by fpvOS ar9311-play.
#include "display/ar9311_screen.hpp"
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace display {
namespace {
void ctl(int fd, unsigned long op, void *arg, const char *name) {
    int rc;
    do {
        rc = ioctl(fd, op, arg);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        throw std::runtime_error(std::string(name) + ": " + strerror(errno));
}
class Ar9311Screen final : public SoftwareScreen {
    struct Buffer {
        uint32_t handle = 0, fb = 0, pitch = 0;
        uint64_t size = 0;
        uint8_t *pixels = nullptr;
    };
    int fd_ = -1;
    std::array<Buffer, 3> buffers_{};
    uint32_t connector_ = 0;
    drm_mode_crtc crtc_{};
    bool active_ = false;
    volatile bool *stop_;
    std::mutex mutex_;
    std::shared_ptr<DecodedUnit> latest_;
    std::shared_ptr<const hal::I420Overlay> overlay_;
    uint64_t revision_ = 0;
    std::atomic<uint64_t> presented_{0};
    std::atomic<bool> failed_{false};
    void close_display() {
        if (fd_ < 0)
            return;
        if (active_) {
            drm_mode_crtc off{};
            off.crtc_id = crtc_.crtc_id;
            ioctl(fd_, DRM_IOCTL_MODE_SETCRTC, &off);
            active_ = false;
        }
        for (auto &b : buffers_) {
            if (b.pixels)
                munmap(b.pixels, b.size);
            if (b.fb)
                ioctl(fd_, DRM_IOCTL_MODE_RMFB, &b.fb);
            drm_mode_destroy_dumb destroy{};
            destroy.handle = b.handle;
            if (b.handle)
                ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            b = {};
        }
        close(fd_);
        fd_ = -1;
    }
    void create(Buffer &b) {
        drm_mode_create_dumb d{};
        d.width = 1920;
        d.height = 1080 * 3 / 2;
        d.bpp = 8;
        ctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &d, "I420 scanout allocation");
        b.handle = d.handle;
        b.pitch = d.pitch;
        b.size = d.size;
        if (d.pitch != 1920 || d.size < 1920 * 1080 * 3 / 2)
            throw std::runtime_error("unsupported scanout pitch");
        drm_mode_fb_cmd2 fb{};
        fb.width = 1920;
        fb.height = 1080;
        fb.pixel_format = DRM_FORMAT_YUV420;
        for (unsigned i = 0; i < 3; ++i)
            fb.handles[i] = d.handle;
        fb.pitches[0] = 1920;
        fb.pitches[1] = fb.pitches[2] = 960;
        fb.offsets[1] = 1920 * 1080;
        fb.offsets[2] = 1920 * 1080 * 5 / 4;
        ctl(fd_, DRM_IOCTL_MODE_ADDFB2, &fb, "I420 framebuffer");
        b.fb = fb.fb_id;
        drm_mode_map_dumb map{};
        map.handle = d.handle;
        ctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map, "scanout mapping");
        auto p = mmap(nullptr, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, map.offset);
        if (p == MAP_FAILED)
            throw std::runtime_error("cannot map scanout buffer");
        b.pixels = static_cast<uint8_t *>(p);
    }
    void flip(uint32_t fb) {
        if (!active_) {
            crtc_.fb_id = fb;
            ctl(fd_, DRM_IOCTL_MODE_SETCRTC, &crtc_, "enable fixed 1080p60");
            active_ = true;
            return;
        }
        drm_mode_crtc_page_flip f{};
        f.crtc_id = crtc_.crtc_id;
        f.fb_id = fb;
        f.flags = DRM_MODE_PAGE_FLIP_EVENT;
        ctl(fd_, DRM_IOCTL_MODE_PAGE_FLIP, &f, "page flip");
        // Completion is the ownership boundary. Never recycle on a timeout.
        pollfd p{fd_, POLLIN, 0};
        int r;
        do {
            r = poll(&p, 1, 3000);
        } while (r < 0 && errno == EINTR);
        if (r <= 0 || !(p.revents & POLLIN))
            throw std::runtime_error("HDMI page flip timed out");
        alignas(drm_event_vblank) char events[1024];
        ssize_t n = read(fd_, events, sizeof(events));
        for (size_t at = 0; n > 0 && at + sizeof(drm_event) <= size_t(n);) {
            auto *e = reinterpret_cast<drm_event *>(events + at);
            if (e->length < sizeof(*e) || e->length > size_t(n) - at)
                break;
            if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(drm_event_vblank))
                return;
            at += e->length;
        }
        throw std::runtime_error("missing HDMI flip completion");
    }

  public:
    Ar9311Screen(const std::string &device, bool force, volatile bool *stop) : stop_(stop) {
        try {
            fd_ = open(device.c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ < 0)
                throw std::runtime_error("cannot open " + device);
            ctl(fd_, DRM_IOCTL_SET_MASTER, nullptr, "DRM master (stop other display clients)");
            uint32_t crtcs[16]{}, connectors[16]{};
            drm_mode_card_res r{};
            r.crtc_id_ptr = uintptr_t(crtcs);
            r.count_crtcs = 16;
            r.connector_id_ptr = uintptr_t(connectors);
            r.count_connectors = 16;
            ctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &r, "DRM resources");
            if (r.count_crtcs != 1 || r.count_connectors != 1)
                throw std::runtime_error("AR9311 requires one CRTC/connector");
            drm_mode_get_connector c{};
            c.connector_id = connectors[0];
            ctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &c, "HDMI connector");
            if (c.connection != 1 && !force)
                throw std::runtime_error(
                    "HDMI reports disconnected; --force-display enables the board's fixed mode");
            connector_ = connectors[0];
            crtc_.crtc_id = crtcs[0];
            crtc_.set_connectors_ptr = uintptr_t(&connector_);
            crtc_.count_connectors = 1;
            crtc_.mode_valid = 1;
            auto &m = crtc_.mode;
            m.clock = 148500;
            m.hdisplay = 1920;
            m.hsync_start = 2008;
            m.hsync_end = 2052;
            m.htotal = 2200;
            m.vdisplay = 1080;
            m.vsync_start = 1084;
            m.vsync_end = 1089;
            m.vtotal = 1125;
            m.vrefresh = 60;
            m.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
            m.type = DRM_MODE_TYPE_DRIVER;
            strcpy(m.name, "1920x1080");
            for (auto &b : buffers_)
                create(b);
        } catch (...) {
            close_display();
            throw;
        }
    }
    ~Ar9311Screen() override { close_display(); }
    void queue_frame(std::shared_ptr<DecodedUnit> f) override {
        if (!f || f->width != 1920 || f->height != 1080 || !f->data[0] || !f->data[1] || !f->data[2] ||
            f->linesize[0] < 1920 || f->linesize[1] < 960 || f->linesize[2] < 960) {
            failed_ = true;
            fprintf(stderr, "display: only complete 1920x1080 I420 frames are supported\n");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(f);
        ++revision_;
    }
    void request_flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset();
        ++revision_;
    }
    void overlay(std::shared_ptr<const hal::I420Overlay> p) override {
        std::lock_guard<std::mutex> lock(mutex_);
        overlay_ = std::move(p);
        ++revision_;
    }
    uint64_t presented() const override { return presented_.load(); }
    bool failed() const override { return failed_.load(); }
    void run() override {
        try {
            uint64_t last = ~uint64_t(0);
            unsigned next = 0;
            // Compose in cached RAM: per-pixel reads/writes against the DRM
            // write-combined mapping are prohibitively expensive on AR9311.
            std::vector<uint8_t> composed(1920 * 1080 * 3 / 2);
            uint64_t copy_us = 0, blend_us = 0, upload_us = 0, flip_us = 0;
            auto clock = [] {
                return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
            };
            bool expired = false;
            while (!*stop_ && !failed_) {
                std::shared_ptr<DecodedUnit> frame;
                std::shared_ptr<const hal::I420Overlay> overlay;
                uint64_t version;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frame = latest_;
                    overlay = overlay_;
                    version = revision_;
                }
                const auto began = clock();
                const bool stale = frame && frame->dec_end_ts && began > frame->dec_end_ts &&
                                   began - frame->dec_end_ts > 2000000;
                if (version == last && stale == expired) {
                    usleep(2000);
                    continue;
                }
                expired = stale;
                if (stale)
                    frame.reset();
                auto &b = buffers_[next];
                auto *y = composed.data();
                auto *u = y + 1920 * 1080;
                auto *v = u + 960 * 540;
                if (frame) {
                    uint8_t *planes[] = {y, u, v};
                    for (unsigned p = 0; p < 3; ++p)
                        for (unsigned row = 0; row < (p ? 540u : 1080u); ++row)
                            memcpy(planes[p] + row * (p ? 960 : 1920),
                                   frame->data[p] + row * frame->linesize[p], p ? 960 : 1920);
                } else {
                    memset(y, 16, 1920 * 1080);
                    memset(u, 128, 1920 * 1080 / 2);
                }
                // Scanout uses a separate pool: CPU composition never modifies
                // decoder reference pictures or a buffer currently being shown.
                const auto copied = clock();
                if (overlay)
                    overlay->blend(y, u, v, 1920, 1080);
                const auto blended = clock();
                memcpy(b.pixels, composed.data(), composed.size());
                const auto uploaded = clock();
                flip(b.fb);
                copy_us += copied - began;
                blend_us += blended - copied;
                upload_us += uploaded - blended;
                flip_us += clock() - uploaded;
                ++presented_;
                next = (next + 1) % buffers_.size();
                last = version;
            }
            const auto n = std::max(uint64_t(1), presented_.load());
            printf("display: average us copy=%llu blend=%llu upload=%llu flip=%llu\n",
                   (unsigned long long)(copy_us / n), (unsigned long long)(blend_us / n),
                   (unsigned long long)(upload_us / n), (unsigned long long)(flip_us / n));
        } catch (const std::exception &e) {
            failed_ = true;
            fprintf(stderr, "display: %s\n", e.what());
        }
        request_flush();
        close_display();
    }
};
} // namespace
std::shared_ptr<SoftwareScreen> make_ar9311_screen(const std::string &path, bool force, volatile bool *stop) {
    return std::make_shared<Ar9311Screen>(path, force, stop);
}
} // namespace display
