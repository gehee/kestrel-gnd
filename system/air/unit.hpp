#pragma once
// The air unit as a whole: what it is and runs, its state, its standby mode,
// its flight logs.
#include <cstdint>
#include <cstring>
#include <string>

#include "system/part.hpp"

namespace air {

// What the air unit says it is and runs (AIR UNIT > INFO): from its version
// message (cmd 0x04, stock and kestrel-air) and kestrel-air's info message
// (0x50, "key=value" lines). Emptied when the link goes, so it is always
// the unit on the other end.
struct Info {
    bool version = false;       // a version message came
    bool kestrel = false;       // ... saying kestrel-air
    int  stock[3] = {0, 0, 0};  // the stock firmware's APP_VERSION, both apps
    int  hw = -1;               // board-ID version byte (0x10..0x13)
    uint32_t chipid = 0;        // bytes 1..4 from the stock app (it sends 0); the tag from kestrel-air
    int  rf_hw = -1;            // the RF board's version, TLV 0x14 of cmd 0x03
    int  proto = 0;             // kestrel-air's protocol and feature bits
    uint8_t feat = 0;
    // What it has beyond those bits, as it shows it: kAirFeat864p144
    // (its info), kAirSaidFrameClock, kAirSaidSensorTiming.
    uint16_t said = 0;
    // kestrel-air's info (0x50): its "key=value" lines, as many as fit (keys
    // to 15 characters, values to 63).
    static constexpr int kMaxKv = 24;
    struct Kv { char key[16]; char value[64]; };
    int kv_n = 0;
    Kv kv[kMaxKv] = {};
    // What it has on, by name (Radio Clock, Frame Clock...): its feature bits
    // and what it said, as the radio names them.
    static constexpr int kMaxCaps = 12;
    int caps_n = 0;
    char caps[kMaxCaps][24] = {};
    std::string get(const char *k) const {
        for (int i = 0; i < kv_n; i++) if (strcmp(kv[i].key, k) == 0) return kv[i].value;
        return std::string();
    }
};

// The camera settings it applies (VIDEO tab rows), bit n for the tab's row n
// (OSD video_row: 1 Rotate .. 9 Focus Mode), by its info's cam_ctrl names
// (cam= from older kestrel-air); all of them when the info does not say
// (stock, the Lite). White Balance has two more bits for its values: wb, the
// kelvin presets; wb_lock, Lock current (-1 on the wire) - only where said.
enum CamSetting : uint16_t {
    kCamRotate = 1 << 1, kCamScene = 1 << 2, kCamEv = 1 << 3, kCamWb = 1 << 4,
    kCamSat = 1 << 5, kCamContrast = 1 << 6, kCamSharpness = 1 << 7, kCamDnr3d = 1 << 8,
    kCamFocus = 1 << 9, kCamWbKelvin = 1 << 10, kCamWbLock = 1 << 11, kCamAll = 0xffff,
};
// White Balance's values an air unit takes: the kelvin presets, Lock current.
inline bool cam_wb_kelvin(uint16_t cam) { return cam == kCamAll || (cam & kCamWbKelvin); }
inline bool cam_wb_lock(uint16_t cam) { return cam != kCamAll && (cam & kCamWbLock); }
inline uint16_t cam_settings_of(const std::string &list) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"rotate", kCamRotate}, {"scene", kCamScene}, {"ev", kCamEv},
        {"wb", kCamWb | kCamWbKelvin}, {"wb_lock", kCamWb | kCamWbLock},
        {"sat", kCamSat}, {"contrast", kCamContrast}, {"sharpness", kCamSharpness},
        {"3dnr", kCamDnr3d}, {"focus", kCamFocus},
    };
    uint16_t m = 0;
    for (size_t p = 0; p <= list.size();) {
        size_t c = list.find(',', p);
        if (c == std::string::npos) c = list.size();
        const std::string n = list.substr(p, c - p);
        for (const auto &e : names) if (n == e.name) m |= e.bit;
        p = c + 1;
    }
    return m;
}

struct UnitState {
    Info info;
    uint16_t features = 0;     // kestrel-air's feature bits and its info's (0: none or stock)
    uint16_t cam_settings = kCamAll;   // the camera rows it applies (CamSetting)
    bool stock = false;        // the stock air app
    bool max_bw = false;       // takes the goggle's bandwidth cap
    bool low_power = false;    // drawing standby power (cmd 0x05 bytes 29-30)
    bool vtx_temp_valid = false;
    float vtx_cpu_c = 0;       // its SoC, cmd 0x05 byte 1
    uint64_t handshake_sent_us = 0;   // the last air handshake sent (0: none)
    // Its standby mode (TLV 0x12) as it reports it, with how many times it has:
    // whether it goes to standby disarmed.
    uint32_t standby_n = 0;
    bool standby = false;
};

class Unit : public Part<UnitState> {
public:
    virtual Ticket set_standby(bool on) = 0;   // AIR UNIT > Standby
    virtual Ticket set_logs(bool on) = 0;      // its flight logs (kestrel-air): HUD > Debug Info
};

}  // namespace air
