#pragma once
// The radio's own terms: the fields of the air unit's settings it carries,
// its RF controls, what it holds, and the commands it carries out, in order,
// on its own thread. The rest of kestrel-gnd sees none of it: it calls the
// parts the radio carries (system/air/, system/gnd/), which radio::Model
// implements and turns into these. A radio implements radio::Link
// (radio/link.hpp) - radio/ar8030_link is the one there is.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "system/air/camera.hpp"
#include "system/air/power.hpp"

namespace radio {

// An air-unit setting the radio sends (request_setting(field, value)): the
// camera's (air::CameraField, the same numbers) and the others riding the
// same queue.
enum CamField { CAM_EV = 1, CAM_SAT, CAM_CONTRAST, CAM_SHARPNESS,
                CAM_SCENE,
                // This ordinal (cmd_cb id 0x306) was Anti-Flicker
                // (sky cmd 0x1F) - dropped because stock's own Camera
                // menu has no such row at all (confirmed against a
                // photo of it: Scene, EV, Saturation, Sharpness,
                // Contrast, WB, Rotate, Ratio, 3D DNR only). Reused
                // here for "Focus Mode", sky cmd 0x0E
                // (CMD_SET_CHN_FOCUS) - present in stock's Display
                // tab (also confirmed by photo) and already had its
                // own wire command and SkyConfig field, just never
                // wired to the menu.
                CAM_FOCUS, CAM_AWB,
                // Air-unit standby / low-RF-power mode. Not a camera
                // setting, but it travels the same sky socket, so it
                // rides the same queue rather than growing a second one.
                CAM_STANDBY,
                // Air-unit RF bandwidth, sky cmd 0x24.
                CAM_BW,
                // Air-unit TX power in mW, sky cmd 0x22.
                CAM_PWR,
                // Camera rotation - stock's "Rotate" row, sky cmd 0x06.
                CAM_ANGLE,
                // Image aspect ratio - WAS stock's "Ratio" row, sky cmd
                // 0x07. Menu row removed (verification pass found this
                // air unit acks the SET but, unlike every other camera
                // field, never reflects a change in its own status
                // echo - looks unsupported on this hardware). The
                // ordinal stays reserved and unused rather than being
                // deleted: main.cpp computes CamField from a literal
                // cmd_cb id as (cmd - 0x300), so CAM_3DNR's 0x30D only
                // lines up with CAM_3DNR's ordinal as long as nothing
                // shifts into this slot.
                CAM_RATIO_UNUSED,
                // 3D DNR, sky cmd 0x1B. body[35] of the config carries it
                // too, but that is the air's echo in its status report -
                // writing it there is acked and ignored.
                CAM_3DNR,
                // Ours: the cap on the air unit's video bitrate, kbps
                // (0 = none), sky cmd 0x40 - kestrel-air only. 0x30E.
                CAM_MAX_KBPS,
                // Ours: the cap on the video link's bandwidth, MHz (20
                // or 40), sky cmd 0x41 - kestrel-air only. 0x30F.
                CAM_MAX_BW,
                // Ours: the air unit's flight logs on or off (HUD >
                // Debug Info), sky cmd 0x42 - kestrel-air only. Not a
                // menu command id (0x310 up are the RF controls).
                CAM_AIR_LOGS,
                // Ours, not menu ids either: AUTO in a known country
                // (region_auto_apply). The air unit's work channel
                // list, sky cmd 0x29 (region_q_lists_, the value its
                // length), then its own hopping on or off, sky cmd
                // 0x21 (the value the link's channel, kHz).
                CAM_WORK_LIST, CAM_HOP_ON, CAM_HOP_OFF,
                // Ours: the cap on the air unit's auto exposure, us (0 =
                // none), sky cmd 0x43 - a kestrel-air that says maxexp=1.
                CAM_MAX_EXP_US,
                // Ours: the strips a picture is encoded in (1: whole
                // pictures), sky cmd 0x44 - a kestrel-air that says strips=N/M.
                CAM_STRIPS };

// RF controls on our own radio: rf_control(field, value).
enum RfField { RF_TX_POWER = 1, RF_MCS, RF_LNA, RF_BW, RF_CHAN, RF_HOP,
               RF_TX_POWER_FIT = 100 };   // ours: the chosen power fitted to the air unit now

// The settings it holds and its pairing window (radio::Link::held), which the
// model hands on to the parts they belong to, with the last command carried
// out.
struct State {
    int radio_fw = 0;              // our radio's firmware: 0 stock, 1 Open8030
    int tx_power_mw = 0;           // TX Power as held (stock's encoding)
    int tx_power_want_mw = 0;      // TX Power as chosen
    int max_mcs = -1;
    int frame_mode = 0;
    int max_kbps = 0;
    int max_exp_us = 0;
    int strips = 1;                // strips asked of the air unit (1: whole pictures)
    int max_bw_mhz = 0;
    int steady = 0;
    bool chan_auto = false;        // channel adaptation on
    int chan_hop = 0;              // AUTO HOP (1) or AUTO (0)
    int bind_state = 0;            // gnd::BindState
    int bind_secs_left = 0;
    unsigned bind_peer = 0;        // peer MAC, packed big-endian
    // The last command the radio has carried out (CommandMsg::seq): the menu
    // shows what it asked for until then.
    uint32_t commands_taken = 0;
    bool resend_known = false;     // the air radio's resends are known (air::Radio::take_resend)
};
inline bool operator==(const State& a, const State& b) {
    return a.radio_fw == b.radio_fw && a.tx_power_mw == b.tx_power_mw &&
           a.tx_power_want_mw == b.tx_power_want_mw && a.max_mcs == b.max_mcs &&
           a.frame_mode == b.frame_mode && a.max_kbps == b.max_kbps && a.max_exp_us == b.max_exp_us && a.strips == b.strips && a.max_bw_mhz == b.max_bw_mhz &&
           a.steady == b.steady && a.chan_auto == b.chan_auto && a.chan_hop == b.chan_hop &&
           a.bind_state == b.bind_state && a.bind_secs_left == b.bind_secs_left && a.bind_peer == b.bind_peer &&
           a.commands_taken == b.commands_taken && a.resend_known == b.resend_known;
}
inline bool operator!=(const State& a, const State& b) { return !(a == b); }

// ---- commands -----------------------------------------------------------------
// What the parts are asked (radio::Model makes one of each change, numbered
// in order, and saves what is a setting). The radio carries them out on its
// own thread (radio::Link::apply), in order, so its settings have one writer.
namespace cmd {
struct AirLogs { bool on; };
struct CameraSetting { int field; int value; };           // CamField
struct RfControl { int field; int value; int fixed_mhz; };   // RfField; fixed_mhz: RF_CHAN's, else -1
struct MaxBitrate { int kbps; };
struct MaxExposure { int us; };        // 0: no cap
struct Strips { int n; };              // 1: whole pictures, 2-4 strips
struct MaxBandwidth { int mhz; };      // 0: no cap
struct Firmware { int firmware; };
struct FrameMode { int mode; };
struct MaxMcs { int mcs; };
struct Steady { int on; };
struct ChannelHop { int hop; };
struct VideoMode { int index; };
struct Bind {};
struct RegionChanged {};
struct ScanScreen { bool open; };     // the channel-scan screen: scan fast while open
}  // namespace cmd
using Command = std::variant<cmd::AirLogs, cmd::CameraSetting, cmd::RfControl,
                             cmd::MaxBitrate, cmd::MaxBandwidth, cmd::Firmware, cmd::FrameMode,
                             cmd::MaxMcs, cmd::Steady, cmd::ChannelHop, cmd::VideoMode, cmd::Bind,
                             cmd::RegionChanged, cmd::ScanScreen, cmd::MaxExposure, cmd::Strips>;

// A command as the radio's queue holds it: numbered in the order it was
// asked (from 1; 0 is no command).
struct CommandMsg {
    uint32_t seq = 0;
    Command c;
};

// Each value in the range the radio and the settings file take, whatever
// was asked: a channel picked is FIXED from then on (fixed_mhz),
// Max Bandwidth 0 (no cap), 5, 10, 20 or 40 (a stock radio takes 20 or 40),
// the frame 0 or 2, Max MCS 0..13 or -1, the rest on or off.
inline void normalize(Command& c) {
    if (auto* a = std::get_if<cmd::RfControl>(&c)) {
        a->fixed_mhz = a->field == RF_CHAN ? (a->value > 0 ? a->value / 1000 : 0) : -1;
    } else if (auto* a = std::get_if<cmd::MaxBandwidth>(&c)) {
        const int m = a->mhz;
        a->mhz = m == 0 || m == 5 || m == 10 || m == 20 ? m : 40;
    } else if (auto* a = std::get_if<cmd::Firmware>(&c)) {
        a->firmware = a->firmware == 1 ? 1 : 0;
    } else if (auto* a = std::get_if<cmd::FrameMode>(&c)) {
        a->mode = a->mode == 0 ? 0 : 2;
    } else if (auto* a = std::get_if<cmd::MaxMcs>(&c)) {
        a->mcs = a->mcs >= 0 && a->mcs <= 13 ? a->mcs : -1;
    } else if (auto* a = std::get_if<cmd::Steady>(&c)) {
        a->on = a->on ? 1 : 0;
    } else if (auto* a = std::get_if<cmd::ChannelHop>(&c)) {
        a->hop = a->hop ? 1 : 0;
    }
}

}  // namespace radio
