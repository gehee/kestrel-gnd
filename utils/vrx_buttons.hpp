#ifndef VRX_BUTTONS_HPP
#define VRX_BUTTONS_HPP

// The VRX Pro's 7 front buttons (D-pad up/down/left/right + ok, plus esc and
// rec) are NOT on the kernel input layer. The DTB's adc-keys node only declares
// four unrelated keys (volume up/down, menu, back) on SARADC channel 0; the real
// buttons are a resistor ladder on SARADC channel 6 that stock's ar_ldy_gnd
// polls directly through /sys/bus/iio. That is why nothing shows up in
// /dev/input and why InputReader never saw them.
//
// The table below is stock's, recovered from ar_ldy_gnd (.data:0x30b148, entries
// of {int id; int mv; const char *name}) together with its ±79 mV match window
// and its 134 mV "any key down" floor. Verified against this unit: every button
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
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

class VrxButtons {
    public:
        enum Key { VB_NONE = -1, VB_UP, VB_DOWN, VB_LEFT, VB_RIGHT,
                   VB_OK, VB_REC, VB_ESC, VB_COUNT };

        // Default channel is stock's; overridable for boards that differ.
        explicit VrxButtons(const char *path =
                "/sys/bus/iio/devices/iio:device0/in_voltage6_raw")
            : path_(path ? path : "") {}

        bool available() const {
            int fd = open(path_, O_RDONLY);
            if (fd < 0) return false;
            close(fd);
            return true;
        }

        // Poll once. Returns the button that has just been pressed (or is
        // repeating), else VB_NONE. Call every ~20-30 ms.
        Key poll(uint64_t now_ms) {
            int mv = read_mv();
            Key cur = classify(mv);

            // Debounce: a reading only counts once it has been seen twice.
            if (cur != pending_) { pending_ = cur; stable_ = 0; return VB_NONE; }
            if (stable_ < 2) { stable_++; if (stable_ < 2) return VB_NONE; }

            if (cur == VB_NONE) { held_ = VB_NONE; return VB_NONE; }
            if (cur != held_) {                       // fresh press
                held_ = cur;
                held_since_ = now_ms;
                next_repeat_ = now_ms + kRepeatDelayMs;
                return cur;
            }
            // Auto-repeat, directions only - repeating "ok"/"esc"/"rec" would
            // fire menu actions over and over while the button is held.
            if (repeats(cur) && now_ms >= next_repeat_) {
                next_repeat_ = now_ms + kRepeatRateMs;
                return cur;
            }
            return VB_NONE;
        }

        static const char *name(Key b) {
            switch (b) {
                case VB_UP: return "up";     case VB_DOWN:  return "down";
                case VB_LEFT: return "left"; case VB_RIGHT: return "right";
                case VB_OK: return "ok";     case VB_REC:   return "rec";
                case VB_ESC: return "esc";   default:        return "none";
            }
        }

    private:
        static const int kFloorMv     = 134;   // stock: below this, nothing is down
        static const int kToleranceMv = 79;    // stock's match window
        static const int kRepeatDelayMs = 450;
        static const int kRepeatRateMs  = 140;

        static bool repeats(Key b) {
            return b == VB_UP || b == VB_DOWN || b == VB_LEFT || b == VB_RIGHT;
        }

        int read_mv() const {
            int fd = open(path_, O_RDONLY);
            if (fd < 0) return 0;
            char buf[32] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) return 0;
            // 10-bit SARADC against a 1.8 V reference, same scaling as stock.
            return (int)((long)atoi(buf) * 1800 / 1023);
        }

        static Key classify(int mv) {
            if (mv <= kFloorMv) return VB_NONE;
            static const struct { Key b; int mv; } kTab[] = {
                { VB_REC,    390 }, { VB_UP,    800 }, { VB_LEFT,  1000 },
                { VB_RIGHT, 1200 }, { VB_OK,   1400 }, { VB_DOWN,  1600 },
                { VB_ESC,   1780 },
            };
            Key best = VB_NONE; int bestd = kToleranceMv + 1;
            for (size_t i = 0; i < sizeof(kTab) / sizeof(kTab[0]); i++) {
                int d = mv - kTab[i].mv; if (d < 0) d = -d;
                if (d < bestd) { bestd = d; best = kTab[i].b; }
            }
            return bestd <= kToleranceMv ? best : VB_NONE;
        }

        const char *path_;
        Key pending_ = VB_NONE;
        Key held_    = VB_NONE;
        int stable_  = 0;
        uint64_t held_since_ = 0, next_repeat_ = 0;
};

// The BIND button.
//
// It is not on the seven-key ladder above. It sits on SARADC channel 0, the
// one the device tree wires to an "adc-keys" node whose keys are labelled
// "volume up/down/menu/back" - a stock RK3568 node Caddx reused without
// renaming. This goggle has no volume control, so the key the tree calls
// "volume up" is the bind button.
//
// That node does publish a real input device (/dev/input/event4, KEY_VOLUMEUP),
// but kestrel never opens it: InputReader looks for something keyboard-shaped
// and a four-key ADC node does not qualify, so the process runs with no evdev
// descriptors at all. Reading the channel directly is simpler and uses the same
// mechanism as the front panel, which is already proven on this hardware.
//
// Polarity is inverted relative to the seven-key ladder: this channel idles
// HIGH (raw 1023, the tree's keyup threshold) and a press pulls it to ground.
// Measured on this unit: 1023 idle, 10-11 held, about 18 mV against the tree's
// 1.75 mV threshold for that key.
class VrxBindButton {
    public:
        explicit VrxBindButton(const char *path =
                "/sys/bus/iio/devices/iio:device0/in_voltage0_raw")
            : path_(path ? path : "") {}

        bool available() const {
            int fd = open(path_, O_RDONLY);
            if (fd < 0) return false;
            close(fd);
            return true;
        }

        // True exactly once per press. Binding takes 30 seconds, so this never
        // auto-repeats and fires only on the edge: holding the button must not
        // queue a second bind behind the first.
        bool pressed() {
            bool down = (read_mv() < kPressMaxMv);
            // Debounce with the same two-sample rule the front panel uses.
            if (down != pending_) { pending_ = down; stable_ = 0; return false; }
            if (stable_ < 2) { stable_++; if (stable_ < 2) return false; }
            if (down && !held_) { held_ = true; return true; }
            if (!down) held_ = false;
            return false;
        }

    private:
        // The tree puts this key at 1.75 mV and its neighbour ("volume down")
        // at 297.5 mV, so this window clears both that neighbour and the
        // 1800 mV idle level.
        static const int kPressMaxMv = 150;

        int read_mv() const {
            int fd = open(path_, O_RDONLY);
            if (fd < 0) return 1800;          // unreadable reads as "not pressed"
            char buf[32] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) return 1800;
            return (int)((long)atoi(buf) * 1800 / 1023);
        }

        const char *path_;
        bool pending_ = false;
        bool held_    = false;
        int  stable_  = 0;
};

#endif
