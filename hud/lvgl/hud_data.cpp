#include "controller.hpp"
#include <cstdio>

namespace kestrel_ui {
namespace {
std::string number(float value, const char* format) {
    char text[80]; std::snprintf(text, sizeof(text), format, value); return text;
}
}
HudData Controller::hud_data(uint64_t now) const {
    const auto& m = model_;
    const auto& t = m.backend.vrx;
    const auto& f = m.backend.flight;
    const bool fresh = f.stamp_us && now >= f.stamp_us && now - f.stamp_us < 2000000;
    HudData data;
    data.enabled = m.hud_visible(); data.details = m.show_details; data.theme = m.theme; data.simulated = m.simulated;
    if (!data.enabled) { data.show_status = false; return data; }
    data.show_status = !t.link_valid(now); data.status = m.status(now);
    auto visual = m.visual;
    if (!visual.visible && m.menu_open) {
        visual.visible = true; visual.rule = visual.shade = visual.block = 1;
        visual.idle_dim = visual.rail_dim = 0.65f;
    }
    for (int side = 0; side < 2; ++side) {
        auto& p = data.panels[side];
        p.visible = visual.visible; p.fresh = fresh;
        p.picked = m.menu_open && m.stage == MenuStage::Pick && m.side == side;
        p.shade = visual.shade; p.rule = visual.rule; p.block = visual.block;
        p.dim = visual.idle_dim; p.rail_dim = visual.rail_dim;
    }
    auto& left = data.panels[0]; auto& right = data.panels[1];
    left.armed = fresh && f.arm == 1;
    left.upper_label = fresh && f.have_pack_v && !f.have_cell_v ? "PACK" : "CELL";
    left.lower_label = "SAG";
    if (fresh && (f.have_cell_v || f.have_pack_v)) {
        left.primary = number(f.have_cell_v ? f.cell_v : f.pack_v, "%.2f"); left.primary_unit = "V";
    }
    if (fresh && f.have_amps) { left.secondary = number(f.amps, "%.1f"); left.secondary_unit = "A"; }
    if (fresh && f.arm == 0) left.state = "DISARMED";
    else if (left.armed) {
        if (f.have_timer) { char text[24]; std::snprintf(text, sizeof(text), "%d'%02d\"", f.timer_s / 60, f.timer_s % 60); left.state = text; }
        else left.state = "ARMED";
    }
    left.state_opacity = 1;
    if (fresh && f.have_cell_v) {
        left.upper_level = std::clamp((f.cell_v - 3.3f) / 0.9f, 0.0f, 1.0f);
        left.lower_level = std::clamp((cell_peak_ - f.cell_v) / 0.5f, 0.0f, 1.0f);
    }
    left.detail1 = fresh && f.have_mode ? std::string("\xee\x80\x80   ") +
        std::string(f.mode, std::find(f.mode, f.mode + sizeof(f.mode), '\0')) : "\xee\x80\x80   FC TELEMETRY --";
    left.detail1_quiet = !(fresh && f.have_mode);
    left.detail2 = "\xee\x80\x81   " + (t.fresh(now) ? number(t.cpu_temperature_x100 / 100.0f, "VRX %.0fC") : "VRX --C");
    if (t.fresh(now) && t.voltage_mv > 0) left.detail2 += "  \xe2\x80\xa2 " + number(t.voltage_mv / 1000.0f, "%.1fV");
    const auto rc = m.rc_signal(now);
    right.upper_label = rc.label(); right.upper_level = rc.level(); right.lower_label = "VIDEO";
    if (fresh && f.have_speed) { right.primary = number(f.speed_kmh, "%.0f"); right.primary_unit = "km/h"; }
    if (visual.mark > 0) right.state = "IDLE";
    right.state_opacity = visual.mark;
    constexpr int ladder[] = {1, 2, 5, 7, 8, 10, 12};
    int rung = 0;
    if (t.link_valid(now)) for (int value : ladder) if (t.mcs >= value) ++rung;
    right.lower_level = float(rung) / 7;
    right.detail1 = (t.link_valid(now) ? "FREQ RAW " + std::to_string(t.frequency_raw) : "FREQ RAW --") + std::string("   \xee\x80\x80");
    right.detail2 = (t.link_valid(now) ? "VRX  MCS " + std::to_string(t.mcs) : "VRX  MCS --") + std::string("   \xee\x80\x81");
    // Renderer-neutral, display-ready instruments. The passive View never
    // infers units, cell count, capacity, signal source or telemetry freshness.
    auto add = [&](HudMetricId id, const char* label, bool valid, std::string value, const char* unit = "") -> HudMetric& {
        data.metrics.push_back({id, valid, false, 0, label, valid ? std::move(value) : "--", unit, {}});
        return data.metrics.back();
    };
    const bool pack = fresh && f.have_pack_v;
    const bool cell = fresh && f.have_cell_v;
    const float voltage = pack ? f.pack_v : f.cell_v;
    auto& volts = add(HudMetricId::Voltage, pack ? "PACK" : cell ? "CELL" : "PACK", pack || cell, number(voltage, "%.2f"), "V");
    volts.metered = true; volts.level = (pack || cell) ? std::clamp(voltage / (pack ? 30.0f : 5.0f), 0.0f, 1.0f) : 0;
    volts.scale = pack || !cell ? std::vector<std::string>{"0", "10", "20", "30"} : std::vector<std::string>{"0", "1", "2", "3", "4", "5"};
    add(HudMetricId::Current, "CURRENT", fresh && f.have_amps, number(f.amps, "%.2f"), "A");
    auto& signal = add(HudMetricId::Rc, rc.label(), rc.source != RcSource::Unavailable, std::to_string(rc.percent), "%");
    signal.metered = true; signal.level = rc.level(); signal.scale = {"0", "25", "50", "75", "100"};
    auto& video = add(HudMetricId::Mcs, "VIDEO MCS", t.link_valid(now), std::to_string(t.mcs));
    video.metered = true; video.level = right.lower_level;
    add(HudMetricId::FrequencyRaw, "FREQ RAW", t.link_valid(now), std::to_string(t.frequency_raw));
    add(HudMetricId::ReceiverVoltage, "RX SUPPLY", t.fresh(now) && t.voltage_mv > 0, number(t.voltage_mv / 1000.0f, "%.2f"), "V");
    add(HudMetricId::CpuTemperature, "VRX CPU", t.fresh(now), number(t.cpu_temperature_x100 / 100.0f, "%.0f"), "C");
    add(HudMetricId::RfTemperatureRaw, "RF TEMP RAW", t.fresh(now), std::to_string(t.rf_temperature_raw));
    add(HudMetricId::FlightState, "FC STATE", fresh && f.arm >= 0, f.arm == 1 ? "ARMED" : "DISARMED");
    add(HudMetricId::FlightMode, "MODE", fresh && f.have_mode, std::string(f.mode, std::find(f.mode, f.mode + sizeof(f.mode), '\0')));
    add(HudMetricId::FlightTimer, "FLIGHT TIMER", fresh && f.have_timer, std::to_string(f.timer_s), "s");
    add(HudMetricId::Speed, "SPEED", fresh && f.have_speed, number(f.speed_kmh, "%.0f"), "km/h");
    add(HudMetricId::Altitude, "ALTITUDE", fresh && f.have_alt, number(f.alt_m, "%.1f"), "m");
    add(HudMetricId::FcVariant, "FC", m.backend.serial.fresh(now) && !m.backend.serial.variant.empty(), m.backend.serial.variant);
    add(HudMetricId::FcFirmware, "FC FW", m.backend.serial.fresh(now) && !m.backend.serial.firmware.empty(), m.backend.serial.firmware);
    add(HudMetricId::MspFeed, "MSP FEED", m.backend.serial.fresh(now), "ACTIVE");
    add(HudMetricId::RfState, "RECEIVER LINK", t.fresh(now), t.link_valid(now) ? "RF CONNECTED" : m.status(now));
    add(HudMetricId::QualityRaw, "QUALITY RAW", t.link_valid(now), std::to_string(t.quality_raw));
    add(HudMetricId::DistanceRaw, "DISTANCE RAW", t.link_valid(now), std::to_string(t.distance_raw));
    add(HudMetricId::UiCadence, "UI LIMIT", true, std::to_string(m.fps), "Hz");
    return data;
}
} // namespace kestrel_ui
