#include "dvr/stream_recorder.hpp"
#include "dvr/dvr_sink.hpp"
#include "dvr/dvr_library.hpp"
#include "dvr/writer.hpp"
#include "third_party/minimp4.h"
#include "recording_fixture.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
template<class F> void wait(F f) {for(int i=0;i<2000;++i) {if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(2));}throw std::runtime_error("Recorder timeout");}
}
int main(int argc,char** argv) {
 try {
    char dir[]="stream-recorder-test-XXXXXX";check(mkdtemp(dir),"Create recording test folder");
    std::vector<std::string> files;
    {
        dvr::StreamRecorder rec(dir,VideoCodec::H265,64,64,60);
        rec.set_parameter_sets(recording_ps);
        for(int pass=0;pass<2;++pass) {
            rec.toggle();wait([&]{return rec.state().recording;});
            auto telemetry=std::make_shared<media::TelemetrySample>();telemetry->values["battery_v"]=16.4;
            rec.set_telemetry(telemetry);
            for(size_t i=0;i<recording_frames.size();++i)
                rec.feed(std::make_shared<std::vector<uint8_t>>(recording_frames[i]),1000000+i*16667);
            wait([&]{return rec.state().frames==recording_frames.size();});
            files.push_back(rec.state().file); rec.stop();
            wait([&]{const auto s=rec.state();return !s.recording && !s.finishing;});
            check(rec.state().error.empty(),"Successful recording has no error");
            FILE* file=fopen(files.back().c_str(),"rb");check(file,"Recorded file exists");
            fseek(file,0,SEEK_END);const auto size=ftell(file);
            MP4D_demux_t mp4{};
            check(MP4D_open(&mp4,[](int64_t off,void* buf,size_t n,void* ctx)->int {FILE* f=static_cast<FILE*>(ctx);return fseek(f,off,SEEK_SET)||fread(buf,1,n,f)!=n;},file,size),"Recorded MP4 can be demuxed");
            check(mp4.track_count==1 && mp4.track[0].sample_count==8,"All eight HEVC pictures retained");
            check(mp4.track[0].SampleDescription.video.width==64 && mp4.track[0].SampleDescription.video.height==64,"MP4 geometry matches source");
            MP4D_close(&mp4);fclose(file);
            const double duration=dvr_lib::mp4_duration(files.back());
            check(duration>.13 && duration<.14,"MP4 uses 60fps capture timestamps");
        }
        check(files[0]!=files[1],"Same-second recording names cannot overwrite earlier clips");
    }
    {
        dvr::StreamRecorder missing(std::string(dir)+"/missing",VideoCodec::H265,64,64,60);
        missing.toggle();wait([&]{return !missing.state().error.empty();});
        check(!missing.is_recording(),"Failed open clears recording state");
    }
    {
        int fd=open("/dev/full",O_WRONLY);check(fd>=0,"Open failing storage");
        int failures=0; std::string reason;
        DvrWriter writer(fd,"","",[&](const char* why){++failures;reason=why;},1024);
        uint8_t b[16]{};writer.write(0,b,sizeof(b));
        check(!writer.finish(),"Storage failures propagate after asynchronous drain");
        check(failures==1 && reason=="CARD FULL","Shared writer reports one failure to the recorder");
        check(writer.write(0,b,sizeof(b))!=0,"Failed writer rejects further data");close(fd);
    }
    {
        const auto storage=std::filesystem::path(dir)/"cleanup";
        std::filesystem::create_directory(storage);
        for(const char* name:{"fpvOS_old.mp4","kestrel_old.h265","fpvOS_active.mp4","fpvOS_radio.log","other.mp4"}) {
            FILE* f=fopen((storage/name).c_str(),"w");check(f,"Create storage fixture");fputs("data",f);fclose(f);
        }
        check(purge_oldest_recordings(storage.string(),UINT64_MAX,"fpvOS_active.mp4")==8,"Reclaim only finished recordings");
        check(std::filesystem::exists(storage/"fpvOS_active.mp4") && std::filesystem::exists(storage/"fpvOS_radio.log") &&
              std::filesystem::exists(storage/"other.mp4"),"Keep active video, radio logs and unrelated files");
    }
    // The shared stream sink asks for a keyframe and never starts a file on P frames.
    {
        dvr::StreamRecorder rec(dir,VideoCodec::H265,64,64,60);
        DvrSink sink(VideoCodec::H265,rec);sink.on_parameter_sets(recording_ps);
        std::vector<uint8_t> memory(16*1024);bus::PicturePool pool(memory.data(),16,1024,0);
        auto send=[&](size_t i) {auto p=pool.acquire();check(bool(p),"Picture pool available");
            std::memcpy(p->data(),recording_frames[i].data(),recording_frames[i].size());p->len=recording_frames[i].size();p->pts=2000000+i*16667;p->whole=1;sink.on_picture(p);};
        rec.toggle();wait([&]{return rec.state().recording;});send(1);
        wait([&]{return sink.take_keyframe_request();});check(rec.state().frames==0,"P frames wait for random access");
        send(0);wait([&]{return rec.state().frames==1;});files.push_back(rec.state().file);
        rec.stop();wait([&]{const auto s=rec.state();return !s.recording && !s.finishing;});
    }
    if(argc>1) std::filesystem::copy_file(files.front(),argv[1],std::filesystem::copy_options::overwrite_existing);
    {
        dvr::TelemetryReader telemetry(files.front());const auto first=telemetry.at(0);
        check(first && first->us==0 && first->sample.values.at("battery_v")==16.4,"Telemetry starts at first MP4 sample, independent of source clock");
    }
    std::filesystem::remove_all(dir);
    puts("PASS: HEVC MP4 samples/timing, collision-free names, missing/full storage, keyframe recovery");return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
