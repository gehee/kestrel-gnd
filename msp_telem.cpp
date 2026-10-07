#include "msp_telem.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
int16_t  i16(const uint8_t* p) { return (int16_t)u16(p); }
int32_t  i32(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }

// Betaflight's permanent box ids (box.c) for the modes shown. The flight mode flags of
// MSP_STATUS are bits by position in the list MSP_BOXIDS answers with, so the id at
// that position says what the bit is.
const int kBoxAngle = 1, kBoxHorizon = 2, kBoxFailsafe = 27, kBoxGpsRescue = 46;

}  // namespace

void MspTelem::reset() {
    st_ = MspTelemState();
    flags_ = 0;
    have_flags_ = false;
    n_boxes_ = 0;
}

// The mode the flags and the box list say: rescue and failsafe first, then angle and
// horizon, and acro when none of them is on. Not known until both have come.
void MspTelem::update_mode() {
    if (!have_flags_ || !n_boxes_) return;
    bool angle = false, horizon = false, failsafe = false, rescue = false;
    for (int i = 0; i < n_boxes_ && i < 32; i++) {
        if (!(flags_ & (1u << i))) continue;
        const int id = box_ids_[i];
        if (id == kBoxAngle) angle = true;
        else if (id == kBoxHorizon) horizon = true;
        else if (id == kBoxFailsafe) failsafe = true;
        else if (id == kBoxGpsRescue) rescue = true;
    }
    const char* m = rescue ? "RESC" : failsafe ? "FAIL" : angle ? "ANGL" : horizon ? "HOR" : "ACRO";
    st_.have_modes = true;
    std::snprintf(st_.mode, sizeof(st_.mode), "%s", m);
}

unsigned MspTelem::handle(uint16_t fn, const uint8_t* p, size_t n) {
    switch (fn) {
    case 130:       // MSP_BATTERY_STATE: cells u8, capacity u16, legacy volts u8 (0.1), mAh u16, amps i16 (0.01), state u8, volts u16 (0.01)
        if (n < 11) return 0;
        st_.cells = p[0];
        st_.capacity_mah = u16(p + 1);
        st_.mah_drawn = u16(p + 4);
        st_.amps = i16(p + 6) / 100.0f;
        st_.pack_v = u16(p + 9) ? u16(p + 9) / 100.0f : p[3] / 10.0f;
        st_.have_batt = true;
        return G_BATT;

    case 109:       // MSP_ALTITUDE: estimated altitude i32 cm, vario i16 cm/s
        if (n < 6) return 0;
        st_.have_alt = true;
        st_.alt_m = i32(p) / 100.0f;
        st_.vario_ms = i16(p + 4) / 100.0f;
        return G_ALT;

    case 106:       // MSP_RAW_GPS: fix u8, sats u8, lat, lon i32 (1e-7), alt u16 m, speed u16 cm/s, course u16
        if (n < 14) return 0;
        st_.have_gps = true;
        st_.fix = p[0];
        st_.sats = p[1];
        st_.lat = i32(p + 2) / 1e7;
        st_.lon = i32(p + 6) / 1e7;
        st_.gps_speed_kmh = u16(p + 12) * 0.036f;
        return G_GPS;

    case 107:       // MSP_COMP_GPS: distance to home u16 m, direction to home i16 deg
        if (n < 4) return 0;
        st_.have_home = true;
        st_.home_dist_m = u16(p);
        st_.home_dir_deg = ((i16(p + 2) % 360) + 360) % 360;
        return G_HOME;

    case 119: {     // MSP_BOXIDS: one byte per active box, in the flags' bit order
        const int c = n > sizeof(box_ids_) ? (int)sizeof(box_ids_) : (int)n;
        std::memcpy(box_ids_, p, (size_t)c);
        n_boxes_ = c;
        update_mode();
        return G_MODES;
    }

    case 101:       // MSP_STATUS / _EX: ... flight mode flags u32 at 6
    case 150:
        if (n < 10) return 0;
        flags_ = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16 | (uint32_t)p[9] << 24;
        have_flags_ = true;
        st_.have_arm = true;
        st_.armed = flags_ & 1u;       // BOXARM is the first box
        update_mode();
        return G_MODES;

    default:
        return 0;
    }
}
