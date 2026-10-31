#pragma once
// The HUD's layer on a KMS display drawn with GLES (display/hud_layer.hpp): a
// GBM surface the GPU draws each frame into, its buffers wrapped as DRM
// framebuffers and handed to the display's HUD plane once the GPU has drawn
// them. Moved out of the OSD as it was.
#include <cstdint>
#include <map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "display/hud_layer.hpp"

class DrmDevice;
struct gbm_surface;
struct gbm_bo;

namespace platform {

class GlesHudLayer final : public display::HudLayer {
public:
    explicit GlesHudLayer(DrmDevice& dev) : dev_(dev) {}
    ~GlesHudLayer() override;
    bool init() override;
    void begin() override;
    void present(const display::Tile& tile) override;

private:
    void release_unused_bos();

    DrmDevice& dev_;
    struct gbm_surface* gs_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    std::map<struct gbm_bo*, uint32_t> bo_to_fb_;   // each buffer's framebuffer, made once
    struct gbm_bo* locked_bo_ = nullptr;
    std::vector<struct gbm_bo*> bo_release_queue_;
    // Native fences (EGL_ANDROID_native_fence_sync): a frame goes to the
    // display once the GPU has drawn it; and its real completion, profiling.
    PFNEGLCREATESYNCKHRPROC pfn_eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC pfn_eglDestroySyncKHR = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC pfn_eglDupNativeFenceFDANDROID = nullptr;
};

}  // namespace platform
