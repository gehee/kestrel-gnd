#include "imu_stream.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

namespace stab {

namespace {

constexpr char kUuid[17] = "kestrel-air-IMU1";
constexpr int64_t kGapUs = 50000;         // no gyro integration across a longer hole
constexpr int64_t kBackJumpUs = 1000000;  // the air clock going back this far is a restart
constexpr double kPi = 3.14159265358979323846;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
int16_t rds16(const uint8_t* p) { return (int16_t)rd16(p); }
uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

}  // namespace

ImuStream& ImuStream::get() {
    static ImuStream s;
    return s;
}

void ImuStream::clear_locked() {
    head_ = 0;
    count_ = 0;
    q_ = Quat();
    still_us_ = 0;
    blk_n_ = 0;
    blk_sq_ = 0;
    blk_sum_[0] = blk_sum_[1] = blk_sum_[2] = 0;
}

void ImuStream::reset() {
    std::lock_guard<std::mutex> l(mu_);
    clear_locked();
}

void ImuStream::set_camera_angle(int angle) {
    angle = angle == 180 ? 180 : 0;
    std::lock_guard<std::mutex> l(mu_);
    if (angle == angle_) return;
    angle_ = angle;
    clear_locked();
}

void ImuStream::test_fix_bias(const double dps[3]) {
    std::lock_guard<std::mutex> l(mu_);
    for (int i = 0; i < 3; i++) bias_[i] = dps[i] * kPi / 180.0;
    bias_fixed_ = true;
}

// gyro_rad: the IMU's own axes, rad/s.
void ImuStream::add_sample(int64_t t, const double g[3], const double a[3]) {
    // Learn the gyro's offset while the unit sits still: 250-sample blocks whose spread is tiny
    // and whose mean is small pull the bias estimate towards the block mean.
    if (!bias_fixed_) {
        for (int i = 0; i < 3; i++) blk_sum_[i] += g[i];
        blk_sq_ += g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
        if (++blk_n_ == 250) {
            double m[3] = { blk_sum_[0] / 250, blk_sum_[1] / 250, blk_sum_[2] / 250 };
            double var = blk_sq_ / 250 - (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            double sd_dps = std::sqrt(std::max(var, 0.0)) * 180.0 / kPi;
            double mean_dps = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) * 180.0 / kPi;
            if (sd_dps < 0.6 && mean_dps < 4.0) {
                double k = (bias_[0] == 0 && bias_[1] == 0 && bias_[2] == 0) ? 1.0 : 0.2;
                for (int i = 0; i < 3; i++) bias_[i] += k * (m[i] - bias_[i]);
            }
            blk_n_ = 0;
            blk_sq_ = 0;
            blk_sum_[0] = blk_sum_[1] = blk_sum_[2] = 0;
        }
    }

    // Into the camera's axes (OpenCV: x right, y down, z forward), from the motion test:
    // the chip sits behind the lens turned half a turn about its y axis; a 180 degree camera
    // angle turns the picture half a turn about the optical axis.
    double u[3] = { g[0] - bias_[0], g[1] - bias_[1], g[2] - bias_[2] };
    double w[3] = { -u[0], u[1], -u[2] };
    if (angle_ == 180) { w[0] = -w[0]; w[1] = -w[1]; }

    // The accelerometer in the camera's axes, the same flips as the gyro's. Resting gravity: while
    // nothing turns and it reads 1 g, average it; any movement starts over.
    double ac[3] = { -a[0], a[1], -a[2] };
    if (angle_ == 180) { ac[0] = -ac[0]; ac[1] = -ac[1]; }
    {
        const double wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 180.0 / kPi;
        const double an = std::sqrt(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
        if (wn < 3.0 && std::fabs(an - 1.0) < 0.04) {
            const int64_t dt = count_ ? t - at(count_ - 1).t : 0;
            if (still_us_ == 0 || dt <= 0 || dt > kGapUs) {
                for (int i = 0; i < 3; i++) acc_lp_[i] = ac[i];
                still_us_ = 1;
            } else {
                const double k = std::min(1.0, dt * 1e-6 / 0.4);
                for (int i = 0; i < 3; i++) acc_lp_[i] += k * (ac[i] - acc_lp_[i]);
                still_us_ += dt;
            }
        } else {
            still_us_ = 0;
        }
    }

    if (count_) {
        const Entry& last = at(count_ - 1);
        if (t <= last.t) return;                              // a repeat
        if (t < last.t - kBackJumpUs) { /* handled by the caller */ }
        int64_t dt = t - last.t;
        if (dt <= kGapUs) {
            double dtv = dt * 1e-6, v[3];
            for (int i = 0; i < 3; i++) v[i] = 0.5 * (last.w[i] + w[i]) * dtv;
            q_ = qnorm(qmul(q_, qexp(v)));
        } else {
            gaps_++;
        }
    }
    Entry e;
    e.t = t;
    e.q = q_;
    e.w[0] = w[0]; e.w[1] = w[1]; e.w[2] = w[2];
    if (count_ < ring_.size()) {
        ring_[(head_ + count_) % ring_.size()] = e;
        count_++;
    } else {
        ring_[head_] = e;
        head_ = (head_ + 1) % ring_.size();
    }
    samples_++;
}

bool ImuStream::on_sei_nal(const uint8_t* nal, size_t len) {
    if (len < 2 + 3) return false;
    int type = (nal[0] >> 1) & 0x3f;
    if (type != 39 && type != 40) return false;

    // remove emulation prevention bytes
    static thread_local std::vector<uint8_t> rb;
    rb.clear();
    rb.reserve(len);
    int zeros = 0;
    for (size_t i = 2; i < len; i++) {
        uint8_t c = nal[i];
        if (zeros >= 2 && c == 3) { zeros = 0; continue; }
        rb.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }

    size_t i = 0;
    bool ours = false;
    while (i + 2 <= rb.size() && rb[i] != 0x80) {
        unsigned pt = 0, ps = 0;
        while (i < rb.size() && rb[i] == 0xff) { pt += 255; i++; }
        if (i >= rb.size()) break;
        pt += rb[i++];
        while (i < rb.size() && rb[i] == 0xff) { ps += 255; i++; }
        if (i >= rb.size()) break;
        ps += rb[i++];
        if (i + ps > rb.size()) break;
        const uint8_t* p = rb.data() + i;
        i += ps;
        if (pt != 5 || ps < 16 + 26 || memcmp(p, kUuid, 16) != 0) continue;
        if (p[16] != 1) continue;                      // a version this does not know
        const int n = rd16(p + 16 + 20);
        const double glsb = rd16(p + 16 + 22) / 10.0;  // LSB per degree per second
        const double alsb = rd16(p + 16 + 24);         // LSB per g
        if (glsb <= 0 || alsb <= 0 || (size_t)(16 + 26 + n * 14) > ps) continue;
        const uint64_t t0 = rd64(p + 16 + 12);
        ours = true;

        std::lock_guard<std::mutex> l(mu_);
        messages_++;
        last_rx_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int k = 0; k < n; k++) {
            const uint8_t* s = p + 16 + 26 + k * 14;
            int64_t t = (int64_t)t0 + rd16(s);
            if (count_ && t < at(count_ - 1).t - kBackJumpUs) {   // the air clock restarted
                clear_locked();
                resets_++;
            }
            double g[3];
            for (int j = 0; j < 3; j++) g[j] = rds16(s + 2 + 2 * j) / glsb * kPi / 180.0;
            double a[3];
            for (int j = 0; j < 3; j++) a[j] = rds16(s + 8 + 2 * j) / alsb;
            add_sample(t, g, a);
        }
    }
    return ours;
}

bool ImuStream::orientation_at(int64_t t, Quat* out) const {
    std::lock_guard<std::mutex> l(mu_);
    if (count_ < 2) return false;
    const Entry& first = at(0);
    const Entry& last = at(count_ - 1);
    if (t < first.t) return false;
    if (t >= last.t) {
        // The newest sample is a little older than the picture: carry on at its rate.
        int64_t d = t - last.t;
        if (d > 15000) return false;
        double v[3] = { last.w[0] * d * 1e-6, last.w[1] * d * 1e-6, last.w[2] * d * 1e-6 };
        *out = qnorm(qmul(last.q, qexp(v)));
        return true;
    }
    size_t lo = 0, hi = count_ - 1;       // at(lo).t <= t < at(hi).t
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (at(mid).t <= t) lo = mid; else hi = mid;
    }
    const Entry& a = at(lo);
    const Entry& b = at(hi);
    if (b.t - a.t > kGapUs) return false;
    double f = (double)(t - a.t) / (double)(b.t - a.t);
    *out = qslerp(a.q, b.q, f);
    return true;
}

bool ImuStream::recent_rate_dps(int64_t window_us, double w_dps[3]) const {
    std::lock_guard<std::mutex> l(mu_);
    if (!count_ || last_rx_ms_ < 0) return false;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - last_rx_ms_ > 100) return false;
    const int64_t newest = at(count_ - 1).t;
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (size_t k = count_; k > 0 && newest - at(k - 1).t <= window_us; k--, n++)
        for (int i = 0; i < 3; i++) sum[i] += at(k - 1).w[i];
    if (!n) return false;
    for (int i = 0; i < 3; i++) w_dps[i] = sum[i] / n * 180.0 / kPi;
    return true;
}

bool ImuStream::resting_down(double down[3]) const {
    std::lock_guard<std::mutex> l(mu_);
    if (!count_ || last_rx_ms_ < 0 || still_us_ < 500000) return false;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - last_rx_ms_ > 100) return false;
    const double n = std::sqrt(acc_lp_[0] * acc_lp_[0] + acc_lp_[1] * acc_lp_[1] + acc_lp_[2] * acc_lp_[2]);
    if (n < 0.5) return false;
    for (int i = 0; i < 3; i++) down[i] = -acc_lp_[i] / n;       // the accelerometer reads up
    return true;
}

int64_t ImuStream::extend_stamp(uint32_t cap32) const {
    std::lock_guard<std::mutex> l(mu_);
    if (!count_) return cap32;
    int64_t ref = at(count_ - 1).t;
    int64_t c = (ref & ~0xffffffffLL) | cap32;
    if (c - ref > (1LL << 31)) c -= (1LL << 32);
    else if (ref - c > (1LL << 31)) c += (1LL << 32);
    return c;
}

ImuStream::Status ImuStream::status() const {
    std::lock_guard<std::mutex> l(mu_);
    Status s;
    if (last_rx_ms_ >= 0)
        s.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count() - last_rx_ms_;
    s.samples = samples_;
    s.messages = messages_;
    s.resets = resets_;
    s.gaps = gaps_;
    for (int i = 0; i < 3; i++) s.bias_dps[i] = bias_[i] * 180.0 / kPi;
    if (count_ >= 2) {
        const Entry& last = at(count_ - 1);
        s.newest_us = last.t;
        size_t k = count_ - 1;
        while (k > 0 && last.t - at(k).t < 1000000) k--;
        s.rate_hz = (double)(count_ - 1 - k) * 1e6 / std::max<int64_t>(last.t - at(k).t, 1);
        s.rate_dps = std::sqrt(last.w[0] * last.w[0] + last.w[1] * last.w[1] + last.w[2] * last.w[2]) * 180.0 / kPi;
    }
    s.live = s.age_ms >= 0 && s.age_ms < 1000;
    return s;
}

}  // namespace stab
