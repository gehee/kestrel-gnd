#include "bsp/bsp.hpp"
namespace bsp {
// Native fpvOS owns pinmux and power sequencing. No validated Linux input,
// voltage or thermal provider is currently exported by this board profile.
static const Board kAscent = {
    "ascent", {{"", 0, 1}, 0, 0, nullptr, 0}, {{"", 0, 1}, 0}, {{"", 0, 1}, 1, 0, 1}, "", {""}};
const Board &ascent() {
    return kAscent;
}
} // namespace bsp
