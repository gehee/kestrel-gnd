#pragma once
#include "dvr/mp4_api.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace dvr {
// Sequential, bounded access to native HEVC recordings. No frame payload cache.
class ExportSource {
  public:
    explicit ExportSource(const std::string &path);
    ~ExportSource();
    ExportSource(const ExportSource &) = delete;
    ExportSource &operator=(const ExportSource &) = delete;
    unsigned width = 0, height = 0, count = 0, timescale = 0;
    uint64_t duration_us = 0;
    std::vector<std::vector<uint8_t>> parameters;
    struct Sample {
        std::vector<uint8_t> bytes;
        uint64_t us = 0;
        unsigned ticks = 0;
        uint8_t nal = 0;
        bool key = false;
    };
    Sample sample(unsigned index) const;

  private:
    int fd_ = -1;
    MP4D_demux_t mp4_{};
    unsigned track_ = 0, length_ = 0;
    std::vector<uint64_t> offsets_;
    void close();
};
} // namespace dvr
