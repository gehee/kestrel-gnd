#ifndef WIFI_AP_HPP
#define WIFI_AP_HPP

// The goggle's WiFi access point, as the SYSTEM tab sees it.
//
// fpvOS does the work in /usr/sbin/fpvos-wifi (chip power, driver, channel
// choice, hostapd, DHCP); this only asks it to start or stop, on a thread of
// its own because a start takes a few seconds, and reads back what it wrote
// so the menu can show the network name and password.

#include <string>

bool wifi_ap_available();         // the fpvOS script is on this system
// Whether the SYSTEM row exists at all: shown unless settings has
// wifi_ap_menu: false. While hidden, a saved wifi_ap: true is ignored too, so
// the radio can never be on without a way to turn it off.
bool wifi_ap_menu_enabled();
void wifi_ap_set(bool on);        // start or stop, in the background
std::string wifi_ap_describe();   // one line for the menu help

#endif
