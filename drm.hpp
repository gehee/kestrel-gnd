#ifndef DRM_DEVICE_H  // Check if DRM_DEVICE_H is not defined
#define DRM_DEVICE_H  // Define DRM_DEVICE_H

#include <atomic>
#include <string>
#include <vector>
#include <deque>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#include "screen_tap.hpp"

extern "C" {
    #include "drm/drm.h"
    #include <gbm.h>
    #include <EGL/egl.h>
    #include <EGL/eglext.h>
    #include <GLES2/gl2.h>
    #include <GLES2/gl2ext.h>

extern bool g_disable_gl;
}

struct FlipStats {
    uint64_t recv_ts;
    uint64_t dec_start_ts;
    uint64_t dec_end_ts;
    uint64_t submission_start_ts;
    uint32_t tx_age_us;
    uint32_t fb_id;
};

struct CompletedStats {
    uint64_t total_latency_us;
    uint64_t display_latency_us;
    uint64_t submission_latency_us;
    uint64_t completion_ts;
    bool available;
};

// Render cadence: measured from hardware flip-completion timestamps, so it captures
// what the eye sees regardless of whether a hiccup came from the link, decode, or
// display. stutters is cumulative; p99/max are over the rolling gap window (~2s @120fps).
struct RenderCadence {
    float p99_ms;
    float max_ms;
    uint32_t stutters;
};

class DrmDevice {
public:
    // One captured frame: the buffer it went into, the fence that signals when
    // the CRTC has finished writing it, and when it was committed.
    struct WbShot { int idx; int fence; uint64_t us; };
private:

private:
    struct modeset_output *output_list_original = nullptr;

    int video_zpos = 0;
    pthread_mutex_t video_mutex;
    pthread_cond_t video_cond;

    int osd_zpos = 0;
    pthread_mutex_t osd_mutex;
    // true = stock's blocking atomic commit (no completion event); false = the
    // original non-blocking commit + PAGE_FLIP_EVENT. See page_flip().
    bool blocking_flip = false;
    uint32_t wb_connector_id = 0;   // 0 = none found
    std::vector<uint32_t> wb_fbs_;      // one framebuffer per encoder buffer
    std::vector<uint32_t> wb_handles_;  // their imported GEM handles, closed at deinit
    // The writeback connector's property ids, looked up once in writeback_init.
    uint32_t wb_prop_crtc_ = 0, wb_prop_fb_ = 0, wb_prop_fence_ = 0;
    std::mutex wb_mu_;
    std::condition_variable wb_cv_;
    std::deque<int>    wb_free_;         // buffers the display may write
    std::deque<WbShot> wb_ready_;        // written (or being written), to encode
    std::atomic<bool>     wb_on_{false};
    // Add a capture to a commit being built, if a buffer is free: returns its
    // index, or -1 when capturing is off or the encoder has every buffer.
    int  wb_attach(drmModeAtomicReq* req, int32_t* fence);
    // After the commit: queue the capture for the encoder, or take the buffer
    // back if the commit failed.
    void wb_committed(int idx, int32_t fence, bool ok);
    uint32_t pending_osd_fb_id = 0;   // rendered, waiting for a commit to carry it
    uint32_t current_osd_fb_id = 0;   // in the last commit made
    // The OSD fb the last in-flight video flip carried, and the one the display
    // is showing. A buffer in either (or pending) must not be drawn into again.
    uint32_t inflight_osd_fb_id = 0;
    uint32_t onscreen_osd_fb_id = 0;
    // Which OSD frame each of those holds: a new one per set_osd_fb, since the
    // fbs themselves are reused. For the screen recorder.
    uint64_t osd_gen_ = 0, pending_osd_gen_ = 0, current_osd_gen_ = 0;
    // The OSD fbs' DMA-BUFs and pitches, for the screen recorder (osd.cpp
    // registers each one as it makes it). Under osd_mutex.
    std::map<uint32_t, std::pair<int, uint32_t>> osd_fb_dmabuf_;
    // The picture the next page_flip shows, for the screen tap (renderer
    // thread only).
    std::shared_ptr<DecodedUnit> flip_picture_;
    std::atomic<uint64_t> last_video_flip_us{0};   // last video flip submitted
    uint64_t last_pageflip_ts = 0;

    // GBM/EGL state
    struct gbm_device *gbm_dev = nullptr;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    
    pthread_t event_thread;
    std::atomic<bool> event_thread_exit{false};
    std::atomic<bool> flip_pending{false};  // Atomic to prevent races with event thread
    uint64_t flip_submit_us = 0;            // watchdog: when the pending flip was submitted

    pthread_mutex_t stats_mutex;
    pthread_mutex_t flip_mutex;
    pthread_cond_t flip_cond;
    FlipStats pending_stats;
    CompletedStats latest_stats = {0};
    uint64_t last_flip_submit_ts = 0;
    bool enable_vrr = true;

    // Display hotplug: the event thread probes the connector every ~2s and, when a
    // display (re)appears, redoes the full modeset. Dedicated atomic request so the
    // hotplug commit never races the OSD thread's reuse of osd_request.
    bool display_connected = false;
    uint64_t last_hotplug_check_us = 0;
    drmModeAtomicReq *hotplug_request = nullptr;
    void check_display_hotplug();

    // Render-cadence tracking (guarded by stats_mutex, written in the flip handler)
    static const int CADENCE_RING = 256;
    uint32_t cadence_gaps_us[CADENCE_RING] = {0};
    int      cadence_idx = 0;
    int      cadence_count = 0;
    uint64_t cadence_last_flip_us = 0;
    double   cadence_gap_ewma_us = 0.0;   // expected frame period (EWMA of normal gaps)
    uint32_t cadence_stutters = 0;        // cumulative gaps > max(2.5x expected, 20ms)

public:
    EGLConfig egl_config;

public:
    void init(uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, int video_zpos, int osd_zpos, bool enable_vrr = true);
    void set_frame_size(uint32_t width, uint32_t height);
    void set_picture_scale(int pct);   // whole picture, both planes, 60..100 (100 = full screen)
    // The scaled picture rectangle inside a base_w x base_h area, centred and
    // even-aligned. ONE implementation for both consumers: the video plane's
    // destination in page_flip, and the OSD's scissor - so the overlay's clip
    // edge lands on exactly the pixels the hardware-scaled video ends on.
    void picture_rect(int base_w, int base_h, int &x, int &y, int &w, int &h) const;
    void cleanup();
    void cond_signal();
    // video
    void perform_modeset_video(int fb_id);
    bool page_flip(int fb_id, uint64_t recv_ts = 0, uint64_t dec_start_ts = 0, uint64_t dec_end_ts = 0, uint64_t sub_start_ts = 0, uint32_t tx_age_us = 0);
    CompletedStats get_latest_stats();
    RenderCadence get_render_cadence();

    // What the screen showed, for the screen recorder (screen_tap.hpp).
    ScreenTap tap;
    // The decoded picture the next page_flip carries - the renderer says so
    // just before, while recording.
    void set_flip_picture(std::shared_ptr<DecodedUnit> du) { flip_picture_ = std::move(du); }
    // Block until the next vblank: its sequence number and time (CLOCK_MONOTONIC us).
    bool wait_vblank(uint32_t* seq, uint64_t* ts_us);
    // The refresh rate of the current mode, in Hz (rounded).
    int refresh_hz() const;
    // An OSD fb's DMA-BUF (owned here) and pitch in bytes.
    void register_osd_fb(uint32_t fb_id, int dmabuf_fd, uint32_t stride);
    int  osd_fb_dmabuf(uint32_t fb_id, uint32_t* stride);

    // osd
    bool wait_for_flip_completion(int timeout_ms = 10);
    void set_osd_fb(uint32_t fb_id);
    // Whether an OSD fb may still be read by the display: pending, in flight
    // or on screen. The OSD releases its buffers by this, not by count.
    bool osd_fb_in_use(uint32_t fb_id);
    void set_blocking_flip(bool on) { blocking_flip = on; }

    // --- DRM writeback capture ------------------------------------------
    // The CRTC composites every plane (video + OSD) and writes the result into
    // a buffer we supply. That is the only way to capture what is actually on
    // screen: glReadPixels sees the GL/OSD layer only, because the video lives
    // on a separate DRM plane the display controller blends at scanout.
    //
    // This connector advertises NV12 among WRITEBACK_PIXEL_FORMATS, so the
    // hardware also does the colour conversion the encoder would otherwise
    // need - and if the supplied buffer is the encoder's own DMA-BUF, the frame
    // is never copied at all.
    // Hardware picture controls on the HDMI connector (brightness, contrast,
    // saturation, hue - all 0..100, default 50). These act on the whole display
    // pipeline, so they lift the video and the OSD together.
    bool set_display_property(const char* name, uint64_t value);

    bool writeback_available() const { return wb_connector_id != 0; }
    // dmabuf_fds: NV12 targets, the encoder's input buffers, used in turn so
    // the display can write one while the encoder reads another.
    bool writeback_init(int w, int h, const std::vector<int>& dmabuf_fds);
    void writeback_deinit();
    // For the encoder thread: the next captured frame, waiting up to
    // timeout_ms. Hand the buffer back with writeback_release() once encoded.
    bool writeback_next(WbShot& shot, int timeout_ms);
    void writeback_release(int idx);
    // Composite the current screen into a free buffer: a commit of its own,
    // queued without waiting for the vblank. Skipped when the encoder holds
    // every buffer.
    //
    // Deliberately separate from the video's page flips. Carrying the capture
    // on every flip (so the recording got every frame shown) made the flips
    // themselves fail with EBUSY - the pilot's live picture dropping frames -
    // and stalled the VEPU until reboot.
    void writeback_capture();
    static void writeback_wait(int fence);
    // FrontBuffer mode never waits for a vblank - the frame is memcpy'd straight
    // into the scanout buffer and is live as soon as the copy lands. Record the
    // real dec->visible latency here instead of timing a page flip the frame
    // never went through.
    void record_direct_frame(uint64_t recv_ts, uint64_t dec_start_ts,
                             uint64_t dec_end_ts, uint64_t sub_start_ts,
                             uint32_t tx_age_us);
    static void* drm_event_thread_func(void* arg);
    void handle_events();

    // OpenGL OSD helpers
    void init_gl();
    void* get_egl_display() { return egl_display; }
    void* get_egl_context() { return egl_context; }
    struct gbm_device* get_gbm_device() { return gbm_dev; }
    int get_osd_zpos() { return osd_zpos; }
    bool is_flip_pending() { return flip_pending; }
    // Kernel timestamp (CLOCK_MONOTONIC us) of the last vblank on our CRTC.
    bool last_vblank(uint64_t *ts_us, uint32_t *seq = nullptr);
    // One refresh of the current mode, in microseconds.
    double frame_period_us() const {
        return output_list && output_list->mode.clock
            ? (double)output_list->mode.htotal * output_list->mode.vtotal * 1000.0 / output_list->mode.clock : 0;
    }

public:
    int drm_fd = -1;
    struct modeset_output *output_list = nullptr;
    // The ID (utils/screen_id.h) of the screen this was set up for, "" if it
    // had none. A different one plugged in later restarts kestrel-gnd.
    std::string display_id;
    // Video variables
	uint32_t video_frm_width;
	uint32_t video_frm_height;
	int video_fb_x, video_fb_y, video_fb_width, video_fb_height;
	int video_pfd;
    uint64_t video_ts;
    int picture_scale_pct = 100;  // whole-picture scale; applied to both planes at commit

private:
    // What the video plane is showing and where, as of the last page_flip.
    // When frames stop, the last one stays on the plane in that rectangle,
    // and only a new frame would move it - so a Picture Size change while
    // the link is down is applied from the OSD side (see set_osd_fb).
    int last_video_fb_id_ = 0;
    int vrect_x_ = 0, vrect_y_ = 0, vrect_w_ = 0, vrect_h_ = 0;
    void sync_frozen_video_rect();
};

#endif // DRM_DEVICE_H  // End of the header guard