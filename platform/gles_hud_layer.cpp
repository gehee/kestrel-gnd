#include "platform/gles_hud_layer.hpp"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <drm_fourcc.h>
#include <gbm.h>

// rockchip-mali ships a cut-down gbm.h that omits gbm_bo_get_modifier, even
// though libmali exports the symbol (verified: defined in the shipped
// libmali.so.1). Mesa's gbm.h declares it. Declare it here so kestrel builds
// against either header - a second identical declaration is harmless where the
// header already provides one.
extern "C" uint64_t gbm_bo_get_modifier(struct gbm_bo *bo);
#include <GLES2/gl2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "base/prof.hpp"
#include "base/time_util.h"
#include "display/drm.hpp"
#include "render/gl_context.hpp"

namespace platform {

GlesHudLayer::~GlesHudLayer() {
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(GlContext::instance().display(), surface_);
}

// One GBM surface at the screen's size, its buffers locked as they come (the
// framebuffers are made on the fly and kept, present()).
bool GlesHudLayer::init() {
    const display::Mode mode = dev_.mode();
    const EGLDisplay display = GlContext::instance().display();
    struct gbm_surface *gs = gbm_surface_create(GlContext::instance().gbm(), mode.w, mode.h,
                                             GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!gs) {
        fprintf(stderr, "Failed to create GBM surface\n");
        return false;
    }

    EGLSurface surf = eglCreateWindowSurface(display, GlContext::instance().config(), (EGLNativeWindowType)gs, NULL);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface from GBM surface (error: 0x%x)\n", eglGetError());
        return false;
    }
    gs_ = gs;
    surface_ = surf;
    printf("Created GL Surface: gs=%p, surf=%p\n", (void*)gs, (void*)surf);

    const char* ext = eglQueryString(display, EGL_EXTENSIONS);
    if (ext && strstr(ext, "EGL_ANDROID_native_fence_sync")) {
        pfn_eglCreateSyncKHR  = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        pfn_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        pfn_eglDupNativeFenceFDANDROID =
            (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
        if (!pfn_eglDestroySyncKHR || !pfn_eglDupNativeFenceFDANDROID) pfn_eglCreateSyncKHR = nullptr;
    }
    if (prof::enabled())
        printf("[OSD] prof: native fences %s\n", pfn_eglCreateSyncKHR ? "available" : "NOT available");
    return true;
}

void GlesHudLayer::begin() {
    release_unused_bos();
    eglMakeCurrent(GlContext::instance().display(), surface_, surface_, GlContext::instance().context());
}

// Hand buffers back to GBM - so the GPU may draw into them again - once the
// display can no longer read them: not pending, not in a flip in flight, not
// on screen. It used to be "keep the newest two", which with frames that never
// reached the screen (several renders between two video flips) could hand
// back the very buffer being scanned out. Run at both ends of a frame, so a
// flip that landed while the loop slept frees its buffer before EGL wants one.
void GlesHudLayer::release_unused_bos() {
    struct gbm_surface* gs = gs_;
    for (size_t i = 0; i < bo_release_queue_.size(); ) {
        struct gbm_bo* old_bo = bo_release_queue_[i];
        auto it = bo_to_fb_.find(old_bo);
        if (it != bo_to_fb_.end() && dev_.osd_fb_in_use(it->second)) { i++; continue; }
        gbm_surface_release_buffer(gs, old_bo);
        bo_release_queue_.erase(bo_release_queue_.begin() + i);
    }
    // Backstop: never hold more than the old code did (two queued plus the
    // current one). Correct tracking never needs more; stale tracking (a lost
    // flip event) must not be able to lock every buffer and stall EGL.
    while (bo_release_queue_.size() > 2) {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[OSD] releasing a buffer still marked in use (backstop)\n"); }
        gbm_surface_release_buffer(gs, bo_release_queue_.front());
        bo_release_queue_.erase(bo_release_queue_.begin());
    }
}

// The end of a frame: swap, wrap the buffer as a DRM framebuffer, and hand it to
// the display with the picture's rectangle.
void GlesHudLayer::present(const display::Tile& tile) {
    struct gbm_surface *gs = gs_;
    const display::Mode mode = dev_.mode();
    const EGLDisplay display = GlContext::instance().display();
    const int screen_w = mode.w;
    const int screen_h = mode.h;

    prof::mark(prof::kDraw);
    // Profiling only: a native fence for this frame's GPU work, so its real
    // completion time can be read from the kernel without waiting on it.
    EGLSyncKHR prof_sync = EGL_NO_SYNC_KHR;
    if (prof::enabled() && pfn_eglCreateSyncKHR) {
        const EGLint a[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE };
        prof_sync = pfn_eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    }
    // The frame is offered to the display only once the GPU has drawn it. A
    // video flip carries the newest offered OSD frame in its own commit, and
    // the kernel waits for that frame's rendering before the commit takes
    // effect: with the OSD redrawn every refresh (the stats screen) half the
    // video flips missed their vblank by waiting on the GPU - 5 ms more
    // latency than on the live view. Waiting here costs the OSD thread, not
    // the video.
    EGLSyncKHR done_sync = EGL_NO_SYNC_KHR;
    if (pfn_eglCreateSyncKHR && pfn_eglDupNativeFenceFDANDROID) {
        const EGLint a[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE };
        done_sync = pfn_eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    }
    if (done_sync == EGL_NO_SYNC_KHR) glFinish();
    uint64_t prof_submit = get_time_us();
    eglSwapBuffers(display, surface_);
    if (prof_sync != EGL_NO_SYNC_KHR) {
        prof::gpu_fence(pfn_eglDupNativeFenceFDANDROID(display, prof_sync), prof_submit);
        pfn_eglDestroySyncKHR(display, prof_sync);
    }
    // closed on every way out of here, the early returns below included
    struct FenceFd { int fd = -1; ~FenceFd() { if (fd >= 0) close(fd); } } done;
    if (done_sync != EGL_NO_SYNC_KHR) {
        done.fd = pfn_eglDupNativeFenceFDANDROID(display, done_sync);
        pfn_eglDestroySyncKHR(display, done_sync);
    }
    prof::mark(prof::kSwap);

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
    if (!bo) {
        fprintf(stderr, "Failed to lock front buffer\n");
        return;
    }

    uint32_t fb_id;
    if (bo_to_fb_.count(bo) == 0) {
        int prime_fd = gbm_bo_get_fd(bo);
        if (prime_fd < 0) {
            fprintf(stderr, "Failed to get prime FD from gbm_bo: %d\n", errno);
            gbm_surface_release_buffer(gs, bo);
            return;
        }
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(dev_.drm_fd, prime_fd, &handle) < 0) {
            perror("drmPrimeFDToHandle (OSD)");
            close(prime_fd);
            gbm_surface_release_buffer(gs, bo);
            return;
        }
        // prime_fd is kept: the screen recorder reads the OSD through it
        // (register_osd_fb below). The BOs live as long as the app.

        uint32_t stride = gbm_bo_get_stride(bo);
        uint32_t handles[4] = {handle, 0, 0, 0};
        uint32_t pitches[4] = {stride, 0, 0, 0};
        uint32_t offsets[4] = {0, 0, 0, 0};
        // Mesa allocates the GBM scanout buffer with the GPU's native tiling
        // (e.g. AMD GFX9), which carries a DRM format modifier. A plain
        // drmModeAddFB2 declares no modifier, so the kernel assumes the buffer
        // is linear and amdgpu rejects the mismatch with EINVAL. Pass the bo's
        // real modifier via drmModeAddFB2WithModifiers — matching the video
        // DMA-BUF import path in renderer.cpp. Rockchip/mali returns
        // MOD_INVALID here, in which case we fall back to the plain call.
        uint64_t modifier = gbm_bo_get_modifier(bo);
        uint64_t modifiers[4] = {modifier, 0, 0, 0};
        uint32_t bo_w = gbm_bo_get_width(bo);
        uint32_t bo_h = gbm_bo_get_height(bo);
        uint32_t bo_fmt = gbm_bo_get_format(bo);
        printf("drmModeAddFB2 OSD: dev->drm_fd=%d, screen_w=%d, screen_h=%d, bo_w=%u, bo_h=%u, bo_fmt=0x%08x, handle=%u, stride=%u, modifier=0x%llx\n",
               dev_.drm_fd, screen_w, screen_h, bo_w, bo_h, bo_fmt, handle, stride,
               (unsigned long long)modifier);
        int addfb_ret;
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            addfb_ret = drmModeAddFB2WithModifiers(dev_.drm_fd, screen_w, screen_h,
                            DRM_FORMAT_ARGB8888, handles, pitches, offsets, modifiers,
                            &fb_id, DRM_MODE_FB_MODIFIERS);
        } else {
            addfb_ret = drmModeAddFB2(dev_.drm_fd, screen_w, screen_h,
                            DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb_id, 0);
        }
        if (addfb_ret) {
            perror("drmModeAddFB2 (OSD)");
            close(prime_fd);
            gbm_surface_release_buffer(gs, bo);
            return;
        }
        bo_to_fb_[bo] = fb_id;
        dev_.register_osd_fb(fb_id, prime_fd, stride);
    } else {
        fb_id = bo_to_fb_[bo];
    }

    if (locked_bo_) {
        bo_release_queue_.push_back(locked_bo_);
    }
    locked_bo_ = bo;
    prof::mark(prof::kLock);
    if (done.fd >= 0) {
        struct pollfd pfd = { done.fd, POLLIN, 0 };
        poll(&pfd, 1, 100);   // the GPU has finished this frame (see done_sync)
    }
    dev_.stage_tile(tile);   // the picture's rectangle goes with this frame
    dev_.set_osd_fb(fb_id);
    prof::mark(prof::kHandoff);

    release_unused_bos();
    prof::frame_end();
}

}  // namespace platform
