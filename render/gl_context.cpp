#include "render/gl_context.hpp"

#include <cstdio>

#include "settings.hpp"

// Set from main() when --no-osd is given: skips all EGL/GLES bring-up.
bool g_disable_gl = false;

GlContext &GlContext::instance() {
    static GlContext g;
    return g;
}

bool GlContext::init(int drm_fd) {
    // Create GBM device
    gbm_dev_ = gbm_create_device(drm_fd);
    if (!gbm_dev_) {
        fprintf(stderr, "Failed to create GBM device\n");
        return false;
    }

    // Initialize EGL
    // libmali's legacy eglGetDisplay(gbm) returns NULL on RK3568; use the GBM
    // platform display instead (verified working via egltest).
    {
        PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        egl_display_ = getPlatDisp ? getPlatDisp(EGL_PLATFORM_GBM_KHR, gbm_dev_, nullptr)
                                  : eglGetDisplay((EGLNativeDisplayType)gbm_dev_);
    }
    if (egl_display_ == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display_, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return false;
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
    if (!try_msaa || !eglChooseConfig(egl_display_, config_attribs_msaa, &egl_config_, 1, &num_configs) || num_configs == 0) {
        if (try_msaa) fprintf(stderr, "No 4x MSAA EGL config available; retrying without MSAA\n");
        if (!eglChooseConfig(egl_display_, config_attribs_plain, &egl_config_, 1, &num_configs) || num_configs == 0) {
            fprintf(stderr, "Failed to choose EGL config (error: 0x%x)\n", eglGetError());
            return false;
        }
    }

    {
        EGLint id = 0, samples = 0, sbuf = 0, depth = 0, stencil = 0;
        eglGetConfigAttrib(egl_display_, egl_config_, EGL_CONFIG_ID, &id);
        eglGetConfigAttrib(egl_display_, egl_config_, EGL_SAMPLES, &samples);
        eglGetConfigAttrib(egl_display_, egl_config_, EGL_SAMPLE_BUFFERS, &sbuf);
        eglGetConfigAttrib(egl_display_, egl_config_, EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(egl_display_, egl_config_, EGL_STENCIL_SIZE, &stencil);
        printf("EGL config %d: samples %d (buffers %d), depth %d, stencil %d\n",
               id, samples, sbuf, depth, stencil);
    }

    // Create EGL Context
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attribs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }

    printf("EGL Context Created\n");
    return true;
}
