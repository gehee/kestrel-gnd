#pragma once
// DISPLAY > Refresh Sync NUDGE's step (DisplayLock::run), apart from the lock so
// it can be tested without a display (tools/test/display_lock_test.cpp).

#include <cmath>

namespace display {

// The way NUDGE may move the vblank: frames a line shorter, longer, or either.
enum class Way { Shorter = -1, Either = 0, Longer = 1 };

// One way only needs a drift to bring the vblank back the other way: this
// much between the camera's frame and the refresh's (a whole multiple of it).
// The Lite+'s 60.03 fps on 60.000 Hz is some 530 ppm; the BETAFPV P1 is
// within ~30 ppm of it, minutes to bring back a few ms.
constexpr double kOneWayPpm = 100;

struct LineStep {
    int lines;      // this frame's lines over the mode's: -1, 0 or +1
    bool moving;    // carried to the next frame
    bool arrived;   // reached the target this frame
};

// One frame of NUDGE: a line at a time. One way only (Longer, Shorter) where
// the camera drifts against the screen: frames made longer where the screen
// is the faster, shorter where it is the slower, and the drift brings the
// vblank back the other way, so it never has to move against it. A vblank a
// little on the drift's side waits for the drift; one far the other way goes
// round, up to a refresh, the one way lines can move it. Either: no drift to
// wait for - the nearer way, at most half a refresh, never round.
// e: the target less where the vblank is, us, wrapped into [-P/2, P/2)
// (> 0: the vblank early); P the refresh, line one line's time; moving as the
// last step left it.
inline LineStep line_step(double e, double P, double line, Way way, bool moving) {
    const int s = way == Way::Shorter ? -1 : way == Way::Longer ? 1 : e < 0 ? -1 : 1;
    double x = s * e;                 // how far to go, the way lines move it
    if (way != Way::Either && x < -P / 4) x += P;
    LineStep r{0, moving, false};
    if (!r.moving && x > 3 * line) r.moving = true;
    if (r.moving && x < 0.5 * line) { r.moving = false; r.arrived = true; }
    r.lines = r.moving && x > 0 ? s : 0;
    return r;
}

// The way for a camera frame of Pc against a refresh of Pm: Pc as measured
// from this camera's capture stamps, 0 where it is not (a stored period may
// be another air unit's). One way only for a drift of kOneWayPpm or more.
inline Way nudge_way(double Pc, double Pm) {
    if (Pc <= 0 || Pm <= 0) return Way::Either;
    const double k = std::fmax(1.0, std::round(Pc / Pm));
    const double drift = (k * Pm - Pc) / Pc;
    if (std::fabs(drift) < kOneWayPpm * 1e-6) return Way::Either;
    return drift > 0 ? Way::Shorter : Way::Longer;
}

}  // namespace display
