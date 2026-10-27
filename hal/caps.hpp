#pragma once
// What this build, on this unit, can do: the build's features (Kconfig /
// cmake/features.cmake) and what the system has installed. Worked out once at startup and logged in one line;
// the menu shows only the rows these allow, instead of each feature
// checking for itself.

namespace hal {

struct Caps {
    bool stream_decode = false;   // per-slice decode, early tops, halves
    bool phase_lock = false;      // Refresh Sync: the display controller's frame length
    bool screen_enc = false;      // screen recording and the phone view's encoder
    bool wifi_ap = false;         // the WiFi access point
};

// Worked out on the first call (call it early in main(), before threads).
const Caps &caps();

}  // namespace hal
