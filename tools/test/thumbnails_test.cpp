#include "dvr/thumbnails.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
using namespace std::chrono_literals;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    char dir[]="thumbnail-test-XXXXXX";check(mkdtemp(dir),"test directory");
    struct Cleanup {std::string p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{dir};
    std::vector<uint8_t> y(64*48,126),u(32*24,128),v(32*24,128);
    DecodedUnit frame{};frame.width=64;frame.height=48;frame.data[0]=y.data();frame.data[1]=u.data();frame.data[2]=v.data();
    frame.linesize[0]=64;frame.linesize[1]=frame.linesize[2]=32;frame.dec_end_ts=100;
    const auto sample=dvr::Thumbnails::sample(frame,32,18);check(sample && sample->argb.size()==576,"bounded I420 sampling");
    check((sample->argb[0]&0xffffff)==0x808080,"limited-range neutral conversion");
    frame.early=true;check(!dvr::Thumbnails::sample(frame,32,18),"partial frames rejected");frame.early=false;
    std::ofstream(std::string(dir)+"/clip.mp4")<<"recording";
    {
        dvr::Thumbnails thumbnails(dir);thumbnails.arm("clip.mp4",101);thumbnails.capture(std::make_shared<DecodedUnit>(frame));
        std::this_thread::sleep_for(30ms);check(!std::filesystem::exists(std::string(dir)+"/.thumbs/clip.mp4.jpg"),"old frame not captured");
        frame.dec_end_ts=102;thumbnails.capture(std::make_shared<DecodedUnit>(frame));
        std::shared_ptr<const media::Thumbnail> image;
        for(int i=0;i<1000 && !image;++i){thumbnails.capture(std::make_shared<DecodedUnit>(frame));image=thumbnails.get("clip.mp4",213,120);std::this_thread::sleep_for(2ms);}
        check(image && image->width==213 && image->height==120,"async JPEG capture/load/resize");
        check(!thumbnails.get("../escape",320,180),"reject path traversal");
        std::ofstream(std::string(dir)+"/.thumbs/broken.mp4.jpg")<<"broken JPEG";
        check(!thumbnails.get("broken.mp4",320,180),"corrupt preview is a placeholder");
    }
    {
        dvr::Thumbnails thumbnails(dir);std::shared_ptr<const media::Thumbnail> image;
        for(int i=0;i<1000 && !image;++i){image=thumbnails.get("clip.mp4",320,180);std::this_thread::sleep_for(2ms);}
        check(bool(image),"cached thumbnail survives process lifetime");
        thumbnails.arm("removed.mp4");
        for(int i=0;i<20;++i){thumbnails.capture(std::make_shared<DecodedUnit>(frame));std::this_thread::sleep_for(2ms);}
    }
    check(!std::filesystem::exists(std::string(dir)+"/.thumbs/removed.mp4.jpg"),"Late capture cannot recreate a deleted clip's preview");
    puts("PASS: thumbnail capture, JPEG persistence, pre-scaled loading, stale/partial frames and malformed inputs");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
