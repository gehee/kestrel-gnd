#pragma once
#include <stdint.h>
#include <pthread.h>
#include <functional>
#include <memory>
#include <GLES2/gl2.h>

// Forward declarations only — including vdec_rk.hpp here would create an
// include cycle (vdec_rk.hpp -> renderer.hpp -> osd.hpp -> bg_video.hpp).
// The full type is included in bg_video.cpp.
#ifdef USE_RKMPP
class VdecRK;
#endif
class DrmDevice;

// Metadata for a hardware-decoded frame that lives in VPU memory (DRM_PRIME).
// The shared_ptr `ref` keeps the AVFrame (and thus the DRM fd) alive.
struct BgDrmFrame {
    int      fd        = -1;
    int      width     = 0,  height    = 0;
    uint32_t stride_y  = 0,  stride_uv = 0;
    uint32_t offset_uv = 0;
    uint64_t modifier  = 0;
    uint32_t drm_fmt   = 0;   // DRM_FORMAT_NV12 etc.
    std::shared_ptr<void> ref; // keeps AVFrame alive so fd stays valid
    bool valid = false;
};

// Looping background video player.
// Decodes an MP4/video in a background thread using the best available
// decoder (h264_rkmpp on RK3588).
//
// Two frame delivery paths, selected automatically:
//   DRM path  (hardware decoder, USE_RKMPP build):
//     get_latest_drm_frame() — returns VPU DMA-BUF metadata for zero-copy
//     EGL import by the caller; no CPU pixel copy.
//   CPU path  (software decoder fallback):
//     upload_latest_frame()  — uploads BGRA data to a GL_TEXTURE_2D.
class BgVideoPlayer {
public:
    // `dev` is needed to construct the VdecRK hardware decoder (DRM buffer
    // allocation). The decoder feeds frames back through a sink into the same
    // zero-copy DRM path (get_latest_drm_frame) the GL/OSD layer already uses.
    BgVideoPlayer(const char* path, std::shared_ptr<DrmDevice> dev);
    ~BgVideoPlayer();

    void start();
    void stop();

    // Fired by decode thread each time a new frame is ready — drives OSD wakeup.
    std::function<void()> on_new_frame;

    // --- DRM / zero-copy path (hardware decoder) ---
    bool is_drm() const { return is_drm_; }
    // Returns true and fills `out` when a new DRM frame is available.
    // `out.ref` keeps the AVFrame alive; safe to drop after eglCreateImageKHR.
    bool get_latest_drm_frame(BgDrmFrame& out);

    // --- CPU / BGRA path (software decoder fallback) ---
    // GL thread: upload the latest decoded BGRA frame into `tex` (GL_TEXTURE_2D).
    bool upload_latest_frame(GLuint tex);

    int width()  const { return w_; }
    int height() const { return h_; }

private:
    static void* thread_func(void* arg);
    void decode_loop();

    const char* path_;
    volatile bool running_ = false;   // true while the feed/demux loop runs
    volatile bool vdec_stop_ = false; // VdecRK::run_frame stop signal (true = stop)
    pthread_t thread_;                 // feed/demux thread (decode_loop)
    pthread_t vdec_thread_ = 0;        // VdecRK::run_frame output thread
    pthread_mutex_t mu_;

    // Direct MPP hardware decoder (created in decode_loop once the codec is
    // known). Reuses the same path as the live FPV feed; its output is routed
    // to front_drm_ via a frame sink instead of the video plane.
    std::shared_ptr<DrmDevice> dev_;
#ifdef USE_RKMPP
    std::unique_ptr<VdecRK> vdec_;
#endif

    // DRM double-buffer (hardware path)
    BgDrmFrame front_drm_, back_drm_;
    bool is_drm_ = false;

    // CPU double-buffer (software path)
    uint8_t* front_ = nullptr;
    uint8_t* back_  = nullptr;

    bool new_frame_ = false;
    int w_ = 0, h_ = 0;
};
