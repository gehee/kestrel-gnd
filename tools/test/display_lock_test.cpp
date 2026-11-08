// Host test of display/lock_step.hpp: DISPLAY > Refresh Sync NUDGE's step, a
// line at a time, against a screen a little faster and a little slower than
// the camera, and one as fast as it - the vblank's place in the refresh
// simulated frame by frame.
//   g++ -std=c++17 -I. -o display_lock_test tools/test/display_lock_test.cpp && ./display_lock_test
#include <cmath>
#include <cstdio>
#include "display/lock_step.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static double wrap(double x, double P) {     // into [-P/2, P/2), as display_lock.cpp's
    x = std::fmod(x, P);
    if (x >= P / 2) x -= P;
    if (x < -P / 2) x += P;
    return x;
}
static double mod(double x, double P) { x = std::fmod(x, P); return x < 0 ? x + P : x; }

struct Run {
    int min_lines = 0, max_lines = 0;   // the frame lengths it used, over the mode's
    double worst_us = 0;                // the largest error once in step, us
    int arrived = 0;                    // frames it reached the target on
    double worst_after_jump_us = 0;     // the largest error after the jump, us
    int back_after_jump = -1;           // frames from the jump to within 2 lines (-1: never)
};

// A 1080p60 mode (1125 lines) against a camera frame of Pc, for n frames,
// starting a third of a refresh off. Each frame the pictures land Pm - Pc
// earlier in the refresh (later, with the screen the faster); the vblank
// moves by the lines the step asked for. At frame jump_at (if any) the
// pictures land jump_us later at once (the link from 40 MHz to 20 MHz).
static Run run(double Pc, display::Way way, int n, int jump_at = -1, double jump_us = 0) {
    const double Pm = 16666.667, line = Pm / 1125;
    double target = Pm / 3, G = 0;
    bool moving = false, in_step = false;
    Run r;
    for (int f = 0; f < n; f++) {
        target = mod(target - (Pm - Pc), Pm);
        if (f == jump_at) target = mod(target + jump_us, Pm);
        const display::LineStep s = display::line_step(wrap(target - G, Pm), Pm, line, way, moving);
        moving = s.moving;
        if (s.arrived) { r.arrived++; in_step = true; }
        if (s.lines < r.min_lines) r.min_lines = s.lines;
        if (s.lines > r.max_lines) r.max_lines = s.lines;
        G = mod(G + s.lines * line, Pm);
        const double err = std::fabs(wrap(target - G, Pm));
        if (in_step && (jump_at < 0 || f < jump_at)) r.worst_us = std::fmax(r.worst_us, err);
        if (jump_at >= 0 && f >= jump_at) {
            r.worst_after_jump_us = std::fmax(r.worst_after_jump_us, err);
            if (r.back_after_jump < 0 && err < 2 * line) r.back_after_jump = f - jump_at;
        }
    }
    return r;
}

// The screen the faster (a 60.000 Hz mode, a 59.988 fps camera): longer frames
// only, as before, and held within a few lines.
static void test_faster_screen() {
    const Run r = run(16670.0, display::Way::Longer, 20000);
    CHECK(r.arrived >= 1);
    CHECK(r.min_lines == 0 && r.max_lines == 1);
    CHECK(r.worst_us < 4 * 16666.667 / 1125);
}

// The screen the slower (60.000 Hz against the Lite+'s 60.03 fps, measured on
// an Ascent VRX Pro): longer frames only never hold it - the vblank goes round
// a whole refresh again and again.
static void test_slower_screen_longer_only() {
    const Run r = run(16658.1, display::Way::Longer, 20000);
    CHECK(r.worst_us > 16666.667 / 4);
}

// ... and shorter frames only hold it, within a few lines, a line short on
// about every other frame.
static void test_slower_screen_shorter() {
    const Run r = run(16658.1, display::Way::Shorter, 20000);
    CHECK(r.arrived >= 1);
    CHECK(r.min_lines == -1 && r.max_lines == 0);
    CHECK(r.worst_us < 4 * 16666.667 / 1125);
}

// A vblank a little on the drift's side waits for the drift; one a little the
// other way goes round, the one way lines can move it.
static void test_step_direction() {
    const double P = 16666.667, line = P / 1125;
    // e is the target less the vblank: > 0, the vblank early. Longer only: a
    // little late waits, early moves it later, far too late goes round.
    CHECK(display::line_step(-10 * line, P, line, display::Way::Longer, false).lines == 0);
    CHECK(display::line_step(10 * line, P, line, display::Way::Longer, false).lines == 1);
    CHECK(display::line_step(-P / 3, P, line, display::Way::Longer, false).lines == 1);
    // Shorter only, the mirror: a little early waits, late moves it earlier,
    // far too early goes round.
    CHECK(display::line_step(10 * line, P, line, display::Way::Shorter, false).lines == 0);
    CHECK(display::line_step(-10 * line, P, line, display::Way::Shorter, false).lines == -1);
    CHECK(display::line_step(P / 3, P, line, display::Way::Shorter, false).lines == -1);
    // Within half a line while moving: there.
    const display::LineStep s = display::line_step(-0.2 * line, P, line, display::Way::Shorter, true);
    CHECK(s.lines == 0 && !s.moving && s.arrived);
}

// Either way: the nearer one, whichever side the vblank is on, never round.
static void test_step_either_way() {
    const double P = 16666.667, line = P / 1125;
    CHECK(display::line_step(10 * line, P, line, display::Way::Either, false).lines == 1);
    CHECK(display::line_step(-10 * line, P, line, display::Way::Either, false).lines == -1);
    CHECK(display::line_step(P / 3, P, line, display::Way::Either, false).lines == 1);
    CHECK(display::line_step(-P / 3, P, line, display::Way::Either, false).lines == -1);
    // Within 3 lines and still: stays. Within half a line while moving: there.
    CHECK(display::line_step(2 * line, P, line, display::Way::Either, false).lines == 0);
    CHECK(display::line_step(-2 * line, P, line, display::Way::Either, false).lines == 0);
    const display::LineStep s = display::line_step(0.2 * line, P, line, display::Way::Either, true);
    CHECK(s.lines == 0 && !s.moving && s.arrived);
}

// The BETAFPV P1 on a 60.000 Hz screen: the camera within ~10 ppm of it, and
// the pictures 3.5 ms later at once. One way only has no drift to bring the
// vblank back: it waits on it, or goes round a whole refresh through the
// pictures (drops for ~15 s, flight logs 2026-11-08). Either way goes the
// 3.5 ms straight back, in some 240 frames, and holds.
static void test_camera_as_fast_as_screen() {
    const double Pc = 16666.5, jump = 3500;
    const Run one = run(Pc, display::Way::Shorter, 20000, 5000, jump);
    CHECK(one.back_after_jump < 0 || one.back_after_jump > 900);
    const Run r = run(Pc, display::Way::Either, 20000, 5000, jump);
    CHECK(r.arrived >= 1);
    CHECK(r.worst_us < 4 * 16666.667 / 1125);
    CHECK(r.worst_after_jump_us < jump + 50);
    CHECK(r.back_after_jump >= 0 && r.back_after_jump < 260);
    // ... and 3.5 ms sooner (20 MHz to 40 MHz) the same, the other way.
    const Run e = run(Pc, display::Way::Either, 20000, 5000, -jump);
    CHECK(e.worst_after_jump_us < jump + 50);
    CHECK(e.back_after_jump >= 0 && e.back_after_jump < 260);
}

// Which way NUDGE may move: one way only where this camera's own period is
// measured and drifts from the refresh by 100 ppm or more; either way
// otherwise (not measured - a stored one may be another air unit's - or too
// little drift to bring the vblank back).
static void test_nudge_way() {
    const double Pm = 16666.667;
    CHECK(display::nudge_way(16657.86, Pm) == display::Way::Shorter);   // Lite+ 60.032 fps
    CHECK(display::nudge_way(16690.0, Pm) == display::Way::Longer);     // 59.92 fps
    CHECK(display::nudge_way(16666.5, Pm) == display::Way::Either);     // P1 ~60.00 fps
    CHECK(display::nudge_way(16666.667 * (1 + 90e-6), Pm) == display::Way::Either);
    CHECK(display::nudge_way(0, Pm) == display::Way::Either);           // not measured
    CHECK(display::nudge_way(9987.3, 1e6 / 50.0) == display::Way::Shorter);  // 100.13 fps, 2 a 50 Hz refresh
}

int main() {
    test_faster_screen();
    test_slower_screen_longer_only();
    test_slower_screen_shorter();
    test_step_direction();
    test_step_either_way();
    test_camera_as_fast_as_screen();
    test_nudge_way();
    std::printf(fails ? "display_lock_test: %d FAILED\n" : "display_lock_test: all passed\n", fails);
    return fails ? 1 : 0;
}
