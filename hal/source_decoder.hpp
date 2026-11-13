#pragma once
#include "hal/vdec.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <vector>
namespace hal {
// One hardware decoding session, switched on its own worker. The radio keeps
// receiving/recording while playback owns it. Switching never joins on the UI.
class SourceDecoder : public Vdec {
public:
    using Factory=std::function<std::shared_ptr<Vdec>()>;
    SourceDecoder(Factory factory,std::atomic<bool>* stop) : factory_(std::move(factory)),stop_(stop) {}
    void playback(bool on);
    void restart_playback();
    void pause_input(bool on) override;
    bool playback_ready() const;
    bool playback_failed() const {return playback_failed_;}
    void feed_playback(void* data,int len,int64_t pts,uint64_t recv,uint8_t nal,bool key=false);
    void feed_packet_to_decoder(void*,int,int64_t,uint64_t,uint8_t,uint32_t,uint32_t,bool=false) override;
    // Temporarily yield the hardware to an offline export. HDMI/UI stay alive.
    void suspend(bool on);
    bool suspended() const;
    // Strips decoded by decoders of their own (hal/artosyn/wave5_strips),
    // which need the live decoder's hardware and memory: no live decoder
    // while on (a playback still runs). live_released: none left.
    void hold_live(bool on);
    bool live_released() const;
    void run_frame() override;
    void cleanup() override;
    bool keyframe_in() const override;
    bool failed() const override {return failed_;}
    uint64_t last_frame_us() const override;
private:
    Factory factory_;std::atomic<bool>* stop_;
    mutable std::mutex mutex_;
    std::shared_ptr<Vdec> active_;
    std::array<std::vector<uint8_t>,3> live_parameters_;
    bool suspended_=false,live_held_=false;
    bool wanted_=false,playing_=false,paused_=false;
    uint64_t generation_=0,active_generation_=0;
    std::atomic<bool> quit_{false},failed_{false},playback_failed_{false};
};
}
