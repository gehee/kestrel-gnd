// ClockLock (display/clock_lock.hpp) against a simulated camera and screen: the
// camera's pictures arrive with jitter at their own rate, the screen's slack
// moves 1 us a second per ppm the trim is off the camera's rate.
#include "display/clock_lock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {
const double kTarget = display::ClockLock::Config{}.target_slack_us;
int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Result {
    double final_slack, final_trim, max_step, settle_s, trim_spread, settled_step;
    int crossings;       // pictures past the deadline (a repeated refresh each)
    int crossings_late;  // the same once settled
};

// base: the trim that matches the camera (ppm); start: the first slack (us).
Result run(double base, double start_slack, double start_trim, double seconds, double jitter_us,
           unsigned seed = 1) {
    const double P = 16667;
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, jitter_us);
    display::ClockLock lock(start_trim);
    double slack = start_slack, trim = start_trim, prev = trim, max_step = 0;
    Result r{0, 0, 0, -1, 0, 0, 0, 0};
    std::vector<double> late_trims;
    for (int s = 0; s < int(seconds); ++s) {
        std::vector<double> xs;
        for (int k = 0; k < 60; ++k) {
            slack += (trim - base) / 60.0;  // 1 us/s per ppm
            slack = std::fmod(slack + P, P);
            double x = slack + noise(rng);
            if (x < 0 || x >= P) {
                ++r.crossings;
                if (r.settle_s >= 0) ++r.crossings_late;
            }
            xs.push_back(std::fmod(x + P, P));
        }
        std::nth_element(xs.begin(), xs.begin() + 30, xs.end());
        trim = lock.update(s, xs[30], P);
        max_step = std::max(max_step, std::fabs(trim - prev));
        if (r.settle_s >= 0 && s > r.settle_s + 60) r.settled_step = std::max(r.settled_step, std::fabs(trim - prev));
        prev = trim;
        if (r.settle_s < 0 && std::fabs(slack - kTarget) < 300 && std::fabs(trim - base) < 5) r.settle_s = s;
        if (r.settle_s >= 0) late_trims.push_back(trim);
    }
    r.final_slack = slack;
    r.final_trim = trim;
    r.max_step = max_step;
    if (!late_trims.empty()) {
        auto [lo, hi] = std::minmax_element(late_trims.begin(), late_trims.end());
        r.trim_spread = *hi - *lo;
    }
    return r;
}

void report(const char *name, const Result &r) {
    std::printf("%-34s settle %5.0f s  slack %6.0f us  trim %7.1f ppm  step %.2f (settled %.2f)  spread %.1f  crossings %d (%d settled)\n",
                name, r.settle_s, r.final_slack, r.final_trim, r.max_step, r.settled_step, r.trim_spread, r.crossings, r.crossings_late);
}
}  // namespace

int main() {
    // First run: no trim learnt, the camera 400 ppm slow, slack half a frame off.
    auto a = run(400, 12000, 0, 1500, 500);
    report("first run, camera 400 ppm slow", a);
    CHECK(a.settle_s >= 0 && a.settle_s < 900, "settles within 15 min (%.0f)", a.settle_s);
    CHECK(std::fabs(a.final_slack - kTarget) < 400, "slack at the target (%.0f)", a.final_slack);
    CHECK(std::fabs(a.final_trim - 400) < 5, "trim at the camera's rate (%.1f)", a.final_trim);
    CHECK(a.max_step <= 4.0001, "never faster than the PCR follows (%.3f)", a.max_step);
    CHECK(a.crossings_late == 0, "no picture past the deadline once settled (%d)", a.crossings_late);
    CHECK(a.trim_spread < 30, "trim quiet once settled (%.1f)", a.trim_spread);
    CHECK(a.settled_step <= 1.0001, "a minute after settling: no faster than 1 ppm/s (%.3f)", a.settled_step);

    // Trim learnt before: only the phase to bring in, from either side.
    for (double start : {1500.0, 9000.0, 15500.0}) {
        auto b = run(400, start, 400, 600, 500, unsigned(start));
        char name[64];
        std::snprintf(name, sizeof name, "learnt trim, slack from %.0f", start);
        report(name, b);
        CHECK(b.settle_s >= 0 && b.settle_s < 300, "settles within 5 min (%.0f)", b.settle_s);
        CHECK(b.crossings_late == 0, "none past the deadline once settled (%d)", b.crossings_late);
        CHECK(b.max_step <= 4.0001, "slew kept (%.3f)", b.max_step);
    }

    // A camera faster than the screen, and heavier jitter.
    auto c = run(-250, 7000, 0, 1500, 900, 7);
    report("camera 250 ppm fast, 900 us jitter", c);
    CHECK(c.settle_s >= 0, "settles");
    CHECK(std::fabs(c.final_trim + 250) < 8, "trim at the camera's rate (%.1f)", c.final_trim);

    // An air unit seen before, 520 ppm from the last: its rate expected, the
    // trim slewed there (never a step: a PCR re-lock is a black screen).
    {
        const double P = 16667, base = 160;
        display::ClockLock k(-360);
        k.expect_rate(base);
        CHECK(std::fabs(k.trim_ppm() + 360) < 1e-9 && k.estimated(), "expect_rate does not move the trim at once");
        double slack = 2000, trim = k.trim_ppm(), prev = trim, max_step = 0, worst = 0;
        int settle = -1;
        for (int s = 0; s < 600; ++s) {
            slack = std::fmod(slack + (trim - base) + P, P);
            trim = k.update(s, slack, P);
            max_step = std::max(max_step, std::fabs(trim - prev));
            prev = trim;
            if (settle < 0 && std::fabs(slack - kTarget) < 300 && std::fabs(trim - base) < 5) settle = s;
            if (settle >= 0) worst = std::max(worst, std::fabs(slack - kTarget));
        }
        std::printf("%-34s settle %5d s  trim %7.1f ppm  step %.2f  worst after %.0f us\n", "known unit 520 ppm away", settle, trim, max_step, worst);
        CHECK(settle >= 0 && settle < 300, "rate and phase in within 5 min (%d)", settle);
        CHECK(std::fabs(trim - base) < 5, "trim at its rate (%.1f)", trim);
        CHECK(max_step <= 4.0001, "slew kept (%.3f)", max_step);
    }

    // A gap in the pictures restarts the estimate without a jump.
    display::ClockLock lock(100);
    double t = lock.update(0, kTarget, 16667);
    t = lock.update(10, kTarget, 16667);
    CHECK(std::fabs(t - 100) <= 4.0001, "a gap moves the trim at most one step (%.2f)", t);

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("clock_lock: all checks passed\n");
    return failures ? 1 : 0;
}
