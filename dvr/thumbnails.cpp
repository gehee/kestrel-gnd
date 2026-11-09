#include "thumbnails.hpp"
#include "dvr_library.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 2048
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"
namespace dvr {
namespace {
std::string path(const std::string& dir,const std::string& name) {return dvr_lib::thumb_path(dir,name);}
std::shared_ptr<media::Thumbnail> resize(const unsigned char* rgb,int sw,int sh,int w,int h,int channels) {
    auto out=std::make_shared<media::Thumbnail>();out->width=w;out->height=h;out->argb.resize(size_t(w)*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const auto* p=rgb+((y*sh/h)*sw+x*sw/w)*channels;
        out->argb[y*w+x]=0xff000000u|uint32_t(p[0])<<16|uint32_t(p[1])<<8|p[2];
    }
    return out;
}
std::shared_ptr<media::Thumbnail> read(const std::string& name,int w,int h) {
    int fd=open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return {};
    struct stat s{};std::vector<uint8_t> bytes;
    if(!fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_size>0 && s.st_size<=1024*1024) {
        bytes.resize(s.st_size);size_t got=0;
        while(got<bytes.size()) {auto n=::read(fd,bytes.data()+got,bytes.size()-got);if(n<0 && errno==EINTR)continue;if(n<=0)break;got+=n;}
        if(got!=bytes.size())bytes.clear();
    }
    close(fd);if(bytes.empty())return {};
    int sw=0,sh=0,c=0;
    if(!stbi_info_from_memory(bytes.data(),int(bytes.size()),&sw,&sh,&c) || sw<1 || sh<1 || sw>640 || sh>360)return {};
    auto* rgb=stbi_load_from_memory(bytes.data(),int(bytes.size()),&sw,&sh,&c,3);if(!rgb)return {};
    auto out=resize(rgb,sw,sh,w,h,3);stbi_image_free(rgb);return out;
}
bool write(const std::string& file,const media::Thumbnail& image) {
    std::vector<uint8_t> rgb;rgb.reserve(image.argb.size()*3);
    for(auto p:image.argb) {rgb.push_back(p>>16);rgb.push_back(p>>8);rgb.push_back(p);}
    std::vector<uint8_t> jpeg;
    if(!stbi_write_jpg_to_func([](void* ctx,void* p,int n){auto& b=*static_cast<std::vector<uint8_t>*>(ctx);auto* v=static_cast<uint8_t*>(p);b.insert(b.end(),v,v+n);},
        &jpeg,image.width,image.height,3,rgb.data(),80))return false;
    std::string temp=file+".tmp-XXXXXX";int fd=mkstemp(temp.data());if(fd<0)return false;
    size_t put=0;while(put<jpeg.size()) {auto n=::write(fd,jpeg.data()+put,jpeg.size()-put);if(n<0 && errno==EINTR)continue;if(n<=0)break;put+=n;}
    const bool ok=put==jpeg.size() && !fsync(fd);close(fd);
    if(ok && !rename(temp.c_str(),file.c_str()))return true;
    unlink(temp.c_str());return false;
}
}
Thumbnails::Thumbnails(std::string dir):directory_(std::move(dir)),worker_([this]{run();}) {}
Thumbnails::~Thumbnails() {{std::lock_guard<std::mutex> lock(mutex_);quit_=true;}wake_.notify_one();worker_.join();}
void Thumbnails::arm(const std::string& name,uint64_t pts) {
    if(!dvr_lib::safe_name(name))return;
    std::lock_guard<std::mutex> lock(mutex_);armed_=name;decoded_after_us_=pts;wanted_=false;
    const auto generation=++generation_;
    if(jobs_.size()<12){Job job;job.name=name;job.check_generation=generation;jobs_.push_front(std::move(job));wake_.notify_one();}
}
void Thumbnails::cancel() {std::lock_guard<std::mutex> lock(mutex_);wanted_=false;armed_.clear();++generation_;}
std::shared_ptr<media::Thumbnail> Thumbnails::sample(const DecodedUnit& f,int w,int h) {
    if(f.early || !f.width || !f.height || w<1 || h<1 || w>320 || h>180 ||
        !f.data[0] || !f.data[1] || !f.data[2] || f.linesize[0]<int(f.width) ||
        f.linesize[1]<int((f.width+1)/2) || f.linesize[2]<int((f.width+1)/2))return {};
    dma_buf_sync sync{DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
    if(f.has_prime_fd && ioctl(f.prime_fd,DMA_BUF_IOCTL_SYNC,&sync)<0)return {};
    auto out=std::make_shared<media::Thumbnail>();out->width=w;out->height=h;out->argb.resize(size_t(w)*h);
    auto clamp=[](int v){return uint32_t(std::clamp(v,0,255));};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const int sx=x*f.width/w,sy=y*f.height/h;
        const int l=int(f.data[0][sy*f.linesize[0]+sx])-16;
        const int u=int(f.data[1][(sy/2)*f.linesize[1]+sx/2])-128,v=int(f.data[2][(sy/2)*f.linesize[2]+sx/2])-128;
        out->argb[y*w+x]=0xff000000u|clamp((298*l+459*v+128)>>8)<<16|
            clamp((298*l-55*u-136*v+128)>>8)<<8|clamp((298*l+541*u+128)>>8);
    }
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;if(f.has_prime_fd)ioctl(f.prime_fd,DMA_BUF_IOCTL_SYNC,&sync);
    return out;
}
void Thumbnails::capture(std::shared_ptr<DecodedUnit> frame) {
    if(!wanted_.load(std::memory_order_relaxed))return;
    std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
    if(!lock || !wanted_ || capture_pending_ || frame->early || frame->dec_end_ts<decoded_after_us_ || jobs_.size()>=12)return;
    jobs_.push_front({armed_,{},0,0,{},std::move(frame)});capture_pending_=true;
    wanted_=false;armed_.clear();wake_.notify_one();
}
std::shared_ptr<const media::Thumbnail> Thumbnails::get(const std::string& name,int w,int h) {
    if(!dvr_lib::safe_name(name) || w<1 || h<1 || w>320 || h>180)return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key=name+":"+std::to_string(w)+":"+std::to_string(h);
    auto it=cache_.find(key);if(it!=cache_.end()){it->second.used=++tick_;return it->second.image;}
    if(jobs_.size()>=12)return {};
    if(cache_.size()>=12) {
        auto oldest=std::min_element(cache_.begin(),cache_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        cache_.erase(oldest);
    }
    cache_[key]={nullptr,++tick_};jobs_.push_back({name,key,w,h,{}});wake_.notify_one();return {};
}
void Thumbnails::run() {
    pthread_setname_np(pthread_self(),"dvr-thumbs");sched_param p{};pthread_setschedparam(pthread_self(),SCHED_OTHER,&p);
    for(;;) {
        Job job;{std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return quit_ || !jobs_.empty();});
            if(jobs_.empty() && quit_)return;job=std::move(jobs_.front());jobs_.pop_front();}
        try {
        if(job.check_generation) {
            const bool cached=bool(read(path(directory_,job.name),1,1));
            std::lock_guard<std::mutex> lock(mutex_);
            if(job.check_generation==generation_)wanted_=!cached;
            continue;
        }
        if(job.frame) {
            const auto start=std::chrono::steady_clock::now();
            job.image=sample(*job.frame,320,180);job.frame.reset();
            {std::lock_guard<std::mutex> lock(mutex_);capture_pending_=false;}
            fprintf(stderr,"thumbnail: sampled %s on worker in %lld us\n",job.name.c_str(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
            if(!job.image)continue;
        }
        const auto file=path(directory_,job.name);
        if(job.image) {
            // Keep a valid existing preview. Playback of an older clip supplies
            // its first thumbnail without opening a competing video decoder.
            if(!read(file,320,180)) {
                mkdir(dvr_lib::thumb_dir(directory_).c_str(),0755);
                if(!write(file,*job.image))fprintf(stderr,"thumbnail: cannot save %s\n",job.name.c_str());
            }
            // A clip can be deleted while this worker encodes its last frame.
            // Deletion removes video first; this post-write check prevents an
            // in-flight capture from recreating an orphan preview afterward.
            if(access((directory_+"/"+job.name).c_str(),F_OK)<0)unlink(file.c_str());
            std::lock_guard<std::mutex> lock(mutex_);cache_.clear();
        } else {
            auto image=read(file,job.width,job.height);
            std::lock_guard<std::mutex> lock(mutex_);auto it=cache_.find(job.key);if(it!=cache_.end())it->second.image=std::move(image);
        }
        } catch(const std::exception& e) {job.frame.reset();std::lock_guard<std::mutex> lock(mutex_);capture_pending_=false;fprintf(stderr,"thumbnail: %s\n",e.what());}
    }
}
}
