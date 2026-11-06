#pragma once
#include "recording_target.hpp"
#include "telemetry_track.hpp"
#include "base/video_codec.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace dvr {
// Portable stream-only recorder. Control, muxing and storage run on workers;
// neither a menu key nor the radio thread waits for filesystem I/O.
class StreamRecorder : public RecordingTarget {
public:
    struct State {
        bool recording=false, finishing=false;
        std::string file,error,telemetry_error;
        uint64_t frames=0, dropped=0;
        uint64_t free_bytes=0;
        bool storage_ready=false;
    };
    StreamRecorder(std::string directory,VideoCodec codec,int width,int height,int fps);
    ~StreamRecorder() override;
    void toggle();
    void stop();
    State state() const;
    void set_telemetry(std::shared_ptr<const media::TelemetrySample> sample);
    bool is_recording() const override {return requested_.load();}
    uint64_t session() const override {return session_.load();}
    void set_parameter_sets(const std::vector<std::vector<uint8_t>>&) override;
    void feed(std::shared_ptr<std::vector<uint8_t>>,uint64_t us) override;
    void feed(const bus::PictureRef&,uint64_t us) override;
private:
    struct Item {
        enum Kind {Start,Stop,Params,Frame} kind=Frame;
        std::shared_ptr<std::vector<uint8_t>> data;
        bus::PictureRef picture;
        std::vector<std::vector<uint8_t>> params;
        uint64_t us=0;
        std::shared_ptr<const media::TelemetrySample> telemetry;
        size_t size() const {return picture?picture->len:data?data->size():0;}
        const uint8_t* bytes() const {return picture?picture->data():data->data();}
    };
    void enqueue(Item item);
    void run();
    const std::string directory_;
    const VideoCodec codec_;
    const int width_,height_,fps_;
    std::atomic<bool> requested_{false};
    std::atomic<uint64_t> session_{0};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    size_t queued_=0;
    bool shutdown_=false;
    State state_;
    TelemetryWriter telemetry_;
    std::shared_ptr<const media::TelemetrySample> latest_telemetry_;
    std::thread worker_;
};
}
