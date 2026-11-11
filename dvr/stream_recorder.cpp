#include "stream_recorder.hpp"
#include "dvr_recover.hpp"
#include "writer.hpp"
#include "third_party/minimp4.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace dvr {
StreamRecorder::StreamRecorder(std::string dir,VideoCodec codec,int width,int height,int fps)
    :directory_(std::move(dir)),codec_(codec),width_(width),height_(height),fps_(std::max(1,fps)),worker_([this]{run();}) {
    // Recordings a power cut or a crash left fragmented (the file is made
    // plain only when REC stops), made playable: once, at start, on a thread
    // of its own at the lowest priority - a long one takes a while to read.
    // It only reads the name of the one being recorded, kept apart from the
    // recorder so that the thread may outlive it.
    struct stat st{};
    if(directory_.empty() || stat(directory_.c_str(),&st) || !S_ISDIR(st.st_mode)) return;
    std::thread([dir=directory_,active=active_] {
        pthread_setname_np(pthread_self(),"dvr-recover");
        setpriority(PRIO_PROCESS,0,19);   // this thread only, on Linux
        dvr_recover_recordings(dir,[active] {std::lock_guard<std::mutex> lock(active->mutex);return active->name;});
    }).detach();
}
StreamRecorder::~StreamRecorder() {
    stop();
    {std::lock_guard<std::mutex> lock(mutex_);shutdown_=true;}
    wake_.notify_one(); worker_.join();
}
StreamRecorder::State StreamRecorder::state() const {std::lock_guard<std::mutex> lock(mutex_);auto s=state_;s.telemetry_error=telemetry_.error();if(!s.recording && telemetry_.pending())s.finishing=true;return s;}
void StreamRecorder::set_telemetry(std::shared_ptr<const media::TelemetrySample> sample) {
    std::lock_guard<std::mutex> lock(mutex_);latest_telemetry_=std::move(sample);
}
void StreamRecorder::toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(requested_) {requested_=false; Item item;item.kind=Item::Stop;queue_.push_back(std::move(item));}
    else {
        ++session_; requested_=true;latest_telemetry_=std::make_shared<media::TelemetrySample>();
        Item item; item.kind=Item::Start;queue_.push_back(std::move(item));
    }
    wake_.notify_one();
}
void StreamRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(requested_.exchange(false)) {Item item;item.kind=Item::Stop;queue_.push_back(std::move(item));wake_.notify_one();}
}
void StreamRecorder::enqueue(Item item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(shutdown_ || (item.kind==Item::Frame && !requested_)) return;
    if(item.kind==Item::Frame && (queue_.size()>=60 || queued_+item.size()>(4u<<20))) {
        ++state_.dropped;
        // Do not silently produce a recording with an unbounded gap.
        state_.error="Recording storage cannot keep up";
        requested_=false; Item stop;stop.kind=Item::Stop;queue_.push_back(std::move(stop));wake_.notify_one();return;
    }
    if(item.kind==Item::Frame)item.telemetry=latest_telemetry_;
    queued_+=item.size();queue_.push_back(std::move(item));wake_.notify_one();
}
void StreamRecorder::set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {Item i;i.kind=Item::Params;i.params=ps;enqueue(std::move(i));}
void StreamRecorder::feed(std::shared_ptr<std::vector<uint8_t>> data,uint64_t us) {Item i;i.data=std::move(data);i.us=us;enqueue(std::move(i));}
void StreamRecorder::feed(const bus::PictureRef& p,uint64_t us) {
    if(p->pool->free_count()<kRecordingPoolReserve) {feed(std::make_shared<std::vector<uint8_t>>(p->data(),p->data()+p->len),us);return;}
    Item i;i.picture=p;i.us=us;enqueue(std::move(i));
}
void StreamRecorder::run() {
    int fd=-1; MP4E_mux_t* mux=nullptr; mp4_h26x_writer_t video{};
    std::unique_ptr<DvrWriter> writer;
    std::vector<std::vector<uint8_t>> params;
    uint64_t frames=0,ticks=0;
    std::string file;
    // A picture is written once the next is in: its duration is the time to
    // the next, not since the one before (DVR::run_dvr's rule). After a gap
    // the picture past it came early and then stood - a jump, then a freeze,
    // where the screen had frozen on the one before it.
    Item held;
    bool have_held=false;
    // Ready: 64 MiB free, or that much to be had from the oldest finished
    // recordings, which REC start and the writer clear (purge_oldest_recordings)
    // as the VRX Pro does - a full card of old clips must not refuse REC.
    auto storage=[&] {
        struct statvfs space{};
        const bool ok=!directory_.empty() && !statvfs(directory_.c_str(),&space);
        const uint64_t free_bytes=ok?uint64_t(space.f_bavail)*space.f_frsize:0;
        const std::string active=file.empty()?std::string():file.substr(file.find_last_of('/')+1);
        const bool ready=ok && (free_bytes>=(64u<<20) ||
            free_bytes+reclaimable_recording_bytes(directory_,active)>=(64u<<20));
        std::lock_guard<std::mutex> lock(mutex_);
        state_.free_bytes=free_bytes;
        state_.storage_ready=ready;
    };
    storage();
    auto storage_time=std::chrono::steady_clock::now();
    const int nominal=90000/fps_;
    // Within a quarter of the nominal interval it snaps to nominal.
    auto duration=[&](uint64_t from,uint64_t to) {
        int d=nominal;
        if(from && to>from) {
            const int measured=int(std::clamp<uint64_t>(to-from,1000,1000000)*90000/1000000);
            if(std::abs(measured-nominal)>nominal/4) d=measured;
        }
        return d;
    };
    // The held picture out, lasting until next_us (0: unknown, nominal).
    // Its telemetry at its own start. It counted as recorded once taken
    // (state_.frames); one the muxer refuses counts as dropped instead.
    auto put_held=[&](uint64_t next_us) {
        if(!have_held) return;
        have_held=false;
        const Item h=std::move(held);
        held=Item();
        const int d=duration(h.us,next_us);
        const int rc=mp4_h26x_write_nal(&video,h.bytes(),int(h.size()),d);
        if(rc!=MP4E_STATUS_OK && rc!=MP4E_STATUS_BAD_ARGUMENTS) throw std::runtime_error("Recording write failed");
        if(rc==MP4E_STATUS_OK){telemetry_.append(ticks*1000000/90000,h.telemetry);ticks+=d;}
        else {std::lock_guard<std::mutex> lock(mutex_);state_.frames=--frames;++state_.dropped;}
    };
    auto set_active=[&](const std::string& path) {
        std::lock_guard<std::mutex> lock(active_->mutex);
        active_->name=path.empty()?std::string():path.substr(path.find_last_of('/')+1);
    };
    // The size the stream's SPS gives (HEVC), else the one configured.
    auto stream_size=[&](int& w,int& h) {
        w=width_;h=height_;
        if(codec_!=VideoCodec::H265) return;
        for(const auto& p:params) {
            size_t at=0;   // past its start code (Annex-B), if it has one
            if(p.size()>=4 && !p[0] && !p[1] && !p[2] && p[3]==1) at=4;
            else if(p.size()>=3 && !p[0] && !p[1] && p[2]==1) at=3;
            if(at+2>p.size() || ((p[at]>>1)&63)!=33) continue;
            int sw=0,sh=0;
            if(MP4E_hevc_sps_size(p.data()+at,int(p.size()-at),&sw,&sh)) {w=sw;h=sh;return;}
        }
    };
    auto close=[&] {
        // The last picture first, nominal: there is no next.
        if(mux) {try{put_held(0);}catch(const std::exception&){}}
        have_held=false;held=Item();
        set_active(std::string());
        if(fd>=0)telemetry_.finish();
        {std::lock_guard<std::mutex> lock(mutex_);state_.recording=false;state_.finishing=fd>=0;}
        if(mux) {MP4E_close(mux);mp4_h26x_write_close(&video);mux=nullptr;}
        if(writer && !writer->finish()) {
            std::lock_guard<std::mutex> lock(mutex_);state_.error="Recording storage write failed";
        }
        writer.reset();
        if(fd>=0) {::close(fd);fd=-1;}
        if(!frames && !file.empty()) unlink(file.c_str());
        {std::lock_guard<std::mutex> lock(mutex_);state_.finishing=false;}
    };
    auto header=[&] {
        if(mux) for(const auto& p:params) if(!p.empty()) {
            const int rc=mp4_h26x_write_nal(&video,p.data(),int(p.size()),nominal);
            if(rc!=MP4E_STATUS_OK && rc!=MP4E_STATUS_BAD_ARGUMENTS) throw std::runtime_error("Recording parameter sets rejected");
        }
    };
    for(;;) {
        Item item;
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::seconds(1),[&]{return shutdown_ || !queue_.empty();});
         // Idle, the card is asked only every 5 s (statvfs; the oldest
         // recordings listed only when it is nearly full).
         if(queue_.empty()) {
             if(shutdown_) break;
             lock.unlock();
             const auto now=std::chrono::steady_clock::now();
             if(mux || now-storage_time>=std::chrono::seconds(5)) {storage();storage_time=now;}
             continue;
         }
         item=std::move(queue_.front());queue_.pop_front();queued_-=item.size();}
        try {
            const auto now=std::chrono::steady_clock::now();
            if(now-storage_time>=std::chrono::seconds(1)) {
                storage();storage_time=now;
                if(mux && !state().storage_ready) throw std::runtime_error("Recording storage full or unavailable");
            }
            if(item.kind==Item::Stop) {close();continue;}
            if(item.kind==Item::Params) {params=std::move(item.params);header();continue;}
            if(item.kind==Item::Start) {
                close(); frames=0;ticks=0;file.clear(); video={};
                {std::lock_guard<std::mutex> lock(mutex_);state_.error.clear();state_.frames=state_.dropped=0;}
                struct statvfs space{};
                if(directory_.empty()) throw std::runtime_error("No recording folder configured");
                // Room on the card first, from the oldest finished recordings.
                purge_oldest_recordings(directory_,kDvrMinFreeBytes);
                if(statvfs(directory_.c_str(),&space)) throw std::runtime_error(std::string("Recording folder: ")+strerror(errno));
                if(uint64_t(space.f_bavail)*space.f_frsize<(64u<<20)) throw std::runtime_error("Less than 64 MiB recording space remains");
                std::time_t wall=std::time(nullptr);std::tm tm{};localtime_r(&wall,&tm);
                char stamp[32];std::strftime(stamp,sizeof(stamp),"%Y%m%d_%H%M%S",&tm);
                for(int suffix=0;suffix<1000;++suffix) {
                    // The VRX Pro's names: the gallery, the web view and the
                    // card's clean-up (purge_oldest_recordings) take them as recordings.
                    file=directory_+"/fpvOS_"+stamp+(suffix?"_"+std::to_string(suffix):"")+".mp4";
                    fd=open(file.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0644);
                    if(fd>=0 || errno!=EEXIST) break;
                }
                if(fd<0) {file.clear();throw std::runtime_error(std::string("Create recording: ")+strerror(errno));}
                set_active(file);
                telemetry_.begin(file);
                // The writer keeps room on the card while it writes (never
                // this file) and says when the card stops taking it: the
                // recording ends then, with the reason shown.
                const std::string name=file.substr(file.find_last_of('/')+1);
                writer=std::make_unique<DvrWriter>(fd,directory_,name,[this](const char* why) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.error=std::string("Recording stopped: ")+(std::strcmp(why,"CARD FULL")==0?"card full":"card error");
                    if(requested_.exchange(false)) {Item stop;stop.kind=Item::Stop;queue_.push_back(std::move(stop));}
                    wake_.notify_one();
                },8u<<20);
                mux=MP4E_open(0,1,writer.get(),[](int64_t off,const void* p,size_t n,void* w){return static_cast<DvrWriter*>(w)->write(off,p,n);});
                if(!mux) throw std::runtime_error("Cannot open MP4 writer");
                MP4E_set_finalize(mux);MP4E_set_full_frame_rate(mux);
                int w=0,h=0;stream_size(w,h);
                if(mp4_h26x_write_init(&video,mux,w,h,codec_==VideoCodec::H265)!=MP4E_STATUS_OK) throw std::runtime_error("Cannot initialize MP4 track");
                header();
                {std::lock_guard<std::mutex> lock(mutex_);state_.recording=true;state_.file=file;}
                continue;
            }
            if(!mux) continue;
            put_held(item.us);
            held=std::move(item);
            have_held=true;
            {std::lock_guard<std::mutex> lock(mutex_);state_.frames=++frames;}
        } catch(const std::exception& e) {
            requested_=false; close();
            // The writer's own reason (card full) when it gave one: the muxer's
            // failure after it says less.
            std::lock_guard<std::mutex> lock(mutex_);if(state_.error.empty())state_.error=e.what();
        }
    }
    close();
}
}
