#pragma once
// Strips: kestrel-air's pictures in strips (KA_SLICE_STRIP), each strip its
// own stream - its parameter sets, a picture of the strip's rows - so the
// top of a picture is on the radio while the camera still reads its bottom.
// Each strip into a VideoOut and a decoder of its own; the decoded strips
// joined into whole pictures again (hal::StripJoiner), which alone reach the
// screen. The radio's figures are kept per picture, as for one stream: the
// capture timed once, the arrival from its top strip's first byte to its
// bottom strip's last. Runs on the radio thread, as VideoOut.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <pthread.h>
#include "hal/strip_joiner.hpp"
#include "radio/video_out.hpp"
namespace radio {
class StripVideo {
public:
    static constexpr int kMax = 4;
    // A hardware decoder whose pictures go to sink, running until *stop.
    using MakeDecoder = std::function<std::unique_ptr<Vdec>(std::function<void(std::shared_ptr<DecodedUnit>)> sink,
                                                            std::atomic<bool>* stop)>;
    StripVideo(VideoCodec codec, bool decode_enabled, Model& model, VideoOut::Source& src, MakeDecoder make,
               std::unique_ptr<hal::StripJoiner> joiner);
    ~StripVideo();
    StripVideo(const StripVideo&) = delete;
    StripVideo& operator=(const StripVideo&) = delete;
    // One NAL of video, as VideoOut::nal takes it; hdr says which strip
    // (idx) of which picture, and whether it is the picture's last.
    bool nal(const uint8_t* nal, size_t len, uint64_t recv_us, VideoOut::SliceHeader& hdr);
    void end_of_read();
    void drop_picture();
    void relearn();
private:
    // What each strip's VideoOut tells the radio, kept per picture.
    class Src : public VideoOut::Source {
    public:
        Src(StripVideo& sv, int k) : sv_(sv), k_(k) {}
        void picture_start(uint64_t recv_us, VideoOut::PictureTiming& t) override { sv_.picture_start(k_, recv_us, t); }
        void picture_done(const VideoOut::PictureDone& d) override { sv_.picture_done(k_, d); }
        bool ask_keyframe(const char* why) override { return sv_.src_.ask_keyframe(why); }
        void request_keyframe() override { sv_.src_.request_keyframe(); }
    private:
        StripVideo& sv_;
        const int k_;
    };
    struct Strip {
        std::unique_ptr<Src> src;
        std::shared_ptr<Vdec> dec;
        std::unique_ptr<VideoOut> out;
        pthread_t thread = 0;
    };
    void picture_start(int k, uint64_t recv_us, VideoOut::PictureTiming& t);
    void picture_done(int k, const VideoOut::PictureDone& d);
    bool make_strip(int k);
    const VideoCodec codec_;
    const bool decode_enabled_;
    Model& model_;
    VideoOut::Source& src_;
    MakeDecoder make_;
    std::unique_ptr<hal::StripJoiner> joiner_;
    std::atomic<bool> stop_{false};
    Strip strip_[kMax];
    std::atomic<int> n_{0};          // strips a picture has (the last one's index + 1)
    std::atomic<int> ov_{0};         // rows each strip codes past each of its joins
    int64_t pic_ = -1;               // the picture number, counted on from the header's 16 bits
    // The picture's capture, timed for its first strip to arrive, for the rest.
    int64_t timed_pic_ = -1;
    uint64_t timed_recv_us_ = 0;
    VideoOut::PictureTiming timed_;
    // Its top strip's arrival, until the bottom strip's completes the picture.
    int64_t done_pic_ = -1;
    VideoOut::PictureDone done_;
};
}  // namespace radio
