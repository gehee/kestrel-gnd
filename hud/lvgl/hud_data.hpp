#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace kestrel_ui {
// Display-ready input for a passive HUD. No stock packets, timestamps to
// reinterpret, user actions, framebuffer handles, LVGL objects or GL resources.
struct HudPanelData {
    bool visible = false, picked = false, fresh = false, armed = false;
    float shade = 0, rule = 0, block = 0, dim = 1, rail_dim = 1;
    float upper_level = 0, lower_level = 0, state_opacity = 0;
    std::string primary = "--", primary_unit, secondary, secondary_unit;
    std::string state, upper_label, lower_label, detail1, detail2;
    bool detail1_quiet = false;
};
enum class HudMetricId : uint16_t {
    Voltage = 1, Current, Rc, Mcs, FrequencyRaw, ReceiverVoltage, CpuTemperature,
    RfTemperatureRaw, FlightState, FlightMode, FlightTimer, Speed, Altitude,
    FcVariant, FcFirmware, MspFeed, RfState, QualityRaw, DistanceRaw, UiCadence
};
struct HudMetric {
    HudMetricId id = HudMetricId::Voltage;
    bool valid = false, metered = false;
    float level = 0;
    std::string label, value = "--", unit;
    std::vector<std::string> scale; // Display-ready labels, low to high.
};
struct HudData {
    bool enabled = true, details = true, show_status = true;
    int theme = 0;
    int text_scale = 10; // tenths, 5..20; geometry is unchanged
    std::string status;
    std::array<HudPanelData, 2> panels;
    bool simulated = false;
    std::vector<HudMetric> metrics;
    const HudMetric* metric(HudMetricId id) const {
        for (const auto& item : metrics) if (item.id == id) return &item;
        return nullptr;
    }
};
} // namespace kestrel_ui
