#pragma once
// The air unit's camera: its settings as the air unit reports them, the ones
// the menu sets, and its video mode.
#include <cstdint>

#include "system/part.hpp"

namespace air {

// A camera setting, by the air unit's own number for it (the menu's command
// ids were 0x300 + these).
enum class CameraField { EV = 1, SAT = 2, CONTRAST = 3, SHARPNESS = 4, SCENE = 5, FOCUS = 6, AWB = 7,
                         ANGLE = 11, DNR3D = 13 };

// Its settings as it reports them, each with how many times it has (its _n):
// a reader takes a report once, however many came - the menu follows the
// camera's own values, the newest of them.
struct CameraState {
    uint32_t mode_n = 0;
    int video_mode = -1;           // its mode, by its index in the list
    uint32_t camera_n = 0;         // all its settings at once
    int ev_x10 = 0, sat = 0, contrast = 0, sharp = 0, scene = 0, awb = 0, angle = 0, dnr3d = 0, focus = 0;
    uint32_t dnr_n = 0;            // 3D DNR alone (dnr3d)
    uint32_t angle_n = 0;          // its angle alone (angle)
};

class Camera : public Part<CameraState> {
public:
    virtual Ticket set(CameraField f, int value) = 0;
    virtual Ticket set_mode(int index) = 0;   // VIDEO > Mode: by its index in the list
};

}  // namespace air
