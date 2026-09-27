#ifndef VDEC_RK_H  // Check if VDEC_H is not defined
#include <cstring>
#define VDEC_RK_H  // Define VDEC_H

#include <rockchip/rk_mpi.h>
#include <functional>
#include <memory>

#include "vdec.hpp"
#include "renderer.hpp"

#define MAX_FRAMES 16		// min 16 and 20+ recommended (mpp/readme.txt)
#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179


struct RkMPI {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		uint32_t fb_id;
		uint32_t handle;
	} frame_to_drm[MAX_FRAMES];
} ;

class VdecRK : public Vdec {
    private:
        std::shared_ptr<Renderer> renderer;
        std::shared_ptr<DrmDevice> dev;
        int decoder_stalled_count = 0;
        volatile bool *should_stop;
        RkMPI mpi;
        pthread_mutex_t video_mutex;
        pthread_cond_t video_cond;
        bool frm_eos = false;
        MppPacket packet;
        static constexpr int NUM_SLICES_BUFFERS = 64;
        uint8_t* slices_buffers[NUM_SLICES_BUFFERS];
        int current_slice_buffer_idx = 0;

        void init_buffer(MppFrame frame);
        void set_mpp_decoding_parameters();
        void set_control_verbose(MpiCmd control,RK_U32 enable);
        void emit_decoded(std::shared_ptr<DecodedUnit> du, MppBuffer buffer);

        std::vector<uint8_t> cached_vps;
        std::vector<uint8_t> cached_sps;
        std::vector<uint8_t> cached_pps;

        bool first_successful_decode = false;
        uint64_t last_decoded_frame_time_ms = 0;
        uint64_t last_idr_request_ms = 0;

        // Optional frame sink. When set, decoded NV12 frames are delivered here
        // (as DecodedUnit, with frame_ref holding the MPP buffer alive) instead
        // of renderer->queue_frame(). The background-video player uses this to
        // feed its GL-texture path; the live feed leaves it unset.
        std::function<void(std::shared_ptr<DecodedUnit>)> frame_sink_;

    public:
        void set_frame_sink(std::function<void(std::shared_ptr<DecodedUnit>)> cb) { frame_sink_ = std::move(cb); }
        // Bound the blocking output wait (default is infinite) so the run_frame
        // thread can notice *should_stop and exit promptly when the background
        // player is stopped mid-stream. parameter is in milliseconds.
        void set_output_timeout(RK_S64 ms) { mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_TIMEOUT, &ms); }
        void feed_packet_to_decoder(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_key_override = false);
        void cleanup();
        void run_frame();

        virtual ~VdecRK() {
            for (int i = 0; i < NUM_SLICES_BUFFERS; i++) {
                if (slices_buffers[i]) {
                    free(slices_buffers[i]);
                }
            }
        }

        VdecRK(VideoCodec codec, std::shared_ptr<Renderer> rdr, std::shared_ptr<DrmDevice> dev_, volatile bool* signal_stop) :
            should_stop(signal_stop), renderer(rdr), dev(dev_),
            first_successful_decode(false), last_decoded_frame_time_ms(0), last_idr_request_ms(0) {
            int ret;
            memset(&mpi, 0, sizeof(mpi));   // frm_grp/frame_to_drm must be zero before first init_buffer
            MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
            if(codec==VideoCodec::H264) {
                mpp_type = MPP_VIDEO_CodingAVC;
            }
            ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
            assert(!ret);

            ret = mpp_create(&mpi.ctx, &mpi.mpi);
            assert(!ret);
            set_mpp_decoding_parameters();
            ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
            assert(!ret);
            set_mpp_decoding_parameters();

            // blocked/wait read of frame in thread
            int param = MPP_POLL_BLOCK;
            ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
            assert(!ret);

            for (int i = 0; i < NUM_SLICES_BUFFERS; i++) {
                slices_buffers[i] = (uint8_t*)malloc(READ_BUF_SIZE);
                assert(slices_buffers[i]);
            }

            uint8_t* nal_buffer = (uint8_t*)malloc(1024 * 1024);
            assert(nal_buffer);
            ret = mpp_packet_init(&packet, nal_buffer, READ_BUF_SIZE);
            assert(!ret);
        }
};

#endif