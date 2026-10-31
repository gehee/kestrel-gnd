#pragma once
// The recording of the camera's stream, as a consumer of the radio's video.
// On the radio thread it only queues each picture - a reference to its pool
// buffer, or a copy while the pool runs low - and wakes its own thread, which
// waits for the keyframe a recording starts on, repairs where the radio lost
// part of the stream, and hands the pictures to the DvrRecorder. The radio
// thread takes none of the recorder's locks and copies nothing else.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "base/bus.hpp"
#include "base/video_codec.hpp"
#include "dvr/hevc_repair.hpp"

class DvrSink : public bus::VideoSink {
public:
    // screen_rec: whether REC records the screen (DVR > Source SCREEN) - then
    // the screen recorder has the pictures, and this sink takes none.
    DvrSink(VideoCodec codec, std::function<bool()> screen_rec);
    ~DvrSink() override;
    DvrSink(const DvrSink&) = delete;
    DvrSink& operator=(const DvrSink&) = delete;

    // The radio thread.
    void on_picture(const bus::PictureRef& pic) override;
    void on_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) override;
    // A recording waiting for its keyframe: the radio asks the air unit.
    bool take_keyframe_request() override { return keyframe_wanted_.exchange(false); }

private:
    using ParamSets = std::shared_ptr<const std::vector<std::vector<uint8_t>>>;
    struct Item {
        enum Kind : uint8_t { Picture, Stopped, Params };
        Kind kind = Picture;
        bus::PictureRef pic;                          // the picture, in the pool...
        std::shared_ptr<std::vector<uint8_t>> copy;   // ...or a copy of it
        uint64_t pts = 0;
        int8_t whole = -1;                            // bus::PictureBuf::whole
        ParamSets ps;                                 // Params: the stream's new VPS/SPS/PPS
    };

    // ---- the radio thread ----
    bool post(Item&& it);            // false: the queue was full, it is dropped (and counted)
    bool rec_queued_ = false;        // pictures went to a recording since the last stop
    ParamSets ps_owed_;              // parameter sets a full queue had no room for: first in

    // ---- the sink's thread ----
    void run();
    void take(Item& it);
    bool has_keyframe(const uint8_t* au, size_t n) const;
    const VideoCodec codec_;
    std::function<bool()> screen_rec_;
    bool keyed_ = false;            // the recording running has had its keyframe
    bool rec_was_ = false;          // a recording was running at the last picture
    HevcRepair repair_;             // its lost pictures and slices replaced by copies
    uint64_t idr_asked_ms_ = 0;     // the last keyframe asked for it
    std::atomic<bool> keyframe_wanted_{false};   // asked of the radio, not taken yet

    bus::Ring<Item, 64> ring_;      // ~0.5 s at 120 pictures a second
    bus::Waker wake_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
