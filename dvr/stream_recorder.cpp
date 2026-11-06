#include "stream_recorder.hpp"
#include "writer.hpp"
#include "third_party/minimp4.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <sys/statvfs.h>
#include <unistd.h>

namespace dvr {
StreamRecorder::StreamRecorder(std::string dir,VideoCodec codec,int width,int height,int fps)
    :directory_(std::move(dir)),codec_(codec),width_(width),height_(height),fps_(std::max(1,fps)),worker_([this]{run();}) {}
StreamRecorder::~StreamRecorder() {
    stop();
    {std::lock_guard<std::mutex> lock(mutex_);shutdown_=true;}
    wake_.notify_one(); worker_.join();
}
StreamRecorder::State StreamRecorder::state() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
void StreamRecorder::toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(requested_) {requested_=false; Item item;item.kind=Item::Stop;queue_.push_back(std::move(item));}
    else {
        ++session_; requested_=true;
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
    queued_+=item.size();queue_.push_back(std::move(item));wake_.notify_one();
}
void StreamRecorder::set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {Item i;i.kind=Item::Params;i.params=ps;enqueue(std::move(i));}
void StreamRecorder::feed(std::shared_ptr<std::vector<uint8_t>> data,uint64_t us) {Item i;i.data=std::move(data);i.us=us;enqueue(std::move(i));}
void StreamRecorder::feed(const bus::PictureRef& p,uint64_t us) {
    if(p->pool->free_count()<8) {feed(std::make_shared<std::vector<uint8_t>>(p->data(),p->data()+p->len),us);return;}
    Item i;i.picture=p;i.us=us;enqueue(std::move(i));
}
void StreamRecorder::run() {
    int fd=-1; MP4E_mux_t* mux=nullptr; mp4_h26x_writer_t video{};
    std::unique_ptr<DvrWriter> writer;
    std::vector<std::vector<uint8_t>> params;
    uint64_t previous=0,frames=0;
    std::string file;
    auto storage=[&] {
        struct statvfs space{};
        const bool ok=!directory_.empty() && !statvfs(directory_.c_str(),&space);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.free_bytes=ok?uint64_t(space.f_bavail)*space.f_frsize:0;
        state_.storage_ready=ok && state_.free_bytes>=(64u<<20);
    };
    storage();
    auto storage_time=std::chrono::steady_clock::now();
    const int nominal=90000/fps_;
    auto close=[&] {
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
         if(queue_.empty()) {if(shutdown_) break;lock.unlock();storage();continue;}
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
                close(); previous=0;frames=0;file.clear(); video={};
                {std::lock_guard<std::mutex> lock(mutex_);state_.error.clear();state_.frames=state_.dropped=0;}
                struct statvfs space{};
                if(directory_.empty()) throw std::runtime_error("No recording folder configured");
                if(statvfs(directory_.c_str(),&space)) throw std::runtime_error(std::string("Recording folder: ")+strerror(errno));
                if(uint64_t(space.f_bavail)*space.f_frsize<(64u<<20)) throw std::runtime_error("Less than 64 MiB recording space remains");
                std::time_t wall=std::time(nullptr);std::tm tm{};localtime_r(&wall,&tm);
                char stamp[32];std::strftime(stamp,sizeof(stamp),"%Y%m%d_%H%M%S",&tm);
                for(int suffix=0;suffix<1000;++suffix) {
                    file=directory_+"/recording_"+stamp+(suffix?"_"+std::to_string(suffix):"")+".mp4";
                    fd=open(file.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0644);
                    if(fd>=0 || errno!=EEXIST) break;
                }
                if(fd<0) {file.clear();throw std::runtime_error(std::string("Create recording: ")+strerror(errno));}
                writer=std::make_unique<DvrWriter>(fd,8u<<20);
                mux=MP4E_open(0,1,writer.get(),[](int64_t off,const void* p,size_t n,void* w){return static_cast<DvrWriter*>(w)->write(off,p,n);});
                if(!mux) throw std::runtime_error("Cannot open MP4 writer");
                MP4E_set_finalize(mux);MP4E_set_full_frame_rate(mux);
                if(mp4_h26x_write_init(&video,mux,width_,height_,codec_==VideoCodec::H265)!=MP4E_STATUS_OK) throw std::runtime_error("Cannot initialize MP4 track");
                header();
                {std::lock_guard<std::mutex> lock(mutex_);state_.recording=true;state_.file=file;}
                continue;
            }
            if(!mux) continue;
            int duration=nominal;
            if(previous && item.us>previous) {
                const int measured=int(std::clamp<uint64_t>(item.us-previous,1000,1000000)*90000/1000000);
                if(std::abs(measured-nominal)>nominal/4) duration=measured;
            }
            previous=item.us;
            const int rc=mp4_h26x_write_nal(&video,item.bytes(),int(item.size()),duration);
            if(rc!=MP4E_STATUS_OK && rc!=MP4E_STATUS_BAD_ARGUMENTS) throw std::runtime_error("Recording write failed");
            {std::lock_guard<std::mutex> lock(mutex_);if(rc==MP4E_STATUS_OK)state_.frames=++frames;else ++state_.dropped;}
        } catch(const std::exception& e) {
            requested_=false; close();
            std::lock_guard<std::mutex> lock(mutex_);state_.error=e.what();
        }
    }
    close();
}
}
