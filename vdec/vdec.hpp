#ifndef VDEC_H  // Check if VDEC_H is not defined
#define VDEC_H  // Define VDEC_H

#include <cstdint>
#include <unordered_map>
#include <functional>
#include <mutex>

#include "../utils/slice_times.hpp"

struct timing_stats {
    uint64_t recv_start_us;
    uint64_t decode_start_us;
    uint32_t tx_encode_delay_us;
    uint32_t tx_processing_delay_us;
    bool is_keyframe;
    SliceTimesPtr slices = nullptr;   // each slice's own timing, where the source has it
};
using timing_stats_t = timing_stats;

class Vdec {
    protected:
        std::unordered_map<int64_t, timing_stats_t> decoding_stats;
        std::mutex decoding_stats_mutex;
        std::function<void()> idr_request_callback;
        bool idr_requested = false;
        SliceTimesPtr next_slice_times_;   // for the next picture fed (set_next_slice_times)
        
    public:
        // Feed a compressed packet (e.g., from RTP) to the decoder
        virtual void feed_packet_to_decoder(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t encode_delay_us, uint32_t processing_delay_us, bool is_key_override = false) = 0;

        // Streamed pictures: the decoder starts on a picture's first slice and
        // takes the rest as it arrives (stream_append), instead of waiting for
        // all of it. Only where the whole chain can (VdecRK with fpvOS's MPP
        // and kernel); elsewhere stream_supported() stays false and pictures
        // go whole through feed_packet_to_decoder.
        virtual bool stream_supported() const { return false; }
        // The first slice of a picture of `slices` slices; the same arguments
        // as feed_packet_to_decoder otherwise.
        virtual void stream_start(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t encode_delay_us, uint32_t processing_delay_us, int slices) {
            (void)slices;
            feed_packet_to_decoder(data_p, data_len, pts, recv_ts, nal_type, encode_delay_us, processing_delay_us);
        }
        // A later slice of picture `pts`, start code included; `last` for its
        // last. False if the decoder did not take it.
        virtual bool stream_append(const void* data, int len, int64_t pts, bool last) { (void)data; (void)len; (void)pts; (void)last; return false; }
        // Picture `pts` gets no more slices: decode it as far as it got.
        virtual void stream_end(int64_t pts) { (void)pts; }

        // While another decoder runs on the same hardware (a gallery clip, the
        // background video), live pictures go whole: a streamed one holds the
        // decoder between its slices. Counted by the other decoders' owners.
        static std::atomic<int>& stream_holds() { static std::atomic<int> n{0}; return n; }

        // The next picture fed (feed_packet_to_decoder or stream_start) has
        // these slices; the decoder stamps when each one's rows are decoded.
        void set_next_slice_times(SliceTimesPtr st) { next_slice_times_ = std::move(st); }

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