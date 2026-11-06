#include "hal/source_decoder.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstdio>
namespace {
void check(bool v,const char* s) {if(!v)throw std::runtime_error(s);}
template<class F> void wait(F f) {for(int i=0;i<2000;++i) {if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("Decoder timeout");}
struct Fake : Vdec {
    std::atomic<bool> quit{false},error{false},paused{false};
    std::atomic<unsigned> packets{0};
    std::atomic<uint64_t> frame_stamp{0};
    void feed_packet_to_decoder(void*,int,int64_t,uint64_t,uint8_t,uint32_t,uint32_t,bool) override {++packets;}
    void cleanup() override {quit=true;}
    void run_frame() override {while(!quit && !error)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    bool failed() const override {return error;}
    uint64_t last_frame_us() const override {return frame_stamp;}
    void pause_input(bool on) override {paused=on;}
};
}
int main() {
 try {
    volatile bool stop=false;std::mutex mutex;std::vector<std::shared_ptr<Fake>> sessions;
    std::atomic<bool> throw_next{false};
    hal::SourceDecoder decoder([&]() -> std::shared_ptr<Vdec> {
        if(throw_next.exchange(false))throw std::runtime_error("Test factory failure");
        std::lock_guard<std::mutex> lock(mutex);
        if(!sessions.empty())check(sessions.back()->quit || sessions.back()->error,"Old decoder must be stopped before creating the next");
        auto f=std::make_shared<Fake>();sessions.push_back(f);return f;
    },&stop);
    auto count=[&] {std::lock_guard<std::mutex> lock(mutex);return sessions.size();};
    auto session=[&](size_t i) {std::lock_guard<std::mutex> lock(mutex);return sessions.at(i);};
    struct Worker {hal::SourceDecoder& d;std::thread t;~Worker(){d.cleanup();t.join();}} worker{decoder,std::thread([&]{decoder.run_frame();})};
    uint8_t packet[6]={0,0,0,1,64,1};
    // Parameter sets can arrive before the decoder worker is ready.
    for(int nal=32;nal<=34;++nal)decoder.feed_packet_to_decoder(packet,6,0,0,nal,0,0);
    wait([&]{return count()==1 && session(0)->packets==3;});
    decoder.playback(true);wait([&]{return decoder.playback_ready();});
    check(count()==2,"Playback creates one new decoder");
    decoder.feed_packet_to_decoder(packet,6,1,1,1,0,0);
    check(session(1)->packets==0,"Live stream must not enter playback decoder");
    decoder.feed_playback(packet,6,1,1,1);check(session(1)->packets==1,"Playback reaches its decoder");
    decoder.pause_input(true);check(session(1)->paused,"Playback pause suspends its watchdog");
    decoder.playback(false);wait([&]{return count()==3 && session(2)->packets==3;});
    decoder.feed_playback(packet,6,1,1,1);check(session(2)->packets==3,"Playback must not enter live decoder");
    check(!session(2)->paused,"Returning live restores the watchdog");
    decoder.feed_packet_to_decoder(packet,6,1,1,1,0,0);check(session(2)->packets==4,"Live resumes with cached parameter sets");
    decoder.playback(true);wait([&]{return decoder.playback_ready();});session(3)->error=true;
    wait([&]{return decoder.playback_failed() && count()==5;});check(!decoder.failed(),"Playback failure recovers live reception");
    throw_next=true;decoder.playback(true);wait([&]{return decoder.playback_failed() && count()==6;});
    check(!decoder.failed(),"Playback factory failure recovers live reception");
    session(5)->error=true;wait([&]{return count()==7 && session(6)->packets==3;});
    check(!decoder.failed(),"Live failure recreates only the decoder with cached parameters");
    session(6)->frame_stamp=123;session(6)->error=true;
    wait([&]{return count()==8 && session(7)->packets==3;});
    check(!decoder.failed(),"Decoded progress permits recovery from a later stall");
    session(7)->error=true;wait([&]{return decoder.failed();});
    check(count()==8,"Repeated failures without a decoded picture are bounded");
    puts("PASS: exclusive live/playback decoder ownership, parameter cache, failure recovery");return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
