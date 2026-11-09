#pragma once
#include "system/recorded_telemetry.hpp"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
namespace dvr {
// Writes are bounded and independent of the video muxer. Overflow or storage
// failure disables this session's telemetry, never the video recording.
class TelemetryWriter {
public:
    TelemetryWriter();
    ~TelemetryWriter();
    void begin(const std::string& video);
    void append(uint64_t us,std::shared_ptr<const media::TelemetrySample>);
    void finish();
    std::string error() const;
    bool pending() const;
    void drain(); // tests/shutdown only; never on UI or radio threads
private:
    struct Job {enum Kind {Begin,Sample,End} kind;std::string path;uint64_t us=0;std::shared_ptr<const media::TelemetrySample> sample;};
    void run();
    mutable std::mutex mutex_;
    std::condition_variable wake_,drained_;
    std::deque<Job> jobs_;
    bool quit_=false,busy_=false,disabled_=false;
    std::string error_;
    std::thread worker_;
};
// Sequential reader with checkpoint offsets collected as it advances. Backward
// lookup restarts at the closest known full snapshot. Parsing stays on the player.
class TelemetryReader {
public:
    explicit TelemetryReader(const std::string& video);
    ~TelemetryReader();
    bool available() const {return file_!=nullptr && error_.empty();}
    const std::string& error() const {return error_;}
    std::shared_ptr<const media::TimedTelemetry> at(uint64_t us);
private:
    bool next();
    FILE* file_=nullptr;
    int64_t data_offset_=0;
    std::vector<std::pair<uint64_t,int64_t>> index_;
    std::shared_ptr<media::TimedTelemetry> current_,next_;
    std::string error_;
    uint64_t last_line_us_=0;
    bool have_line_=false,eof_=false;
};
}
