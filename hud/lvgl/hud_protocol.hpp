#pragma once
#include "hud_data.hpp"
#include <vector>
#include <cstddef>

namespace kestrel_ui::hud_ipc {
constexpr size_t max_packet = 8192, max_regions = 8;
enum class Type : uint16_t { Hello = 1, Setup = 2, Data = 3, Frame = 4, Release = 5 };
enum class Role : uint32_t { Hud = 0, LeftWing = 1, RightWing = 2, Status = 3 };
struct Region {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    Role role = Role::Hud;
    size_t bytes() const { return size_t(width) * height * 4; }
    bool operator==(const Region& b) const { return x == b.x && y == b.y && width == b.width && height == b.height && role == b.role; }
};
struct Layout {
    uint32_t width = 0, height = 0;
    std::vector<Region> regions;
    size_t bytes() const;
    size_t offset(size_t region) const;
    void validate() const;
    bool operator==(const Layout& b) const { return width == b.width && height == b.height && regions == b.regions; }
};
struct Packet {
    Type type = Type::Hello;
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
    int fd = -1;
    Packet() = default;
    ~Packet();
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
};
struct Frame {
    uint32_t slot = 0;
    uint64_t render_us = 0, cache_bytes = 0, rasters = 0;
    uint32_t changed_regions = 0;
};
std::vector<uint8_t> encode(const Layout& layout);
Layout decode_layout(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encode(const HudData& data);
HudData decode_data(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encode(const Frame& frame);
Frame decode_frame(const std::vector<uint8_t>& bytes);
// false means EAGAIN; EOF and malformed packets throw. Received fds are owned
// by Packet until explicitly taken. All header/field encodings are little-endian.
bool send(int socket, Type type, uint64_t sequence, const std::vector<uint8_t>& payload, int fd = -1);
bool receive(int socket, Packet& packet);
} // namespace kestrel_ui::hud_ipc
