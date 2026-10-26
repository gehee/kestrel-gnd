// Caddx VRX Pro (RK3568).
#include "bsp/bsp.hpp"

namespace bsp {

// The 7 front buttons are a resistor ladder on SARADC channel 6 that stock's
// ar_ldy_gnd polls through /sys/bus/iio - they are not on the input layer.
// The table is stock's, recovered from ar_ldy_gnd (.data:0x30b148, entries
// of {int id; int mv; const char *name}) with its +-79 mV match window and
// its 134 mV "any key down" floor. Verified against a unit: every button
// measured within +36 mV of its stock value.
//
//   button   stock mV   measured mV
//   rec         390        426
//   up          800        829
//   left       1000       1026
//   right      1200       1230
//   ok         1400       1427
//   down       1600       1628
//   esc        1780       1800
static const AdcKey kPanel[] = {
    { kKeyRec,    390 }, { kKeyUp,    800 }, { kKeyLeft,  1000 },
    { kKeyRight, 1200 }, { kKeyOk,   1400 }, { kKeyDown,  1600 },
    { kKeyEsc,   1780 },
};

static const Board kVrxPro = {
    "vrx_pro",
    // 10-bit SARADC against a 1.8 V reference, scaled as stock does.
    { { "/sys/bus/iio/devices/iio:device0/in_voltage6_raw", 1800, 1023 },
      134, 79, kPanel, sizeof(kPanel) / sizeof(kPanel[0]) },
    // BIND sits on SARADC channel 0, the device tree's "adc-keys" node (keys
    // labelled volume up/down/menu/back, a stock RK3568 node Caddx reused).
    // It idles high (raw 1023) and a press pulls it to about 18 mV; the tree
    // puts the neighbouring key at 297.5 mV, so 150 mV clears both.
    { { "/sys/bus/iio/devices/iio:device0/in_voltage0_raw", 1800, 1023 }, 150 },
    // The supply is on SARADC channel 5, a bare IIO attribute with no
    // consumer node (so /sys/class/power_supply is empty). Recovered from
    // ar_ldy_gnd (read at 0xa7ee8, scaled at 0x98678): pin mV = raw * 1800 /
    // 1024, mean of 10 samples, * 16 - 300. Full scale 28.5 V covers 1S-6S.
    { { "/sys/bus/iio/devices/iio:device0/in_voltage5_raw", 1800, 1024 }, 16, 300, 10 },
    "/sys/class/thermal/thermal_zone0/temp",
    { "/lib/firmware/open8030-vrxpro-usb.img" },
};

const Board &vrx_pro() { return kVrxPro; }

}  // namespace bsp
