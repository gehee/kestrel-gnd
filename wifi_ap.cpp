#include "wifi_ap.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

static const char* kScript = "/usr/sbin/fpvos-wifi";
static const char* kIdent  = "/etc/fpvos/wifi-ap";     // SSID=, PASSPHRASE=
static const char* kState  = "/run/fpvos-wifi/state";  // "channel N" while on

static std::mutex s_run;                 // one start/stop at a time, in order
static std::atomic<int> s_busy{0};       // starts/stops not yet finished
static std::atomic<bool> s_want{false};  // what the menu last asked for

bool wifi_ap_available() { return access(kScript, X_OK) == 0; }

bool wifi_ap_on() {
    if (s_busy.load() > 0) return s_want;
    // fpvos-wifi writes the state file while the access point is up; /run is
    // cleared at boot, so it is off after every boot.
    std::string state;
    { std::ifstream f(kState); std::getline(f, state); }
    return !state.empty();
}

void wifi_ap_set(bool on) {
    if (!wifi_ap_available()) return;
    s_want = on;
    s_busy++;
    std::thread([on] {
        std::lock_guard<std::mutex> lk(s_run);
        // Toggled again while waiting: only the latest request matters.
        if (s_want.load() == on) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "%s %s >>/var/log/fpvos-wifi.log 2>&1",
                     kScript, on ? "start" : "stop");
            if (system(cmd) != 0)
                printf("wifi-ap: '%s %s' failed, see /var/log/fpvos-wifi.log\n",
                       kScript, on ? "start" : "stop");
        }
        s_busy--;
    }).detach();
}

static std::string read_key(const char* path, const char* key) {
    std::ifstream f(path);
    std::string line, k = std::string(key) + "=";
    while (std::getline(f, line))
        if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
    return "";
}

std::string wifi_ap_describe() {
    if (!wifi_ap_available()) return "This system has no WiFi access point support.";
    if (s_busy.load() > 0) return s_want ? "Starting the access point..." : "Stopping...";

    std::string ssid = read_key(kIdent, "SSID"), pass = read_key(kIdent, "PASSPHRASE");
    std::string state;
    { std::ifstream f(kState); std::getline(f, state); }

    if (!state.empty())
        return "Network " + ssid + "   Password " + pass + "   (" + state +
               "). Watch the video at http://192.168.4.1";
    if (s_want)
        return "Could not start - see /var/log/fpvos-wifi.log.";
    return "Wifi access point for spectators";
}
