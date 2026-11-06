#pragma once
#include "dvr_library.hpp"
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
    };
    struct State {
        bool busy=false,playing=false,paused=false;
        std::string file,error;
        uint64_t position_us=0,duration_us=0,revision=0;
        std::vector<dvr_lib::Recording> recordings;
    };
    StreamPlayer(std::string directory,Output output,int width=1920,int height=1080);
    ~StreamPlayer();
    void play(const std::string& name);
    void stop();
    void pause();
    State state() const;
private:
    void run();
    void play_file(const std::string&,uint64_t generation);
    std::string directory_;Output output_;int width_,height_;
    mutable std::mutex mutex_;std::condition_variable wake_;
    State state_;std::string requested_;
    std::atomic<bool> quit_{false},paused_{false};
    std::atomic<uint64_t> generation_{0};
    std::thread worker_;
};
}
