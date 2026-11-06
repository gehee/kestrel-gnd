#include "dvr/stream_player.hpp"
#include "dvr/stream_recorder.hpp"
#include "recording_fixture.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <sys/stat.h>
namespace {
void check(bool v,const char* s) {if(!v)throw std::runtime_error(s);}
template<class F> void wait(F f) {for(int i=0;i<2000;++i) {if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(2));}throw std::runtime_error("Player timeout");}
}
int main() {
 try {
    char path[]="stream-player-test-XXXXXX";check(mkdtemp(path),"Create test folder");
    struct Folder {std::string path;~Folder(){std::filesystem::remove_all(path);}} folder{path};
    std::string name;
    {
        dvr::StreamRecorder recorder(path,VideoCodec::H265,64,64,60);recorder.set_parameter_sets(recording_ps);
        recorder.toggle();wait([&]{return recorder.is_recording();});
        for(size_t i=0;i<recording_frames.size();++i)recorder.feed(std::make_shared<std::vector<uint8_t>>(recording_frames[i]),1000000+i*16667);
        wait([&]{return recorder.state().frames==8;});name=std::filesystem::path(recorder.state().file).filename();
        recorder.stop();wait([&]{return !recorder.state().recording && !recorder.state().finishing;});
    }
    std::atomic<bool> selected{false},failed{false};std::atomic<int> pictures{0},parameters{0};
    std::vector<uint64_t> timestamps;std::mutex mutex;
    dvr::StreamPlayer::Output output{
        [&](bool p){selected=p;},[&]{return selected.load();},[&]{return failed.load();},
        [&](const std::vector<uint8_t>& p,uint64_t ts,uint8_t nal,bool key) {
            check(selected,"Only playback source accepts playback packets");
            check(p.size()>6 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1,"Playback packets are Annex B");
            if(nal>=32)++parameters;
            else {std::lock_guard<std::mutex> lock(mutex);timestamps.push_back(ts);if(pictures++==0)check(key,"First picture is random access");}
        }};
    {
        dvr::StreamPlayer player(path,output,64,64);
        player.play(name);wait([&]{return !player.state().busy;});
        check(player.state().error.empty(),"Valid recording plays without error");
        check(!selected && pictures==8 && parameters==3,"Playback sends eight pictures and all parameter sets then returns live");
        check(timestamps.back()-timestamps.front()>116000 && timestamps.back()-timestamps.front()<117000,"Original capture pacing retained");
        pictures=0;player.play(name);wait([&]{return player.state().playing;});player.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));const auto paused=pictures.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));check(pictures==paused && player.state().paused,"Paused stream stops producing pictures");
        player.pause();wait([&]{return !player.state().busy;});check(pictures==8,"Resume completes the clip");
        player.play(name);player.stop();check(!player.state().busy,"Cancelling queued playback clears busy state");
        bool rejected=false;try{player.play("../escape.mp4");}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Traversal rejected");
        std::ofstream(std::string(path)+"/broken.mp4")<<"truncated MP4 file contents";
        player.play("broken.mp4");wait([&]{return !player.state().busy;});check(!player.state().error.empty() && !selected,"Bad file leaves live source selected");
        const auto pipe=std::string(path)+"/pipe.mp4";check(!mkfifo(pipe.c_str(),0600),"Create nonregular test file");
        player.play("pipe.mp4");wait([&]{return !player.state().busy;});check(!player.state().error.empty(),"Nonregular file rejected without blocking");
        std::filesystem::create_symlink(name,std::string(path)+"/link.mp4");
        player.play("link.mp4");wait([&]{return !player.state().busy;});check(!player.state().error.empty(),"Player refuses symlinks");
        pictures=0;failed=true;player.play(name);wait([&]{return !player.state().busy;});check(!player.state().error.empty() && !selected,"Decoder failure returns live with error");failed=false;
        pictures=0;player.play(name);wait([&]{return !player.state().busy;});check(player.state().error.empty() && pictures==8,"Playback recovers after failure");
    }
    {
        dvr::StreamPlayer player(path,output);player.play(name);wait([&]{return !player.state().busy;});
        check(player.state().error.find("dimensions")!=std::string::npos && !selected,"Unsupported geometry rejected before source switch");
    }
    puts("PASS: MP4 playback, pacing, pause/resume, cancel, invalid files, decoder errors and recovery");return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
