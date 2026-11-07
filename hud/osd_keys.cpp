// The keys: the menu, the canopy, the channel page and the gallery.
#include "hud/osd_detail.hpp"


void OSD::handle_key(int key) {
    // Flags set under the mutex; acted on AFTER unlock to avoid deadlock.
    // (bg_player_->stop() calls pthread_join which would block while holding
    // osd_mutex, and the decode thread calls on_new_frame → signal_render →
    // pthread_mutex_lock(&osd_mutex) → deadlock.)
    bool do_start_player = false;
    bool do_stop_player  = false;

    pthread_mutex_lock(&osd_mutex);
    sync_radio();   // the menu acts on the radio as it is now

    // Refresh Sync on trial: Enter, anywhere in the menu, keeps it. Not in the
    // gallery: there Enter plays a recording, and the trial runs out as it would.
    if (sync_confirm_ && !gallery_.active() && (key == 13 || key == '\n')) {
        sync_confirm_ = false;
        settings_.set(display::setting::display_lock(screen_id_), refresh_sync().mode);
        printf("menu: refresh sync %s kept for %s\n", refresh_sync_label(refresh_sync().mode),
               screen_id_.empty() ? "this screen" : screen_id_.c_str());
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    // A screen mode on trial: Enter, anywhere in the menu, keeps it (not in the gallery).
    if (screen_confirm_ && !gallery_.active() && (key == 13 || key == '\n')) {
        screen_confirm_ = false;
        settings_.set(display::setting::screen_mode(screen_id_),
            screen_confirm_mode_ == "auto" ? std::string() : screen_confirm_mode_);
        printf("menu: screen mode %s kept for %s\n", screen_confirm_mode_.c_str(),
               screen_id_.empty() ? "this screen" : screen_id_.c_str());
        menu_open = false;
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    
    // The buttons. On the live picture the D-pad's Enter opens the menu and Back
    // opens the gallery. With the menu open they are what they were: Enter
    // applies, Back closes (as M does). With the gallery open they are its keys.
    if (gallery_.active()) {
        Gallery::Key gk = Gallery::kUp;
        bool is_gallery_key = true;
        if      (key == 'w' || key == 'W' || key == KEY_UP    || key == 0x101) gk = Gallery::kUp;
        else if (key == 's' || key == 'S' || key == KEY_DOWN  || key == 0x102) gk = Gallery::kDown;
        else if (key == 'a' || key == 'A' || key == KEY_LEFT  || key == 0x104) gk = Gallery::kLeft;
        else if (key == 'd' || key == 'D' || key == KEY_RIGHT || key == 0x103) gk = Gallery::kRight;
        else if (key == 13 || key == '\n')                                     gk = Gallery::kEnter;
        else if (key == kKeyBack || key == 27)                                  gk = Gallery::kBack;
        else is_gallery_key = false;   // M and the rest wait until it is closed
        if (is_gallery_key) {
            gallery_.key(gk);
            playback_ui_key_us_ = get_time_us();   // playback's overlay comes back
        }
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    if (menu_open && menu_scan_open) {
        chan_key(key);
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    if (key == kKeyBack || key == 27) {
        if (!menu_open) {
            // The next frame copies the screen, then opens the gallery on it
            // (gallery_capture_frame).
            gallery_capture_ = true;
            render_requested = true;
            hud_wake_.poke();
            pthread_mutex_unlock(&osd_mutex);
            return;
        }
        // The canopy folds one step at a time: a value being changed (its
        // pending change stays, as it always has), then the blade.
        if (ui_stage_ == 1) {
            if (menu_focus == 2) menu_focus = 1;
            else ui_stage_ = 0;
            menu_refresh_options();
            render_requested = true;
            hud_wake_.poke();
            pthread_mutex_unlock(&osd_mutex);
            return;
        }
        key = 'm';   // the canopy goes back to sleep
    } else if ((key == 13 || key == '\n') && !menu_open) {
        key = 'm';   // Enter on the live picture opens the menu
    }

    if (key == 'm' || key == 'M') {
        menu_open = !menu_open;
        if (menu_open) {
             // The canopy wakes with a blade picked - the one used last - and
             // nothing unfolded yet.
             ui_stage_ = 0;
             // DISPLAY > Screen Mode lists what the connector offers at this
             // open: a display plugged in since brings modes of its own. A
             // choice staged against the old list goes with it.
             // The HUD's thread makes the list again (refill_screen_modes): it
             // reads it while drawing, without the lock this holds.
             screen_modes_stale_ = true;
             menu_screen_mode = -1;
             menu_dirty[3][kDispScreenMode] = false;
             menu_pending_from[3][kDispScreenMode][0] = '\0';
             menu_opt_tab_ = -1;
             // Focus opens on the section column (menu_focus = 0, below) so
             // the first press goes somewhere useful, the same on every open
             // - but the row list underneath it must already have a real,
             // selectable row lit, not the -1 this used to leave behind.
             // That -1 was fine for drawing (the render loop clamped it to 0
             // before the next frame), but the key handler runs on its own
             // thread and reads menu_index too, under the same lock, and a
             // key pressed in the gap before that clamp ran acted on a row
             // that did not exist - and a header row is just as invalid a
             // "selection" as -1 is, if the tab or the settings under it
             // changed while the menu was closed. Reuse the same check a tab
             // switch already does.
             std::vector<MenuItem> rows = menu_items(menu_tab);
             if (menu_tab < 0 || menu_tab >= kMenuTabs ||
                 menu_index < 0 || menu_index >= (int)rows.size() ||
                 rows[menu_index].type == 2)
                 menu_index = menu_first_selectable_row(menu_tab);
             menu_focus = 0;
             // First open of the boot: start somewhere deliberate rather than
             // on whatever tab the enum happens to begin with. After that the
             // menu reopens where it was left.
             if (menu_first_open_) {
                 menu_first_open_ = false;
                 menu_tab = kTabSystem;
                 menu_index = menu_first_selectable_row(menu_tab);
                 menu_focus = 0;
             }
             menu_scan_open = false;
             unknown_command(0xe, 0);
             memset(menu_dirty, 0, sizeof(menu_dirty));   // re-seeded = nothing pending
             menu_bf_osd = bf_osd;
             menu_clock_mode = clock_mode;
             menu_clock_show = clock_show;
             menu_hud_theme = hud_theme_idx;
             menu_zone_ = zones::selected();
             menu_zone_gen_ = zones::generation();
             menu_licence_ = zones::licensed() ? 1 : 0;
             menu_hud_style = hud_style;
             menu_volt_mode = volt_mode;
             menu_ui_scale = osd_vars.ui_scale;
             menu_picture_scale = dev->picture_scale();
             menu_bg_video = bg_video_enabled;
             menu_frame_mode = g_frame_mode.load();
             {
                 const display::RefreshSync rs = refresh_sync();
                 menu_refresh_sync = rs.mode;
                 menu_sync_slice = rs.slice;
                 menu_sync_margin = rs.margin_us / 1000;
             }
             menu_hud_reactivity = hud_reactivity;
             menu_hud_debug = hud_debug;
             menu_dvr_source = dvr_source_now();
             // Seed the RF rows from what the radio and the air unit report.
             // This air unit's own (Ar8030Link::load_sky_link), which the
             // radio is running with.
             const hud::LinkView rs = link_view();
             menu_ar_power = rs.tx_power_want_mw;   // shown fitted to the air unit (or --)
             menu_ar_standby = air_standby;   // start the row at the air's real state
             menu_ar_hop = rs.chan_auto;
             menu_ar_maxbr = ar_maxbr_index(rs.max_kbps);
             menu_ar_maxbw = ar_maxbw_index(rs.max_bw_mhz);
             menu_ar_frame = ar_frame_index(rs.frame_mode);
             menu_ar_maxmcs = rs.max_mcs < 0 ? 0 : rs.max_mcs + 1;
             menu_ar_steady = rs.steady;
             menu_ar_fw = rs.radio_fw;
             menu_refresh_options();      // seeded: work out what row 0 shows
        }
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    
    if (!menu_open) {
        pthread_mutex_unlock(&osd_mutex);
        return;
    }

    // The canopy (osd.hpp, ui_*). Picking: Left and Right are the two blades,
    // OK unfolds one. Unfolded: Left and Right walk its sections; Up and Down
    // its rows; OK on a setting starts changing it (Up/Down) and OK again
    // applies it; OK on an action (Bind, Channel) does it.
    {
        const bool up    = (key == 'w' || key == 'W' || key == KEY_UP    || key == 0x101);
        const bool down  = (key == 's' || key == 'S' || key == KEY_DOWN  || key == 0x102);
        const bool left  = (key == 'a' || key == 'A' || key == KEY_LEFT  || key == 0x104);
        const bool right = (key == 'd' || key == 'D' || key == KEY_RIGHT || key == 0x103);
        const bool enter = (key == 13 || key == '\n');
        auto done = [&]() {
            render_requested = true;
            hud_wake_.poke();
            pthread_mutex_unlock(&osd_mutex);
        };
        if (ui_stage_ == 0) {
            if (left) ui_side_ = 0;
            else if (right) ui_side_ = 1;
            else if (enter) {
                const std::vector<int> tabs = ui_side_tabs(ui_side_);
                int t = ui_side_tab_[ui_side_];
                if (std::find(tabs.begin(), tabs.end(), t) == tabs.end()) t = tabs[0];
                ui_side_tab_[ui_side_] = menu_tab = t;
                menu_index = menu_first_selectable_row(menu_tab);
                menu_focus = 1;
                ui_stage_ = 1;
                menu_refresh_options();
            }
            (void)up; (void)down;
            done();
            return;
        }
        if (left || right) {
            if (menu_focus == 2) {
                if (left) menu_focus = 1;            // out of the value, the change stays pending
            } else {
                const std::vector<int> tabs = ui_side_tabs(ui_side_);
                int i = (int)(std::find(tabs.begin(), tabs.end(), menu_tab) - tabs.begin());
                if (i >= (int)tabs.size()) i = 0;
                i = std::max(0, std::min((int)tabs.size() - 1, i + (right ? 1 : -1)));
                ui_side_tab_[ui_side_] = menu_tab = tabs[i];
                menu_index = menu_first_selectable_row(menu_tab);
            }
            menu_refresh_options();
            done();
            return;
        }
        if (enter && menu_focus == 1) {
            std::vector<MenuItem> rows = menu_items(menu_tab);
            const int type = (menu_index >= 0 && menu_index < (int)rows.size()) ? rows[menu_index].type : 0;
            if (type == 1) {                          // a setting: start changing it
                menu_log_applied("edit");             // what it was
                menu_focus = 2;
                menu_edit_tab_ = menu_tab;
                menu_edit_label_ = rows[menu_index].label ? rows[menu_index].label : "";
                menu_refresh_options();
                done();
                return;
            }
            if (type != 3) { done(); return; }        // a reading or a group's name
            // An action: Enter below does it.
        } else if (enter && menu_focus == 2) {
            menu_focus = 1;                           // OK applies (below) and the row closes
            if (!menu_edit_row_intact()) {            // ...unless the row went: OK only closes it
                menu_refresh_options();
                done();
                return;
            }
        }
        // Up and Down go on below: the row with menu_focus 1, the value with 2.
    }

    if (key == 9) {
       menu_tab = (menu_tab + 1) % kMenuTabs;
       menu_index = menu_first_selectable_row(menu_tab);   // a real row, as every other tab change
       render_requested = true;
       hud_wake_.poke();
       pthread_mutex_unlock(&osd_mutex);
       return;
    }

    // Derived, never hardcoded - see OSD::menu_items().
    int items_count = (int)menu_items(menu_tab).size();

    // Up and down move within the list (menu_focus 1) or change the value
    // being edited (2); left and right are the canopy's (above).
    const bool k_up    = (key == 'w' || key == 'W' || key == KEY_UP    || key == 0x101);
    const bool k_down  = (key == 's' || key == 'S' || key == KEY_DOWN  || key == 0x102);

    if (k_up || k_down) {
        const int dir = k_down ? -1 : 1;   // rows ascend, so UP increments
        if (menu_focus == 0) {
            bool connected = (osd_vars.radio_link.state == 2);
            int next = menu_tab - dir;     // the section column reads top-down
            while (next < 0) next += kMenuTabs;
            next %= kMenuTabs;
            if (!connected && next < 1) next = (dir < 0) ? 1 : kMenuTabs - 1;   // VIDEO needs a link; RF LINK holds Bind
            menu_tab = next;
            menu_index = menu_first_selectable_row(menu_tab);
        } else if (menu_focus == 1) {
            std::vector<MenuItem> rows = menu_items(menu_tab);
            int n = (int)rows.size();
            if (n > 0) {
                menu_index = (menu_index - dir + n) % n;
                // Section headers are not a row to land on - keep stepping
                // the same direction until a real setting turns up.
                for (int guard = 0; guard < n && rows[menu_index].type == 2; guard++)
                    menu_index = (menu_index - dir + n) % n;
            }
        } else if (!menu_edit_row_intact()) {
            menu_focus = 1;                           // the row being changed went: nothing to step
        } else {
            std::vector<MenuItem> rows = menu_items(menu_tab);
            const bool ro = (menu_index >= 0 && menu_index < (int)rows.size() &&
                             rows[menu_index].type == 0);
            const bool tracked = !ro && menu_tab >= 0 && menu_tab < kMenuTabs &&
                                 menu_index >= 0 && menu_index < kMenuRows &&
                                 !menu_row_applies_live(menu_tab, menu_index);
            // A row Enter has to commit: remember what it read before the
            // first nudge, so the value column can say what is pending and
            // so walking back to it counts as changing nothing.
            char* from = tracked ? menu_pending_from[menu_tab][menu_index] : nullptr;
            if (from && !menu_dirty[menu_tab][menu_index])
                menu_plain_value(menu_tab, menu_index, from, sizeof(menu_pending_from[0][0]));
            // The value column is a list read top to bottom, and the
            // highlight follows the key: UP is the entry above, which is
            // the one before it in the row's order.
            if (!ro) menu_apply_change(menu_tab, menu_index, -dir);
            if (!ro && !tracked) menu_log_applied("live");
            if (from) {
                char now[32];
                menu_plain_value(menu_tab, menu_index, now, sizeof(now));
                const bool back = (strcmp(now, from) == 0);
                menu_dirty[menu_tab][menu_index] = !back;
                if (back) from[0] = '\0';
            }
        }
        // The selection or the value just moved, so the value column has to be
        // worked out again - here, where the menu state is ours to step.
        menu_refresh_options();
        render_requested = true;
        hud_wake_.poke();
        pthread_mutex_unlock(&osd_mutex);
        return;
    }

    // The tab strip has no row of its own for Enter to act on. This used to
    // be keyed off menu_index == -1, a sentinel a fresh menu open left
    // behind - but left/right (above) and up/down (further above) already
    // move the SECTION column via menu_focus alone and never touched that
    // sentinel, so menu_index could just as easily be a real row while
    // menu_focus was still 0 (move to the row column, then back to the
    // tabs with a left press). menu_focus is what actually says which
    // column is lit, and now that a menu open always leaves menu_index on a
    // real, selectable row (see where it opens), it is the only thing this
    // needs to check.
    if (menu_focus == 0) {
        pthread_mutex_unlock(&osd_mutex);
        return;
    }
    
    if (key == 13 || key == '\n') { // Enter
        if (menu_open) {
            menu_mark_dirty(false);      // committing is what clears the marker
            menu_log_applied("enter");
            if (menu_tab == 0) {
                // 0x01 = video mode (routed to SET_CHN_RES in main); 0x300+n are
                // the AR8030 camera settings, kept out of the kestrel-air id space.
                switch (video_row(menu_index)) {
                    case 0: parts_.camera.set_mode(menu_video_mode); break;
                    case 3: parts_.camera.set(air::CameraField::EV, kEvSteps[menu_cam_ev]); break;
                    case 5: parts_.camera.set(air::CameraField::SAT, menu_cam_sat); break;
                    case 6: parts_.camera.set(air::CameraField::CONTRAST, menu_cam_contrast); break;
                    case 7: parts_.camera.set(air::CameraField::SHARPNESS, menu_cam_sharp); break;
                    case 2: parts_.camera.set(air::CameraField::SCENE, kSceneVals[menu_cam_scene]); break;
                    case 4: parts_.camera.set(air::CameraField::AWB, wb_kelvin(menu_cam_wb)); break;
                    case 1:
                        // 0x30B = CAM_ANGLE -> sky cmd 0x06. The camera
                        // accepts this and reports the new angle back in
                        // its status frame, but the picture does not change
                        // on its own - so follow it with the current video
                        // mode, which is what makes the camera reconfigure
                        // the channel (section 21.7). Order matters: the
                        // angle has to be set before the mode is re-applied.
                        parts_.camera.set(air::CameraField::ANGLE, menu_cam_flip ? 1 : 0);
                        parts_.camera.set_mode(menu_video_mode);
                        break;
                    // 0x30D = CAM_3DNR. No sky command exists for it, so
                    // the source re-sends CMD_SET_CONFIG instead.
                    case 8: parts_.camera.set(air::CameraField::DNR3D, kDnrVals[menu_cam_3dnr]); break;
                    case 9: parts_.camera.set(air::CameraField::FOCUS, menu_cam_focus ? 1 : 0); break;
                    // Max Bitrate: saved, and sent again at every link-up.
                    case 10: asked_.kbps = parts_.encoder.set_max_bitrate(kArMaxBrVals[menu_ar_maxbr]); break;
                    default: break;
                }
            } else if (menu_tab == 1) {
                // AR8030: RF ioctls go straight to our own radio via
                // Ar8030Link (0x310 + field). No kestrel-air hop, and no
                // a second SDK client would fight the source
                // for the daemon's single owner slot.
                auto rfi = ar_rf_items();
                if (menu_index >= 0 && menu_index < (int)rfi.size()) {
                    switch (rfi[menu_index].second) {
                        case 5:
                            chan_open();
                            break;
                        case 1:     // the level and the mode are one value: saved per air unit
                        case 16: {  // there; nothing before it is known
                            const int prj = air_radio_.prj;
                            if (prj > 0) parts_.air_radio.set_tx_power(ar_pwr_fit(prj, menu_ar_power, air_radio_.fine));
                            break;
                        }
                        case 8:
                            // Force standby on/off (sky cmd 0x23 via the queue that
                            // owns the port-2 socket). Momentary: NOT persisted, and
                            // the air overrides it via fpv_sky_standby_mode_thread
                            // (auto-parks when idle, exits when flying). Stock offers
                            // the same temporary force from its menu.
                            parts_.air_unit.set_standby(menu_ar_standby);
                            break;
                        case 11:
                            // Saved and sent to the air unit; again at every link-up.
                            asked_.bw = parts_.radio.set_max_bandwidth(kArMaxBwVals[menu_ar_maxbw]);
                            break;
                        case 12:
                            // Saved and sent to the radio now; again after every link-up.
                            asked_.frame = parts_.radio.set_frame(kArFrameVals[menu_ar_frame]);
                            break;
                        case 14:
                            // Saved and sent to our radio now; again after every link-up.
                            asked_.mcs = parts_.radio.set_max_mcs(menu_ar_maxmcs - 1);
                            break;
                        case 15:
                            // Saved and sent to our radio now; again after every link-up.
                            asked_.steady = parts_.radio.set_steady(menu_ar_steady == 1);
                            break;
                        case 13:
                            // Saved; a change restarts the radio with that firmware.
                            asked_.fw = parts_.radio.set_firmware(menu_ar_fw);
                            break;
                        case 9:
                            // Bind. Parked for the RX thread - the menu runs on the
                            // OSD thread and must never call into the baseband SDK.
                            // This branch runs whenever the radio reports any
                            // capability, i.e. always in practice; the id-based
                            // dispatch below it is only the no-capabilities
                            // fallback, which is why a handler there alone left
                            // this row's Enter doing nothing.
                            parts_.radio.bind();
                            break;
                        default: break;
                    }
                }
            } else if (menu_tab == 3) { // DISPLAY
                // Brightness, UI Scaling, Theme and Picture Size all apply and
                // save as they are nudged. Background Video is the one row
                // here Enter still owns, because switching it starts or stops
                // a player rather than changing a number.
                if (disp_row(menu_index) == kDispBgVideo) {
                    bg_video_enabled = menu_bg_video;
                    settings_.set(hud::setting::bg_video_enabled, bg_video_enabled);
                    // Only start the player if we are still in idle (BACKGROUND) state -
                    // once FPV has been received the frozen last frame stays on screen.
                    do_start_player = bg_video_enabled && (video_state_ == VideoState::BACKGROUND);
                    do_stop_player  = !bg_video_enabled;
                }
                if (disp_row(menu_index) == kDispScreenMode && screen_mode_menu() != screen_mode_saved()) {
                    // Only a trial: main.cpp applies it at the restart and asks
                    // (begin_screen_mode_confirm); the screen's screen_mode is
                    // written once it is kept.
                    const int m = screen_mode_menu();
                    char v[32] = "auto";
                    if (m > 0) {
                        const ScreenModeOpt& o = screen_modes()[m - 1];
                        snprintf(v, sizeof(v), "%dx%d@%d", o.w, o.h, o.hz);
                    }
                    settings_.set(display::setting::screen_mode_try, (screen_id_.empty() ? std::string("-") : screen_id_) + " " + v);
                    printf("menu: screen mode %s on trial - restarting to apply it\n", v);
                    kestrel_request_restart();
                }
            } else if (menu_tab == kTabDvr) {
                if (menu_index == 0) {
                    // Just a mode flag now. The REC button owns starting and
                    // stopping; this used to spin up a second DVR of its own the
                    // moment it was switched on, which recorded to a fixed
                    // dvr_screen.mp4 and had nothing to do with the REC button.
                    dvr_screen = menu_dvr_source > 0;
                    settings_.set(dvr::setting::dvr_screen, dvr_screen);
                    DvrRecorder::instance().set_screen_mode(dvr_screen);
                }
            } else if (menu_tab == kTabSystem) {
                if (menu_index == kSysWifi && platform::caps().wifi_ap) {
                    wifi_ap_set(menu_wifi_ap != 0);   // for this session only
                }
            }
            render_requested = true;
        }
    }
    
    // Set the flag, not just the signal. run()'s wait is
    //     while (!render_requested && ... ) pthread_cond_timedwait(...)
    // so a bare signal wakes the thread, the predicate re-checks, and it goes
    // straight back to sleep - the wakeup is swallowed. Several key paths
    // (menu up/down among them) signalled without setting the flag, so those
    // keys only redrew on the 1000 ms idle timeout. With video decoding the
    // per-frame redraw hid it; with no link and no video there is nothing else
    // driving the loop, which is why navigation felt laggy exactly then.
    render_requested = true;
    hud_wake_.poke();
    pthread_mutex_unlock(&osd_mutex);

    // Player start/stop MUST happen outside osd_mutex: stop() calls
    // pthread_join() which blocks until the decode thread exits, and the
    // decode thread calls on_new_frame → signal_render → locks osd_mutex.
    if (do_stop_player && bg_player_)  bg_player_->stop();
    else if (do_start_player && bg_player_) bg_player_->start();
}
