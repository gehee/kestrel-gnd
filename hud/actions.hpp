#pragma once
// What the menu asks the rest of kestrel-gnd to do. The views emit these;
// hud::dispatch() (hud/dispatch.cpp) carries each one out. They replace the
// numeric command codes the OSD used to send through a callback to a switch
// in main.cpp (the old code is noted on each).

#include <variant>

namespace hud {

namespace action {

// The channel-scan screen opened or closed: poll the scan fast while open. (0x200)
struct ScanScreen { bool open; };
// HUD > Debug Info: the log on the card and the air unit's flight logs. (0x201)
struct DebugInfo { bool on; };
// A camera setting on the VIDEO tab, sent to the air unit. (0x300 + field)
struct CameraSetting { int field; int value; };          // field: Ar8030Source::CamField
// An RF control on the RADIO tab, for our radio. (0x310 + field)
struct RfControl { int field; int value; };              // field: Ar8030Source::RfField
// RADIO > Max Bitrate, kbit/s. (0x30E)
struct MaxBitrate { int kbps; };
// RADIO > Max Bandwidth, MHz (0: no cap). (0x30F)
struct MaxBandwidth { int mhz; };
// RADIO > VRX Radio Firmware: 0 stock, 1 Open8030. (0x309)
struct RadioFirmware { int firmware; };
// RADIO > Radio Frame: 0 standard, 2 short. (0x30A)
struct RadioFrame { int mode; };
// RADIO > Max MCS, -1 for none. (0x320)
struct MaxMcs { int mcs; };
// RADIO > Steady Latency. (0x321)
struct SteadyLatency { bool on; };
// Channel page: AUTO (false) or AUTO HOP (true). (0x322)
struct ChannelHop { bool hop; };
// VIDEO > Mode: the camera's mode by its index in the list. (0x01)
struct VideoMode { int index; };
// Bind: the radio's pairing window, run by its RX thread.
struct Bind {};
// Something AUTO's channel list depends on changed (licence, country, time
// zone): it goes to the air unit again.
struct RegionChanged {};
// A code no handler takes: the old air link's (0x0E, 0x101, 0x104, 0x107).
// Logged as unknown, as they always were.
struct Unknown { int code; int value; };

}  // namespace action

using Action = std::variant<action::ScanScreen, action::DebugInfo, action::CameraSetting,
                            action::RfControl, action::MaxBitrate, action::MaxBandwidth,
                            action::RadioFirmware, action::RadioFrame, action::MaxMcs,
                            action::SteadyLatency, action::ChannelHop, action::VideoMode,
                            action::Bind, action::RegionChanged, action::Unknown>;

// Carries an action out (Ar8030Source, the settings, the SD log). Called on
// the OSD thread; nothing in it waits on the radio.
void dispatch(const Action &a);

}  // namespace hud
