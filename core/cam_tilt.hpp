#ifndef CAM_TILT_HPP
#define CAM_TILT_HPP

#include <cmath>
#include <cstdint>

// How far up the air unit's camera is tilted on the airframe, found when the aircraft is known to be
// level: the moment it goes from disarmed to armed. While it sits disarmed and still, the camera's
// pitch against gravity is noted (smoothed); when it is armed, that is the tilt - the aircraft is
// level, so the camera's pitch is the camera's tilt. The tilt then holds through the flight and is
// saved for the next one. Nothing else changes it: carried about disarmed, or held tilted, it moves
// nothing.
class CamTilt {
public:
    void set(double deg) { deg_ = deg; }             // from the saved setting
    double deg() const { return deg_; }

    // The camera's pitch against the horizon (degrees, nose up), read while disarmed and still.
    void rest(double pitch_deg, uint64_t now_us) {
        pending_ = have_ ? pending_ + 0.2 * (pitch_deg - pending_) : pitch_deg;
        have_ = true;
        at_us_ = now_us;
    }

    // The aircraft has just been armed. True if that moved the tilt by a degree or more (to save it).
    // What was read more than 3 s ago is not trusted: it was moved since.
    bool armed(uint64_t now_us) {
        const bool fresh = have_ && now_us - at_us_ <= 3000000ULL;
        have_ = false;
        if (!fresh) return false;
        const bool changed = std::fabs(pending_ - deg_) >= 1.0;
        deg_ = pending_;
        return changed;
    }

private:
    double deg_ = 0.0, pending_ = 0.0;
    bool have_ = false;
    uint64_t at_us_ = 0;
};

#endif
