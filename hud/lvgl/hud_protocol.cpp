#include "hud_protocol.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace kestrel_ui::hud_ipc {
namespace {
constexpr uint32_t magic = 0x4455484b; // KHUD
struct Writer {
    std::vector<uint8_t> out;
    void u(uint64_t value, size_t n) { for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(value >> (8 * i))); }
    void flag(bool value) { u(value, 1); }
    void fraction(float value) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::runtime_error("Invalid HUD fraction");
        uint32_t bits; std::memcpy(&bits, &value, 4); u(bits, 4);
    }
    void text(const std::string& value) {
        if (value.size() > 128 || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid HUD text");
        u(value.size(), 2); out.insert(out.end(), value.begin(), value.end());
    }
};
struct Reader {
    const std::vector<uint8_t>& in;
    size_t at = 0;
    uint64_t u(size_t n) {
        if (at + n > in.size()) throw std::runtime_error("Truncated HUD packet");
        uint64_t value = 0; for (size_t i = 0; i < n; ++i) value |= uint64_t(in[at++]) << (8 * i); return value;
    }
    bool flag() { const auto v = u(1); if (v > 1) throw std::runtime_error("Invalid HUD flag"); return v; }
    float fraction() {
        const uint32_t bits = uint32_t(u(4)); float v; std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v) || v < 0 || v > 1) throw std::runtime_error("Invalid HUD fraction");
        return v;
    }
    std::string text() {
        const size_t n = u(2);
        if (n > 128 || at + n > in.size()) throw std::runtime_error("Invalid HUD text length");
        std::string value(in.begin() + at, in.begin() + at + n); at += n;
        if (value.find('\0') != std::string::npos) throw std::runtime_error("Invalid HUD text");
        return value;
    }
    void end() { if (at != in.size()) throw std::runtime_error("Trailing HUD fields"); }
};
}
Packet::~Packet() { if (fd >= 0) close(fd); }
size_t Layout::bytes() const { size_t n = 0; for (const auto& r : regions) n += r.bytes(); return n; }
size_t Layout::offset(size_t region) const { size_t n = 0; for (size_t i = 0; i < region; ++i) n += regions.at(i).bytes(); return n; }
void Layout::validate() const {
    if (width < 640 || width > 4096 || height < 360 || height > 2160 || regions.empty() || regions.size() > max_regions)
        throw std::runtime_error("Invalid HUD viewport/layout");
    for (const auto& r : regions) {
        if (!r.width || !r.height || r.x >= width || r.y >= height || uint64_t(r.x) + r.width > width ||
            uint64_t(r.y) + r.height > height || uint32_t(r.role) > uint32_t(Role::Status))
            throw std::runtime_error("Invalid HUD region");
    }
    if (bytes() > size_t(width) * height * 4) throw std::runtime_error("HUD surfaces exceed pixel budget");
}
std::vector<uint8_t> encode(const Layout& layout) {
    layout.validate(); Writer w; w.u(layout.width, 4); w.u(layout.height, 4);
    w.u(1, 4); // ARGB8888, straight alpha, tightly packed width*4-byte stride.
    w.u(2, 4); // Two explicitly leased slots.
    w.u(layout.regions.size(), 4);
    for (const auto& r : layout.regions) { w.u(r.x, 4); w.u(r.y, 4); w.u(r.width, 4); w.u(r.height, 4); w.u(uint32_t(r.role), 4); }
    return w.out;
}
Layout decode_layout(const std::vector<uint8_t>& bytes) {
    Reader r{bytes}; Layout layout; layout.width = uint32_t(r.u(4)); layout.height = uint32_t(r.u(4));
    if (r.u(4) != 1 || r.u(4) != 2) throw std::runtime_error("Unsupported HUD pixel format/slots");
    const auto count = r.u(4); if (!count || count > max_regions) throw std::runtime_error("Invalid HUD region count");
    for (size_t i = 0; i < count; ++i) {
        Region region; region.x = uint32_t(r.u(4)); region.y = uint32_t(r.u(4));
        region.width = uint32_t(r.u(4)); region.height = uint32_t(r.u(4)); region.role = Role(r.u(4)); layout.regions.push_back(region);
    }
    r.end(); layout.validate(); return layout;
}
std::vector<uint8_t> encode(const HudData& data) {
    if(data.text_scale<5 || data.text_scale>20) throw std::runtime_error("Invalid HUD text scale");
    if (data.theme < 0 || data.theme > 255) throw std::runtime_error("Invalid HUD theme");
    Writer w; w.flag(data.enabled); w.flag(data.details); w.flag(data.show_status); w.u(data.theme, 4); w.text(data.status);
    for (const auto& p : data.panels) {
        w.flag(p.visible); w.flag(p.picked); w.flag(p.fresh); w.flag(p.armed); w.flag(p.detail1_quiet);
        for (float v : {p.shade, p.rule, p.block, p.dim, p.rail_dim, p.upper_level, p.lower_level, p.state_opacity}) w.fraction(v);
        for (const auto* s : {&p.primary, &p.primary_unit, &p.secondary, &p.secondary_unit, &p.state, &p.upper_label, &p.lower_label, &p.detail1, &p.detail2}) w.text(*s);
    }
    if (data.metrics.size() > 24) throw std::runtime_error("Too many HUD metrics");
    w.flag(data.simulated); w.u(data.metrics.size(), 2);
    uint32_t seen = 0;
    for (const auto& item : data.metrics) {
        const auto id = uint16_t(item.id);
        if (!id || id > uint16_t(HudMetricId::UiCadence) || (seen & (1u << id)) || item.scale.size() > 6)
            throw std::runtime_error("Invalid/duplicate HUD metric");
        seen |= 1u << id; w.u(id, 2); w.flag(item.valid); w.flag(item.metered); w.fraction(item.level);
        w.text(item.label); w.text(item.value); w.text(item.unit); w.u(item.scale.size(), 1);
        for (const auto& label : item.scale) w.text(label);
    }
    w.u(data.text_scale,1);
    return w.out;
}
HudData decode_data(const std::vector<uint8_t>& bytes) {
    Reader r{bytes}; HudData data; data.enabled = r.flag(); data.details = r.flag(); data.show_status = r.flag();
    data.theme = int(r.u(4)); if (data.theme < 0 || data.theme > 255) throw std::runtime_error("Invalid HUD theme");
    data.status = r.text();
    for (auto& p : data.panels) {
        p.visible = r.flag(); p.picked = r.flag(); p.fresh = r.flag(); p.armed = r.flag(); p.detail1_quiet = r.flag();
        for (auto* v : {&p.shade, &p.rule, &p.block, &p.dim, &p.rail_dim, &p.upper_level, &p.lower_level, &p.state_opacity}) *v = r.fraction();
        for (auto* s : {&p.primary, &p.primary_unit, &p.secondary, &p.secondary_unit, &p.state, &p.upper_label, &p.lower_label, &p.detail1, &p.detail2}) *s = r.text();
    }
    data.simulated = r.flag(); const size_t count = r.u(2);
    if (count > 24) throw std::runtime_error("Too many HUD metrics");
    uint32_t seen = 0;
    for (size_t i = 0; i < count; ++i) {
        HudMetric item; const auto id = r.u(2);
        if (!id || id > uint16_t(HudMetricId::UiCadence) || (seen & (1u << id))) throw std::runtime_error("Invalid/duplicate HUD metric");
        seen |= 1u << id; item.id = HudMetricId(id); item.valid = r.flag(); item.metered = r.flag(); item.level = r.fraction();
        item.label = r.text(); item.value = r.text(); item.unit = r.text(); const size_t labels = r.u(1);
        if (labels > 6) throw std::runtime_error("Too many HUD scale labels");
        for (size_t j = 0; j < labels; ++j) item.scale.push_back(r.text());
        data.metrics.push_back(std::move(item));
    }
    data.text_scale=int(r.u(1));
    if(data.text_scale<5 || data.text_scale>20) throw std::runtime_error("Invalid HUD text scale");
    r.end(); return data;
}
std::vector<uint8_t> encode(const Frame& frame) {
    if (frame.slot > 1) throw std::runtime_error("Invalid HUD frame slot");
    Writer w; w.u(frame.slot, 4); w.u(frame.render_us, 8); w.u(frame.cache_bytes, 8); w.u(frame.rasters, 8); w.u(frame.changed_regions, 4); return w.out;
}
Frame decode_frame(const std::vector<uint8_t>& bytes) {
    Reader r{bytes}; Frame f; f.slot = uint32_t(r.u(4)); f.render_us = r.u(8); f.cache_bytes = r.u(8); f.rasters = r.u(8); f.changed_regions = uint32_t(r.u(4));
    r.end(); if (f.slot > 1) throw std::runtime_error("Invalid HUD frame slot"); return f;
}
bool send(int socket, Type type, uint64_t sequence, const std::vector<uint8_t>& payload, int fd) {
    Writer w; w.u(magic, 4); w.u(4, 2); w.u(uint16_t(type), 2); w.u(sequence, 8);
    w.out.insert(w.out.end(), payload.begin(), payload.end());
    if (w.out.size() > max_packet) throw std::runtime_error("Oversized HUD packet");
    iovec io{w.out.data(), w.out.size()}; msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    if (fd >= 0) {
        msg.msg_control = control; msg.msg_controllen = sizeof(control);
        auto* c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    }
    const ssize_t n = sendmsg(socket, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
    if (n != ssize_t(w.out.size())) throw std::runtime_error("HUD IPC send failed");
    return true;
}
bool receive(int socket, Packet& packet) {
    uint8_t buffer[max_packet]; iovec io{buffer, sizeof(buffer)}; msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)]{}; msg.msg_control = control; msg.msg_controllen = sizeof(control);
    const ssize_t n = recvmsg(socket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
    if (n <= 0) throw std::runtime_error("HUD IPC disconnected");
    std::vector<int> fds;
    for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
            const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const auto* values = reinterpret_cast<const int*>(CMSG_DATA(c));
            fds.insert(fds.end(), values, values + count);
        }
    }
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || fds.size() > 1) {
        for (int fd : fds) close(fd);
        throw std::runtime_error("Truncated/extra-fd HUD packet");
    }
    if (!fds.empty()) packet.fd = fds[0];
    packet.payload.assign(buffer, buffer + n); Reader r{packet.payload};
    if (r.u(4) != magic || r.u(2) != 4) throw std::runtime_error("Unsupported HUD protocol");
    const auto type = r.u(2); if (type < 1 || type > 5) throw std::runtime_error("Invalid HUD message type");
    packet.type = Type(type); packet.sequence = r.u(8);
    packet.payload.erase(packet.payload.begin(), packet.payload.begin() + r.at); return true;
}
} // namespace kestrel_ui::hud_ipc
