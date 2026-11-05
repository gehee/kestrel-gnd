#include "hal/i420_overlay.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
void check(bool ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}
int main() {
    // A padded destination must keep both its padding and transparent pixels.
    std::array<uint8_t, 8> y;
    y.fill(80);
    std::array<uint8_t, 2> u{120, 42}, v{140, 43};
    uint32_t transparent[4] = {0xffffff, 0xff, 0xff0000, 0};
    hal::I420Overlay(2, 2, transparent).blend(y.data(), u.data(), v.data(), 4, 2);
    for (auto pixel : y)
        check(pixel == 80, "transparent luma changed");
    check(u[0] == 120 && v[0] == 140, "transparent chroma changed");
    uint32_t white[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    hal::I420Overlay(2, 2, white).blend(y.data(), u.data(), v.data(), 4, 2);
    check(y[0] == 235 && y[1] == 235 && y[4] == 235 && y[5] == 235, "white must be limited-range luma");
    check(y[2] == 80 && y[3] == 80 && y[6] == 80 && y[7] == 80 && u[1] == 42 && v[1] == 43,
          "padding overwritten");
    check(u[0] >= 127 && u[0] <= 129 && v[0] == 128, "white must be neutral chroma");
    // One opaque red pixel covers one quarter of the chroma block. The other
    // three pixels have arbitrary RGB but zero alpha and cannot tint it.
    uint32_t red[4] = {0xffff0000, 0x00ffffff, 0x0000ff00, 0x000000ff};
    y.fill(16);
    u[0] = v[0] = 128;
    hal::I420Overlay(2, 2, red).blend(y.data(), u.data(), v.data(), 4, 2);
    check(y[0] >= 62 && y[0] <= 64 && y[1] == 16 && y[4] == 16, "red luma or alpha incorrect");
    check(u[0] >= 121 && u[0] <= 122 && v[0] == 156, "alpha-weighted chroma incorrect");
    bool rejected = false;
    try {
        hal::I420Overlay invalid(3, 2, white);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "odd overlay geometry accepted");
    puts("I420 overlay tests passed");
}
