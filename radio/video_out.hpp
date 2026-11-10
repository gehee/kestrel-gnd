#pragma once
// The radio's video out, whatever radio: its H.265 NALs, as the radio splits
// them off the air, into pictures and to their consumers. The parameter sets
// checked and kept (for a recording started mid-stream); each slice into a
// picture built in a buffer of the video pool (base/video_pool), each slice
// written once; the decoder fed slice by slice as they land (streamed
// pictures, Vdec::stream_supported) or a picture at a time - a direct call,
// on the radio thread: slice-timed decoding waits on nothing - and each whole
// picture handed to the video sinks (the DVR). Where a picture's last slice
// starts is learned, so it is decoded as that slice arrives, not when the
// next picture's first does.
//
// What only the radio knows - when a picture was captured, by its clock, and
// how to ask the air unit for a keyframe - it says through its Source. Runs on
// the radio thread only.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/bus.hpp"
#include "base/video_codec.hpp"
#include "hal/vdec.hpp"
#include "base/slice_times.hpp"

namespace radio {

class Model;

class VideoOut {
public:
    // The capture of a picture about to be built, as the radio measured it.
    struct PictureTiming {
        bool measured = false;     // by the radio's clock
        bool air_unknown = false;  // no air figure at all (the radio clock not checked yet)
        uint32_t air_us = 0;       // capture -> its first slice here (measured, or the floor estimate)
        uint32_t enc_us = 0;       // ...of which capture -> the encoder handing it out (0: estimated)
        uint64_t cap_us = 0;       // its capture stamp, the air unit's clock (0: none) - the recording's time
        // Its slices' own times (base/slice_times.hpp), from frame_cap_us
        // (our clock; 0: not timed), the picture's height and the sensor's.
        uint64_t frame_cap_us = 0;
        uint32_t height = 0, readout_us = 0, exposure_us = 0;
    };
    // A picture handed to the decoder, for the radio's latency figures.
    struct PictureDone {
        bool measured = false;
        uint64_t first_recv_us = 0, last_recv_us = 0;
        uint32_t air_us = 0, enc_us = 0;
    };
    class Source {
    public:
        virtual ~Source() = default;
        virtual void picture_start(uint64_t recv_us, PictureTiming& t) = 0;
        virtual void picture_done(const PictureDone& d) = 0;
        // A keyframe asked of the air unit: why (logged), at most once in
        // 1.5 s; false when it was too soon.
        virtual bool ask_keyframe(const char* why) = 0;
        // ...and for a sink that needs one (a recording starts on one).
        virtual void request_keyframe() = 0;
    };
    // What the radio says ahead of a slice: the picture it belongs to (its
    // counter, -1: none), the slice's number in it (-1: not said) and whether
    // it is the picture's last, and when the encoder handed it out (our clock,
    // 0: unknown) - each taken once, by the slice that follows - and the
    // frame rate it runs at.
    struct SliceHeader {
        int hseq = -1;
        int idx = -1;
        bool last = false;
        bool ends = false;   // last, said by a kestrel-air unit (stock's byte 32 is not known to mean it)
        // The air unit's picture number, counted on (-1: not used) - a strip's
        // picture goes by it, so the strips of one capture find each other.
        int64_t pic = -1;
        // A strip's: the rows it codes past each of its joins (kestrel-air's
        // header byte 8), cut off again where the strips are joined.
        int strip_ov = 0;
        uint64_t out_us = 0;
        uint32_t fps = 0;
    };

    // tag: how its log lines start ("ar8030").
    VideoOut(const char* tag, VideoCodec codec, std::shared_ptr<Vdec> vdec, bool decode_enabled,
             bool stream_decode, Model& model, Source& src);
    VideoOut(const VideoOut&) = delete;
    VideoOut& operator=(const VideoOut&) = delete;

    // Before the radio runs.
    void add_sink(bus::VideoSink* s) {
        sinks_.push_back(s);
        if (s->wants_pictures()) picture_sinks_ = true;
    }
    // A sink records the stream as it comes right now (bus::VideoSink::needs_stream).
    bool sinks_need_stream() const {
        for (const bus::VideoSink* s : sinks_)
            if (s->needs_stream()) return true;
        return false;
    }

    // One NAL of video, no start code, its bytes in at recv_us. False: not
    // taken (held back, or not this picture's) - not counted as video.
    bool nal(const uint8_t* nal, size_t len, uint64_t recv_us, SliceHeader& hdr);
    // The end of one read of the radio: the streamed slices it brought go to
    // the decoder in one part.
    void end_of_read() { send_held_slices(); }
    // A new video mode (another size or rate): where pictures end, and the
    // sensor's readout, are learned again.
    void relearn();
    // The picture being built goes (a link lost or found again, the radio
    // stopping); one streamed to the decoder is ended there.
    void drop_picture() { drop_au(); }

    uint64_t frames_seen() const { return frames_seen_; }
    unsigned long long foreign_slices() const { return foreign_slices_; }
    uint32_t readout_us() const { return readout_us_; }
    const std::vector<std::vector<uint8_t>>& param_sets() const { return param_sets_; }

private:
    bool au_begin();                 // a buffer for a new picture
    bool au_append(const uint8_t* nal, size_t len, uint32_t* at);   // false: it does not fit
    void feed_au(const uint8_t* data, size_t len, int stream_slices);
    void drop_au();                  // the open picture goes, a streamed one ended in the decoder
    void flush_access_unit(uint64_t recv_us);
    void learn_last_slice(int max_addr, int slices);
    void late_slice();
    void note_slice(int addr, bool first_slice, uint64_t recv_us, SliceHeader& hdr);
    uint32_t predict_first_end(int hseq) const;
    void send_held_slices(bool last = false);

    const char* const tag_;
    const VideoCodec codec_;
    std::shared_ptr<Vdec> vdec_;
    const bool decode_enabled_;
    const bool stream_decode_;
    Model& model_;
    Source& src_;
    std::vector<bus::VideoSink*> sinks_;
    bool picture_sinks_ = false;     // a sink wants whole pictures

    int64_t frame_pts_ = 0;
    uint64_t frames_seen_ = 0;

    // The parameter sets: every distinct valid VPS/SPS/PPS (one per type and
    // id), for the sinks; what reading a slice's start address takes.
    std::vector<std::vector<uint8_t>> param_sets_;
    unsigned long long ps_dropped_ = 0;   // corrupt param-set NALs rejected
    uint16_t sps_ids_ok_ = 0;             // SPS ids a valid SPS has been seen for
    bool pps_ok_ = false;                 // a valid PPS has reached the decoder
    bool ps_since_pic_ = false;           // parameter sets came since the last picture (a GOP start)
    int ctb_addr_bits_ = 0;               // slice address width, from the SPS
    uint8_t pps_dep_slices_[64] = {0};    // dependent_slice_segments_enabled_flag per PPS
    uint32_t ctb_size_ = 0, ctbs_per_row_ = 0;

    // The picture being built.
    bus::PictureRef au_pic_;
    uint64_t au_no_buffer_ = 0;      // pictures lost: the pool had no buffer, or one too small
    int64_t au_pts_ = 0;
    uint8_t au_nal_type_ = 0;        // its key-ness (IDR vs trailing)
    bool au_open_ = false;
    int au_hseq_ = -1;               // the picture's counter, as the radio said it
    // Arrival of its first slice (the zero point for glass to glass on the
    // ground side) and of its newest (reassembly is the span between).
    uint64_t au_first_recv_us_ = 0, au_last_recv_us_ = 0;
    uint64_t au_cap_us_ = 0;         // its capture stamp, the air unit's clock (0: none)
    bool au_measured_ = false, au_air_unknown_ = false;
    uint32_t au_air_us_ = 0, au_enc_us_ = 0;
    SliceTimesPtr au_times_;         // its slices' own times
    int au_max_addr_ = -1;           // largest slice start in it
    int au_slice_count_ = 0;
    // Its slices by the numbers the radio gave them (bus::PictureBuf::whole).
    uint32_t au_idx_seen_ = 0;       // a bit per number
    int au_idx_last_ = -1;           // the one said to be the last (-1: none came)
    bool au_idx_unsaid_ = false;     // a slice came without a number
    bool au_streamed_ = false;       // it went to the decoder from its first slice
    bool au_stream_done_ = false;    // ...and its last slice followed

    // Where a picture's last slice starts, and how many slices it has:
    // learned (learn_last_slice).
    int last_slice_addr_ = -1;
    int last_addr_cand_ = -1, last_addr_streak_ = 0;
    int last_slice_count_ = 0, last_count_cand_ = 0;
    uint64_t stream_pics_ = 0, stream_ended_ = 0, stream_refused_ = 0;
    uint32_t late_slices_ = 0, late_window_pics_ = 0;
    uint64_t late_slices_total_ = 0;
    unsigned long long foreign_slices_ = 0;
    static constexpr int kLearnRun = 32;
    static constexpr uint32_t kLateMax = 4;
    bool ends_off_ = false;          // slices came after the air unit said a picture ended: not believed
    bool ended_by_mark_ = false;     // the last picture was ended by the air unit's mark
    uint32_t late_marked_ = 0;       // late slices after such pictures, this window
    bool stream_paused_ = false;     // /tmp/kestrel-stream-off, or the frame mode's whole pictures
    unsigned pause_check_ = 0;

    // Where a picture's first slice ends, before its second slice says so:
    // usually at the last slice's learned start; the air unit's intra
    // refresh cuts its sweep-start pictures after the refresh rows instead.
    // Learned: the period and phase of those cuts.
    int cut_last_hseq_ = -1;
    int cut_period_ = 0, cut_confirms_ = 0;
    uint32_t cut_row_ = 0;
    uint32_t pics_since_cut_ = 0;
    // How long the sensor takes to read a frame out, top row to bottom:
    // learned from how far apart the first and last slices leave the encoder.
    std::vector<uint32_t> readout_win_;
    uint32_t readout_us_ = 0;

    // A streamed picture's slices that came in one read go to the decoder as
    // one part, at the end of the read: its bytes held_off_..held_end_.
    uint32_t held_off_ = 0, held_end_ = 0;
    int64_t held_pts_ = -1;

    float rsm_ewma_ = -1.0f;         // reassembly, for the model's timing
};

}  // namespace radio
