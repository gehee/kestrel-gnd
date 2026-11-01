#pragma once
// The radio's model, apart from any radio: the parts of the system the radio
// carries (system/air/, system/gnd/) - the air unit's camera, encoder, radio
// and the unit itself, its FC bytes and IMU samples, our radio, the pictures'
// timing - each one's state made from what the radio's threads report, with
// the logic that is the link's rather than the chip's (the distance's zero);
// and the changes asked of them: numbered, saved when they are settings, and
// queued for the radio, which takes them between its steps. No thread of its
// own: its callers' run it. One is made at startup; it outlives the radio.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "base/bus.hpp"
#include "radio/link.hpp"
#include "radio/radio.hpp"
#include "radio/settings.hpp"
#include "radio/streams.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/radio.hpp"
#include "system/air/unit.hpp"
#include "system/gnd/radio.hpp"
#include "system/gnd/timing.hpp"

namespace radio {

class Model;

namespace detail {
// A part's state as the model keeps it: the newest in a seqlock slot, which
// any thread reads without waiting; the watcher poked at each publish. One
// writer at a time (the model's lock, or one thread).
template <class I> class Held : public I {
public:
    using S = decltype(std::declval<const I&>().state());
    explicit Held(Model& m) : m_(m) {}
    S state() const override { S s; slot_.read(s); return s; }
    uint32_t version() const override { return slot_.version(); }
    void watch(bus::Waker* w) override { slot_.set_waker(w); watched_ = w != nullptr; }
    void publish(const S& s) { slot_.publish(s); }
    bool watched() const { return watched_; }
protected:
    Model& m_;
private:
    bus::Topic<S> slot_;
    bool watched_ = false;
};
}  // namespace detail

class CameraPart final : public detail::Held<air::Camera> {
public:
    using Held::Held;
    Ticket set(air::CameraField f, int value) override;
    Ticket set_mode(int index) override;
};

class EncoderPart final : public detail::Held<air::Encoder> {
public:
    using Held::Held;
    Ticket set_max_bitrate(int kbps) override;
};

class AirRadioPart final : public detail::Held<air::Radio> {
public:
    using Held::Held;
    Ticket set_tx_power(int mw) override;
};

class UnitPart final : public detail::Held<air::Unit> {
public:
    using Held::Held;
    Ticket set_standby(bool on) override;
    Ticket set_logs(bool on) override;
};

class GroundRadioPart final : public detail::Held<gnd::Radio> {
public:
    using Held::Held;
    void watch(bus::Waker* w) override {
        Held::watch(w);
        scans_.set_waker(w);
        rates_.set_waker(w);
    }
    Ticket set_firmware(int firmware) override;
    Ticket set_frame(int mode) override;
    Ticket set_max_mcs(int mcs) override;
    Ticket set_max_bandwidth(int mhz) override;
    Ticket set_steady(bool on) override;
    Ticket set_channel(int mhz) override;
    Ticket set_channel_auto(bool hop) override;
    void search_channels() override;
    void scan_screen(bool open) override;
    void bind() override;
    void set_dist_offset(int raw) override;
    void region_changed() override;
    bool take_scan(gnd::ScanSample& s) override { return scans_.pop(s); }
    bool take_rate(float& mbps) override { return rates_.pop(mbps); }
    uint64_t dropped() const override { return scans_.drops() + rates_.drops(); }
    // Under the model's lock.
    void scan(const gnd::ScanSample& s) { scans_.push(s); }
    void rate(float mbps) { rates_.push(mbps); }
private:
    bus::Ring<gnd::ScanSample, 16> scans_;
    bus::Ring<float, 16> rates_;
};

class TimingPart final : public detail::Held<gnd::Timing> {
public:
    using Held::Held;
};

class Model {
public:
    Model(Settings& settings);
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // ---- the parts, for whoever shows or sets them ---------------------------
    // A watcher is set on each before the radio runs.
    air::Camera& camera() { return camera_; }
    air::Encoder& encoder() { return encoder_; }
    air::Radio& air_radio() { return air_radio_; }
    air::Unit& air_unit() { return unit_; }
    FcFeed& fc() { return fc_; }               // the radio writes it, its reader takes it
    ImuFeed& imu() { return imu_; }
    gnd::Radio& radio() { return radio_; }
    gnd::Timing& timing() { return timing_; }

    // Our radio's state has a watcher (the HUD): what only such a reader
    // needs (the channel scan, the link figures' poll) is skipped without one.
    bool viewers() const { return radio_.watched(); }

    // ---- the radio: what its threads learn ------------------------------------
    void link_stats(const gnd::LinkStats& v);  // the stats thread's full poll
    void link_state(int state);                // the chip's event, or the poll: the state alone
    void rx_mcs(int mcs);                      // the chip's MCS event
    void distance(int raw);                    // the ranging reading, metres (-1 none)
    void low_power(bool on);                   // standby power or radiating
    void video_bandwidth(float mbps);          // each second (the radio thread): the link's figure and the rate
    // What a part's state holds, changed by f and published.
    template <class F> void update_camera(F&& f) { update(camera_, cam_, f); }
    template <class F> void update_unit(F&& f) { update(unit_, unit_s_, f); }
    template <class F> void update_air_radio(F&& f) { update(air_radio_, air_radio_s_, f); }
    template <class F> void update_ground(F&& f) { update(radio_, radio_s_, f); }
    void air_info(const air::Info& a) { update_unit([&](air::UnitState& u) { u.info = a; }); }
    // What the radio holds (radio::Link::held), after its commands: each part
    // it belongs to published when it changes.
    void held(const State& h);
    // Each picture's (the radio thread only).
    void air_delay(float ms) { tim_.air_delay_ms = ms; tim_.air_delay_valid = true; timing_.publish(tim_); }
    void clear_air_delay() { tim_.air_delay_valid = false; timing_.publish(tim_); }
    void reassembly(float ms) { tim_.rsm_ms = ms; timing_.publish(tim_); }
    void scan(const gnd::ScanSample& sc);
    void set_dist_offset(int raw);             // HUD > Calib Distance (gnd::Radio::set_dist_offset)

    // The radio thread, between its steps: the changes asked since, each
    // carried out by the radio (link.apply), then what it holds (link.held).
    void take_commands(Link& link);

    // A change asked of a part: normalized (radio::normalize), saved if it is
    // a setting, numbered and queued for the radio. Any thread.
    CommandMsg post(Command c);
    // What one change saves, if it is a setting (on the caller's thread).
    static void save(Settings& s, const Command& c);

private:
    template <class P, class S, class F> void update(P& part, S& s, F& f) {
        std::lock_guard<bus::PiMutex> lk(mu_);
        f(s);
        part.publish(s);
    }
    // ---- the model's own (mu_: the radio's threads; no reader takes it) ----
    void publish_radio();                      // under mu_
    MadeSetting<int> dist_offset(int def) const;   // dist_offset_<the air unit's radio MAC>
    void flush_log();

    Settings& settings_;
    CameraPart camera_{*this};
    EncoderPart encoder_{*this};
    AirRadioPart air_radio_{*this};
    UnitPart unit_{*this};
    FcFeed fc_;
    ImuFeed imu_;
    GroundRadioPart radio_{*this};
    TimingPart timing_{*this};

    bus::PiMutex mu_;                    // the radio thread is SCHED_FIFO; the others are not
    std::string log_;                    // printed once mu_ is let go (flush_log)
    air::CameraState cam_;
    air::EncoderState encoder_s_;
    air::RadioState air_radio_s_;
    air::UnitState unit_s_;
    gnd::RadioState radio_s_;
    gnd::TimingState tim_;               // the radio thread's alone
    State held_;                         // as last published

    char peer_mac_[20] = {0};            // the air unit linked: whose Calib Distance offset is in use

    // ---- the changes asked, for the radio: in order, numbered as they are
    // queued (post_mu_ among the askers), taken by the radio thread alone.
    // Deep enough for the menu's bursts while the radio is in a bring-up
    // attempt; none asked before it runs is lost.
    bus::PiMutex post_mu_;
    uint32_t seq_ = 0;                   // under post_mu_
    bus::Ring<CommandMsg, 256> commands_;
    std::atomic<uint32_t> taken_{0};     // the last one the radio carried out
};

}  // namespace radio
