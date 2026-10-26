#pragma once
// The GBM device and EGL display, config and context the GLES HUD draws with.
// One per process, made by main() after the display is up (and not at all
// with --no-osd). It used to live inside DrmDevice; the display itself needs
// no GL, and a box without a GPU has none to make.

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

// Set from main() when --no-osd is given: skips all EGL/GLES bring-up.
extern bool g_disable_gl;

class GlContext {
public:
    static GlContext &instance();

    // On the DRM device drm_fd. False (and logged) if any step fails; the
    // getters then hold what was made before it.
    bool init(int drm_fd);

    struct gbm_device *gbm() const { return gbm_dev_; }
    EGLDisplay display() const { return egl_display_; }
    EGLContext context() const { return egl_context_; }
    EGLConfig  config() const { return egl_config_; }

private:
    GlContext() = default;
    struct gbm_device *gbm_dev_ = nullptr;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig  egl_config_ = nullptr;
};
