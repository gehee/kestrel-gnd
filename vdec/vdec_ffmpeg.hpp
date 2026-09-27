#ifndef VDEC_FFMPEG_H  // Check if VDEC_H is not defined
#define VDEC_FFMPEG_H  // Define VDEC_H


#include <memory>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/hwcontext_drm.h>
#ifdef USE_VAAPI
    #include <libavutil/hwcontext_vaapi.h>
#endif
    #include <libavutil/pixdesc.h>
    #include <libavutil/frame.h>  // for AVFrame
}

#include "vdec.hpp"
#include "../renderer.hpp"
#include "../common.hpp"
#include "../utils/scheduling_helper.hpp"
#include "../utils/time_util.h"
#include "../utils/print_utils.hpp"

#define FRAME_RING_SIZE 20

class VdecFfmpeg : public Vdec {
    private:
        volatile bool *should_stop;
        std::shared_ptr<Renderer> renderer;

        // AVCodec
        const AVCodec *codec;
        AVCodecContext *av_ctx= NULL;
        AVBufferRef *hw_device_ctx = NULL;
        //VADisplay va_display = NULL;
        AVCodecParserContext *av_parser = NULL;
        //AVFrame *frame;
        int consecutive_errors = 0;
        int frame_count = 0;
        bool waiting_for_recovery_point = false;
        long long packets_input = 0;
        long long frames_output = 0;
        
        bool first_successful_decode = false;
        uint64_t last_idr_request_ms = 0;
        uint64_t last_successful_decode_ms = 0;  // Track last clean decode for time-based recovery

    // DRM PRIME/EGL state
        int frame_ring_cur=0;
        AVFrame* frame_ring[FRAME_RING_SIZE];
        AVPacket *pkt;
        bool drm_initialized = false;
        bool use_vaapi = false;
        bool use_qsv = false;


#ifdef USE_VAAPI
        std::unordered_map<VASurfaceID, DecodedUnit> exported_surfaces;
#endif
        // std::mutex fb_cache_mtx;
        
        // --- Shared resources ---
        std::mutex mtx;
        
        bool first_frame_sent = false;
        std::condition_variable cv_packet_ready; // Signaled by sender when packet is sent (or EOF)
        std::condition_variable cv_frame_ready;  // Signaled by receiver when frame is consumed (space for packet)

        // Flags to manage state
        std::atomic<bool> decoder_finished{false};    // True when receiver got AVERROR_EOF
        std::atomic<bool> packet_sender_waiting{false};// True if sender is waiting for receiver
        std::atomic<bool> frame_receiver_waiting{false};// True if receiver is waiting for sender


    private:

        AVFrame* convert_to_nv12(AVFrame* decoded_yuv420p_frame);
        void populate_drm(AVFrame* frame, DecodedUnit* du);
#ifdef USE_VAAPI
        bool populate_drm_from_vaapi(AVFrame* frame, DecodedUnit* du);
#endif
        void process_avframe(AVFrame* frame, uint64_t dec_start_ts, uint64_t dec_end_ts, uint64_t recvts, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_keyframe);
        void run_frame();
        void receive_avframe();
        AVFrame* next_frame_slot();

    public:
        VdecFfmpeg(VideoCodec cdc, std::string decoder_name, std::shared_ptr<Renderer> rdr, volatile bool* signal_stop) : 
            should_stop(signal_stop), renderer(rdr)  {
    
            // av_log_set_level(AV_LOG_TRACE); // Or AV_LOG_TRACE for maximum detail

            pkt = av_packet_alloc();
            if (!pkt)
                exit(1);

            if (decoder_name.empty()) {
                codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
            } else if (decoder_name == "vaapi") {
#ifdef USE_VAAPI
                codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
                use_vaapi = true;
#else
                fprintf(stderr, "VAAPI requested but not supported in this build\n");
                exit(1);
#endif
            } else {
                codec = avcodec_find_decoder_by_name(decoder_name.c_str());
                if (decoder_name.find("qsv") != std::string::npos) {
                    use_qsv = true;
                }
            }
            if (!codec) {
                fprintf(stderr, "Codec not found\n");
                exit(1);
            }
            printf("Using decoder %s:%s\n", codec->name,  codec->long_name);

            av_parser = av_parser_init(codec->id);
            if (!av_parser) {
                fprintf(stderr, "parser not found\n");
                exit(1);
            }

            for (int i = 0; i < FRAME_RING_SIZE; ++i) {
                frame_ring[i] = av_frame_alloc();
                if (!frame_ring[i]) {
                    printf("Error allocating frame ring\n");
                    exit(1);
                }
            }

            if (!init_decoder(NULL, 0)) {
                if (use_qsv) {
                    printf("QSV Init failed. Falling back to VAAPI...\n");
                    use_qsv = false;
                    use_vaapi = true;
                    // Switch to generic HEVC decoder for VAAPI hwaccel
                    codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
                    if (!codec) {
                        fprintf(stderr, "HEVC codec not found for fallback\n");
                        exit(1);
                    }
                     // Retry init
                    if (!init_decoder(NULL, 0)) {
                        fprintf(stderr, "Fallback to VAAPI also failed.\n");
                        exit(1);
                    }
                } else {
                     fprintf(stderr, "Decoder initialization failed.\n");
                     exit(1);
                }
            }
            printf("Vdec created\n");
        };

        ~VdecFfmpeg() {
            for (int i = 0; i < FRAME_RING_SIZE; ++i) {
                if (frame_ring[i]) { // Ensure it's not nullptr (though it should be non-null if allocated successfully)
                    av_frame_free(&frame_ring[i]); // Pass address of the pointer in the ring buffer
                }
            }
            av_packet_free(&pkt);
            av_parser_close(av_parser);
            av_ctx = NULL;
            av_parser = NULL;
        }; 

        bool init_decoder(void* data_p,int data_len);
        void feed_packet_to_decoder(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_key_override = false) override;
        void update_decoding_stats(uint64_t feed_data_ts);
        void cleanup();
        void cleanup_device();
        
};

#endif // VDEC_FFMPEG_H  // End of the header guard