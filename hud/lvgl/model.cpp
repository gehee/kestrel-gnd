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
    const auto& telemetry = backend.vrx;
    if (!telemetry.fresh(now)) return telemetry.stamp_us ? "BACKEND STALE" : "WAITING FOR BACKEND";
    if (!telemetry.connected) return "NO RF LINK";
    if (telemetry.mcs < 0) return "LINK UP / NO VALID MCS";
    if (backend.media.video_monitored) return backend.media.video_fresh(now) ? "RECEIVING VIDEO" : "RF CONNECTED / WAITING FOR VIDEO";
    return "RF CONNECTED / VIDEO UNVERIFIED";
}
bool Model::show_status(uint64_t now) const {
    return !backend.vrx.link_valid(now) || (backend.media.video_monitored && !backend.media.video_fresh(now));
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
    return mode == SceneMode::Live || mode == SceneMode::Menu;
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
    auto unavailable = [](std::string id, std::string name, std::string help) {
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
        result.push_back(unavailable("radio_firmware", "VRX Radio Firmware", "This VRX's own radio firmware. The air unit's is not changed."));
        if (backend.vrx.link_valid(now)) result.push_back(unavailable("standby", "Auto Standby", "The air unit's low-power standby."));
        return result;
    }
    if (tab == 2) {
        std::vector<MenuRow> result{
            choice("hud", "HUD Style", show_hud ? 1 : 0, {"OFF", hud_style == "vector" ? "VECTOR" : "CANOPY"}, "Which overlay is drawn over the video."),
            unavailable("bf_osd", "Betaflight OSD", "Draw Betaflight's own OSD across the screen.")};
        if (show_hud && hud_style != "vector") result.push_back(choice("per_cell", "Canopy Voltage", per_cell ? 0 : 1, {"PER CELL", "PACK"},
            "The canopy's headline voltage: per cell reads the same on any pack size."));
        result.push_back(unavailable("distance", "Calib Distance", "Zero the range reading where you are standing."));
        result.push_back(unavailable("clock", "Clock", "When the time is shown at the top of the screen, and how it is written."));
        result.push_back(unavailable("dynamic", "Dynamic HUD", "Let the panels move with the aircraft."));
        result.push_back(unavailable("debug", "Debug Info", "A line of radio details under the right panel, for field tests: the radio's frame, and the mode it means."));
        return result;
    }
    if (tab == 3) {
        std::vector<std::string> themes;
        for (int i = 0; i < hud_theme_count(); ++i) themes.push_back(hud_theme_at(i).name);
        return {choice("theme", "Theme", theme, std::move(themes), "Which hues carry which meaning."),
            unavailable("bg_video", "Background Video", "Play the idle scene when nothing is linked."),
            unavailable("picture_size", "Picture Size", "Shrink the whole picture if your optics clip the edges."),
            unavailable("ui_scale", "UI Scaling", "Size of the HUD's type. The panels stay put."),
            unavailable("brightness", "Brightness", "Panel backlight. Lifts video and HUD together."),
            heading("ADVANCED"),
            reading("frame_mode", "Frame Mode", "FULL DECODE", "The decoder receives whole pictures; only complete decoded frames are displayed.")};
    }
    if (tab == 4) return {
        unavailable("dvr_source", "Source", "What REC captures: the FPV stream, or the screen."),
        reading("folder", "Folder", "--", "Where recordings are written. Recording is not yet supported on Ascent."),
        reading("space", "Space Left", "--", "Room remaining on the recordings partition.")};
    if (tab == 5) return {
        reading("build", "Build", build, "When this receiver application was built."),
        reading("version", "Version", version, "The receiver application version and source revision."),
        reading("decoder", "Decoder", decoder, "The Ascent hardware video decoder."),
        reading("screen_mode", "Screen Mode", screen_mode, "The current HDMI mode. Mode selection is not yet supported."),
        unavailable("wifi", "WiFi AP", "Wifi access point for spectators."), heading("WHERE YOU FLY"),
        unavailable("licence", "Amateur Licence", "Whether you hold an amateur radio licence."),
        unavailable("area", "Area", "Where you fly, starting with the part of the world."),
        unavailable("country", "Country", "The country you fly in: its rules for the radio's channels.")};
    if (tab == 6) return {heading("AIR UNIT"),
        reading("model", "Model", "--", "The air unit on the other end of the link, as it reports itself."),
        reading("hw_version", "HW Version", "--", "The board's version, then the RF board's."),
        reading("sn", "SN", "--", "The model, then the radio chip's ID."),
        reading("sw_version", "SW Version", "--", "The Caddx firmware on the air unit, as stock shows it.")};
    return {heading("FLIGHT CONTROLLER"),
        reading("receiving", "Receiving", serial.fresh(now) ? "YES" : "NO", "Whether MSP frames from the flight controller are coming in now."),
        reading("frames", "Frames", std::to_string(serial.frames), "MSP frames received and understood."),
        reading("data", "Data", std::to_string(serial.bytes) + " B", "Total flight controller bytes received.")};
}
int Model::setting(const std::string& id) const {
    if (id == "hud") return show_hud ? 1 : 0;
    if (id == "per_cell") return per_cell ? 0 : 1;
    if (id == "theme") return theme;
    if (id == "details") return show_details ? 1 : 0;
    if (id == "repeat") return key_repeat ? 1 : 0;
    if (id == "fps") return fps - 1;
    return 0;
}
void Model::set_setting(const std::string& id, int value) {
    if (id == "hud") show_hud = value != 0;
    else if (id == "per_cell") per_cell = value == 0;
    else if (id == "theme") theme = value;
    else if (id == "details") show_details = value != 0;
    else if (id == "repeat") key_repeat = value != 0;
    else if (id == "fps") fps = std::clamp(value + 1, 1, 60);
}
void Model::set_preferences(const UiPreferences& p) {
    if (p.theme < 0 || p.theme >= hud_theme_count() || p.fps < 1 || p.fps > 60)
        throw std::invalid_argument("Invalid UI preferences");
    theme = p.theme; fps = p.fps; show_details = p.details; key_repeat = p.repeat; show_hud = p.hud; per_cell = p.per_cell;
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
        << " pack_v=" << (backend.flight.have_pack_v ? backend.flight.pack_v : -1.0f);
    return out.str();
}
} // namespace kestrel_ui
