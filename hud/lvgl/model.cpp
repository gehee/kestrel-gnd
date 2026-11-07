#include "model.hpp"
#include "msp_names.hpp"
#include "hud/hud_theme.hpp"
#include <cctype>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace kestrel_ui {
namespace {
MenuRow reading(std::string id, std::string name, std::string value, std::string help) {
    return {std::move(name), std::move(value), false, std::move(id), std::move(help), {}};
}
MenuRow choice(std::string id, std::string name, int current, std::vector<std::string> options, std::string help) {
    const std::string value = options.at(size_t(current));
    return {std::move(name), value, true, std::move(id), std::move(help), std::move(options)};
}
}
const char* key_name(Key key) {
    static const char* names[] = {"menu", "up", "down", "left", "right", "enter", "back", "quit"};
    return names[static_cast<unsigned>(key)];
}
bool parse_key(const std::string& text, Key& key) {
    if (text.size() > 32) return false;
    std::string normalized;
    for (unsigned char c : text) normalized += char(std::tolower(c));
    while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.back()))) normalized.pop_back();
    size_t first = normalized.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    normalized.erase(0, first);
    if (normalized == "ok" || normalized == "return") normalized = "enter";
    if (normalized == "esc" || normalized == "cancel") normalized = "back";
    for (unsigned i = 0; i < 8; ++i) if (normalized == key_name(static_cast<Key>(i))) { key = static_cast<Key>(i); return true; }
    return false;
}
std::string Model::status(uint64_t now) const {
    if (backend.media.video_monitored && backend.media.video_fresh(now)) return "RECEIVING VIDEO";
    const auto& telemetry = backend.vrx;
    if (!telemetry.fresh(now)) return telemetry.stamp_us ? "BACKEND STALE" : "WAITING FOR BACKEND";
    if (!telemetry.connected) return "NO RF LINK";
    if (telemetry.mcs < 0) return "LINK UP / NO VALID MCS";
    if (backend.media.video_monitored) return backend.media.video_fresh(now) ? "RECEIVING VIDEO" : "RF CONNECTED / WAITING FOR VIDEO";
    return "RF CONNECTED / VIDEO UNVERIFIED";
}
bool Model::show_status(uint64_t now) const {
    if(scene==SceneMode::GalleryPlayback)return false;
    // Decoded video is independent of RF telemetry (including recorded replay).
    // Never cover a fresh picture with the opaque idle scene.
    return backend.media.video_monitored ? !backend.media.video_fresh(now) : !backend.vrx.link_valid(now);
}
RcSignal Model::rc_signal(uint64_t now) const {
    const auto& flight = backend.flight;
    if (!flight.stamp_us || now < flight.stamp_us || now - flight.stamp_us >= 2000000) return {};
    // RSSI is signal strength, not packet delivery quality. Keep the source
    // distinct and prefer real LQ when a backend supplies it, including 0% LQ.
    if (flight.have_lq && flight.lq_pct >= 0 && flight.lq_pct <= 100)
        return {RcSource::LinkQuality, flight.lq_pct};
    if (flight.have_rssi && flight.rssi_pct >= 0 && flight.rssi_pct <= 100)
        return {RcSource::Rssi, flight.rssi_pct};
    return {};
}
SceneMode Model::scene_mode() const {
    if (scene == SceneMode::GalleryBrowse || scene == SceneMode::GalleryPlayback) return scene;
    return menu_open ? SceneMode::Menu : SceneMode::Live;
}
bool Model::hud_visible() const {
    const auto mode = scene_mode();
    return mode == SceneMode::Live || mode == SceneMode::Menu ||
        (mode == SceneMode::GalleryPlayback && playback_hud_enabled && bool(backend.playback_hud));
}
const char* Model::tab_name(int t) {
    static const char* names[] = {"VIDEO", "RADIO", "HUD", "DISPLAY", "DVR", "SYSTEM", "INFO", "SERIAL"};
    return names[t >= 0 && t < 8 ? t : 0];
}
std::vector<int> Model::side_tabs() const {
    if (side == 0) return {2, 3, 4, 5};
    return backend.vrx.connected ? std::vector<int>{0, 1, 7, 6} : std::vector<int>{1, 7, 6};
}
int Model::remembered_section() const {
    const auto tabs = side_tabs();
    return std::find(tabs.begin(), tabs.end(), remembered_tab_[side]) == tabs.end() ? tabs.front() : remembered_tab_[side];
}
int Model::first_row(uint64_t now) const {
    const auto items = rows(now);
    for (size_t i = 0; i < items.size(); ++i) if (!items[i].heading) return int(i);
    return 0;
}
std::vector<MenuRow> Model::rows(uint64_t now) const {
    // Labels/order/help mirror hud/osd_menu_rows.cpp. Do not give a control a
    // pretend value or accept an edit when this platform cannot apply it.
    auto unavailable = [&](std::string id, std::string name, std::string help) {
        const auto found = backend.controls.find(id);
        if (found != backend.controls.end()) {
            const auto& c = found->second;
            auto row = reading(id, name, c.known ? c.text : "--", help);
            for (const auto& option : c.choices) row.options.push_back(option.label);
            if (c.known && !c.choices.empty()) {
                int index = setting(id);
                row.value = index >= 0 && index < int(c.choices.size()) ? c.choices[index].label : std::to_string(c.value);
            }
            row.action = c.action && c.enabled;
            row.editable = c.enabled && !c.action && c.choices.size() > 1;
            row.disabled = !c.enabled;
            if (!c.reason.empty()) row.help += " " + c.reason;
            return row;
        }
        auto row = reading(std::move(id), std::move(name), "UNAVAILABLE", std::move(help));
        row.disabled = true;
        row.help += " Not yet supported on Ascent.";
        return row;
    };
    auto heading = [](const char* name) {
        auto row = reading(name, name, "", ""); row.heading = true; return row;
    };
    const auto& serial = backend.serial;
    if (tab == 0) {
        std::vector<MenuRow> result;
        for (const char* name : {"Video Mode", "Max Bitrate", "Rotate", "Scene", "EV", "White Balance",
                                 "Saturation", "Contrast", "Sharpness", "3D DNR", "Focus Mode"})
            result.push_back(unavailable(name, name, "Changes the air unit's camera."));
        return result;
    }
    if (tab == 1) {
        std::vector<MenuRow> result;
        if (!backend.vrx.link_valid(now))
            result.push_back(unavailable("bind", "Bind", "Pairs this VRX with an air unit."));
        else {
            result.push_back(unavailable("channel", "Channel", "Where the link sits, what is on every channel and what your country allows."));
            result.push_back(unavailable("power", "TX Power", "Radio output. More reaches further and runs hotter."));
        }
        if (backend.controls.count("max_mcs")) result.push_back(unavailable("max_mcs", "Max MCS", "The highest receive MCS allowed by Open8030."));
        result.push_back(unavailable("radio_firmware", "VRX Radio Firmware", "This VRX's own radio firmware. The air unit's is not changed."));
        if (backend.vrx.link_valid(now)) result.push_back(unavailable("standby", "Auto Standby", "The air unit's low-power standby."));
        return result;
    }
    if (tab == 2) {
        std::vector<MenuRow> result{
            choice("hud", "HUD Style", show_hud ? 1 : 0, {"OFF", hud_style == "vector" ? "VECTOR" : "CANOPY"}, "Which overlay is drawn over the video."),
            backend.bf_available?choice("bf_osd", "Betaflight OSD", bf_osd, {"OFF","ON"}, "Draw Betaflight's own OSD across the screen."):
                reading("bf_osd","Betaflight OSD","ASSETS MISSING","Install the generated Betaflight glyph atlas.")};
        if (show_hud && hud_style != "vector") result.push_back(choice("per_cell", "Canopy Voltage", per_cell ? 0 : 1, {"PER CELL", "PACK"},
            "The canopy's headline voltage: per cell reads the same on any pack size."));
        result.push_back(unavailable("distance", "Calib Distance", "Zero the range reading where you are standing."));
        result.push_back(choice("clock", "Clock", clock, {"OFF","IDLE ONLY 12H","IDLE ONLY 24H","ON 12H","ON 24H"}, "When the time is shown at the top of the screen, and how it is written."));
        result.push_back(choice("dynamic", "Dynamic HUD", dynamic, {"OFF","SMALL","MEDIUM","EXTREME"}, "Move the panels horizontally and vertically with the aircraft's camera IMU or flight-controller attitude, while keeping them level."));
        result.push_back(choice("link_edge_warning", "Link Edge Warning", link_edge_warning, {"OFF","ON"}, "Amber for a weak link, red for a very weak link or stalled video."));
        result.push_back(choice("debug", "Debug Info", debug, {"OFF","ON"}, "Show the radio's current frame and bandwidth under the right panel."));
        return result;
    }
    if (tab == 3) {
        std::vector<std::string> themes;
        std::vector<std::string> scales;
        for(int i=5;i<=20;++i) scales.push_back(std::to_string(i/10)+"."+std::to_string(i%10)+"x");
        for (int i = 0; i < hud_theme_count(); ++i) themes.push_back(hud_theme_at(i).name);
        return {choice("theme", "Theme", theme, std::move(themes), "Which hues carry which meaning."),
            reading("bg_video", "Idle Background", "STATIC", "The compiled artwork is cached and drawn only when it changes."),
            reading("picture_size", "Picture Size", "FULL", "The native display driver currently exposes fixed full-screen planes. Hardware scaling needs driver support."),
            choice("ui_scale", "UI Scaling", ui_scale-5, scales, "Size of HUD type and menu contents. The menu panel stays put."),
            reading("brightness", "Brightness", "MONITOR", "Use the external monitor's brightness control. HDMI does not expose a local panel backlight."),
            heading("ADVANCED"),
            reading("frame_mode", "Frame Mode", "FULL DECODE", "The decoder receives whole pictures; only complete decoded frames are displayed.")};
    }
    if (tab == 4) return {
        reading("dvr_source", "Source", "FPV STREAM", "Records the received video. Screen capture requires a hardware compositor-to-encoder path."),
        unavailable("record", "Recording", "Start or stop recording; the REC button does the same."),
        unavailable("gallery", "Gallery", "Browse and play recordings."),
        unavailable("folder", "Folder", "Where recordings are written."),
        unavailable("space", "Space Left", "Room remaining on the recordings partition.")};
    if (tab == 5) return {
        reading("build", "Build", build, "When this receiver application was built."),
        reading("version", "Version", version, "The receiver application version and source revision."),
        reading("decoder", "Decoder", decoder, "The Ascent hardware video decoder."),
        backend.controls.count("screen_mode") ? unavailable("screen_mode", "Screen Mode", "Select the HDMI output resolution and refresh rate. Confirm the new picture within 15 seconds or it will revert.") : reading("screen_mode", "Screen Mode", screen_mode, "The current HDMI output mode."),
        reading("wifi", "WiFi AP", "NOT SUPPORTED", "The native Ascent image has no WiFi AP or spectator video service."), heading("WHERE YOU FLY"),
        unavailable("licence", "Amateur Licence", "Whether you hold an amateur radio licence."),
        unavailable("area", "Area", "Where you fly, starting with the part of the world."),
        unavailable("country", "Country", "The country you fly in: its rules for the radio's channels."),
        unavailable("timezone", "Timezone", "Which local time the clock and recording filenames use.")};
    if (tab == 6) {
        auto info = [&](const char* key) { auto it=backend.air_info.find(key); return it==backend.air_info.end()?std::string("--"):it->second; };
        std::vector<MenuRow> rows{heading("AIR UNIT"),
            reading("model","Model",info("model"),"The connected air unit, as it reports itself."),
            reading("hw_version","HW Version",info("hw_version"),"Board and RF board versions."),
            reading("sn","SN",info("sn"),"The model and radio chip identifier reported by the air unit."),
            reading("sw_version","SW Version",info("sw_version"),"Stock firmware version reported by the air unit.")};
        for (const auto& field : std::vector<std::pair<const char*,const char*>>{{"app","Application"},{"ver","App Version"},{"os","OS"},{"radio","Radio"},{"kernel","Kernel"},{"sensor","Sensor"}})
            if (backend.air_info.count(field.first) && !info(field.first).empty()) rows.push_back(reading(field.first,field.second,info(field.first),"Reported by the connected air unit."));
        return rows;
    }
    return {heading("FLIGHT CONTROLLER"),
        reading("receiving", "Receiving", serial.fresh(now) ? "YES" : "NO", "Whether MSP frames from the flight controller are coming in now."),
        reading("frames", "Frames", std::to_string(serial.frames), "MSP frames received and understood."),
        reading("data", "Data", std::to_string(serial.bytes) + " B", "Total flight controller bytes received.")};
}
int Model::setting(const std::string& id) const {
    if (const auto c = backend.controls.find(id); c != backend.controls.end()) {
        const auto preview = receiver_preview_.find(id);
        const int value = preview == receiver_preview_.end() ? c->second.value : preview->second;
        for (size_t i=0;i<c->second.choices.size();++i) if (c->second.choices[i].value==value) return int(i);
        return -1;
    }
    if (id == "hud") return show_hud ? 1 : 0;
    if (id == "per_cell") return per_cell ? 0 : 1;
    if (id == "theme") return theme;
    if (id == "details") return show_details ? 1 : 0;
    if (id == "repeat") return key_repeat ? 1 : 0;
    if (id == "fps") return fps - 1;
    if (id == "clock") return clock;
    if (id == "bf_osd") return bf_osd;
    if (id == "debug") return debug;
    if (id == "link_edge_warning") return link_edge_warning;
    if (id == "dynamic") return dynamic;
    if (id == "ui_scale") return ui_scale-5;
    return 0;
}
void Model::set_setting(const std::string& id, int value) {
    if (const auto c = backend.controls.find(id); c != backend.controls.end()) {
        if (value >= 0 && value < int(c->second.choices.size())) receiver_preview_[id]=c->second.choices[value].value;
        else receiver_preview_.erase(id);
        return;
    }
    if (id == "hud") show_hud = value != 0;
    else if (id == "per_cell") per_cell = value == 0;
    else if (id == "theme") theme = value;
    else if (id == "details") show_details = value != 0;
    else if (id == "repeat") key_repeat = value != 0;
    else if (id == "fps") fps = std::clamp(value + 1, 1, 60);
    else if (id == "clock") clock = std::clamp(value,0,4);
    else if (id == "bf_osd") bf_osd=value!=0;
    else if (id == "debug") debug=value!=0;
    else if (id == "link_edge_warning") link_edge_warning=value!=0;
    else if (id == "dynamic") dynamic=std::clamp(value,0,3);
    else if (id == "ui_scale") ui_scale=std::clamp(value+5,5,20);
}
void Model::set_preferences(const UiPreferences& p) {
    if (p.theme < 0 || p.theme >= hud_theme_count() || p.fps < 1 || p.fps > 60 || p.clock<0 || p.clock>4 || p.dynamic<0 || p.dynamic>3 || p.ui_scale<5 || p.ui_scale>20)
        throw std::invalid_argument("Invalid UI preferences");
    theme = p.theme; fps = p.fps; show_details = p.details; key_repeat = p.repeat; show_hud = p.hud; per_cell = p.per_cell;
    clock=p.clock;
    bf_osd=p.bf_osd;debug=p.debug;dynamic=p.dynamic;
    ui_scale=p.ui_scale; link_edge_warning=p.link_edge_warning;
}
std::string Model::clock_text(std::time_t wall_time,uint64_t now) const {
    if(!hud_visible() || !clock || (clock<=2 && !show_status(now))) return {};
    std::tm tm{};
    if(!localtime_r(&wall_time,&tm)) return {};
    char text[32]; std::strftime(text,sizeof(text),clock%2?"%I:%M %p":"%H:%M",&tm);
    return text;
}
bool Model::edit_intact() const {
    const auto menu = rows(monotonic_us());
    return tab == edit_tab_ && selected >= 0 && selected < int(menu.size()) && menu[selected].editable && menu[selected].id == edit_id_;
}
int Model::option_index() const { return stage == MenuStage::Edit ? setting(edit_id_) : 0; }
std::string Model::selected_help(uint64_t now) const {

    const auto menu = rows(now);
    if (selected < 0 || selected >= int(menu.size())) return "";
    const auto& row = menu[selected];
    std::string help = row.help;
    const auto changed = pending_.find(row.id);
    if (changed != pending_.end() && changed->second >= 0 && changed->second < int(row.options.size()))
        help = "Pending: " + row.options[changed->second] + " -> " + row.value + ". OK applies it.\n" + help;
    return feedback.empty() ? help : feedback;
}
std::string Model::describe() const {
    std::ostringstream out;
    out << "OK open=" << menu_open << " stage=" << (!menu_open ? "closed" : stage == MenuStage::Pick ? "pick" : stage == MenuStage::Browse ? "browse" : "edit")
        << " side=" << (side ? "air" : "ground") << " tab=" << tab_name(tab) << " row=" << selected
        << " theme=" << hud_theme_at(theme).name << " fps=" << fps << " details=" << show_details << " repeat=" << key_repeat
        << " keys=" << key_count << " last=" << last_key << " hud_pid=" << hud_pid << " hud_restarts=" << hud_restarts
        << " backend_connected=" << backend.service.connected << " backend_session=" << backend.service.session
        << " backend_caps=" << backend.service.capabilities << " provider_connected=" << backend.service.provider_connected
        << " msp_frames=" << backend.serial.frames << " pack_valid=" << backend.flight.have_pack_v
        << " pack_v=" << (backend.flight.have_pack_v ? backend.flight.pack_v : -1.0f)
        << " scene=" << int(scene_mode()) << " recording=" << backend.media.recording
        << " playing=" << backend.media.playing << " paused=" << backend.playback_paused
        << " playback_us=" << backend.playback_position_us << " recorded_hud=" << bool(backend.playback_hud)
        << " playback_hud=" << playback_hud_enabled << " playback_dx=" << backend.playback_dx
        << " playback_bank=" << backend.playback_bank << " playback_warning=" << backend.playback_warning;
    return out.str();
}
} // namespace kestrel_ui
