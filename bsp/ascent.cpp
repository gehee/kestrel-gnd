#include "bsp/bsp.hpp"
namespace bsp {
// Native fpvOS owns pinmux and power sequencing. Panel I2C/GPIO/PWM facts
// live in ascent_panel.hpp; these legacy IIO fields are intentionally empty.
static const Board kAscent = {
    "ascent", {{"", 0, 1}, 0, 0, nullptr, 0}, {{"", 0, 1}, 0}, {{"", 0, 1}, 1, 0, 1}, "", {""}};
const Board &ascent() {
    return kAscent;
}
} // namespace bsp
