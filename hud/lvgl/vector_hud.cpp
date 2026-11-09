#include "vector_hud.hpp"
#include "hud/hud_theme.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kestrel_ui {
namespace {
constexpr float pi = 3.14159265358979323846f;
lv_opa_t opacity(float value) { return lv_opa_t(std::clamp(value * 255, 0.0f, 255.0f)); }
lv_color_t tint(const HudData& data, bool accent = false) {
    const auto& theme = hud_theme_at(data.theme);
    // A pale instrumentation palette, retaining theme hue for active readings.
    const float* rgb = accent ? theme.data : theme.text;
    return lv_color_make(uint8_t((0.72f + 0.28f * rgb[0]) * 255),
                         uint8_t((0.82f + 0.18f * rgb[1]) * 255),
                         uint8_t((0.79f + 0.21f * rgb[2]) * 255));
}
}
VectorHud::VectorHud(int w, int h, const std::string& assets)
    : width_(w), height_(h), u_(std::min(w / 1536.0f, h / 1024.0f)), text_(assets + "/canopy-font.kuf", w, h, 512 * 1024) {
    auto region = [&](float x, float y, float rw, float rh, hud_ipc::Role role = hud_ipc::Role::Hud) {
        return hud_ipc::Region{uint32_t(std::lround(x * w)), uint32_t(std::lround(y * h)),
            uint32_t(std::lround(rw * w)), uint32_t(std::lround(rh * h)), role};
    };
    layout_ = {uint32_t(w), uint32_t(h), {
        region(.014f, .014f, .25f, .205f), region(.29f, .014f, .42f, .155f),
        region(.755f, .014f, .231f, .205f),
        region(.014f, .29f, .365f, .375f, hud_ipc::Role::LeftWing),
        region(.621f, .29f, .365f, .375f, hud_ipc::Role::RightWing),
        region(.014f, .775f, .972f, .211f), region(.34f, .405f, .32f, .19f)}};
    layout_.validate();
    signatures_.resize(layout_.regions.size());
    screen_ = lv_obj_create(nullptr); lv_obj_remove_style_all(screen_); lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    for (const auto& r : layout_.regions) {
        auto* object = lv_obj_create(screen_); lv_obj_remove_style_all(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(object, r.x, r.y); lv_obj_set_size(object, r.width, r.height);
        lv_obj_add_event_cb(object, draw_event, LV_EVENT_DRAW_MAIN, this); objects_.push_back(object);
    }
}
VectorHud::~VectorHud() { if (screen_) lv_obj_delete(screen_); }
void VectorHud::render(const HudData& data, HudBuffer& buffer, uint32_t slot) {
    if (data.theme < 0 || data.theme >= hud_theme_count() || !(buffer.layout() == layout_)) throw std::runtime_error("Invalid Vector HUD setup");
    data_ = data; text_.begin_frame();text_.set_scale(data.text_scale/10.0f);
    if (last_buffer_ != &buffer) {
        previous_slot_ = -1; last_buffer_ = &buffer;
        for (auto& cached : signatures_) cached.clear();
    }
    for (size_t i = 0; i < objects_.size(); ++i) {
        auto key = signature(i);
        if (previous_slot_ >= 0 && key == signatures_[i]) {
            if (uint32_t(previous_slot_) != slot)
                std::memcpy(buffer.region(slot, i), buffer.region(uint32_t(previous_slot_), i), layout_.regions[i].bytes());
            continue;
        }
        const auto& r = layout_.regions[i]; lv_draw_buf_t draw{};
        if (lv_draw_buf_init(&draw, r.width, r.height, LV_COLOR_FORMAT_ARGB8888, r.width * 4, buffer.region(slot, i), r.bytes()) != LV_RESULT_OK ||
            lv_snapshot_take_to_draw_buf(objects_[i], LV_COLOR_FORMAT_ARGB8888, &draw) != LV_RESULT_OK) throw std::runtime_error("Vector HUD render failed");
        signatures_[i] = std::move(key);
    }
    previous_slot_ = int(slot);
}
std::vector<uint8_t> VectorHud::signature(size_t region) const {
    HudData relevant; relevant.enabled = data_.enabled; relevant.theme = data_.theme;
    relevant.text_scale=data_.text_scale;
    relevant.details = (region == 0 || region == 5) ? data_.details : true;
    if (region == 0) relevant.simulated = data_.simulated;
    if (region == 1) relevant.status = data_.status;
    if (region == 3 || region == 4) relevant.panels[region - 3].picked = data_.panels[region - 3].picked;
    auto add = [&](HudMetricId id) { if (const auto* item = data_.metric(id)) relevant.metrics.push_back(*item); };
    switch (region) {
    case 0: for (auto id : {HudMetricId::FcVariant, HudMetricId::FcFirmware, HudMetricId::UiCadence}) add(id); break;
    case 1: add(HudMetricId::RfState); break;
    case 2: for (auto id : {HudMetricId::Rc, HudMetricId::Voltage, HudMetricId::Mcs}) add(id); break;
    case 3: add(HudMetricId::Voltage); add(HudMetricId::Current); break;
    case 4: add(HudMetricId::Rc); break;
    case 5:
        for (auto id : {HudMetricId::ReceiverVoltage, HudMetricId::CpuTemperature, HudMetricId::RfTemperatureRaw,
                       HudMetricId::Mcs, HudMetricId::FrequencyRaw, HudMetricId::QualityRaw, HudMetricId::FlightState,
                       HudMetricId::MspFeed, HudMetricId::FlightMode, HudMetricId::FlightTimer}) add(id);
        break;
    default: break;
    }
    return hud_ipc::encode(relevant);
}
void VectorHud::draw_event(lv_event_t* event) {
    auto* self = static_cast<VectorHud*>(lv_event_get_user_data(event));
    if (!self->data_.enabled) return;
    auto* target = lv_event_get_target(event);
    for (size_t i = 0; i < self->objects_.size(); ++i) if (target == self->objects_[i]) { self->draw(lv_event_get_layer(event), i); break; }
}
void VectorHud::line(lv_layer_t* layer, float x1, float y1, float x2, float y2, float alpha, float width) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1 = {int32_t(std::lround(x1)), int32_t(std::lround(y1))}; d.p2 = {int32_t(std::lround(x2)), int32_t(std::lround(y2))};
    d.color = tint(data_, true); d.width = std::max(1, int(std::lround(width * u_))); d.opa = opacity(alpha);
    lv_draw_line(layer, &d);
}
void VectorHud::arc(lv_layer_t* layer, float x, float y, float radius, float start, float end, float alpha, float width) {
    const int steps = std::max(2, int(std::abs(end - start) / 3));
    for (int i = 0; i < steps; ++i) {
        const float a = (start + (end - start) * i / steps) * pi / 180;
        const float b = (start + (end - start) * (i + 1) / steps) * pi / 180;
        line(layer, x + radius * std::cos(a), y + radius * std::sin(a), x + radius * std::cos(b), y + radius * std::sin(b), alpha, width);
    }
}
void VectorHud::backing(lv_layer_t* layer, float x, float y, float w, float h, float alpha) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_make(5, 15, 20); d.bg_opa = opacity(alpha);
    lv_area_t a{int32_t(x), int32_t(y), int32_t(x + w - 1), int32_t(y + h - 1)}; lv_draw_rect(layer, &d, &a);
}
void VectorHud::text(lv_layer_t* layer, const std::string& value, float x, float baseline, float size, float alpha, bool right, bool accent) {
    const auto c = tint(data_, accent); const float rgb[]{c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f};
    // A restrained shadow makes the thin typography legible over bright video.
    constexpr float shadow[]{0, 0, 0};
    text_.canvas(layer, value, x + u_, baseline + u_, size * u_, right, shadow, alpha * .45f);
    text_.canvas(layer, value, x, baseline, size * u_, right, rgb, alpha);
}
const HudMetric& VectorHud::metric(HudMetricId id) const {
    const auto* item = data_.metric(id);
    static const HudMetric missing{};
    return item ? *item : missing;
}
void VectorHud::centered(lv_layer_t* layer, const std::string& value, float x, float baseline, float size, float alpha, bool accent) {
    text(layer, value, x - text_.advance(value) * size * u_ / (2 * canopy::kFontPixels), baseline, size, alpha, false, accent);
}
void VectorHud::row(lv_layer_t* layer, HudMetricId id, float x, float y, float w) {
    const auto& item = metric(id);
    text(layer, item.label, x, y, 13, .60f);
    text(layer, item.value + (item.unit.empty() ? "" : " " + item.unit), x + w, y, 16, item.valid ? .92f : .38f, true, item.valid);
}
void VectorHud::gauge(lv_layer_t* layer, const HudMetric& value, bool left) {
    const float cy = height_ * .485f, radius = height_ * .135f;
    const float cx = width_ * (left ? .355f : .645f);
    const float begin = left ? 120.0f : 60.0f, end = left ? 240.0f : -60.0f;
    const bool picked = data_.panels[left ? 0 : 1].picked;
    arc(layer, cx, cy, radius, begin, end, .09f, 5);
    arc(layer, cx, cy, radius, begin, end, picked ? .95f : .70f, picked ? 2 : 1);
    if (value.valid && value.metered) arc(layer, cx, cy, radius, begin, begin + (end - begin) * value.level, .95f, 2);
    for (int i = 0; i <= 24; ++i) {
        const float a = (begin + (end - begin) * i / 24) * pi / 180;
        const float len = (i % 6 == 0 ? 15 : 7) * u_;
        const float sign = left ? 1 : -1;
        line(layer, cx + radius * std::cos(a), cy + radius * std::sin(a),
            cx + (radius + sign * len) * std::cos(a), cy + (radius + sign * len) * std::sin(a), i % 6 == 0 ? .55f : .25f);
    }
    for (size_t i = 0; i < value.scale.size(); ++i) {
        const float a = (begin + (end - begin) * i / std::max<size_t>(1, value.scale.size() - 1)) * pi / 180;
        const float label_r = radius + (left ? 36 : 28) * u_;
        text(layer, value.scale[i], cx + label_r * std::cos(a), cy + label_r * std::sin(a) + 5 * u_, 13, .65f, left);
    }
    if (value.valid && value.metered) {
        const float a = (begin + (end - begin) * value.level) * pi / 180;
        const float px = cx + (radius - 9 * u_) * std::cos(a), py = cy + (radius - 9 * u_) * std::sin(a);
        lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
        const float s = left ? -1 : 1;
        d.p[0] = {int32_t(px + s * 7 * u_), int32_t(py)};
        d.p[1] = {int32_t(px - s * 3 * u_), int32_t(py - 6 * u_)};
        d.p[2] = {int32_t(px - s * 3 * u_), int32_t(py + 6 * u_)};
        d.bg_color = tint(data_, true); d.bg_opa = LV_OPA_COVER; lv_draw_triangle(layer, &d);
    }
    const float tx = width_ * (left ? .17f : .83f);
    backing(layer, tx - (left ? 8 : 180) * u_, cy - 66 * u_, 188 * u_, 119 * u_, .06f);
    text(layer, value.label.empty() ? "TELEMETRY" : value.label, tx, cy - 35 * u_, 20, .90f, !left);
    text(layer, value.value, tx, cy + 15 * u_, 52, value.valid ? 1 : .38f, !left, true);
    text(layer, value.unit, tx, cy + 43 * u_, 16, .68f, !left);
    if (left) {
        const auto& current = metric(HudMetricId::Current);
        text(layer, current.label + "  " + current.value + " " + current.unit, tx, cy + 83 * u_, 13, current.valid ? .75f : .38f);
    }
}
void VectorHud::draw(lv_layer_t* layer, size_t region) {
    const float w = float(width_), h = float(height_), u = u_;
    switch (region) {
    case 0: {
        backing(layer, w * .026f, h * .035f, w * .22f, h * .15f, .08f);
        line(layer, w * .020f, h * .045f, w * .035f, h * .024f, .85f);
        line(layer, w * .035f, h * .024f, w * .15f, h * .024f, .85f);
        line(layer, w * .15f, h * .024f, w * .235f, h * .060f, .40f);
        line(layer, w * .020f, h * .065f, w * .020f, h * .197f, .28f);
        text(layer, "KESTREL", w * .037f, h * .068f, 27, .98f);
        text(layer, data_.simulated ? "SIMULATED TELEMETRY" : "VRX / TELEMETRY", w * .037f, h * .097f, 13, .80f, false, true);
        row(layer, HudMetricId::FcVariant, w * .037f, h * .137f, w * .182f);
        row(layer, HudMetricId::FcFirmware, w * .037f, h * .167f, w * .182f);
        if (data_.details) row(layer, HudMetricId::UiCadence, w * .037f, h * .197f, w * .182f);
        break;
    }
    case 1: {
        const auto& link = metric(HudMetricId::RfState);
        centered(layer, link.valid ? link.value : data_.status, w * .5f, h * .070f, 17, link.valid ? .86f : .50f);
        line(layer, w * .32f, h * .105f, w * .46f, h * .105f, .40f);
        line(layer, w * .54f, h * .105f, w * .68f, h * .105f, .40f);
        line(layer, w * .295f, h * .051f, w * .310f, h * .073f, .37f);
        line(layer, w * .310f, h * .073f, w * .33f, h * .073f, .37f);
        line(layer, w * .705f, h * .051f, w * .690f, h * .073f, .37f);
        line(layer, w * .690f, h * .073f, w * .67f, h * .073f, .37f);
        centered(layer, "RX LINK", w * .5f, h * .128f, 11, .48f);
        break;
    }
    case 2: {
        line(layer, w * .765f, h * .060f, w * .85f, h * .024f, .40f);
        line(layer, w * .85f, h * .024f, w * .965f, h * .024f, .85f);
        line(layer, w * .965f, h * .024f, w * .980f, h * .045f, .85f);
        line(layer, w * .980f, h * .065f, w * .980f, h * .197f, .28f);
        backing(layer, w * .77f, h * .035f, w * .204f, h * .16f, .08f);
        const auto& rc = metric(HudMetricId::Rc);
        text(layer, rc.label, w * .795f, h * .068f, 14, .88f);
        text(layer, rc.value + rc.unit, w * .966f, h * .106f, 24, rc.valid ? .95f : .38f, true, true);
        for (int i = 0; i < 8; ++i) {
            const float x = w * .89f + i * 8 * u, bottom = h * .066f;
            line(layer, x, bottom, x, bottom - (5 + i * 3) * u, rc.valid && i < int(rc.level * 8 + .5f) ? .92f : .17f, 3);
        }
        row(layer, HudMetricId::Voltage, w * .795f, h * .145f, w * .171f);
        row(layer, HudMetricId::Mcs, w * .795f, h * .180f, w * .171f);
        break;
    }
    case 3:
    case 4: {
        const bool left = region == 3;
        const float x = w * (left ? .019f : .981f), s = left ? 1.0f : -1.0f;
        line(layer, x, h * .32f, x + s * 12 * u, h * .338f, .62f);
        line(layer, x + s * 12 * u, h * .338f, x + s * 12 * u, h * .58f, .58f);
        line(layer, x + s * 12 * u, h * .58f, x, h * .60f, .62f);
        gauge(layer, metric(left ? HudMetricId::Voltage : HudMetricId::Rc), left);
        break;
    }
    case 5: {
        // The reference's map/radar areas become actual receiver diagnostics.
        const float y = h * .865f;
        line(layer, w * .018f, h * .913f, w * .038f, h * .967f, .75f);
        line(layer, w * .038f, h * .967f, w * .238f, h * .967f, .75f);
        line(layer, w * .238f, h * .967f, w * .272f, h * .93f, .35f);
        line(layer, w * .982f, h * .913f, w * .962f, h * .967f, .75f);
        line(layer, w * .962f, h * .967f, w * .762f, h * .967f, .75f);
        line(layer, w * .762f, h * .967f, w * .728f, h * .93f, .35f);
        backing(layer, w * .03f, h * .798f, w * .235f, h * .151f, .09f);
        backing(layer, w * .735f, h * .798f, w * .235f, h * .151f, .09f);
        text(layer, "RECEIVER", w * .040f, h * .822f, 15, .83f, false, true);
        row(layer, HudMetricId::ReceiverVoltage, w * .040f, y, w * .205f);
        row(layer, HudMetricId::CpuTemperature, w * .040f, y + 29 * u, w * .205f);
        if (data_.details) row(layer, HudMetricId::RfTemperatureRaw, w * .040f, y + 58 * u, w * .205f);
        text(layer, "RADIO", w * .753f, h * .822f, 15, .83f, false, true);
        row(layer, HudMetricId::Mcs, w * .753f, y, w * .207f);
        row(layer, HudMetricId::FrequencyRaw, w * .753f, y + 29 * u, w * .207f);
        if (data_.details) row(layer, HudMetricId::QualityRaw, w * .753f, y + 58 * u, w * .207f);
        const auto& state = metric(HudMetricId::FlightState);
        const auto& feed = metric(HudMetricId::MspFeed);
        const float bx = w * .405f, by = h * .877f;
        backing(layer, bx, by - 25 * u, w * .19f, 61 * u, .13f);
        line(layer, bx, by - 25 * u, bx + w * .19f, by - 25 * u, .45f);
        line(layer, bx, by + 36 * u, bx + w * .19f, by + 36 * u, .45f);
        centered(layer, state.valid ? state.value : "FC STATE --", w * .5f, by, 19, state.valid ? .92f : .40f, true);
        centered(layer, feed.valid ? "MSP ACTIVE" : "MSP --", w * .5f, by + 24 * u, 12, .58f);
        const auto& mode = metric(HudMetricId::FlightMode);
        const auto& timer = metric(HudMetricId::FlightTimer);
        if (data_.details && (mode.valid || timer.valid))
            centered(layer, (mode.valid ? "MODE " + mode.value : "") + (timer.valid ? "  FLIGHT " + timer.value + " " + timer.unit : ""),
                     w * .5f, h * .963f, 12, .60f);
        break;
    }
    case 6: {
        const float cx = w * .5f, cy = h * .485f, r = 19 * u;
        // Fixed optical sight only. No fake horizon, heading or navigation.
        arc(layer, cx, cy, r, 15, 75, .48f); arc(layer, cx, cy, r, 105, 165, .48f);
        arc(layer, cx, cy, r, 195, 255, .48f); arc(layer, cx, cy, r, 285, 345, .48f);
        line(layer, cx - 29 * u, cy, cx - 23 * u, cy, .65f, 2);
        line(layer, cx + 23 * u, cy, cx + 29 * u, cy, .65f, 2);
        line(layer, cx, cy - 29 * u, cx, cy - 23 * u, .65f, 2);
        line(layer, cx, cy + 23 * u, cx, cy + 29 * u, .65f, 2);
        line(layer, w * .35f, cy, cx - 48 * u, cy, .34f);
        line(layer, cx + 48 * u, cy, w * .65f, cy, .34f);
        break;
    }
    }
}
} // namespace kestrel_ui
