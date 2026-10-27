#include "hud/actions.hpp"

#include <cstdio>
#include <cstdlib>

#include "artosyn/ar8030_source.hpp"
#include "settings.hpp"
#include "utils/sd_log.hpp"

namespace hud {

// The old command codes were 0x300 + CamField and 0x310 + RfField: the
// fields the actions carry are those same numbers.
static_assert(Ar8030Source::CAM_EV == 1 && Ar8030Source::CAM_FOCUS == 6 && Ar8030Source::CAM_AWB == 7 &&
              Ar8030Source::CAM_STANDBY == 8 && Ar8030Source::CAM_ANGLE == 11 && Ar8030Source::CAM_3DNR == 13,
              "camera fields moved: the VIDEO tab's actions would set the wrong ones");
static_assert(Ar8030Source::RF_TX_POWER == 1 && Ar8030Source::RF_HOP == 6, "RF fields moved");

namespace {

// The goggle's radio config is loaded with the radio, so a change to it
// restarts the radio, and kestrel-gnd with it (the driver cannot go while
// kestrel-gnd holds it) - from a shell of its own.
void restart_radio() {
    Settings::getInstance().flush();   // bb_only.sh reads it
    if (system("setsid sh -c '/etc/init.d/S70kestrel stop; /etc/init.d/S60ar8030 restart; "
               "sleep 2; /etc/init.d/S70kestrel start' </dev/null >/dev/null 2>&1 &") != 0)
        printf("menu: could not restart the radio\n");
}

void caps_now() {
    Ar8030Source::caps_live.store(true);
    Ar8030Source::caps_due.store(true);
}

struct Dispatch {
    void operator()(const action::ScanScreen &a) const {
        Ar8030Source::scan_active = a.open;
    }
    void operator()(const action::DebugInfo &a) const {
        sd_log::set_enabled(a.on);
        Ar8030Source::air_logs = a.on;
        Ar8030Source::request_setting(Ar8030Source::CAM_AIR_LOGS, a.on);
    }
    void operator()(const action::CameraSetting &a) const {
        Ar8030Source::request_setting(a.field, a.value);
    }
    void operator()(const action::RfControl &a) const {
        Ar8030Source::request_rf(a.field, a.value);
    }
    void operator()(const action::MaxBitrate &a) const {
        // Kept here and sent at every link-up.
        Ar8030Source::max_kbps = a.kbps;
        Settings::getInstance().set("ar8030_max_kbps", a.kbps);
        Ar8030Source::request_setting(Ar8030Source::CAM_MAX_KBPS, a.kbps);
    }
    void operator()(const action::MaxBandwidth &a) const {
        // Kept here (and sent to a kestrel-air that takes it, at every
        // link-up). Open8030 here: our radio takes it live (SET 22), 5 to 40
        // MHz. Stock: the goggle's radio config caps the width (bb_only.sh
        // picks it from this setting), so a change restarts the radio.
        const int val = a.mhz;
        if (Ar8030Source::radio_fw == 1) {
            Ar8030Source::max_bw_mhz = val == 0 || val == 5 || val == 10 || val == 20 ? val : 40;
            Settings::getInstance().set("ar8030_max_bw_mhz", Ar8030Source::max_bw_mhz);
            Ar8030Source::request_setting(Ar8030Source::CAM_MAX_BW,
                Ar8030Source::max_bw_mhz == 0 || Ar8030Source::max_bw_mhz >= 40 ? 40 : 20);
            caps_now();
            Ar8030Source::region_changed();   // AUTO's channels are judged within this cap
            return;
        }
        const int was = Ar8030Source::max_bw_mhz;
        Ar8030Source::max_bw_mhz = (val == 20) ? 20 : 40;
        Settings::getInstance().set("ar8030_max_bw_mhz", Ar8030Source::max_bw_mhz);
        Ar8030Source::request_setting(Ar8030Source::CAM_MAX_BW, Ar8030Source::max_bw_mhz);
        if (Ar8030Source::max_bw_mhz != was) {
            printf("menu: max bandwidth %d MHz - restarting the radio with it\n", Ar8030Source::max_bw_mhz);
            fflush(stdout);
            restart_radio();
        }
    }
    void operator()(const action::RadioFirmware &a) const {
        // Which baseband firmware bb_only.sh uploads with the radio (0 stock,
        // 1 Open8030, when its image is on the unit). Loaded with the radio,
        // so a change restarts the radio, as Max Bandwidth does.
        const int was = Ar8030Source::radio_fw;
        Ar8030Source::radio_fw = a.firmware == 1 ? 1 : 0;
        Settings::getInstance().set("ar8030_firmware", Ar8030Source::radio_fw);
        if (Ar8030Source::radio_fw != was) {
            printf("menu: radio firmware %s - restarting the radio with it\n",
                   Ar8030Source::radio_fw ? "Open8030" : "stock");
            fflush(stdout);
            restart_radio();
        }
    }
    void operator()(const action::RadioFrame &a) const {
        // Kept here, sent now and after every link-up.
        Ar8030Source::frame_mode = (a.mode == 0) ? 0 : 2;
        Settings::getInstance().set("ar8030_frame_mode", Ar8030Source::frame_mode);
        Ar8030Source::frame_mode_changed.store(true);
    }
    void operator()(const action::MaxMcs &a) const {
        // The highest MCS our radio's receive policy may use (Open8030), -1
        // none. Live; again at every link-up.
        Ar8030Source::max_mcs = a.mcs >= 0 && a.mcs <= 13 ? a.mcs : -1;
        Settings::getInstance().set("ar8030_max_mcs", Ar8030Source::max_mcs);
        caps_now();
    }
    void operator()(const action::SteadyLatency &a) const {
        // A block the air radio sent again steps our radio's MCS down
        // (Open8030). Live; again at link-up.
        Ar8030Source::steady = a.on ? 1 : 0;
        Settings::getInstance().set("ar8030_steady", Ar8030Source::steady);
        caps_now();
    }
    void operator()(const action::ChannelHop &a) const {
        // Saved; AUTO's pick is made now.
        Ar8030Source::chan_hop = a.hop ? 1 : 0;
        Settings::getInstance().set("ar8030_chan_hop", Ar8030Source::chan_hop.load());
        Ar8030Source::region_pick = true;
        Ar8030Source::region_changed();
    }
    void operator()(const action::VideoMode &a) const {
        // The camera is reached over the baseband's port-2 control socket
        // (SET_CHN_RES), and it has its own list of modes.
        Ar8030Source::request_mode_index(a.index);
    }
    void operator()(const action::Bind &) const {
        // Parked for the RX thread: the menu must never call into the radio's SDK.
        Ar8030Source::request_bind();
    }
    void operator()(const action::RegionChanged &) const {
        Ar8030Source::region_changed();
    }
    void operator()(const action::Unknown &a) const {
        printf("Unknown command 0x%x = %d\n", a.code, a.value);
    }
};

}  // namespace

void dispatch(const Action &a) { std::visit(Dispatch{}, a); }

}  // namespace hud
