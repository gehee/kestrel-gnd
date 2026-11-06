#pragma once
#include <algorithm>
#include <vector>

namespace base {
struct Damage { int x, y, width, height; };
// Each scanout buffer accumulates changes until it is safe to write again.
// Copy from the latest retained image, never from an active scanout buffer.
class DamageRows {
public:
    DamageRows(int width, int height) : width_(width), rows_(height, {width, 0}) {}
    void add(Damage d) {
        const int x1 = std::clamp(d.x, 0, width_), x2 = std::clamp(d.x + d.width, 0, width_);
        if (x1 >= x2) return;
        for (int y = std::max(0, d.y); y < std::min(int(rows_.size()), d.y + d.height); ++y) {
            rows_[y].first = std::min(rows_[y].first, x1);
            rows_[y].second = std::max(rows_[y].second, x2);
        }
    }
    void all() { add({0, 0, width_, int(rows_.size())}); }
    void clear() { std::fill(rows_.begin(), rows_.end(), std::pair<int, int>{width_, 0}); }
    const auto& rows() const { return rows_; }
private:
    int width_;
    std::vector<std::pair<int, int>> rows_;
};
} // namespace base
