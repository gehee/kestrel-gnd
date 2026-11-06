#pragma once
#include <cstdint>

namespace bsp::ascent_panel {
// Gnd485 VRX wiring. These are board facts, not Linux device enumeration.
enum class Key { None, Up, Down, Left, Right, Ok, Back, Rec, Link, Upgrade };
constexpr const char *adc_device = "/dev/i2c-3";
constexpr uint16_t adc_address = 0x54;
constexpr uint8_t adc_register = 0;
constexpr const char *gpio_label = "ar-gpio0";
constexpr unsigned link_offset = 2; // active low
constexpr const char *pwm_controller = "1000000.pwm";
constexpr unsigned pwm_channel = 0;
constexpr unsigned period_ns = 1000000, duty_ns = 200000;
struct Level {
    Key key;
    int mv;
};
constexpr Level levels[] = {{Key::Rec, 400}, {Key::Up, 800},    {Key::Left, 1000}, {Key::Right, 1200},
                            {Key::Ok, 1400}, {Key::Down, 1600}, {Key::Back, 1800}};
inline Key decode(uint16_t word) {
    const int mv = ((word >> 4) & 255) * 3300 / 256;
    for (const auto &level : levels)
        if (mv >= level.mv - 89 && mv <= level.mv + 89)
            return level.key;
    return Key::None;
}
inline bool direction(Key key) {
    return key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right;
}
inline const char *name(Key key) {
    switch (key) {
    case Key::Up:
        return "up";
    case Key::Down:
        return "down";
    case Key::Left:
        return "left";
    case Key::Right:
        return "right";
    case Key::Ok:
        return "enter";
    case Key::Back:
        return "back";
    case Key::Rec:
        return "rec";
    case Key::Link:
        return "link";
    case Key::Upgrade:
        return "upgrade";
    default:
        return "none";
    }
}
} // namespace bsp::ascent_panel
