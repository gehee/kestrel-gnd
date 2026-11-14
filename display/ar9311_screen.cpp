#include <algorithm>
#include <atomic>
// Native AR9311: independent DMA-BUF video and ARGB UI planes, blended by VO.
#include "display/ar9311_screen.hpp"
#include "display/ascent_kms_mode.hpp"
#include "display/clock_lock.hpp"
#include "base/ltrace.hpp"
#include "base/scheduling_helper.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <string>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

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
uint64_t clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Properties {
    uint32_t id = 0;
    std::map<std::string, uint32_t> keys;
    std::map<std::string, uint64_t> values;
    Properties() = default;
    Properties(int fd, uint32_t object, uint32_t type) : id(object) {
        drm_mode_obj_get_properties p{};
        p.obj_id = object;
        p.obj_type = type;
        ctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p, "property count");
        std::vector<uint32_t> ids(p.count_props);
        std::vector<uint64_t> vals(p.count_props);
        p.props_ptr = uintptr_t(ids.data());
        p.prop_values_ptr = uintptr_t(vals.data());
        ctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p, "properties");
        if (p.count_props > ids.size())
            throw std::runtime_error("property list changed");
        for (unsigned i = 0; i < p.count_props; ++i) {
            drm_mode_get_property prop{};
            prop.prop_id = ids[i];
            ctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop, "property name");
            keys[prop.name] = ids[i];
            values[prop.name] = vals[i];
        }
    }
};
struct Atomic {
    std::vector<uint32_t> objects, counts, properties;
    std::vector<uint64_t> values;
    void add(const Properties &object, std::initializer_list<std::pair<const char *, uint64_t>> list) {
        objects.push_back(object.id);
        counts.push_back(list.size());
        for (const auto &item : list) {
            properties.push_back(object.keys.at(item.first));
            values.push_back(item.second);
        }
    }
    void plane(const Properties &object, uint32_t crtc, uint32_t fb, int sw, int sh, int dw, int dh) {
        add(object, {{"FB_ID", fb},
                     {"CRTC_ID", fb ? crtc : 0},
                     {"SRC_X", 0},
                     {"SRC_Y", 0},
                     {"SRC_W", uint64_t(sw) << 16},
                     {"SRC_H", uint64_t(sh) << 16},
                     {"CRTC_X", 0},
                     {"CRTC_Y", 0},
                     {"CRTC_W", uint64_t(dw)},
                     {"CRTC_H", uint64_t(dh)}});
        if (object.keys.count("pixel blend mode")) {
            ++counts.back();
            properties.push_back(object.keys.at("pixel blend mode"));
            values.push_back(1); // DRM_MODE_BLEND_COVERAGE (straight alpha)
        }
    }
    bool commit(int fd, uint32_t flags, uint64_t cookie = 0) {
        drm_mode_atomic a{};
        a.flags = flags;
        a.count_objs = objects.size();
        a.objs_ptr = uintptr_t(objects.data());
        a.count_props_ptr = uintptr_t(counts.data());
        a.props_ptr = uintptr_t(properties.data());
        a.prop_values_ptr = uintptr_t(values.data());
        a.user_data = cookie;
        if (flags & DRM_MODE_ATOMIC_TEST_ONLY)
            return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a) == 0;
        ctl(fd, DRM_IOCTL_MODE_ATOMIC, &a, "atomic plane commit");
        return true;
    }
};
class Ar9311Screen final : public PlaneScreen {
    struct Buffer {
        uint32_t handle = 0, fb = 0, pitch = 0;
        uint64_t size = 0;
        uint8_t *pixels = nullptr;
    };
    // A decoder buffer's framebuffer, made the first time the buffer is shown
    // and kept while the decoder holds that buffer (its frame_ref): the
    // capture pool is a fixed set of DMA-BUFs, and importing every picture
    // again was four ioctls a frame on this thread (PRIME + ADDFB2, then RMFB
    // + GEM_CLOSE). Owners are matched by their control block, which a
    // weak_ptr keeps from being reused while the entry exists.
    struct CachedFb {
        std::weak_ptr<void> owner;
        uint32_t handle = 0, fb = 0, width = 0, height = 0;
        std::array<uint32_t, 3> pitches{}, offsets{};
    };
    // A picture on its way to the screen or on it: its lease, and its fb.
    struct Shown {
        std::shared_ptr<DecodedUnit> frame;
        uint32_t fb = 0;
    };
    std::vector<CachedFb> fbs_;   // the KMS thread's (run)
    void release(CachedFb &c) {
        if (c.fb)
            ioctl(fd_, DRM_IOCTL_MODE_RMFB, &c.fb);
        if (c.handle) {
            drm_gem_close close{};
            close.handle = c.handle;
            ioctl(fd_, DRM_IOCTL_GEM_CLOSE, &close);
        }
        c.fb = c.handle = 0;
    }
    static bool same_owner(const std::weak_ptr<void> &a, const std::shared_ptr<void> &b) {
        return !a.owner_before(b) && !b.owner_before(a);
    }
    // Buffers their decoder has let go. None is on the screen: a shown
    // picture's lease (Shown::frame) keeps its owner alive.
    void drop_expired_fbs() {
        for (auto it = fbs_.begin(); it != fbs_.end();) {
            if (it->owner.expired()) {
                release(*it);
                it = fbs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    void drop_all_fbs() {
        for (auto &c : fbs_)
            release(c);
        fbs_.clear();
    }
    uint32_t framebuffer(const DecodedUnit &f) {
        drop_expired_fbs();
        const std::array<uint32_t, 3> pitches{f.pitches[0], f.pitches[1], f.pitches[2]},
            offsets{f.offsets[0], f.offsets[1], f.offsets[2]};
        for (auto it = fbs_.begin(); it != fbs_.end(); ++it) {
            if (!same_owner(it->owner, f.frame_ref))
                continue;
            if (it->width == f.width && it->height == f.height && it->pitches == pitches && it->offsets == offsets)
                return it->fb;
            // The same buffer laid out anew: it is not on the screen, since
            // the decoder could not have handed it out again while leased.
            release(*it);
            fbs_.erase(it);
            break;
        }
        CachedFb c;
        c.owner = f.frame_ref;
        c.width = f.width;
        c.height = f.height;
        c.pitches = pitches;
        c.offsets = offsets;
        try {
            drm_prime_handle prime{};
            prime.fd = f.prime_fd;
            ctl(fd_, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime, "import decoded DMA-BUF");
            c.handle = prime.handle;
            drm_mode_fb_cmd2 cmd{};
            cmd.width = f.width;
            cmd.height = f.height;
            cmd.pixel_format = DRM_FORMAT_YUV420;
            for (int i = 0; i < 3; ++i) {
                cmd.handles[i] = c.handle;
                cmd.pitches[i] = pitches[i];
                cmd.offsets[i] = offsets[i];
            }
            ctl(fd_, DRM_IOCTL_MODE_ADDFB2, &cmd, "decoded framebuffer");
            c.fb = cmd.fb_id;
        } catch (...) {
            release(c);
            throw;
        }
        fbs_.push_back(std::move(c));
        return fbs_.back().fb;
    }
    enum class Slot { Free, Writing, Ready, Pending, Active };
    int width_, height_;
    int fd_ = -1;
    // Match the last decoded source during flush/idle, so changing decoders
    // does not reconfigure the scaler or interrupt the HDMI signal.
    Buffer black_, black_720_;
    // A blank at any other source size (864p), so a flush keeps the
    // scaler's geometry instead of forcing a modeset. No new memory (CMA is
    // nearly all taken): a framebuffer over the 1080p blank, its Y plane in
    // that buffer's black luma, both chroma planes in its neutral chroma.
    uint32_t other_fb_ = 0;
    int other_w_ = 0, other_h_ = 0;
    void drop_other_fb() {
        if (other_fb_) ioctl(fd_, DRM_IOCTL_MODE_RMFB, &other_fb_);
        other_fb_ = 0; other_w_ = other_h_ = 0;
    }
    uint32_t blank_fb(int w, int h) {
        if (w == 1920 && h == 1080) return black_.fb;
        if (w == 1280 && h == 720 && black_720_.fb) return black_720_.fb;
        if (w != other_w_ || h != other_h_) {
            // Never on screen here: what is showing has this new size.
            drop_other_fb();
            drm_mode_fb_cmd2 fb{};
            fb.width = w; fb.height = h;
            fb.pixel_format = DRM_FORMAT_YUV420;
            fb.handles[0] = fb.handles[1] = fb.handles[2] = black_.handle;
            fb.pitches[0] = w;
            fb.pitches[1] = fb.pitches[2] = w / 2;
            fb.offsets[1] = 1920 * 1080;
            // The firmware takes plane offsets in multiples of 64 only.
            fb.offsets[2] = 1920 * 1080 + ((w / 2) * (h / 2) + 63) / 64 * 64;
            ctl(fd_, DRM_IOCTL_MODE_ADDFB2, &fb, "blank framebuffer");
            other_fb_ = fb.fb_id; other_w_ = w; other_h_ = h;
        }
        return other_fb_;
    }
    // I420 the VO scaler takes: even sizes, up to 2x smaller or larger - on
    // firmware that scales any size (any_scale_, probed at open). Older
    // firmware shows the screen's own size and 1080p only: anything else is
    // dropped and said, not committed (a refused commit ends the session).
    bool any_scale_ = false;
    bool scannable(int w, int h) const {
        if (!any_scale_)
            return (w == 1920 && h == 1080) || (w == width_ && h == height_);
        return w > 0 && h > 0 && !(w & 1) && !(h & 1) && w <= 1920 && h <= 1080 &&
               w * 2 >= width_ && w <= width_ * 2 && h * 2 >= height_ && h <= height_ * 2;
    }
    std::array<Buffer, 3> ui_{};
    std::array<Slot, 3> slots_{};
    std::array<base::DamageRows, 3> ui_damage_;
    bool first_overlay_ = true;
    std::vector<AscentMode> supported_modes_;
    Properties connector_, crtc_, primary_, overlay_;
    uint32_t mode_ = 0;
    bool active_ = false;
    std::atomic<bool> *stop_;
    mutable std::mutex mutex_;
    VideoStats video_stats_;
    uint64_t stats_start_ = 0, stats_frames_ = 0;
    std::vector<float> latency_samples_;
    std::condition_variable wake_;
    std::shared_ptr<DecodedUnit> latest_;
    bool done_ = false, video_active_ = false;
    std::atomic<uint64_t> presented_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> hold_video_{false};
    uint64_t video_updates_ = 0;
    std::atomic<uint64_t> ui_updates_{0};

    // Screen follows camera (display/clock_lock.hpp): the pixel clock trimmed
    // through fpvOS's artosyn_dsi.clock_trim_ppb so pictures are ready a fixed
    // time before the commit deadline, never at it. Ready times are taken as
    // pictures come (queue_frame), the refresh from the flips' vblanks; once
    // a second the trim is moved. The camera's rate (the trim that holds the
    // slack) is kept for the next start, where the DSI comes up with it before
    // the LT9611 locks. /tmp/kestrel-screenlock-off holds the trim where it is.
    static constexpr const char *kTrimParam = "/sys/module/artosyn_dsi/parameters/clock_trim_ppb";
    // One line per air unit ("<MAC hex> <ppb>") and "default <ppb>", the rate
    // a start begins with; a file of one number is that default.
    static constexpr const char *kTrimSaved = "/var/lib/kestrel/screen-clock-ppb";
    std::map<std::string, long long> saved_rates_;
    std::string source_, pending_source_;  // pending_source_ under mutex_
    // In order: while pictures come within kNearUs of the deadline (either
    // side), jitter puts two in one refresh and none in the next - newest
    // first loses one and repeats one. Held here instead and shown oldest
    // first, one a refresh (at most two waiting: one refresh more latency),
    // until the phase has been clear of the deadline for kClearS seconds.
    static constexpr double kNearUs = 2500;
    static constexpr unsigned kClearS = 10;
    std::deque<std::shared_ptr<DecodedUnit>> held_;  // under mutex_
    std::atomic<bool> in_order_{false};
    unsigned in_order_clear_s_ = 0, in_order_s_ = 0;
    std::atomic<unsigned> held_dropped_{0};
    void set_in_order(bool on) {
        if (on == in_order_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        in_order_ = on;
        held_.clear();
        printf("screen lock: %s\n", on ? "pictures near the deadline - shown in order, one a refresh"
                                       : "pictures clear of the deadline - newest first again");
    }
    std::vector<uint64_t> ready_us_;  // under mutex_
    display::ClockLock clock_lock_;
    std::atomic<bool> clock_lock_on_{false};
    double refresh_us_ = 0;
    uint64_t lock_next_us_ = 0, lock_log_us_ = 0, lock_saved_us_ = 0, prev_vblank_us_ = 0;
    long long trim_written_ppb_ = LLONG_MIN;
    std::vector<double> lock_slacks_;  // since the last log line
    unsigned lock_late_ = 0;           // pictures that missed their deadline since then
    // Where refreshes go without a new picture, since the last log line:
    // flips that landed a vblank late (the commit went after its deadline),
    // pictures replaced by a newer one before they were shown.
    unsigned lock_missed_vblanks_ = 0;
    std::atomic<unsigned> lock_replaced_{0};
    bool latest_taken_ = true;  // under mutex_: latest_ was picked up by the loop

    static bool write_text(const char *path, const std::string &text) {
        FILE *f = std::fopen(path, "w");
        if (!f) return false;
        const bool ok = std::fputs(text.c_str(), f) >= 0;
        return std::fclose(f) == 0 && ok;
    }
    void write_trim(double ppm) {
        const long long ppb = std::llround(ppm * 1000);
        if (ppb == trim_written_ppb_) return;
        if (write_text(kTrimParam, std::to_string(ppb) + "\n")) trim_written_ppb_ = ppb;
    }
    // The lock moves the trim once a second; the clock gets there in a
    // straight line over that second, a little at every refresh (a 4 ppm
    // jump at once made the LT9611's PCR re-lock now and then: a black
    // screen).
    double ramp_from_ = 0, ramp_to_ = 0;
    uint64_t ramp_start_us_ = 0;
    void ramp_trim(double ppm, uint64_t now) {
        ramp_from_ = ramp_now(now);
        ramp_to_ = ppm;
        ramp_start_us_ = now;
    }
    double ramp_now(uint64_t now) const {
        if (!ramp_start_us_) return ramp_to_;
        const double f = std::clamp(double(now - ramp_start_us_) / 1e6, 0.0, 1.0);
        return ramp_from_ + (ramp_to_ - ramp_from_) * f;
    }
    // Before the first modeset: the saved rate, so the bridge locks to it.
    void clock_lock_start() {
        if (access(kTrimParam, W_OK) != 0) {
            printf("screen lock: no artosyn_dsi.clock_trim_ppb on this firmware\n");
            return;
        }
        double ppm = 0;
        if (FILE *f = std::fopen(kTrimSaved, "r")) {
            char line[96], key[32], extra;
            long long ppb = 0;
            while (std::fgets(line, sizeof line, f)) {
                if (std::sscanf(line, "%31s %lld %c", key, &ppb, &extra) == 2) saved_rates_[key] = ppb;
                else if (std::sscanf(line, "%lld %c", &ppb, &extra) == 1) saved_rates_["default"] = ppb;  // the old form
            }
            std::fclose(f);
        }
        for (auto it = saved_rates_.begin(); it != saved_rates_.end();)
            it = std::llabs(it->second) <= 2500000 ? std::next(it) : saved_rates_.erase(it);
        if (saved_rates_.count("default")) ppm = saved_rates_["default"] / 1000.0;
        clock_lock_ = display::ClockLock(ppm);
        clock_lock_on_ = true;
        ramp_from_ = ramp_to_ = ppm;
        ramp_start_us_ = 0;
        write_trim(ppm);
        printf("screen lock: pixel clock trim %+.1f ppm at start (%s)\n", ppm,
               ppm != 0 ? "saved" : "none saved");
    }
    // After each flip: the refresh from the vblanks; once a second the lock.
    void clock_lock_tick(uint64_t vblank_us) {
        if (prev_vblank_us_ && vblank_us > prev_vblank_us_) {
            const double d = double(vblank_us - prev_vblank_us_);
            const double k = std::round(d / double(frame_us_));
            if (k >= 2 && k <= 3 && video_active_) ++lock_missed_vblanks_;
            if (k >= 1 && k <= 3) {
                const double sample = d / k;
                refresh_us_ = refresh_us_ ? refresh_us_ + 0.02 * (sample - refresh_us_) : sample;
            }
        }
        prev_vblank_us_ = vblank_us;
        const uint64_t now = clock_us();
        if (clock_lock_on_) write_trim(ramp_now(now));
        if (!clock_lock_on_ || now < lock_next_us_ || !refresh_us_) return;
        lock_next_us_ = now + 1000000;
        // Another air unit (told at link-up, before its pictures): the lock
        // aims at its own camera rate, slewing there (a step would be a PCR
        // re-lock, seen as a black screen: six of six on the SteadyDigital).
        std::string source;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            source = pending_source_;
        }
        if (!source.empty() && source != source_) {
            source_ = source;
            const auto it = saved_rates_.find(source_);
            if (it != saved_rates_.end()) {
                const double ppm = it->second / 1000.0;
                clock_lock_.expect_rate(ppm);
                printf("screen lock: air unit %s, camera %+.1f ppm (saved), trim %+.1f ppm going there\n",
                       source_.c_str(), ppm, clock_lock_.trim_ppm());
            } else {
                clock_lock_.reset();
                printf("screen lock: air unit %s, camera rate not known yet\n", source_.c_str());
            }
        }
        std::vector<uint64_t> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready.swap(ready_us_);
        }
        // 1:1 only, with enough pictures to go by.
        if (ready.size() < 30) { set_in_order(false); return; }
        const double interval = double(ready.back() - ready.front()) / double(ready.size() - 1);
        if (std::fabs(interval / refresh_us_ - 1) > 0.03) { set_in_order(false); return; }
        const double P = refresh_us_, V = double(vblank_us);
        std::vector<double> slacks;
        slacks.reserve(ready.size());
        for (const uint64_t r : ready) {
            // The first deadline (a vblank less the lead) not before r.
            const double t = double(r) + double(kLeadUs);
            const double n = V + std::ceil((t - V) / P) * P;
            const double slack = n - t;
            slacks.push_back(slack);
            lock_slacks_.push_back(slack);
        }
        std::vector<double> sorted = slacks;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double median = sorted[sorted.size() / 2];
        // Late: ready past the deadline the slack's median aims at.
        unsigned near = 0;
        for (const double x : slacks) {
            if (std::fabs(x - median) > P / 2) ++lock_late_;
            if (x < kNearUs || x > P - kNearUs) ++near;
        }
        if (near) {
            in_order_clear_s_ = 0;
            set_in_order(true);
        } else if (in_order_ && ++in_order_clear_s_ >= kClearS) {
            set_in_order(false);
        }
        if (in_order_) ++in_order_s_;
        if (access("/tmp/kestrel-screenlock-off", F_OK) == 0) return;
        // Test hook: another target slack.
        if (FILE *f = std::fopen("/tmp/kestrel-screenlock-target-us", "r")) {
            double us = 0;
            if (std::fscanf(f, "%lf", &us) == 1 && us >= 500 && us <= 14000) clock_lock_.set_target_us(us);
            std::fclose(f);
        }
        const double ppm = clock_lock_.update(double(now) / 1e6, median, P);
        ramp_trim(ppm, now);
        if (now >= lock_log_us_) {
            lock_log_us_ = now + 5000000;
            std::sort(lock_slacks_.begin(), lock_slacks_.end());
            const auto at = [&](double q) { return lock_slacks_[size_t(q * double(lock_slacks_.size() - 1))]; };
            printf("screen lock: slack min %.0f p01 %.0f p05 %.0f p50 %.0f p95 %.0f us (target %.0f), trim %+.1f ppm, camera %+.1f ppm%s, refresh %.2f us, off-median %u, late flips %u, replaced %u, in order %us (held dropped %u)\n",
                   lock_slacks_.front(), at(0.01), at(0.05), at(0.5), at(0.95), clock_lock_.target_us(), ppm,
                   clock_lock_.base_ppm(), clock_lock_.estimated() ? "" : " (not yet)", P, lock_late_,
                   lock_missed_vblanks_, lock_replaced_.exchange(0), in_order_s_, held_dropped_.exchange(0));
            lock_missed_vblanks_ = 0;
            in_order_s_ = 0;
            lock_slacks_.clear();
            lock_late_ = 0;
        }
        if (clock_lock_.estimated() && clock_lock_.updates() >= 120 && now >= lock_saved_us_) {
            lock_saved_us_ = now + 60000000;
            const long long ppb = std::llround(clock_lock_.base_ppm() * 1000);
            saved_rates_["default"] = ppb;
            if (!source_.empty()) saved_rates_[source_] = ppb;
            std::string text;
            for (const auto &[key, value] : saved_rates_) text += key + " " + std::to_string(value) + "\n";
            write_text(kTrimSaved, text);
        }
    }
    std::atomic<uint64_t> rejected_frames_{0};

    void destroy(Buffer &b) {
        if (b.pixels)
            munmap(b.pixels, b.size);
        if (b.fb)
            ioctl(fd_, DRM_IOCTL_MODE_RMFB, &b.fb);
        if (b.handle) {
            drm_mode_destroy_dumb d{};
            d.handle = b.handle;
            ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        b = {};
    }
    void disable() {
        if (!active_)
            return;
        Atomic a;
        a.plane(primary_, crtc_.id, 0, width_, height_, width_, height_);
        a.plane(overlay_, crtc_.id, 0, width_, height_, width_, height_);
        a.add(crtc_, {{"ACTIVE", 0}, {"MODE_ID", 0}});
        a.add(connector_, {{"CRTC_ID", 0}});
        a.commit(fd_, DRM_MODE_ATOMIC_ALLOW_MODESET);
        active_ = false;
    }
    void close_display() {
        if (fd_ < 0)
            return;
        try {
            disable();
        } catch (const std::exception &e) {
            fprintf(stderr, "display stop: %s\n", e.what());
        }
        drop_all_fbs();
        for (auto &b : ui_)
            destroy(b);
        destroy(black_);
        drop_other_fb();
        destroy(black_720_);
        if (mode_) {
            drm_mode_destroy_blob b{};
            b.blob_id = mode_;
            ioctl(fd_, DRM_IOCTL_MODE_DESTROYPROPBLOB, &b);
        }
        close(fd_);
        fd_ = -1;
    }
    void create(Buffer &b, bool rgb, int width, int height) {
        drm_mode_create_dumb d{};
        d.width = width;
        d.height = rgb ? height : height*3/2;
        d.bpp = rgb ? 32 : 8;
        ctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &d, "plane buffer allocation");
        b.handle = d.handle;
        b.pitch = d.pitch;
        b.size = d.size;
        if (d.pitch != unsigned(rgb ? ((width+511)&~511)*4 : width) || d.size < uint64_t(d.pitch) * d.height)
            throw std::runtime_error("unsupported plane pitch/size");
        drm_mode_fb_cmd2 fb{};
        fb.width = width;
        fb.height = height;
        fb.pixel_format = rgb ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_YUV420;
        fb.handles[0] = d.handle;
        fb.pitches[0] = d.pitch;
        if (!rgb) {
            fb.handles[1] = fb.handles[2] = d.handle;
            fb.pitches[1] = fb.pitches[2] = width/2;
            fb.offsets[1] = width * height;
            fb.offsets[2] = width * height * 5 / 4;
        }
        ctl(fd_, DRM_IOCTL_MODE_ADDFB2, &fb, "plane framebuffer");
        b.fb = fb.fb_id;
        drm_mode_map_dumb m{};
        m.handle = b.handle;
        ctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &m, "plane mapping");
        void *pixels = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, m.offset);
        if (pixels == MAP_FAILED)
            throw std::runtime_error("cannot map plane buffer");
        b.pixels = static_cast<uint8_t *>(pixels);
        memset(b.pixels, rgb ? 0 : 128, b.size);
        if (!rgb)
            memset(b.pixels, 16, width * height);
    }
    // The vblank of the last flip (CLOCK_MONOTONIC, the kernel's stamp) and
    // the refresh period: when the next one comes.
    uint64_t last_vblank_us_ = 0, frame_us_ = 16667;
    // How long a start waits for the HDMI bridge to report the screen.
    static constexpr int kConnectWaitMs = 3000, kConnectPollMs = 50;
    // One commit a frame, this long before the vblank, with the newest
    // picture and UI: a commit is applied at the next vblank (one made 0.8 ms
    // before it still was), and only one can be pending. Committed as soon
    // as a picture came, it was pending all frame - right after a vblank, the
    // loop being free only then - and every picture decoded mid-frame waited
    // for the vblank after next: a whole frame more (measured 15.6-16.6 ms
    // from the commit to the screen, every picture).
    static constexpr uint64_t kLeadUs = 2000;
    // When the next commit goes: 0 (now) without a vblank to go by.
    uint64_t ui_deadline(uint64_t now) const {
        if (!last_vblank_us_ || now < last_vblank_us_ || now - last_vblank_us_ > 1000000) return 0;
        const uint64_t next = last_vblank_us_ + ((now - last_vblank_us_) / frame_us_ + 1) * frame_us_;
        return next > kLeadUs ? next - kLeadUs : 0;
    }
    void wait_flip(uint64_t cookie, uint32_t fb) {
        pollfd p{fd_, POLLIN, 0};
        int r;
        do {
            r = poll(&p, 1, 3000);
        } while (r < 0 && errno == EINTR);
        if (r <= 0 || !(p.revents & POLLIN))
            throw std::runtime_error("HDMI flip timed out");
        alignas(drm_event_vblank) char events[1024];
        const ssize_t n = read(fd_, events, sizeof(events));
        for (size_t at = 0; n > 0 && at + sizeof(drm_event) <= size_t(n);) {
            auto *e = reinterpret_cast<drm_event *>(events + at);
            if (e->length < sizeof(*e) || e->length > size_t(n) - at)
                break;
            if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(drm_event_vblank) &&
                reinterpret_cast<drm_event_vblank *>(e)->user_data == cookie) {
                auto* vblank = reinterpret_cast<drm_event_vblank *>(e);
                const uint64_t at_us = uint64_t(vblank->tv_sec) * 1000000 + vblank->tv_usec;
                if (ltrace::on()) ltrace::rec(ltrace::kFlipDone, clock_us(), fb, at_us);
                last_vblank_us_ = at_us;
                return;
            }
            at += e->length;
        }
        throw std::runtime_error("missing matching HDMI flip completion");
    }

  public:
    Ar9311Screen(const std::string &device, bool force, std::atomic<bool> *stop, int width, int height, int hz) : width_(width), height_(height), ui_damage_{{{width,height},{width,height},{width,height}}}, stop_(stop) {
        try {
            if (!AscentMode{height,hz}.valid() || AscentMode{height,hz}.width()!=width)
                throw std::invalid_argument("Unsupported Ascent screen mode");
            frame_us_ = hz > 0 ? uint64_t(1000000 / hz) : 16667;
            fd_ = open(device.c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ < 0)
                throw std::runtime_error("cannot open " + device);
            drm_set_client_cap cap{};
            cap.capability = DRM_CLIENT_CAP_ATOMIC;
            cap.value = 1;
            ctl(fd_, DRM_IOCTL_SET_CLIENT_CAP, &cap, "atomic KMS");
            ctl(fd_, DRM_IOCTL_SET_MASTER, nullptr, "DRM master");
            uint32_t crtcs[16]{}, connectors[16]{};
            drm_mode_card_res r{};
            r.crtc_id_ptr = uintptr_t(crtcs);
            r.count_crtcs = 16;
            r.connector_id_ptr = uintptr_t(connectors);
            r.count_connectors = 16;
            ctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &r, "HDMI resources");
            if (r.count_crtcs != 1 || r.count_connectors != 1)
                throw std::runtime_error("expected one CRTC and connector");
            crtc_ = Properties(fd_, crtcs[0], DRM_MODE_OBJECT_CRTC);
            connector_ = Properties(fd_, connectors[0], DRM_MODE_OBJECT_CONNECTOR);
            // At boot the receiver can start before the HDMI bridge has seen
            // the screen (the SteadyDigital's root mounts from flash, so it
            // starts ~1 s into the boot): ask again for up to 3 s before giving
            // up, rather than exiting and losing the supervisor's 2 s backoff.
            drm_mode_get_connector c{};
            std::vector<drm_mode_modeinfo> modes;
            for (int waited_ms = 0;; waited_ms += kConnectPollMs) {
                c = {};
                c.connector_id = connectors[0];
                ctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &c, "connector status");
                modes.resize(c.count_modes);
                c.modes_ptr = uintptr_t(modes.data());
                c.count_props = c.count_encoders = 0;
                ctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &c, "HDMI modes");
                const bool settled = c.count_modes <= modes.size();
                if (settled && (c.connection == 1 || force)) {
                    if (waited_ms)
                        std::fprintf(stderr, "display: HDMI screen reported after %d ms\n", waited_ms);
                    break;
                }
                if (waited_ms >= kConnectWaitMs || (stop_ && stop_->load()))
                    throw std::runtime_error(settled ? "HDMI disconnected; use --force-display"
                                                     : "HDMI modes changed");
                usleep(kConnectPollMs * 1000);
            }
            drm_mode_get_plane_res planes{};
            ctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes, "plane count");
            std::vector<uint32_t> ids(planes.count_planes);
            planes.plane_id_ptr = uintptr_t(ids.data());
            ctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes, "planes");
            if (planes.count_planes > ids.size())
                throw std::runtime_error("plane list changed");
            for (auto id : ids) {
                drm_mode_get_plane plane{};
                plane.plane_id = id;
                ctl(fd_, DRM_IOCTL_MODE_GETPLANE, &plane, "plane capabilities");
                std::vector<uint32_t> formats(plane.count_format_types);
                plane.format_type_ptr = uintptr_t(formats.data());
                ctl(fd_, DRM_IOCTL_MODE_GETPLANE, &plane, "plane formats");
                if (!(plane.possible_crtcs & 1))
                    continue;
                Properties props(fd_, id, DRM_MODE_OBJECT_PLANE);
                const auto type = props.values.at("type");
                for (auto format : formats) {
                    if (type == 1 /* DRM primary type */ && format == DRM_FORMAT_YUV420)
                        primary_ = props;
                    if (type == 0 /* DRM overlay type */ && format == DRM_FORMAT_ARGB8888)
                        overlay_ = props;
                }
            }
            if (!primary_.id || !overlay_.id)
                throw std::runtime_error("fpvOS needs YUV primary and ARGB hardware overlay planes");
            auto m=ascent_kms_mode({height,hz});
            drm_mode_create_blob blob{};
            blob.data = uintptr_t(&m);
            blob.length = sizeof(m);
            ctl(fd_, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob, "Ascent screen mode");
            mode_ = blob.blob_id;
            create(black_, false, 1920, 1080);
            if (height_ == 720)
                create(black_720_, false, 1280, 720);
            for (auto &b : ui_)
                create(b, true, width_, height_);
            // Reserve before producers can publish: this buffer is scanned
            // during bootstrap, so overlay_damage must not write into it.
            slots_[0]=Slot::Active;
            for(int y=0;y<height_;++y) {
                auto* row=reinterpret_cast<uint32_t*>(ui_[0].pixels+y*ui_[0].pitch);
                std::fill(row,row+width_,0xff000000u);
            }
            // EDID alone does not prove that installed firmware can drive a
            // mode. TEST_ONLY also checks the fixed 1080p-to-720p video path.
            // --force-display permits this probe without a valid EDID.
            for(const auto mode:ascent_modes) {
                auto candidate=ascent_kms_mode(mode);
                const bool advertised=std::any_of(modes.begin(),modes.begin()+c.count_modes,[&](const auto& edid) {
                    return same_ascent_timing(edid,candidate);
                });
                if(!force && !advertised) continue;
                drm_mode_create_blob probe{};
                probe.data=uintptr_t(&candidate); probe.length=sizeof(candidate);
                ctl(fd_,DRM_IOCTL_MODE_CREATEPROPBLOB,&probe,"HDMI capability probe");
                Atomic test;
                test.plane(primary_,crtc_.id,black_.fb,1920,1080,mode.width(),mode.height);
                test.plane(overlay_,crtc_.id,0,mode.width(),mode.height,mode.width(),mode.height);
                test.add(connector_,{{"CRTC_ID",crtc_.id}});
                test.add(crtc_,{{"MODE_ID",probe.blob_id},{"ACTIVE",1}});
                if(test.commit(fd_,DRM_MODE_ATOMIC_ALLOW_MODESET|DRM_MODE_ATOMIC_TEST_ONLY)) {
                    supported_modes_.push_back(mode);
                    printf("display: supported HDMI mode %s%s\n",mode.name().c_str(),advertised?"":" (forced)");
                } else {
                    fprintf(stderr,"display: firmware rejected %s: %s\n",mode.name().c_str(),strerror(errno));
                }
                drm_mode_destroy_blob destroy_probe{};destroy_probe.blob_id=probe.blob_id;
                ioctl(fd_,DRM_IOCTL_MODE_DESTROYPROPBLOB,&destroy_probe);
            }
            if(!supports_mode({height,hz}))
                throw std::runtime_error("Requested HDMI mode is not supported by the display and firmware");
            {
                // Does this firmware scale a size that is neither the screen's
                // nor 1080p (fpvOS patch 0034)? Asked, never assumed.
                Atomic test;
                test.plane(primary_, crtc_.id, blank_fb(1536, 864), 1536, 864, width_, height_);
                test.plane(overlay_, crtc_.id, 0, width_, height_, width_, height_);
                test.add(connector_, {{"CRTC_ID", crtc_.id}});
                test.add(crtc_, {{"MODE_ID", mode_}, {"ACTIVE", 1}});
                any_scale_ = test.commit(fd_, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY);
                drop_other_fb();
                printf("display: video sizes other than the screen's and 1080p: %s\n",
                       any_scale_ ? "scaled by the firmware" : "not on this firmware");
            }
            printf("display: hardware planes video=%u UI=%u, DMA-BUF scanout\n", primary_.id, overlay_.id);
        } catch (...) {
            close_display();
            throw;
        }
    }
    ~Ar9311Screen() override { close_display(); }
    void queue_frame(std::shared_ptr<DecodedUnit> frame) override {
        if (!frame)
            return;
        if (!scannable(int(frame->width), int(frame->height)) ||
            !frame->has_prime_fd || frame->prime_fd < 0 || frame->drm_pixel_format != DRM_FORMAT_YUV420 || !frame->frame_ref) {
            // A picture this pipeline cannot scan out (another size or
            // format) says nothing about HDMI: dropped, and said, rather than
            // a display failure that rolled the screen mode back or ended the
            // session in a restart loop.
            const auto n = ++rejected_frames_;
            if (n == 1 || n % 600 == 0)
                fprintf(stderr, "display: %llu decoded frames dropped as unsupported (last %ux%u, format %08x)\n",
                        (unsigned long long)n, frame->width, frame->height, frame->drm_pixel_format);
            std::lock_guard<std::mutex> lock(mutex_);
            video_stats_.rejected_us = clock_us();
            video_stats_.rejected_width = int(frame->width);
            video_stats_.rejected_height = int(frame->height);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (latest_ && !latest_taken_ && !in_order_) ++lock_replaced_;
            if (in_order_) {
                held_.push_back(frame);
                while (held_.size() > 2) { held_.pop_front(); ++held_dropped_; }
            }
            latest_ = std::move(frame);
            latest_taken_ = false;
            if (clock_lock_on_ && ready_us_.size() < 1024) ready_us_.push_back(clock_us());
        }
        wake_.notify_all();
    }
    void set_source(const std::string &id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_source_ = id;
    }
    void request_flush() override {
        std::unique_lock<std::mutex> lock(mutex_);
        latest_.reset();
        held_.clear();
        wake_.notify_all();
        wake_.wait(lock, [this] { return done_ || !video_active_; });
    }
    void overlay(const uint32_t *argb, unsigned width, unsigned height) override {
        overlay_damage(argb, width, height, {{0, 0, int(width), int(height)}});
    }
    void overlay_damage(const uint32_t *argb, unsigned width, unsigned height,
                        const std::vector<base::Damage>& damage) override {
        if (!argb || width != unsigned(width_) || height != unsigned(height_))
            throw std::invalid_argument("UI plane geometry");
        // This state belongs exclusively to the UI producer. Even a dropped
        // update must remain dirty in every buffer until its next upload.
        for (auto& dirty : ui_damage_) {
            if (first_overlay_) dirty.all();
            for (const auto& area : damage) dirty.add(area);
        }
        first_overlay_ = false;
        int slot = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_)
                return;
            for (int i = 0; i < 3; ++i)
                if (slots_[i] == Slot::Free) {
                    slot = i;
                    break;
                }
            if (slot < 0)
                for (int i = 0; i < 3; ++i)
                    if (slots_[i] == Slot::Ready) {
                        slot = i;
                        break;
                    }
            if (slot < 0)
                return;
            slots_[slot] = Slot::Writing;
        }
        // Upload on UI thread; display/video never waits for this copy or LVGL.
        auto &b = ui_[slot];
        auto& dirty = ui_damage_[slot];
        for (unsigned row = 0; row < height; ++row) {
            const auto [left, right] = dirty.rows()[row];
            if (left < right)
                memcpy(b.pixels + row * b.pitch + left * 4, argb + row * width + left, (right - left) * 4);
        }
        dirty.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &state : slots_)
                if (state == Slot::Ready)
                    state = Slot::Free;
            slots_[slot] = Slot::Ready;
        }
        wake_.notify_all();
    }
    bool supports_mode(AscentMode mode) const override {
        return std::find(supported_modes_.begin(),supported_modes_.end(),mode)!=supported_modes_.end();
    }
    VideoStats video_stats() const override {
        std::lock_guard<std::mutex> lock(mutex_); return video_stats_;
    }
    uint64_t presented() const override { return presented_.load(); }
    uint64_t overlay_presented() const override { return ui_updates_.load(); }
    void hold_video(bool on) override { hold_video_=on;wake_.notify_all(); }
    bool failed() const override { return failed_.load(); }
    void run() override {
        Shown showing, pending;
        int active_ui = 0, next_ui = -1;
        int source_width = width_, source_height = height_;
        uint64_t cookie = 0;
        // Commits go 2 ms before the vblank; on two cores shared with the
        // decoder and the HUD's software rendering a normal thread woke past
        // that now and then (a picture a refresh late, the next replacing it).
        // Below the radio's receive thread (20).
        SchedulingHelper::set_thread_params_max_realtime("AR9311_SCREEN", 15);
        try {
            clock_lock_start();
            Atomic initial;
            // Prime the hardware at native size, then establish the 1080p
            // decoder geometry before publishing UI or live frames. The VO
            // needs this native-size reset sequence even for 720p output.
            // An opaque UI buffer hides the bootstrap video surface.
            initial.plane(primary_, crtc_.id, height_ == 720 ? black_720_.fb : black_.fb,
                          source_width, source_height, width_, height_);
            initial.plane(overlay_, crtc_.id, ui_[0].fb, width_, height_, width_, height_);
            initial.add(connector_, {{"CRTC_ID", crtc_.id}});
            initial.add(crtc_, {{"MODE_ID", mode_}, {"ACTIVE", 1}});
            initial.commit(fd_, DRM_MODE_ATOMIC_ALLOW_MODESET);
            active_ = true;
            if(height_==720) {
                Atomic prepare;
                prepare.plane(primary_,crtc_.id,black_.fb,1920,1080,width_,height_);
                prepare.plane(overlay_,crtc_.id,ui_[0].fb,width_,height_,width_,height_);
                prepare.commit(fd_,DRM_MODE_ATOMIC_ALLOW_MODESET);
                source_width=1920; source_height=1080;
            }
            while (!*stop_ && !failed_) {
                drop_expired_fbs();
                std::shared_ptr<DecodedUnit> frame;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    // In order: the oldest held picture, else nothing new.
                    frame = !in_order_ ? latest_ : held_.empty() ? showing.frame : held_.front();
                    if (!hold_video_ && frame && frame->dec_end_ts && clock_us() - frame->dec_end_ts > 2000000)
                        frame.reset();
                    next_ui = active_ui;
                    // Anything new goes up just before the vblank, with
                    // whatever else came meanwhile (see kLeadUs).
                    if (frame != showing.frame ||
                        std::find(slots_.begin(), slots_.end(), Slot::Ready) != slots_.end()) {
                        const uint64_t now = clock_us(), deadline = ui_deadline(now);
                        if (deadline > now) {
                            wake_.wait_for(lock, std::chrono::microseconds(std::min<uint64_t>(deadline - now, 2000)));
                            continue;
                        }
                    }
                    if (frame && frame == latest_) latest_taken_ = true;  // committed now
                    if (in_order_ && !held_.empty() && frame == held_.front()) held_.pop_front();
                    for (int i = 0; i < 3; ++i)
                        if (slots_[i] == Slot::Ready) {
                            next_ui = i;
                            slots_[i] = Slot::Pending;
                        }
                    if (frame == showing.frame && next_ui == active_ui) {
                        wake_.wait_for(lock, std::chrono::milliseconds(2));
                        continue;
                    }
                    // Include the import/commit interval in flush ownership.
                    video_active_ = bool(frame) || bool(showing.frame);
                }
                const bool video_changed = frame != showing.frame;
                if (video_changed && frame)
                    pending = {frame, framebuffer(*frame)};
                const int next_width = frame ? frame->width : source_width;
                const int next_height = frame ? frame->height : source_height;
                const bool need_blank = video_changed ? !pending.frame : !showing.frame;
                const uint32_t blank = need_blank ? blank_fb(next_width, next_height) : 0;
                uint32_t video_fb =
                    video_changed ? (pending.frame ? pending.fb : blank) : (showing.frame ? showing.fb : blank);
                Atomic a;
                a.plane(primary_, crtc_.id, video_fb, next_width, next_height, width_, height_);
                a.plane(overlay_, crtc_.id, next_ui < 0 ? 0 : ui_[next_ui].fb, width_, height_, width_, height_);
                if (frame && ltrace::on()) ltrace::rec(ltrace::kRender, clock_us(), uint32_t(frame->pts), video_fb);
                // A source-size change may modeset: older firmware needs one
                // at 720p; fpvOS patch 0034 instead applies it at frame start,
                // HDMI staying up. Flushes use matching I420 black buffers, and
                // release the old decoder lease only after the blank flip completes.
                const bool geometry_changed = next_width != source_width || next_height != source_height;
                // Blocking: the commit's register writes run here, in this
                // real-time thread, not on a kernel worker that a busy box
                // ran past the vblank now and then (the next refresh then
                // repeated the old picture). This waits for the flip, as
                // wait_flip did anyway.
                const uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT |
                    (geometry_changed ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0);
                a.commit(fd_, flags, ++cookie);
                wait_flip(cookie, video_fb);
                clock_lock_tick(last_vblank_us_);
                source_width = next_width;
                source_height = next_height;
                if (video_changed) {
                    showing = std::move(pending);
                    pending = {};
                    ++video_updates_;
                }
                if (next_ui != active_ui)
                    ++ui_updates_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (active_ui >= 0 && active_ui != next_ui)
                        slots_[active_ui] = Slot::Free;
                    active_ui = next_ui;
                    if (active_ui >= 0)
                        slots_[active_ui] = Slot::Active;
                    video_active_ = bool(showing.frame);
                    if (video_changed && frame) {
                        const uint64_t stamp = clock_us();
                        if (!stats_start_) stats_start_ = stamp;
                        ++stats_frames_;
                        if (frame->recv_ts && stamp >= frame->recv_ts && frame->tx_processing_delay_us != kAirUnknown)
                            latency_samples_.push_back(float(stamp - frame->recv_ts + uint64_t(frame->tx_encode_delay_us) + frame->tx_processing_delay_us) / 1000);
                        video_stats_.stamp_us = stamp;video_stats_.pts_us=frame->pts;
                        video_stats_.rejected_us = 0;
                        video_stats_.width = frame->width; video_stats_.height = frame->height;
                        if (stamp - stats_start_ >= 1000000) {
                            video_stats_.fps = int(std::lround(stats_frames_ * 1000000.0 / (stamp - stats_start_)));
                            video_stats_.latency_ms = -1;
                            if (!latency_samples_.empty()) {
                                auto middle = latency_samples_.begin() + latency_samples_.size() / 2;
                                std::nth_element(latency_samples_.begin(), middle, latency_samples_.end());
                                video_stats_.latency_ms = *middle;
                            }
                            latency_samples_.clear(); stats_frames_ = 0; stats_start_ = stamp;
                        }
                    } else if (video_changed) {
                        const auto rejected = video_stats_;
                        video_stats_ = {}; stats_start_ = stats_frames_ = 0; latency_samples_.clear();
                        video_stats_.rejected_us = rejected.rejected_us;
                        video_stats_.rejected_width = rejected.rejected_width;
                        video_stats_.rejected_height = rejected.rejected_height;
                    }
                }
                ++presented_;
                wake_.notify_all();
            }
        } catch (const std::exception &e) {
            failed_ = true;
            fprintf(stderr, "display: %s\n", e.what());
        }
        // Keep active and uncertain pending frame leases until scanout is stopped.
        try {
            disable();
        } catch (const std::exception &e) {
            failed_ = true;
            fprintf(stderr, "display stop: %s\n", e.what());
        }
        pending = {};
        showing = {};
        // Scanout is stopped (or the device is gone): no fb is on the screen.
        drop_all_fbs();
        // UI buffers/mappings stay alive until the UI caller has stopped too.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
            video_active_ = false;
            latest_.reset();
            held_.clear();
        }
        wake_.notify_all();
        printf("display: video updates=%llu UI updates=%llu hardware composition\n",
               (unsigned long long)video_updates_, (unsigned long long)ui_updates_.load());
    }
};
} // namespace
std::shared_ptr<PlaneScreen> make_ar9311_screen(const std::string &path, bool force, std::atomic<bool> *stop, int width, int height, int hz) {
    try {
        return std::make_shared<Ar9311Screen>(path, force, stop, width, height, hz);
    } catch (const std::exception& e) {
        throw ScreenError(e.what());
    }
}
} // namespace display
