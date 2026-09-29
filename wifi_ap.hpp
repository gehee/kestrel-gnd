#ifndef WIFI_AP_HPP
#define WIFI_AP_HPP

// The goggle's WiFi access point, as the SYSTEM tab sees it.
//
// fpvOS does the work in /usr/sbin/fpvos-wifi (chip power, driver, channel
// choice, hostapd, DHCP); this only asks it to start or stop, on a thread of
// its own because a start takes a few seconds, and reads back what it wrote
// so the menu can show the network name and password.
//
// It is off after every boot and only ever started from the menu: nothing is
// saved, so the radio is never on unless someone turned it on in this session.
// kestrel leaves it running when it exits, so a restart does not drop a phone
// that is watching.

#include <string>

bool wifi_ap_available();         // the fpvOS script is on this system
bool wifi_ap_on();                // up, or asked to start - what the menu shows
void wifi_ap_set(bool on);        // start or stop, in the background
std::string wifi_ap_describe();   // one line for the menu help

#endif
