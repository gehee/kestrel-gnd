#include "base/video_pool.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() {
    auto& pool = video_pool::get();
    std::array<bus::PictureRef, video_pool::kPictures> refs;
    std::array<uint8_t*, video_pool::kPictures> addresses{};
    for (int i=0;i<video_pool::kPictures;++i) {
        refs[i]=pool.acquire();
        check(bool(refs[i]), "pool capacity");
        addresses[i]=refs[i]->data();
        memset(addresses[i], 0x7a, 65536);
    }
    for (int i=2;i<video_pool::kPictures;++i) refs[i].reset();
    video_pool::trim();
    check(pool.free_count()==video_pool::kPictures-2, "trim preserves pool accounting");
    for (int i=0;i<video_pool::kPictures;++i) {
        unsigned char resident=0;
        check(mincore(addresses[i], sysconf(_SC_PAGESIZE), &resident)==0, "mincore");
        if (i<2) {
            check(resident & 1, "referenced pages remain resident");
            for (int j=0;j<65536;++j) check(addresses[i][j]==0x7a, "referenced contents preserved");
        } else check(!(resident & 1), "unused pages returned to kernel");
    }
    refs[0].reset(); refs[1].reset();
    video_pool::trim();
    auto reused=pool.acquire();
    check(bool(reused) && reused->data()[0]==0, "discarded slot can be reused");
}
