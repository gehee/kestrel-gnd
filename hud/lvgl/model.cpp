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
    return "RF CONNECTED / VIDEO UNVERIFIED";
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
    static const char* names[] = {"RADIO", "HUD", "SYSTEM", "DISPLAY", "INPUT", "INFO", "SERIAL"};
    return names[t >= 0 && t < 7 ? t : 0];
}
std::vector<int> Model::side_tabs() const { return side == 0 ? std::vector<int>{1, 3, 2, 4} : std::vector<int>{0, 6, 5}; }
int Model::remembered_section() const {
    const auto tabs = side_tabs();
    return std::find(tabs.begin(), tabs.end(), remembered_tab_[side]) == tabs.end() ? tabs.front() : remembered_tab_[side];
}
std::vector<MenuRow> Model::rows(uint64_t now) const {
    const auto& telemetry = backend.vrx;
    const auto& serial = backend.serial;
    const auto& flight = backend.flight;
    const bool fresh = telemetry.fresh(now), valid = telemetry.link_valid(now);
    auto value = [&](int v, bool available) { return available ? std::to_string(v) : "--"; };
    if (tab == 0) return {
        reading("link", "State", status(now), "RF connection is not proof of decoded video. Stock backend owns reception."),
        reading("mcs", "MCS", value(telemetry.mcs, valid), "Raw receive modulation/coding value; unavailable if telemetry is stale or the RF link is down."),
        reading("frequency", "Frequency (raw)", value(telemetry.frequency_raw, valid), "Uncalibrated stock frequency-table value. No frequency command is sent by this menu."),
        reading("distance", "Distance (raw)", value(telemetry.distance_raw, valid), "Raw stock ranging value; units/calibration are not yet validated."),
        reading("quality", "Quality grade (raw)", value(telemetry.quality_raw, valid), "Raw vendor quality enum, not a percentage or signal strength."),
        reading("rf_controls", "RF / camera controls", "Backend-owned", "Configuration writes require validated backend commands and readback. These rows are read-only.")};
    if (tab == 1) {
        std::vector<std::string> themes;
        for (int i = 0; i < hud_theme_count(); ++i) themes.push_back(hud_theme_at(i).name);
        return {choice("theme", "Theme", theme, std::move(themes), "Preview the canopy colors. OK applies; BACK restores the original theme."),
            choice("details", "Detail rows", show_details ? 1 : 0, {"OFF", "ON"}, "Show the aircraft/receiver detail rows beneath the canopy gauges."),
            reading("renderer", "HUD renderer", hud_style, "Passive HUD executable selected for this session. Its telemetry is prepared by the UI controller."),
            reading("flight", "Aircraft telemetry", !flight.stamp_us ? "Unavailable" :
                (now >= flight.stamp_us && now - flight.stamp_us < 2000000 ? "Received" : "Stale"),
                "Validated MSP STATUS supplies arm state; ANALOG supplies pack voltage/current. Each expires independently. No cell count or flight mode is guessed.")};
    }
    if (tab == 3) {
        std::vector<std::string> options;
        for (int r = 1; r <= 60; ++r) options.push_back(std::to_string(r) + " Hz");
        return {choice("fps", "UI cadence", fps - 1, std::move(options), "Maximum UI animation/update cadence. This does not change camera FPS or HDMI video mode."),
            reading("canvas", "Overlay canvas", std::to_string(display_width) + "x" + std::to_string(display_height), "Framebuffer geometry read from the current stock display. No modeset is performed."),
            reading("format", "Overlay format", "ARGB8888", "Software alpha overlay, separate from the hardware video plane."),
            reading("video_owner", "Video output", "Stock backend", "The decoder and HDMI video remain owned by ar_ldy_gnd.")};
    }
    if (tab == 4) return {
        choice("repeat", "Arrow repeat", key_repeat ? 1 : 0, {"OFF", "ON"}, "Allow held arrow keys to repeat. OK, MENU and BACK are never repeated."),
        reading("sources", "Sources", input_sources, "All input sources feed the same menu controller. Stock GPIO 4-6 are board-ID straps, not buttons."),
        reading("key_count", "Keys received", std::to_string(key_count), "Accepted normalized menu keys, including the local control socket and SIGUSR1."),
        reading("last_key", "Last key", last_key, "Latest accepted key; BACK cancels an edit, folds a blade, then closes the menu."),
        reading("rejected", "Ignored packets", std::to_string(rejected_inputs), "Malformed/unmapped input packets are ignored; they do not issue radio or recording commands."),
        reading("control", "Control socket", control_path.empty() ? "Disabled" : control_path.substr(control_path.find_last_of('/') + 1), "Use the SSH interactive controller or --send-key to operate a detached UI.")};
    if (tab == 5) return {reading("backend", "Receiver backend", "ar_ldy_gnd", "Stock backend supplies RF/media and normalized GUI status."),
        reading("layout", "Status payload", value(int(telemetry.status_payload_bytes), fresh) + " bytes", "Known 17.5.3/18.21.10 layouts are decoded; unknown extensions are not guessed."),
        reading("capabilities", "Control writes", "Not enabled", "Only local UI settings are editable at this stage.")};
    if (tab == 6) return {
        reading("serial_feed", "MSP feed", serial.fresh(now) ? "Active" : serial.frames ? "Stale" : "Waiting", "Checksum-validated MSP v1/native v2 from the stock forwarding feed. Receiving packets does not imply arming or a valid video stream."),
        reading("fc_variant", "Flight controller", serial.variant.empty() ? "--" : serial.variant, "MSP_FC_VARIANT supplied by the flight controller, for example BTFL = Betaflight."),
        reading("fc_firmware", "FC firmware", serial.firmware.empty() ? "--" : serial.firmware, "MSP_FC_VERSION, including the captured length-prefixed Betaflight version string when present."),
        reading("msp_frames", "MSP frames", std::to_string(serial.frames), "Validated complete MSP frames, including requests/errors counted separately. One stock envelope can contain multiple frames or a partial frame."),
        reading("serial_bytes", "Serial bytes", std::to_string(serial.bytes), "Forwarded serial bytes excluding the stock envelope's one-byte prefix. The UI never opens the physical UART."),
        reading("msp_last", "Last MSP", std::string(msp_name(serial.last_function)) + " (" + std::to_string(serial.last_function) + ")", "Last checksum-validated MSP function. Other valid functions remain counted without guessing their payload semantics."),
        reading("msp_status", "Status messages", std::to_string(serial.status_frames), "MSP_STATUS/STATUS_EX flight mode flags, including the Betaflight BOXARM bit."),
        reading("msp_analog", "Analog messages", std::to_string(serial.analog_frames), "MSP_ANALOG pack voltage, current and RSSI. Pack voltage is not divided by an assumed cell count."),
        reading("rc_signal", "RC indicator", rc_signal(now).source == RcSource::Unavailable ? "Unavailable" :
            std::string(rc_signal(now).label()) + " " + std::to_string(rc_signal(now).percent) + "%",
            "The RC bar prefers actual LQ, otherwise fresh MSP_ANALOG RSSI. RSSI is not relabeled as LQ; stale or unavailable data leaves the bar unfilled."),
        reading("msp_bad", "Malformed MSP", std::to_string(serial.malformed), "Bad direction, length, checksum or decoded payload. Invalid packets never supply flight readings."),
        reading("msp_other", "Other MSP", std::to_string(serial.unhandled), "Valid MSP functions whose data is not decoded yet, including DisplayPort grid messages."),
        reading("vendor_serial", "Vendor notifications", std::to_string(serial.vendor_notifications), "Separate channel-0 prefix-1 stock sky notifications. Contents remain opaque and are not parsed as MSP."),
        reading("notifications", "Unmapped messages", std::to_string(telemetry.unknown_frames), "Other stock envelope types whose framing or routing is not mapped. Valid serial/MSP envelopes are counted above instead."),
        reading("serial_owner", "VRX UART", "Stock backend", "KestrelUI does not open or reconfigure the backend-owned ttyS4 serial port."),
        reading("fc", "FC polling", "Receive-only", "No MSP requests, UART-mode changes or backend control writes are sent.")};
    return {reading("voltage", "Receiver voltage", fresh && telemetry.voltage_mv > 0 ? std::to_string(telemetry.voltage_mv) + " mV" : "--", "Fitted stock supply-voltage measurement."),
        reading("cpu", "CPU temperature", fresh ? std::to_string(telemetry.cpu_temperature_x100 / 100.0f) + " C" : "--", "Receiver CPU temperature, not air-unit temperature."),
        reading("rf_temperature", "RF temperature (raw)", value(telemetry.rf_temperature_raw, fresh), "Raw stock RF-board temperature field."),
        reading("status_count", "Status frames", std::to_string(telemetry.status_frames), "Validated periodic receiver status packets."),
        reading("unknown_count", "Unknown frames", std::to_string(telemetry.unknown_frames), "Correctly framed notifications with unmapped semantics."),
        reading("bad_count", "Rejected frames", std::to_string(telemetry.bad_frames), "Invalid framing, lengths or checksums rejected by the parser.")};
}
int Model::setting(const std::string& id) const {
    if (id == "theme") return theme;
    if (id == "details") return show_details ? 1 : 0;
    if (id == "repeat") return key_repeat ? 1 : 0;
    if (id == "fps") return fps - 1;
    return 0;
}
void Model::set_setting(const std::string& id, int value) {
    if (id == "theme") theme = value;
    else if (id == "details") show_details = value != 0;
    else if (id == "repeat") key_repeat = value != 0;
    else if (id == "fps") fps = std::clamp(value + 1, 1, 60);
}
void Model::set_preferences(const UiPreferences& p) {
    if (p.theme < 0 || p.theme >= hud_theme_count() || p.fps < 1 || p.fps > 60)
        throw std::invalid_argument("Invalid UI preferences");
    theme = p.theme; fps = p.fps; show_details = p.details; key_repeat = p.repeat;
}
bool Model::edit_intact() const {
    const auto menu = rows(monotonic_us());
    return tab == edit_tab_ && selected >= 0 && selected < int(menu.size()) && menu[selected].editable && menu[selected].id == edit_id_;
}
int Model::option_index() const { return stage == MenuStage::Edit ? setting(edit_id_) : 0; }
std::string Model::selected_help(uint64_t now) const {
    if (stage == MenuStage::Edit) return "Preview: UP/DOWN changes. OK applies and saves. BACK cancels.";
    const auto menu = rows(now);
    if (selected < 0 || selected >= int(menu.size())) return "";
    return feedback.empty() ? menu[selected].help : feedback;
}
std::string Model::describe() const {
    std::ostringstream out;
    out << "OK open=" << menu_open << " stage=" << (!menu_open ? "closed" : stage == MenuStage::Pick ? "pick" : stage == MenuStage::Browse ? "browse" : "edit")
        << " side=" << (side ? "air" : "goggle") << " tab=" << tab_name(tab) << " row=" << selected
        << " theme=" << hud_theme_at(theme).name << " fps=" << fps << " details=" << show_details << " repeat=" << key_repeat
        << " keys=" << key_count << " last=" << last_key << " hud_pid=" << hud_pid << " hud_restarts=" << hud_restarts
        << " backend_connected=" << backend.service.connected << " backend_session=" << backend.service.session
        << " backend_caps=" << backend.service.capabilities << " provider_connected=" << backend.service.provider_connected
        << " msp_frames=" << backend.serial.frames << " pack_valid=" << backend.flight.have_pack_v
        << " pack_v=" << (backend.flight.have_pack_v ? backend.flight.pack_v : -1.0f);
    return out.str();
}
} // namespace kestrel_ui
