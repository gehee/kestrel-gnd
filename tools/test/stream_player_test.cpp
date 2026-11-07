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
        auto sample=std::make_shared<media::TelemetrySample>();sample->values["battery_v"]=16.4;
        recorder.set_telemetry(sample);
        for(size_t i=0;i<recording_frames.size();++i)recorder.feed(std::make_shared<std::vector<uint8_t>>(recording_frames[i]),1000000+i*16667);
        wait([&]{return recorder.state().frames==8;});name=std::filesystem::path(recorder.state().file).filename();
        recorder.stop();wait([&]{return !recorder.state().recording && !recorder.state().finishing;});
    }
    std::atomic<bool> selected{false},failed{false};std::atomic<int> pictures{0},parameters{0};
    std::vector<uint64_t> timestamps;std::mutex mutex;dvr::StreamPlayer* active=nullptr;
    dvr::StreamPlayer::Output output{
        [&](bool p){selected=p;},[&]{return selected.load();},[&]{return failed.load();},
        [&](const std::vector<uint8_t>& p,uint64_t ts,uint8_t nal,bool key) {
            check(selected,"Only playback source accepts playback packets");
            check(p.size()>6 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1,"Playback packets are Annex B");
            if(nal>=32)++parameters;
            else {if(active){auto sample=active->telemetry_at(ts);check(sample && sample->values.at("battery_v")==16.4,"Recorded telemetry accompanies video PTS");}
                std::lock_guard<std::mutex> lock(mutex);timestamps.push_back(ts);
                check(p==recording_frames[pictures%recording_frames.size()],"Indexed sample offset preserves encoded picture bytes");
                if(pictures++==0)check(key,"First picture is random access");}
        }};
    {
        dvr::StreamPlayer player(path,output,64,64);active=&player;
        player.play(name);wait([&]{return !player.state().busy;});
        check(player.state().error.empty(),"Valid recording plays without error");
        check(!selected && pictures==8 && parameters==3,"Playback sends eight pictures and all parameter sets then returns live");
        check(timestamps.front()==0,"Playback PTS are relative to the MP4 timeline");
        check(timestamps.back()-timestamps.front()>116000 && timestamps.back()-timestamps.front()<117000,"Original capture pacing retained");
        pictures=0;player.play(name);player.pause();
        wait([&]{return pictures>0;});
        check(player.state().paused,"Pausing during open still presents the first picture");
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
    active=nullptr;
    {
        dvr::StreamPlayer player(path,output);player.play(name);wait([&]{return !player.state().busy;});
        check(player.state().error.find("dimensions")!=std::string::npos && !selected,"Unsupported geometry rejected before source switch");
    }
    {
        // Repeated independently decodable GOPs with changing recorded values.
        dvr::StreamRecorder recorder(path,VideoCodec::H265,64,64,60);
        recorder.set_parameter_sets(recording_ps);recorder.toggle();wait([&]{return recorder.is_recording();});
        for(unsigned group=0;group<120;++group) {
            auto sample=std::make_shared<media::TelemetrySample>();sample->values["group"]=group;
            recorder.set_telemetry(sample);
            for(unsigned j=0;j<8;++j)recorder.feed(std::make_shared<std::vector<uint8_t>>(recording_frames[j]),1000000+(group*8+j)*16667);
            wait([&]{return recorder.state().frames==(group+1)*8;});
        }
        const auto seek_name=std::filesystem::path(recorder.state().file).filename().string();
        recorder.stop();wait([&]{return !recorder.state().recording && !recorder.state().finishing;});
        std::atomic<uint64_t> floor{0},visible{0},decoded{0};std::atomic<unsigned> resets{0};
        std::atomic<bool> first{false},selected{false};
        dvr::StreamPlayer* seeking=nullptr;
        dvr::StreamPlayer player(path,{
            [&](bool on){selected=on;},[&]{return selected.load();},[]{return false;},
            [&](const std::vector<uint8_t>&,uint64_t pts,uint8_t nal,bool key){
                if(nal>=32)return;
                decoded=pts+1;
                if(first.exchange(false))check(key,"Seek starts from a random access frame");
                if(pts>=floor) {
                    const auto sample=seeking->telemetry_at(pts);
                    check(sample && sample->values.at("group")==double((pts*60/1000000+1)/8),"Seek HUD matches target video time");
                    visible=pts;
                }
            },{}, {},[&](uint64_t target){floor=target;decoded=0;first=true;++resets;},[&]{return decoded.load();}
        },64,64);seeking=&player;
        player.play(seek_name);wait([&]{return player.state().playing;});player.pause();
        player.seek_relative(5,0);
        wait([&]{return !player.state().seeking && player.state().position_us>=4900000;});
        check(player.state().paused && visible>5000000 && visible<5140000,"Paused forward seek snaps to the following keyframe");
        const auto held=visible.load();std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(visible==held,"Seek preserves a paused picture");
        player.seek_relative(-5,held);wait([&]{return !player.state().seeking;});
        check(visible==0 && player.state().paused,"Backward seek restores beginning and recorded telemetry");
        player.seek_relative(5,0);player.seek_relative(5,0);
        wait([&]{return !player.state().seeking;});
        check(visible>10000000 && visible<10200000,"Rapid requests accumulate from pending keyframe");
        const auto before_end=visible.load();const auto before_resets=resets.load();
        player.seek_relative(100,visible);wait([&]{return !player.state().seeking;});
        check(player.state().busy && player.state().paused && visible==before_end && resets==before_resets,"No later keyframe preserves paused picture without resetting");
        player.seek_relative(-100,visible);wait([&]{return !player.state().seeking;});
        check(visible==0 && resets>=4,"Backward boundary clamps to zero");
        player.pause();wait([&]{return visible>100000;});
        player.seek_relative(5,visible);wait([&]{return !player.state().seeking;});
        check(!player.state().paused && visible>5000000,"Playing seek stays playing");
        player.seek_relative(5,visible);player.stop();wait([&]{return !selected;});
        check(!player.state().busy && !player.state().seeking,"Stop cancels an in-flight seek");
        std::filesystem::create_directory(std::string(path)+"/.thumbs");
        const auto preview=std::string(path)+"/.thumbs/"+seek_name+".jpg";
        std::ofstream(preview)<<"preview";
        const auto clip=std::string(path)+"/"+seek_name;
        check(std::filesystem::exists(clip+".telemetry.jsonl"),"Deletion fixture has telemetry");
        player.play(seek_name);
        bool busy_rejected=false;try{player.erase(seek_name);}catch(const std::runtime_error&){busy_rejected=true;}
        check(busy_rejected,"Deletion rejects an active player");
        player.stop();wait([&]{return !selected;});
        player.erase(seek_name);wait([&]{return player.state().deletion_revision==1;});
        check(!std::filesystem::exists(clip) && !std::filesystem::exists(preview) &&
              !std::filesystem::exists(clip+".telemetry.jsonl"),"Deletion removes video and companion files");
        check(!player.state().deleting && player.state().deletion_message=="Recording deleted","Worker reports deletion completion");
        const auto catalog=player.state().recordings;
        check(std::none_of(catalog.begin(),catalog.end(),[&](const auto& r){return r.name==seek_name;}),"Completed deletion refreshes catalog");
        player.erase(seek_name);wait([&]{return player.state().deletion_revision==2;});
        check(player.state().deletion_message.find("Delete failed:")==0,"Missing file reports deletion error");
        bool path_rejected=false;try{player.erase("../escape.mp4");}catch(const std::invalid_argument&){path_rejected=true;}
        check(path_rejected,"Deletion rejects path traversal");
    }
    {
        // Sparse keyframes with a fake decoder: seeking must submit only the
        // destination keyframe, never the intervening dependent pictures.
        // This tests scheduling, not the validity of the repeated P-picture payload.
        dvr::StreamRecorder recorder(path,VideoCodec::H265,64,64,60);
        recorder.set_parameter_sets(recording_ps);recorder.toggle();wait([&]{return recorder.is_recording();});
        for(unsigned i=0;i<900;++i) {
            if(i%30==0) {
                auto sample=std::make_shared<media::TelemetrySample>();sample->values["group"]=i/30;
                recorder.set_telemetry(sample);
            }
            recorder.feed(std::make_shared<std::vector<uint8_t>>(recording_frames[i%480?1:0]),1000000+i*16667);
            if(i%30==29) {wait([&]{const auto state=recorder.state();if(!state.error.empty())throw std::runtime_error(state.error);return state.frames+state.dropped==i+1;});check(recorder.state().dropped==0,"Long GOP fixture has no rejected NALs");}
        }
        const auto long_name=std::filesystem::path(recorder.state().file).filename().string();
        recorder.stop();wait([&]{return !recorder.state().recording && !recorder.state().finishing;});
        std::atomic<bool> selected{false},expect_key{true};
        std::atomic<uint64_t> floor{0},visible{0},decoded{0};
        std::atomic<unsigned> resets{0},pictures{0};
        uint64_t last=0;
        dvr::StreamPlayer* continuing=nullptr;
        dvr::StreamPlayer player(path,{
            [&](bool on){selected=on;},[&]{return selected.load();},[]{return false;},
            [&](const std::vector<uint8_t>&,uint64_t pts,uint8_t nal,bool key){
                if(nal>=32)return;
                if(expect_key.exchange(false))check(key,"Reset requires random access");
                else check(pts>last,"Continuing seek never duplicates or reverses submitted frames");
                last=pts;++pictures;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(pts>=floor) {
                    const auto sample=continuing->telemetry_at(pts);
                    check(sample && sample->values.at("group")==double((pts*60/1000000+1)/30),
                          "Continuing seek HUD matches the exact visible picture");
                    visible=pts;
                }
                decoded=pts+1;
            },{},{},[&](uint64_t target){floor=target;decoded=0;expect_key=true;++resets;},
            [&]{return decoded.load();}
        },64,64);continuing=&player;
        player.play(long_name);wait([&]{return pictures>0;});player.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto before=pictures.load();
        player.seek_relative(5,0);wait([&]{return !player.state().seeking;});
        check(player.state().error.empty() && resets==1 && visible==8000000 && pictures==before+1,
              "Sparse forward seek decodes only the next keyframe");
        check(player.state().paused,"Keyframe jump preserves pause");
        check(player.state().present_from_us==visible && player.state().seek_target_us==visible,
              "HUD floor and timeline use the actual keyframe destination");
        player.seek_relative(5,visible);wait([&]{return !player.state().seeking;});
        check(resets==1 && visible==8000000 && pictures==before+1,"No later keyframe is a no-op");
        player.seek_relative(-5,visible);wait([&]{return !player.state().seeking;});
        check(resets==2 && visible==0 && pictures==before+2,"Backward jump decodes only preceding keyframe");
        check(player.state().present_from_us==0,"Backward seek restores HUD presentation floor");
        player.seek_relative(-5,0);check(!player.state().seeking && resets==2,"Left at the first keyframe is a no-op");
        player.seek_relative(5,0);player.seek_relative(-5,0);
        wait([&]{return !player.state().seeking;});
        check(visible==0 && player.state().error.empty(),"Reversing a pending jump restores the previous keyframe");
        player.seek_relative(5,0);player.stop();
        wait([&]{return !selected;});check(!player.state().seeking,"Stop cancels keyframe jump");
    }
    puts("PASS: MP4 playback, pacing, pause/resume, cancel, invalid files, decoder errors and recovery");return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
