#pragma once
#include <array>
#include <stdexcept>
#include <string>

namespace display {
// HDMI output mode, independent of camera and UI rendering rates. Assets are
// selected by resolution, so every refresh rate shares the same baked images.
struct AscentMode {
    int height = 1080, hz = 60;
    constexpr AscentMode(int height = 1080, int hz = 60) : height(height), hz(hz) {}
    constexpr int width() const { return height == 720 ? 1280 : 1920; }
    constexpr bool valid() const {
        return (height == 720 && (hz == 50 || hz == 60 || hz == 100 || hz == 120)) ||
               (height == 1080 && (hz == 50 || hz == 60));
    }
    constexpr int id() const { return height * 1000 + hz; }
    static constexpr AscentMode from_id(int id) { return {id / 1000, id % 1000}; }
    constexpr bool operator==(AscentMode other) const { return height == other.height && hz == other.hz; }
    constexpr bool operator!=(AscentMode other) const { return !(*this == other); }
    std::string name() const { return std::to_string(width()) + "x" + std::to_string(height) + "@" + std::to_string(hz); }
    std::string label() const { return std::to_string(width()) + "x" + std::to_string(height) + " @ " + std::to_string(hz) + " Hz"; }
};
inline constexpr std::array<AscentMode, 6> ascent_modes{{
    {720, 50}, {720, 60}, {720, 100}, {720, 120}, {1080, 50}, {1080, 60}
}};
inline AscentMode parse_ascent_mode(const std::string& text) {
    for (const auto mode : ascent_modes) if (mode.name() == text) return mode;
    throw std::invalid_argument("Supported screen modes: 1280x720@50/60/100/120, 1920x1080@50/60");
}
} // namespace display
