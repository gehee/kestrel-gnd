#pragma once
// What kestrel-gnd starts with: its settings, then its command line, which
// overrides some of them for this run and saves some (--help lists it).

#include <climits>
#include <cstdint>
#include <string>

#include "base/settings.hpp"
#include "base/video_codec.hpp"
#include "display/display.hpp"   // RenderMode
#include "dvr/dvr.hpp"            // DvrFormat
#include "radio/ar8030_config.hpp"

struct StartOptions {
    Ar8030Config ar;                     // the radio's (run_radio)
    int ar8030_slot = 0, ar8030_port = 3;
    // The screen: its EDID ID ("": none), its mode (0s: Auto), a mode on trial.
    std::string screen_id;
    uint16_t mode_width = 0, mode_height = 0;
    uint32_t mode_vrefresh = 0;
    std::string screen_mode_trial;
    // DISPLAY > Refresh Sync (the screen's), Sync To, Sync Margin.
    int sync_mode = 0, sync_slice = 1, sync_margin_us = 0;
    RenderMode render_mode = FrontBuffer;
    bool enable_vrr = false;
    int video_zpos = 1, osd_zpos = 2;
    // The OSD: at all, its stats period, the stats on the console, its scale.
    int enable_osd = 1;
    int osd_refresh_frequency_ms = 1000;
    bool console_stats = false;
    float ui_scale = 1.0f;
    bool simulation_mode = false;        // --simulation: no display, no OSD
    // The --dvr recording.
    std::string dvr_filename;
    int dvr_framerate = 0;
    bool dvr_screen = false;
    DvrFormat dvr_format = DvrFormat::FMP4;
    std::string decoder_name;
    VideoCodec codec = VideoCodec::H265;
};

// read_start_options()'s "start the app"; anything else is main()'s exit code.
constexpr int kStart = INT_MIN;

// From the settings (already loaded) and the command line. Also applies the
// settings read at start: the time zone, the SD log, the display lock's
// globals, the frame mode, the low-latency switches.
int read_start_options(int argc, char** argv, Settings& settings, StartOptions& o);
