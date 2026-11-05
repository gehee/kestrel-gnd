#include "controller.hpp"

namespace kestrel_ui {
void Controller::tick(uint64_t now) {
    auto& m = model_;
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
    if (model_.stage == MenuStage::Edit) cancel_edit();
    model_.scene = scene;
    model_.menu_open = scene == SceneMode::Menu;
    model_.stage = MenuStage::Pick;
    model_.feedback.clear(); ++model_.menu_version;
}
void Controller::cancel_edit() {
    auto& m = model_;
    if (!m.edit_id_.empty()) m.set_setting(m.edit_id_, m.edit_original_);
    m.pending_.erase(m.edit_id_);
    m.edit_id_.clear(); m.stage = MenuStage::Browse; m.feedback = "Change cancelled"; ++m.menu_version;
}
void Controller::key(Key key, bool repeat) {
    auto& m = model_;
    const bool arrow = key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right;
    if (repeat && (!m.key_repeat || !arrow)) return;
    ++m.key_count; m.last_key = key_name(key); ++m.menu_version;
    if (key == Key::Quit) { if (m.stage == MenuStage::Edit) cancel_edit(); m.quit = true; return; }
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
    if (!m.menu_open) { if (key == Key::Enter) open_menu(); return; }
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
                if (save_preferences) {
                    Model committed = m;
                    for (const auto& pending : m.pending_)
                        if (pending.first != m.edit_id_) committed.set_setting(pending.first, pending.second);
                    save_preferences(committed.preferences()); m.feedback.clear();
                }
                else m.feedback.clear();
            } catch (const std::exception& error) {
                m.set_setting(m.edit_id_, m.edit_original_);
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
    } else if (key == Key::Enter && menu[m.selected].editable) {
        m.edit_tab_ = m.tab; m.edit_id_ = menu[m.selected].id;
        m.edit_original_ = m.pending(m.edit_id_) ? m.pending_.at(m.edit_id_) : m.setting(m.edit_id_); m.stage = MenuStage::Edit;
    }
}
} // namespace kestrel_ui
