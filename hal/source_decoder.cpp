#include "source_decoder.hpp"
#include <cstdio>
namespace hal {
void SourceDecoder::playback(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(wanted_==on)return;
    wanted_=on;paused_=false;++generation_;playback_failed_=false;
    if(active_)active_->cleanup();
}
void SourceDecoder::pause_input(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);paused_=on;
    if(active_ && wanted_ && playing_ && active_generation_==generation_)active_->pause_input(on);
}
bool SourceDecoder::playback_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && wanted_ && playing_ && active_generation_==generation_;
}
void SourceDecoder::feed_playback(void* data,int len,int64_t pts,uint64_t recv,uint8_t nal,bool key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(active_ && wanted_ && playing_ && active_generation_==generation_)
        active_->feed_packet_to_decoder(data,len,pts,recv,nal,0,0,key);
}
void SourceDecoder::feed_packet_to_decoder(void* data,int len,int64_t pts,uint64_t recv,uint8_t nal,uint32_t enc,uint32_t proc,bool key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(nal>=32 && nal<=34 && len>0 && len<=65536)
        live_parameters_[nal-32].assign(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+len);
    if(active_ && !wanted_ && !playing_ && active_generation_==generation_) {
        active_->set_next_slice_times(std::move(next_slice_times_));
        active_->feed_packet_to_decoder(data,len,pts,recv,nal,enc,proc,key);
    }
}
bool SourceDecoder::keyframe_in() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // A player is not a live-stream loss and must not trigger IDR requests.
    return wanted_ || (active_ && !playing_ && active_generation_==generation_ && active_->keyframe_in());
}
uint64_t SourceDecoder::last_frame_us() const {
    std::lock_guard<std::mutex> lock(mutex_);return active_?active_->last_frame_us():0;
}
void SourceDecoder::cleanup() {
    quit_=true;std::lock_guard<std::mutex> lock(mutex_);if(active_)active_->cleanup();
}
void SourceDecoder::run_frame() {
    while(!quit_ && !*stop_) {
        bool session_playback;
        uint64_t session_generation;
        {std::lock_guard<std::mutex> lock(mutex_);session_playback=wanted_;session_generation=generation_;}
        try {
            auto next=factory_();
            {std::lock_guard<std::mutex> lock(mutex_);
             if(quit_ || *stop_)break;
             if(session_generation!=generation_)continue;
             active_=next;playing_=session_playback;active_generation_=session_generation;
             next->pause_input(playing_ && paused_);
             if(!playing_) for(size_t i=0;i<live_parameters_.size();++i) {
                 auto& p=live_parameters_[i];if(!p.empty()) next->feed_packet_to_decoder(p.data(),int(p.size()),0,0,uint8_t(32+i),0,0);
             }}
            next->run_frame();
            {std::lock_guard<std::mutex> lock(mutex_);
             if(next->failed() && session_generation==generation_) {
                 if(!session_playback)failed_=true;
                 else {playback_failed_=true;wanted_=false;++generation_;}
             }
             active_.reset();}
            if(failed_)break;
        } catch(const std::exception& e) {
            fprintf(stderr,"decoder source: %s\n",e.what());
            std::lock_guard<std::mutex> lock(mutex_);active_.reset();
            if(session_generation!=generation_)continue;
            if(session_playback) {playback_failed_=true;wanted_=false;++generation_;}
            else {failed_=true;break;}
        }
    }
}
}
