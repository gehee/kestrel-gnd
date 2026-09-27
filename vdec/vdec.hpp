#ifndef VDEC_H  // Check if VDEC_H is not defined
#define VDEC_H  // Define VDEC_H

#include <cstdint>
#include <unordered_map>
#include <functional>
#include <mutex>

struct timing_stats {
    uint64_t recv_start_us;
    uint64_t decode_start_us;
    uint32_t tx_capture_delay_us;
    uint32_t tx_processing_delay_us;
    bool is_keyframe;
};
using timing_stats_t = timing_stats;

class Vdec {
    protected:
        std::unordered_map<int64_t, timing_stats_t> decoding_stats;
        std::mutex decoding_stats_mutex;
        std::function<void()> idr_request_callback;
        bool idr_requested = false;
        
    public:
        // Feed a compressed packet (e.g., from RTP) to the decoder
        virtual void feed_packet_to_decoder(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_key_override = false) = 0;

        // Clean up decoder resources
        virtual void cleanup() = 0;

        // Run a single frame decode step
        virtual void run_frame() = 0;

        // Set callback to request IDR from TX
        void set_idr_request_callback(std::function<void()> cb) {
            idr_request_callback = cb;
        }

        // Notify that IDR was requested externally
        void notify_idr_requested() {
            idr_requested = true;
        }

        static void* run_frame_thread(void* arg) {
            static_cast<Vdec*>(arg)->run_frame();
            return nullptr;
        }
};

#endif // VDEC_H  // End of the header guard