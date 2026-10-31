#include "radio/model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <variant>

#include "base/settings.hpp"

namespace radio {

namespace {
uint64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}
}  // namespace

Model::Model(Settings& settings) : settings_(settings) {
    radio_s_.dist_offset = settings_.get(setting::dist_offset);
}

// ---- the parts: the changes asked of them -------------------------------------

// The camera's fields are the radio's own numbers for them.
static_assert((int)air::CameraField::EV == CAM_EV && (int)air::CameraField::SAT == CAM_SAT &&
              (int)air::CameraField::CONTRAST == CAM_CONTRAST && (int)air::CameraField::SHARPNESS == CAM_SHARPNESS &&
              (int)air::CameraField::SCENE == CAM_SCENE && (int)air::CameraField::FOCUS == CAM_FOCUS &&
              (int)air::CameraField::AWB == CAM_AWB && (int)air::CameraField::ANGLE == CAM_ANGLE &&
              (int)air::CameraField::DNR3D == CAM_3DNR,
              "camera fields moved: the VIDEO tab would set the wrong ones");

namespace {
Ticket ticket(const CommandMsg& m, int value) { return Ticket{m.seq, value}; }
}  // namespace

Ticket CameraPart::set(air::CameraField f, int value) {
    return ticket(m_.post(cmd::CameraSetting{(int)f, value}), value);
}
Ticket CameraPart::set_mode(int index) { return ticket(m_.post(cmd::VideoMode{index}), index); }

Ticket EncoderPart::set_max_bitrate(int kbps) {
    const CommandMsg m = m_.post(cmd::MaxBitrate{kbps});
    return ticket(m, std::get<cmd::MaxBitrate>(m.c).kbps);
}

Ticket AirRadioPart::set_tx_power(int mw) { return ticket(m_.post(cmd::RfControl{RF_TX_POWER, mw}), mw); }

Ticket UnitPart::set_standby(bool on) { return ticket(m_.post(cmd::CameraSetting{CAM_STANDBY, on ? 1 : 0}), on); }
Ticket UnitPart::set_logs(bool on) { return ticket(m_.post(cmd::AirLogs{on}), on); }

Ticket GroundRadioPart::set_firmware(int firmware) {
    const CommandMsg m = m_.post(cmd::Firmware{firmware});
    return ticket(m, std::get<cmd::Firmware>(m.c).firmware);
}
Ticket GroundRadioPart::set_frame(int mode) {
    const CommandMsg m = m_.post(cmd::FrameMode{mode});
    return ticket(m, std::get<cmd::FrameMode>(m.c).mode);
}
Ticket GroundRadioPart::set_max_mcs(int mcs) {
    const CommandMsg m = m_.post(cmd::MaxMcs{mcs});
    return ticket(m, std::get<cmd::MaxMcs>(m.c).mcs);
}
Ticket GroundRadioPart::set_max_bandwidth(int mhz) {
    const CommandMsg m = m_.post(cmd::MaxBandwidth{mhz});
    return ticket(m, std::get<cmd::MaxBandwidth>(m.c).mhz);
}
Ticket GroundRadioPart::set_steady(bool on) {
    const CommandMsg m = m_.post(cmd::Steady{on ? 1 : 0});
    return ticket(m, std::get<cmd::Steady>(m.c).on);
}
// A channel picked lasts for the session: no setting, no ticket.
Ticket GroundRadioPart::set_channel(int mhz) {
    m_.post(cmd::RfControl{RF_CHAN, mhz * 1000});
    return Ticket();
}
void GroundRadioPart::search_channels() { m_.post(cmd::RfControl{RF_HOP, 1}); }
void GroundRadioPart::scan_screen(bool open) { m_.post(cmd::ScanScreen{open}); }
void GroundRadioPart::bind() { m_.post(cmd::Bind{}); }
void GroundRadioPart::set_dist_offset(int raw) { m_.set_dist_offset(raw); }

CommandMsg Model::post(Command c) {
    normalize(c);
    save(settings_, c);
    CommandMsg m;
    m.c = c;
    bool landed;
    {
        std::lock_guard<bus::PiMutex> lk(post_mu_);
        m.seq = ++seq_;
        landed = commands_.push(m);
    }
    if (!landed) printf("radio: a command found a full queue: %u dropped\n", m.seq);
    return m;
}

// The commands come normalized (radio::normalize): saved as they are.
void Model::save(Settings& s, const Command& c) {
    if (const auto* a = std::get_if<cmd::MaxBitrate>(&c)) {
        s.set(setting::ar8030_max_kbps, a->kbps);          // sent at every link-up
    } else if (const auto* a = std::get_if<cmd::MaxBandwidth>(&c)) {
        s.set(setting::ar8030_max_bw_mhz, a->mhz);         // and to a kestrel-air that takes it
    } else if (const auto* a = std::get_if<cmd::Firmware>(&c)) {
        s.set(setting::ar8030_firmware, a->firmware);      // bb_only.sh loads it with the radio
    } else if (const auto* a = std::get_if<cmd::FrameMode>(&c)) {
        s.set(setting::ar8030_frame_mode, a->mode);
    } else if (const auto* a = std::get_if<cmd::MaxMcs>(&c)) {
        s.set(setting::ar8030_max_mcs, a->mcs);
    } else if (const auto* a = std::get_if<cmd::Steady>(&c)) {
        s.set(setting::ar8030_steady, a->on);
    }
}

// ---- the radio: what its threads learn -------------------------------------------

void Model::publish_radio() {
    const int d = radio_s_.dist_raw;
    radio_s_.dist_m = d < 0 ? -1 : std::max(0, d - radio_s_.dist_offset);
    radio_.publish(radio_s_);
}

void Model::link_stats(const gnd::LinkStats& v) {
    {
        std::lock_guard<bus::PiMutex> lk(mu_);
        radio_s_.link = v;
        // Calib Distance is per air unit (its radio's MAC): one never
        // calibrated starts from the last calibration made.
        if (v.state == gnd::kLinkUp && v.peer_mac[0] && strncmp(v.peer_mac, peer_mac_, sizeof(peer_mac_)) != 0) {
            snprintf(peer_mac_, sizeof(peer_mac_), "%s", v.peer_mac);
            radio_s_.dist_offset = settings_.get(dist_offset(settings_.get(setting::dist_offset)));
            char line[96];
            snprintf(line, sizeof(line), "[OSD] air unit %s: distance offset %d\n", peer_mac_, radio_s_.dist_offset);
            log_ += line;
        }
        publish_radio();
    }
    flush_log();
}

// The state alone, every other figure at its last value: the full poll is
// slow, and this is what says the link went.
void Model::link_state(int state) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    const int prev = radio_s_.link.state;
    if (state == prev) return;                 // the poll's: nothing new
    radio_s_.link.state = state;
    publish_radio();
}

void Model::video_bandwidth(float mbps) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_s_.video_mbps = mbps;
    publish_radio();
    radio_.rate(mbps);
}

void Model::scan(const gnd::ScanSample& sc) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_.scan(sc);
}

// Each setting to the part it belongs to: our radio's, the air unit's radio's
// (its power), the encoder's (the bitrate cap). Each with
// the last change carried out.
void Model::held(const State& h) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    if (h == held_) return;
    held_ = h;
    radio_s_.radio_fw = h.radio_fw;
    radio_s_.max_mcs = h.max_mcs;
    radio_s_.frame_mode = h.frame_mode;
    radio_s_.max_bw_mhz = h.max_bw_mhz;
    radio_s_.steady = h.steady;
    radio_s_.chan_auto = h.chan_auto;
    radio_s_.bind_state = h.bind_state;
    radio_s_.bind_secs_left = h.bind_secs_left;
    radio_s_.bind_peer = h.bind_peer;
    radio_s_.taken = h.commands_taken;
    publish_radio();
    air_radio_s_.tx_power_mw = h.tx_power_mw;
    air_radio_s_.tx_power_want_mw = h.tx_power_want_mw;
    air_radio_s_.taken = h.commands_taken;
    air_radio_.publish(air_radio_s_);
    encoder_s_.max_kbps = h.max_kbps;
    encoder_s_.taken = h.commands_taken;
    encoder_.publish(encoder_s_);
}

void Model::rx_mcs(int mcs) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_s_.link.rx_mcs_val = mcs;
    publish_radio();
}

void Model::distance(int raw) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_s_.dist_raw = raw;
    publish_radio();
}

void Model::flush_log() {
    std::string out;
    {
        std::lock_guard<bus::PiMutex> lk(mu_);
        out.swap(log_);
    }
    if (!out.empty()) fputs(out.c_str(), stdout);
}

void Model::low_power(bool on) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    unit_s_.low_power = on;
    unit_.publish(unit_s_);
}

// Calib Distance's setting for the linked air unit: dist_offset_<its radio's
// MAC>, or the plain dist_offset before any link.
MadeSetting<int> Model::dist_offset(int def) const {
    std::string mac;
    for (const char *c = peer_mac_; *c; c++) if (*c != ':') mac += *c;
    return setting::air_dist_offset(mac, def);
}

// Saved for this air unit, and as the starting value for one never calibrated.
void Model::set_dist_offset(int raw) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_s_.dist_offset = std::max(0, raw);
    if (peer_mac_[0]) settings_.set(dist_offset(0), radio_s_.dist_offset);
    settings_.set(setting::dist_offset, radio_s_.dist_offset);
    publish_radio();
}

// ---- commands ------------------------------------------------------------------

void Model::take_commands(Link& l) {
    CommandMsg m;
    while (commands_.pop(m)) {
        l.apply(m.c);
        taken_.store(m.seq);
    }
    // What it holds now: what the commands set, its pairing window's
    // countdown, what its other threads changed since the last pass.
    State h = l.held();
    h.commands_taken = taken_.load();
    held(h);
}

}  // namespace radio
