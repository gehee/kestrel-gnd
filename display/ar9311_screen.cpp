// Native AR9311: independent DMA-BUF video and ARGB UI planes, blended by VO.
#include "display/ar9311_screen.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <fcntl.h>
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
    void plane(const Properties &object, uint32_t crtc, uint32_t fb) {
        add(object, {{"FB_ID", fb},
                     {"CRTC_ID", fb ? crtc : 0},
                     {"SRC_X", 0},
                     {"SRC_Y", 0},
                     {"SRC_W", 1920u << 16},
                     {"SRC_H", 1080u << 16},
                     {"CRTC_X", 0},
                     {"CRTC_Y", 0},
                     {"CRTC_W", 1920},
                     {"CRTC_H", 1080}});
        if (object.keys.count("pixel blend mode")) {
            ++counts.back();
            properties.push_back(object.keys.at("pixel blend mode"));
            values.push_back(1); // DRM_MODE_BLEND_COVERAGE (straight alpha)
        }
    }
    void commit(int fd, uint32_t flags, uint64_t cookie = 0) {
        drm_mode_atomic a{};
        a.flags = flags;
        a.count_objs = objects.size();
        a.objs_ptr = uintptr_t(objects.data());
        a.count_props_ptr = uintptr_t(counts.data());
        a.props_ptr = uintptr_t(properties.data());
        a.prop_values_ptr = uintptr_t(values.data());
        a.user_data = cookie;
        ctl(fd, DRM_IOCTL_MODE_ATOMIC, &a, "atomic plane commit");
    }
};
class Ar9311Screen final : public PlaneScreen {
    struct Buffer {
        uint32_t handle = 0, fb = 0, pitch = 0;
        uint64_t size = 0;
        uint8_t *pixels = nullptr;
    };
    struct Imported {
        int fd;
        uint32_t handle = 0, fb = 0;
        std::shared_ptr<DecodedUnit> frame;
        Imported(int drm, std::shared_ptr<DecodedUnit> f) : fd(drm), frame(std::move(f)) {
            try {
                drm_prime_handle prime{};
                prime.fd = frame->prime_fd;
                ctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime, "import decoded DMA-BUF");
                handle = prime.handle;
                drm_mode_fb_cmd2 cmd{};
                cmd.width = frame->width;
                cmd.height = frame->height;
                cmd.pixel_format = DRM_FORMAT_YUV420;
                for (int i = 0; i < 3; ++i) {
                    cmd.handles[i] = handle;
                    cmd.pitches[i] = frame->pitches[i];
                    cmd.offsets[i] = frame->offsets[i];
                }
                ctl(fd, DRM_IOCTL_MODE_ADDFB2, &cmd, "decoded framebuffer");
                fb = cmd.fb_id;
            } catch (...) {
                release();
                throw;
            }
        }
        ~Imported() { release(); }
        void release() {
            if (fb)
                ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb);
            if (handle) {
                drm_gem_close close{};
                close.handle = handle;
                ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
            }
            fb = handle = 0;
        }
    };
    enum class Slot { Free, Writing, Ready, Pending, Active };
    int fd_ = -1;
    Buffer black_;
    std::array<Buffer, 3> ui_{};
    std::array<Slot, 3> slots_{};
    Properties connector_, crtc_, primary_, overlay_;
    uint32_t mode_ = 0;
    bool active_ = false;
    volatile bool *stop_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::shared_ptr<DecodedUnit> latest_;
    bool done_ = false, video_active_ = false;
    std::atomic<uint64_t> presented_{0};
    std::atomic<bool> failed_{false};
    uint64_t video_updates_ = 0, ui_updates_ = 0;

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
        a.plane(primary_, crtc_.id, 0);
        a.plane(overlay_, crtc_.id, 0);
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
        for (auto &b : ui_)
            destroy(b);
        destroy(black_);
        if (mode_) {
            drm_mode_destroy_blob b{};
            b.blob_id = mode_;
            ioctl(fd_, DRM_IOCTL_MODE_DESTROYPROPBLOB, &b);
        }
        close(fd_);
        fd_ = -1;
    }
    void create(Buffer &b, bool rgb) {
        drm_mode_create_dumb d{};
        d.width = 1920;
        d.height = rgb ? 1080 : 1620;
        d.bpp = rgb ? 32 : 8;
        ctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &d, "plane buffer allocation");
        b.handle = d.handle;
        b.pitch = d.pitch;
        b.size = d.size;
        if (d.pitch != unsigned(rgb ? 8192 : 1920) || d.size < uint64_t(d.pitch) * d.height)
            throw std::runtime_error("unsupported plane pitch/size");
        drm_mode_fb_cmd2 fb{};
        fb.width = 1920;
        fb.height = 1080;
        fb.pixel_format = rgb ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_YUV420;
        fb.handles[0] = d.handle;
        fb.pitches[0] = d.pitch;
        if (!rgb) {
            fb.handles[1] = fb.handles[2] = d.handle;
            fb.pitches[1] = fb.pitches[2] = 960;
            fb.offsets[1] = 1920 * 1080;
            fb.offsets[2] = 1920 * 1080 * 5 / 4;
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
            memset(b.pixels, 16, 1920 * 1080);
    }
    void wait_flip(uint64_t cookie) {
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
                reinterpret_cast<drm_event_vblank *>(e)->user_data == cookie)
                return;
            at += e->length;
        }
        throw std::runtime_error("missing matching HDMI flip completion");
    }

  public:
    Ar9311Screen(const std::string &device, bool force, volatile bool *stop) : stop_(stop) {
        try {
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
            drm_mode_get_connector c{};
            c.connector_id = connectors[0];
            ctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &c, "connector status");
            if (c.connection != 1 && !force)
                throw std::runtime_error("HDMI disconnected; use --force-display");
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
            drm_mode_modeinfo m{};
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
            drm_mode_create_blob blob{};
            blob.data = uintptr_t(&m);
            blob.length = sizeof(m);
            ctl(fd_, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob, "1080p60 mode");
            mode_ = blob.blob_id;
            create(black_, false);
            for (auto &b : ui_)
                create(b, true);
            printf("display: hardware planes video=%u UI=%u, DMA-BUF scanout\n", primary_.id, overlay_.id);
        } catch (...) {
            close_display();
            throw;
        }
    }
    ~Ar9311Screen() override { close_display(); }
    void queue_frame(std::shared_ptr<DecodedUnit> frame) override {
        if (!frame || frame->width != 1920 || frame->height != 1080 || !frame->has_prime_fd ||
            frame->prime_fd < 0 || frame->drm_pixel_format != DRM_FORMAT_YUV420) {
            failed_ = true;
            wake_.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = std::move(frame);
        }
        wake_.notify_all();
    }
    void request_flush() override {
        std::unique_lock<std::mutex> lock(mutex_);
        latest_.reset();
        wake_.notify_all();
        wake_.wait(lock, [this] { return done_ || !video_active_; });
    }
    void overlay(const uint32_t *argb, unsigned width, unsigned height) override {
        if (!argb || width != 1920 || height != 1080)
            throw std::invalid_argument("UI plane geometry");
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
        for (unsigned row = 0; row < height; ++row)
            memcpy(b.pixels + row * b.pitch, argb + row * width, width * 4);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &state : slots_)
                if (state == Slot::Ready)
                    state = Slot::Free;
            slots_[slot] = Slot::Ready;
        }
        wake_.notify_all();
    }
    uint64_t presented() const override { return presented_.load(); }
    bool failed() const override { return failed_.load(); }
    void run() override {
        std::unique_ptr<Imported> showing, pending;
        int active_ui = -1, next_ui = -1;
        uint64_t cookie = 0;
        try {
            Atomic initial;
            initial.plane(primary_, crtc_.id, black_.fb);
            initial.plane(overlay_, crtc_.id, 0);
            initial.add(connector_, {{"CRTC_ID", crtc_.id}});
            initial.add(crtc_, {{"MODE_ID", mode_}, {"ACTIVE", 1}});
            initial.commit(fd_, DRM_MODE_ATOMIC_ALLOW_MODESET);
            active_ = true;
            while (!*stop_ && !failed_) {
                std::shared_ptr<DecodedUnit> frame;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    frame = latest_;
                    if (frame && frame->dec_end_ts && clock_us() - frame->dec_end_ts > 2000000)
                        frame.reset();
                    next_ui = active_ui;
                    for (int i = 0; i < 3; ++i)
                        if (slots_[i] == Slot::Ready) {
                            next_ui = i;
                            slots_[i] = Slot::Pending;
                        }
                    if (frame == (showing ? showing->frame : nullptr) && next_ui == active_ui) {
                        wake_.wait_for(lock, std::chrono::milliseconds(2));
                        continue;
                    }
                    // Include the import/commit interval in flush ownership.
                    video_active_ = bool(frame) || bool(showing);
                }
                const bool video_changed = frame != (showing ? showing->frame : nullptr);
                if (video_changed && frame)
                    pending = std::make_unique<Imported>(fd_, frame);
                uint32_t video_fb =
                    video_changed ? (pending ? pending->fb : black_.fb) : (showing ? showing->fb : black_.fb);
                Atomic a;
                a.plane(primary_, crtc_.id, video_fb);
                a.plane(overlay_, crtc_.id, next_ui < 0 ? 0 : ui_[next_ui].fb);
                a.commit(fd_, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, ++cookie);
                wait_flip(cookie);
                if (video_changed) {
                    showing = std::move(pending);
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
                    video_active_ = bool(showing);
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
        pending.reset();
        showing.reset();
        // UI buffers/mappings stay alive until the UI caller has stopped too.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
            video_active_ = false;
            latest_.reset();
        }
        wake_.notify_all();
        printf("display: video updates=%llu UI updates=%llu hardware composition\n",
               (unsigned long long)video_updates_, (unsigned long long)ui_updates_);
    }
};
} // namespace
std::shared_ptr<PlaneScreen> make_ar9311_screen(const std::string &path, bool force, volatile bool *stop) {
    return std::make_shared<Ar9311Screen>(path, force, stop);
}
} // namespace display
