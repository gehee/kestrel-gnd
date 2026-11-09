#pragma once
#include <cstdint>

// Flight values shared by the MSP decoder and both HUD implementations.
struct BfTelem {
    // The OSD elements the canopy can show a value of: the glyphs and figures
    // of each, as the scrape read them (MspOsd::draw_region's hide).
    enum Elem : uint8_t {
        kElCellV  = 1 << 0,   // average cell voltage, with its battery icon
        kElPackV  = 1 << 1,   // pack voltage, idem
        kElAmps   = 1 << 2,
        kElTimer  = 1 << 3,   // the timer whose figure was read (timer_s)
        kElArm    = 1 << 4,   // DISARMED
        kElMode   = 1 << 5,   // the flight mode
        kElSpeed  = 1 << 6,
        kElLq     = 1 << 7,   // Link Quality
    };
    // Elem bits of the values below read off the OSD canvas here, not
    // asked of the flight controller over MSP by the air unit (MSP_STATUS,
    // MSP_ANALOG, the polled groups): the elements that alone carry what the
    // canopy shows.
    unsigned osd_src = 0;
    bool  have_cell_v = false;  float cell_v    = 0.0f;  // V, per cell
    bool  have_pack_v = false;  float pack_v    = 0.0f;  // V, whole pack
    bool  have_amps   = false;  float amps      = 0.0f;  // A
    bool  have_alt    = false;  float alt_m     = 0.0f;  // m, relative to arm
    bool  have_speed  = false;  float speed_kmh = 0.0f;
    bool  have_rssi   = false;  int   rssi_pct  = 0;     // 0-100
    bool  have_dbm    = false;  int   rssi_dbm  = 0;     // CRSF link, negative
    bool  have_lq     = false;  int   lq_pct    = 0;     // 0-100
    bool  have_timer  = false;  int   timer_s   = 0;     // seconds since arming
    bool  have_mode   = false;  char  mode[8]   = {0};   // "ACRO", "ANGL", ...
    // FC attitude, degrees, from the "Pitch/Roll Angle" OSD elements - the only
    // way this air unit's link carries attitude (it never relays MSP_ATTITUDE
    // or RAW_IMU). Positive pitch = nose up, positive roll = right side down,
    // matching Betaflight's own sign convention for these elements.
    bool  have_pitch  = false;  float pitch_deg = 0.0f;
    bool  have_roll   = false;  float roll_deg  = 0.0f;
    // From the values the air unit polls (msp_telem.hpp); the OSD text has none of them.
    // Nothing draws these yet.
    bool  have_batt_pct = false; int batt_pct = 0;             // from mAh drawn and the pack's capacity
    bool  have_gps      = false; int gps_fix = 0, sats = 0;    // fix: 0 none, 1 2D, 2 3D
    bool  have_pos      = false; double lat = 0.0, lon = 0.0;
    bool  have_home     = false; int home_dist_m = 0, home_dir_deg = 0;   // to home
    // -1 unknown (nothing in the OSD says either way), 0 disarmed, 1 armed.
    int   arm = -1;
    uint64_t stamp_us = 0;      // when this screen was committed
};
