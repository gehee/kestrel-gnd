#pragma once
// What this box is made of, in software: the one place that picks each
// backend - the display and what puts the pictures on it, the HUD's layer,
// the decoders, the screen's encoder and compositor - and says what the box
// can do. A file per box (platform/vrx_pro.cpp, platform/pc.cpp), the one
// KESTREL_BOARD names compiled in, as bsp/ is; the rest of kestrel-gnd gets
// what it makes as interfaces (display::Display, display::HudLayer,
// display::Presenter, Vdec, hal::ScreenEncoder...) and never names a backend.
#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <string>

#include "base/settings.hpp"
#include "base/video_codec.hpp"
#include "display/display.hpp"
#include "display/hud_layer.hpp"
#include "display/video_listener.hpp"
#include "hal/screen_out.hpp"
#include "hal/strip_joiner.hpp"
#include "hal/vdec.hpp"
#include "render/canvas.hpp"

namespace platform {

const char* name();                  // the box, as KESTREL_BOARD names it

// ---- what it can do -------------------------------------------------------------
// The build's features (cmake/features.cmake) and what the unit has installed,
// worked out once at startup and logged in one line; the menu shows only the
// rows these allow, instead of each feature checking for itself.
struct Caps {
    bool stream_decode = false;   // per-slice decode, early tops, halves
    bool phase_lock = false;      // Refresh Sync: the display controller's frame length
    bool screen_enc = false;      // screen recording and the phone view's encoder
    bool wifi_ap = false;         // the WiFi access point
};
const Caps& caps();                  // the first call works them out: early in main(), before threads

// ---- the display ----------------------------------------------------------------
// How main() sets the screen up: the mode asked for (0s: the screen's own, or
// Screen Mode Auto), the planes' order, VRR, the screen's ID (a different one
// plugged in later restarts kestrel-gnd), and Auto following the camera with
// the rate and lines last seen.
struct DisplaySetup {
    uint16_t width = 0, height = 0;
    uint32_t refresh = 0;
    int video_zpos = 0, osd_zpos = 0;
    bool vrr = true;
    std::string screen_id;
    bool follow_camera = false;
    // Refresh Sync's settings (display::RefreshSync).
    int sync_mode = 0, sync_slice = 1, sync_margin_us = 0;
};
std::shared_ptr<display::Display> make_display(Settings& settings, const DisplaySetup& setup);
// What the HUD draws with, on the display's device (GLES on the VRX Pro and
// a PC); false (logged) if it cannot be had - the HUD is then not made.
bool init_graphics(display::Display& display, bool msaa);
// The canvas the HUD draws with (render/canvas.hpp), for what init_graphics() set up.
std::unique_ptr<render::Canvas> make_canvas();
// The HUD's layer on the display, for what init_graphics() set up.
std::unique_ptr<display::HudLayer> make_hud_layer(display::Display& display);
// What puts the decoded pictures on the display, on a thread of its own: a
// decoder's sink. on_frame_size: a new picture size (the recorders').
std::shared_ptr<display::Presenter> make_presenter(std::shared_ptr<display::Display> display, RenderMode mode,
                                                   Settings& settings,
                                                   std::function<void(int, int)> on_frame_size,
                                                   std::shared_ptr<display::VideoListener> listener,
                                                   std::atomic<bool>* stop, bool console_stats);

// ---- the decoders ---------------------------------------------------------------
// The live decoder: by --decoder-name / decoder_name, the box's hardware one
// for its name (rkmpp on the VRX Pro) and ffmpeg's under any other.
std::shared_ptr<Vdec> make_decoder(VideoCodec codec, const std::string& name, std::shared_ptr<hal::FrameSink> sink,
                                   std::shared_ptr<display::Display> display, std::atomic<bool>* stop);
// Whether the decoder is the box's hardware one for that name (the menu says so).
bool hardware_decoder(const std::string& name);
// The menu's background clip: hardware, no presenter, each picture to `sink`
// as a DMA-BUF. False / nullptr where the box has none; the menu then shows
// its still background.
bool has_background_decoder();
std::unique_ptr<Vdec> make_background_decoder(VideoCodec codec, std::shared_ptr<hal::Scanout> scanout,
                                              std::atomic<bool>* stop,
                                              std::function<void(std::shared_ptr<DecodedUnit>)> sink);
// Strips (kestrel-air's, radio/strip_video): what joins the strips' decoded
// pictures into whole ones for `out`; a strip's decoder is a background one.
// False / nullptr where the box cannot: the air unit is then never asked for
// strips.
bool has_strip_joiner();
std::unique_ptr<hal::StripJoiner> make_strip_joiner(std::shared_ptr<hal::FrameSink> out,
                                                    std::shared_ptr<hal::Scanout> scanout);

// ---- the screen as a stream -----------------------------------------------------
// Screen recording and the phone view (hal/screen_out.hpp): where the box has
// none, stand-ins that refuse init() and the recorder reports it unavailable.
bool screen_compositor_available();
std::unique_ptr<hal::ScreenEncoder> make_screen_encoder();
std::unique_ptr<hal::ScreenCompositor> make_screen_compositor();

}  // namespace platform
