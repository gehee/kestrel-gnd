#include "stream_player.hpp"
#include "third_party/minimp4.h"
#include "base/video_codec.hpp"
#include "base/time_util.h"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <cstdio>
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
    if(state_.deleting)throw std::runtime_error("Wait for recording deletion to finish");
    ++generation_;seek_revision_=0;state_.seeking=false;paused_=false;requested_=name;state_.busy=true;state_.playing=false;
    state_.position_us=state_.duration_us=state_.present_from_us=0;state_.error.clear();state_.telemetry_error.clear();state_.started_us=get_time_us();telemetry_samples_.clear();keyframes_us_.clear();state_.file=name;wake_.notify_one();
}
void StreamPlayer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;paused_=false;requested_.clear();state_.busy=state_.playing=state_.seeking=false;wake_.notify_one();
}
void StreamPlayer::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.busy) {paused_=!paused_.load();if(output_.pause)output_.pause(paused_);}
    wake_.notify_one();
}
void StreamPlayer::seek_relative(int seconds,uint64_t presented_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!state_.playing || keyframes_us_.empty() || !seconds || !output_.seek)return;
    const uint64_t base=state_.seeking?state_.seek_target_us:presented_us;
    const int64_t target=int64_t(std::min(base,state_.duration_us))+int64_t(seconds)*1000000;
    const uint64_t want=uint64_t(std::max<int64_t>(target,0));
    // Match Pro: forward lands at/after the requested time, backward at/before.
    // With no later keyframe, keep playing (or keep the paused picture).
    auto key=seconds>0?std::lower_bound(keyframes_us_.begin(),keyframes_us_.end(),want)
                      :std::upper_bound(keyframes_us_.begin(),keyframes_us_.end(),want);
    if(seconds>0) {if(key==keyframes_us_.end())return;}
    else if(key!=keyframes_us_.begin())--key;
    if(*key==base)return;
    state_.seek_target_us=*key;
    state_.seeking=true;++seek_revision_;wake_.notify_one();
}
void StreamPlayer::erase(const std::string& name) {
    if(!dvr_lib::safe_name(name) || !dvr_lib::ends_with(name,".mp4"))throw std::invalid_argument("Not a recording filename");
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.busy)throw std::runtime_error("Stop playback before deleting a recording");
    if(state_.deleting)throw std::runtime_error("Wait for recording deletion to finish");
    state_.deleting=true;state_.deletion_message.clear();delete_requested_=name;wake_.notify_one();
}
void StreamPlayer::refresh() {
    std::lock_guard<std::mutex> lock(mutex_);refresh_=true;wake_.notify_one();
}
void StreamPlayer::run() {
    dvr_lib::set_dir(directory_);
    while(!quit_) {
        std::string file,remove;uint64_t generation;bool list;
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return quit_ || refresh_ || !requested_.empty() || !delete_requested_.empty();});
         if(quit_)break;file=std::move(requested_);requested_.clear();generation=generation_;
         remove=std::move(delete_requested_);delete_requested_.clear();list=refresh_;refresh_=false;}
        const int deletion_error=remove.empty()?0:dvr_lib::delete_recording(remove,"");
        if(!file.empty()) {
            try {play_file(file,generation);}
            catch(const std::exception& e) {std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_)state_.error=e.what();}
            output_.select(false);
            std::lock_guard<std::mutex> lock(mutex_);
            if(generation==generation_)state_.playing=state_.busy=state_.seeking=false;
            ++state_.revision;
        }
        if(!list && file.empty() && remove.empty())continue;
        const auto recordings=dvr_lib::list_recordings();
        {std::lock_guard<std::mutex> lock(mutex_);state_.recordings=recordings;++state_.revision;
         if(!remove.empty()) {
             state_.deleting=false;++state_.deletion_revision;
             state_.deletion_message=deletion_error?std::string("Delete failed: ")+strerror(deletion_error):"Recording deleted";
         }}
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
    // minimp4's frame_offset walks from chunk zero on every lookup. Build
    // byte offsets once so pre-roll and keyframe searches remain linear even
    // in long recordings with a separate chunk for every captured picture.
    if(!t.chunk_count || !t.sample_to_chunk_count || !t.chunk_offset || !t.sample_to_chunk ||
       !t.entry_size || !t.timestamp || !t.duration || t.sample_to_chunk[0].first_chunk!=1)
        throw std::runtime_error("Invalid MP4 sample index");
    std::vector<uint64_t> offsets(t.sample_count);
    unsigned sample=0,group=0;
    for(unsigned chunk=0;chunk<t.chunk_count && sample<t.sample_count;++chunk) {
        if(quit_ || generation!=generation_)return;
        if(group+1<t.sample_to_chunk_count && chunk+1==t.sample_to_chunk[group+1].first_chunk)++group;
        const auto count=t.sample_to_chunk[group].samples_per_chunk;
        if(!count)throw std::runtime_error("Empty MP4 sample chunk");
        uint64_t offset=t.chunk_offset[chunk];
        for(unsigned j=0;j<count && sample<t.sample_count;++j,++sample) {
            const auto bytes=t.entry_size[sample];
            if(bytes<4 || bytes>1024*1024 || offset>uint64_t(st.st_size) || bytes>uint64_t(st.st_size)-offset)
                throw std::runtime_error("Invalid MP4 sample bounds");
            offsets[sample]=offset;offset+=bytes;
        }
    }
    if(sample!=t.sample_count)throw std::runtime_error("Incomplete MP4 sample index");
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
    const auto last=t.sample_count-1;
    // Inspect NAL headers without reading/decoding the picture payloads.
    // MP4 sample offsets/timestamps are already indexed.
    auto random_access=[&](unsigned index) {
        const auto bytes=t.entry_size[index];
        const auto offset=offsets[index];
        unsigned at=0;
        while(at<bytes) {
            uint8_t header[6];const unsigned n=length_bytes+2;
            if(bytes-at<n || pread(file.fd,header,n,offset+at)!=ssize_t(n))
                throw std::runtime_error("Cannot read seek NAL header");
            uint32_t len=0;for(unsigned j=0;j<length_bytes;++j)len=(len<<8)|header[j];
            if(len<2 || len>bytes-at-length_bytes)throw std::runtime_error("Invalid seek NAL length");
            const auto nal=(header[length_bytes]>>1)&63;
            if(nal<=31)return hevc_irap(nal);
            at+=length_bytes+len;
        }
        return false;
    };
    auto timestamp=[&](unsigned index) {
        return uint64_t(t.timestamp[index])*1000000/t.timescale;
    };
    // Index random-access timestamps once. Arrow handling then needs only a
    // binary search, and the UI can show the actual landing point immediately.
    std::vector<uint64_t> keyframes;
    for(unsigned i=0;i<t.sample_count;++i) {
        if(quit_ || generation!=generation_)return;
        if(random_access(i))keyframes.push_back(timestamp(i));
    }
    if(keyframes.empty())throw std::runtime_error("Recording has no random access frame");
    {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;keyframes_us_=std::move(keyframes);}
    {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;state_.playing=true;state_.position_us=0;
     state_.duration_us=(uint64_t(t.timestamp[last])+t.duration[last])*1000000/t.timescale;}
    if(output_.started)output_.started(name);
    uint64_t origin=get_time_us(),base_position=0,seek_revision=0;
    std::deque<uint64_t> outstanding;
    auto wait_decoded=[&](uint64_t pts) {
        const auto start=get_time_us();
        while(output_.decoded && output_.decoded()<=pts) {
            if(quit_ || generation!=generation_ || seek_revision!=seek_revision_)return;
            if(output_.failed() || get_time_us()-start>10000000)
                throw std::runtime_error("Hardware seek decode did not progress");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    // An early pause still needs one picture to hold on screen.
    bool priming=false,first_picture=true;unsigned target_frame=0;
    uint64_t seek_begin=0;
    for(unsigned i=0;i<t.sample_count;) {
        if(seek_revision!=seek_revision_) {
            uint64_t target;
            {std::lock_guard<std::mutex> lock(mutex_);seek_revision=seek_revision_;target=state_.seek_target_us;}
            unsigned low=0,high=t.sample_count;
            while(low<high) {const auto mid=low+(high-low)/2;if(timestamp(mid)<=target)low=mid+1;else high=mid;}
            target_frame=low?low-1:0;i=target_frame;
            seek_begin=get_time_us();
            // The requested destination is already an indexed keyframe.
            // Reset references and submit that picture directly, with no pre-roll.
            outstanding.clear();
            output_.seek(timestamp(target_frame));
            const auto restart=get_time_us();
            while(!output_.ready()) {
                if(quit_ || generation!=generation_)return;
                if(seek_revision!=seek_revision_)break;
                if(output_.failed() || get_time_us()-restart>5000000)throw std::runtime_error("Hardware seek decoder did not start");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if(seek_revision!=seek_revision_)continue;
            for(const auto& p:parameters)output_.feed(p,0,(p[4]>>1)&63,false);
            {std::lock_guard<std::mutex> lock(mutex_);
                if(generation!=generation_)return;
                state_.started_us=get_time_us();telemetry_samples_.clear();
                state_.present_from_us=timestamp(target_frame);
                if(output_.pause)output_.pause(paused_);
            }
            priming=true;base_position=timestamp(i);origin=get_time_us();
        }
        if(quit_ || generation!=generation_)return;
        const auto offset=offsets[i];
        const auto size=t.entry_size[i];
        const uint64_t position=timestamp(i);
        while((paused_ && !priming && !first_picture) || (!(priming && output_.decoded) && get_time_us()<origin+(position-base_position))) {
            if(quit_ || generation!=generation_)return;
            if(seek_revision!=seek_revision_)break;
            const bool paused=paused_ && !priming;const auto before=get_time_us();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if(paused)origin+=get_time_us()-before;
        }
        if(seek_revision!=seek_revision_)continue;
        if(output_.failed())throw std::runtime_error("Hardware playback decode failed");
        // Bound outstanding pictures so playback never triggers the live
        // decoder's compressed-queue drop policy.
        if(output_.decoded) {
            const auto completed=output_.decoded();
            while(!outstanding.empty() && outstanding.front()<completed)outstanding.pop_front();
        }
        if(output_.decoded && outstanding.size()>=6) {
            wait_decoded(outstanding.front());
            if(quit_ || generation!=generation_)return;
            if(seek_revision!=seek_revision_)continue;
            outstanding.pop_front();
        }
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
            if(nal<=31) {type=nal;key|=hevc_irap(nal);}
            annex.insert(annex.end(),{0,0,0,1});annex.insert(annex.end(),packet.begin()+p,packet.begin()+p+len);p+=len;
        }
        auto recorded=telemetry.at(position);
        {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;
            state_.telemetry_error=telemetry.error();telemetry_samples_.push_back({position,std::move(recorded)});
            while(telemetry_samples_.size()>128)telemetry_samples_.pop_front();}
        if(seek_revision!=seek_revision_)continue;
        output_.feed(annex,position,type,key);
        first_picture=false;
        if(output_.decoded) {
            outstanding.push_back(position);
            if(priming && i>=target_frame)wait_decoded(position);
        }
        {std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_ && seek_revision==seek_revision_) {
            state_.position_us=position;
            if(priming && i>=target_frame) {
                fprintf(stderr,"playback: keyframe jump %.3fs in %.1fms\n",
                        position/1000000.0,(get_time_us()-seek_begin)/1000.0);
                priming=false;state_.seeking=false;origin=get_time_us();base_position=position;
            }
        }}
        // A seek to the last frame must stay paused instead of closing the clip.
        while(i+1==t.sample_count && paused_ && seek_revision==seek_revision_) {
            if(quit_ || generation!=generation_)return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(seek_revision==seek_revision_)++i;
    }
    // Leave the last submitted picture enough time to reach scanout.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}
