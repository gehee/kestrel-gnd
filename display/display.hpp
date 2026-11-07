#pragma once
// The display as the rest of kestrel-gnd sees it: the screen's mode and the
// modes it offers, where the picture sits on it, its flips, and what a screen
// recording reads of it. The platform makes the one there is
// (platform/platform.hpp): display/drm (DrmDevice, KMS) on every box today,
// with the renderer (display/renderer) that puts the decoded pictures on it.
// The HUD's frames reach it through a layer of their own (display/hud_layer.hpp).
#include <cstdint>
#include <memory>
#include <vector>

#include "hal/vdec.hpp"   // hal::Scanout, DecodedUnit

class ScreenTap;

// Render cadence: measured from hardware flip-completion timestamps, so it captures
// what the eye sees regardless of whether a hiccup came from the link, decode, or
// display. stutters is cumulative; p99/max are over the rolling gap window (~2s @120fps).
struct RenderCadence {
    float p99_ms;
    float max_ms;
    uint32_t stutters;
};

// How the pictures reach the screen.
enum RenderMode {
    Disable,     // No rendering (benchmarking only)
    Atomic,      // V-Sync enabled (tear-free)
    FrontBuffer  // No V-Sync (lowest latency, tearing possible)
};

namespace display {

// A mode: its size and refresh, Hz.
struct Mode { int w = 0, h = 0, hz = 0; };

// The gallery shrinks the live picture into a tile on its strip: where the
// video plane goes instead of the picture rectangle, in screen pixels from
// the top left, and it may run off the screen (the strip scrolls it past the
// edge) - the plane is cut at the edge and the picture cut with it, so what
// shows is not squeezed. The HUD hands one with each frame (HudLayer::present),
// so the hole the HUD leaves for the picture and the picture's rectangle
// change together.
struct Tile { bool active = false; int x = 0, y = 0, w = 0, h = 0; };

// A flip's completion time and the gap since the one before.
struct FlipSample { uint64_t t_us; uint32_t gap_us; };

// Refresh Sync (DISPLAY > Refresh Sync, Sync To, Sync Margin): the screen's
// refresh held in step with the pictures, where the display can
// (platform::Caps::phase_lock - elsewhere the values are kept and nothing
// holds). Its settings, and what it sees as of its last pass: for the menu's
// help line and a screen recording's rate.
struct RefreshSync {
    int mode = 0;               // 0 OFF, 1 ON, 2 FREE
    int slice = 1;              // what it times the refresh to: 1 a picture's first slice, 2 its last
    int margin_us = 0;          // the refresh this much later than where the expected wait is least
    float mode_hz = 0;          // the screen mode's refresh
    float camera_fps = 0;       // the camera's measured rate (0: not known yet)
    bool holding = false;       // it holds the screen in step now
    const char* why = nullptr;  // if not, why (its log line)
};

// The HUD frames the video flips picked up, and those replaced before one did
// (profiling: a replaced frame is a step of an animation the screen never shows).
struct HudFrames { uint64_t consumed = 0, superseded = 0; };

class Display : public hal::Scanout {
public:
    // ---- the screen -----------------------------------------------------------
    virtual bool has_output() const = 0;           // a screen is connected and set up
    virtual Mode mode() const = 0;                  // the mode it runs (0s without one)
    // The modes the screen offers, progressive only (interlaced ones do not
    // composite here), as it lists them; read without probing it (a probe
    // re-reads the EDID).
    virtual std::vector<Mode> modes() const = 0;
    virtual int refresh_hz() const = 0;            // the mode's, rounded
    // One refresh, in microseconds: the frame being scanned out, which Refresh
    // Sync may make longer than the mode's.
    virtual double frame_period_us() const = 0;
    // The refresh the screen really runs at, Hz: vblanks counted between
    // flips a second or more apart, by the kernel's times (Refresh Sync's
    // longer frames in it). 0 while not known: no flip for a few seconds.
    virtual float measured_refresh_hz() const = 0;
    // The mode's own refresh, exact: its pixel clock over its total size, not
    // the rounded figure the mode is named by (0: no screen).
    virtual double mode_refresh_hz() const = 0;
    // Blocks until the next vblank: its sequence number and time (CLOCK_MONOTONIC us).
    virtual bool wait_vblank(uint32_t* seq, uint64_t* ts_us) = 0;
    // Hardware picture controls on the screen's connector (brightness,
    // contrast, saturation, hue - 0..100, default 50): the video and the HUD
    // together.
    virtual bool set_display_property(const char* name, uint64_t value) = 0;
    // Refresh Sync changed (FREE wants the fastest mode): Screen Mode Auto
    // chooses again.
    virtual void refollow() = 0;

    // ---- Refresh Sync ------------------------------------------------------------
    virtual RefreshSync refresh_sync() const = 0;
    // Each setting on its own, from any thread; the mode before the display
    // starts too (Screen Mode Auto's first mode depends on it).
    virtual void set_sync_mode(int mode) = 0;
    virtual void set_sync_slice(int slice) = 0;
    virtual void set_sync_margin_us(int us) = 0;

    // ---- the picture ----------------------------------------------------------
    virtual int picture_scale() const = 0;          // Picture Size, % (100: full screen)
    virtual void set_picture_scale(int pct) = 0;    // 60..100, both planes
    // The scaled picture rectangle inside a base_w x base_h area, centred and
    // even-aligned: where the video plane goes, and the HUD's clip - so the
    // HUD's edge lands on exactly the pixels the scaled video ends on.
    virtual void picture_rect(int base_w, int base_h, int& x, int& y, int& w, int& h) const = 0;
    // The part of a tile that is on a sw x sh screen, aligned as the plane
    // needs it; false if none of it is. The HUD leaves exactly this as the
    // hole the picture shows through, so it is the plane's own arithmetic.
    virtual bool tile_on_screen(const Tile& t, int sw, int sh, int& x0, int& y0, int& x1, int& y1) const = 0;
    // The picture the screen is showing (null before the first), for drawing a second copy of it.
    virtual std::shared_ptr<DecodedUnit> shown_picture() = 0;

    // ---- its flips ------------------------------------------------------------
    // Called on the display's thread each time a flip has landed (the HUD
    // starts an animation frame right after one). Before the threads start,
    // and cleared (nullptr) before the caller goes.
    virtual void set_flip_hook(void (*hook)(void*), void* ctx) = 0;
    // Whether a flip is a picture reaching the screen (true with vsync):
    // otherwise its timing says nothing about smoothness.
    virtual bool flips_are_frames() const = 0;
    virtual RenderCadence get_render_cadence() = 0;
    // The flips since since_us, oldest first; and those newer than after_us
    // appended to out (the log is in time order).
    virtual void copy_flip_log(uint64_t since_us, std::vector<FlipSample>& out) = 0;
    virtual void append_flip_log_after(uint64_t after_us, std::vector<FlipSample>& out) = 0;
    virtual HudFrames hud_frames() const = 0;
    // How long HUD frames waited between being handed over and being
    // committed, since the last call: <5 <10 <20 <35 <60 more ms (KESTREL_PROF).
    virtual void take_hud_age_hist(uint32_t out[6]) = 0;

    // ---- a screen recording ---------------------------------------------------
    virtual ScreenTap& tap() = 0;                   // what the screen showed (display/screen_tap.hpp)
    virtual void start_screen_tap() = 0;            // seeded with what is on screen now
    // A HUD buffer's DMA-BUF (owned by the display) and pitch in bytes, -1 if none.
    virtual int osd_fb_dmabuf(uint32_t fb_id, uint32_t* stride) = 0;

    // ---- shutting down ---------------------------------------------------------
    virtual void cond_signal() = 0;                 // wakes whoever waits on a flip, as the app stops
    virtual void cleanup() = 0;                     // the screen let go, at exit
};

// What puts the decoded pictures on the display (display/renderer on KMS): a
// decoder's sink, run on a thread of its own.
class Presenter : public hal::FrameSink {
public:
    virtual void run() = 0;                         // until the stop signal
    // Before the display goes: what of it still reads the display stops (the
    // Refresh Sync thread). Idempotent.
    virtual void stop_display_lock() = 0;
};

}  // namespace display
