#include <atomic>
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
    std::atomic<bool> stop=false;std::mutex mutex;std::vector<std::shared_ptr<Fake>> sessions;
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
    decoder.restart_playback();wait([&]{return decoder.playback_ready() && count()==3;});
    check(session(1)->quit && session(2)->paused,"Seek replaces the decoder and preserves pause without switching live");
    decoder.feed_packet_to_decoder(packet,6,1,1,1,0,0);
    check(session(2)->packets==0,"Live packets remain excluded during seek");
    decoder.feed_playback(packet,6,5000000,1,19,true);
    check(session(2)->packets==1,"Seek keyframe reaches the replacement decoder");
    decoder.playback(false);wait([&]{return count()==4 && session(3)->packets==3;});
    decoder.feed_playback(packet,6,1,1,1);check(session(3)->packets==3,"Playback must not enter live decoder");
    check(!session(3)->paused,"Returning live restores the watchdog");
    decoder.feed_packet_to_decoder(packet,6,1,1,1,0,0);check(session(3)->packets==4,"Live resumes with cached parameter sets");
    decoder.suspend(true);wait([&]{return decoder.suspended();});
    check(count()==4 && session(3)->quit && decoder.keyframe_in(),"Export releases decoder and suppresses live IDR requests");
    decoder.feed_packet_to_decoder(packet,6,1,1,1,0,0);
    check(session(3)->packets==4,"Live packets are excluded while exporting");
    decoder.suspend(false);wait([&]{return count()==5 && session(4)->packets==3;});
    // Remove the export-only session from test numbering for the existing recovery cases.
    {std::lock_guard<std::mutex> lock(mutex);sessions.erase(sessions.begin()+3);}
    decoder.playback(true);wait([&]{return decoder.playback_ready();});session(4)->error=true;
    wait([&]{return decoder.playback_failed() && count()==6;});check(!decoder.failed(),"Playback failure recovers live reception");
    throw_next=true;decoder.playback(true);wait([&]{return decoder.playback_failed() && count()==7;});
    check(!decoder.failed(),"Playback factory failure recovers live reception");
    session(6)->error=true;wait([&]{return count()==8 && session(7)->packets==3;});
    check(!decoder.failed(),"Live failure recreates only the decoder with cached parameters");
    session(7)->frame_stamp=123;session(7)->error=true;
    wait([&]{return count()==9 && session(8)->packets==3;});
    check(!decoder.failed(),"Decoded progress permits recovery from a later stall");
    session(8)->error=true;wait([&]{return decoder.failed();});
    check(count()==9,"Repeated failures without a decoded picture are bounded");
    puts("PASS: exclusive live/playback decoder ownership, parameter cache, failure recovery");return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
