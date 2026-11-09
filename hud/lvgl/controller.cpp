#include "controller.hpp"
#include <stdexcept>

namespace kestrel_ui {
void Controller::tick(uint64_t now) {
    auto& m = model_;
    // Pending edits belong to the connected unit. Drop them when its control
    // disappears or loses capability, including while the menu is closed.
    for (auto i = m.receiver_preview_.begin(); i != m.receiver_preview_.end();) {
        const auto control = m.backend.controls.find(i->first);
        if (control == m.backend.controls.end() || !control->second.enabled) {
            m.pending_.erase(i->first);
            i = m.receiver_preview_.erase(i);
        } else ++i;
    }
    const auto& media = m.backend.media;
    warning_.update(now, m.backend.vrx.link_valid(now), m.link_range(now).warning(),
                    media.video_monitored, media.video_stamp_us, m.backend.playback_busy);
    if (media.recording && (!was_recording_ || media.recording_id != recording_id_)) recording_since_ = now;
    was_recording_ = media.recording; recording_id_ = media.recording_id;
    const bool armed = m.backend.flight.arm == 1 && m.backend.flight.stamp_us &&
        now >= m.backend.flight.stamp_us && now - m.backend.flight.stamp_us < 2000000;
    if (armed && !was_armed_) armed_since_ = now;
    was_armed_ = armed;
    m.visual = canopy::link_visual(m.backend.vrx.link_valid(now), now, previous_, up_, down_);
    const auto& f = m.backend.flight;
    if (f.stamp_us && now >= f.stamp_us && now - f.stamp_us < 2000000 && f.have_cell_v)
        cell_peak_ = std::max(cell_peak_, f.cell_v);
    if (m.menu_open) {
        if (m.side == 1 && m.tab == 0 && !m.backend.vrx.link_valid(now)) {
            if (m.stage == MenuStage::Edit) cancel_edit();
            m.tab = m.remembered_tab_[1] = 1; m.selected = 0; ++m.menu_version;
        }
        const auto menu = m.rows(now);
        if (m.stage == MenuStage::Edit && !m.edit_intact()) cancel_edit();
        m.selected = std::clamp(m.selected, 0, std::max(0, int(menu.size()) - 1));
    }
}
void Controller::open_menu() { model_.menu_open = true; model_.stage = MenuStage::Pick; model_.scene = SceneMode::Live; model_.feedback.clear(); ++model_.menu_version; }
void Controller::set_scene(SceneMode scene) {
    model_.gallery_delete_name.clear();model_.gallery_delete_chosen=false;
    if (model_.stage == MenuStage::Edit) cancel_edit();
    if(scene==SceneMode::GalleryPlayback) {
        model_.playback_hud_enabled=model_.show_hud;
        model_.playback_controls_visible=true;
    }
    model_.scene = scene;
    model_.menu_open = scene == SceneMode::Menu;
    model_.stage = MenuStage::Pick;
    model_.feedback.clear(); ++model_.menu_version;
}
void Controller::cancel_edit() {
    auto& m = model_;
    if (m.backend.controls.count(m.edit_id_)) m.receiver_preview_.erase(m.edit_id_);
    else if (!m.edit_id_.empty()) m.set_setting(m.edit_id_, m.edit_original_);
    m.pending_.erase(m.edit_id_);
    m.edit_id_.clear(); m.stage = MenuStage::Browse; m.feedback = "Change cancelled"; ++m.menu_version;
}
void Controller::key(Key key, bool repeat) {
    auto& m = model_;
    const bool arrow = key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right;
    if (repeat && (!m.key_repeat || !arrow)) return;
    ++m.key_count; m.last_key = key_name(key); ++m.menu_version;
    if (key == Key::Quit) { if (m.stage == MenuStage::Edit) cancel_edit(); m.quit = true; return; }
    if(m.scene==SceneMode::GalleryBrowse || m.scene==SceneMode::GalleryPlayback) {
        if(m.scene==SceneMode::GalleryBrowse && !m.gallery_delete_name.empty()) {
            if(key==Key::Back || key==Key::Menu || key==Key::Record) {
                m.gallery_delete_name.clear();m.gallery_delete_chosen=false;
            } else if(key==Key::Left) m.gallery_delete_chosen=false;
            else if(key==Key::Right) m.gallery_delete_chosen=true;
            else if(key==Key::Enter) {
                const auto name=m.gallery_delete_name;
                const bool remove=m.gallery_delete_chosen;
                m.gallery_delete_name.clear();m.gallery_delete_chosen=false;
                if(remove)try {
                    if(!apply_receiver)throw std::runtime_error("Recording storage is not connected");
                    m.feedback=apply_receiver("delete:"+name,0);
                }catch(const std::exception& e){m.feedback=e.what();}
            }
            return;
        }
        if(key==Key::Back || key==Key::Menu) {
            try {if(m.scene==SceneMode::GalleryPlayback && apply_receiver) apply_receiver("playback_stop",0);}
            catch(const std::exception& e) {m.feedback=e.what();return;}
            set_scene(key==Key::Back && m.scene==SceneMode::GalleryPlayback?SceneMode::GalleryBrowse:SceneMode::Live);
        } else if(m.scene==SceneMode::GalleryPlayback && key==Key::Enter) {
            try {if(apply_receiver) apply_receiver("playback_pause",0);}
            catch(const std::exception& e) {m.feedback=e.what();}
        } else if(m.scene==SceneMode::GalleryPlayback && key==Key::Up) {
            m.playback_hud_enabled=!m.playback_hud_enabled;
        } else if(m.scene==SceneMode::GalleryPlayback && key==Key::Down) {
            if(!repeat)m.playback_controls_visible=!m.playback_controls_visible;
        } else if(m.scene==SceneMode::GalleryPlayback && (key==Key::Left || key==Key::Right)) {
            try {if(apply_receiver)apply_receiver("playback_seek",key==Key::Left?-5:5);}
            catch(const std::exception& e) {m.feedback=e.what();}
        } else if(m.scene==SceneMode::GalleryBrowse) {
            const auto& clips=m.backend.recordings;
            const int last=std::max(0,int(clips.size())-1);
            if(key==Key::Up || key==Key::Left) m.gallery_selected=std::clamp(m.gallery_selected-1,0,last);
            if(key==Key::Down || key==Key::Right) m.gallery_selected=std::clamp(m.gallery_selected+1,0,last);
            if(key==Key::Record && !clips.empty()) {
                m.gallery_selected=std::clamp(m.gallery_selected,0,last);
                m.gallery_delete_name=clips[m.gallery_selected].name;
                m.gallery_delete_chosen=false;m.feedback.clear();
            }
            if(key==Key::Enter && !clips.empty()) {
                m.gallery_selected=std::clamp(m.gallery_selected,0,last);
                try {
                    if(!apply_receiver)throw std::runtime_error("Playback is not connected");
                    apply_receiver("play:"+clips[m.gallery_selected].name,0);
                    set_scene(SceneMode::GalleryPlayback);
                } catch(const std::exception& e) {m.feedback=e.what();}
            }
        }
        return;
    }
    if (m.channel_open) {
        const auto& scan=m.backend.channels;
        if(key==Key::Back || key==Key::Menu) {m.channel_open=false; m.feedback.clear(); return;}
        std::vector<int> choices{0};
        for(const auto& c:scan.readings) choices.push_back(c.mhz);
        auto at=std::find(choices.begin(),choices.end(),m.channel_mhz);
        int index=at==choices.end()?0:int(at-choices.begin());
        if(key==Key::Left || key==Key::Right) {
            index=std::clamp(index+(key==Key::Left?-1:1),0,int(choices.size())-1);
            m.channel_mhz=choices[index]; m.feedback.clear();
        } else if(key==Key::Down) {m.channel_mhz=scan.current_mhz; m.feedback.clear();}
        else if(key==Key::Up) {
            const ChannelReading* best=nullptr;
            for(int pass=0;pass<2 && !best;++pass) for(const auto& c:scan.readings)
                if((pass || c.permitted) && (!best || c.peak_dbm<best->peak_dbm)) best=&c;
            if(best) m.channel_mhz=best->mhz;
            m.feedback.clear();
        } else if(key==Key::Enter) {
            try {
                if(!apply_receiver) throw std::runtime_error("Receiver controls are not connected");
                m.feedback=apply_receiver("channel",m.channel_mhz);
            } catch(const std::exception& e) {m.feedback=e.what();}
        }
        return;
    }
    if (!m.hud_visible()) {
        if (key == Key::Back) set_scene(SceneMode::Live);
        else if (key == Key::Menu) open_menu();
        return;
    }
    if (key == Key::Menu) {
        if (m.menu_open) { m.edit_id_.clear(); m.menu_open = false; m.stage = MenuStage::Pick; m.scene = SceneMode::Live; }
        else open_menu();
        return;
    }
    if (!m.menu_open) {
        if(key==Key::Enter)open_menu();
        else if(key==Key::Back) {
            const auto gallery=m.backend.controls.find("gallery");
            if(gallery!=m.backend.controls.end() && gallery->second.enabled)set_scene(SceneMode::GalleryBrowse);
        }
        return;
    }
    if (m.stage == MenuStage::Edit) {
        if (!m.edit_intact()) { cancel_edit(); return; }
        if (key == Key::Back || key == Key::Left) { m.edit_id_.clear(); m.stage = MenuStage::Browse; return; }
        const auto menu = m.rows(monotonic_us());
        if (key == Key::Up || key == Key::Down) {
            const int n = int(menu[m.selected].options.size());
            if (n) {
                m.set_setting(m.edit_id_, (m.setting(m.edit_id_) + (key == Key::Down ? 1 : n - 1)) % n);
                if (m.setting(m.edit_id_) == m.edit_original_) m.pending_.erase(m.edit_id_);
                else m.pending_[m.edit_id_] = m.edit_original_;
            }
        } else if (key == Key::Enter) {
            try {
                if (m.backend.controls.count(m.edit_id_)) {
                    if (!apply_receiver) throw std::runtime_error("Receiver controls are not connected");
                    const auto& choices=m.backend.controls.at(m.edit_id_).choices;
                    const int index=m.setting(m.edit_id_);
                    if (index<0 || index>=int(choices.size())) throw std::runtime_error("Control choices changed");
                    m.feedback=apply_receiver(m.edit_id_,choices[index].value);
                    m.receiver_preview_.erase(m.edit_id_);
                } else if (save_preferences) {
                    Model committed = m;
                    for (const auto& pending : m.pending_)
                        if (pending.first != m.edit_id_) committed.set_setting(pending.first, pending.second);
                    save_preferences(committed.preferences()); m.feedback.clear();
                }
                else m.feedback.clear();
            } catch (const std::exception& error) {
                if (m.backend.controls.count(m.edit_id_)) m.receiver_preview_.erase(m.edit_id_);
                else m.set_setting(m.edit_id_, m.edit_original_);
                m.feedback = std::string("Save failed; cancelled: ") + std::string(error.what()).substr(0, 80);
            }
            m.pending_.erase(m.edit_id_);
            m.edit_id_.clear(); m.stage = MenuStage::Browse;
        }
        return;
    }
    if (key == Key::Back) {
        if (m.stage == MenuStage::Browse) m.stage = MenuStage::Pick;
        else { m.menu_open = false; m.scene = SceneMode::Live; }
        m.feedback.clear(); return;
    }
    if (m.stage == MenuStage::Pick) {
        if (key == Key::Left) m.side = 0;
        else if (key == Key::Right) m.side = 1;

        else if (key == Key::Enter) {
            m.tab = m.remembered_section(); m.selected = m.first_row(monotonic_us()); m.stage = MenuStage::Browse;
        }
        return;
    }
    m.feedback.clear();
    const auto menu = m.rows(monotonic_us());
    if (menu.empty()) return;
    m.selected = std::clamp(m.selected, 0, int(menu.size()) - 1);
    if (key == Key::Up || key == Key::Down) {
        const int step = key == Key::Down ? 1 : int(menu.size()) - 1;
        for (size_t i = 0; i < menu.size(); ++i) {
            m.selected = (m.selected + step) % int(menu.size());
            if (!menu[m.selected].heading) break;
        }
    }
    else if (key == Key::Left || key == Key::Right) {
        const auto tabs = m.side_tabs();
        const auto found = std::find(tabs.begin(), tabs.end(), m.tab);
        int index = found == tabs.end() ? 0 : int(found - tabs.begin());
        index = std::clamp(index + (key == Key::Right ? 1 : -1), 0, int(tabs.size()) - 1);
        m.remembered_tab_[m.side] = m.tab = tabs[index]; m.selected = m.first_row(monotonic_us());
    } else if (key == Key::Enter && menu[m.selected].action) {
        if(menu[m.selected].id=="gallery") {set_scene(SceneMode::GalleryBrowse);return;}
        if(menu[m.selected].id=="channel") {
            m.channel_open=true; m.channel_mhz=m.backend.channels.automatic?0:m.backend.channels.current_mhz; return;
        }
        try {
            if (!apply_receiver) throw std::runtime_error("Receiver controls are not connected");
            m.feedback=apply_receiver(menu[m.selected].id,0);
        } catch (const std::exception& e) { m.feedback=e.what(); }
    } else if (key == Key::Enter && menu[m.selected].editable) {
        m.edit_tab_ = m.tab; m.edit_id_ = menu[m.selected].id;
        m.edit_original_ = m.pending(m.edit_id_) ? m.pending_.at(m.edit_id_) : m.setting(m.edit_id_); m.stage = MenuStage::Edit;
    }
}
} // namespace kestrel_ui
