#include "view.hpp"
#include "hud/hud_theme.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sstream>

namespace kestrel_ui {
namespace {
lv_color_t color(const float* rgb) { return lv_color_make(uint8_t(rgb[0] * 255), uint8_t(rgb[1] * 255), uint8_t(rgb[2] * 255)); }
lv_opa_t opacity(float v) { return lv_opa_t(std::clamp(v * 255, 0.0f, 255.0f)); }
void hidden(lv_obj_t* object, bool hide) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void rect(lv_layer_t* layer, int x, int y, int w, int h, const float* rgb, float alpha, int border = 0) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color(rgb); d.bg_opa = border ? lv_opa_t(0) : opacity(alpha);
    d.border_color = color(rgb); d.border_opa = border ? opacity(alpha) : lv_opa_t(0); d.border_width = border;
    lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &a);
}
}
View::View(const Controller& controller, int w, int h, const std::string& assets, HudSource* hud)
    : controller_(controller), model_(controller.model()), hud_(hud), width_(w), height_(h), text_(assets + "/canopy-font.kuf", w, h, 512 * 1024) {
    if (!hud_) { owned_hud_ = std::make_unique<InlineHud>(w, h, assets); hud_ = owned_hud_.get(); }
    screen_ = lv_screen_active();
    lv_obj_remove_style_all(screen_);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    picker_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(picker_);
    lv_obj_set_pos(picker_, 0, h * 58 / 100);
    // Only the two label rows live here. Hiding a full-height picker used to
    // invalidate/redraw both complete HUD wings at every open/fold keyframe.
    lv_obj_set_size(picker_, w, h * 74 / 100 - h * 58 / 100);
    lv_obj_remove_flag(picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(picker_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(picker_, LV_OBJ_FLAG_HIDDEN);
    menu_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(menu_);
    lv_obj_set_size(menu_, w * 34 / 100, h * 92 / 100);
    lv_obj_remove_flag(menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
    sync_hud_objects();
}
View::~View() { lv_obj_clean(screen_); drop_menu_snapshot(); }
void View::drop_menu_snapshot() {
    if (!menu_snapshot_) return;
    lv_image_cache_drop(menu_snapshot_); lv_image_header_cache_drop(menu_snapshot_);
    lv_draw_buf_destroy(menu_snapshot_); menu_snapshot_ = nullptr;
}
void View::prepare_menu_snapshot() {
    // Live serial counters can change forty times a second. Sample passive
    // content at 4 Hz; button/selection/theme changes still refresh immediately.
    if (menu_snapshot_ && menu_version_ == model_.menu_version && old_tab_ == model_.tab &&
        old_selected_ == model_.selected && old_theme_ == model_.theme && now_ >= menu_snapshot_checked_us_ &&
        now_ - menu_snapshot_checked_us_ < 250000) return;
    menu_snapshot_checked_us_ = now_;
    // Snapshot a derived preview. The View never mutates controller state.
    menu_preview_ = model_;
    if (model_.stage == MenuStage::Pick) {
        menu_preview_->tab = model_.remembered_section(); menu_preview_->selected = 0; menu_preview_->stage = MenuStage::Browse;
    }
    const auto& menu = *menu_preview_;
    std::ostringstream signature;
    signature << menu.side << ':' << menu.tab << ':' << menu.selected << ':' << menu.theme << ':'
              << (menu.stage == MenuStage::Edit) << ':' << menu.selected_help(now_);
    for (const auto& row : menu.rows(now_)) signature << '\n' << row.id << ':' << row.value;
    const std::string key = signature.str();
    if (menu_snapshot_ && key == menu_snapshot_signature_) {
        menu_preview_.reset(); return;
    }
    const float progress = menu_progress_;
    const int active_side = active_menu_side_;
    active_menu_side_ = model_.side; menu_progress_ = 1; capturing_menu_ = true;
    lv_obj_set_pos(menu_, active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, height_ * 6 / 100);
    drop_menu_snapshot();
    // The blade is entirely opaque while moving. XRGB avoids an unnecessary
    // per-pixel source-alpha blend when copying its cached image to ARGB.
    menu_snapshot_ = lv_snapshot_take(menu_, LV_COLOR_FORMAT_XRGB8888);
    capturing_menu_ = false; menu_progress_ = progress; active_menu_side_ = active_side;
    menu_preview_.reset();
    lv_obj_set_pos(menu_, active_side ? width_ - lv_obj_get_width(menu_) : 0, height_ * 6 / 100);
    if (menu_snapshot_) {
        menu_snapshot_signature_ = key; ++menu_snapshot_count_;
        lv_obj_invalidate(menu_);
    }
}
void View::draw_event(lv_event_t* e) {
    auto* self = static_cast<View*>(lv_event_get_user_data(e));
    auto* target = lv_event_get_target(e);
    auto* layer = lv_event_get_layer(e);
    if (target == self->menu_) self->draw_menu(layer);
    else if (target == self->picker_) self->draw_picker(layer);
    else for (size_t i = 0; i < self->hud_objects_.size(); ++i) if (target == self->hud_objects_[i]) { self->draw_hud(layer, i); break; }
}
void View::sync_hud_objects() {
    if (hud_layout_ == hud_->layout()) return;
    for (auto* object : hud_objects_) lv_obj_delete(object);
    hud_objects_.clear(); hud_layout_ = hud_->layout();
    for (size_t i = 0; i < hud_layout_.regions.size(); ++i) {
        const auto& r = hud_layout_.regions[i]; auto* object = lv_obj_create(screen_);
        lv_obj_remove_style_all(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(object, r.x, r.y); lv_obj_set_size(object, r.width, r.height);
        lv_obj_add_event_cb(object, draw_event, LV_EVENT_DRAW_MAIN, this);
        // HUD is below picker/menu, including when a worker reconnects.
        lv_obj_move_to_index(object, int(i)); hud_objects_.push_back(object);
    }
}
void View::draw_hud(lv_layer_t* layer, size_t index) {
    const auto* image = hud_->image(index); if (!image || !model_.hud_visible()) return;
    const auto& r = hud_layout_.regions.at(index);
    if (menu_progress_ > 0 && ((r.role == hud_ipc::Role::LeftWing && active_menu_side_ == 0) ||
                             (r.role == hud_ipc::Role::RightWing && active_menu_side_ == 1))) return;
    lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = image; d.opa = LV_OPA_COVER;
    lv_area_t area{int32_t(r.x), int32_t(r.y), int32_t(r.x + r.width - 1), int32_t(r.y + r.height - 1)};
    lv_draw_image(layer, &d, &area);
}
void View::draw_picker(lv_layer_t* layer) {
    const auto& th = hud_theme_at(model_.theme);
    const float alpha = 1 - menu_progress_;
    for (int side = 0; side < 2; ++side) {
        const bool selected = side == model_.side;
        text_.panel(layer, side ? "AIR UNIT" : "GOGGLE", side == 0, 56, 412,
                    canopy::kTextState, side != 0, selected ? th.text : th.quiet, alpha * (selected ? 1 : 0.6f));
        text_.panel(layer, selected ? "OK TO OPEN" : "LEFT / RIGHT", side == 0, 56, 439,
                    canopy::kTextRow, side != 0, th.quiet, alpha * 0.8f);
    }
}
lv_area_t View::menu_reveal(float progress) const {
    // Ease all the way to an empty strip at the blade's lower edge. Ending at
    // the canopy asset's top left a large rectangle to disappear when hidden.
    // Quintic easing also makes the last 50-Hz pose only a few pixels tall.
    const float p = std::clamp(progress, 0.0f, 1.0f);
    const float e = std::clamp(p * p * p * (p * (p * 6 - 15) + 10), 0.0f, 1.0f);
    const int h = int(std::lround((height_ * 92 / 100) * e));
    const int y = height_ * 6 / 100 + height_ * 92 / 100 - h;
    const int w = int(width_ * 0.31f + (lv_obj_get_width(menu_) - width_ * 0.31f) * e);
    const int x = active_menu_side_ ? width_ - w : 0;
    return {x, y, x + w - 1, y + h - 1};
}
void View::draw_menu(lv_layer_t* layer) {
    const auto& menu_model = menu_preview_ ? *menu_preview_ : model_;
    const auto& th = hud_theme_at(model_.theme);
    lv_area_t bounds;
    lv_obj_get_coords(menu_, &bounds);
    const int x = bounds.x1, y = bounds.y1, w = lv_obj_get_width(menu_), h = height_ * 92 / 100;
    const float u = height_ / 1080.0f;
    const int pad = int(22 * u), row_h = int(50 * u);
    const auto reveal = menu_reveal(menu_progress_);
    if (reveal.x1 > reveal.x2 || reveal.y1 > reveal.y2) return;
    const int backdrop_x = reveal.x1, backdrop_y = reveal.y1;
    const int backdrop_w = reveal.x2 - reveal.x1 + 1, backdrop_h = reveal.y2 - reveal.y1 + 1;
    if (!capturing_menu_ && menu_snapshot_) {
        // Reveal a single completed plane. Its opaque backing lets LVGL copy
        // most pixels, avoiding rerendering/blending all menu primitives.
        const lv_area_t previous_clip = layer->_clip_area;
        lv_area_t clipped{std::max(previous_clip.x1, reveal.x1), std::max(previous_clip.y1, reveal.y1),
                          std::min(previous_clip.x2, reveal.x2), std::min(previous_clip.y2, reveal.y2)};
        if (clipped.x1 <= clipped.x2 && clipped.y1 <= clipped.y2) {
            layer->_clip_area = clipped;
            lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = menu_snapshot_; d.opa = LV_OPA_COVER;
            lv_area_t image_area{x, y, x + int(menu_snapshot_->header.w) - 1, y + int(menu_snapshot_->header.h) - 1};
            lv_draw_image(layer, &d, &image_area);
            layer->_clip_area = previous_clip;
        }
        return;
    }
    // Keep the blade opaque when settled too: fading back to 94% requires a
    // costly full-plane blend exactly at the animation's final keyframe.
    rect(layer, backdrop_x, backdrop_y, backdrop_w, backdrop_h, th.ground, 1);
    rect(layer, backdrop_x, backdrop_y, backdrop_w, std::max(1, int(3 * u)), th.accent, menu_progress_);
    if (menu_progress_ < 0.5f) return;
    const float content_alpha = std::clamp((menu_progress_ - 0.5f) * 2, 0.0f, 1.0f);
    text_.canvas(layer, active_menu_side_ ? "AIR UNIT" : "GOGGLE", pad, pad + 14 * u, 15 * u, false, th.accent, content_alpha, x, y);
    int tab_x = pad;
    const std::vector<int> tabs = active_menu_side_ ? std::vector<int>{0, 6, 5} : std::vector<int>{1, 3, 2, 4};
    for (int tab : tabs) {
        const std::string title = Model::tab_name(tab);
        const int tw = int(text_.advance(title) * 21 * u / canopy::kFontPixels);
        text_.canvas(layer, title, tab_x, pad + 52 * u, 21 * u, false,
                      tab == menu_model.tab ? th.text : th.quiet, content_alpha, x, y);
        if (tab == menu_model.tab) rect(layer, x + tab_x, y + pad + int(61 * u), tw, std::max(1, int(3 * u)), th.accent, 1);
        tab_x += tw + int(26 * u);
    }
    const int list_top = pad + int(78 * u);
    rect(layer, x + pad, y + list_top - int(6 * u), w - 2 * pad, 1, th.quiet, 0.3f);
    const auto rows = menu_model.rows(now_);
    const bool editing = menu_model.stage == MenuStage::Edit;
    const int extra = editing ? int(80 * u) : 0;
    const int help_y = h - int(150 * u);
    const int visible = std::max(1, (help_y - list_top - extra - int(12 * u)) / row_h);
    const int start = std::clamp(menu_model.selected - visible / 2, 0, std::max(0, int(rows.size()) - visible));
    int ry = list_top;
    for (int i = start; i < std::min(int(rows.size()), start + visible); ++i) {
        const bool selected = i == menu_model.selected;
        const bool expanded = selected && editing;
        const int rh = row_h + (expanded ? extra : 0);
        if (selected) {
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, th.ground, content_alpha);
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, th.accent, content_alpha, std::max(1, int(3 * u)));
        }
        const float baseline = ry + row_h * 0.5f + 23 * u * 0.36f;
        text_.canvas(layer, rows[i].name, pad, baseline, 23 * u, false, th.text, content_alpha * (selected ? 1 : 0.82f), x, y);
        if (expanded) {
            const int current = menu_model.option_index(), n = int(rows[i].options.size());
            for (int delta = -1; delta <= 1; ++delta) {
                const int choice = (current + delta + n) % n;
                const float size = (delta == 0 ? 30 : 18) * u;
                text_.canvas(layer, rows[i].options[choice], w - pad, ry + rh * 0.5f + delta * 34 * u + size * 0.36f,
                             size, true, delta == 0 ? th.accent : th.quiet, content_alpha * (delta == 0 ? 1 : 0.7f), x, y);
            }
            ry += rh; continue;
        }
        const float available = w - 2 * pad - text_.advance(rows[i].name) * 23 * u / canopy::kFontPixels - 16 * u;
        const float value_width = text_.advance(rows[i].value) * 20 * u / canopy::kFontPixels;
        const float size = value_width > available && available > 0 ? std::max(12 * u, 20 * u * available / value_width) : 20 * u;
        text_.canvas(layer, rows[i].value, w - pad, baseline, size, true,
                     selected ? th.accent : th.quiet, content_alpha, x, y);
        ry += rh;
    }
    rect(layer, x + pad, y + help_y, w - 2 * pad, 1, th.quiet, 0.35f);
    std::istringstream words(menu_model.selected_help(now_));
    std::string word, line; int help_line = 0;
    auto emit_line = [&]() {
        text_.canvas(layer, line, pad, help_y + (28 + help_line * 26) * u, 18 * u, false, th.quiet, content_alpha * 0.85f, x, y);
        ++help_line; line.clear();
    };
    while (words >> word && help_line < 3) {
        const auto candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_.advance(candidate) * 18 * u / canopy::kFontPixels > w - 2 * pad) emit_line();
        line = line.empty() ? word : line + " " + word;
    }
    if (!line.empty() && help_line < 3) emit_line();
    text_.canvas(layer, editing ? "OK APPLY   /   BACK CANCEL" : "ARROWS NAVIGATE   /   OK EDIT   /   BACK FOLD", pad,
                 h - 18 * u, 14 * u, false, th.accent, content_alpha * 0.8f, x, y);
}
void View::update(uint64_t now) {
    text_.begin_frame();
    now_ = now;
    hud_->update(controller_.hud_data(now), now);
    sync_hud_objects();
    const bool hud_changed = hud_revision_ != hud_->revision();
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const bool unfolded = model_.menu_open && model_.stage != MenuStage::Pick;
    // Do not rebuild on telemetry changes during motion. A bounded snapshot
    // is refreshed in the picker or when the fully opened content changes.
    // Keep the outgoing blade intact during closure, even though the model has
    // already returned to Pick (or the user changes the next side quickly).
    if (model_.menu_open && (menu_progress_ == 0 || (unfolded && menu_progress_ == 1))) prepare_menu_snapshot();
    const float target = unfolded ? 1 : 0;
    const auto old_reveal = menu_reveal(menu_progress_);
    const float next = model_.hud_visible() ? menu_progress_ + std::clamp(target - menu_progress_, -dt / 0.24f, dt / 0.24f) : 0;
    const bool moving = next != menu_progress_;
    const bool wing_visibility_changed = (menu_progress_ == 0) != (next == 0);
    animating_ = moving || model_.visual.animating;
    menu_progress_ = next;
    if (unfolded) active_menu_side_ = model_.side;
    const bool right_side = active_menu_side_ != 0;
    const int menu_w = lv_obj_get_width(menu_);
    const int mx = right_side ? width_ - menu_w : 0;
    if (moving || old_tab_ != model_.tab || model_.menu_version != menu_version_) lv_obj_set_pos(menu_, mx, height_ * 6 / 100);
    hidden(menu_, next <= 0);
    // Reveal the two-wing picker after the closing blade has settled. Fading
    // it in during closure redraws both HUD wings for every moving frame.
    hidden(picker_, !model_.menu_open || model_.stage != MenuStage::Pick || menu_progress_ > 0);
    const auto& t = model_.backend.vrx;
    const auto& flight = model_.backend.flight;
    const bool flight_fresh = flight.stamp_us && now >= flight.stamp_us && now - flight.stamp_us < 2000000;
    const bool fresh = t.fresh(now), valid = t.link_valid(now);
    const bool hud_visible = model_.hud_visible();
    for (size_t i = 0; i < hud_objects_.size(); ++i)
        hidden(hud_objects_[i], !hud_visible || (hud_layout_.regions[i].role == hud_ipc::Role::Status && valid));
    const bool appearance = !initialized_ || old_theme_ != model_.theme || old_fresh_ != fresh || old_valid_ != valid || old_details_ != model_.show_details || old_hud_visible_ != hud_visible;
    const bool data = !initialized_ || model_.backend.flight_version != flight_version_ || flight_fresh != old_flight_fresh_ ||
        t.connected != presented_.connected || t.mcs != presented_.mcs ||
        t.frequency_raw != presented_.frequency_raw || t.distance_raw != presented_.distance_raw ||
        t.quality_raw != presented_.quality_raw || t.voltage_mv != presented_.voltage_mv ||
        t.cpu_temperature_x100 != presented_.cpu_temperature_x100 || t.rf_temperature_raw != presented_.rf_temperature_raw;
    const bool menu = old_menu_ != model_.menu_open || old_tab_ != model_.tab || old_selected_ != model_.selected || model_.menu_version != menu_version_;
    const bool counters = t.status_frames != presented_.status_frames || t.unknown_frames != presented_.unknown_frames ||
                          t.bad_frames != presented_.bad_frames || model_.rejected_inputs != rejected_inputs_;
    if (appearance || (!moving && menu) || (moving && next == 0) ||
        (!moving && !menu_snapshot_ && (data || (model_.menu_open && (model_.tab == 2 || model_.tab == 4 || model_.tab == 6) && counters))))
        lv_obj_invalidate(menu_);
    else if (moving) {
        // The cached plane is stationary: pixels shared by the two reveals
        // have not changed. Repaint only newly exposed/removed edge strips,
        // rather than copying the whole growing blade every frame.
        const auto reveal = menu_reveal(next);
        const lv_area_t shared{std::max(old_reveal.x1, reveal.x1), std::max(old_reveal.y1, reveal.y1),
                               std::min(old_reveal.x2, reveal.x2), std::min(old_reveal.y2, reveal.y2)};
        auto invalidate = [&](lv_area_t area) {
            if (area.x1 <= area.x2 && area.y1 <= area.y2) lv_obj_invalidate_area(menu_, &area);
        };
        for (const auto area : {old_reveal, reveal}) {
            if (shared.x1 > shared.x2 || shared.y1 > shared.y2) { invalidate(area); continue; }
            invalidate({area.x1, area.y1, area.x2, shared.y1 - 1});
            invalidate({area.x1, shared.y2 + 1, area.x2, area.y2});
            invalidate({area.x1, shared.y1, shared.x1 - 1, shared.y2});
            invalidate({shared.x2 + 1, shared.y1, area.x2, shared.y2});
        }
    }
    if (appearance || menu) lv_obj_invalidate(picker_);
    if (appearance || (menu && next == 0)) {
        for (auto* object : hud_objects_) lv_obj_invalidate(object);
    } else {
        if (hud_changed) for (size_t i = 0; i < hud_objects_.size(); ++i)
            if (hud_->changed_regions() & (1u << i)) lv_obj_invalidate(hud_objects_[i]);
        if (wing_visibility_changed) {
            for (size_t i = 0; i < hud_objects_.size(); ++i)
                if ((hud_layout_.regions[i].role == hud_ipc::Role::LeftWing && active_menu_side_ == 0) ||
                    (hud_layout_.regions[i].role == hud_ipc::Role::RightWing && active_menu_side_ == 1)) lv_obj_invalidate(hud_objects_[i]);
        }
    }
    presented_ = t; initialized_ = true;
    old_fresh_ = fresh; old_valid_ = valid; old_theme_ = model_.theme;
    old_menu_ = model_.menu_open; old_tab_ = model_.tab; old_selected_ = model_.selected;
    menu_version_ = model_.menu_version; old_details_ = model_.show_details;
    old_hud_visible_ = hud_visible;
    rejected_inputs_ = model_.rejected_inputs;
    flight_version_ = model_.backend.flight_version; old_flight_fresh_ = flight_fresh;
    hud_revision_ = hud_->revision();
}
} // namespace kestrel_ui
