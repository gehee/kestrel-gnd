#pragma once
// AR9311 screens follow the camera by their pixel clock (fpvOS kernel
// parameter artosyn_dsi.clock_trim_ppb), the VRX Pro's Refresh Sync done
// with the one knob the LT9611 bridge takes: its PCR recovers the HDMI clock
// from the DSI stream and follows a clock that moves up to ~5 ppm a second,
// but loses lock over frames of another length or over steps (a re-lock is
// a black screen for a moment: never a step, once the screen is up).
//
// Pure logic, fed once a second, so it is tested without a display
// (tools/test/clock_lock_test.cpp). Slack: how long before the commit
// deadline (the vblank less the screen's lead) a picture is ready, in
// [0, refresh). A trim of +1 ppm (a slower screen) moves every later vblank,
// and so the slack, by +1 us a second against the camera.
#include <algorithm>
#include <cmath>
#include <deque>

namespace display {

class ClockLock {
  public:
    struct Config {
        // Pictures ready this long before the deadline. The SteadyDigital's
        // arrivals spread about +-2 ms with a tail past 4 ms (decoder waits up
        // to 9 ms): at 4 ms ~0.3 refreshes a second still repeated, at 7 ms
        // ~0.05, and a picture waits about what it did at a random phase.
        double target_slack_us = 7000;
        // How fast the trim may move: the PCR followed 1-5 ppm/s ramps clean
        // (0 -> 400 -> 0 at 1, +-200 at 5), re-locked at 7.
        double slew_ppm_per_s = 4.0;
        // Once the trim is near where it is wanted: 1 ppm/s, the ramp the
        // PCR followed for minutes without a blink. 4 ppm/s is for catching
        // up only (another air unit, a first run). Held at 4 ppm/s, the
        // trim swung +-10 ppm round the camera's rate, and back-to-back
        // 4 ppm jumps (the trim was written once a second) made the PCR
        // slip (0x8397 0x52 -> 0x53) and re-lock every 10-80 s: black. The
        // screen now ramps each second's move over the second as well.
        double fine_slew_ppm_per_s = 1.0;
        double fine_band_ppm = 10;
        double max_drift_ppm = 200;     // fastest the slack is moved
        double max_trim_ppm = 2500;     // the kernel takes +-3000
        // Near the target: drift asked per us of error (0.05 chased the
        // slack's jitter: steps every second).
        double gain_per_s = 0.02;
        double window_s = 60;           // for the camera-rate estimate
        unsigned min_samples = 8;
    };

    explicit ClockLock(double trim_ppm = 0) : ClockLock(Config{}, trim_ppm) {}
    ClockLock(Config c, double trim_ppm) : c_(c), trim_(clamp_trim(trim_ppm)), base_(trim_) {}

    // A second's pictures: their median slack at time t (s). Returns the trim
    // to apply, ppm.
    double update(double t, double slack_us, double refresh_us) {
        if (have_ && (t - last_t_ > 3 || t <= last_t_)) reset();
        double dt = 1;
        if (have_) {
            dt = t - last_t_;
            integral_ += trim_ * dt;  // the trim held since the last sample
            // Unwrap: the slack crosses the refresh boundary while unlocked.
            double d = slack_us - last_slack_;
            while (d > refresh_us / 2) d -= refresh_us;
            while (d < -refresh_us / 2) d += refresh_us;
            phase_ += d;
        } else {
            phase_ = slack_us;
            integral_ = 0;
            have_ = true;
        }
        last_t_ = t;
        last_slack_ = slack_us;
        ++updates_;
        // phase(t) - integral(trim) falls at the camera's own rate (the trim
        // that would hold the slack): its slope is -base.
        history_.push_back({t, phase_ - integral_});
        while (!history_.empty() && t - history_.front().t > c_.window_s) history_.pop_front();
        if (history_.size() >= c_.min_samples) {
            const double slope = fit_slope();
            base_ = clamp_trim(estimated_ ? base_ + 0.05 * (-slope - base_) : -slope);
            estimated_ = true;
        }
        // Toward the target without crossing the deadline: the error is taken
        // inside [0, refresh), never round it. As fast as can still be undone
        // in time at the trim's slew (stopping distance v^2 / 2a), slower near
        // the target.
        const double err = c_.target_slack_us - slack_us;
        const double room = std::sqrt(2 * c_.slew_ppm_per_s * std::fabs(err)) * 0.7;
        const double drift =
            std::copysign(std::min({c_.max_drift_ppm, room, c_.gain_per_s * std::fabs(err)}), err);
        const double want = clamp_trim((estimated_ ? base_ : trim_) + drift);
        const bool far = std::fabs(want - trim_) > c_.fine_band_ppm;
        const double step = (far ? c_.slew_ppm_per_s : c_.fine_slew_ppm_per_s) * dt;
        trim_ += std::clamp(want - trim_, -step, step);
        return trim_;
    }

    double trim_ppm() const { return trim_; }
    double target_us() const { return c_.target_slack_us; }
    void set_target_us(double us) { c_.target_slack_us = us; }
    // The camera's rate as a trim (what holds the slack), once estimated.
    bool estimated() const { return estimated_; }
    double base_ppm() const { return base_; }
    void reset() {
        have_ = false;
        estimated_ = false;
        history_.clear();
        updates_ = 0;
    }
    // The camera's rate known already (an air unit seen before): the trim
    // goes there at the slew, not in a step, and the phase with it. Later
    // estimates refine it.
    void expect_rate(double ppm) {
        reset();
        base_ = clamp_trim(ppm);
        estimated_ = true;
    }
    // Seconds of pictures since the last reset or set_rate. The first
    // estimates swing (+240 ppm seen on the way to +135): an estimate is
    // worth keeping after a couple of minutes.
    unsigned updates() const { return updates_; }

  private:
    struct Sample { double t, q; };
    double fit_slope() const {
        double st = 0, sq = 0;
        for (auto &s : history_) { st += s.t; sq += s.q; }
        const double n = double(history_.size()), mt = st / n, mq = sq / n;
        double num = 0, den = 0;
        for (auto &s : history_) { num += (s.t - mt) * (s.q - mq); den += (s.t - mt) * (s.t - mt); }
        return den > 0 ? num / den : 0;
    }
    double clamp_trim(double v) const { return std::clamp(v, -c_.max_trim_ppm, c_.max_trim_ppm); }

    Config c_;
    double trim_, base_;
    bool have_ = false, estimated_ = false;
    double last_t_ = 0, last_slack_ = 0, phase_ = 0, integral_ = 0;
    unsigned updates_ = 0;
    std::deque<Sample> history_;
};

}  // namespace display
