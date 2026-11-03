// The menu's rows: what each tab lists, what each row shows and its help,
// the values a row walks through, and what a change to it does.
#include "hud/osd_detail.hpp"

std::vector<std::pair<const char*, int>> OSD::ar_rf_items() const {
    std::vector<std::pair<const char*, int>> v;
    // Unlinked: binding is the only thing this tab can do, and the only time
    // it makes sense - it is how an air unit becomes associated in the first
    // place. Linked: an air unit is already associated, so Bind is hidden and
    // the tuning rows take over. Pairing is a baseband operation and does not
    // depend on the rf_caps bits, which only describe what the radio lets us
    // tune, so this row never looks at them.
    if (osd_vars.radio_link.state != 2) {
        v.push_back({"Bind", 9});
        // Also unlinked: a firmware that does not link is the time to switch.
        v.push_back({"VRX Radio Firmware", 13});
        return v;
    }
    // Channel opens the channel page. Channel Hop (id 7) was the other half of
    // the same decision - picking a channel turns hopping off - and is AUTO HOP
    // on that page now. Every consumer switches on the item id (.second), not the
    // row index, so this order is free to change.
    if (rf_caps & 16) v.push_back({"Channel",   5});  // RF_CHAN, RF_HOP
    // TX Power Mode (16) and TX Power (1) are one setting in two rows: FIXED or AUTO,
    // and the level (AUTO's top). The value on the wire is stock's, N or N + 1.
    if (rf_caps & 1)  v.push_back({"TX Power Mode", 16});  // RF_TX_POWER
    if (rf_caps & 1)  v.push_back({"TX Power",  1});       // RF_TX_POWER
    // Radio Frame (12) and Steady Latency (15) are hidden: their settings
    // (ar8030_frame_mode, ar8030_steady) still apply from the yaml, and their
    // handlers stay, so listing them again is one line each here. Steady
    // Latency needs an Open8030 with steady mode (request 0x0c5a0037), which
    // its main lacks.
    // Open8030 on the goggle: the MCS its receive policy may go up to.
    if (link_->radio_fw == 1) v.push_back({"Max MCS", 14});
    // The widest the link may go (Open8030: live, the width policy decides
    // below it; stock: 20 or 40, the radio restarts).
    v.push_back({"Max Bandwidth", 11});
    v.push_back({"VRX Radio Firmware", 13});
    // MCS, Bandwidth and LNA Mode were removed rather than left visible.
    // BB_SET_MCS has no direction field, so it can only set our own transmit -
    // the video MCS is the air unit's choice, driven by the policy table in
    // section 38.2, and no ground-side setting moves it. BB_SET_BANDWIDTH is
    // not how this firmware sets bandwidth at all and measurably costs
    // throughput (section 38.4). LNA does work, but it only trims our own
    // front end and AUTO is the right answer outside a bench.
    // Standby is not an rf_caps bit - it is a sky command to the air unit
    // (0x23), so it is offered whenever the link is up rather than gated on
    // what the local baseband's ioctls answer.
    //
    // "Auto Standby", not "Standby Mode": this row reflects/toggles the
    // air's enable/disable SETTING for parking itself in low power while
    // idle - not whether it is in standby right now, which the row cannot
    // show reliably (TLV 0x12 was confirmed stuck reporting ON through an
    // entire arm/disarm cycle - see ar8030-power-and-standby-verified.md).
    // The live state is the canopy's own STANDBY label instead.
    v.push_back({"Auto Standby", 8});
    return v;
}

void OSD::menu_mark_dirty(bool on) {
    if (menu_tab < 0 || menu_tab >= kMenuTabs) return;
    if (menu_index < 0 || menu_index >= kMenuRows) return;   // -1 is the tab strip
    menu_dirty[menu_tab][menu_index] = on;
    if (!on) menu_pending_from[menu_tab][menu_index][0] = '\0';
}

// One step on SYSTEM > Time Zone's Area, Country or Timezone row, from the zone
// in menu_zone_: the zone it lands on. Area walks NOT SET and the areas and
// lands on the area's first country and zone; Country walks the area's
// countries and lands on its first zone; Timezone walks the country's zones.
int OSD::menu_step_zone(int row, int dir) {
    const int z = menu_zone_;
    if (row == kSysArea) {
        const int cur = z < 0 ? 0 : zones::zone(z).area + 1;
        const int v = menu_step(cur, dir, zones::area_count() + 1);
        if (v == cur) return z;
        if (v == 0) return -1;
        const std::vector<int> cs = zones::countries_in(v - 1);
        if (cs.empty()) return z;
        const std::vector<int> zs = zones::zones_in(v - 1, cs[0]);
        return zs.empty() ? z : zs[0];
    }
    if (z < 0) return z;
    const int area = zones::zone(z).area, c = zones::zone(z).country;
    if (row == kSysCountry) {
        const std::vector<int> cs = zones::countries_in(area);
        const int cur = (int)(std::find(cs.begin(), cs.end(), c) - cs.begin());
        if (cur >= (int)cs.size()) return z;
        const int v = menu_step(cur, dir, (int)cs.size());
        if (v == cur) return z;
        const std::vector<int> zs = zones::zones_in(area, cs[v]);
        return zs.empty() ? z : zs[0];
    }
    const std::vector<int> zs = zones::zones_in(area, c);
    const int cur = (int)(std::find(zs.begin(), zs.end(), z) - zs.begin());
    if (cur >= (int)zs.size()) return z;
    return zs[menu_step(cur, dir, (int)zs.size())];
}

// One press on a row that holds a set of values. For the arrows it wraps, so
// a row never dead-ends. While a row is being enumerated (menu_stepping_) it
// stops at the ends instead: that is what lets menu_options walk to the start
// of a set and along it to the end, and so learn a row's values in their own
// order without a table of every row's domain.
int OSD::menu_step(int v, int dir, int n) const {
    if (n <= 1) return 0;
    if (menu_stepping_) {
        int r = v + dir;
        return r < 0 ? 0 : (r >= n ? n - 1 : r);
    }
    return (v + dir + n) % n;
}

// What Enter does on an action row, and how the action is going. The hint
// is the item's own unless the action is under way or has just finished,
// when the verb changes with it: a running bind offers the cancel, a failed
// one the retry. The status is empty for anything that finishes at once.
void OSD::menu_action_status(int tab, int i, char* hint, size_t hcap,
                             char* status, size_t scap) {
    hint[0] = '\0'; status[0] = '\0';
    std::vector<MenuItem> rows = menu_items(tab);
    if (i < 0 || i >= (int)rows.size() || rows[i].type != 3) return;
    snprintf(hint, hcap, "%s", rows[i].hint ? rows[i].hint : "ENTER");

    if (tab == 1) {
        auto rfi = ar_rf_items();
        if (i < (int)rfi.size() && rfi[i].second == 9) {
            const unsigned p = link_->bind_peer;
            switch (link_->bind_state) {
                case gnd::BIND_RUNNING:
                    snprintf(hint, hcap, "ENTER: CANCEL");
                    snprintf(status, scap, "BINDING  %ds", link_->bind_secs_left);
                    break;
                case gnd::BIND_OK:
                    snprintf(status, scap, "BOUND  %02X:%02X:%02X:%02X",
                             (p >> 24) & 0xFF, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
                    break;
                case gnd::BIND_FAILED:
                    snprintf(hint, hcap, "ENTER: RETRY");
                    snprintf(status, scap, "NO AIR UNIT FOUND");
                    break;
                default: break;
            }
        }
    }
}

// The canopy has no telemetry link of its own: every figure it shows is read
// off the Betaflight OSD canvas (msp_osd.cpp, scrape_locked). So the help for
// HUD Style, while CANOPY is the value under the cursor, is the list of
// elements the pilot has to enable in Betaflight for the canopy to be
// complete - said here, where the choice is made, rather than in a manual.
// And how they must sit: an element is recognised by its glyphs, so one that
// overlaps a neighbour can lose them - a voltage whose V was blanked by the
// next element's padding stayed on screen as a bare figure under the canopy.
// SYSTEM > Screen Mode's choices: the modes the connector in use advertises,
// progressive only (interlaced ones do not composite here), each size and
// rate once, largest and fastest first. Read without probing the connector -
// a probe re-reads the EDID - and made at the first frame after a menu open
// (frame_take, under osd_mutex): made here, lazily, it was filled on the HUD's
// thread while drawing, unlocked, as the keys' thread cleared it.
const std::vector<OSD::ScreenModeOpt>& OSD::screen_modes() { return screen_modes_; }

void OSD::refill_screen_modes() {
    if (!dev || !dev->has_output()) return;      // no connector yet: asked again next frame
    screen_modes_stale_ = false;
    screen_modes_.clear();
    for (const display::Mode& m : dev->modes()) {
        const ScreenModeOpt o{m.w, m.h, m.hz};
        if (std::none_of(screen_modes_.begin(), screen_modes_.end(), [&](const ScreenModeOpt& x) {
                return x.w == o.w && x.h == o.h && x.hz == o.hz; }))
            screen_modes_.push_back(o);
    }
    std::sort(screen_modes_.begin(), screen_modes_.end(), [](const ScreenModeOpt& a, const ScreenModeOpt& b) {
        return a.w * a.h != b.w * b.h ? a.w * a.h > b.w * b.h : a.hz > b.hz; });
    menu_screen_mode = -1;                       // a choice staged against the old list goes with it
    screen_mode_menu();
    menu_refresh_options();
}

// screen_mode as a menu index: 0 when unset (Auto). A mode the screen does not
// advertise - set by hand, or from another display - reads as the mode the
// display was actually given, which is what kestrel fell back to.
int OSD::screen_mode_saved() {
    const std::vector<ScreenModeOpt>& modes = screen_modes();
    unsigned w, h, hz;
    const std::string s = screen_confirm_
        ? (screen_confirm_mode_ == "auto" ? std::string() : screen_confirm_mode_)
        : settings_.get(display::setting::screen_mode(screen_id_));
    if (sscanf(s.c_str(), "%ux%u@%u", &w, &h, &hz) != 3) return 0;
    for (size_t i = 0; i < modes.size(); i++)
        if (modes[i].w == (int)w && modes[i].h == (int)h && modes[i].hz == (int)hz) return (int)i + 1;
    if (dev && dev->has_output())
        for (size_t i = 0; i < modes.size(); i++)
            if (modes[i].w == dev->mode().w && modes[i].h == dev->mode().h &&
                modes[i].hz == (int)dev->mode().hz)
                return (int)i + 1;
    return 0;
}

void OSD::begin_screen_mode_confirm(const std::string& mode) {
    pthread_mutex_lock(&osd_mutex);
    screen_confirm_ = true;
    screen_confirm_mode_ = mode;
    // Long enough to find the button once the picture is back; short enough
    // that a black screen is not a long wait.
    screen_confirm_deadline_ms_ = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) + 15000;
    menu_open_at_start = true;
    render_requested = true;
    hud_wake_.poke();
    pthread_mutex_unlock(&osd_mutex);
}

int OSD::sync_confirm_left_s() {
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now >= sync_confirm_deadline_ms_ ? 0 : (int)((sync_confirm_deadline_ms_ - now + 999) / 1000);
}

int OSD::screen_confirm_left_s() {
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now >= screen_confirm_deadline_ms_ ? 0 : (int)((screen_confirm_deadline_ms_ - now + 999) / 1000);
}

int OSD::screen_mode_menu() {
    if (menu_screen_mode < 0) menu_screen_mode = screen_mode_saved();
    return menu_screen_mode;
}

std::string OSD::screen_mode_label(int idx) {
    const std::vector<ScreenModeOpt>& modes = screen_modes();
    if (idx <= 0 || idx > (int)modes.size()) return "Auto";
    char b[32];
    snprintf(b, sizeof(b), "%dx%d @ %d Hz", modes[idx - 1].w, modes[idx - 1].h, modes[idx - 1].hz);
    return b;
}

// The WiFi board's internal antenna cannot hold a link for the video stream,
// and the radio sits next to the RC link: say so on the row, on or off.

const char* OSD::menu_help_warning(int tab, int i) {
    if (tab == kTabSystem && i == 3 && screen_confirm_) {
        static char ask[96];
        snprintf(ask, sizeof(ask), "Keep this mode? Enter keeps it - going back in %d s.",
                 screen_confirm_left_s());
        return ask;
    }
    if (tab == 3 && i == kDispRefreshSync && sync_confirm_) {
        static char ask[96];
        snprintf(ask, sizeof(ask), "Keep it? Enter keeps it - back to %s in %d s.",
                 refresh_sync_label(sync_confirm_prev_), sync_confirm_left_s());
        return ask;
    }
    if (tab == kTabSystem && i == 3 && menu_dirty[kTabSystem][3])
        return "Enter tries it: the app restarts, then asks whether to keep it.";
    if (tab == kTabSystem && i == 4)
        return "Warning: Before enabling, plug an external antenna to the wifi uFL connector.";
    return nullptr;
}

// The DVR Source row's value as the menu shows it: FPV or SCREEN.
int OSD::dvr_source_now() const {
    return dvr_screen ? 1 : 0;
}

int OSD::hud_row(int shown) const {
    const bool voltage_listed = menu_hud_style == kHudCanopy;
    return (!voltage_listed && shown >= kHudRowVoltage) ? shown + 1 : shown;
}

const char* OSD::menu_help_text(int tab, int i, const char* fallback) {
    if (tab == 3 && i == kDispFrameMode) {
        // Lowest latency first; what each one costs.
        switch (menu_frame_mode) {
        case kFrameSplit:
            return "Lowest latency: the top of the picture goes up before the bottom.\n"
                   "May tear in the middle on fast motion. No gain on a screen that buffers frames.";
        case kFrameWholeDecode:
            return "Highest latency, the most conservative: the decoder only gets\n"
                   "whole pictures.";
        default:
            return "Low latency, whole pictures: slices are decoded as they land, and\n"
                   "only whole pictures go up. No tearing. The default.";
        }
    }
    if (tab == 3 && i == kDispRefreshSync) {
        // With the screen's refresh and the camera's rate known (the display
        // lock's, measured): what the value shown does to this screen in this
        // video mode, and while it is the one running, whether it holds now.
        const display::RefreshSync rs = refresh_sync();
        const float hz = rs.mode_hz, fps = rs.camera_fps;
        if (hz > 0 && fps > 0) {
            static char t[256];
            const bool live = menu_refresh_sync == rs.mode;
            const char* why = rs.why;
            char now[96] = "";
            if (live && menu_refresh_sync) {
                if (rs.holding) snprintf(now, sizeof(now), " Holding now.");
                else if (why) snprintf(now, sizeof(now), " Now: %s.", why);
            }
            switch (menu_refresh_sync) {
            case 1: {
                const float k = std::round(hz / fps);
                if (k >= 1 && std::fabs(hz - k * fps) <= 0.01f * hz)
                    snprintf(t, sizeof(t), "Holds this screen's %.2f Hz just after each picture, %.0f refresh%s a\n"
                             "camera frame (%.2f fps). Off if the screen blanks.%s", hz, k, k > 1 ? "es" : "", fps, now);
                else
                    snprintf(t, sizeof(t), "Cannot hold here: %.2f Hz is not a whole multiple of the camera's\n"
                             "%.2f fps. Screen Mode Auto picks one that is, or use FREE.", hz, fps);
                return t;
            }
            case 2: {
                if (fps > hz / 0.999f) {
                    snprintf(t, sizeof(t), "Cannot slow this screen: the camera (%.2f fps) is faster than\n"
                             "its %.2f Hz. Pick a faster screen mode.", fps, hz);
                    return t;
                }
                const float k = std::max(1.0f, std::floor(hz / fps + 1e-6f));
                char per[40];
                if (k > 1) snprintf(per, sizeof(per), "%.0f refreshes a camera frame", k);
                else snprintf(per, sizeof(per), "a refresh a camera frame");
                snprintf(t, sizeof(t), "Slows this screen from %.2f Hz to %.2f Hz in this video mode, %s.\n"
                         "Only for screens that take any frame length.%s", hz, k * fps, per, now);
                return t;
            }
            default:
                snprintf(t, sizeof(t), "The screen refreshes on its own timing: %.2f Hz, the camera %.2f fps.\n"
                         "ON or FREE hold it in step with the pictures.", hz, fps);
                return t;
            }
        }
        switch (menu_refresh_sync) {
        case 1:  return "Holds the refresh just after each picture arrives, a line at a time.\n"
                        "Needs a refresh in step with the camera (Screen Mode Auto). Off if the screen blanks.";
        case 2:  return "Each frame made as long as the camera's, at the fastest mode: for screens that\n"
                        "show their refresh change (118, 100 Hz...) without blanking. Lowest latency.";
        default: return "The screen refreshes on its own timing. ON or FREE hold it in step with\n"
                        "the pictures - FREE only for a screen that takes any frame length.";
        }
    }
    if (tab == kTabDvr && i == 0) {
        switch (menu_dvr_source) {
        case 1:  return "Records the screen - video and HUD, as you see them - 60 times a second.\n"
                        "Adds about 1 ms to the FPV feed's latency while recording.";
        default: return "Records the FPV stream as the air unit sends it: full quality, no HUD.\n"
                        "No impact on the FPV feed's latency.";
        }
    }
    if (tab == kTabSystem && i == 3) {
        if (menu_dirty[kTabSystem][3] || screen_confirm_) return "";   // the warning says it
        static std::string mode;
        char now[64] = "--";
        if (dev && dev->has_output())
            snprintf(now, sizeof(now), "%dx%d @ %d Hz", dev->mode().w,
                     dev->mode().h, dev->mode().hz);
        mode = std::string("Showing ") + now + ". Auto: the fastest 1080p above 60 Hz, else 720p above\n"
               "60 Hz, else 1080p60, else 720p60; then in step with the camera.";
        return mode.c_str();
    }
    if (tab == kTabSystem && i == kSysTimezone && menu_zone_ >= 0) {
        // Which zone exactly, and whose rules the channel page uses.
        static std::string zone_help;
        const zones::Zone& z = zones::zone(menu_zone_);
        const zones::Country& c = zones::country(z.country);
        zone_help = std::string(z.name) + ". Only the clock follows this; the channel rules are " +
                    c.name + "'s either way" +
                    (zones::has_rules(z.country) ? "." : std::string(", and none are known for it yet."));
        return zone_help.c_str();
    }
    if (tab == kTabSystem && i == 4) {
        // While a change is pending, the "Pending: OFF -> ON" line says it
        // all; the antenna warning above it comes from menu_help_warning().
        if (menu_dirty[kTabSystem][4]) return "";
        static std::string wifi;          // rebuilt each frame the row is selected
        wifi = wifi_ap_describe();
        return wifi.c_str();
    }
    if (tab == 2 && i == kHudRowStyle && menu_hud_style == kHudCanopy)
        return "Battery, flight mode and GPS come from the flight controller "
               "(polled over MSP) and the HUD's movement from the camera's IMU.\n"
               "Link quality is still read off Betaflight's OSD: enable that "
               "element there.";
    return fallback;
}

// A row's value as words: menu_value_text without the "< >" chevrons and
// without the dirty mark, so it can be compared and quoted.
// What a change did, said in the log as the menu shows it: a field test or a
// screen recording's log then tells which row was changed, and to what.
void OSD::menu_log_applied(const char* how) {
    const std::vector<MenuItem> rows = menu_items(menu_tab);
    if (menu_tab < 0 || menu_tab >= kMenuTabs || menu_index < 0 || menu_index >= (int)rows.size()) return;
    char v[64];
    menu_plain_value(menu_tab, menu_index, v, sizeof(v));
    printf("menu: %s > %s: %s (%s)\n", kTabNames[menu_tab], rows[menu_index].label ? rows[menu_index].label : "?", v, how);
}

void OSD::menu_plain_value(int tab, int i, char* out, size_t cap) {
    char raw[64];
    menu_value_text(tab, i, raw, sizeof(raw));
    const char* b = raw;
    if (b[0] == '<' && b[1] == ' ') b += 2;
    size_t n = strlen(b);
    if (n >= 2 && b[n - 2] == ' ' && b[n - 1] == '*') n -= 2;
    if (n >= 2 && b[n - 2] == ' ' && b[n - 1] == '>') n -= 2;
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n); out[n] = '\0';
}

// AIR UNIT > INFO's capabilities rows: their help, which also marks them.
static const char kAirCapsHelp[] =
    "What this air unit's kestrel-air has on. Air Timing: its times in each slice. "
    "Radio Clock: pictures stamped on the radio's clock. Frame Clock: its radio's frame, for exact latency. "
    "Sensor Timing: readout and exposure, for STATS. Intra Refresh: no keyframe spikes. "
    "Camera IMU: the camera's gyro with the video. FC IMU: the flight controller's gyro instead. "
    "Bandwidth Cap: takes the VRX's limit. "
    "864p120, 864p144: Video Mode offers a 1536x864 window at 120 or 144 fps.";

// A row label made at run time (the capabilities list), kept for good: a
// MenuItem holds only the pointer, and a set's strings never move.
const char* OSD::menu_intern(const std::string& s) {
    static std::mutex m;
    static std::set<std::string> pool;
    std::lock_guard<std::mutex> g(m);
    return pool.insert(s).first->c_str();
}

std::vector<OSD::MenuItem> OSD::menu_items(int tab) const {
    std::vector<MenuItem> items;
    if (tab == 0) { // Video Tab
        {
            // Grouped by what the setting acts on: the format block first,
            // then the capture settings, then everything applied to the image
            // after it has been captured. Rows render bottom-up, so index 0
            // sits nearest the tab strip.
            items = {
                // Ratio (aspect ratio, sky cmd 0x07) was here but is gone:
                // ACKed on send, but this air unit never reflects it in its
                // own status echo the way every other camera field does, and
                // there is no other way to tell it landed - looks unsupported
                // on this hardware. See ar8030-camera-settings-verification.
                //
                // Anti-Flicker was here too and is also gone: stock's own
                // Camera menu simply has no such row (Scene/EV/Saturation/
                // Sharpness/Contrast/WB/Rotate/Ratio/3D DNR, confirmed
                // against a photo of stock's UI) - it was never a real
                // setting to expose, whatever cmd 0x1F does on the wire.
                // The air unit's video bitrate: AUTO follows the link; a cap is
                // a kestrel-air command (sky cmd 0x40), ignored by stock. Next to
                // Video Mode, the other setting that decides the stream; listed
                // here, handled as row 10 (video_row).
                {"Video Mode", 1},
                air_stock_
                    ? MenuItem{"Max Bitrate", 0, "Needs an fpvOS air unit: this one runs the stock "
                                                 "air app, which ignores the cap.", nullptr, true}
                    : MenuItem{"Max Bitrate", 1, "Cap on the video bitrate. AUTO follows the link. A cap "
                                                 "below it gives a steadier picture and less time on the "
                                                 "air per frame. fpvOS air units only."},
                {"Rotate", 1},
                {"Scene", 1}, {"EV", 1}, {"White Balance", 1},
                {"Saturation", 1}, {"Contrast", 1}, {"Sharpness", 1}, {"3D DNR", 1},
                // Stock's own Display tab (photo-confirmed), not Camera - but
                // it is still a sky-wire command to the air unit (cmd 0x0E,
                // CMD_SET_CHN_FOCUS), so it lives with the other AR8030
                // camera settings here rather than in our DISPLAY tab, which
                // only holds goggle-local drawing settings.
                {"Focus Mode", 1}
            };
        }
    } else if (tab == 1) { // RF Tab
        // Bind and Channel are actions (type 3): Enter does the thing, the
        // arrows do nothing, and the value column says what Enter will do
        // rather than echoing a value there is not.
        for (auto& it : ar_rf_items()) {
            switch (it.second) {
                case 9: items.push_back({it.first, 3,
                    "Pairs this VRX with an air unit. Put the air unit in its "
                    "pairing mode first: hold its bind button until the LED turns red.",
                    "ENTER: BIND"}); break;
                case 5: items.push_back({it.first, 3,
                    "Where the link sits, what is on every channel and what your "
                    "country allows. With SYSTEM > Time Zone set: AUTO takes the quietest "
                    "allowed channel when the link connects, AUTO HOP lets the air unit hop by itself.",
                    "ENTER: OPEN"}); break;
                case 1: items.push_back({it.first, 1,
                    "Radio output. More reaches further and runs hotter. In AUTO, the most "
                    "it may use: the air unit asks for no more than this."}); break;
                case 16: items.push_back({it.first, 1,
                    "FIXED holds the TX Power level. AUTO lets the link ask the air unit for "
                    "what it needs, up to that level."}); break;
                case 8: items.push_back({it.first, 1,
                    "Force the air unit's low-power standby on or off for now."}); break;
                case 11: items.push_back({it.first, 1, link_->radio_fw == 1
                    ? "How wide the video link may go; below it the link picks its width from "
                      "the signal. 40 on a strong link: more bitrate, a shorter radio frame, "
                      "less range. Narrower reaches further. Enter applies it, no restart."
                    : "How wide the video link may go. 40 widens it on a strong link: more "
                      "bitrate, a shorter radio frame, less range. 20 reaches further. "
                      "Enter applies it: the radio restarts, the picture is back in ~10 s."}); break;
                case 12: items.push_back({it.first, 1,
                    "How long the radio's frame is. SHORT gets each slice out sooner; "
                    "STANDARD is stock's, with a longer, sturdier uplink, and what a stock "
                    "air unit always gets. "
                    "HUD > Debug Info shows the frame in use."}); break;
                case 13: items.push_back({it.first, 1,
                    "This VRX's own radio firmware: STOCK, or OPEN8030 (open source) when "
                    "it is installed. The air unit's is not changed. Enter applies it: the "
                    "radio restarts, the picture is back in ~10 s."}); break;
                case 15: items.push_back({it.first, 1,
                    "Each block the air radio has to send again takes the link one MCS "
                    "lower at once, and it climbs back only after 5 s without one: steadier "
                    "latency, less bitrate. OFF, the default: the MCS follows the signal."}); break;
                default: items.push_back({it.first, 1}); break;
            }
        }
    } else if (tab == 2) { // HUD Tab - what is drawn over the video
        items = {
            // Order matches HudRow in osd.hpp - change both together.
            {"HUD Style", 1, "Which overlay is drawn over the video."},
            {"Betaflight OSD", 1, "Draw Betaflight's own OSD across the screen."},
            {"Canopy Voltage", 1, "The canopy's headline voltage: per cell reads the same on any pack size."},
            {"Clock", 1, "When the time is shown at the top of the screen, and how it is written."},
            {"Dynamic HUD", 1, "Let the panels move with the aircraft."},
            {"Debug Info", 1, "A line of radio details under the right panel, for field "
                              "tests: the radio's frame, and the mode it means."}
        };
        // Only the canopy has a headline voltage to set; the rows after it close up (hud_row()).
        if (menu_hud_style != kHudCanopy) items.erase(items.begin() + kHudRowVoltage);
    } else if (tab == 3) { // DISPLAY Tab - how the screen looks
        // This tab was PHYSICS, two cosmetic toggles on their own. The screen
        // and decoration settings that were scattered across HUD and SYSTEM
        // join them, which is what retires a tab holding two rows.
        // Order matches DisplayRow in osd.hpp - change both together.
        items = {
            {"Theme", 1, "Which hues carry which meaning."},
            {"Background Video", 1, "Play the idle scene when nothing is linked."},
            {"Picture Size", 1, "Shrink the whole picture if your optics clip the edges."},
            {"UI Scaling", 1, "Size of the HUD's type. The panels stay put."},
            {"Brightness", 1, "Panel backlight. Lifts video and HUD together."},
            // How pictures are timed onto the screen: worth leaving alone
            // unless chasing latency or smoothness.
            {"ADVANCED", 2},
            {"Frame Mode", 1, "How pictures go from the radio to the screen."},  // see menu_help_text
        };
        // Refresh Sync only where the display controller's frame length can be
        // set (platform::caps), and Sync Margin only while it is ON or FREE: with it
        // OFF the margin does nothing. Sync To (kDispSyncSlice, after it: which
        // slice the lock times the screen to) is hidden; display_lock_slice
        // still applies.
        if (platform::caps().phase_lock)
            items.push_back({"Refresh Sync", 1, ""});                            // see menu_help_text
        if (platform::caps().phase_lock && menu_refresh_sync != 0)
            items.push_back({"Sync Margin", 1, "Refresh Sync's refresh this much later. Fewer pictures miss their refresh (a hitch each), for that much latency: 2 MS, about a third fewer."});
    } else if (tab == kTabDvr) {
        items = {
            // A reading where the screen cannot be recorded: FPV is all there is.
            {"Source", platform::caps().screen_enc ? 1 : 0, "What REC captures: the FPV stream, or the screen."},  // see menu_help_text
            {"Folder", 0, "Where recordings are written."},
            {"Space Left", 0, "Room remaining on the recordings partition."}
        };
    } else if (tab == kTabAirInfo) { // AIR UNIT > INFO - what the linked air unit says it is
        // The first rows are stock's Device Info for the air unit (TX HW / SN / SW
        // Version), worked out as stock does, so they read the same for either air
        // app. The rest only when kestrel-air answers: what makes it fpvOS. Rows
        // come and go with that, so the values go by label (menu_value_text).
        const air::Info &a = air_unit_.info;
        items = {
            {"AIR UNIT", 2},
            {"Model", 0, "The air unit on the other end of the link, as it reports itself."},
            {"HW Version", 0, "As stock shows it: the board's version, then the RF board's."},
            {"SN", 0, "As stock shows it: the model, then the radio chip's ID. Caddx's air app "
                      "always reports 0 there, so an Ascent reads H_Sky_000000."},
            {"SW Version", 0, "The Caddx firmware on the air unit, as stock shows it. With fpvOS it "
                              "is the stock firmware fpvOS runs on."},
        };
        if (osd_vars.radio_link.state == 2 && (a.kestrel || a.kv_n > 0)) {
            items.push_back({"FPVOS", 2});
            items.push_back({"fpvOS", 0, "The fpvOS air image the unit was flashed with."});
            items.push_back({"kestrel-air", 0, "fpvOS's air app: it runs the camera and the radio in "
                                               "place of Caddx's."});
            items.push_back({"Radio Driver", 0, "What drives the AR8030 radio on the air unit."});
            items.push_back({"Radio Firmware", 0, "What runs on the air unit's AR8030 radio chip: the stock "
                                                  "firmware, or Open8030, the open one (Lite and Lite+)."});
            items.push_back({"Kernel", 0, "The air unit's Linux kernel (the stock one)."});
            items.push_back({"Camera", 0, "The camera's image sensor."});
            // What it has on, as one comma-separated list: a row of type 4,
            // which the blade wraps over as many lines as it takes.
            if (a.caps_n > 0) {
                items.push_back({"CAPABILITIES", 2});
                std::string list;
                for (int k = 0; k < a.caps_n; k++) list += std::string(k ? ", " : "") + a.caps[k];
                items.push_back({menu_intern(list), 4, kAirCapsHelp});
            }
        }
    } else if (tab == kTabAirSerial) { // AIR UNIT > SERIAL - what the flight controller stream carries
        // Read-only, and counted here at the goggle from the telemetry stream the air
        // unit relays (nothing extra is sent for it). Values go by label (menu_value_text).
        items = {
            {"FLIGHT CONTROLLER", 2},
            {"Receiving", 0, "Whether MSP frames from the flight controller are coming in now."},
            {"Frames", 0, "MSP frames received and understood, with the rate."},
            {"Data", 0, "Flight controller frames arriving, in bytes a second. Measured while this tab is open."},
        };
    } else { // SYSTEM
        items = {
            {"Build", 0},
            {"Version", 0},
            {"Decoder", 0},
            // Help is built live by menu_help_text: the mode in use.
            {"Screen Mode", 1, ""}
        };
        // Help is built live by menu_help_text: name, password, channel.
        items.push_back({"WiFi AP", 1, ""});
        // Where you fly: the clock's time zone and the country whose rules the
        // channel page shows. Order matches SysRow in osd.hpp.
        items.push_back({"WHERE YOU FLY", 2});
        items.push_back({"Amateur Licence", 1, "Whether you hold an amateur radio licence. With it, the amateur "
                                               "band of your country's ITU region reads OK on the channel page; "
                                               "your licence sets the power."});
        items.push_back({"Area", 1, "Where you fly, starting with the part of the world. Sets the clock and "
                                    "which channels the channel page allows. Or set it from a phone, on the "
                                    "VRX's web page."});
        items.push_back({"Country", 1, "The country you fly in: its rules for the radio's channels."});
        // Only where the country has more than one time zone (in this area):
        // which one the clock shows. The channel rules are per country and do
        // not depend on it. Last, so hiding it moves no other row.
        if (menu_zone_ >= 0) {
            const zones::Zone& z = zones::zone(menu_zone_);
            if (zones::zones_in(z.area, z.country).size() > 1)
                items.push_back({"Timezone", 1, "Which of the country's time zones the clock shows. "
                                                "The channel rules are the country's either way."});
        }
    }
    return items;
}

// See the header. Steps the row's own handler to find its values, then puts it
// back exactly where it was.
//
// Two rows are excluded because there is nothing to enumerate: Brightness
// and UI Scaling are ranges far longer than the cap. Walking them would spend a dozen steps to
// find that out. The walk itself is harmless now - menu_stepping_ keeps the
// steps off the HUD and out of the settings file.
int OSD::menu_options(int tab, int index, char out[kMenuOptMax][kMenuOptLen], int* cur) {
    if (cur) *cur = 0;
    const bool self_saving = (tab == 3 && index == kDispBrightness) ||
                             (tab == 3 && index == kDispUiScale);
    if (self_saving) return 0;

    if (tab == menu_opt_tab_ && index == menu_opt_idx_) {
        if (cur) *cur = menu_opt_cur_;
        memcpy(out, menu_opt_, sizeof(menu_opt_));
        return menu_opt_n_;
    }

    // Stepping, not setting: the walk moves the row's mirror to read the text
    // off it and puts it back, and neither the HUD nor the settings file wants
    // to hear about the values it passed through on the way.
    const bool was_stepping = menu_stepping_;
    menu_stepping_ = true;
    // Stepping back is not undoing for the time zone rows (a step on Area or
    // Country starts the next one over at its first zone), so its walk puts
    // the zone itself back.
    const int zone_before = menu_zone_;

    // Stepping stops at the ends while menu_stepping_ is set (menu_step), so
    // a set and a clamped range look the same from here: walk down until the
    // value stops moving - that is the start, and the number of steps it
    // took is where the value in force sits - then walk up collecting until
    // it stops again. A walk that never reaches an end inside the cap is a
    // long range, which has no list to draw; the ladder shows it instead.
    char t[64], prev[64];
    menu_value_text(tab, index, prev, sizeof(prev));
    int back = 0;
    for (; back < kMenuOptMax; back++) {
        menu_apply_change(tab, index, -1);
        menu_value_text(tab, index, t, sizeof(t));
        if (!strcmp(t, prev)) break;
        snprintf(prev, sizeof(prev), "%s", t);
    }
    int n = 0, fwd = 0;
    if (back < kMenuOptMax) {
        menu_value_text(tab, index, out[0], kMenuOptLen);
        n = 1;
        while (n < kMenuOptMax) {
            menu_apply_change(tab, index, +1);
            menu_value_text(tab, index, t, sizeof(t));
            if (!strcmp(t, out[n - 1])) break;
            fwd++;
            snprintf(out[n++], kMenuOptLen, "%s", t);
        }
        if (n >= kMenuOptMax) n = 0;    // never found the top: a range
    }
    // Put it back exactly where it was: the walk is fwd steps up from the
    // start, and the value in force is back steps up from it.
    for (; fwd > back; fwd--) menu_apply_change(tab, index, -1);
    for (; fwd < back; fwd++) menu_apply_change(tab, index, +1);
    menu_zone_ = zone_before;
    menu_stepping_ = was_stepping;

    menu_opt_tab_ = tab; menu_opt_idx_ = index;
    menu_opt_n_ = n; menu_opt_cur_ = (n > 0) ? back : 0;
    memcpy(menu_opt_, out, sizeof(menu_opt_));
    if (cur) *cur = menu_opt_cur_;
    return n;
}

// The values either side of the current one, for a row that is a range rather
// than a set.
//
// A range has no list to draw, so the value column used to show one number in
// the middle of an empty panel - which says what the setting reads but nothing
// about where that sits, or how far a press moves it. A short ladder either
// side answers both, and costs nothing to find: the same step-and-read trick
// menu_options uses, walked a few steps instead of all the way round.
//
// The self-saving rows are the exception. Brightness writes to the panel and
// UI Scaling rewrites the layout this very frame, so walking them to fill a
// ladder would strobe the screen. Those two are computed from their own
// domains instead - the one place a setting's range is spelled out twice, and
// worth it to keep the draw free of side effects.
int OSD::menu_range_ladder(int tab, int index, int span,
                           char out[kMenuOptMax][kMenuOptLen]) {
    if (span < 1) span = 1;
    const int n = span * 2 + 1;
    if (n > kMenuOptMax) return 0;
    // Whole rows, not just the terminator: the render thread reads this buffer
    // while it is being filled, and a partial write over longer leftovers is
    // the one way it could come back without a terminator at all.
    memset(out, 0, (size_t)kMenuOptMax * kMenuOptLen);

    // menu_value_text writes "< X >" for a nudgeable row; the chevrons said
    // which key to press on a menu that no longer works that way.
    // The pending mark goes too: it belongs to the row, not to each of the
    // values it could take, and menu_value_text stamps every reading it makes
    // while a row is dirty - including the ones this walk asks for.
    auto strip = [](char* t) {
        size_t len = strlen(t);
        if (len >= 2 && t[len-1] == '*' && t[len-2] == ' ') {
            t[len - 2] = '\0';
            len -= 2;
        }
        if (len >= 4 && t[0] == '<' && t[1] == ' ' && t[len-2] == ' ' && t[len-1] == '>') {
            memmove(t, t + 2, len - 4);
            t[len - 4] = '\0';
        }
    };

    if (tab == 3 && index == kDispBrightness) {   // 0..100 in fives
        for (int k = -span; k <= span; k++) {
            int v = menu_brightness + k * 5;
            if (v < 0 || v > 100) continue;
            snprintf(out[k + span], kMenuOptLen, "%d", v);
        }
        return n;
    }
    if (tab == 3 && index == kDispUiScale) {      // 0.5x..2.0x in tenths
        for (int k = -span; k <= span; k++) {
            float v = menu_ui_scale + (float)k * 0.1f;
            if (v < 0.5f - 0.001f || v > 2.0f + 0.001f) continue;
            snprintf(out[k + span], kMenuOptLen, "%.1fx", v);
        }
        return n;
    }

    const bool was_stepping = menu_stepping_;
    menu_stepping_ = true;
    const int zone_before = menu_zone_;   // see menu_options

    char centre[64];
    menu_value_text(tab, index, centre, sizeof(centre));
    snprintf(out[span], kMenuOptLen, "%s", centre);
    strip(out[span]);

    // Outwards in both directions, stopping where the value stops moving -
    // which is how a clamped range says it has reached its end.
    for (int dir = -1; dir <= 1; dir += 2) {
        char prev[64];
        snprintf(prev, sizeof(prev), "%s", centre);
        int steps = 0;
        for (int k = 1; k <= span; k++) {
            menu_apply_change(tab, index, dir);
            char t[64];
            menu_value_text(tab, index, t, sizeof(t));
            // A step that changed nothing is the end of the range - and it is
            // NOT counted, because it also needs no undoing. Counting it was
            // the whole bug in Overscan: pressing down at 10px walked to 0,
            // stopped against the clamp, and then unwound two steps from 0,
            // which put the setting back at 10px and ate the press.
            if (!strcmp(t, prev)) break;
            steps++;
            snprintf(out[span + dir * k], kMenuOptLen, "%s", t);
            strip(out[span + dir * k]);
            snprintf(prev, sizeof(prev), "%s", t);
        }
        while (steps-- > 0) menu_apply_change(tab, index, -dir);
        menu_zone_ = zone_before;
    }
    menu_stepping_ = was_stepping;
    return n;
}

// Work out what the selected row's value column will show, for the render
// thread to read.
//
// Both ways of discovering a row's values - menu_options walking a set until
// it comes back round, menu_range_ladder walking a few steps either side of a
// range - find them by stepping the row's own handler and putting it back
// afterwards. That is a write, and it used to happen inside the draw: a key
// that landed mid-walk was applied to whatever temporary value the walk had
// left behind, and then the walk's own restore put its result back. Overscan
// would not go below 10px because the ladder was standing two steps below it
// every frame and undoing each press.
//
// So both walks run here instead, on the thread that owns the menu state and
// under the lock its keys are handled with, whenever the selection or a value
// changes. The draw only reads.
void OSD::menu_refresh_options() {
    // Drop the set cache first. It is keyed by the row, not by what the row
    // reads, and after a press the cached position would be stale.
    menu_opt_tab_ = -1;
    menu_opt_idx_ = -1;
    menu_lad_tab_ = menu_tab;
    menu_lad_idx_ = menu_index;
    menu_lad_n_   = 0;
    menu_lad_cur_ = 0;

    // Chevrons and the pending mark off, so the list holds bare values.
    auto strip = [](char* t) {
        size_t len = strlen(t);
        if (len >= 2 && t[len-1] == '*' && t[len-2] == ' ') { t[len - 2] = '\0'; len -= 2; }
        if (len >= 4 && t[0] == '<' && t[1] == ' ' && t[len-2] == ' ' && t[len-1] == '>') {
            memmove(t, t + 2, len - 4);
            t[len - 4] = '\0';
        }
    };

    // A set: every value, in the row's own order, and where the current one
    // sits in it. This is the list the column draws.
    char opts[kMenuOptMax][kMenuOptLen];
    int cur = 0;
    int n = menu_options(menu_tab, menu_index, opts, &cur);
    if (n > 0) {
        for (int k = 0; k < n; k++) {
            snprintf(menu_lad_[k], kMenuOptLen, "%s", opts[k]);
            strip(menu_lad_[k]);
        }
        menu_lad_n_   = n;
        menu_lad_cur_ = cur;
        return;
    }

    // A range too long to list: the values either side of the current one,
    // ascending, with the ends left off rather than wrapped.
    char lad[kMenuOptMax][kMenuOptLen];
    const int m = menu_range_ladder(menu_tab, menu_index, kMenuLadderSpan, lad);
    for (int k = 0; k < m; k++) {
        if (!lad[k][0]) continue;
        if (k == kMenuLadderSpan) menu_lad_cur_ = menu_lad_n_;
        snprintf(menu_lad_[menu_lad_n_++], kMenuOptLen, "%s", lad[k]);
    }
}

// The Clock row is one choice made of two settings - when the time shows
// (clock_show: 0 off, 1 idle only, 2 always) and how it is written
// (clock_mode: 1 = 12H, 2 = 24H): OFF, then idle only and always, each in 12H
// and 24H. OFF leaves the format as it was, so coming back to it keeps it.
static const int kClockChoices = 5;
static const char* const kClockLabels[kClockChoices] =
    { "OFF", "IDLE ONLY 12H", "IDLE ONLY 24H", "ON 12H", "ON 24H" };
static int clock_choice(int show, int mode) {
    return show <= 0 ? 0 : 1 + (show - 1) * 2 + (mode == 2 ? 1 : 0);
}

// Rows that take effect the moment the arrows move them.
//
// These write the setting as they go, so there is nothing for Enter to apply
// and nothing pending to mark: the "*" would be claiming the screen and the
// file disagree with each other when they do not. Everything else stays on
// Enter, either because applying it costs something real (the air unit has to
// be told, the background player has to be started or stopped) or because it
// is a capture the row makes at the moment you press it.
bool OSD::menu_row_applies_live(int tab, int index) {
    if (tab == 2) {
        // Every HUD row writes as it goes.
        // (The headers cannot be selected, so which row `index` is makes no difference.)
        return true;
    }
    if (tab == 3)   // DISPLAY: all but Background Video, which owns a player
        return index != kDispBgVideo;
    // SYSTEM: Screen Mode restarts the app and WiFi AP waits for Enter, after
    // its antenna warning has been shown; neither applies as it is stepped.
    // The time zone applies as it is walked: the clock follows it.
    if (tab == kTabSystem) return index >= kSysLicence && index <= kSysTimezone;
    return false;
}

// Section headers (type 2) sit in the row list to break it into groups but
// are not a row anyone can land on - there is nothing to set. Called
// wherever a tab is entered or changed and the row index gets reset, so the
// cursor opens on the first real setting rather than a label.
int OSD::menu_first_selectable_row(int tab) {
    std::vector<MenuItem> rows = menu_items(tab);
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[i].type != 2) return i;
    return 0;
}

// Apply one left/right step to a menu row.
//
// Extracted from the key handler so it can be driven from somewhere other than
// a keypress - menu_options() below steps a row through its own values to find
// out what they are, which is how a value column gets a list without a second
// table of every setting's domain to keep in sync.
void OSD::menu_apply_change(int tab, int index, int dir) {
    const int menu_tab = tab, menu_index = index;
            if (menu_tab == 0) {
       switch (video_row(menu_index)) {
           case 0: {
               int n = video_mode_names.empty() ? 1 : (int)video_mode_names.size();
               // Past the modes this air unit does not have (864p120 needs kestrel-air's).
               int v = menu_video_mode;
               for (int k = 0; k < n; k++) {
                   const int s = menu_step(v, dir, n);
                   if (s == v) break;
                   v = s;
                   if (video_mode_ok(v)) break;
               }
               if (video_mode_ok(v)) menu_video_mode = v;
               break;
           }
           case 1: menu_cam_flip = menu_step(menu_cam_flip ? 1 : 0, dir, 2) != 0; break;
           case 2: menu_cam_scene    = menu_step(menu_cam_scene, dir, kSceneCount); break;
           case 3: menu_cam_ev       = menu_step(menu_cam_ev, dir, kEvCount); break;
           case 4: menu_cam_wb       = menu_step(menu_cam_wb, dir, kWbCount); break;
           case 5: menu_cam_sat      = std::min(kCamSatMax,      std::max(0, menu_cam_sat + dir)); break;
           case 6: menu_cam_contrast = std::min(kCamContrastMax, std::max(0, menu_cam_contrast + dir)); break;
           case 7: menu_cam_sharp    = std::min(kCamSharpMax,    std::max(0, menu_cam_sharp + dir)); break;
           case 8: menu_cam_3dnr    = menu_step(menu_cam_3dnr, dir, kDnrCount); break;
           case 9: menu_cam_focus = menu_step(menu_cam_focus ? 1 : 0, dir, 2) != 0; break;
           case 10: menu_ar_maxbr = menu_step(menu_ar_maxbr, dir, kArMaxBrCount); break;
           default: break;
       }
    } else if (menu_tab == 1) {
       auto rfi = ar_rf_items();
       if (menu_index >= 0 && menu_index < (int)rfi.size()) {
           switch (rfi[menu_index].second) {
               case 5: break;   // Channel is read-only (see above)
               case 1:
                   {
                       // The levels this air unit is offered (system/air/power.hpp);
                       // none before it has said what it is.
                       const int prj = air_radio_.prj;
                       if (prj <= 0) break;
                       // The level: the fixed level, or AUTO's top, among the ones the
                       // mode has.
                       const bool fine = air_radio_.fine;
                       const int cur = ar_pwr_fit(prj, menu_ar_power, fine);
                       const bool autom = ar_pwr_is_auto(cur);
                       int t[kArPwrTopsMax];
                       const int n = ar_pwr_tops(prj, fine, autom, t);
                       if (n <= 0) break;
                       int i = 0;
                       while (i + 1 < n && t[i] < ar_pwr_top(cur)) i++;
                       i = menu_step(i, dir, n);
                       menu_ar_power = autom ? t[i] + 1 : t[i];
                   }
                   break;
               case 16:
                   {
                       // The mode, at the same level where the air unit has it there.
                       const int prj = air_radio_.prj;
                       if (prj <= 0) break;
                       const bool fine = air_radio_.fine;
                       const int cur = ar_pwr_fit(prj, menu_ar_power, fine);
                       const int v = ar_pwr_pick(prj, fine, !ar_pwr_is_auto(cur), ar_pwr_top(cur));
                       if (v >= 0) menu_ar_power = v;
                   }
                   break;
               case 8: menu_ar_standby = menu_step(menu_ar_standby ? 1 : 0, dir, 2) != 0; break;  // forced on Enter
               case 11:   // applied on Enter; stock's radio: 20 or 40 only
                   if (link_view().radio_fw == 1) menu_ar_maxbw = menu_step(menu_ar_maxbw, dir, kArMaxBwCount);
                   else menu_ar_maxbw = menu_ar_maxbw == 4 ? 3 : 4;
                   break;
               case 14: menu_ar_maxmcs = menu_step(menu_ar_maxmcs, dir, kArMaxMcsCount); break;  // applied on Enter
               case 15: menu_ar_steady = menu_ar_steady ? 0 : 1; break;  // applied on Enter
               case 12: menu_ar_frame = menu_step(menu_ar_frame, dir, kArFrameCount); break;  // applied on Enter
               case 13: menu_ar_fw = menu_step(menu_ar_fw, dir, kArFwCount); break;  // applied on Enter
               default: break;
           }
       }
    } else if (menu_tab == 2) { // HUD - rows as listed in menu_items(), see hud_row()
        if (hud_row(menu_index) == kHudRowStyle) {
            menu_hud_style = menu_step(menu_hud_style, dir, kHudStyleCount);
            if (!menu_stepping_) {
                hud_style = menu_hud_style;
                settings_.set(hud::setting::hud_overlay, hud_style);
            }
        } else if (hud_row(menu_index) == kHudRowBfOsd) {
            menu_bf_osd = menu_step(menu_bf_osd ? 1 : 0, dir, 2) != 0;
            if (!menu_stepping_) {
                bf_osd = menu_bf_osd;
                settings_.set(hud::setting::bf_osd, bf_osd ? 1 : 0);
            }
        } else if (hud_row(menu_index) == kHudRowVoltage) {
            menu_volt_mode = menu_step(menu_volt_mode, dir, 2);
            if (!menu_stepping_) {
                volt_mode = menu_volt_mode;
                settings_.set(hud::setting::volt_mode, volt_mode);
            }
        } else if (hud_row(menu_index) == kHudRowClock) {
            const int c = menu_step(clock_choice(menu_clock_show, menu_clock_mode), dir, kClockChoices);
            menu_clock_show = c == 0 ? 0 : 1 + (c - 1) / 2;
            if (c > 0) menu_clock_mode = ((c - 1) % 2) ? 2 : 1;
            if (!menu_stepping_) {
                clock_show = menu_clock_show;
                clock_mode = menu_clock_mode;
                settings_.set(hud::setting::clock_show, clock_show);
                settings_.set(hud::setting::clock_mode, clock_mode);
            }
        } else if (hud_row(menu_index) == kHudRowDynamic) {
            menu_hud_reactivity = menu_step(menu_hud_reactivity, dir, 4);
            if (!menu_stepping_) {
                hud_reactivity = menu_hud_reactivity;
                settings_.set(hud::setting::hud_reactivity, hud_reactivity);
            }
        } else if (hud_row(menu_index) == kHudRowDebug) {
            menu_hud_debug = menu_step(menu_hud_debug ? 1 : 0, dir, 2) != 0;
            if (!menu_stepping_) {
                hud_debug = menu_hud_debug;
                settings_.set(hud::setting::hud_debug, hud_debug);
                sd_log::set_enabled(hud_debug);   // the logs follow it: the card's,
                parts_.air_unit.set_logs(hud_debug);   // and the air unit's
            }
        }
    } else if (menu_tab == 3) { // DISPLAY
        if (menu_index == kDispBrightness) {
            // Hardware picture control on the HDMI connector: lifts the
            // video and the OSD together, which is what made the grid
            // read better when the video was accidentally drawn twice.
            menu_brightness += dir * 5;
            if (menu_brightness < 0)   menu_brightness = 0;
            if (menu_brightness > 100) menu_brightness = 100;
            if (!menu_stepping_) {
                if (dev) dev->set_display_property("brightness", menu_brightness);
                settings_.set(hud::setting::brightness, menu_brightness);
            }
        } else if (menu_index == kDispUiScale) {
            // Applies and saves as it is nudged, like Brightness beside it.
            // A scale is judged by looking at what it did to the HUD, and the
            // menu is drawn at this scale too - holding it back until Enter
            // meant choosing a size without being able to see it.
            menu_ui_scale = std::min(2.0f, std::max(0.5f, menu_ui_scale + (float)dir * 0.1f));
            if (!menu_stepping_) {
                osd_vars.ui_scale = menu_ui_scale;
                settings_.set(hud::setting::ui_scale, menu_ui_scale);
            }
        } else if (menu_index == kDispBgVideo) {
            menu_bg_video = menu_step(menu_bg_video ? 1 : 0, dir, 2) != 0;
        } else if (menu_index == kDispTheme) {
            int n = hud_theme_count();
            menu_hud_theme = menu_step(menu_hud_theme, dir, n);
            // A theme is chosen by looking at it, and the menu is drawn over
            // the same HUD it recolours.
            if (!menu_stepping_) {
                hud_theme_idx = menu_hud_theme;
                hud_theme_set(hud_theme_idx);
                settings_.set(hud::setting::hud_theme, hud_theme_idx);
            }
        } else if (menu_index == kDispPicture) {
            // Whole-picture scale: video and HUD shrink together, black all
            // round. Not Overscan, which only pulls the HUD's margins in. For
            // optics that clip the frame, this is the one that shows the lot.
            // Applied as it is nudged, like Overscan: you set it by looking.
            menu_picture_scale = std::min(100, std::max(60, menu_picture_scale + dir * 5));
            if (!menu_stepping_ && dev) {
                dev->set_picture_scale(menu_picture_scale);
                settings_.set(hud::setting::picture_scale, menu_picture_scale);
            }
        } else if (menu_index == kDispFrameMode) {
            // Applied as it is stepped: the difference is seen in the video.
            menu_frame_mode = menu_step(menu_frame_mode, dir, kFrameModeCount);
            if (!menu_stepping_) {
                g_frame_mode = menu_frame_mode;
                settings_.set(base::setting::frame_mode, menu_frame_mode);
                printf("menu: frame mode %s\n", frame_mode_label(menu_frame_mode));
            }
        } else if (menu_index == kDispSyncMargin) {
            // Applied as it is stepped, like Sync To.
            menu_sync_margin = menu_step(menu_sync_margin, dir, 5);
            if (!menu_stepping_) {
                if (dev) dev->set_sync_margin_us(menu_sync_margin * 1000);
                settings_.set(display::setting::display_lock_margin_ms, menu_sync_margin);
                printf("menu: refresh sync margin %d ms\n", menu_sync_margin);
            }
        } else if (menu_index == kDispSyncSlice) {
            menu_sync_slice = menu_step(menu_sync_slice - 1, dir, 2) + 1;
            if (!menu_stepping_) {
                if (dev) dev->set_sync_slice(menu_sync_slice);
                settings_.set(display::setting::display_lock_slice, menu_sync_slice);
                printf("menu: refresh sync to slice %d\n", menu_sync_slice);
            }
        } else if (menu_index == kDispRefreshSync) {
            // The screen's own, like its mode: one that blanks on it keeps it off
            // while another has it on. FREE also moves Screen Mode Auto to the
            // fastest mode (and back), so Auto chooses again.
            // ON and FREE go on trial (sync_confirm_): live at once, kept on
            // Enter, the kept value back after 10 s without it. OFF, or the
            // kept value itself, is kept at once.
            const int was = refresh_sync().mode;
            menu_refresh_sync = menu_step(menu_refresh_sync, dir, 3);
            if (!menu_stepping_) {
                if (!sync_confirm_) sync_confirm_prev_ = was;
                if (dev) {
                    dev->set_sync_mode(menu_refresh_sync);
                    dev->refollow();
                }
                if (menu_refresh_sync == 0 || menu_refresh_sync == sync_confirm_prev_) {
                    settings_.set(display::setting::display_lock(screen_id_), menu_refresh_sync);
                    sync_confirm_ = false;
                    printf("menu: refresh sync %s\n", refresh_sync_label(menu_refresh_sync));
                } else {
                    sync_confirm_ = true;
                    sync_confirm_deadline_ms_ = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()) + 10000;
                    printf("menu: refresh sync %s on trial - Enter keeps it\n", refresh_sync_label(menu_refresh_sync));
                }
            }
        }
    } else if (menu_tab == kTabDvr) {
        if (menu_index == 0) {
            menu_dvr_source = menu_step(menu_dvr_source, dir, 2);
        }
    } else if (menu_tab == kTabSystem) {
        if (menu_index == 3) {
            // Only stages the choice: the display is set up once, at start,
            // so Enter saves it and restarts kestrel (below).
            menu_screen_mode = menu_step(screen_mode_menu(), dir, (int)screen_modes().size() + 1);
        } else if (menu_index == 4 && platform::caps().wifi_ap) {
            // Only stages the choice. Enter applies it (below, with DVR
            // Source), so turning the radio on always passes through the
            // pending state, whose help line is the antenna warning.
            menu_wifi_ap = menu_step(menu_wifi_ap ? 1 : 0, dir, 2) != 0;
        } else if (menu_index == kSysLicence) {
            menu_licence_ = menu_step(menu_licence_, dir, 2);
            if (!menu_stepping_) { zones::set_licensed(menu_licence_ != 0); parts_.radio.region_changed(); }
        } else if (menu_index >= kSysArea && menu_index <= kSysTimezone) {
            menu_zone_ = menu_step_zone(menu_index, dir);
            if (!menu_stepping_) {
                zones::select(menu_zone_);
                parts_.radio.region_changed();   // AUTO's channels
                menu_opt_tab_ = -1;   // Country's and Timezone's lists follow the zone
            }
        }
    }
}

// Where recordings are written, and how much room is left there.
//
// The path is a setting rather than a constant - main() hands the same one to
// the recorder - so the menu reads it (dvr_dir_) rather than repeating the
// default. statvfs is a syscall and the value column asks once a frame while
// the DVR tab is open, so the answer is held for a couple of seconds; free
// space does not move faster than that, and while recording it moves slowly.
static const char* dvr_space_left(const char* dir) {
    static char buf[32] = "--";
    static uint64_t last_us = 0;
    const uint64_t now = get_time_us();
    if (last_us == 0 || now - last_us > 2000000) {
        last_us = now;
        struct statvfs st;
        if (statvfs(dir, &st) == 0) {
            // Binary units, so this agrees with what df on the goggle says
            // rather than being a gigabyte larger for no visible reason.
            const double gb = (double)st.f_bavail * (double)st.f_frsize / 1073741824.0;
            if (gb >= 10.0)     snprintf(buf, sizeof(buf), "%.0f GB", gb);
            else if (gb >= 1.0) snprintf(buf, sizeof(buf), "%.1f GB", gb);
            else                snprintf(buf, sizeof(buf), "%.0f MB", gb * 1024.0);
        } else {
            snprintf(buf, sizeof(buf), "--");   // not mounted
        }
    }
    return buf;
}

// The text shown for one menu row's current value.
//
// Lifted out of draw_menu so more than one thing can ask what a row reads. It
// was 200 lines inline, which meant the value could only ever be rendered in
// the one place the loop happened to be - and a value column needs the same
// answer from somewhere else entirely.
void OSD::menu_value_text(int tab, int i, char* val_buf, size_t cap) {
    const int menu_tab = tab;
    val_buf[0] = '\0';
    
    if (menu_tab == 0) {
        switch (video_row(i)) {
            case 0: {
                int idx = menu_video_mode;
                if (video_mode_names.empty()) { snprintf(val_buf, cap, "< -- >"); break; }
                if (idx < 0 || idx >= (int)video_mode_names.size()) idx = 0;
                snprintf(val_buf, cap, "< %s >", video_mode_names[idx].c_str());
                break;
            }
            case 1: snprintf(val_buf, cap, "< %s >", kRotateLabels[menu_cam_flip ? 1 : 0]); break;
            case 2: snprintf(val_buf, cap, "< %s >", kSceneLabels[menu_cam_scene]); break;
            case 3: snprintf(val_buf, cap, "< %s EV >", kEvLabels[menu_cam_ev]); break;
            case 4: snprintf(val_buf, cap, "< %s >", wb_label(menu_cam_wb)); break;
            case 5: snprintf(val_buf, cap, "< %s >", cam_auto_label(menu_cam_sat)); break;
            case 6: snprintf(val_buf, cap, "< %s >", cam_auto_label(menu_cam_contrast)); break;
            case 7: snprintf(val_buf, cap, "< %s >", cam_auto_label(menu_cam_sharp)); break;
            case 8: snprintf(val_buf, cap, "< %s >", kDnrLabels[menu_cam_3dnr]); break;
            case 9: snprintf(val_buf, cap, "< %s >", menu_cam_focus ? "On" : "Off"); break;
            case 10:
                if (air_stock_) snprintf(val_buf, cap, "FPVOS AIR ONLY");
                else snprintf(val_buf, cap, "< %s >", kArMaxBrLabels[menu_ar_maxbr]);
                break;
            default: break;
        }
    } else if (menu_tab == 1) { // AR8030 RF items
        auto rfi = ar_rf_items();
        if (i >= 0 && i < (int)rfi.size()) {
            switch (rfi[i].second) {
                case 9:     // Bind - an action. The row is the verb; what
                    // Enter does and how the bind is going are the value
                    // column's (menu_action_status), the countdown is the
                    // animation's.
                    val_buf[0] = '\0';
                    break;
                case 5: {   // Channel: where the link is, and whether it hops.
                    // No chevrons, so it does not look adjustable; Enter opens
                    // the channel page, where it is moved.
                    const gnd::ScanSample &cs = osd_vars.chan_scan;
                    int f = 0;
                    bool hop = link_->chan_auto;
                    if (cs.last_update_ms && cs.work_chan < cs.chan_num) {
                        f = cs.freq_mhz[cs.work_chan];
                        hop = cs.auto_mode != 0;
                    }
                    if (!f) f = osd_vars.radio_link.tx_freq;
                    // AUTO / AUTO HOP only where they are offered (a known country).
                    bool offered = false;
                    for (int i = 0; i < std::min(std::max(cs.chan_num, 0), 64) && !offered; i++)
                        offered = cs.usable[i] != 0;
                    const char* mode = !hop || !offered ? "FIXED" : link_->chan_hop ? "AUTO HOP" : "AUTO";
                    if (f) snprintf(val_buf, cap, "%d MHz  %s", f, mode);
                    else   snprintf(val_buf, cap, "--");
                    break;
                }
                case 1: {   // TX power: -- until the air unit has said what it is
                    const int prj = air_radio_.prj;
                    if (prj <= 0) { snprintf(val_buf, cap, "--"); break; }
                    // The level; in AUTO, the most it may use.
                    const int cur = ar_pwr_fit(prj, menu_ar_power, air_radio_.fine);
                    const int top = ar_pwr_top(cur);
                    snprintf(val_buf, cap, ar_pwr_is_auto(cur) ? "< %dmW max >" : "< %dmW >", top);
                    break;
                }
                case 16: {   // FIXED or AUTO
                    const int prj = air_radio_.prj;
                    if (prj <= 0) { snprintf(val_buf, cap, "--"); break; }
                    snprintf(val_buf, cap, "< %s >",
                             ar_pwr_is_auto(ar_pwr_fit(prj, menu_ar_power, air_radio_.fine)) ? "AUTO" : "FIXED");
                    break;
                }
                case 8:     // Standby. Shows the air's current state (menu value is
                    // seeded from it); the arrows + Enter force it on/off as a
                    // momentary override. The air still dictates it on its own
                    // (auto-parks when idle, exits when flying), so a force does
                    // not necessarily stick - see fpv_sky_standby_mode_thread.
                    snprintf(val_buf, cap, "< %s >", menu_ar_standby ? "ON" : "OFF");
                    break;
                case 11:    // Max Bandwidth
                    snprintf(val_buf, cap, "< %s >", kArMaxBwLabels[menu_ar_maxbw]);
                    break;
                case 12:    // Radio Frame
                    snprintf(val_buf, cap, "< %s >", kArFrameLabels[menu_ar_frame]);
                    break;
                case 14:    // Max MCS
                    snprintf(val_buf, cap, "< %s >", ar_maxmcs_label(menu_ar_maxmcs));
                    break;
                case 15:    // Steady Latency
                    snprintf(val_buf, cap, "< %s >", menu_ar_steady ? "ON" : "OFF");
                    break;
                case 13:    // VRX Radio Firmware
                    snprintf(val_buf, cap, "< %s >", kArFwLabels[menu_ar_fw]);
                    break;
                default: break;
            }
        }
        } else if (menu_tab == 2) { // HUD Tab - see menu_items() and hud_row()
            if (hud_row(i) == kHudRowStyle) {
                static const char* kStyleLabels[kHudStyleCount] = { "OFF", "CANOPY", "ARENA", "ARENA FULL" };
                int m = (menu_hud_style >= 0 && menu_hud_style < kHudStyleCount) ? menu_hud_style : 0;
                snprintf(val_buf, cap, "< %s >", kStyleLabels[m]);
            } else if (hud_row(i) == kHudRowBfOsd) {
                snprintf(val_buf, cap, "< %s >", menu_bf_osd ? "ON" : "OFF");
            } else if (hud_row(i) == kHudRowVoltage) {
                snprintf(val_buf, cap, "< %s >", menu_volt_mode == 1 ? "PACK" : "PER CELL");
            } else if (hud_row(i) == kHudRowClock) {
                const int show = (menu_clock_show >= 0 && menu_clock_show < 3) ? menu_clock_show : 1;
                snprintf(val_buf, cap, "< %s >", kClockLabels[clock_choice(show, menu_clock_mode)]);
            } else if (hud_row(i) == kHudRowDynamic) {
                const char* react_labels[4] = { "OFF", "SMALL", "MEDIUM", "EXTREME" };
                int r = (menu_hud_reactivity >= 0 && menu_hud_reactivity < 4) ? menu_hud_reactivity : 2;
                snprintf(val_buf, cap, "< %s >", react_labels[r]);
            } else if (hud_row(i) == kHudRowDebug) {
                snprintf(val_buf, cap, "< %s >", menu_hud_debug ? "ON" : "OFF");
            }
        } else if (menu_tab == 3) { // DISPLAY Tab
            if (i == kDispBrightness) {
                snprintf(val_buf, cap, "< %d >", menu_brightness);
            } else if (i == kDispUiScale) {
                snprintf(val_buf, cap, "< %.1fx >", menu_ui_scale);
            } else if (i == kDispBgVideo) {
                snprintf(val_buf, cap, "< %s >", menu_bg_video ? "ON" : "OFF");
            } else if (i == kDispTheme) {
                snprintf(val_buf, cap, "< %s >", hud_theme_at(menu_hud_theme).name);
            } else if (i == kDispPicture) {
                if (menu_picture_scale >= 100) snprintf(val_buf, cap, "< FULL >");
                else                           snprintf(val_buf, cap, "< %d%% >", menu_picture_scale);
            } else if (i == kDispFrameMode) {
                snprintf(val_buf, cap, "< %s >", frame_mode_label(menu_frame_mode));
            } else if (i == kDispRefreshSync) {
                snprintf(val_buf, cap, "< %s >", refresh_sync_label(menu_refresh_sync));
            } else if (i == kDispSyncSlice) {
                snprintf(val_buf, cap, "< SLICE %d >", menu_sync_slice);
            } else if (i == kDispSyncMargin) {
                snprintf(val_buf, cap, "< %d MS >", menu_sync_margin);
            }
        } else if (menu_tab == kTabDvr) {
            if (i == 0) {
                // Says what the DVR records, which "Record OSD" as a bare
                // ON/OFF never did.
                static const char* kSource[2] = { "FPV", "SCREEN" };
                int src = (menu_dvr_source >= 0 && menu_dvr_source < 2) ? menu_dvr_source : 0;
                snprintf(val_buf, cap, "< %s >", kSource[src]);
            } else if (i == 1) {
                snprintf(val_buf, cap, "%s", dvr_dir_.c_str());
            } else if (i == 2) {
                snprintf(val_buf, cap, "%s", dvr_space_left(dvr_dir_.c_str()));
            }
        } else if (menu_tab == kTabAirInfo) {
            // Stock's placeholders until the air unit has said ("-.-", "--------",
            // "-.-.-"), and its formats after (GlassesUI's Device Info).
            const std::vector<MenuItem> rows = menu_items(tab);
            const char* L = (i >= 0 && i < (int)rows.size()) ? rows[i].label : "";
            const bool linked = osd_vars.radio_link.state == 2;
            const air::Info a = linked ? air_unit_.info : air::Info();
            const int prj = linked ? air_radio_.prj : 0;
            std::string v;
            if (!strcmp(L, "Model")) {
                v = a.get("model");
                if (v.empty() && prj > 0) v = ar_air_name(prj);
                if (v.empty()) v = "--";
            } else if (!strcmp(L, "HW Version")) {
                if (a.hw > 0 && a.rf_hw >= 0)
                    snprintf(val_buf, cap, "%d.%d-%d.%d", a.hw >> 4, a.hw & 15, a.rf_hw >> 4, a.rf_hw & 15);
                else
                    snprintf(val_buf, cap, "-.-");
                return;
            } else if (!strcmp(L, "SN")) {
                if (a.hw > 0 && prj > 0) snprintf(val_buf, cap, "%s%06X", ar_air_sn_prefix(prj), a.chipid & 0xffffff);
                else snprintf(val_buf, cap, "--------");
                return;
            } else if (!strcmp(L, "SW Version")) {
                if (a.hw > 0) snprintf(val_buf, cap, "%d.%d.%d", a.stock[0], a.stock[1], a.stock[2]);
                else snprintf(val_buf, cap, "-.-.-");
                return;
            } else if (!strcmp(L, "Radio Firmware")) {
                // What kestrel-air loaded (an older one does not say: --).
                const std::string fw = a.get("radiofw");
                if (fw == "open8030") v = "Open8030";
                else if (fw == "stock") v = "Stock";
                else v = fw;
            } else if (!strcmp(L, "fpvOS")) {
                v = a.get("os");
            } else if (!strcmp(L, "kestrel-air")) {
                v = a.get("ver");
                if (!v.empty() && a.proto) v += " (protocol " + std::to_string(a.proto) + ")";
            } else if (!strcmp(L, "Radio Driver")) {
                v = a.get("radio");
                if (!v.empty()) v = "ar_libre " + v;
            } else if (!strcmp(L, "Kernel")) {
                v = a.get("kernel");
            } else if (!strcmp(L, "Camera")) {
                v = a.get("sensor");
            } else if (i >= 0 && i < (int)rows.size() && rows[i].help == kAirCapsHelp) {
                return;                                // a line of the capabilities list
            }
            snprintf(val_buf, cap, "%s", v.empty() ? "--" : v.c_str());
        } else if (menu_tab == kTabAirSerial) {
            const std::vector<MenuItem> rows = menu_items(tab);
            const char* L = (i >= 0 && i < (int)rows.size()) ? rows[i].label : "";
            const bool linked = osd_vars.radio_link.state == 2;
            const MspOsd::MspCounts g = msp_osd.msp_counts();
            snprintf(val_buf, cap, "--");
            if (false) {
            } else if (!strcmp(L, "Receiving")) {
                if (linked) snprintf(val_buf, cap, "%s", g.active ? "MSP" : "NOTHING");
            } else if (!strcmp(L, "Data")) {
                if (linked) {
                    if (serial_bps_ >= 1000.0f) snprintf(val_buf, cap, "%.1f KB/s", serial_bps_ / 1000.0f);
                    else snprintf(val_buf, cap, "%.0f B/s", serial_bps_);
                }
            } else if (!strcmp(L, "Frames")) {
                if (linked) snprintf(val_buf, cap, "%u  (%.0f/s)", (unsigned)g.frames, serial_rate_);
            }
        } else if (menu_tab == kTabSystem) {
            if (i == 0) {
                snprintf(val_buf, cap, "%s", KESTREL_GND_BUILD_TIME);
            } else if (i == 1) {
                snprintf(val_buf, cap, "v%s", KESTREL_GND_VERSION);
            } else if (i == 2) {
                snprintf(val_buf, cap, "%s", decoder_name.c_str());
            } else if (i == 3) {
                if (screen_modes().empty())
                    snprintf(val_buf, cap, "--");
                else
                    snprintf(val_buf, cap, "< %s >", screen_mode_label(screen_mode_menu()).c_str());
            } else if (i == 4) {
                if (platform::caps().wifi_ap)
                    snprintf(val_buf, cap, "< %s >", menu_wifi_ap ? "ON" : "OFF");
                else
                    snprintf(val_buf, cap, "N/A");
            } else if (i == kSysLicence) {
                snprintf(val_buf, cap, "< %s >", menu_licence_ ? "YES" : "NO");
            } else if (i >= kSysArea && i <= kSysTimezone) {
                const int z = menu_zone_;
                if (z < 0) snprintf(val_buf, cap, i == kSysArea ? "< NOT SET >" : "--");
                else if (i == kSysArea) snprintf(val_buf, cap, "< %s >", zones::area_name(zones::zone(z).area));
                else if (i == kSysCountry) snprintf(val_buf, cap, "< %s >", zones::country(zones::zone(z).country).name);
                else snprintf(val_buf, cap, "< %s >", zones::zone(z).city);
            }
        }

    // "*" marks a row the arrows have changed but Enter has not applied.
    //
    // The room test used to be against sizeof(val_buf), which is the size of
    // the pointer - so the mark only ever appeared on values shorter than six
    // characters, and rows like Overscan reading "NONE" never wore it. cap is
    // what the caller passed for exactly this.
    if (menu_tab >= 0 && menu_tab < kMenuTabs && i >= 0 && i < kMenuRows &&
        menu_dirty[menu_tab][i] && val_buf[0]) {
        size_t n = strlen(val_buf);
        if (n + 3 <= cap) { val_buf[n] = ' '; val_buf[n + 1] = '*'; val_buf[n + 2] = '\0'; }
    }

}

// The sections each blade holds, as menu_tab numbers, in the order its tabs
// are listed. VIDEO is the air unit's camera and needs a link to reach.
std::vector<int> OSD::ui_side_tabs(int side) const {
    if (side == 0) return { 2, 3, kTabDvr, kTabSystem };
    std::vector<int> v;
    if (osd_vars.radio_link.state == 2) v.push_back(0);
    v.push_back(1);
    v.push_back(+kTabAirSerial);   // by value: a static const int has no definition to bind to
    v.push_back(+kTabAirInfo);
    return v;
}

// Where a folded blade is, in pixels from the top left: its side of the
// picture, from the canopy's top rail (canopy_top_y_, frustum units) down.
void OSD::ui_blade_rect(int side, int W, int H, float fh, float& x, float& y, float& w, float& h) {
    int px, py, pw, ph;
    dev->picture_rect(W, H, px, py, pw, ph);
    const float top = py + (fh - canopy_top_y_) / (2.0f * fh) * ph;
    w = 0.31f * pw;
    x = side == 0 ? (float)px : px + pw - w;
    y = top;
    h = py + ph - top;
}

// The rows under the cursor change by themselves: the link dropping takes
// VIDEO away (draw_canopy_ui) and RADIO down to Bind, and an air unit's
// announce adds or removes a RADIO row. A value being changed acts only on
// the row it started on - OK on whatever row is there now would do that
// row's action instead: a bind that keeps the air unit out for 120 s, a
// standby.
bool OSD::menu_edit_row_intact() const {
    const std::vector<MenuItem> rows = menu_items(menu_tab);
    return menu_tab == menu_edit_tab_ && menu_index >= 0 && menu_index < (int)rows.size() &&
           rows[menu_index].type == 1 && rows[menu_index].label &&
           menu_edit_label_ == rows[menu_index].label;
}
