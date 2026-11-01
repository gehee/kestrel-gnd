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

Model::Model(Settings& settings) : settings_(settings) {}

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

Ticket AirRadioPart::set_tx_power(int mw) { return ticket(m_.post(cmd::RfControl{RF_TX_POWER, mw, -1}), mw); }

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
// A channel picked is FIXED from here on: AUTO HOP is off with it (the
// ticket's value is the hop's).
Ticket GroundRadioPart::set_channel(int mhz) {
    const CommandMsg m = m_.post(cmd::RfControl{RF_CHAN, mhz * 1000, -1});
    return std::get<cmd::RfControl>(m.c).fixed_mhz > 0 ? ticket(m, 0) : Ticket();
}
Ticket GroundRadioPart::set_channel_auto(bool hop) {
    const CommandMsg m = m_.post(cmd::ChannelHop{hop ? 1 : 0});
    return ticket(m, std::get<cmd::ChannelHop>(m.c).hop);
}
void GroundRadioPart::search_channels() { m_.post(cmd::RfControl{RF_HOP, 1, -1}); }
void GroundRadioPart::scan_screen(bool open) { m_.post(cmd::ScanScreen{open}); }
void GroundRadioPart::bind() { m_.post(cmd::Bind{}); }
void GroundRadioPart::region_changed() { m_.post(cmd::RegionChanged{}); }

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
    if (const auto* a = std::get_if<cmd::RfControl>(&c)) {
        // A channel picked on the channel page is FIXED from here on;
        // AUTO HOP is off with it.
        if (a->fixed_mhz < 0) return;
        s.set(setting::ar8030_chan_mhz, a->fixed_mhz);
        if (a->fixed_mhz) s.set(setting::ar8030_chan_hop, 0);
    } else if (const auto* a = std::get_if<cmd::MaxBitrate>(&c)) {
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
    } else if (const auto* a = std::get_if<cmd::ChannelHop>(&c)) {
        s.set(setting::ar8030_chan_mhz, 0);                // no longer FIXED
        s.set(setting::ar8030_chan_hop, a->hop);
    }
}

// ---- the radio: what its threads learn -------------------------------------------

void Model::publish_radio() {
    const int d = radio_s_.dist_raw;
    radio_s_.dist_m = d < 0 ? -1 : dist_zero_ < 0 ? -2 : std::max(0, d - dist_zero_);
    radio_.publish(radio_s_);
}

void Model::link_stats(const gnd::LinkStats& v) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    radio_s_.link = v;
    // Another air unit: its own distance zero, if it is still the power-up
    // the saved one was taken in.
    if (v.state == gnd::kLinkUp && v.peer_mac[0] && strncmp(v.peer_mac, peer_mac_, sizeof(peer_mac_)) != 0) {
        snprintf(peer_mac_, sizeof(peer_mac_), "%s", v.peer_mac);
        air_boot_.clear();
        dist_zero_ = -1;
        dist_zero_unkeyed_ = false;
        standby_since_us_ = 0;
        lp_known_ = false;
        dist_pre_head_ = 0;
        memset(dist_pre_us_, 0, sizeof(dist_pre_us_));
    }
    publish_radio();
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

void Model::resend(const air::Resend& r) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    air_radio_.resend(r);
}

// Each setting to the part it belongs to: our radio's, the air unit's radio's
// (its power, its resends known), the encoder's (the bitrate cap). Each with
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
    radio_s_.chan_hop = h.chan_hop;
    radio_s_.bind_state = h.bind_state;
    radio_s_.bind_secs_left = h.bind_secs_left;
    radio_s_.bind_peer = h.bind_peer;
    radio_s_.taken = h.commands_taken;
    publish_radio();
    air_radio_s_.tx_power_mw = h.tx_power_mw;
    air_radio_s_.tx_power_want_mw = h.tx_power_want_mw;
    air_radio_s_.resend_known = h.resend_known;
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
    // Disarmed: kept, for the zero the arming takes.
    if (raw >= 0 && lp_known_ && low_power_) {
        dist_pre_[dist_pre_head_] = raw;
        dist_pre_us_[dist_pre_head_] = now_us();
        dist_pre_head_ = (dist_pre_head_ + 1) % 8;
    }
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
    low_power_quiet(on);
    flush_log();
}

void Model::low_power_quiet(bool on) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    // Standby -> radiating, after 3 s of standby seen (a link coming up can
    // read standby for a moment with the aircraft armed - a goggle restarted
    // in flight would zero where it flies), for a known air unit, no zero yet.
    const uint64_t now = now_us();
    if (lp_known_ && low_power_ && !on && dist_zero_ < 0 && peer_mac_[0] &&
        standby_since_us_ && now - standby_since_us_ >= 3000000ULL)
        dist_take_zero();
    if (on && !(lp_known_ && low_power_)) standby_since_us_ = now;
    if (!on) standby_since_us_ = 0;
    low_power_ = on;
    lp_known_ = true;
    unit_s_.low_power = on;
    unit_.publish(unit_s_);
    publish_radio();   // a zero taken at the arming
}

void Model::air_boot(const std::string& id) {
    air_boot_quiet(id);
    flush_log();
}

void Model::air_boot_quiet(const std::string& id) {
    std::lock_guard<bus::PiMutex> lk(mu_);
    if (id != air_boot_) {
        // A new power-up of the same air unit (it restarted while linked): zero again.
        if (!air_boot_.empty()) { dist_zero_ = -1; dist_pre_head_ = 0; memset(dist_pre_us_, 0, sizeof(dist_pre_us_)); }
        air_boot_ = id;
        // A zero taken before the id came (it is said every 2 s): this
        // power-up's saved one wins if there is one, else it is saved under it.
        if (dist_zero_unkeyed_ && !id.empty()) {
            const int taken = dist_zero_;
            dist_zero_ = -1;
            dist_load_zero();
            if (dist_zero_ < 0) {
                dist_zero_ = taken;
                settings_.set(dist_zero(), id + ":" + std::to_string(taken));
            }
            dist_zero_unkeyed_ = false;
        }
        dist_load_zero();
    }
    publish_radio();
}

// The distance's zero (see dist_zero_ above), saved as "<boot id>:<raw>"
// under dist_zero_<the air unit's radio MAC>.
MadeSetting<std::string> Model::dist_zero() const {
    std::string mac;
    for (const char *c = peer_mac_; *c; c++) if (*c != ':') mac += *c;
    return setting::dist_zero(mac);
}

void Model::dist_load_zero() {
    if (!peer_mac_[0] || air_boot_.empty() || dist_zero_ >= 0) return;
    const std::string v = settings_.get(dist_zero());
    const size_t colon = v.find(':');
    if (colon == std::string::npos || v.substr(0, colon) != air_boot_) return;
    dist_zero_ = atoi(v.c_str() + colon + 1);
    char line[160];
    snprintf(line, sizeof(line), "[OSD] air unit %s, power-up %s: distance zero %d (taken at its first arming)\n",
             peer_mac_, air_boot_.c_str(), dist_zero_);
    log_ += line;
}

// The zero: the median of the readings of the last 4 s before the arming
// (the aircraft still where it was armed - the reading after can already be
// a takeoff), the latest reading if there are none.
void Model::dist_take_zero() {
    const uint64_t now = now_us();
    int v[8], n = 0;
    for (int i = 0; i < 8; i++)
        if (dist_pre_us_[i] && now - dist_pre_us_[i] <= 4000000ULL) v[n++] = dist_pre_[i];
    if (!n && radio_s_.dist_raw >= 0) v[n++] = radio_s_.dist_raw;
    if (!n) return;
    std::sort(v, v + n);
    dist_zero_ = v[n / 2];
    dist_zero_unkeyed_ = air_boot_.empty();
    if (peer_mac_[0])
        settings_.set(dist_zero(), (air_boot_.empty() ? std::string("-") : air_boot_) + ":" +
                                                     std::to_string(dist_zero_));
    char line[200];
    snprintf(line, sizeof(line), "[OSD] air unit %s armed: distance zero %d (median of %d reading%s before; power-up %s)\n",
             peer_mac_[0] ? peer_mac_ : "?", dist_zero_, n, n == 1 ? "" : "s",
             air_boot_.empty() ? "unknown" : air_boot_.c_str());
    log_ += line;
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
