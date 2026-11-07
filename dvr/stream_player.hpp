#pragma once
#include "dvr_library.hpp"
#include "telemetry_track.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace dvr {
class StreamPlayer {
public:
    struct Output {
        std::function<void(bool)> select;
        std::function<bool()> ready,failed;
        std::function<void(const std::vector<uint8_t>&,uint64_t,uint8_t,bool)> feed;
        std::function<void(bool)> pause;
        std::function<void(const std::string&)> started;
        std::function<void(uint64_t)> seek; // reset decoder; suppress frames before this PTS
        std::function<uint64_t()> decoded; // last completed PTS + 1, zero after reset
        std::function<void(uint64_t)> advance; // suppress pre-roll without resetting the decoder
    };
    struct State {
        bool busy=false,playing=false,paused=false,seeking=false,deleting=false;
        uint64_t deletion_revision=0;
        std::string deletion_message;
        std::string file,error,telemetry_error;
        uint64_t started_us=0;
        uint64_t position_us=0,duration_us=0,revision=0,seek_target_us=0;
        uint64_t present_from_us=0; // exact seek frame; older scanout cannot supply playback HUD
        std::vector<dvr_lib::Recording> recordings;
    };
    StreamPlayer(std::string directory,Output output,int width=1920,int height=1080);
    ~StreamPlayer();
    void play(const std::string& name);
    void stop();
    void pause();
    void seek_relative(int seconds,uint64_t presented_us);
    void erase(const std::string& name);
    State state() const;
    std::shared_ptr<const media::TelemetrySample> telemetry_at(uint64_t position_us) const;
private:
    void run();
    void play_file(const std::string&,uint64_t generation);
    std::string directory_;Output output_;int width_,height_;
    mutable std::mutex mutex_;std::condition_variable wake_;
    State state_;std::string requested_,delete_requested_;
    std::atomic<bool> quit_{false},paused_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> seek_revision_{0};
    struct PresentedSample {uint64_t us;std::shared_ptr<const media::TimedTelemetry> data;};
    std::deque<PresentedSample> telemetry_samples_;
    std::thread worker_;
};
}
