#pragma once
#include "render/software/projective.hpp"
#include <istream>
#include <string>

namespace kestrel_ui {
// Host-baked glyphs use the GLES OSD's 40px Chakra Petch ink, 64px texture cell
// and baseline 48. No target font engine/library is needed.
class FontAtlas {
public:
    explicit FontAtlas(const std::string& path);
    explicit FontAtlas(std::istream& input);
    int pixels() const { return pixels_; }
    int cell_height() const { return cell_height_; }
    int baseline() const { return baseline_; }
    float advance(const std::string& text) const;
    canopy::AlphaSurface rasterize(const std::string& text) const;
private:
    struct Glyph { uint32_t code; int width, advance; std::vector<uint8_t> pixels; };
    int pixels_=40, cell_height_=64, baseline_=48;
    std::vector<Glyph> glyphs_;
    std::vector<int16_t> kerning_;
    void load(std::istream& input);
    std::vector<size_t> indices(const std::string& text) const;
};
} // namespace kestrel_ui
