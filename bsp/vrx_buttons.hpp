#ifndef VRX_BUTTONS_HPP
#define VRX_BUTTONS_HPP

// Front-panel keys read straight from an ADC channel, for boxes whose keys
// are not on the kernel input layer. On the VRX Pro the 7 front buttons are a
// resistor ladder that stock polls through /sys/bus/iio, which is why nothing
// shows up in /dev/input and InputReader never sees them. The channels, the
// key voltages and the match windows are the box's own and come from its BSP
// (bsp/vrx_pro.cpp holds the VRX Pro's, with where they were recovered from).
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "bsp/bsp.hpp"

class VrxButtons {
    public:
        enum Key { VB_NONE = -1, VB_UP = bsp::kKeyUp, VB_DOWN = bsp::kKeyDown,
                   VB_LEFT = bsp::kKeyLeft, VB_RIGHT = bsp::kKeyRight,
                   VB_OK = bsp::kKeyOk, VB_REC = bsp::kKeyRec, VB_ESC = bsp::kKeyEsc,
                   VB_COUNT };

        explicit VrxButtons(const bsp::AdcLadder &ladder)
            : ladder_(ladder), path_(ladder.adc.path ? ladder.adc.path : "") {}

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
            return bsp::adc_mv(ladder_.adc, atoi(buf));
        }

        Key classify(int mv) const {
            if (mv <= ladder_.floor_mv) return VB_NONE;
            Key best = VB_NONE; int bestd = ladder_.tolerance_mv + 1;
            for (size_t i = 0; i < ladder_.n_keys; i++) {
                int d = mv - ladder_.keys[i].mv; if (d < 0) d = -d;
                if (d < bestd) { bestd = d; best = (Key)ladder_.keys[i].key; }
            }
            return bestd <= ladder_.tolerance_mv ? best : VB_NONE;
        }

        const bsp::AdcLadder &ladder_;
        const char *path_;
        Key pending_ = VB_NONE;
        Key held_    = VB_NONE;
        int stable_  = 0;
        uint64_t held_since_ = 0, next_repeat_ = 0;
};

// The BIND button: one key alone on its own ADC channel, idling high and
// pulled low by a press (the opposite of the ladder above). On the VRX Pro it
// is the device tree's "adc-keys" node, which does publish an input device,
// but InputReader only opens keyboard-shaped devices, so it is read here the
// same way as the front panel.
class VrxBindButton {
    public:
        explicit VrxBindButton(const bsp::AdcButton &button)
            : button_(button), path_(button.adc.path ? button.adc.path : "") {}

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
            bool down = (read_mv() < button_.press_max_mv);
            // Debounce with the same two-sample rule the front panel uses.
            if (down != pending_) { pending_ = down; stable_ = 0; return false; }
            if (stable_ < 2) { stable_++; if (stable_ < 2) return false; }
            if (down && !held_) { held_ = true; return true; }
            if (!down) held_ = false;
            return false;
        }

    private:
        int read_mv() const {
            int fd = open(path_, O_RDONLY);
            if (fd < 0) return button_.adc.ref_mv;   // unreadable reads as "not pressed"
            char buf[32] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) return button_.adc.ref_mv;
            return bsp::adc_mv(button_.adc, atoi(buf));
        }

        const bsp::AdcButton &button_;
        const char *path_;
        bool pending_ = false;
        bool held_    = false;
        int  stable_  = 0;
};

#endif
