#include "base/video_pool.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

namespace video_pool {

bus::PicturePool& get() {
    // Never freed (see the header). Reserved, not committed: a buffer only
    // takes memory as far as the largest picture written into it.
    static bus::PicturePool* pool = [] {
        const size_t bytes = (size_t)kPictures * kCapacity;
        void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mem == MAP_FAILED) {
            perror("video_pool: mmap");
            abort();
        }
        return new bus::PicturePool((uint8_t*)mem, kPictures, kCapacity, 0);
    }();
    return *pool;
}

}  // namespace video_pool
