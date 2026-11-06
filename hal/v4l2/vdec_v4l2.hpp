#pragma once
#include "hal/vdec.hpp"
#include "base/video_codec.hpp"
#include <deque>
#include <array>
#include <string>
#include <vector>

// Stateful V4L2 decoder. Only run_frame() touches the device; the radio
// submits complete access units into a bounded queue and never waits for it.
class VdecV4l2 final : public Vdec {
  public:
    VdecV4l2(VideoCodec codec, std::string device, std::shared_ptr<hal::FrameSink> sink, volatile bool *stop);
    ~VdecV4l2();
    void feed_packet_to_decoder(void *, int, int64_t, uint64_t, uint8_t, uint32_t, uint32_t,
                                bool = false) override;
    void run_frame() override;
    void cleanup() override;
    bool keyframe_in() const override { return ready_.load(); }
    uint64_t last_frame_us() const override { return last_frame_us_.load(); }
    uint64_t frames() const { return frames_.load(); }
    uint64_t dropped() const { return dropped_.load(); }
    bool failed() const override { return failed_.load(); }
    void pause_input(bool on) override { paused_=on; }

  private:
    struct Packet {
        std::vector<uint8_t> bytes;
        int64_t pts = 0;
        timing_stats timing{};
    };
    void decode();
    VideoCodec codec_;
    std::string device_;
    std::shared_ptr<hal::FrameSink> sink_;
    volatile bool *stop_;
    std::mutex mutex_;
    std::deque<Packet> packets_;
    std::array<std::vector<uint8_t>, 3> parameters_;
    std::atomic<bool> ready_{false}, failed_{false}, quit_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> frames_{0}, dropped_{0}, last_frame_us_{0};
    std::atomic<uint64_t> overflows_{0}, capture_errors_{0}, peak_pending_{0}, peak_wait_us_{0};
};
