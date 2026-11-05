#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hal {
// Prepared straight-alpha ARGB overlay in BT.709 limited-range I420. Chroma
// accumulates each 2x2 block with alpha weighting, including transparent pixels.
// Only nonempty blocks are stored; unchanged overlays need no RGB conversion.
class I420Overlay {
    struct Block {
        uint32_t x, y;
        uint8_t a[4], luma[4], alpha, u, v;
    };
    unsigned width_, height_;
    std::vector<Block> blocks_;
    static uint8_t clip(int x) { return std::clamp(x, 0, 255); }
    static uint8_t mix(unsigned src, unsigned dst, unsigned a) {
        return (src * a + dst * (255 - a) + 127) / 255;
    }

  public:
    I420Overlay(unsigned w, unsigned h, const uint32_t *argb) : width_(w), height_(h) {
        if (!w || !h || w > 4096 || h > 2160 || ((w | h) & 1) || !argb)
            throw std::invalid_argument("overlay geometry");
        for (unsigned y = 0; y < h; y += 2)
            for (unsigned x = 0; x < w; x += 2) {
                Block b{};
                b.x = x;
                b.y = y;
                unsigned sum = 0, usum = 0, vsum = 0;
                for (unsigned k = 0; k < 4; ++k) {
                    uint32_t p = argb[(y + k / 2) * w + x + k % 2];
                    int r = (p >> 16) & 255, g = (p >> 8) & 255, blue = p & 255;
                    b.a[k] = p >> 24;
                    b.luma[k] = clip(16 + ((47 * r + 157 * g + 16 * blue + 128) >> 8));
                    auto u = clip(128 + ((-26 * r - 87 * g + 112 * blue + 128) >> 8));
                    auto v = clip(128 + ((112 * r - 102 * g - 10 * blue + 128) >> 8));
                    sum += b.a[k];
                    usum += u * b.a[k];
                    vsum += v * b.a[k];
                }
                if (sum) {
                    b.alpha = (sum + 2) / 4;
                    b.u = (usum + sum / 2) / sum;
                    b.v = (vsum + sum / 2) / sum;
                    blocks_.push_back(b);
                }
            }
    }
    void blend(uint8_t *y, uint8_t *u, uint8_t *v, unsigned stride, unsigned height) const {
        if (stride < width_ || height < height_ || (stride & 1))
            throw std::invalid_argument("overlay destination");
        for (const auto &b : blocks_) {
            for (unsigned k = 0; k < 4; ++k) {
                auto &d = y[(b.y + k / 2) * stride + b.x + k % 2];
                d = mix(b.luma[k], d, b.a[k]);
            }
            const auto at = (b.y / 2) * (stride / 2) + b.x / 2;
            u[at] = mix(b.u, u[at], b.alpha);
            v[at] = mix(b.v, v[at], b.alpha);
        }
    }
};
} // namespace hal
