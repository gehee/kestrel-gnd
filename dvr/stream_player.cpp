#include "stream_player.hpp"
#include "third_party/minimp4.h"
#include "base/time_util.h"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace dvr {
StreamPlayer::StreamPlayer(std::string directory,Output output,int width,int height)
    :directory_(std::move(directory)),output_(std::move(output)),width_(width),height_(height),worker_([this]{run();}) {}
StreamPlayer::~StreamPlayer() {quit_=true;stop();worker_.join();}
StreamPlayer::State StreamPlayer::state() const {std::lock_guard<std::mutex> lock(mutex_);auto s=state_;s.paused=paused_;return s;}
std::shared_ptr<const media::TelemetrySample> StreamPlayer::telemetry_at(uint64_t us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!state_.playing || telemetry_samples_.empty())return {};
    auto after=std::upper_bound(telemetry_samples_.begin(),telemetry_samples_.end(),us,[](uint64_t t,const PresentedSample& s){return t<s.us;});
    if(after==telemetry_samples_.begin())return {};
    const auto& before=*std::prev(after);if(!before.data || us-before.us>2500000)return {};
    std::shared_ptr<media::TelemetrySample> sample;
    if(after!=telemetry_samples_.end() && after->data && after->us>before.us && after->us-before.us<=100000) {
        const double a=double(us-before.us)/(after->us-before.us);
        for(const char* key:{"motion.dx","motion.dy","motion.bank_rad"}) {
            const auto x=before.data->sample.values.find(key);const auto y=after->data->sample.values.find(key);
            if(x!=before.data->sample.values.end() && y!=after->data->sample.values.end() && x->second!=y->second) {
                if(!sample)sample=std::make_shared<media::TelemetrySample>(before.data->sample);
                sample->values[key]=x->second+(y->second-x->second)*a;
            }
        }
    }
    if(sample)return sample;
    return std::shared_ptr<const media::TelemetrySample>(before.data,&before.data->sample);
}
void StreamPlayer::play(const std::string& name) {
    if(!dvr_lib::safe_name(name) || !dvr_lib::ends_with(name,".mp4"))throw std::invalid_argument("Not a recording filename");
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;paused_=false;requested_=name;state_.busy=true;state_.playing=false;
    state_.position_us=state_.duration_us=0;state_.error.clear();state_.telemetry_error.clear();state_.started_us=get_time_us();telemetry_samples_.clear();state_.file=name;wake_.notify_one();
}
void StreamPlayer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;paused_=false;requested_.clear();state_.busy=state_.playing=false;wake_.notify_one();
}
void StreamPlayer::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.busy) {paused_=!paused_.load();if(output_.pause)output_.pause(paused_);}
    wake_.notify_one();
}
void StreamPlayer::run() {
    dvr_lib::set_dir(directory_);
    while(!quit_) {
        std::string file;uint64_t generation;
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::milliseconds(500),[&]{return quit_ || !requested_.empty();});
         if(quit_)break;file=std::move(requested_);requested_.clear();generation=generation_;}
        if(!file.empty()) {
            try {play_file(file,generation);}
            catch(const std::exception& e) {std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_)state_.error=e.what();}
            output_.select(false);
            std::lock_guard<std::mutex> lock(mutex_);
            if(generation==generation_)state_.playing=state_.busy=false;
            ++state_.revision;
        }
        const auto recordings=dvr_lib::list_recordings();
        {std::lock_guard<std::mutex> lock(mutex_);state_.recordings=recordings;++state_.revision;}
    }
    output_.select(false);
}
void StreamPlayer::play_file(const std::string& name,uint64_t generation) {
    struct File {int fd=-1;~File(){if(fd>=0)close(fd);}} file;
    file.fd=open((directory_+"/"+name).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(file.fd<0)throw std::runtime_error(std::string("Open recording: ")+strerror(errno));
    struct stat st{};if(fstat(file.fd,&st) || !S_ISREG(st.st_mode) || st.st_size<16)throw std::runtime_error("Recording is not a complete regular file");
    struct Demux {MP4D_demux_t m{};~Demux(){MP4D_close(&m);}} demux;
    auto& mp4=demux.m;
    if(!MP4D_open(&mp4,[](int64_t off,void* b,size_t n,void* ctx)->int {
        const int fd=*static_cast<int*>(ctx);size_t got=0;
        while(got<n) {ssize_t k=pread(fd,static_cast<uint8_t*>(b)+got,n-got,off+got);if(k<0 && errno==EINTR)continue;if(k<=0)return 1;got+=size_t(k);}return 0;
    },&file.fd,st.st_size))throw std::runtime_error("Cannot read MP4; recording may still be saving");
    unsigned track=0;
    while(track<mp4.track_count && mp4.track[track].object_type_indication!=MP4_OBJECT_TYPE_HEVC)++track;
    if(track==mp4.track_count)throw std::runtime_error("This player requires an HEVC recording");
    const auto& t=mp4.track[track];
    if(t.SampleDescription.video.width!=unsigned(width_) || t.SampleDescription.video.height!=unsigned(height_))
        throw std::runtime_error("Recording dimensions do not match the current display pipeline");
    if(!t.sample_count || !t.timescale || t.dsi_bytes<23)throw std::runtime_error("Recording has no playable video");
    const auto* cfg=t.dsi;const unsigned length_bytes=(cfg[21]&3)+1;
    std::vector<std::vector<uint8_t>> parameters;unsigned at=23;
    for(unsigned array=0;array<cfg[22];++array) {
        if(at+3>t.dsi_bytes)throw std::runtime_error("Truncated HEVC parameter array");
        const unsigned type=cfg[at++]&63;const unsigned count=(cfg[at]<<8)|cfg[at+1];at+=2;
        for(unsigned i=0;i<count;++i) {
            if(at+2>t.dsi_bytes)throw std::runtime_error("Truncated HEVC parameter size");
            const unsigned size=(cfg[at]<<8)|cfg[at+1];at+=2;
            if(size<2 || at+size>t.dsi_bytes)throw std::runtime_error("Invalid HEVC parameter size");
            if(type>=32 && type<=34) {std::vector<uint8_t> p{0,0,0,1};p.insert(p.end(),cfg+at,cfg+at+size);parameters.push_back(std::move(p));}
            at+=size;
        }
    }
    if(parameters.size()<3)throw std::runtime_error("Missing HEVC parameter sets");
    if(quit_ || generation!=generation_)return;
    TelemetryReader telemetry(directory_+"/"+name);
    output_.select(true);
    uint64_t started=get_time_us();
    while(!output_.ready()) {
        if(quit_ || generation!=generation_)return;
        if(output_.failed() || get_time_us()-started>5000000)throw std::runtime_error("Hardware playback decoder did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {std::lock_guard<std::mutex> lock(mutex_);if(output_.pause)output_.pause(paused_);}
    for(const auto& p:parameters) output_.feed(p,0,(p[4]>>1)&63,false);
    unsigned size=0,pts=0,duration=0;
    MP4D_frame_offset(&mp4,track,t.sample_count-1,&size,&pts,&duration);
    {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;state_.playing=true;state_.position_us=0;
     state_.duration_us=(uint64_t(pts)+duration)*1000000/t.timescale;}
    if(output_.started)output_.started(name);
    uint64_t origin=get_time_us();
    for(unsigned i=0;i<t.sample_count;++i) {
        if(quit_ || generation!=generation_)return;
        const uint64_t offset=MP4D_frame_offset(&mp4,track,i,&size,&pts,&duration);
        if(size<4 || size>1024*1024 || offset>uint64_t(st.st_size) || size>uint64_t(st.st_size)-offset)
            throw std::runtime_error("Invalid MP4 sample bounds");
        const uint64_t position=uint64_t(pts)*1000000/t.timescale;
        while(paused_ || get_time_us()<origin+position) {
            if(quit_ || generation!=generation_)return;
            const bool paused=paused_;const auto before=get_time_us();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if(paused)origin+=get_time_us()-before;
        }
        if(output_.failed())throw std::runtime_error("Hardware playback decode failed");
        std::vector<uint8_t> packet(size);size_t got=0;
        while(got<size) {
            const auto n=pread(file.fd,packet.data()+got,size-got,offset+got);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)throw std::runtime_error("Recording read failed");got+=size_t(n);
        }
        std::vector<uint8_t> annex;annex.reserve(size+16);uint8_t type=0;bool key=false;
        size_t p=0;
        while(p<packet.size()) {
            if(packet.size()-p<length_bytes)throw std::runtime_error("Truncated MP4 NAL length");
            uint32_t len=0;for(unsigned j=0;j<length_bytes;++j)len=(len<<8)|packet[p++];
            if(len<2 || len>packet.size()-p)throw std::runtime_error("Invalid MP4 NAL length");
            const uint8_t nal=(packet[p]>>1)&63;
            if(nal<=31) {type=nal;key|=nal>=16 && nal<=21;}
            annex.insert(annex.end(),{0,0,0,1});annex.insert(annex.end(),packet.begin()+p,packet.begin()+p+len);p+=len;
        }
        auto recorded=telemetry.at(position);
        {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;
            state_.telemetry_error=telemetry.error();telemetry_samples_.push_back({position,std::move(recorded)});
            while(telemetry_samples_.size()>128)telemetry_samples_.pop_front();}
        output_.feed(annex,position,type,key);
        {std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_)state_.position_us=position;}
    }
    // Leave the last submitted picture enough time to reach scanout.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}
