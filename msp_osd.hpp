#ifndef MSP_OSD_HPP
#define MSP_OSD_HPP

#include <atomic>
#include <cstdint>
#include <vector>
#include <mutex>
#include "utils/math_utils.hpp"
#include <functional>
#include "msp_telem.hpp"
#include "cam_tilt.hpp"

// MSP DisplayPort Commands (0-based index in payload[0])
#define MSP_DP_HEARTBEAT 0
#define MSP_DP_RELEASE 1
#define MSP_DP_CLEAR_SCREEN 2
#define MSP_DP_WRITE_STRING 3
#define MSP_DP_DRAW_SCREEN 4
#define MSP_DP_SET_OPTIONS 5

// Values scraped back out of the DisplayPort character grid.
//
// We are receive-only on the telemetry socket - we tee off it and never
// transmit - so MSP_ALTITUDE, MSP_ANALOG and friends cannot be requested.
// Everything the HUD wants from the flight controller therefore has to be
// read out of the glyphs Betaflight already drew, which means a value only
// exists here if the pilot has that element enabled in their OSD.
// `have_*` false is the normal state, not an error: the HUD leaves the slot
// empty rather than printing a zero that looks like a measurement.
struct BfTelem {
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

struct MspOsdCmd {
    uint8_t cmd;
    std::vector<uint8_t> payload;
};

class MspOsd {
    private:
        std::mutex mtx;
        
        // Parsing State
        int state = 0;
        uint8_t flags;
        uint16_t function;
        uint16_t payload_size;
        std::vector<uint8_t> payload_buf;
        uint8_t crc;
        
        bool is_v2 = false;
        uint8_t checksum_v1 = 0;
        
        // Canvas State
        // 30x16 or 50x18 or 60x22 ?
        // Standard HD is usually 50x18 or 60x22.
        // We will store the characters and attributes in a grid.
        struct Cell {
            uint16_t char_idx; // 0-511
            uint8_t attr; // Blink, Invert, etc?
        };
        
        static const int GRID_W = 53;
        static const int GRID_H = 20;
        Cell grid[GRID_H][GRID_W];
        Cell pending_grid[GRID_H][GRID_W]; // Back buffer for Double Buffering
        
        // Double buffer?
        // MSP_DP_DRAW_SCREEN commits changes.
        
        // Bumped whenever something the HUD shows from the FC changes: the
        // character grid, arm state, pack voltage or current at display
        // resolution. The OSD loop redraws on a new version and not on every
        // MSP packet - the FC resends an unchanged screen continuously.
        std::atomic<uint32_t> content_ver_{0};
        bool hud_moving_ = false;   // spring not yet settled (update_physics)
        std::atomic<bool> motion_wanted_{false};   // reactive HUD on: attitude is news

        // MSP_RAW_IMU (102): kestrel-air polls it 20 times a second when it has no IMU on
        // its camera (fc-imu, the Lite+). Accelerometer: raw (typically 2048 = 1 g).
        int16_t raw_acc_x = 0;
        int16_t raw_acc_y = 0;
        int16_t raw_acc_z = 2048;
        // Its gyro, deg/s, in Betaflight's axes: roll (+ right side down), pitch (+ nose
        // down), yaw (+ nose left), and when it came. The reactive HUD's input when no
        // camera IMU is sending (fc_rate_locked).
        int16_t  fc_gyro_[3] = {0, 0, 0};
        uint64_t fc_gyro_us_ = 0;
        bool fc_rate_locked(double w[3]) const;

        // Pitch/roll in degrees, scraped from the FC's own "Pitch Angle" /
        // "Roll Angle" OSD text elements (glyph 0x15 / 0x14, a number reading
        // like "-01.4" immediately after). This is the primary attitude
        // source: DisplayPort crosses this link, MSP_RAW_IMU does not.
        float osd_pitch_deg = 0.0f;
        float osd_roll_deg = 0.0f;
        uint64_t osd_angles_us = 0;

        // Cell/pack voltage and flight mode, recovered the same
        // grid-independent way (scan_osd_values) - merged into get_telem()
        // with the same "structured/robust source wins" priority as
        // msp_analog_us_ below.
        bool raw_have_cell_v_ = false;  float raw_cell_v_ = 0.0f;
        bool raw_have_pack_v_ = false;  float raw_pack_v_ = 0.0f;
        bool raw_have_mode_   = false;  char  raw_mode_[8] = {0};
        uint64_t raw_values_us_ = 0;

        // Spring-damper HUD bounce state variables. bank is a rotation in the
        // view plane (radians), driven by roll - a banking aircraft tilts the
        // HUD rather than sliding it sideways, which reads as yaw.
        float hud_pos_x = 0.0f;
        float hud_pos_y = 0.0f;
        float hud_pos_bank = 0.0f;
        float hud_vel_x = 0.0f;
        float hud_vel_y = 0.0f;
        float hud_vel_bank = 0.0f;
        float reactivity_scale_ = 1.0f;   // set per call from get_hud_offset
        uint64_t last_physics_update_us = 0;

        void update_physics(float dt_sec);

        BfTelem telem;          // whatever the last screen's glyphs yielded

        // Values from the structured MSP replies the air unit already polls
        // for. They are kept apart from the scraped ones because they arrive
        // on their own schedule, not with a screen refresh, and get_telem()
        // merges the two - a scrape must not wipe them.
        int      msp_arm_          = -1;
        uint64_t msp_armed_since_  = 0;
        uint64_t msp_status_us_    = 0;
        bool     msp_have_rssi_    = false;  int   msp_rssi_pct_ = 0;
        bool     msp_have_pack_v_  = false;  float msp_pack_v_   = 0.0f;
        bool     msp_have_amps_    = false;  float msp_amps_     = 0.0f;
        uint64_t msp_analog_us_    = 0;

        // Arm state fell back to watching the flight timer advance when
        // nothing else said; kept for links that carry no MSP_STATUS.
        int      last_timer_s   = -1;
        uint64_t timer_moved_us = 0;
        // Polled values (msp_telem.hpp), each group stamped when it last came.
        MspTelem mt_;
        uint64_t mt_us_[6] = {0, 0, 0, 0, 0, 0};    // by MspTelem::Group, bit 0 first
        uint64_t msp_frame_us_ = 0;
        std::atomic<uint32_t> n_msp_{0}, n_msp_bytes_{0};
        void apply_polled_locked(unsigned groups);
        // The camera's tilt on the airframe, taken each time the aircraft is armed (cam_tilt.hpp).
        CamTilt tilt_;
        bool tilt_save_ = false;
        void scrape_locked();
        void handle_status(const uint8_t* p, size_t n);
        void handle_analog(const uint8_t* p, size_t n);

    private:
        void clear_internal();
        void write_string_locked(uint8_t row, uint8_t col, const uint8_t* str, size_t len);
        float m_scale = 1.0f;
        // Absolute cell size in frustum units. The panel layout and the
        // full-screen canvas need different sizes, so the owner sets it per
        // frame instead of both sides re-deriving it from m_scale.
        float m_cell_w = 0.03375f;
        float m_cell_h = 0.045f;

    public:
        // See content_ver_: changes whenever the HUD has something new to show.
        // Whether attitude changes are worth a redraw (the reactive HUD is
        // showing them). Set by the OSD every frame.
        void set_motion_wanted(bool on) { motion_wanted_ = on; }
        // Also what the camera's IMU says (the reactive HUD's input): any turning is news. Polled
        // here, from the OSD thread's own loop.
        uint32_t content_version();
        // The camera's saved tilt, degrees up (settings), and the tilt to save when arming has changed it.
        void set_cam_tilt(int deg) { std::lock_guard<std::mutex> l(mtx); tilt_.set(deg); }
        bool take_cam_tilt_to_save(int* deg);
        MspOsd();
        void parse_byte(uint8_t b);
        void parse_bytes(const uint8_t* p, size_t size);
        
        void handle_msp_frame(uint16_t function, const uint8_t* payload, size_t size);
        BfTelem get_telem();
        // What this end has received and understood, for AIR UNIT > SERIAL: MSP frames
        // and their bytes, and whether one came in the last 3 s.
        struct MspCounts { uint32_t frames = 0, bytes = 0; bool active = false; };
        MspCounts msp_counts();
        
        void draw(math::Mat4& projection, math::Mat4& view, std::function<void(float x, float y, const std::vector<uint16_t>& span)> draw_span_cb);
        void draw_region(int r_start, int c_start, int r_cnt, int c_cnt, float x, float y, std::function<void(float x, float y, const std::vector<uint16_t>& span)> draw_span_cb);
        void set_scale(float scale) { m_scale = scale; }
        void set_cell_size(float w, float h) { m_cell_w = w; m_cell_h = h; }
        void get_cell_size(float& w, float& h) { w = m_cell_w; h = m_cell_h; }
        static int grid_w() { return GRID_W; }
        static int grid_h() { return GRID_H; }
        
        // Retrieve dynamic bounce displacement offsets
        // dx/dy are a translation, bank_rad a view-plane rotation (from roll).
        // scale multiplies the whole effect (the HUD > Reactive HUD setting:
        // SMALL/MEDIUM/EXTREME); 1.0 is the tuned default.
        void get_hud_offset(float& dx, float& dy, float& bank_rad, float scale, bool& moving);
        // Drop the HUD back to centre and kill its momentum. For IDLE, where
        // the last attitude is meaningless - the aircraft is gone - and
        // leaving the panels parked at its offset reads as a stuck HUD.
        void reset_hud_motion();
        // Forget the whole screen and every reading taken off it. For a
        // CHANGE OF AIRCRAFT: the air unit only resends a row whose content
        // changed, so without this the previous drone's labels sit there
        // until each one happens to differ on the new one.
        void reset_screen();
        
        // Helper to update grid
        void write_string(uint8_t row, uint8_t col, const uint8_t* str, size_t len);
        bool handle_batched_displayport(const uint8_t* payload, size_t size);
        void scan_osd_angles(const uint8_t* payload, size_t size);
        void scan_osd_values(const uint8_t* payload, size_t size);
        void clear();
        void simulate_startup();
};

#endif
