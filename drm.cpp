#include <ostream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <poll.h>
#include <chrono>
#include <unistd.h>

#include "drm.hpp"
#include "utils/time_util.h"
#include "utils/prof.hpp"
#include "settings.hpp"
#include "utils/scheduling_helper.hpp"

// Set from main() when --no-osd is given: skips all EGL/GLES bring-up.
bool g_disable_gl = false;


void DrmDevice::init(uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, int vzpos, int ozpos, bool vrr) {
    enable_vrr = vrr;
    int ret;
    char card[20]; 
    for (int i = 0; i < 10; i++) {
        snprintf(card, sizeof(card), "/dev/dri/card%d", i);
        ret = modeset_open(&drm_fd, card);
        if(ret==0){
            printf("Using drm device %s\n", card);
            break;
        }
    }
    if (ret < 0) {
        printf("modeset_open(/dev/dri/card[0-9]) =  %d\n", ret);
    }
	assert(drm_fd >= 0);
	output_list = (struct modeset_output *)malloc(sizeof(struct modeset_output));
    output_list_original = output_list;
	ret = modeset_prepare(drm_fd, output_list, mode_width, mode_height, mode_vrefresh);
    // No connected display is not fatal: booting headless (or with a flaky HDMI
    // cable) used to fall through the compiled-out assert below onto an
    // UNINITIALIZED output struct. Wait here and pick the display up when it
    // appears — mode selection, framebuffers and GL are all sized from it.
    if (ret != 0) {
        printf("[drm] no connected display — waiting for one to be plugged in...\n");
        fflush(stdout);
        while (ret != 0) {
            sleep(2);
            ret = modeset_prepare(drm_fd, output_list, mode_width, mode_height, mode_vrefresh);
        }
    }
	assert(!ret);
    printf("modeset_prepare successful. Using connector %u, crtc %u\n", output_list->connector.id, output_list->crtc.id);
    fflush(stdout);
    output_list->allow_vrr = enable_vrr;

    video_zpos = vzpos;
    video_fb_x = 0;
	video_fb_y = 0;
	video_fb_width = output_list->mode.hdisplay;
	video_fb_height = output_list->mode.vdisplay;	

    osd_zpos = ozpos;

    ret = pthread_mutex_init(&video_mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&video_cond, NULL);
	assert(!ret);
	ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);
    ret = pthread_mutex_init(&stats_mutex, NULL);
	assert(!ret);
    ret = pthread_mutex_init(&flip_mutex, NULL);
	assert(!ret);
    ret = pthread_cond_init(&flip_cond, NULL);
	assert(!ret);

    if (!g_disable_gl) {
        init_gl();
    } else {
        printf("GL/EGL disabled (--no-osd): GPU stays idle\n");
    }

    display_connected = true;                    // init requires a connected display
    hotplug_request = drmModeAtomicAlloc();
    assert(hotplug_request);

    // Start DRM event handler thread to prevent ENOMEM (event queue full)
    event_thread_exit = false;
    ret = pthread_create(&event_thread, NULL, drm_event_thread_func, this);
    assert(!ret);
}

// Probe the connector and, on a disconnected→connected transition, redo the full
// modeset (the CRTC is left off after an unplug; page_flip commits carry no
// ALLOW_MODESET, so without this the screen stays black until an app restart).
// Assumes the same/equivalent display returns: mode, framebuffers and the GL
// surface are reused as-is. A different-resolution monitor still needs a restart.
void DrmDevice::check_display_hotplug() {
    drmModeConnector *conn = drmModeGetConnector(drm_fd, output_list->connector.id);
    if (!conn) return;
    bool now_connected = (conn->connection == DRM_MODE_CONNECTED);
    drmModeFreeConnector(conn);

    if (now_connected && !display_connected) {
        printf("[drm] display hot-plugged — performing full modeset\n");
        fflush(stdout);
        output_list->initialized = false;        // next commit re-adds connector/crtc props

        pthread_mutex_lock(&osd_mutex);
        uint32_t osd_fb = current_osd_fb_id;
        pthread_mutex_unlock(&osd_mutex);

        plane_info osd_plane_info = {
            .plane = &output_list->osd_plane,
            .fb_id = (int)osd_fb,
            .width = output_list->mode.hdisplay,
            .height = output_list->mode.vdisplay,
            .zpos = osd_zpos,
        };
        if (osd_fb > 0) {
            set_drm_object_property(hotplug_request, &output_list->osd_plane, "alpha", 65535);
            set_drm_object_property(hotplug_request, &output_list->osd_plane, "pixel blend mode", 1);
        }
        // video_info=NULL → the full modeset lights the CRTC with the placeholder
        // video FB; the next real video/OSD flip then takes over as usual.
        int ret = modeset_perform_modeset(drm_fd, output_list, hotplug_request, NULL,
                                          (osd_fb > 0) ? &osd_plane_info : NULL);
        if (ret < 0)
            fprintf(stderr, "[drm] hotplug modeset failed: %d\n", ret);
    } else if (!now_connected && display_connected) {
        printf("[drm] display unplugged\n");
        fflush(stdout);
    }
    display_connected = now_connected;
}

void* DrmDevice::drm_event_thread_func(void* arg) {
    DrmDevice* self = static_cast<DrmDevice*>(arg);
    SchedulingHelper::configure_thread(SchedulingHelper::ThreadRole::DrmEvent); 
    self->handle_events();
    return nullptr;
}

void DrmDevice::handle_events() {
    drmEventContext evctx = {};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = [](int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
        if (data) {
            DrmDevice* self = reinterpret_cast<DrmDevice*>(data);
            
            static uint64_t last_flip_complete = 0;
            static int complete_count = 0;
            // Use the kernel-provided timestamp for the actual hardware flip event
            uint64_t completion_ts = (uint64_t)sec * 1000000ULL + usec;
            
            pthread_mutex_lock(&self->osd_mutex);
            if (self->inflight_osd_fb_id) {
                self->onscreen_osd_fb_id = self->inflight_osd_fb_id;
                self->inflight_osd_fb_id = 0;
            }
            pthread_mutex_unlock(&self->osd_mutex);
            self->flip_pending = false;
            last_flip_complete = completion_ts;
            
            pthread_mutex_lock(&self->stats_mutex);
            if (self->pending_stats.recv_ts > 0) {
                // High-precision hardware-to-hardware latency calculation
                self->latest_stats.total_latency_us = (completion_ts - self->pending_stats.recv_ts) + self->pending_stats.tx_age_us;
                self->latest_stats.display_latency_us = completion_ts - self->pending_stats.dec_end_ts;
                self->latest_stats.submission_latency_us = completion_ts - self->pending_stats.submission_start_ts;
                self->latest_stats.completion_ts = completion_ts;
                self->latest_stats.available = true;
                self->pending_stats.recv_ts = 0;
            }

            // Render cadence: gap between hardware flip completions. The EWMA of
            // normal gaps is the expected frame period (self-adapts to 60/90/120fps);
            // a gap well past it is a stutter the viewer can perceive.
            if (self->cadence_last_flip_us > 0 && completion_ts > self->cadence_last_flip_us) {
                uint32_t gap = (uint32_t)(completion_ts - self->cadence_last_flip_us);
                self->cadence_gaps_us[self->cadence_idx] = gap;
                self->cadence_idx = (self->cadence_idx + 1) % CADENCE_RING;
                if (self->cadence_count < CADENCE_RING) self->cadence_count++;

                // Floor 45ms: on a 60Hz panel flips quantize to vsync and the
                // EWMA trains between the 8/16ms mix, so 25-33ms gaps (1-2
                // missed vsyncs) fired constantly at steady state — verified
                // invisible. 45ms = a freeze the eye actually catches.
                double expect = self->cadence_gap_ewma_us;
                double thresh = (expect > 0.0) ? expect * 3.5 : 0.0;
                if (thresh < 45000.0) thresh = 45000.0;
                if (expect > 0.0 && (double)gap > thresh) {
                    self->cadence_stutters++;
                } else {
                    // Only normal gaps train the expectation, so a stutter burst
                    // doesn't inflate the baseline and mask the next one.
                    self->cadence_gap_ewma_us = (expect <= 0.0)
                        ? (double)gap : (0.95 * expect + 0.05 * (double)gap);
                }
            }
            self->cadence_last_flip_us = completion_ts;
            pthread_mutex_unlock(&self->stats_mutex);

            pthread_mutex_lock(&self->flip_mutex);
            pthread_cond_broadcast(&self->flip_cond);
            pthread_mutex_unlock(&self->flip_mutex);
        }
    };


    while (!event_thread_exit) {
        struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };
        // Flip events wake the poll the moment they arrive; the timeout only
        // paces the exit check and the 2 s hotplug probe below. It was 1 ms -
        // a thousand wakeups a second on the OSD's core for nothing.
        int ret = poll(&pfd, 1, 500);
        if (event_thread_exit) break;   // before touching output_list: cleanup may be freeing it
        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Read as many events as available
            int err = drmHandleEvent(drm_fd, &evctx);
            if (err < 0) {
                // perror("drmHandleEvent");
            }
        }

        // Low-frequency display hotplug probe (full connector probe costs ~ms,
        // so keep it well away from the 1ms flip-event cadence).
        uint64_t now = get_time_us();
        if (now - last_hotplug_check_us >= 2000000) {
            last_hotplug_check_us = now;
            check_display_hotplug();
        }
    }
}

void DrmDevice::init_gl() {
    // Create GBM device
    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        fprintf(stderr, "Failed to create GBM device\n");
        return;
    }

    // Initialize EGL
    // libmali's legacy eglGetDisplay(gbm) returns NULL on RK3568; use the GBM
    // platform display instead (verified working via egltest).
    {
        PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        egl_display = getPlatDisp ? getPlatDisp(EGL_PLATFORM_GBM_KHR, gbm_dev, nullptr)
                                  : eglGetDisplay((EGLNativeDisplayType)gbm_dev);
    }
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return;
    }

    printf("EGL Initialized: %d.%d\n", major, minor);

    // Bind GLES API
    eglBindAPI(EGL_OPENGL_ES_API);

    // Find EGL Config. No multisampling unless hud_msaa is set.
    //
    // 4x MSAA was the default, and on this Mali-G52 it was most of the HUD's
    // cost: the only multisampled window config also carries a 24-bit depth
    // and 8-bit stencil buffer, and a 1080p frame took 15 ms of GPU time
    // (measured from its native fence) against 6 ms without - which held the
    // GPU at 800 MHz permanently instead of letting it idle at 200. The HUD
    // is flat 2D; the antialiasing it bought was on the thin slanted rails.
    // Mesa swrast/softpipe has no MSAA window config at all, hence the
    // fallback when it is asked for.
    const EGLint config_attribs_msaa[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 1,
        EGL_SAMPLES, 4,
        EGL_NONE
    };
    const EGLint config_attribs_plain[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint num_configs = 0;
    const bool try_msaa = Settings::getInstance().getBool("hud_msaa", false);
    if (!try_msaa || !eglChooseConfig(egl_display, config_attribs_msaa, &egl_config, 1, &num_configs) || num_configs == 0) {
        if (try_msaa) fprintf(stderr, "No 4x MSAA EGL config available; retrying without MSAA\n");
        if (!eglChooseConfig(egl_display, config_attribs_plain, &egl_config, 1, &num_configs) || num_configs == 0) {
            fprintf(stderr, "Failed to choose EGL config (error: 0x%x)\n", eglGetError());
            return;
        }
    }

    {
        EGLint id = 0, samples = 0, sbuf = 0, depth = 0, stencil = 0;
        eglGetConfigAttrib(egl_display, egl_config, EGL_CONFIG_ID, &id);
        eglGetConfigAttrib(egl_display, egl_config, EGL_SAMPLES, &samples);
        eglGetConfigAttrib(egl_display, egl_config, EGL_SAMPLE_BUFFERS, &sbuf);
        eglGetConfigAttrib(egl_display, egl_config, EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(egl_display, egl_config, EGL_STENCIL_SIZE, &stencil);
        printf("EGL config %d: samples %d (buffers %d), depth %d, stencil %d\n",
               id, samples, sbuf, depth, stencil);
    }

    // Create EGL Context
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return;
    }

    printf("EGL Context Created\n");
}

void DrmDevice::set_osd_fb(uint32_t fb_id) {
    pthread_mutex_lock(&osd_mutex);
    pending_osd_fb_id = fb_id;
    pthread_mutex_unlock(&osd_mutex);

    // While video is flowing, the next video flip carries this fb (page_flip
    // bundles the pending OSD fb into its own commit), so there is nothing to
    // do. This used to commit on its own whenever no flip happened to be in
    // flight - nearly always - which cost the OSD thread a blocking commit
    // per frame (up to a vblank, ~4 ms at 120 Hz) and competed with the video
    // thread's flips for the CRTC.
    //
    // "Flowing" is a flip submitted within three frame periods (40 ms at
    // least). Past that the video is stalled or not started, and the OSD has
    // to commit itself or it would never update.
    uint64_t now = get_time_us();
    uint64_t last = last_video_flip_us;
    pthread_mutex_lock(&stats_mutex);
    double gap = cadence_gap_ewma_us;
    pthread_mutex_unlock(&stats_mutex);
    uint64_t window = (uint64_t)(gap * 3.0);
    if (window < 40000) window = 40000;
    bool flowing = video_frm_width != 0 && last && (flip_pending || now - last < window);
    if (flowing) {
        prof::count(prof::kCountOsdPending);
        return;
    }

    pthread_mutex_lock(&osd_mutex);
    // Video resuming: a flip may have been submitted since the check above.
    // It carries the previous fb; leave this one pending for the next flip
    // rather than racing it with a commit of our own.
    if (flip_pending && get_time_us() - flip_submit_us < 250000) {
        pthread_mutex_unlock(&osd_mutex);
        prof::count(prof::kCountOsdPending);
        return;
    }
    prof::count(prof::kCountOsdCommit);
    current_osd_fb_id = pending_osd_fb_id;
    pending_osd_fb_id = 0;
    uint32_t commit_fb_id = current_osd_fb_id;
    pthread_mutex_unlock(&osd_mutex);

    sync_frozen_video_rect();

    if (commit_fb_id > 0) {
        plane_info osd_plane_info = {
            .plane = &output_list->osd_plane,
            .fb_id = (int)commit_fb_id,
            .width = output_list->mode.hdisplay,
            .height = output_list->mode.vdisplay,
            .zpos = osd_zpos,
        };
        // Blocking: when it returns, this fb is the one on screen.
        if (modeset_perform_modeset(drm_fd, output_list, output_list->osd_request, NULL, &osd_plane_info) == 0) {
            pthread_mutex_lock(&osd_mutex);
            onscreen_osd_fb_id = commit_fb_id;
            pthread_mutex_unlock(&osd_mutex);
        }
    }
}

bool DrmDevice::osd_fb_in_use(uint32_t fb_id) {
    if (!fb_id) return false;
    pthread_mutex_lock(&osd_mutex);
    bool used = fb_id == pending_osd_fb_id || fb_id == current_osd_fb_id ||
                fb_id == inflight_osd_fb_id || fb_id == onscreen_osd_fb_id;
    pthread_mutex_unlock(&osd_mutex);
    return used;
}

// Picture Size moves the video plane's rectangle, but that rectangle is only
// written by page_flip, with each new frame. With the link down there is no
// new frame: the last one stays on the plane exactly where it was, and only
// the overlay around it shrank - until the aircraft came back and the first
// frame snapped the picture into place. So while video is stalled, the OSD
// thread (which keeps committing on its own) checks whether the rectangle
// the plane was last given still matches the one the setting asks for, and
// if not, moves the frozen frame itself. A rectangle-only commit: atomic
// state is sticky, so the FB and the source stay as page_flip left them.
void DrmDevice::sync_frozen_video_rect() {
    if (last_video_fb_id_ <= 0) return;
    int vx, vy, vw, vh;
    picture_rect(video_fb_width, video_fb_height, vx, vy, vw, vh);
    vx += video_fb_x; vy += video_fb_y;
    if (vx == vrect_x_ && vy == vrect_y_ && vw == vrect_w_ && vh == vrect_h_) return;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return;
    set_drm_object_property(req, &output_list->video_plane, "CRTC_X", vx);
    set_drm_object_property(req, &output_list->video_plane, "CRTC_Y", vy);
    set_drm_object_property(req, &output_list->video_plane, "CRTC_W", vw);
    set_drm_object_property(req, &output_list->video_plane, "CRTC_H", vh);
    int ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    if (ret == 0) {
        vrect_x_ = vx; vrect_y_ = vy; vrect_w_ = vw; vrect_h_ = vh;
    } else {
        static bool warned = false;
        if (!warned) { warned = true; perror("picture size: moving the frozen video frame"); }
    }
}

bool DrmDevice::wait_for_flip_completion(int timeout_ms) {
    if (!flip_pending) return true;
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout_ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&flip_mutex);
    int rc = 0;
    while(flip_pending && rc == 0) {
        rc = pthread_cond_timedwait(&flip_cond, &flip_mutex, &ts);
    }
    pthread_mutex_unlock(&flip_mutex);

    // Watchdog: a DRM flip event can be lost (display hiccup, modeset race).
    // If flip_pending sticks, the renderer spins forever, decoded frames stop
    // being consumed, the mpp buffer pool exhausts and the DECODER jams
    // (decode_put_packet -1012 loop). Force-clear after 250ms — worst case we
    // tear one frame instead of wedging the whole pipeline.
    if (flip_pending && flip_submit_us > 0 &&
        get_time_us() - flip_submit_us > 250000ULL) {
        fprintf(stderr, "[drm] flip event lost (>250ms) — force-clearing flip_pending\n");
        pthread_mutex_lock(&osd_mutex);
        if (inflight_osd_fb_id) { onscreen_osd_fb_id = inflight_osd_fb_id; inflight_osd_fb_id = 0; }
        pthread_mutex_unlock(&osd_mutex);
        flip_pending = false;
    }
    return !flip_pending;
}

void DrmDevice::record_direct_frame(uint64_t recv_ts, uint64_t dec_start_ts,
                                    uint64_t dec_end_ts, uint64_t sub_start_ts,
                                    uint32_t tx_age_us) {
    uint64_t now = get_time_us();
    pthread_mutex_lock(&stats_mutex);
    latest_stats.total_latency_us      = (now - recv_ts) + tx_age_us;
    latest_stats.display_latency_us    = now - dec_end_ts;
    latest_stats.submission_latency_us = now - sub_start_ts;
    latest_stats.completion_ts         = now;
    latest_stats.available             = true;
    pthread_mutex_unlock(&stats_mutex);
    (void)dec_start_ts;
}

bool DrmDevice::page_flip(int fb_id, uint64_t recv_ts, uint64_t dec_start_ts, uint64_t dec_end_ts, uint64_t sub_start_ts, uint32_t tx_age_us) {
    uint64_t page_flip_start = get_time_us();
    
    if (flip_pending) {
        return false;
    }

    // Store stats for this flip
    pthread_mutex_lock(&stats_mutex);
    pending_stats = {recv_ts, dec_start_ts, dec_end_ts, sub_start_ts, tx_age_us, (uint32_t)fb_id};
    pthread_mutex_unlock(&stats_mutex);

    // Two commit styles, selectable at runtime so they can be A/B'd against the
    // OSD's "disp" figure.
    //
    // blocking_flip (what stock ar_ldy_gnd does - all four of its
    // drmModeAtomicCommit sites pass flags 0x400, ALLOW_MODESET only, and the
    // binary has no drmHandleEvent at all): the commit does not return until
    // the flip has landed, so there is never a flip in flight when the next
    // frame arrives. Latency is just "time to the next vblank", ~8ms at 60Hz.
    //
    // The non-blocking path asks for a completion event, which sets
    // flip_pending, which then makes page_flip() reject the NEXT frame until
    // the event arrives - costing an extra half-frame on average. That event
    // exists only to timestamp display latency, so the measurement was paying
    // for itself. With a blocking commit the return time IS the completion
    // time, so the stat survives without the event.
    int flags = blocking_flip ? DRM_MODE_ATOMIC_ALLOW_MODESET
                              : (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);

    drmModeAtomicSetCursor(output_list->video_request, 0);
    
    // Enable VRR if supported and not disabled
    set_drm_object_property(output_list->video_request, &output_list->crtc, "VRR_ENABLED", enable_vrr ? 1 : 0);

    // Destination rectangle for the video plane, shrunk about the screen
    // centre by picture_scale_pct. At 100 these are exactly the full-screen
    // rectangles this code always used, so the default path is untouched.
    // Sizes and offsets are kept even: the video plane is YUV and the VOP
    // wants 2-pixel alignment for it, and the OSD follows for symmetry.
    // Only the VIDEO plane is scaled here. The OSD sits on a Smart window,
    // and on this VOP2 Smart windows have no scaler: a smaller CRTC_W/H is
    // accepted by the atomic check and then silently ignored (verified in
    // debugfs - video shrank, OSD stayed 1920x1080, nothing in dmesg). The
    // OSD scissors itself to the same rectangle instead, so the two stay in
    // step; picture_rect() is the one place that rectangle is computed.
    int vx, vy, vw, vh;
    picture_rect(video_fb_width, video_fb_height, vx, vy, vw, vh);
    vx += video_fb_x; vy += video_fb_y;

    // Always set video plane properties
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "FB_ID", fb_id);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "CRTC_ID", output_list->crtc.id);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "SRC_X", 0);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "SRC_Y", 0);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "SRC_W", video_frm_width << 16);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "SRC_H", video_frm_height << 16);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "CRTC_X", vx);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "CRTC_Y", vy);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "CRTC_W", vw);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "CRTC_H", vh);
    set_drm_object_property(output_list->video_request, &output_list->video_plane, "zpos", video_zpos);

    // Bundle OSD update
    pthread_mutex_lock(&osd_mutex);
    if (pending_osd_fb_id > 0) {
        current_osd_fb_id = pending_osd_fb_id;
        pending_osd_fb_id = 0;
    }
    uint32_t osd_fb = current_osd_fb_id;
    // Mark the fb this flip carries BEFORE committing: the flip's event can be
    // handled on the event thread before this one runs again, and it must
    // find the fb there to move it on screen. Undone below if the commit fails.
    uint32_t prev_inflight = inflight_osd_fb_id;
    if (!blocking_flip) inflight_osd_fb_id = osd_fb;
    pthread_mutex_unlock(&osd_mutex);

    if (osd_fb > 0) {
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "FB_ID", osd_fb);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "CRTC_ID", output_list->crtc.id);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "CRTC_X", 0);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "CRTC_Y", 0);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "CRTC_W", output_list->mode.hdisplay);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "CRTC_H", output_list->mode.vdisplay);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "SRC_X", 0);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "SRC_Y", 0);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "SRC_W", output_list->mode.hdisplay << 16);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "SRC_H", output_list->mode.vdisplay << 16);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "zpos", osd_zpos);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "alpha", 65535);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "pixel blend mode", 1); // Pre-multiplied
    }


    // Pacing logic completely removed - frames submit immediately

    flip_pending = true;
    flip_submit_us = get_time_us();
    int ret = drmModeAtomicCommit(drm_fd, output_list->video_request, flags, this);
    if (ret == 0) {
        last_video_flip_us = flip_submit_us;
        if (blocking_flip) {
            pthread_mutex_lock(&osd_mutex);
            onscreen_osd_fb_id = osd_fb;
            inflight_osd_fb_id = 0;
            pthread_mutex_unlock(&osd_mutex);
        }
    } else {
        pthread_mutex_lock(&osd_mutex);
        inflight_osd_fb_id = prev_inflight;   // no flip, no event
        pthread_mutex_unlock(&osd_mutex);
    }

    if (blocking_flip && ret == 0) {
        // The commit returned, so the flip is done. Close out the stats here
        // exactly as the event handler would, and leave nothing pending.
        uint64_t completion_ts = get_time_us();
        flip_pending = false;
        pthread_mutex_lock(&stats_mutex);
        if (pending_stats.recv_ts > 0) {
            latest_stats.total_latency_us =
                (completion_ts - pending_stats.recv_ts) + pending_stats.tx_age_us;
            latest_stats.display_latency_us = completion_ts - pending_stats.dec_end_ts;
            latest_stats.submission_latency_us = completion_ts - pending_stats.submission_start_ts;
            latest_stats.completion_ts = completion_ts;
            latest_stats.available = true;
            pending_stats.recv_ts = 0;
        }
        pthread_mutex_unlock(&stats_mutex);
        // Same cadence bookkeeping the event handler does, so the stutter
        // counter and the expected-period EWMA stay meaningful in this mode.
        if (cadence_last_flip_us > 0 && completion_ts > cadence_last_flip_us) {
            uint32_t gap = (uint32_t)(completion_ts - cadence_last_flip_us);
            cadence_gaps_us[cadence_idx] = gap;
            cadence_idx = (cadence_idx + 1) % CADENCE_RING;
            if (cadence_count < CADENCE_RING) cadence_count++;
            double expect = cadence_gap_ewma_us;
            double thresh = (expect > 0.0) ? expect * 3.5 : 0.0;
            if (thresh < 45000.0) thresh = 45000.0;
            if (expect > 0.0 && (double)gap > thresh) cadence_stutters++;
            else cadence_gap_ewma_us = (expect <= 0.0)
                     ? (double)gap : (0.95 * expect + 0.05 * (double)gap);
        }
        cadence_last_flip_us = completion_ts;
        pthread_mutex_lock(&flip_mutex);
        pthread_cond_broadcast(&flip_cond);
        pthread_mutex_unlock(&flip_mutex);
    }
    if (ret < 0) {
        printf("drmModeAtomicCommit ret <0\n ");
        flip_pending = false; // Commit failed, no event will be sent
        if (errno == EBUSY) prof::count(prof::kCountFlipBusy);
        if (errno != EBUSY) {
            perror("drmModeAtomicCommit (page_flip)");
        }
        return false;
    }
    last_video_fb_id_ = fb_id;
    vrect_x_ = vx; vrect_y_ = vy; vrect_w_ = vw; vrect_h_ = vh;

    // Track flip submission timing (AFTER flip has been submitted)
    static uint64_t last_flip_submit = 0;
    static int flip_count = 0;
    uint64_t now = get_time_us();
    // if (last_flip_submit > 0) {
    //     uint64_t since_last = (now - last_flip_submit) / 1000; // ms
    //     if (flip_count++ % 500 == 0) { // Report every 500 flips (~4s @ 120fps)
    //         time_t now_sec = time(NULL);
    //         struct tm *tm_info = localtime(&now_sec);
    //         char time_buf[64];
    //         strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    //         
    //         printf("[%s] DEBUG: Flip interval: %llu ms, had_to_wait: %s, wait_time: %.2f ms\n", 
    //                time_buf,
    //                (unsigned long long)since_last,
    //                had_to_wait ? "YES" : "NO",
    //                wait_duration_us / 1000.0);
    //     }
    // }
    last_flip_submit = now;
    
    return true;
}

CompletedStats DrmDevice::get_latest_stats() {
    pthread_mutex_lock(&stats_mutex);
    CompletedStats s = latest_stats;
    latest_stats.available = false; // Consume
    pthread_mutex_unlock(&stats_mutex);
    return s;
}

RenderCadence DrmDevice::get_render_cadence() {
    uint32_t gaps[CADENCE_RING];
    int n;
    RenderCadence rc = {0, 0, 0};

    pthread_mutex_lock(&stats_mutex);
    n = cadence_count;
    memcpy(gaps, cadence_gaps_us, sizeof(uint32_t) * n);
    rc.stutters = cadence_stutters;
    pthread_mutex_unlock(&stats_mutex);

    if (n > 0) {
        std::sort(gaps, gaps + n);
        rc.max_ms = gaps[n - 1] / 1000.0f;
        rc.p99_ms = gaps[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1] / 1000.0f;
    }
    return rc;
}

void DrmDevice::set_frame_size(uint32_t width, uint32_t height) {
	video_frm_width = width;
	video_frm_height = height;
}

// Whole-picture scale. The video plane gets a smaller destination rectangle,
// centred, and the display controller scales it at scanout - no renderer
// involvement, no copy; the OSD scissors itself to the same rectangle. What
// is left round the edge is whatever the CRTC shows where no plane covers it,
// which is black. Read at every commit, so it takes effect on the next frame.
void DrmDevice::set_picture_scale(int pct) {
    if (pct < 60)  pct = 60;
    if (pct > 100) pct = 100;
    picture_scale_pct = pct;
}

void DrmDevice::picture_rect(int base_w, int base_h, int &x, int &y, int &w, int &h) const {
    if (picture_scale_pct >= 100) { x = 0; y = 0; w = base_w; h = base_h; return; }
    // Even sizes and offsets: the video plane is YUV and the VOP wants
    // 2-pixel alignment for it; the OSD follows so the two edges coincide.
    w = (base_w * picture_scale_pct / 100) & ~1;
    h = (base_h * picture_scale_pct / 100) & ~1;
    x = ((base_w - w) / 2) & ~1;
    y = ((base_h - h) / 2) & ~1;
}

void DrmDevice::perform_modeset_video(int fb_id) {
    if (fb_id <= 0) return; 
    plane_info video_plane_info = {
        .plane = &output_list->video_plane, 
        .fb_id = fb_id, 
        .width = video_frm_width, 
        .height = video_frm_height, 
        .zpos = video_zpos,
    };

    plane_info osd_plane_info = {
        .plane = &output_list->osd_plane,
        .fb_id = (int)current_osd_fb_id,
        .width = output_list->mode.hdisplay,
        .height = output_list->mode.vdisplay,
        .zpos = osd_zpos,
    };

    if (current_osd_fb_id > 0) {
        // Essential: Set blending properties for the OSD plane during modeset!
        // Otherwise it defaults to opaque and covers the video.
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "alpha", 65535);
        set_drm_object_property(output_list->video_request, &output_list->osd_plane, "pixel blend mode", 1); // Pre-multiplied
    }

    int ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &video_plane_info, (current_osd_fb_id > 0) ? &osd_plane_info : NULL);
    if (ret < 0) {
        static int warn_count = 0;
        if (warn_count++ % 60 == 0) fprintf(stderr, "WARNING: perform_modeset_video failed: %d\n", ret);
    }
}

void DrmDevice::cleanup() {
    // Stop the event thread before anything it uses is freed: it probes the
    // connector (output_list) every 2 s, and polls with a 500 ms timeout.
    event_thread_exit = true;
    pthread_join(event_thread, NULL);

    restore_planes_zpos(drm_fd, output_list);
	drmModeSetCrtc(drm_fd,
			       output_list->saved_crtc->crtc_id,
			       output_list->saved_crtc->buffer_id,
			       output_list->saved_crtc->x,
			       output_list->saved_crtc->y,
			       &output_list->connector.id,
			       1,
			       &output_list->saved_crtc->mode);
	drmModeFreeCrtc(output_list->saved_crtc);
	drmModeAtomicFree(output_list->video_request);
	drmModeAtomicFree(output_list->osd_request);
	if (hotplug_request) drmModeAtomicFree(hotplug_request);
	modeset_cleanup(drm_fd, output_list);

	close(drm_fd);
	
	int ret = pthread_cond_destroy(&video_cond);
	assert(!ret);
	ret = pthread_mutex_destroy(&video_mutex);
	assert(!ret);
    ret = pthread_mutex_destroy(&stats_mutex);
	assert(!ret);
    ret = pthread_mutex_destroy(&flip_mutex);
	assert(!ret);
    ret = pthread_cond_destroy(&flip_cond);
	assert(!ret);

    //free(output_list_original);
}

void DrmDevice::cond_signal() {
    int ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);	
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);	
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);	
}
// ---------------------------------------------------------------- writeback
//
// Capture the composited output. glReadPixels can only see the GL/OSD layer;
// the FPV video is on a separate DRM plane that the display controller blends
// at scanout, which is why GL-based screen capture came out with a black video
// area. The writeback connector hands us what the CRTC actually produced.

bool DrmDevice::writeback_init(int w, int h, const std::vector<int>& dmabuf_fds) {
    if (dmabuf_fds.empty()) return false;
    writeback_deinit();

    // Locate the writeback connector (type 18). Only visible because
    // DRM_CLIENT_CAP_WRITEBACK_CONNECTORS was set at open time.
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) return false;
    for (int i = 0; i < res->count_connectors && !wb_connector_id; i++) {
        drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == 18 /* DRM_MODE_CONNECTOR_WRITEBACK */)
            wb_connector_id = c->connector_id;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (!wb_connector_id) {
        printf("writeback: no connector\n");
        return false;
    }

    {
        drmModeObjectProperties *props =
            drmModeObjectGetProperties(drm_fd, wb_connector_id, DRM_MODE_OBJECT_CONNECTOR);
        for (uint32_t i = 0; props && i < props->count_props; i++) {
            drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
            if (!p) continue;
            if (!strcmp(p->name, "CRTC_ID"))                 wb_prop_crtc_  = p->prop_id;
            if (!strcmp(p->name, "WRITEBACK_FB_ID"))         wb_prop_fb_    = p->prop_id;
            if (!strcmp(p->name, "WRITEBACK_OUT_FENCE_PTR")) wb_prop_fence_ = p->prop_id;
            drmModeFreeProperty(p);
        }
        if (props) drmModeFreeObjectProperties(props);
    }
    if (!wb_prop_crtc_ || !wb_prop_fb_ || !wb_prop_fence_) {
        printf("writeback: connector lacks its properties\n");
        wb_connector_id = 0;
        return false;
    }

    // One framebuffer per encoder buffer. NV12: Y plane then interleaved
    // CbCr, both at the frame stride.
    for (int fd : dmabuf_fds) {
        uint32_t handle = 0, fb = 0;
        if (drmPrimeFDToHandle(drm_fd, fd, &handle)) {
            perror("writeback: drmPrimeFDToHandle");
            writeback_deinit();
            return false;
        }
        uint32_t handles[4] = { handle, handle, 0, 0 };
        uint32_t pitches[4] = { (uint32_t)w, (uint32_t)w, 0, 0 };
        uint32_t offsets[4] = { 0, (uint32_t)(w * h), 0, 0 };
        if (drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0)) {
            perror("writeback: drmModeAddFB2(NV12)");
            writeback_deinit();
            return false;
        }
        std::lock_guard<std::mutex> lk(wb_mu_);
        wb_fbs_.push_back(fb);
        wb_handles_.push_back(handle);
    }

    {
        std::lock_guard<std::mutex> lk(wb_mu_);
        wb_free_.clear();
        wb_ready_.clear();
        for (int i = 0; i < (int)wb_fbs_.size(); i++) wb_free_.push_back(i);
    }
    wb_on_ = true;
    printf("writeback: connector %u -> %dx%d NV12, %zu buffers\n",
           wb_connector_id, w, h, wb_fbs_.size());
    return true;
}

// Called while a capture commit is being built. Attaching the writeback connector to the CRTC for this one commit makes the
// controller composite the frame into the buffer as well as scanning it out.
int DrmDevice::wb_attach(drmModeAtomicReq* req, int32_t* fence) {
    *fence = -1;
    if (!wb_on_) return -1;
    int idx;
    uint32_t fb;
    {
        // Under the lock: a flip on the video thread can race a recording
        // being stopped on another, and the buffer list goes away with it.
        std::lock_guard<std::mutex> lk(wb_mu_);
        if (!wb_on_ || wb_free_.empty()) return -1;   // encoder behind: skip this one
        idx = wb_free_.front();
        wb_free_.pop_front();
        fb = wb_fbs_[idx];
    }
    bool ok = drmModeAtomicAddProperty(req, wb_connector_id, wb_prop_crtc_,
                                       output_list->crtc.id) >= 0 &&
              drmModeAtomicAddProperty(req, wb_connector_id, wb_prop_fb_, fb) >= 0 &&
              drmModeAtomicAddProperty(req, wb_connector_id, wb_prop_fence_,
                                       (uint64_t)(uintptr_t)fence) >= 0;
    if (!ok) {
        std::lock_guard<std::mutex> lk(wb_mu_);
        wb_free_.push_front(idx);
        return -1;
    }
    return idx;
}

void DrmDevice::wb_committed(int idx, int32_t fence, bool ok) {
    if (idx < 0) return;
    std::lock_guard<std::mutex> lk(wb_mu_);
    if (ok && wb_on_) {
        uint64_t now = get_time_us();
        wb_ready_.push_back({idx, fence, now});
        wb_cv_.notify_one();
    } else {
        if (fence >= 0) close(fence);
        wb_free_.push_back(idx);
    }
}

bool DrmDevice::writeback_next(WbShot& shot, int timeout_ms) {
    std::unique_lock<std::mutex> lk(wb_mu_);
    if (!wb_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [this] { return !wb_ready_.empty(); }))
        return false;
    shot = wb_ready_.front();
    wb_ready_.pop_front();
    return true;
}

void DrmDevice::writeback_release(int idx) {
    std::lock_guard<std::mutex> lk(wb_mu_);
    wb_free_.push_back(idx);
}

void DrmDevice::writeback_capture() {
    if (!wb_on_) return;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return;
    int32_t fence = -1;
    int idx = wb_attach(req, &fence);
    if (idx < 0) { drmModeAtomicFree(req); return; }
    // Non-blocking, so the OSD thread is not held for a vblank. The kernel
    // refuses that while another commit is in flight (the video's page flip);
    // then this one waits its turn.
    int ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    if (ret == -EBUSY)
        ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if (ret) {
        static bool warned = false;
        if (!warned) { warned = true; printf("writeback: commit failed (%d)\n", ret); }
    }
    wb_committed(idx, fence, ret == 0);
}

// The fence signals when the CRTC has finished writing the buffer. Encoding
// before then would hand the encoder a half-composited frame.
void DrmDevice::writeback_wait(int fence) {
    if (fence < 0) return;
    struct pollfd pfd = { fence, POLLIN, 0 };
    poll(&pfd, 1, 100);
    close(fence);
}

void DrmDevice::writeback_deinit() {
    std::vector<uint32_t> fbs, handles;
    {
        std::lock_guard<std::mutex> lk(wb_mu_);
        wb_on_ = false;
        // Captures still in flight finish writing before their buffers go.
        for (auto& s : wb_ready_) writeback_wait(s.fence);
        wb_ready_.clear();
        wb_free_.clear();
        fbs.swap(wb_fbs_);
        handles.swap(wb_handles_);
    }
    // A commit that attached a buffer just before this still holds its
    // framebuffer, and through it the memory, until the job is done.
    for (uint32_t fb : fbs) drmModeRmFB(drm_fd, fb);
    // The imported handles were never closed before, which kept every
    // recording's buffers alive for the life of the process.
    for (uint32_t h : handles) {
        struct drm_gem_close gc = {};
        gc.handle = h;
        drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    wb_connector_id = 0;
}

bool DrmDevice::set_display_property(const char* name, uint64_t value) {
    if (!output_list) return false;
    uint32_t conn_id = output_list->connector.id;
    if (!conn_id) return false;

    drmModeObjectProperties *props =
        drmModeObjectGetProperties(drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) return false;
    uint32_t prop_id = 0;
    for (uint32_t i = 0; i < props->count_props && !prop_id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, name)) prop_id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    if (!prop_id) return false;

    int ret = drmModeObjectSetProperty(drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,
                                       prop_id, value);
    if (ret) printf("drm: set %s=%llu failed (%d)\n", name, (unsigned long long)value, ret);
    return ret == 0;
}
