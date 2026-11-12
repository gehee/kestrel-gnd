#pragma once
#include <cstdint>
#include <linux/input-event-codes.h>

namespace bsp::ascent_panel {
// The AR9311 receivers' front panels. Each board has its own wiring and its
// own key map, in its own block below; the device tree names the board, and
// one without a block here gets no panel. These are board facts, not Linux
// device enumeration.
// OkBack is a raw key only, never an event: a tap is OK, a hold is Back. Only
// a board whose map names it has that behaviour (the SteadyDigital's back key).
enum class Key { None, Up, Down, Left, Right, Ok, Back, Rec, Link, Upgrade, OkBack };

// Caddx Ascent Gnd485 (the VRX): seven keys on one ADC ladder, each its own
// key - OK and Back are separate and act on the press, no tap/hold - LINK on a
// GPIO line, and a PWM beeper.
namespace gnd485 {
constexpr const char *compatible = "caddx,ascent-gnd485";
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
constexpr bool reports(Key key) {
    for (const auto &level : levels)
        if (level.key == key)
            return true;
    return false;
}
// The tap/hold key is not this board's: its OK and Back stay two keys.
static_assert(!reports(Key::OkBack) && reports(Key::Ok) && reports(Key::Back),
              "the Gnd485 has its own OK and Back keys, no tap/hold key");
inline Key decode(uint16_t word) {
    const int mv = ((word >> 4) & 255) * 3300 / 256;
    for (const auto &level : levels)
        if (mv >= level.mv - 89 && mv <= level.mv + 89)
            return level.key;
    return Key::None;
}
} // namespace gnd485

// Skyzone SteadyDigital: seven GPIO keys as one polled evdev device (fpvOS DT
// label). PAIR is this board's LINK. No beeper. The d-pad centre is hard to
// press without tilting the pad, so it is REC (occasional, and a hard press
// keeps it from starting by accident); the back key is OK on a tap and Back
// once held (with the menu closed, Back opens the gallery).
namespace steadydigital {
constexpr const char *compatible = "skyzone,steadydigital";
constexpr const char *keys_device = "steadydigital-keys";
struct Button {
    unsigned code; // evdev key code
    Key key;
};
// One key at a time: the first held in this order wins. PAIR comes first: a
// press that also pulls another line is still LINK; then the d-pad centre
// (REC), which a press tilts onto a direction.
constexpr Button buttons[] = {{KEY_WPS_BUTTON, Key::Link}, {KEY_ENTER, Key::Rec}, {KEY_UP, Key::Up},
                              {KEY_DOWN, Key::Down},       {KEY_LEFT, Key::Left}, {KEY_RIGHT, Key::Right},
                              {KEY_ESC, Key::OkBack}};
} // namespace steadydigital

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
