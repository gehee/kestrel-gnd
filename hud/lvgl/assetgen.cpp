#include "render/software/canopy.hpp"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Vertex { canopy::Point p; float alpha; };
float sample(const Vertex& a, const Vertex& b, const Vertex& c, float x, float y) {
    const float det = (b.p.y - c.p.y) * (a.p.x - c.p.x) + (c.p.x - b.p.x) * (a.p.y - c.p.y);
    if (std::abs(det) < 0.0001f) return 0;
    const float u = ((b.p.y - c.p.y) * (x - c.p.x) + (c.p.x - b.p.x) * (y - c.p.y)) / det;
    const float v = ((c.p.y - a.p.y) * (x - c.p.x) + (a.p.x - c.p.x) * (y - c.p.y)) / det;
    const float w = 1 - u - v;
    if (u < 0 || v < 0 || w < 0) return 0;
    return u * a.alpha + v * b.alpha + w * c.alpha;
}
void le32(std::ostream& out, uint32_t v) {
    const char p[] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
    out.write(p, 4);
}
void bake(int width, int height, bool left, const std::string& directory) {
    std::array<Vertex, 4> top, bottom;
    float min_x = float(width), min_y = float(height), max_x = 0, max_y = 0;
    for (int i = 0; i < 4; ++i) {
        top[i] = {canopy::project(width, height, left, canopy::kWedgeX[i], canopy::kWedgeTop[i]), canopy::kWedgeA[i]};
        bottom[i] = {canopy::project(width, height, left, canopy::kWedgeX[i], canopy::kWedgeBot[i]), canopy::kWedgeA[i]};
        for (auto v : {top[i], bottom[i]}) {
            min_x = std::min(min_x, v.p.x); max_x = std::max(max_x, v.p.x);
            min_y = std::min(min_y, v.p.y); max_y = std::max(max_y, v.p.y);
        }
    }
    const int x0 = std::max(0, int(std::floor(min_x)) - 1), y0 = std::max(0, int(std::floor(min_y)) - 1);
    const int w = std::min(width, int(std::ceil(max_x)) + 1) - x0;
    const int h = std::min(height, int(std::ceil(max_y)) + 1) - y0;
    std::vector<uint8_t> mask(size_t(w) * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float alpha = 0;
        for (float dy : {0.25f, 0.75f}) for (float dx : {0.25f, 0.75f}) {
            float a = 0;
            for (int i = 0; i < 3; ++i) {
                a = std::max(a, sample(top[i], bottom[i], top[i + 1], x0 + x + dx, y0 + y + dy));
                a = std::max(a, sample(bottom[i], bottom[i + 1], top[i + 1], x0 + x + dx, y0 + y + dy));
            }
            alpha += a / 4;
        }
        mask[size_t(y) * w + x] = uint8_t(std::clamp(alpha * 255, 0.0f, 255.0f));
    }
    const std::string path = directory + (left ? "/canopy-left.kui" : "/canopy-right.kui");
    std::ofstream out(path, std::ios::binary);
    out.write("KUIA", 4);
    for (uint32_t v : {uint32_t(width), uint32_t(height), uint32_t(x0), uint32_t(y0), uint32_t(w), uint32_t(h)}) le32(out, v);
    out.write(reinterpret_cast<const char*>(mask.data()), mask.size());
    if (!out) throw std::runtime_error("Asset write failed: " + path);
    std::printf("%s: %dx%d A8 crop at %d,%d (%zu bytes)\n", path.c_str(), w, h, x0, y0, mask.size());
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: kestrel-ui-assetgen WIDTH HEIGHT OUTPUT_DIRECTORY");
        const int w = std::stoi(argv[1]), h = std::stoi(argv[2]);
        if (w < 640 || w > 4096 || h < 360 || h > 2160) throw std::runtime_error("Unsupported asset resolution");
        std::filesystem::create_directories(argv[3]);
        bake(w, h, true, argv[3]); bake(w, h, false, argv[3]);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
