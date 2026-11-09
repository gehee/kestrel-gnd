#pragma once
#include "canopy.hpp"
#include <limits>
#include <stdexcept>
#include <vector>

namespace canopy {
struct AlphaSurface {
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

// Exact planar perspective, not an affine skew or two independently stretched
// triangles. Inverse mapping keeps every destination pixel covered and avoids
// a visible seam through text quads.
class Homography {
public:
    explicit Homography(const std::array<Point, 4>& q) {
        double sign = 0;
        for (int i = 0; i < 4; ++i) {
            const auto a = q[i], b = q[(i + 1) % 4], c = q[(i + 2) % 4];
            const double cross = double(b.x - a.x) * (c.y - b.y) - double(b.y - a.y) * (c.x - b.x);
            if (!std::isfinite(cross) || std::abs(cross) < 1e-8 || (sign && cross * sign < 0))
                throw std::invalid_argument("Degenerate or folded perspective quad");
            sign = cross;
        }
        const double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x;
        const double dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
        const double dx3 = q[0].x - q[1].x + q[2].x - q[3].x;
        const double dy3 = q[0].y - q[1].y + q[2].y - q[3].y;
        const double det = dx1 * dy2 - dx2 * dy1;
        if (std::abs(det) < 1e-12) throw std::invalid_argument("Singular perspective quad");
        const double g = (dx3 * dy2 - dx2 * dy3) / det;
        const double h = (dx1 * dy3 - dx3 * dy1) / det;
        matrix_ = {q[1].x - q[0].x + g * q[1].x, q[3].x - q[0].x + h * q[3].x, q[0].x,
                   q[1].y - q[0].y + g * q[1].y, q[3].y - q[0].y + h * q[3].y, q[0].y, g, h, 1};
        const auto& m = matrix_;
        inverse_ = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                    m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                    m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
        const double determinant = m[0] * inverse_[0] + m[1] * inverse_[3] + m[2] * inverse_[6];
        if (std::abs(determinant) < 1e-12) throw std::invalid_argument("Singular perspective mapping");
    }
    Point forward(double x, double y) const { return apply(matrix_, x, y); }
    Point inverse(double x, double y) const { return apply(inverse_, x, y); }
    const std::array<double, 9>& inverse_coefficients() const { return inverse_; }
private:
    std::array<double, 9> matrix_, inverse_;
    static Point apply(const std::array<double, 9>& m, double x, double y) {
        const double w = m[6] * x + m[7] * y + m[8];
        if (std::abs(w) < 1e-12) return {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        return {float((m[0] * x + m[1] * y + m[2]) / w), float((m[3] * x + m[4] * y + m[5]) / w)};
    }
};

inline AlphaSurface warp_alpha(const AlphaSurface& source, const std::array<Point, 4>& q,
                               int screen_width, int screen_height) {
    if (source.width <= 0 || source.width > 4096 || source.height <= 0 || source.height > 512 ||
        source.pixels.size() != size_t(source.width) * source.height ||
        screen_width <= 0 || screen_width > 4096 || screen_height <= 0 || screen_height > 2160)
        throw std::invalid_argument("Invalid perspective surface bounds");
    Homography map(q);
    float minx = q[0].x, maxx = minx, miny = q[0].y, maxy = miny;
    for (auto p : q) { minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
    AlphaSurface out;
    out.x = int(std::floor(std::clamp(minx, 0.0f, float(screen_width))));
    out.y = int(std::floor(std::clamp(miny, 0.0f, float(screen_height))));
    out.width = int(std::ceil(std::clamp(maxx, 0.0f, float(screen_width)))) - out.x;
    out.height = int(std::ceil(std::clamp(maxy, 0.0f, float(screen_height)))) - out.y;
    if (!out.width || !out.height) return out;
    if (size_t(out.width) * out.height > 1024 * 1024) throw std::invalid_argument("Perspective tile exceeds 1 MiB");
    out.pixels.resize(size_t(out.width) * out.height);
    auto at = [&](int x, int y) -> float {
        return x < 0 || y < 0 || x >= source.width || y >= source.height ? 0 : source.pixels[size_t(y) * source.width + x];
    };
    const auto& m = map.inverse_coefficients();
    for (int y = 0; y < out.height; ++y) {
        const double px = out.x + 0.5, py = out.y + y + 0.5;
        double nx = m[0] * px + m[1] * py + m[2], ny = m[3] * px + m[4] * py + m[5];
        double denominator = m[6] * px + m[7] * py + m[8];
        for (int x = 0; x < out.width; ++x, nx += m[0], ny += m[3], denominator += m[6]) {
            if (std::abs(denominator) < 1e-12) continue;
            // One reciprocal per pixel and incremental scanlines retain exact
            // projective sampling while avoiding two divisions/matrix multiplies.
            const double reciprocal = 1 / denominator;
            const float u = float(nx * reciprocal), v = float(ny * reciprocal);
            if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) continue;
            const float sx = u * source.width - 0.5f, sy = v * source.height - 0.5f;
            const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
            const float fx = sx - ix, fy = sy - iy;
            const float a = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                            (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            out.pixels[size_t(y) * out.width + x] = uint8_t(std::lround(a));
        }
    }
    return out;
}
} // namespace canopy
