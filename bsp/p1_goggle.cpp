#include "bsp/bsp.hpp"
namespace bsp {
// Native Linux owns the panel, backlight and ADC keypad (evdev).
// LED and RF wiring must not inherit Ascent VRX hardware paths.
static const Board kP1Goggle = {
    "p1goggle", {{"", 0, 1}, 0, 0, nullptr, 0}, {{"", 0, 1}, 0},
    {{"", 0, 1}, 1, 0, 1}, "", {""}};
const Board &p1_goggle() { return kP1Goggle; }
} // namespace bsp
