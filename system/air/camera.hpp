#pragma once
// The air unit's camera: its settings as the air unit reports them, the ones
// the menu sets, and its video mode.
#include <cstdint>
#include <cstdio>

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

// The choices the menus offer for its settings, by their value on the wire -
// the VRX Pro's OSD and the native Ascent's menu both offer these.
// AR8030 camera menu option tables. EV, the mode list and the anti-flicker
// choices come from the stock GlassesUI; contrast is 0-15 because the config
// frame packs it into a nibble. Saturation/sharpness ranges are NOT confirmed -
// the camera acks every setting, so watch the log for a non-zero status.
static const int   kEvSteps[]      = { -10, -7, -3, 0, 3, 7, 10 };
static const char* kEvLabels[]     = { "-1.0", "-0.7", "-0.3", "0", "+0.3", "+0.7", "+1.0" };
static const int   kEvCount        = 7;
// Scene. Captured off the wire from stock (cmd 0x01): Race = 0, Standard = 2.
// We used to send the menu index (0 = Standard, 1 = Race), i.e. both values
// wrong. The gap at 1 is presumably a scene this unit does not offer.
static const char* kSceneLabels[]  = { "Race", "Standard" };
static const int   kSceneVals[]    = { 0, 2 };
static const int   kSceneCount     = 2;
// 3D DNR, sky cmd 0x1B. The stock goggle sends Off/Low/Mid/High/Auto as
// 1/2/3/4/5, but the air unit - the stock app and kestrel-air alike - takes
// 0 off, 1 its tuning bin's own noise reduction, 2/3/4 low/mid/high, and
// anything else as 1: stock's Off (1) left it on. Off is 0 here, which both
// turn off; Auto stays 5, the bin's.
static const char* kDnrLabels[]    = { "Off", "Low", "Mid", "High", "Auto" };
static const int   kDnrVals[]      = { 0, 2, 3, 4, 5 };
static const int   kDnrCount       = 5;
// Anti-Flicker (sky cmd 0x1F) was here but is gone: stock's own Camera menu
// has no such row at all - confirmed against a photo of the stock UI - so it
// was never a real setting to expose, whatever encoding the command itself
// takes on the wire. See CMD_SET_ANTI_FLICKER in ar8030_sky.hpp.
// White balance. The wire carries the colour temperature in Kelvin (cmd 0x05),
// and stock offers Auto plus 4000K..7000K in 100K steps - 32 entries. The old
// five-value table was a guess and did not match any of stock's steps except
// 4000K. fact_env.json stores the *index*, which is why 7000K reads as 31.
// After them Lock (kWbLock, -1 on the wire): the air unit holds the gains it
// has now - offered only by one whose info says wb_lock (the P1), as the
// kelvin presets only by one that says wb or says nothing (stock, the Lite).
static const int   kWbCount        = 32;
static const int   kWbLock         = 32;
static inline int  wb_kelvin(int i) { return i == kWbLock ? -1 : i <= 0 ? 0 : 3900 + i * 100; }
inline const char* wb_label(int i) {
    static char buf[8];
    if (i == kWbLock) return "Lock";
    if (i <= 0) return "Auto";
    snprintf(buf, sizeof(buf), "%dK", wb_kelvin(i));
    return buf;
}
// Camera rotation, stock's Camera > Rotate row (sky cmd 0x06): the wire
// carries the row's index, 0 or 1 (180 degrees).
static const char* kRotateLabels[] = { "0", "180" };
// Saturation, sharpness and contrast are Auto plus 1..10 on stock, and 0 is
// what Auto puts on the wire - the same scheme white balance uses (0 = Auto,
// otherwise a real value). Rendering 0 as "0" made Auto look like "minimum".
// Contrast used to allow 15: that is the width of the nibble the config body
// packs it into, not stock's range.
static const int   kCamSatMax      = 10;
static const int   kCamSharpMax    = 10;
static const int   kCamContrastMax = 10;
// 0 -> "Auto", anything else the number itself.
inline const char* cam_auto_label(int v) {
    static char buf[8];
    if (v <= 0) return "Auto";
    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}
class Camera : public Part<CameraState> {
public:
    virtual Ticket set(CameraField f, int value) = 0;
    virtual Ticket set_mode(int index) = 0;   // VIDEO > Mode: by its index in the list
};

}  // namespace air
