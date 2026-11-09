#include "hud_view.hpp"
#include "hud/hud_theme.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace kestrel_ui {
namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
lv_color_t color(const float* rgb) { return lv_color_make(uint8_t(rgb[0] * 255), uint8_t(rgb[1] * 255), uint8_t(rgb[2] * 255)); }
lv_opa_t opacity(float v) { return lv_opa_t(std::clamp(v * 255, 0.0f, 255.0f)); }
}
void HudView::Asset::load(const std::string& path, int width, int height) {
    std::ifstream in(path, std::ios::binary); uint8_t header[28]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "KUIA", 4) ||
        le32(header + 4) != uint32_t(width) || le32(header + 8) != uint32_t(height))
        throw std::runtime_error("Missing/mismatched HUD asset: " + path);
    const auto ox = le32(header + 12), oy = le32(header + 16), w = le32(header + 20), h = le32(header + 24);
    if (!w || !h || uint64_t(ox) + w > uint32_t(width) || uint64_t(oy) + h > uint32_t(height))
        throw std::runtime_error("Invalid HUD asset bounds");
    x = int(ox); y = int(oy); pixels.resize(size_t(w) * h);
    if (!in.read(reinterpret_cast<char*>(pixels.data()), pixels.size()) || in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Invalid HUD asset length");
    image.header.magic = LV_IMAGE_HEADER_MAGIC; image.header.cf = LV_COLOR_FORMAT_A8;
    image.header.w = w; image.header.h = h; image.header.stride = w;
    image.data_size = pixels.size(); image.data = pixels.data();
}
HudView::HudView(int w, int h, const std::string& assets)
    : width_(w), height_(h), text_(assets + "/canopy-font.kuf", w, h, 512 * 1024) {
    assets_[0].load(assets + "/canopy-left.kui", w, h); assets_[1].load(assets + "/canopy-right.kui", w, h);
    layout_ = {uint32_t(w), uint32_t(h), {
        {0, uint32_t(h * 3 / 5), uint32_t(w / 2), uint32_t(h - h * 3 / 5), hud_ipc::Role::LeftWing},
        {uint32_t(w / 2), uint32_t(h * 3 / 5), uint32_t(w - w / 2), uint32_t(h - h * 3 / 5), hud_ipc::Role::RightWing},
        {uint32_t(w / 4), uint32_t(h / 3), uint32_t(w / 2), uint32_t(h / 5), hud_ipc::Role::Status}}};
    layout_.validate();
    // An inactive LVGL screen keeps inline test rendering isolated from the
    // UI's active screen. The worker uses the same View with its own display.
    screen_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen_); lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < objects_.size(); ++i) {
        const auto& r = layout_.regions[i]; objects_[i] = lv_obj_create(screen_);
        lv_obj_remove_style_all(objects_[i]); lv_obj_remove_flag(objects_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(objects_[i], r.x, r.y); lv_obj_set_size(objects_[i], r.width, r.height);
        lv_obj_add_event_cb(objects_[i], draw_event, LV_EVENT_DRAW_MAIN, this);
    }
}
HudView::~HudView() { if (screen_) lv_obj_delete(screen_); }
void HudView::render(const HudData& data, HudBuffer& buffer, uint32_t slot) {
    if (data.theme < 0 || data.theme >= hud_theme_count() || !(buffer.layout() == layout_))
        throw std::runtime_error("Invalid HUD theme or renderer buffer layout");
    data_ = data; text_.begin_frame();text_.set_scale(data.text_scale/10.0f);
    for (size_t i = 0; i < objects_.size(); ++i) {
        const auto& r = layout_.regions[i]; lv_draw_buf_t draw{};
        if (lv_draw_buf_init(&draw, r.width, r.height, LV_COLOR_FORMAT_ARGB8888, r.width * 4,
                             buffer.region(slot, i), r.bytes()) != LV_RESULT_OK ||
            lv_snapshot_take_to_draw_buf(objects_[i], LV_COLOR_FORMAT_ARGB8888, &draw) != LV_RESULT_OK)
            throw std::runtime_error("HUD region rendering failed");
    }
}
void HudView::draw_event(lv_event_t* event) {
    auto* self = static_cast<HudView*>(lv_event_get_user_data(event));
    if (!self->data_.enabled) return;
    auto* target = lv_event_get_target(event); auto* layer = lv_event_get_layer(event);
    if (target == self->objects_[2]) self->status(layer);
    else self->panel(layer, target == self->objects_[0]);
}
void HudView::track(lv_layer_t* layer, bool left, float start, float end, float y, float height,
                    int segments, float level, const float* rgb, float alpha, float block) {
    const float pitch = (end - start) / (segments * (1 - 0.32f / 2));
    float cursor = start;
    const int shown = std::clamp(int(level * segments + 0.5f), 0, segments);
    const int revealed = std::clamp(int(block * segments + 0.5f), 0, segments);
    for (int i = 0; i < std::min(segments, revealed); ++i) {
        const float factor = 1 - 0.32f * i / std::max(1, segments - 1), step = pitch * factor, width = step * 10 / 13;
        const auto p0 = canopy::project(width_, height_, left, cursor, y - height * factor);
        const auto p1 = canopy::project(width_, height_, left, cursor + width, y - height * factor);
        const auto p2 = canopy::project(width_, height_, left, cursor + width, y);
        const auto p3 = canopy::project(width_, height_, left, cursor, y);
        for (auto points : {std::array<canopy::Point, 3>{p0, p1, p2}, std::array<canopy::Point, 3>{p0, p2, p3}}) {
            lv_draw_triangle_dsc_t tri; lv_draw_triangle_dsc_init(&tri);
            for (int j = 0; j < 3; ++j) tri.p[j] = {int32_t(std::lround(points[j].x)), int32_t(std::lround(points[j].y))};
            tri.bg_color = color(rgb);
            tri.bg_opa = opacity((i < shown ? 0.7f + 0.3f * (1 - float(i) / segments) : 0.12f) * alpha);
            lv_draw_triangle(layer, &tri);
        }
        cursor += step;
    }
}
void HudView::panel(lv_layer_t* layer, bool left) {
    const auto& p = data_.panels[left ? 0 : 1];
    if (!p.visible) return;
    const auto& th = hud_theme_at(data_.theme); const auto& a = assets_[left ? 0 : 1];
    lv_draw_image_dsc_t image; lv_draw_image_dsc_init(&image);
    image.src = &a.image; image.recolor = color(th.ground); image.recolor_opa = LV_OPA_COVER;
    image.opa = opacity(p.dim * p.shade);
    lv_area_t area{a.x, a.y, a.x + int(a.image.header.w) - 1, a.y + int(a.image.header.h) - 1};
    lv_draw_image(layer, &image, &area);
    const auto frame = canopy::screen_frame(width_, height_);
    const float content = p.dim * p.block * (left && !p.fresh ? 0.75f : 1.0f);
    auto digits_width = [&](std::string value, float size) {
        for (char& c : value) if (c >= '0' && c <= '9') c = '0';
        return text_.design_advance(value, size);
    };
    auto baseline = [&](float size) { return 512 - (canopy::kTextBig - size) * 0.25f / frame.U; };
    float state_x = left ? 60 : 150;
    if (left) {
        text_.panel(layer, p.primary, true, 14, 512, canopy::kTextBig, false, p.primary_unit.empty() ? th.quiet : th.text, content);
        float x = 14 + digits_width(p.primary, canopy::kTextBig);
        if (!p.primary_unit.empty()) {
            text_.panel(layer, p.primary_unit, true, x, baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.text, content * 0.72f);
            x += text_.design_advance(p.primary_unit, canopy::kTextUnit);
        }
        if (!p.secondary.empty()) {
            x += 10 * frame.U / frame.UX;
            text_.panel(layer, "\xe2\x80\xa2", true, x, baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.quiet, content);
            x += text_.design_advance("\xe2\x80\xa2", canopy::kTextUnit) + 10 * frame.U / frame.UX;
            state_x = x;
            text_.panel(layer, p.secondary, true, x, baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.text, content * 0.82f);
            x += digits_width(p.secondary, canopy::kTextUnit);
            text_.panel(layer, p.secondary_unit, true, x, baseline(canopy::kTextSmall), canopy::kTextSmall, false, th.text, content * 0.72f);
        }
    } else if (!p.primary_unit.empty()) {
        text_.panel(layer, p.primary_unit, false, 14, baseline(canopy::kTextSmall), canopy::kTextSmall, true, th.text, content * 0.72f);
        text_.panel(layer, p.primary, false, 14 + text_.design_advance(p.primary_unit, canopy::kTextSmall), 512, canopy::kTextBig, true, th.text, content);
    } else text_.panel(layer, p.primary, false, 14, 512, canopy::kTextBig, true, th.quiet, content);
    if (!left && !p.secondary.empty()) {
        float x = 14 + digits_width(p.primary, canopy::kTextBig) + text_.design_advance(p.primary_unit, canopy::kTextSmall) + 10 * frame.U / frame.UX;
        text_.panel(layer, "•", false, x, baseline(canopy::kTextUnit), canopy::kTextUnit, true, th.quiet, content);
        x += text_.design_advance("•", canopy::kTextUnit) + 10 * frame.U / frame.UX;
        text_.panel(layer, p.secondary_unit, false, x, baseline(canopy::kTextSmall), canopy::kTextSmall, true, th.text, content * .72f);
        x += text_.design_advance(p.secondary_unit, canopy::kTextSmall);
        text_.panel(layer, p.secondary, false, x, baseline(canopy::kTextUnit), canopy::kTextUnit, true, th.text, content * .82f);
    }
    constexpr float amber[] = {1, .72f, .15f}, red[] = {1, .30f, .25f};
    auto alert_color = [&](int alert, const float* normal) { return alert == 2 ? red : alert == 1 ? amber : normal; };
    if (left && !p.recording.empty()) {
        const auto at = canopy::project(width_, height_, true, 14, 440);
        const int size = std::max(4, height_ / 120);
        lv_draw_rect_dsc_t square; lv_draw_rect_dsc_init(&square); square.bg_color = color(red);
        lv_area_t box{int(at.x), int(at.y)-size, int(at.x)+size-1, int(at.y)-1};
        lv_draw_rect(layer, &square, &box);
        text_.panel(layer, p.recording, true, 30, 442, canopy::kTextRow, false, red, 1);
    }
    const float state_width = text_.design_advance(p.state, canopy::kTextState);
    constexpr float rest[] = {0.663f, 0.706f, 0.741f};
    for (float y : {456.0f, 596.0f}) {
        const float reach = p.picked ? 252 : 252 * p.rule;
        for (int i = 0; i < 32; ++i) {
            const float x0 = -canopy::kBleed + (252 + canopy::kBleed) * i / 32;
            const float x1 = std::min(reach, -canopy::kBleed + (252 + canopy::kBleed) * (i + 1) / 32);
            if (x1 <= x0) continue;
            std::array<std::array<float, 2>, 2> spans{{{x0, x1}, {0, 0}}};
            if (!p.state.empty() && y == 456) spans = {{{x0, std::min(x1, state_x - 8)}, {std::max(x0, state_x + state_width + 10), x1}}};
            for (auto span : spans) {
                if (span[1] <= span[0]) continue;
                const auto a = canopy::project(width_, height_, left, span[0], y), b = canopy::project(width_, height_, left, span[1], y);
                lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
                line.p1 = {int32_t(std::lround(a.x)), int32_t(std::lround(a.y))}; line.p2 = {int32_t(std::lround(b.x)), int32_t(std::lround(b.y))};
                line.width = p.picked ? std::max(2, height_ / 270) : std::max(1, height_ / 360);
                line.color = color(p.picked || p.armed ? th.accent : rest);
                const float taper = std::clamp((span[0] - 126) / 126, 0.0f, 1.0f);
                line.opa = opacity((1 - taper * taper) * (p.picked ? 1.0f : 0.8f * p.rail_dim)); lv_draw_line(layer, &line);
            }
        }
    }
    const float label_width = left ? text_.design_advance(p.upper_label, canopy::kTextLabel) :
        std::max(text_.design_advance(p.upper_label, canopy::kTextLabel), text_.design_advance(p.lower_label, canopy::kTextLabel));
    const float start = 14 + label_width + 12;
    text_.panel(layer, p.upper_label, left, 14, 527, canopy::kTextLabel, !left, th.text, content);
    text_.panel(layer, p.lower_label, left, 14, 537, canopy::kTextLabel, !left, th.accent, content);
    track(layer, left, start, 271, 525, 9, 20, p.upper_level, th.data, content, p.block);
    track(layer, left, start, 271, 534, 4, left ? 20 : 7, p.lower_level, alert_color(p.lower_alert, th.accent), content, p.block);
    if (!p.state.empty()) text_.panel(layer, p.state, left, state_x, 456 + canopy::kTextState / frame.U / 2,
        canopy::kTextState, !left, p.armed ? th.accent : th.quiet, left ? content * p.state_opacity : p.state_opacity);
    if (!data_.details) return;
    auto row = [&](const std::string& body, const std::string& tail, float y, const float* body_color, const float* tail_color) {
        if (left) {
            text_.panel(layer, body, true, 14, y, canopy::kTextRow, false, body_color, content);
            text_.panel(layer, tail, true, 14 + text_.design_advance(body, canopy::kTextRow), y, canopy::kTextRow, false, tail_color, content);
        } else {
            text_.panel(layer, tail, false, 14, y, canopy::kTextRow, true, tail_color, content);
            text_.panel(layer, body, false, 14 + text_.design_advance(tail, canopy::kTextRow), y, canopy::kTextRow, true, body_color, content);
        }
    };
    row(p.detail1, p.detail1_tail, 560, p.detail1_quiet ? th.quiet : th.text, alert_color(p.detail1_alert, th.text));
    row(p.detail2, p.detail2_tail, 584, alert_color(p.detail2_alert, th.text), alert_color(p.supply_alert, th.text));
}
void HudView::status(lv_layer_t* layer) {
    if (!data_.show_status) return;
    const auto& th = hud_theme_at(data_.theme); const std::string title = "KESTREL";
    text_.canvas(layer, title, (width_ - text_.advance(title) * 48 / canopy::kFontPixels) / 2, height_ * 0.39f, 48, false, th.text, 0.9f);
    text_.canvas(layer, data_.status, (width_ - text_.advance(data_.status) * 20 / canopy::kFontPixels) / 2, height_ * 0.44f, 20, false, th.quiet, 1);
}
} // namespace kestrel_ui
