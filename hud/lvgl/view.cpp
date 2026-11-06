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
    : controller_(controller), model_(controller.model()), hud_(hud), width_(w), height_(h), text_(assets + "/canopy-font.kuf", w, h, 512 * 1024), idle_(assets, w, h), bf_atlas_(assets+"/betaflight.kui") {
    if (!hud_) { owned_hud_ = std::make_unique<InlineHud>(w, h, assets); hud_ = owned_hud_.get(); }
    screen_ = lv_screen_active();
    lv_obj_remove_style_all(screen_);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    idle_object_ = lv_obj_create(screen_);
    dimmer_ = lv_obj_create(screen_);
    for (auto* object : {idle_object_, dimmer_}) {
        lv_obj_remove_style_all(object); lv_obj_set_size(object, w, h);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(object, draw_event, LV_EVENT_DRAW_MAIN, this);
    }
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
    lv_obj_set_size(menu_, int(std::lround(w * 0.34f)), h);
    lv_obj_remove_flag(menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
    sync_hud_objects();
    channel_=lv_obj_create(screen_);
    lv_obj_remove_style_all(channel_);
    lv_obj_set_pos(channel_,0,h*62/100);
    lv_obj_set_size(channel_,w,h-h*62/100);
    lv_obj_remove_flag(channel_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(channel_,draw_event,LV_EVENT_DRAW_MAIN,this);
    lv_obj_add_flag(channel_,LV_OBJ_FLAG_HIDDEN);
    clock_=lv_obj_create(screen_);
    lv_obj_remove_style_all(clock_);
    lv_obj_set_pos(clock_,w/2-w/10,0);
    lv_obj_set_size(clock_,w/5,h/16);
    lv_obj_remove_flag(clock_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clock_,draw_event,LV_EVENT_DRAW_MAIN,this);
    bf_object_=lv_obj_create(screen_);lv_obj_remove_style_all(bf_object_);
    lv_obj_set_size(bf_object_,w,h);lv_obj_remove_flag(bf_object_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bf_object_,draw_event,LV_EVENT_DRAW_MAIN,this);
    lv_obj_move_to_index(bf_object_,1);
    debug_object_=lv_obj_create(screen_);lv_obj_remove_style_all(debug_object_);
    lv_obj_set_pos(debug_object_,w*2/3,h-h/30);lv_obj_set_size(debug_object_,w/3,h/30);
    lv_obj_remove_flag(debug_object_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(debug_object_,draw_event,LV_EVENT_DRAW_MAIN,this);
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
        menu_preview_->tab = model_.remembered_section(); menu_preview_->selected = menu_preview_->first_row(now_); menu_preview_->stage = MenuStage::Browse;
    }
    const auto& menu = *menu_preview_;
    std::ostringstream signature;
    signature << menu.side << ':' << menu.tab << ':' << menu.selected << ':' << menu.theme << ':'
              << (menu.stage == MenuStage::Edit) << ':' << menu.selected_help(now_);
    for (const auto& row : menu.rows(now_)) signature << '\n' << row.id << ':' << row.value;
    const std::string key = signature.str();
    if (menu_snapshot_ && key == menu_snapshot_signature_) {
        return;
    }
    const float progress = menu_progress_;
    const int active_side = active_menu_side_;
    active_menu_side_ = model_.side; menu_progress_ = 1; capturing_menu_ = true;
    lv_obj_set_pos(menu_, active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, 0);
    drop_menu_snapshot();
    // Preserve the original translucent panel over the video. Cache only the
    // settled layout; moving panels are laid out in their interpolated bounds.
    menu_snapshot_ = lv_snapshot_take(menu_, LV_COLOR_FORMAT_ARGB8888);
    capturing_menu_ = false; menu_progress_ = progress; active_menu_side_ = active_side;
    lv_obj_set_pos(menu_, active_side ? width_ - lv_obj_get_width(menu_) : 0, 0);
    if (menu_snapshot_) {
        menu_snapshot_signature_ = key; ++menu_snapshot_count_;
        lv_obj_invalidate(menu_);
    }
}
void View::draw_event(lv_event_t* e) {
    auto* self = static_cast<View*>(lv_event_get_user_data(e));
    auto* target = lv_event_get_target(e);
    auto* layer = lv_event_get_layer(e);
    if (target == self->idle_object_) self->idle_.draw(layer, self->model_, self->now_);
    else if (target == self->dimmer_) {
        if (self->model_.menu_open || self->menu_progress_ > 0)
            rect(layer, 0, 0, self->width_, self->height_, hud_theme_at(self->model_.theme).ground,
                 (self->model_.menu_open ? 0.10f : 0.0f) + self->menu_progress_ * 0.06f);
    }
    else if (target == self->menu_) self->draw_menu(layer);
    else if (target == self->picker_) self->draw_picker(layer);
    else if (target == self->channel_) self->draw_channel(layer);
    else if (target == self->clock_) {
        const float size=24.0f*self->height_/1080;
        const float x=(self->width_-self->text_.advance(self->clock_text_)*size/canopy::kFontPixels)/2;
        self->text_.canvas(layer,self->clock_text_,x,36.0f*self->height_/1080,size,false,hud_theme_at(self->model_.theme).text,.9f);
    }
    else if(target==self->bf_object_ && self->bf_visible_) self->bf_atlas_.draw(layer,self->bf_grid_,self->width_,self->height_);
    else if(target==self->debug_object_) self->text_.canvas(layer,self->debug_text_,self->width_-18,self->height_-12,
        18.0f*self->height_/1080,true,hud_theme_at(self->model_.theme).quiet,1);
    else for (size_t i = 0; i < self->hud_objects_.size(); ++i) if (target == self->hud_objects_[i]) { self->draw_hud(layer, i); break; }
}
void View::sync_hud_objects() {
    if (hud_layout_ == hud_->layout()) return;
    for (auto* object : hud_objects_) lv_obj_delete(object);
    hud_objects_.clear(); hud_layout_ = hud_->layout(); motion_transform_={};
    for (size_t i = 0; i < hud_layout_.regions.size(); ++i) {
        const auto& r = hud_layout_.regions[i]; auto* object = lv_obj_create(screen_);
        lv_obj_remove_style_all(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_transform_pivot_x(object,width_/2-int(r.x),0);
        lv_obj_set_style_transform_pivot_y(object,height_/2-int(r.y),0);
        lv_obj_set_pos(object, r.x, r.y); lv_obj_set_size(object, r.width, r.height);
        lv_obj_add_event_cb(object, draw_event, LV_EVENT_DRAW_MAIN, this);
        // HUD is below picker/menu, including when a worker reconnects.
        lv_obj_move_to_index(object, int(i + (bf_object_?2:1))); hud_objects_.push_back(object);
    }
}
void View::draw_hud(lv_layer_t* layer, size_t index) {
    const auto* image = hud_->image(index); if (!image || !model_.hud_visible()) return;
    const auto& r = hud_layout_.regions.at(index);
    if (menu_progress_ > 0 && ((r.role == hud_ipc::Role::LeftWing && active_menu_side_ == 0) ||
                             (r.role == hud_ipc::Role::RightWing && active_menu_side_ == 1))) return;
    lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = image; d.opa = LV_OPA_COVER;
    lv_area_t area; lv_obj_get_coords(hud_objects_[index],&area);
    lv_draw_image(layer, &d, &area);
}
void View::draw_picker(lv_layer_t* layer) {
    const auto& th = hud_theme_at(model_.theme);
    const float alpha = 1 - menu_progress_;
    const auto frame = canopy::screen_frame(width_, height_);
    for (int side = 0; side < 2; ++side) {
        const bool selected = side == model_.side;
        text_.panel(layer, side ? "AIR UNIT  >" : "<  GROUND UNIT", side == 0, 14,
                    407 + canopy::kTextState * 0.25f / frame.U,
                    canopy::kTextState, side != 0, selected ? th.accent : th.quiet, alpha);
        text_.panel(layer, side ? "VIDEO   RADIO   SERIAL   INFO" : "HUD   DISPLAY   DVR   SYSTEM", side == 0, 14,
                    427 + canopy::kTextUnit * 0.25f / frame.U,
                    canopy::kTextUnit, side != 0, selected ? th.text : th.quiet, alpha * (selected ? 0.85f : 0.6f));
    }
}
void View::draw_channel(lv_layer_t* layer) {
    const auto& scan=model_.backend.channels;
    const auto& theme=hud_theme_at(model_.theme);
    const float u=height_/1080.0f;
    const int top=height_*62/100, pad=int(28*u), bottom=height_-int(108*u);
    const float amber[]={.95f,.79f,.30f}, danger[]={.93f,.33f,.28f};
    rect(layer,0,top,width_,height_-top,theme.ground,.94f);
    auto label=[&](const std::string& s,float x,float y,int size,const float* rgb) {
        text_.canvas(layer,s,x,y,size*u,false,rgb,1);
    };
    label("CHANNEL",pad,top+40*u,28,theme.text);
    std::string selection=model_.channel_mhz?std::to_string(model_.channel_mhz)+" MHz":"AUTO";
    const ChannelReading* selected=nullptr;
    for(const auto& c:scan.readings) if(c.mhz==model_.channel_mhz) selected=&c;
    if(selected) selection+="   "+std::to_string(selected->dbm)+" dBm   "+selected->rule;
    label(selection,pad+220*u,top+40*u,23,theme.text);
    const float left=pad+100*u, right=width_-pad, pitch=(right-left)/std::max<size_t>(1,scan.readings.size());
    const int chart_top=top+int(78*u), chart_height=bottom-chart_top;
    auto bar_y=[&](int dbm) {return bottom-int(std::clamp((dbm+110)/60.0f,0.0f,1.0f)*chart_height);};
    label("AUTO",pad,bottom,20,model_.channel_mhz?theme.quiet:theme.accent);
    for(size_t i=0;i<scan.readings.size();++i) {
        const auto& c=scan.readings[i]; const int x=int(left+i*pitch), w=std::max(2,int(pitch)-4);
        const bool chosen=c.mhz==model_.channel_mhz;
        const float* hue=c.dbm<=-88?theme.data:c.dbm<=-80?amber:danger;
        rect(layer,x,bar_y(c.dbm),w,bottom-bar_y(c.dbm),hue,scan.fresh?.8f:.25f);
        rect(layer,x,bar_y(c.peak_dbm),w,2,theme.text,.65f);
        if(chosen) rect(layer,x-2,chart_top-4,w+4,chart_height+8,theme.accent,1,2);
        if(c.mhz==scan.current_mhz) label("LINK",x,chart_top-12*u,14,theme.text);
        if(chosen || i%std::max<size_t>(1,scan.readings.size()/12)==0)
            label(std::to_string(c.mhz),x,bottom+26*u,14,theme.quiet);
    }
    label("LEFT / RIGHT  Select     UP  Quietest     DOWN  Current     OK  Apply     BACK  Close",pad,height_-46*u,20,theme.text);
    label(!scan.fresh?"Scan stale; waiting for radio":model_.feedback,pad,height_-17*u,18,theme.quiet);
}
lv_area_t View::menu_reveal(float progress) const {
    const float p = std::clamp(progress, 0.0f, 1.0f), e = p * p * (3 - 2 * p);
    const float fh = std::tan(0.785f / 2) * 3.2f;
    const float top = (fh - canopy::screen_frame(width_, height_).MY(456)) / (2 * fh) * height_;
    const int y = int(std::lround(top + (height_ * 0.06f - top) * e));
    const int h = int(std::lround(height_ - top + (height_ * 0.92f - (height_ - top)) * e));
    const int w = int(std::lround(width_ * (0.31f + 0.03f * e)));
    const int x = active_menu_side_ ? width_ - w : 0;
    return {x, y, x + w - 1, y + h - 1};
}
void View::draw_menu(lv_layer_t* layer) {
    const auto& menu_model = menu_preview_ ? *menu_preview_ : model_;
    const auto& th = hud_theme_at(model_.theme);
    const auto reveal = menu_reveal(menu_progress_);
    const int x = reveal.x1, y = reveal.y1, w = reveal.x2 - x + 1, h = reveal.y2 - y + 1;
    const float u = height_ / 1080.0f;
    const int pad = int(22 * u), row_h = int(50 * u);
    if (!capturing_menu_ && menu_snapshot_ && menu_progress_ == 1) {
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = menu_snapshot_;
        lv_area_t image_area{active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, 0, 0, height_ - 1};
        image_area.x2 = image_area.x1 + int(menu_snapshot_->header.w) - 1;
        lv_draw_image(layer, &d, &image_area);
        return;
    }
    rect(layer, x, y, w, h, th.ground, 0.94f * std::min(1.0f, menu_progress_ * 1.6f));
    rect(layer, x, y, w, std::max(1, int(3 * u)), th.accent, menu_progress_);
    if (menu_progress_ <= 0.5f) return;
    const float content_alpha = (menu_progress_ - 0.5f) * 2;
    text_.canvas(layer, active_menu_side_ ? "AIR UNIT" : "GROUND UNIT", pad, pad + 14 * u, 15 * u, false, th.accent, content_alpha, x, y);
    int tab_x = pad;
    const auto tabs = menu_model.side_tabs();
    for (int tab : tabs) {
        const std::string title = Model::tab_name(tab);
        const int tw = int(text_.advance(title) * 21 * u / canopy::kFontPixels);
        text_.canvas(layer, title, tab_x, pad + 52 * u, 21 * u, false,
                      tab == menu_model.tab ? th.text : th.quiet, content_alpha, x, y);
        if (tab == menu_model.tab) rect(layer, x + tab_x, y + pad + int(61 * u), tw, std::max(1, int(3 * u)), th.accent, content_alpha);
        tab_x += tw + int(26 * u);
    }
    const int list_top = pad + int(78 * u);
    rect(layer, x + pad, y + list_top - int(6 * u), w - 2 * pad, 1, th.quiet, 0.3f);
    const auto rows = menu_model.rows(now_);
    const bool editing = menu_model.stage == MenuStage::Edit;
    const int extra = editing ? int(80 * u) : 0;
    std::vector<std::string> help_lines;
    std::istringstream paragraphs(menu_model.selected_help(now_));
    std::string paragraph;
    while (std::getline(paragraphs, paragraph) && help_lines.size() < 10) {
        std::istringstream words(paragraph); std::string word, line;
        while (words >> word) {
            const auto candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_.advance(candidate) * 18 * u / canopy::kFontPixels > w - 2 * pad) {
                help_lines.push_back(line); line.clear();
                if (help_lines.size() >= 10) break;
            }
            line = line.empty() ? word : line + " " + word;
        }
        if (!line.empty() && help_lines.size() < 10) help_lines.push_back(line);
    }
    const float help_y = h - pad - 18 * 1.4f * u * help_lines.size();
    const float list_bottom = help_y - 20 * u;
    const int visible = std::clamp(int((list_bottom - list_top - extra) / row_h), 3, 16);
    const int start = std::clamp(menu_model.selected - (visible - 1) / 2, 0, std::max(0, int(rows.size()) - visible));
    const float lit[] = {std::min(1.0f, th.ground[0] * 1.6f), std::min(1.0f, th.ground[1] * 1.6f), std::min(1.0f, th.ground[2] * 1.6f)};
    int ry = list_top;
    for (int i = start; i < std::min(int(rows.size()), start + visible); ++i) {
        if (rows[i].heading) {
            text_.canvas(layer, rows[i].name, pad, ry + row_h * 0.72f, 15 * u, false, th.quiet, 0.9f * content_alpha, x, y);
            rect(layer, x + pad, y + ry + int(row_h * 0.86f), w - 2 * pad, 1, th.quiet, 0.3f * content_alpha);
            ry += row_h; continue;
        }
        const bool selected = i == menu_model.selected;
        const bool expanded = selected && editing;
        const int rh = row_h + (expanded ? extra : 0);
        if (selected) {
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, lit, 0.95f * content_alpha);
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, th.accent, content_alpha, std::max(1, int(3 * u)));
        }
        const float baseline = ry + row_h * 0.5f + 23 * u * 0.36f;
        text_.canvas(layer, rows[i].name, pad, baseline, 23 * u, false, rows[i].disabled ? th.quiet : th.text, content_alpha * (selected || rows[i].disabled ? 1 : 0.82f), x, y);
        if (expanded) {
            const int current = menu_model.option_index(), n = int(rows[i].options.size());
            for (int delta = -1; delta <= 1; ++delta) {
                const int choice = current + delta;
                if (choice < 0 || choice >= n) continue;
                const float size = (delta == 0 ? 30 : 18) * u;
                text_.canvas(layer, rows[i].options[choice], w - pad, ry + rh * 0.5f + delta * 34 * u + size * 0.36f,
                             size, true, delta == 0 ? th.accent : th.quiet, content_alpha * (delta == 0 ? 1 : 0.7f), x, y);
            }
            ry += rh; continue;
        }
        const float available = w - 2 * pad - text_.advance(rows[i].name) * 23 * u / canopy::kFontPixels - 16 * u;
        const float value_width = text_.advance(rows[i].value) * 20 * u / canopy::kFontPixels;
        const float size = value_width > available && available > 0 ? std::max(12 * u, 20 * u * available / value_width) : 20 * u;
        text_.canvas(layer, rows[i].value + (menu_model.pending(rows[i].id) ? " *" : ""), w - pad, baseline, size, true,
                     rows[i].disabled ? th.quiet : selected ? th.accent : rows[i].editable ? th.text : th.quiet, content_alpha * (selected ? 1 : 0.85f), x, y);
        ry += rh;
    }
    if (!help_lines.empty()) rect(layer, x + pad, y + int(help_y - 10 * u), w - 2 * pad, 1, th.quiet, 0.35f * content_alpha);
    const float help_color[] = {th.text[0] * 0.7f, th.text[1] * 0.7f, th.text[2] * 0.7f};
    for (size_t i = 0; i < help_lines.size(); ++i)
        text_.canvas(layer, help_lines[i], pad, help_y + 18 * 1.4f * u * i + 18 * 0.9f * u,
                     18 * u, false, help_color, content_alpha, x, y);
    auto arrow = [&](float cy, bool up) {
        lv_draw_triangle_dsc_t tri; lv_draw_triangle_dsc_init(&tri);
        const int cx = x + w / 2, sy = y + int(cy), size = std::max(1, int(6 * u));
        tri.p[0] = {cx - size, sy + (up ? size / 2 : -size / 2)};
        tri.p[1] = {cx + size, tri.p[0].y}; tri.p[2] = {cx, sy + (up ? -size / 2 : size / 2)};
        tri.bg_color = color(th.quiet); tri.bg_opa = opacity(0.8f * content_alpha); lv_draw_triangle(layer, &tri);
    };
    if (start > 0) arrow(list_top + 2 * u, true);
    if (start + visible < int(rows.size())) arrow(list_bottom + 6 * u, false);

}
void View::update(uint64_t now) {
    text_.begin_frame(); idle_.prepare(model_, now);
    now_ = now;
    const bool bf=model_.bf_osd && model_.hud_visible() && model_.backend.serial.fresh(now);
    if(bf!=bf_visible_ || (bf && bf_grid_!=model_.backend.bf_grid)) {
        bf_visible_=bf;
        for(int row=0;row<20;++row) for(int col=0;col<53;++col) {
            const int i=row*53+col;
            const auto glyph=bf?model_.backend.bf_grid[i]:0;
            if(glyph==bf_grid_[i])continue;
            bf_grid_[i]=glyph;
            lv_area_t area{(6+col*36)*width_/1920,row*54*height_/1080,(6+(col+1)*36)*width_/1920,(row+1)*54*height_/1080};
            lv_obj_invalidate_area(bf_object_,&area);
        }
    }
    const auto debug=model_.debug && model_.hud_visible() && !model_.menu_open?model_.backend.radio_debug:std::string();
    if(debug!=debug_text_ || old_theme_!=model_.theme) {debug_text_=debug;lv_obj_invalidate(debug_object_);}
    auto clock=model_.clock_text(std::time(nullptr),now);
    if(model_.backend.media.recording) clock=clock.empty()?"REC":"REC  "+clock;
    if(clock!=clock_text_ || old_theme_!=model_.theme) {
        clock_text_=clock; lv_obj_invalidate(clock_);
    }
    hud_->update(controller_.hud_data(now), now);
    sync_hud_objects();
    const bool hud_changed = hud_revision_ != hud_->revision();
    const float pixels_per_world=height_/(2*std::tan(.785f/2)*3.2f);
    const bool motion=model_.dynamic && !model_.menu_open;
    const std::array<int,3> transform{motion?int(std::lround(model_.backend.hud_dx*pixels_per_world)):0,
        motion?-int(std::lround(model_.backend.hud_dy*pixels_per_world)):0,
        motion?int(std::lround(-model_.backend.hud_bank*1800/3.14159265f)):0};
    if(transform!=motion_transform_) {
        for(auto* object:hud_objects_) {
            if(transform[0]!=motion_transform_[0]) lv_obj_set_style_translate_x(object,transform[0],0);
            if(transform[1]!=motion_transform_[1]) lv_obj_set_style_translate_y(object,transform[1],0);
            if(transform[2]!=motion_transform_[2]) lv_obj_set_style_transform_rotation(object,transform[2],0);
        }
        motion_transform_=transform;
        // LVGL's software transform samples relative to its clipped layer.
        // Keep the sampling origin stable while a rotated HUD moves; static
        // HUD and menu navigation retain their bounded damage path.
        lv_obj_invalidate(screen_);
    }
    if(hud_changed && transform[2]) lv_obj_invalidate(screen_);
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const bool unfolded = model_.menu_open && model_.stage != MenuStage::Pick;
    // Do not rebuild on telemetry changes during motion. A bounded snapshot
    // is refreshed in the picker or when the fully opened content changes.
    // Keep the outgoing blade intact during closure, even though the model has
    // already returned to Pick (or the user changes the next side quickly).
    if (!model_.channel_open && model_.menu_open && (menu_progress_ == 0 || (unfolded && menu_progress_ == 1))) prepare_menu_snapshot();
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
    if (moving || old_tab_ != model_.tab || model_.menu_version != menu_version_) lv_obj_set_pos(menu_, mx, 0);
    hidden(menu_, next <= 0 || model_.channel_open);
    // The picker fades back in as the blade folds into the canopy.
    hidden(picker_, model_.channel_open || menu_progress_ == 1 || (!model_.menu_open && menu_progress_ == 0));
    hidden(channel_,!model_.channel_open);
    if(model_.channel_open && (old_channel_!=model_.channel_open || channel_revision_!=model_.backend.channels.revision ||
                              menu_version_!=model_.menu_version || old_theme_!=model_.theme)) lv_obj_invalidate(channel_);
    old_channel_=model_.channel_open; channel_revision_=model_.backend.channels.revision;
    const auto& t = model_.backend.vrx;
    const auto& flight = model_.backend.flight;
    const bool flight_fresh = flight.stamp_us && now >= flight.stamp_us && now - flight.stamp_us < 2000000;
    const bool fresh = t.fresh(now), valid = t.link_valid(now);
    const bool hud_visible = model_.hud_visible();
    for (size_t i = 0; i < hud_objects_.size(); ++i)
        hidden(hud_objects_[i], !hud_visible || hud_layout_.regions[i].role == hud_ipc::Role::Status);
    const bool appearance = !initialized_ || old_theme_ != model_.theme || old_fresh_ != fresh || old_valid_ != valid || old_details_ != model_.show_details || old_hud_visible_ != hud_visible;
    const bool data = !initialized_ || model_.backend.flight_version != flight_version_ || flight_fresh != old_flight_fresh_ ||
        t.connected != presented_.connected || t.mcs != presented_.mcs ||
        t.frequency_mhz != presented_.frequency_mhz || t.frequency_raw != presented_.frequency_raw || t.distance_raw != presented_.distance_raw ||
        t.quality_raw != presented_.quality_raw || t.voltage_mv != presented_.voltage_mv ||
        t.cpu_temperature_x100 != presented_.cpu_temperature_x100 || t.rf_temperature_raw != presented_.rf_temperature_raw;
    const bool menu = old_menu_ != model_.menu_open || old_tab_ != model_.tab || old_selected_ != model_.selected || model_.menu_version != menu_version_;
    const bool counters = t.status_frames != presented_.status_frames || t.unknown_frames != presented_.unknown_frames ||
                          t.bad_frames != presented_.bad_frames || model_.rejected_inputs != rejected_inputs_;
    if (appearance || (!moving && menu) || (moving && next == 0) ||
        (!moving && !menu_snapshot_ && (data || (model_.menu_open && (model_.tab == 2 || model_.tab == 4 || model_.tab == 6) && counters))))
        lv_obj_invalidate(menu_);
    else if (moving) {
        // The content translates and fades with the blade, so the whole union
        // changes (not just its edges). Repaint its old and new bounds.
        lv_obj_invalidate_area(menu_, &old_reveal);
        const auto reveal = menu_reveal(next); lv_obj_invalidate_area(menu_, &reveal);
    }
    if (appearance || menu || moving) lv_obj_invalidate(picker_);
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
    const bool status_visible = model_.show_status(now);
    // Static artwork stays centered behind either menu; only visibility/theme or
    // acquisition text changes invalidate it.
    if (appearance || status_visible != old_status_ || (valid && old_menu_ != model_.menu_open)) lv_obj_invalidate(idle_object_);
    if (appearance || moving || old_menu_ != model_.menu_open) lv_obj_invalidate(dimmer_);
    old_status_ = status_visible;
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
