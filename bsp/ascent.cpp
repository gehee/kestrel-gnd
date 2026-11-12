#include "bsp/bsp.hpp"
namespace bsp {
// Native fpvOS owns pinmux and power sequencing. Panel I2C/GPIO/PWM facts
// live in ascent_panel.hpp; these legacy IIO fields are intentionally empty.
// The stock Lite link repeatedly drops at 40 MHz; 20 MHz is the validated cap.
static const Board kAscent = {
    "ascent", {{"", 0, 1}, 0, 0, nullptr, 0}, {{"", 0, 1}, 0}, {{"", 0, 1}, 1, 0, 1}, "", {"", 20}};
const Board &ascent() {
    return kAscent;
}
} // namespace bsp
