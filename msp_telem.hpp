#ifndef MSP_TELEM_HPP
#define MSP_TELEM_HPP

#include <cstddef>
#include <cstdint>

// What the air unit polls Betaflight for over MSP (kestrel-air's fcpoll.c) and the
// ground reads out of the responses: battery, altitude, GPS, home and the
// flight modes. Plain values, no drawing, so it builds and tests on a PC; MspOsd
// merges them into the HUD's telemetry. Every value has a `have_` flag: an empty
// slot is the honest state, not a zero.
struct MspTelemState {
    // MSP_BATTERY_STATE
    bool have_batt = false;  int cells = 0;  float pack_v = 0, amps = 0;  int mah_drawn = 0, capacity_mah = 0;
    // MSP_ALTITUDE
    bool have_alt = false;   float alt_m = 0, vario_ms = 0;
    // MSP_RAW_GPS
    bool have_gps = false;   int fix = 0, sats = 0;  double lat = 0, lon = 0;  float gps_speed_kmh = 0;
    // MSP_COMP_GPS
    bool have_home = false;  int home_dist_m = 0, home_dir_deg = 0;
    // MSP_STATUS flags + MSP_BOXIDS
    bool have_modes = false; char mode[8] = {0};
    bool have_arm = false;   bool armed = false;
};

class MspTelem {
public:
    // Which groups a response touched, for the owner's freshness stamps.
    enum Group { G_BATT = 2, G_ALT = 4, G_GPS = 8, G_HOME = 16, G_MODES = 32 };

    // One MSP response (function number, payload). Returns the groups it updated, 0
    // if it is not one of these (or too short).
    unsigned handle(uint16_t function, const uint8_t* p, size_t n);

    const MspTelemState& state() const { return st_; }
    void reset();

private:
    void update_mode();
    MspTelemState st_;
    uint32_t flags_ = 0;
    bool have_flags_ = false;
    uint8_t box_ids_[64] = {0};
    int n_boxes_ = 0;
};

#endif
