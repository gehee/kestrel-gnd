#pragma once
#include "system/flight_telemetry.hpp"
#include <string>

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
    bool recording_known = false, recording = false;
    bool playback_known = false, playing = false;
    std::string recording_id, playback_id;
};
struct Telemetry {
    uint64_t stamp_us = 0;
    bool connected = false;
    int mcs = -1, quality_raw = 0, frequency_raw = 0, distance_raw = 0;
    int voltage_mv = 0, cpu_temperature_x100 = 0, rf_temperature_raw = 0;
    unsigned status_payload_bytes = 0;
    uint64_t status_frames = 0, unknown_frames = 0, bad_frames = 0;
    bool fresh(uint64_t now) const { return stamp_us && now >= stamp_us && now - stamp_us < 2000000; }
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
struct ReceiverState {
    Telemetry vrx;
    VtxState vtx;
    SerialTelemetry serial;
    BfTelem flight;
    uint64_t flight_version = 0;
    MediaState media;
};
struct BackendState : ReceiverState {
    // Transport/session metadata belongs to the service, not the platform.
    ServiceState service;
};
} // namespace kestrel_ui
