#pragma once
#include <cstdint>
#include <vector>
namespace media {
struct Thumbnail {
    int width=0,height=0;
    std::vector<uint32_t> argb;
};
}
