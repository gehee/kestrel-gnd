#ifndef RENDERER_H  // Check if VDEC_H is not defined
#define RENDERER_H  // Define VDEC_H

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/hwcontext_drm.h>
    #include <libavutil/pixdesc.h>
}

#include <memory>
#include <atomic>
#include <mutex>
#include <unistd.h>

#include "dvr.hpp"
#include "osd.hpp"
#include "drm.hpp"
#include "utils/scheduling_helper.hpp"
#include "utils/time_util.h"
#include <functional>
#include "queue/Queue.hpp"

#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt)
#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179

enum RenderMode {
    Disable,     // No rendering (benchmarking only)
    Atomic,      // V-Sync enabled (tear-free)
    FrontBuffer  // No V-Sync (lowest latency, tearing possible)
};

// Structure to hold info about displayable DRM framebuffers
struct DisplayBufferInfo {
    uint32_t drm_fb_id;        // Framebuffer ID from drmModeAddFB2

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;

    // Optional: for HW buffers:
    int prime_fd;              // The original PRIME FD (for tracking/cleanup)
    uint32_t gem_handle;       // GEM handle for this buffer

    // Optional: for SW buffers:
    uint8_t* sw_base = nullptr; // Y base
    uint32_t sw_u_offset = 0;   // Offset to U plane (for YUV420)
    uint32_t sw_v_offset = 0;   // Offset to V plane (for YUV420)
    uint32_t sw_uv_offset = 0;  // Offset to UV plane (for NV12)
    
    // Format helper
    uint32_t format = 0; 

    bool in_use = false;

    DisplayBufferInfo() : drm_fb_id(0), prime_fd(-1), gem_handle(0), sw_base(nullptr), width(0), height(0), stride(0), sw_u_offset(0), sw_v_offset(0), sw_uv_offset(0), format(0) {}
};

struct DrmBuffer {
    uint32_t handle;
    uint32_t fb_id;
    int prime_fd;
    uint32_t size;
    uint32_t stride;   // driver-aligned pitch returned by CREATE_DUMB (amdgpu needs this, not `width`)
};

struct DecodedUnit{
    uint64_t pts;
    uint32_t width;
    uint32_t height;
    uint64_t recv_ts;
    uint64_t dec_start_ts;
    uint64_t dec_end_ts;
    uint32_t tx_capture_delay_us;
    uint32_t tx_processing_delay_us;
    bool is_keyframe = false;

    // AVFrame
    // AVFrame *av_frame_ref;
    std::shared_ptr<void> frame_ref; // Holds ownership of the allocated frame (if any) with custom deleter
    uint32_t drm_pixel_format = 0;
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};

    // HW prime data
    bool has_prime_fd;
    int prime_fd;

    // SW data
    uint8_t *data[8];
    int linesize[8];
};

struct Stats{
    int frame_counter = 0;
    uint64_t decoding_stats_start = 0;
    uint64_t last_frame_ts = 0;

    float proc_latency_avg[200] = {0};
    float proc_latency_min = 1e18f; // Use a large but float-friendly value
    float proc_latency_max = 0;

    float total_latency_avg[200] = {0};
    float total_latency_min = 1e18f;
    float total_latency_max = 0;

    float decoding_latency_avg[200] = {0};
    float decoding_latency_min = 1e18f;
    float decoding_latency_max = 0;

    float display_latency_avg[200] = {0};
    float display_latency_min = 1e18f;
    float display_latency_max = 0;

    float tx_latency_avg[200] = {0};
    float tx_latency_min = 1e18f;
    float tx_latency_max = 0;

    float capture_latency_avg[200] = {0};
    float capture_latency_min = 1e18f;
    float capture_latency_max = 0;

    float reassemble_latency_avg[200] = {0};
    float reassemble_latency_min = 1e18f;
    float reassemble_latency_max = 0;

    float frame_pace_avg[200] = {0};
    float frame_pace_min = 1e18f;
    float frame_pace_max = 0;
};

class Renderer {
    private:
        std::shared_ptr<DrmDevice> dev;
        std::shared_ptr<DVR> dvr;
        std::shared_ptr<OSD> osd;

        volatile bool *should_stop;
        Queue* decoded_unit_queue;

        RenderMode render_mode = Disable;
        
        bool buffer_initialized= false;
        std::vector<DisplayBufferInfo> display_buffers;
        int current_display_buffer_idx = -1;
        int next_display_buffer_idx = 0;

        // Resolution-change flush handshake (set by decoder thread, cleared by renderer thread)
        std::atomic<bool> flush_requested{false};
        std::atomic<bool> flush_done{false};
        uint64_t last_submitted_pts = 0;
        const int MAX_DISPLAY_BUFFERS = 32;

        // Decoding stats
        Stats stats_;
        bool console_stats = false;

    private:
        bool create_drm_buffer(uint32_t width, uint32_t height, uint32_t format, DrmBuffer& out, bool export_fd = false);
        void init_buffers(DecodedUnit *du);
        void copy_sw_frame(DisplayBufferInfo& buf_info, DecodedUnit* du);
        void convert_yuv420_to_nv12(DisplayBufferInfo& buf_info, DecodedUnit* du);
        void cleanup_display_buffer(DisplayBufferInfo& buffer_info);
        int get_or_create_display_buffer(DecodedUnit* frame, DisplayBufferInfo& out_info);
        void reset_stats(uint64_t now);
    public:
        virtual ~Renderer() { delete decoded_unit_queue; }
        virtual bool render_frame(DecodedUnit *du);
        virtual void queue_frame(std::shared_ptr<DecodedUnit> du);
        virtual void present_frame(uint32_t fb_id, uint32_t width, uint32_t height, uint64_t pts, uint64_t recv_ts, uint64_t dec_start_ts, uint64_t dec_end_ts, uint32_t tx_age_us);
        void update_stats(DecodedUnit *du, uint64_t display_ts);

        Renderer(RenderMode rmode,  std::shared_ptr<DrmDevice> dev_, std::shared_ptr<DVR> dvr_, std::shared_ptr<OSD> osd_, volatile bool* stop_signal, bool cstats = false) : 
            render_mode(rmode), dev(dev_), dvr(dvr_), osd(osd_), should_stop(stop_signal), console_stats(cstats) {
                // Increased queue size to allow decoder to run ahead, 
                // enabling the renderer to skip old frames and pick the latest one.
                decoded_unit_queue = new Queue(8);
            }

        //int get_drm_fd() { return dev->drm_fd; }

        // Called by decoder thread before freeing old DRM/MPP resources on resolution change.
        // Signals the renderer to drain its frame queue and clear its display buffer cache,
        // then blocks until the renderer confirms the flush is done.
        void request_flush() {
            flush_done.store(false);
            flush_requested.store(true);
            // Spin-wait up to 200ms for renderer thread to ack
            for (int i = 0; i < 200 && !flush_done.load(); i++)
                usleep(1000);
        }

        void run();

        static void* run_thread(void* arg) {
            static_cast<Renderer*>(arg)->run();
            return nullptr;
        }

        std::function<void(int, int)> cmd_cb;
        void set_command_callback(std::function<void(int, int)> cb) { cmd_cb = cb; }
};


#endif // RENDERER_H  // End of the header guard