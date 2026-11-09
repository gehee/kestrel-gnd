#include "hud_source.hpp"
#include "hud_view.hpp"
#include "clock.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kestrel_ui {
namespace {
void drop_images(std::vector<lv_image_dsc_t>& images) {
    for (auto& image : images) { lv_image_cache_drop(&image); lv_image_header_cache_drop(&image); }
}
void images_for(std::vector<lv_image_dsc_t>& images, const HudBuffer& buffer, uint32_t slot) {
    const auto& layout = buffer.layout(); images.resize(layout.regions.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& r = layout.regions[i]; auto& image = images[i]; image = {};
        image.header.magic = LV_IMAGE_HEADER_MAGIC; image.header.cf = LV_COLOR_FORMAT_ARGB8888;
        image.header.w = r.width; image.header.h = r.height; image.header.stride = r.width * 4;
        image.data_size = r.bytes(); image.data = buffer.region(slot, i);
    }
}
}
struct InlineHud::Impl {
    std::unique_ptr<HudRenderer> view;
    HudBuffer buffer;
    std::vector<lv_image_dsc_t> images;
    std::vector<uint8_t> previous;
    uint64_t revision = 0;
    uint32_t slot = 0;
    uint32_t changed = 0;
    Impl(int w, int h, const std::string& assets, HudStyle style) : view(make_hud_renderer(style, w, h, assets)), buffer(view->layout()) {}
    ~Impl() { drop_images(images); }
};
InlineHud::InlineHud(int w, int h, const std::string& assets, HudStyle style) : impl_(std::make_unique<Impl>(w, h, assets, style)) {}
InlineHud::~InlineHud() = default;
void InlineHud::update(const HudData& data, uint64_t) {
    impl_->changed = 0;
    auto wire = hud_ipc::encode(data); if (wire == impl_->previous) return;
    const uint32_t old_slot = impl_->slot;
    drop_images(impl_->images); impl_->slot = 1 - impl_->slot;
    impl_->view->render(data, impl_->buffer, impl_->slot); images_for(impl_->images, impl_->buffer, impl_->slot);
    for (size_t i = 0; i < impl_->view->layout().regions.size(); ++i)
        if (impl_->previous.empty() || std::memcmp(impl_->buffer.region(old_slot, i), impl_->buffer.region(impl_->slot, i), impl_->view->layout().regions[i].bytes()))
            impl_->changed |= 1u << i;
    impl_->previous = std::move(wire); ++impl_->revision;
}
const hud_ipc::Layout& InlineHud::layout() const { return impl_->view->layout(); }
const lv_image_dsc_t* InlineHud::image(size_t i) const { return i < impl_->images.size() ? &impl_->images[i] : nullptr; }
uint64_t InlineHud::revision() const { return impl_->revision; }
uint32_t InlineHud::changed_regions() const { return impl_->changed; }
size_t InlineHud::cache_bytes() const { return impl_->view->cache_bytes(); }
uint64_t InlineHud::rasters() const { return impl_->view->rasters(); }

struct HudClient::Impl {
    int width, height, socket = -1;
    pid_t child = -1;
    std::string assets, executable;
    hud_ipc::Layout layout;
    std::unique_ptr<HudBuffer> buffer;
    std::vector<lv_image_dsc_t> images;
    std::vector<uint8_t> last_sent;
    std::vector<std::pair<uint32_t, uint64_t>> releases;
    uint64_t revision = 0, sequence = 0, sent = 0, accepted = 0, progress_at = 0, retry_at = 0;
    uint64_t restarts = 0, frames = 0;
    int front = -1;
    bool setup = false;
    uint32_t changed = 0;
    hud_ipc::Frame stats;
    Impl(int w, int h, std::string a, std::string e) : width(w), height(h), assets(std::move(a)), executable(std::move(e)) { start(); }
    ~Impl() { stop(); }
    void stop() {
        drop_images(images); images.clear(); buffer.reset(); layout = {}; releases.clear();
        front = -1; setup = false; last_sent.clear(); sent = accepted = 0;
        if (socket >= 0) { close(socket); socket = -1; }
        if (child > 0) {
            kill(child, SIGTERM);
            bool reaped = false;
            for (int i = 0; i < 100; ++i) {
                const pid_t r = waitpid(child, nullptr, WNOHANG);
                if (r == child || (r < 0 && errno == ECHILD)) { reaped = true; break; }
                usleep(2000);
            }
            if (!reaped) { kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {} }
            child = -1;
        }
        ++revision;
    }
    void start() {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair)) throw std::runtime_error("Cannot create HUD IPC pair");
        const std::string w = std::to_string(width), h = std::to_string(height);
        const pid_t owner = getpid();
        child = fork();
        if (child < 0) { close(pair[0]); close(pair[1]); throw std::runtime_error("Cannot fork HUD"); }
        if (child == 0) {
            close(pair[0]);
            if (pair[1] != 3) { if (dup2(pair[1], 3) < 0) _exit(126); close(pair[1]); }
            if (fcntl(3, F_SETFD, 0) < 0 || prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != owner) _exit(126);
            const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(126);
            if (input != STDIN_FILENO) close(input);
            execl(executable.c_str(), executable.c_str(), "--ipc-fd", "3", "--width", w.c_str(), "--height", h.c_str(), "--assets", assets.c_str(), static_cast<char*>(nullptr));
            std::perror("Cannot exec kestrel-hud"); _exit(127);
        }
        close(pair[1]); socket = pair[0]; progress_at = monotonic_us();
        std::printf("HUD_PROCESS pid=%d executable=%s restarts=%llu\n", int(child), executable.c_str(), (unsigned long long)restarts);
    }
    void packet(hud_ipc::Packet& p, uint64_t now) {
        using namespace hud_ipc;
        if (p.fd >= 0) throw std::runtime_error("Unexpected fd from HUD renderer");
        if (p.type == Type::Hello) {
            if (buffer || p.sequence != 0) throw std::runtime_error("Duplicate HUD setup");
            auto next = decode_layout(p.payload);
            if (next.width != uint32_t(width) || next.height != uint32_t(height)) throw std::runtime_error("HUD viewport mismatch");
            buffer = std::make_unique<HudBuffer>(next); layout = next; ++revision;
        } else if (p.type == Type::Frame && setup) {
            const auto f = decode_frame(p.payload);
            if (!p.sequence || p.sequence > sent || p.sequence <= accepted || int(f.slot) == front)
                throw std::runtime_error("Invalid HUD frame sequence/lease");
            if (f.changed_regions >> layout.regions.size()) throw std::runtime_error("Invalid HUD damage mask");
            // update() runs only between completed synchronous UI refreshes.
            // The old front buffer cannot be released while draw tasks use it.
            drop_images(images);
            if (front >= 0) releases.emplace_back(uint32_t(front), accepted);
            front = int(f.slot); accepted = p.sequence; stats = f;
            images_for(images, *buffer, f.slot); ++revision; ++frames;
            changed |= f.changed_regions;
        } else throw std::runtime_error("Unexpected HUD renderer message");
        progress_at = now;
    }
};
HudClient::HudClient(int w, int h, const std::string& assets, const std::string& executable)
    : impl_(std::make_unique<Impl>(w, h, assets, executable)) {}
HudClient::~HudClient() = default;
void HudClient::update(const HudData& data, uint64_t now) {
    auto& s = *impl_;
    s.changed = 0;
    if (s.socket < 0) {
        if (now < s.retry_at) return;
        try { s.start(); } catch (const std::exception& e) { std::fprintf(stderr, "HUD restart: %s\n", e.what()); s.retry_at = now + 500000; return; }
    }
    try {
        for (int i = 0; i < 8; ++i) { hud_ipc::Packet p; if (!hud_ipc::receive(s.socket, p)) break; s.packet(p, now); }
        if (s.buffer && !s.setup) {
            if (!hud_ipc::send(s.socket, hud_ipc::Type::Setup, 0, hud_ipc::encode(s.layout), s.buffer->fd())) return;
            s.setup = true;
        }
        for (size_t i = 0; i < s.releases.size();) {
            const auto [slot, seq] = s.releases[i]; hud_ipc::Frame f; f.slot = slot;
            if (!hud_ipc::send(s.socket, hud_ipc::Type::Release, seq, hud_ipc::encode(f))) break;
            s.releases.erase(s.releases.begin() + i);
        }
        if (s.setup) {
            auto wire = hud_ipc::encode(data);
            if (wire != s.last_sent) {
                const uint64_t seq = s.sequence + 1;
                if (hud_ipc::send(s.socket, hud_ipc::Type::Data, seq, wire)) {
                    if (s.sent == s.accepted) s.progress_at = now;
                    s.sequence = s.sent = seq; s.last_sent = std::move(wire);
                }
            }
        }
        if ((!s.setup || s.sent > s.accepted) && now >= s.progress_at && now - s.progress_at > 2000000)
            throw std::runtime_error("HUD renderer stalled");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "HUD disconnected: %s; renderer will restart\n", e.what());
        s.stop(); ++s.restarts; s.retry_at = now + 500000;
    }
}
const hud_ipc::Layout& HudClient::layout() const { return impl_->layout; }
const lv_image_dsc_t* HudClient::image(size_t i) const { return i < impl_->images.size() ? &impl_->images[i] : nullptr; }
uint64_t HudClient::revision() const { return impl_->revision; }
uint32_t HudClient::changed_regions() const { return impl_->changed; }
size_t HudClient::cache_bytes() const { return impl_->stats.cache_bytes; }
uint64_t HudClient::rasters() const { return impl_->stats.rasters; }
int HudClient::pid() const { return int(impl_->child); }
uint64_t HudClient::restarts() const { return impl_->restarts; }
uint64_t HudClient::frames() const { return impl_->frames; }
uint64_t HudClient::render_us() const { return impl_->stats.render_us; }
} // namespace kestrel_ui
