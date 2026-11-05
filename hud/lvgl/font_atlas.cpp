#include "font_atlas.hpp"
#include <fstream>
#include <stdexcept>

namespace kestrel_ui {
namespace {
uint32_t u32(std::istream& input) {
    uint8_t p[4];
    if (!input.read(reinterpret_cast<char*>(p), 4)) throw std::runtime_error("Truncated HUD font atlas");
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}
FontAtlas::FontAtlas(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Missing " + path + "; run tools/ui_font_bake.py with ChakraPetch-SemiBold.ttf");
    load(input);
}
FontAtlas::FontAtlas(std::istream& input) { load(input); }
void FontAtlas::load(std::istream& input) {
    if (u32(input) != 0x4649554b || u32(input) != 1 || u32(input) != canopy::kFontPixels ||
        u32(input) != canopy::kTextCellHeight || u32(input) != canopy::kTextBaseline)
        throw std::runtime_error("Unsupported HUD font atlas");
    const uint32_t count = u32(input);
    if (count < 2 || count > 256) throw std::runtime_error("Invalid HUD font glyph count");
    size_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto code = u32(input), width = u32(input), advance = u32(input);
        if (code > 0x10ffff || (i && code <= glyphs_.back().code) || width < 1 || width > 128 || advance > 128 * 64)
            throw std::runtime_error("Invalid HUD font glyph metrics");
        Glyph glyph{code, int(width), int(advance), std::vector<uint8_t>(width * canopy::kTextCellHeight)};
        bytes += glyph.pixels.size();
        if (bytes > 1024 * 1024 || !input.read(reinterpret_cast<char*>(glyph.pixels.data()), glyph.pixels.size()))
            throw std::runtime_error("Invalid HUD font bitmap data");
        glyphs_.push_back(std::move(glyph));
    }
    kerning_.resize(size_t(count) * count);
    for (auto& kern : kerning_) {
        uint8_t p[2];
        if (!input.read(reinterpret_cast<char*>(p), 2)) throw std::runtime_error("Truncated HUD font kerning");
        const uint16_t raw = uint16_t(p[0]) | uint16_t(p[1]) << 8;
        kern = raw < 0x8000 ? int16_t(raw) : int16_t(int(raw) - 0x10000);
        if (kern < -4096 || kern > 4096) throw std::runtime_error("Invalid HUD font kerning");
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected HUD font trailer");
    if (std::none_of(glyphs_.begin(), glyphs_.end(), [](const Glyph& g) { return g.code == '?'; }))
        throw std::runtime_error("HUD font must include the fallback glyph");
}
std::vector<size_t> FontAtlas::indices(const std::string& text) const {
    if (text.size() > 256) throw std::invalid_argument("HUD text exceeds 256 bytes");
    const auto fallback = std::find_if(glyphs_.begin(), glyphs_.end(), [](const Glyph& g) { return g.code == '?'; });
    std::vector<size_t> result;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<uint8_t>(text[i++]);
        int remaining = 0;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; c &= 7; }
        else if (c >= 0x80) c = '?';
        for (int n = 0; n < remaining; ++n) {
            if (i == text.size() || (static_cast<uint8_t>(text[i]) & 0xc0) != 0x80) { c = '?'; break; }
            c = (c << 6) | (static_cast<uint8_t>(text[i++]) & 0x3f);
        }
        auto glyph = std::lower_bound(glyphs_.begin(), glyphs_.end(), c, [](const Glyph& g, uint32_t code) { return g.code < code; });
        if (glyph == glyphs_.end() || glyph->code != c) glyph = fallback;
        result.push_back(size_t(glyph - glyphs_.begin()));
    }
    return result;
}
float FontAtlas::advance(const std::string& text) const {
    const auto glyphs = indices(text);
    int64_t width = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        width += glyphs_[glyphs[i]].advance;
        if (i + 1 < glyphs.size()) width += kerning_[glyphs[i] * glyphs_.size() + glyphs[i + 1]];
    }
    return float(std::max<int64_t>(0, width)) / 64;
}
canopy::AlphaSurface FontAtlas::rasterize(const std::string& text) const {
    const auto glyphs = indices(text);
    const int width = (int(std::ceil(advance(text) + 6)) + 15) & ~15;
    if (width < 1 || width > 4096) throw std::invalid_argument("HUD text bitmap exceeds 4096 pixels");
    canopy::AlphaSurface out{0, 0, width, canopy::kTextCellHeight, std::vector<uint8_t>(size_t(width) * canopy::kTextCellHeight)};
    int64_t cursor = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const auto& g = glyphs_[glyphs[i]];
        const float position = float(cursor) / 64;
        const int x0 = int(std::floor(position));
        const float fraction = position - x0;
        for (int y = 0; y < out.height; ++y) for (int x = 0; x < g.width; ++x) {
            const int alpha = g.pixels[size_t(y) * g.width + x];
            for (int d = 0; d < 2; ++d) {
                const int dest = x0 + x + d;
                if (dest < 0 || dest >= width) continue;
                auto& p = out.pixels[size_t(y) * width + dest];
                p = uint8_t(std::min(255, int(p) + int(std::lround(alpha * (d ? fraction : 1 - fraction)))));
            }
        }
        cursor += g.advance;
        if (i + 1 < glyphs.size()) cursor += kerning_[glyphs[i] * glyphs_.size() + glyphs[i + 1]];
    }
    return out;
}
} // namespace kestrel_ui
