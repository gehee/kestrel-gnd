#pragma once
// Board support: the facts of one box, and no logic. Each box kestrel-gnd
// runs on has one Board, filled in its own file (bsp/vrx_pro.cpp, ...); the
// build picks the box (KESTREL_BOARD) and compiles only its file in.
//
// A path left empty means the box has no such thing; code reading it treats
// that as "not fitted", exactly as it treats a path it can't open.

#include <cstddef>

namespace bsp {

// The front panel's keys, in the order VrxButtons numbers them.
enum PanelKey : int { kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyOk, kKeyRec, kKeyEsc };

// A raw IIO ADC channel and how its counts become millivolts at the pin:
// mV = raw * ref_mv / full_raw.
struct AdcChannel {
    const char *path;
    int ref_mv;
    int full_raw;
};

// Several keys on one resistor ladder: each key pulls the channel to its own
// voltage; nothing below floor_mv is a key, and a reading counts as a key
// within tolerance_mv of its centre.
struct AdcKey { PanelKey key; int mv; };
struct AdcLadder {
    AdcChannel adc;
    int floor_mv;
    int tolerance_mv;
    const AdcKey *keys;
    size_t n_keys;
};

// One key alone on a channel that idles high; a press pulls it below
// press_max_mv.
struct AdcButton {
    AdcChannel adc;
    int press_max_mv;
};

// The box's own supply voltage behind a divider:
// supply_mV = mean(pin_mV over `average` samples) * divider - offset_mv.
struct SupplyAdc {
    AdcChannel adc;
    int divider;
    int offset_mv;
    int average;
};

struct Radio {
    // The Open8030 radio firmware this box uploads when Open8030 is chosen;
    // its presence on the unit is what makes Open8030 selectable.
    const char *open8030_fw;
};

struct Board {
    const char *name;
    AdcLadder   buttons;        // front panel
    AdcButton   bind;           // the bind key
    SupplyAdc   supply;         // battery / input voltage
    const char *thermal_zone;   // the SoC's temperature, millidegrees
    Radio       radio;
};

// Counts on `adc` to millivolts at the pin.
inline int adc_mv(const AdcChannel &adc, int raw) {
    return (int)((long)raw * adc.ref_mv / adc.full_raw);
}

// The supply voltage from the mean pin voltage over `average` samples.
inline int supply_mv(const SupplyAdc &s, int avg_pin_mv) {
    return avg_pin_mv * s.divider - s.offset_mv;
}

// Each box's facts, one per bsp/<box>.cpp.
const Board &vrx_pro();
const Board &pc();

// The box this build is for (KESTREL_BOARD).
const Board &board();

}  // namespace bsp
