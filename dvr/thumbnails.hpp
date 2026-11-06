#pragma once
#include "hal/vdec.hpp"
#include "system/thumbnail.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <string>
namespace dvr {
// One bounded I/O worker. Only an armed capture samples a decoded picture;
// the DMA buffer is released before JPEG compression or filesystem access.
class Thumbnails {
public:
    explicit Thumbnails(std::string directory);
    ~Thumbnails();
    void arm(const std::string& name,uint64_t decoded_after_us=0);
    void cancel();
    void capture(std::shared_ptr<DecodedUnit> frame);
    std::shared_ptr<const media::Thumbnail> get(const std::string& name,int width,int height);
    static std::shared_ptr<media::Thumbnail> sample(const DecodedUnit&,int width,int height);
private:
    struct Job {std::string name,key; int width=0,height=0;std::shared_ptr<media::Thumbnail> image;std::shared_ptr<DecodedUnit> frame;uint64_t check_generation=0;};
    struct Entry {std::shared_ptr<media::Thumbnail> image;uint64_t used=0;};
    void run();
    std::string directory_,armed_;
    uint64_t decoded_after_us_=0,tick_=0,generation_=0;
    std::atomic<bool> wanted_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::map<std::string,Entry> cache_;
    bool quit_=false,capture_pending_=false;
    std::thread worker_;
};
}
