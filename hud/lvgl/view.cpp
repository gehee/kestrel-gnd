#include "view.hpp"
#include "hud/hud_theme.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sstream>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace kestrel_ui {
namespace {
lv_color_t color(const float* rgb) { return lv_color_make(uint8_t(rgb[0] * 255), uint8_t(rgb[1] * 255), uint8_t(rgb[2] * 255)); }
lv_opa_t opacity(float v) { return lv_opa_t(std::clamp(v * 255, 0.0f, 255.0f)); }
int equal_prefix(const uint32_t* a,const uint32_t* b,int width) {
    int x=0;
#ifdef __aarch64__
    for(;x+16<=width;x+=16) {
        auto different=vorrq_u32(veorq_u32(vld1q_u32(a+x),vld1q_u32(b+x)),veorq_u32(vld1q_u32(a+x+4),vld1q_u32(b+x+4)));
        different=vorrq_u32(different,vorrq_u32(veorq_u32(vld1q_u32(a+x+8),vld1q_u32(b+x+8)),veorq_u32(vld1q_u32(a+x+12),vld1q_u32(b+x+12))));
        if(vmaxvq_u32(different))break;
    }
#endif
    while(x<width && a[x]==b[x])++x;
    return x;
}
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
    // Bake native-size alpha strips once. Pulsing changes only their opacity;
    // it neither regenerates glyphs nor asks the HUD worker to redraw wings.
    const int depth = h * 15 / 100;
    for (int side = 0; side < 2; ++side) {
        auto& pixels = edge_pixels_[side]; pixels.resize(size_t(w) * depth);
        for (int y = 0; y < depth; ++y) {
            const float t = float(side ? depth - 1 - y : y) / std::max(1, depth - 1);
            std::fill_n(pixels.data() + size_t(y) * w, w, uint8_t(255 * (1-t) * (1-t)));
        }
        auto& image = edge_images_[side]; image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_A8; image.header.w = w; image.header.h = depth;
        image.header.stride = w; image.data_size = pixels.size(); image.data = pixels.data();
        auto* object = edge_objects_[side] = lv_obj_create(screen_);
        lv_obj_remove_style_all(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(object, 0, side ? h-depth : 0); lv_obj_set_size(object, w, depth);
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
    lv_obj_set_pos(clock_,w/3,0);
    lv_obj_set_size(clock_,w/3,h/12);
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
    gallery_=lv_obj_create(screen_);lv_obj_remove_style_all(gallery_);
    lv_obj_set_size(gallery_,w,h);lv_obj_remove_flag(gallery_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(gallery_,draw_event,LV_EVENT_DRAW_MAIN,this);
    lv_obj_add_flag(gallery_,LV_OBJ_FLAG_HIDDEN);
    capture_screen_=lv_obj_create(nullptr);lv_obj_remove_style_all(capture_screen_);
    menu_capture_=lv_obj_create(capture_screen_);lv_obj_remove_style_all(menu_capture_);
    lv_obj_remove_flag(menu_capture_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_capture_,draw_event,LV_EVENT_DRAW_MAIN,this);
}
View::~View() { for(auto& image:thumbnail_images_){lv_image_cache_drop(&image);lv_image_header_cache_drop(&image);} lv_obj_delete(capture_screen_);lv_obj_clean(screen_); drop_menu_snapshot(); }
void View::draw_edge(lv_layer_t* layer, int side) {
    if (!edge_opacity_) return;
    lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d);
    d.src = &edge_images_[side]; d.recolor_opa = LV_OPA_COVER;
    d.recolor = edge_level_ == 2 ? lv_color_make(255,76,63) : lv_color_make(255,183,38);
    d.opa = edge_opacity_;
    const int depth = edge_images_[side].header.h, y = side ? height_-depth : 0;
    lv_area_t area{0,y,width_-1,y+depth-1}; lv_draw_image(layer,&d,&area);
}
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
    signature << menu.side << ':' << menu.tab << ':' << menu.selected << ':' << menu.theme << ':' << menu.ui_scale << ':'
              << (menu.stage == MenuStage::Edit) << ':' << menu.selected_help(now_);
    for (const auto& row : menu.rows(now_)) {
        signature << '\n' << row.id << ':' << row.name << ':' << row.value << ':' << row.disabled << row.editable << row.action << row.heading << menu.pending(row.id);
        for(const auto& option:row.options)signature<<':'<<option;
    }
    const std::string key = signature.str();
    if (menu_snapshot_ && key == menu_snapshot_signature_) {
        // Several queued keys can return to the same selection in one tick.
        // A version change alone must not repaint an unchanged cached blade.
        menu_damage_applied_=true;
        return;
    }
    const float progress = menu_progress_;
    const int active_side = active_menu_side_;
    active_menu_side_ = model_.side; menu_progress_ = 1; capturing_menu_ = true;
    lv_obj_set_pos(menu_, active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, 0);
    // Stable browsing changes only selected/value rows and their help text.
    // Patch those bands into the cached blade. Scrolling, editing and geometry
    // changes use a fresh capture, with exact pixel damage determined below.
    if(menu_snapshot_ && menu_cached_model_ && menu_cached_layout_ && progress==1 && menu.stage==MenuStage::Browse &&
       menu_cached_model_->stage==MenuStage::Browse && menu.side==menu_cached_model_->side &&
       menu.tab==menu_cached_model_->tab && menu.theme==menu_cached_model_->theme &&
       menu.ui_scale==menu_cached_model_->ui_scale && menu.side_tabs()==menu_cached_model_->side_tabs()) {
        const auto reveal=menu_reveal(1);
        const int w=reveal.x2-reveal.x1+1,h=reveal.y2-reveal.y1+1;
        const auto& before=*menu_cached_layout_;
        const auto after=menu_layout(menu,w,h);
        if(before.start==after.start && before.rows.size()==after.rows.size() &&
           std::min(int(before.rows.size()),before.start+before.visible)==std::min(int(after.rows.size()),after.start+after.visible)) {
            std::vector<lv_area_t> bands;
            for(int i=after.start;i<std::min(int(after.rows.size()),after.start+after.visible);++i) {
                const auto& a=before.rows[i];const auto& b=after.rows[i];
                const bool changed=(i==menu.selected)!=(i==menu_cached_model_->selected) ||
                    std::tie(a.id,a.name,a.value,a.heading,a.disabled,a.editable,a.action)!=std::tie(b.id,b.name,b.value,b.heading,b.disabled,b.editable,b.action) ||
                    menu.pending(b.id)!=menu_cached_model_->pending(a.id);
                if(changed) {
                    const int top=reveal.y1+after.list_top+(i-after.start)*after.row_h;
                    bands.push_back({reveal.x1,std::max(0,top-2),reveal.x2,std::min(height_-1,top+after.row_h+2)});
                }
            }
            if(before.help_lines!=after.help_lines) bands.push_back({reveal.x1,
                std::max(0,reveal.y1+int(std::floor(std::min(before.help_y,after.help_y)-24*after.u))),reveal.x2,reveal.y2});
            bool ok=true;
            capture_layout_=&after;
            lv_image_cache_drop(menu_snapshot_);
            for(const auto& a:bands) {
                lv_obj_set_pos(menu_capture_,a.x1,a.y1);lv_obj_set_size(menu_capture_,w,a.y2-a.y1+1);
                auto* part=lv_snapshot_take(menu_capture_,LV_COLOR_FORMAT_ARGB8888);
                if(!part) {ok=false;break;}
                for(int row=0;row<int(part->header.h);++row)
                    std::memcpy(menu_snapshot_->data+(a.y1+row)*menu_snapshot_->header.stride,
                                part->data+row*part->header.stride,w*4);
                lv_draw_buf_destroy(part);lv_obj_invalidate_area(menu_,&a);
            }
            capture_layout_=nullptr;
            if(ok) {
                menu_cached_layout_=after;
                menu_cached_model_=menu;menu_snapshot_signature_=key;++menu_snapshot_count_;menu_damage_applied_=true;
                capturing_menu_=false;menu_progress_=progress;active_menu_side_=active_side;return;
            }
        }
    }
    auto* previous_snapshot=menu_snapshot_;
    // Preserve the original translucent panel over the video. Cache only the
    // settled layout; moving panels are laid out in their interpolated bounds.
    const auto bounds=menu_reveal(1);
    const auto layout=menu_layout(menu,bounds.x2-bounds.x1+1,bounds.y2-bounds.y1+1);
    capture_layout_=&layout;
    menu_snapshot_ = lv_snapshot_take(menu_, LV_COLOR_FORMAT_ARGB8888);
    capture_layout_=nullptr;
    capturing_menu_ = false; menu_progress_ = progress; active_menu_side_ = active_side;
    lv_obj_set_pos(menu_, active_side ? width_ - lv_obj_get_width(menu_) : 0, 0);
    if (menu_snapshot_) {
        menu_cached_model_=menu;menu_cached_layout_=layout;
        menu_snapshot_signature_ = key; ++menu_snapshot_count_;
        if(previous_snapshot && previous_snapshot->header.w==menu_snapshot_->header.w &&
           previous_snapshot->header.h==menu_snapshot_->header.h) {
            // A selection/value change usually affects two rows and the help
            // text. Retain all other pixels instead of repainting the blade.
            std::vector<lv_area_t> damage;
            const int w=menu_snapshot_->header.w,h=menu_snapshot_->header.h;
            const int ox=lv_obj_get_x(menu_),oy=lv_obj_get_y(menu_);
            for(int row=0;row<h;++row) {
                const auto* old=reinterpret_cast<const uint32_t*>(previous_snapshot->data+row*previous_snapshot->header.stride);
                const auto* fresh=reinterpret_cast<const uint32_t*>(menu_snapshot_->data+row*menu_snapshot_->header.stride);
                const int left=equal_prefix(old,fresh,w);if(left==w)continue;
                int right=w-1;
                while(right>left && old[right]==fresh[right])--right;
                lv_area_t a{ox+left,oy+row,ox+right,oy+row};
                if(!damage.empty() && a.y1<=damage.back().y2+8) {
                    auto& last=damage.back();last.x1=std::min(last.x1,a.x1);last.x2=std::max(last.x2,a.x2);last.y2=a.y2;
                } else damage.push_back(a);
            }
            if(damage.size()>24)lv_obj_invalidate(menu_);
            else for(const auto& a:damage)lv_obj_invalidate_area(menu_,&a);
            menu_damage_applied_=true;
        } else lv_obj_invalidate(menu_);
    }
    if(previous_snapshot) {
        lv_image_cache_drop(previous_snapshot);lv_image_header_cache_drop(previous_snapshot);lv_draw_buf_destroy(previous_snapshot);
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
    else if (target == self->edge_objects_[0] || target == self->edge_objects_[1])
        self->draw_edge(layer, target == self->edge_objects_[0] ? 0 : 1);
    else if (target == self->menu_ || target == self->menu_capture_) self->draw_menu(layer);
    else if (target == self->picker_) self->draw_picker(layer);
    else if (target == self->channel_) self->draw_channel(layer);
    else if (target == self->clock_) {
        const float size=24.0f*self->height_/1080;
        const float x=(self->width_-self->text_.canvas_advance(self->clock_text_,size))/2;
        self->text_.canvas(layer,self->clock_text_,x,36.0f*self->height_/1080,size,false,hud_theme_at(self->model_.theme).text,.9f);
        if (!self->recording_text_.empty()) {
            constexpr float red[] = {1,.30f,.25f};
            const float rx=(self->width_-self->text_.canvas_advance(self->recording_text_,size))/2;
            const float y=(self->clock_text_.empty()?36.0f:66.0f)*self->height_/1080;
            const int marker=std::max(4,self->height_/108);
            rect(layer,int(rx)-marker*2,int(y)-marker,marker,marker,red,1);
            self->text_.canvas(layer,self->recording_text_,rx,y,size,false,red,1);
        }
    }
    else if(target==self->bf_object_ && self->bf_visible_) self->bf_atlas_.draw(layer,self->bf_grid_,self->width_,self->height_);
    else if(target==self->debug_object_) self->text_.canvas(layer,self->debug_text_,self->width_-18,self->height_-12,
        18.0f*self->height_/1080,true,hud_theme_at(self->model_.theme).quiet,1);
    else if(target==self->gallery_) self->draw_gallery(layer);
    else for (size_t i = 0; i < self->hud_objects_.size(); ++i) if (target == self->hud_objects_[i]) { self->draw_hud(layer, i); break; }
}
void View::sync_hud_objects() {
    if (hud_layout_ == hud_->layout()) return;
    for (auto* object : hud_objects_) lv_obj_delete(object);
    hud_objects_.clear(); hud_layout_ = hud_->layout(); motion_offset_={};
    for (size_t i = 0; i < hud_layout_.regions.size(); ++i) {
        const auto& r = hud_layout_.regions[i]; auto* object = lv_obj_create(screen_);
        lv_obj_remove_style_all(object); lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
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
void View::draw_gallery(lv_layer_t* layer) {
    const auto& theme=hud_theme_at(model_.theme);const float u=height_/1080.0f;
    const bool playing=model_.scene==SceneMode::GalleryPlayback;
    auto label=[&](const std::string& s,float x,float y,float size,const float* rgb) {
        text_.canvas(layer,s,x*u,y*u,size*u,false,rgb,1);
    };
    if(playing) {
        rect(layer,0,0,width_,int(140*u),theme.ground,.88f);
        label(model_.backend.playback_seeking?"SEEKING":model_.backend.playback_paused?"PAUSED":"PLAYBACK",32,42,26,theme.accent);
        label(model_.backend.media.playback_id,240,42,22,theme.text);
        const auto elapsed=model_.backend.playback_position_us/1000000,total=model_.backend.playback_duration_us/1000000;
        label(std::to_string(elapsed)+" / "+std::to_string(total)+" s   LEFT/RIGHT: keyframe   OK: pause/resume   BACK: gallery   DOWN: bar   UP: HUD " + std::string(!model_.backend.playback_hud?"UNAVAILABLE":model_.playback_hud_enabled?"ON":"OFF"),32,82,20,theme.text);
        const int left=int(32*u),right=width_-left;
        const auto duration=model_.backend.playback_duration_us;
        const double progress=duration?double(std::min(model_.backend.playback_position_us,duration))/duration:0;
        const int head=left+int((right-left)*progress);
        rect(layer,left,int(108*u),right-left,int(6*u),theme.quiet,.65f);
        if(head>left)rect(layer,left,int(108*u),head-left,int(6*u),theme.accent,1);
        rect(layer,std::clamp(head-int(3*u),left,right-int(6*u)),int(101*u),int(6*u),int(20*u),theme.text,1);
        if(!model_.backend.playback_error.empty())label(model_.backend.playback_error,32,175,24,theme.accent);
        else if(!model_.backend.playback_telemetry_error.empty())label("Recorded HUD unavailable",32,175,24,theme.accent);
        return;
    }
    rect(layer,0,0,width_,height_,theme.ground,.95f);
    label("RECORDINGS",48,70,36,theme.accent);
    const auto& clips=model_.backend.recordings;
    if(clips.empty())label("No recordings in this folder",48,180,28,theme.text);
    const int selected=std::clamp(model_.gallery_selected,0,std::max(0,int(clips.size())-1));
    const int start=selected/6*6;
    for(int i=start;i<std::min(start+6,int(clips.size()));++i) {
        const int slot=i-start;const int x=48+(slot%2)*920,y=130+(slot/2)*265;
        rect(layer,int(x*u),int(y*u),int(880*u),int(230*u),theme.quiet,.12f);
        if(i==selected)rect(layer,int(x*u),int(y*u),int(880*u),int(230*u),theme.accent,1,2);
        if(thumbnails_[slot]) {
            lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=&thumbnail_images_[slot];
            const auto& image=*thumbnails_[slot];
            lv_area_t area{int((x+20)*u),int((y+25)*u),int((x+20)*u)+image.width-1,int((y+25)*u)+image.height-1};
            lv_draw_image(layer,&d,&area);
        } else label("NO PREVIEW",x+40,y+122,20,theme.quiet);
        label(clips[i].name,x+365,y+62,19,theme.text);
        label(std::to_string(clips[i].bytes/(1024*1024))+" MiB",x+365,y+122,23,theme.quiet);
        label("OK  PLAY",x+365,y+190,20,i==selected?theme.accent:theme.quiet);
    }
    label("ARROWS  Select     OK  Play     REC  Actions     BACK  Live video",48,1000,24,theme.text);
    label(model_.feedback,48,1045,20,theme.quiet);
    if(model_.backend.export_busy) {
        rect(layer,0,0,width_,height_,theme.ground,.65f);
        rect(layer,int(390*u),int(325*u),int(1140*u),int(430*u),theme.ground,1);
        rect(layer,int(390*u),int(325*u),int(1140*u),int(430*u),theme.accent,1,2);
        label("EXPORT WITH HUD",440,390,32,theme.text);
        auto name=model_.backend.export_file;
        if(name.size()>64)name=name.substr(0,61)+"...";
        label(name,440,450,24,theme.text);
        label(model_.backend.export_message,440,510,22,theme.quiet);
        const auto progress=std::clamp(model_.backend.export_percent,0,100);
        rect(layer,int(440*u),int(565*u),int(940*u),int(10*u),theme.quiet,.5f);
        if(progress)rect(layer,int(440*u),int(565*u),int(940*u*progress/100),int(10*u),theme.accent,1);
        label(std::to_string(progress)+"%",1400,580,24,theme.text);
        label("BACK  Cancel export",440,710,24,theme.text);
        return;
    }
    if(!model_.gallery_action_name.empty() && model_.gallery_delete_name.empty()) {
        rect(layer,0,0,width_,height_,theme.ground,.65f);
        rect(layer,int(390*u),int(300*u),int(1140*u),int(480*u),theme.ground,1);
        rect(layer,int(390*u),int(300*u),int(1140*u),int(480*u),theme.accent,1,2);
        label("RECORDING ACTIONS",440,365,32,theme.text);
        const auto& name=model_.gallery_action_name;
        label(name.size()>70?name.substr(0,32)+"..."+name.substr(name.size()-32):name,440,420,24,theme.text);
        for(int i=0;i<2;++i) {
            const bool chosen=i==int(model_.gallery_action);
            const bool available=i || model_.recording_export_available();
            const int y=455+i*95;
            rect(layer,int(440*u),int(y*u),int(1040*u),int(80*u),chosen?theme.accent:theme.quiet,1,chosen?3:1);
            label(i?"DELETE":"EXPORT WITH HUD",470,y+52,28,available?theme.text:theme.quiet);
            if(!available)label("UNAVAILABLE",1180,y+50,20,theme.quiet);
        }
        if(!model_.feedback.empty())label(model_.feedback,440,695,20,theme.quiet);
        label("ARROWS  Select     OK  Choose     BACK / REC  Close",440,750,20,theme.text);
    }
    if(!model_.gallery_delete_name.empty()) {
        rect(layer,0,0,width_,height_,theme.ground,.65f);
        rect(layer,int(390*u),int(325*u),int(1140*u),int(430*u),theme.ground,1);
        rect(layer,int(390*u),int(325*u),int(1140*u),int(430*u),theme.accent,1,2);
        label("Delete this recording?",440,390,32,theme.text);
        const auto& name=model_.gallery_delete_name;
        label(name.size()>70?name.substr(0,32)+"..."+name.substr(name.size()-32):name,440,450,24,theme.text);
        label("Video, thumbnail and recorded HUD data will be removed.",440,505,20,theme.quiet);
        for(int i=0;i<2;++i) {
            const bool chosen=bool(i)==model_.gallery_delete_chosen;
            const int x=440+i*560;
            rect(layer,int(x*u),int(560*u),int(480*u),int(85*u),chosen?theme.accent:theme.quiet,1,chosen?3:1);
            label(i?"DELETE":"CANCEL",x+155,615,28,chosen?theme.accent:theme.text);
        }
        label("LEFT / RIGHT  Choose     OK  Confirm     BACK  Cancel",440,710,20,theme.text);
    }
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
View::MenuLayout View::menu_layout(const Model& menu_model,int w,int h) const {
    const float u=height_/1080.0f*menu_model.ui_scale/10.0f;
    const int pad=int(22*u),row_h=int(50*u),list_top=pad+int(78*u);
    const auto rows=menu_model.rows(now_);
    const int extra=menu_model.stage==MenuStage::Edit?int(80*u):0;
    std::vector<std::string> help_lines;
    std::istringstream paragraphs(menu_model.selected_help(now_));
    std::string paragraph;
    while (std::getline(paragraphs, paragraph) && help_lines.size() < 10) {
        std::istringstream words(paragraph); std::string word, line;
        while (words >> word) {
            const auto candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_.canvas_advance(candidate,18*u) > w - 2 * pad) {
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
    return {u,help_y,pad,row_h,list_top,extra,start,visible,rows,help_lines};
}
void View::draw_menu(lv_layer_t* layer) {
    const auto& menu_model = menu_preview_ ? *menu_preview_ : model_;
    const auto& th = hud_theme_at(model_.theme);
    const auto reveal = menu_reveal(menu_progress_);
    const int x = reveal.x1, y = reveal.y1, w = reveal.x2 - x + 1, h = reveal.y2 - y + 1;
    if (!capturing_menu_ && menu_snapshot_ && menu_progress_ == 1) {
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = menu_snapshot_;
        lv_area_t image_area{active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, 0, 0, height_ - 1};
        image_area.x2 = image_area.x1 + int(menu_snapshot_->header.w) - 1;
        lv_draw_image(layer, &d, &image_area);
        return;
    }
    const auto layout_value=capture_layout_?MenuLayout{}:menu_layout(menu_model,w,h);
    const auto& layout=capture_layout_?*capture_layout_:layout_value;
    const float u=layout.u;
    const int pad=layout.pad,row_h=layout.row_h;
    rect(layer, x, y, w, h, th.ground, 0.94f * std::min(1.0f, menu_progress_ * 1.6f));
    rect(layer, x, y, w, std::max(1, int(3 * u)), th.accent, menu_progress_);
    if (menu_progress_ <= 0.5f) return;
    const float content_alpha = (menu_progress_ - 0.5f) * 2;
    text_.canvas(layer, active_menu_side_ ? "AIR UNIT" : "GROUND UNIT", pad, pad + 14 * u, 15 * u, false, th.accent, content_alpha, x, y);
    int tab_x = pad;
    const auto tabs = menu_model.side_tabs();
    for (int tab : tabs) {
        const std::string title = Model::tab_name(tab);
        const int tw = int(text_.canvas_advance(title,21*u));
        text_.canvas(layer, title, tab_x, pad + 52 * u, 21 * u, false,
                      tab == menu_model.tab ? th.text : th.quiet, content_alpha, x, y);
        if (tab == menu_model.tab) rect(layer, x + tab_x, y + pad + int(61 * u), tw, std::max(1, int(3 * u)), th.accent, content_alpha);
        tab_x += tw + int(26 * u);
    }
    const int list_top=layout.list_top;
    rect(layer, x + pad, y + list_top - int(6 * u), w - 2 * pad, 1, th.quiet, 0.3f);
    const auto& rows=layout.rows;
    const bool editing = menu_model.stage == MenuStage::Edit;
    const int extra=layout.extra;
    const auto& help_lines=layout.help_lines;
    const float help_y=layout.help_y,list_bottom=help_y-20*u;
    const int start=layout.start,visible=layout.visible;
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
        const float available = w - 2 * pad - text_.canvas_advance(rows[i].name,23*u) - 16 * u;
        const float value_width = text_.canvas_advance(rows[i].value,20*u);
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
    menu_damage_applied_=false;
    text_.begin_frame(); idle_.prepare(model_, now);
    now_ = now;
    const bool gallery=model_.scene==SceneMode::GalleryBrowse ||
        (model_.scene==SceneMode::GalleryPlayback && model_.playback_controls_visible);
    hidden(gallery_,!gallery);
    if(gallery) {
        const int first=std::max(0,model_.gallery_selected)/6*6;
        for(int slot=0;slot<6;++slot) {
            const auto image=first+slot<int(model_.backend.recordings.size())?model_.backend.recordings[first+slot].thumbnail:nullptr;
            if(image==thumbnails_[slot])continue;
            lv_image_cache_drop(&thumbnail_images_[slot]);lv_image_header_cache_drop(&thumbnail_images_[slot]);
            thumbnails_[slot]=image;auto& d=thumbnail_images_[slot];d={};
            if(image) {
                d.header.magic=LV_IMAGE_HEADER_MAGIC;d.header.cf=LV_COLOR_FORMAT_ARGB8888;
                d.header.w=image->width;d.header.h=image->height;d.header.stride=image->width*4;
                d.data_size=image->argb.size()*4;d.data=reinterpret_cast<const uint8_t*>(image->argb.data());
            }
            lv_obj_invalidate(gallery_);
        }
        std::ostringstream signature;
        signature<<int(model_.scene)<<':'<<model_.gallery_selected<<':'<<model_.theme<<':'<<model_.feedback<<':'
            <<model_.backend.export_busy<<':'<<model_.backend.export_percent<<':'<<model_.backend.export_file<<':'<<model_.backend.export_message<<':'
            <<model_.gallery_action_name<<':'<<int(model_.gallery_action)<<':'<<model_.recording_export_available()<<':'
            <<model_.gallery_delete_name<<':'<<model_.gallery_delete_chosen<<':'
            <<model_.backend.playback_position_us/100000<<':'<<model_.backend.playback_duration_us/1000000<<':'<<model_.backend.playback_seeking<<':'
            <<model_.playback_hud_enabled<<bool(model_.backend.playback_hud)<<model_.backend.playback_telemetry_error<<model_.backend.playback_paused<<':'<<model_.backend.media.playback_id<<':'<<model_.backend.playback_error;
        if(model_.scene==SceneMode::GalleryBrowse) for(const auto& clip:model_.backend.recordings)signature<<clip.name<<':'<<clip.bytes<<';';
        if(signature.str()!=gallery_signature_) {
            const auto next=signature.str();
            if(model_.scene==SceneMode::GalleryPlayback && gallery_signature_.rfind(std::to_string(int(SceneMode::GalleryPlayback))+":",0)==0) {
                const lv_area_t header{0,0,width_-1,int(200.0f*height_/1080)};
                lv_obj_invalidate_area(gallery_,&header);
            } else lv_obj_invalidate(gallery_);
            gallery_signature_=next;
        }
    }
    const bool replay=model_.scene==SceneMode::GalleryPlayback;
    const auto& grid=replay?model_.backend.playback_grid:model_.backend.bf_grid;
    const bool bf=model_.bf_osd && model_.hud_visible() && (replay?model_.backend.playback_osd_fresh:model_.backend.serial.fresh(now));
    if(bf!=bf_visible_ || (bf && bf_grid_!=grid)) {
        bf_visible_=bf;
        for(int row=0;row<20;++row) for(int col=0;col<53;++col) {
            const int i=row*53+col;
            const auto glyph=bf?grid[i]:0;
            if(glyph==bf_grid_[i])continue;
            bf_grid_[i]=glyph;
            lv_area_t area{(6+col*36)*width_/1920,row*54*height_/1080,(6+(col+1)*36)*width_/1920,(row+1)*54*height_/1080};
            lv_obj_invalidate_area(bf_object_,&area);
        }
    }
    const auto debug=!replay && model_.debug && model_.hud_visible() && !model_.menu_open?model_.backend.radio_debug:std::string();
    if(debug!=debug_text_ || old_theme_!=model_.theme) {debug_text_=debug;lv_obj_invalidate(debug_object_);}
    auto clock=replay?(model_.hud_visible() && model_.clock?model_.backend.playback_clock:std::string()):model_.clock_text(std::time(nullptr),now);
    auto hud_data = controller_.hud_data(now);
    const auto recording = hud_data.enabled && hud_data.panels[0].visible && model_.hud_style != "vector" ? std::string() : replay?std::string():controller_.recording_text(now);
    if (recording != recording_text_) { recording_text_ = recording; lv_obj_invalidate(clock_); }
    if(clock!=clock_text_ || old_theme_!=model_.theme) {
        clock_text_=clock; lv_obj_invalidate(clock_);
    }
    hud_->update(hud_data, now);
    sync_hud_objects();
    const bool hud_changed = hud_revision_ != hud_->revision();
    const float pixels_per_world=height_/(2*std::tan(.785f/2)*3.2f);
    const bool motion=model_.hud_visible() && model_.dynamic && !model_.menu_open;
    const float dx=replay?model_.backend.playback_dx:model_.backend.hud_dx;
    const float dy=replay?model_.backend.playback_dy:model_.backend.hud_dy;
    // The native software HUD moves without resampling its cached images.
    // Preserve bank telemetry for recording, but keep live and replay panels level.
    const std::array<int,2> offset{motion?int(std::lround(dx*pixels_per_world)):0,
        motion?-int(std::lround(dy*pixels_per_world)):0};
    if(offset!=motion_offset_) {
        for(auto* object:hud_objects_) {
            if(offset[0]!=motion_offset_[0]) lv_obj_set_style_translate_x(object,offset[0],0);
            if(offset[1]!=motion_offset_[1]) lv_obj_set_style_translate_y(object,offset[1],0);
        }
        // LVGL invalidates both the old and new bounds; no full-screen damage.
        motion_offset_=offset;
    }
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const int warning = model_.link_edge_warning ? (replay?model_.backend.playback_warning:controller_.link_warning()) : 0;
    const float edge_target = warning && model_.hud_visible() ? 1 : 0;
    edge_fade_ = canopy::edge_fade(edge_fade_, edge_target, dt);
    const int prior_level = edge_level_;
    if (warning) edge_level_ = warning;
    const float pulse = edge_level_ == 2 ? canopy::edge_pulse(replay?model_.backend.playback_position_us:now) : 1;
    const int edge_opacity = int(255 * .75f * edge_fade_ * pulse);
    if (edge_opacity != edge_opacity_ || prior_level != edge_level_) {
        edge_opacity_ = edge_opacity;
        for (auto* object : edge_objects_) lv_obj_invalidate(object);
    }
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
    animating_ = moving || model_.visual.animating || edge_fade_ != edge_target || (edge_fade_ > 0 && edge_level_ == 2);
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
    if (appearance || (!moving && menu && !menu_damage_applied_) || (moving && next == 0) ||
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
