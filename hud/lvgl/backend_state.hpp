#pragma once
#include "system/flight_telemetry.hpp"
#include "system/thumbnail.hpp"
#include "hud_data.hpp"
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <limits>
#include <array>

namespace kestrel_ui {
enum Capability : uint32_t {
    TelemetryFeed = 1u << 0, CatalogRead = 1u << 1, RecordingControl = 1u << 2,
    PlaybackControl = 1u << 3, RadioControl = 1u << 4, CameraControl = 1u << 5, PairingControl = 1u << 6,
};
struct ServiceState {
    uint64_t session = 0, sequence = 0, stamp_us = 0;
    uint32_t capabilities = 0;
    bool connected = false, provider_connected = false, hardware_delegated = true;
    std::string adapter;
};
struct MediaState {
    bool video_monitored = false;
    uint64_t video_stamp_us = 0;
    bool video_fresh(uint64_t now) const { return video_stamp_us && now >= video_stamp_us && now - video_stamp_us < 2000000; }
    bool recording_known = false, recording = false;
    bool playback_known = false, playing = false;
    std::string recording_id, playback_id;
};
struct Telemetry {
    static constexpr int unavailable = std::numeric_limits<int>::min();
    uint64_t freshness_us = 2000000;
    int frequency_mhz = 0;
    uint64_t stamp_us = 0;
    bool connected = false;
    int mcs = -1, quality_raw = unavailable, frequency_raw = unavailable, distance_raw = unavailable;
    int voltage_mv = 0, cpu_temperature_x100 = unavailable, rf_temperature_raw = unavailable;
    unsigned status_payload_bytes = 0;
    uint64_t status_frames = 0, unknown_frames = 0, bad_frames = 0;
    bool fresh(uint64_t now) const { return stamp_us && now >= stamp_us && now - stamp_us < freshness_us; }
    bool link_valid(uint64_t now) const { return fresh(now) && connected && mcs >= 0; }
};
struct SerialTelemetry {
    uint64_t stamp_us = 0, envelopes = 0, bytes = 0, frames = 0;
    uint64_t malformed = 0, discarded_bytes = 0, requests = 0, errors = 0, unhandled = 0;
    uint64_t status_frames = 0, analog_frames = 0, vendor_notifications = 0;
    uint16_t last_function = 0;
    std::string variant, firmware;
    bool fresh(uint64_t now) const { return stamp_us && now >= stamp_us && now - stamp_us < 2000000; }
};
struct VtxState {
    // FC firmware is not VTX firmware. Empty values mean not yet reported.
    uint64_t stamp_us = 0;
    std::string identity, firmware;
};
// Native link readings retain explicit units; legacy packet fields stay separate.
struct LinkDetails {
    int distance_m = -1, bandwidth_step = -1, air_power_mw = 0;
    float video_mbps = 0, link_mbps = 0;
    bool standby = false, vtx_temperature_valid = false;
    float vtx_temperature_c = 0;
    int air_live_dbm = 0;   // the air unit's power now (air::RadioState::live_dbm); 0: not known
};
struct VideoDetails {
    uint64_t stamp_us = 0;
    int width = 0, height = 0, fps = 0;
    float latency_ms = -1; // capture to completed scanout; -1 if air clock unknown
};
struct ReceiverState {
    Telemetry vrx;
    VtxState vtx;
    SerialTelemetry serial;
    BfTelem flight;
    uint64_t flight_version = 0;
    MediaState media;
    LinkDetails link;
    VideoDetails video;
};
struct ReceiverChoice { int value; std::string label; };
struct ChannelReading {
    int mhz = 0, dbm = 0, peak_dbm = 0;
    bool permitted = false;
    std::string rule;
};
struct ChannelState {
    bool fresh = false, automatic = false;
    int current_mhz = 0;
    uint64_t revision = 0;
    std::vector<ChannelReading> readings;
};
struct ReceiverControl {
    int value = 0;
    bool known = false, enabled = false, action = false;
    std::string reason, text;
    std::vector<ReceiverChoice> choices;
};
struct BackendState : ReceiverState {
    struct Recording {std::string name;uint64_t bytes=0;std::shared_ptr<const media::Thumbnail> thumbnail;};
    std::vector<Recording> recordings;
    bool playback_busy=false,playback_paused=false,playback_seeking=false;
    uint64_t playback_position_us=0,playback_duration_us=0;
    std::string playback_error,playback_telemetry_error;
    std::shared_ptr<const HudData> playback_hud;
    std::array<uint16_t,53*20> playback_grid{};
    bool playback_osd_fresh=false;
    float playback_dx=0,playback_dy=0,playback_bank=0;
    int playback_warning=0;
    std::string playback_clock;
    std::array<uint16_t,53*20> bf_grid{};
    bool bf_available=false;
    float hud_dx=0,hud_dy=0,hud_bank=0;
    std::string radio_debug;
    ChannelState channels;
    std::map<std::string, ReceiverControl> controls;
    std::map<std::string, std::string> air_info;
    // Transport/session metadata belongs to the service, not the platform.
    ServiceState service;
};
} // namespace kestrel_ui
