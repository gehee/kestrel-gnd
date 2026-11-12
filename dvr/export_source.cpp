#include "export_source.hpp"
#include "base/video_codec.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace dvr {
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void read_at(int fd, uint64_t off, void *ptr, size_t n) {
    auto *p = static_cast<uint8_t *>(ptr);
    while (n) {
        auto k = pread(fd, p, n, off);
        if (k < 0 && errno == EINTR)
            continue;
        require(k > 0, "Cannot read recording");
        p += k;
        off += k;
        n -= k;
    }
}
uint32_t be(const uint8_t *b) {
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}
// minimp4 does not apply composition offsets. Refuse reordered imports rather
// than silently burn telemetry into the wrong picture. Native DVR has no ctts.
void check_boxes(int fd, uint64_t begin, uint64_t end, unsigned depth = 0) {
    require(depth < 8, "MP4 nesting too deep");
    for (auto at = begin; at < end;) {
        require(end - at >= 8, "Truncated MP4 box");
        uint8_t b[16];
        read_at(fd, at, b, 8);
        uint64_t size = be(b);
        unsigned header = 8;
        if (size == 1) {
            require(end - at >= 16, "Truncated MP4 size");
            read_at(fd, at + 8, b + 8, 8);
            size = uint64_t(be(b + 8)) << 32 | be(b + 12);
            header = 16;
        }
        if (!size)
            size = end - at;
        require(size >= header && size <= end - at, "Invalid MP4 box bounds");
        require(memcmp(b + 4, "ctts", 4) != 0, "Export does not support reordered video");
        if (!memcmp(b + 4, "moov", 4) || !memcmp(b + 4, "trak", 4) || !memcmp(b + 4, "mdia", 4) ||
            !memcmp(b + 4, "minf", 4) || !memcmp(b + 4, "stbl", 4))
            check_boxes(fd, at + header, at + size, depth + 1);
        at += size;
    }
}
} // namespace
void ExportSource::close() {
    MP4D_close(&mp4_);
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}
ExportSource::~ExportSource() { close(); }
ExportSource::ExportSource(const std::string &path) {
    try {
        fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        require(fd_ >= 0, "Cannot open recording");
        struct stat st{};
        require(!fstat(fd_, &st) && S_ISREG(st.st_mode) && st.st_size >= 16,
                "Recording is not a complete regular file");
        check_boxes(fd_, 0, st.st_size);
        require(MP4D_open(
                    &mp4_,
                    [](int64_t off, void *p, size_t n, void *ctx) -> int {
                        try {
                            read_at(*static_cast<int *>(ctx), off, p, n);
                            return 0;
                        } catch (...) {
                            return 1;
                        }
                    },
                    &fd_, st.st_size),
                "Cannot read MP4 index");
        require(mp4_.track_count == 1 && mp4_.track[0].object_type_indication == MP4_OBJECT_TYPE_HEVC,
                "Export requires a native HEVC recording without audio");
        const auto &t = mp4_.track[track_];
        width = t.SampleDescription.video.width;
        height = t.SampleDescription.video.height;
        require(width && height && width <= 1920 && height <= 1080 && !(width % 2) && !(height % 2),
                "Unsupported recording dimensions");
        count = t.sample_count;
        timescale = t.timescale;
        require(count && count <= 2000000 && timescale && t.dsi_bytes >= 23 && t.timestamp && t.duration && t.timestamp_count >= count,
                "Invalid MP4 timing");
        const auto *cfg = t.dsi;
        length_ = (cfg[21] & 3) + 1;
        unsigned at = 23;
        bool seen[3]{};
        for (unsigned a = 0; a < cfg[22]; ++a) {
            require(at + 3 <= t.dsi_bytes, "Truncated HEVC configuration");
            unsigned type = cfg[at++] & 63, n = (cfg[at] << 8) | cfg[at + 1];
            at += 2;
            for (unsigned i = 0; i < n; ++i) {
                require(at + 2 <= t.dsi_bytes, "Truncated HEVC parameter");
                unsigned len = (cfg[at] << 8) | cfg[at + 1];
                at += 2;
                require(len >= 2 && at + len <= t.dsi_bytes, "Invalid HEVC parameter");
                if (type >= 32 && type <= 34) {
                    require(((cfg[at] >> 1) & 63) == type, "HEVC parameter type mismatch");
                    seen[type - 32] = true;
                    std::vector<uint8_t> p{0, 0, 0, 1};
                    p.insert(p.end(), cfg + at, cfg + at + len);
                    parameters.push_back(std::move(p));
                }
                at += len;
            }
        }
        require(seen[0] && seen[1] && seen[2], "Missing HEVC parameters");
        require(t.chunk_count && t.sample_to_chunk_count && t.chunk_offset && t.sample_to_chunk &&
                    t.entry_size && t.sample_to_chunk[0].first_chunk == 1,
                "Invalid MP4 sample index");
        offsets_.resize(count);
        unsigned sample = 0, group = 0;
        for (unsigned chunk = 0; chunk < t.chunk_count && sample < count; ++chunk) {
            if (group + 1 < t.sample_to_chunk_count && chunk + 1 == t.sample_to_chunk[group + 1].first_chunk)
                ++group;
            unsigned n = t.sample_to_chunk[group].samples_per_chunk;
            require(n > 0, "Empty MP4 chunk");
            uint64_t off = t.chunk_offset[chunk];
            for (unsigned j = 0; j < n && sample < count; ++j, ++sample) {
                const auto size = t.entry_size[sample];
                require(size >= 4 && size <= 1024 * 1024 && off <= uint64_t(st.st_size) &&
                            size <= uint64_t(st.st_size) - off,
                        "Invalid MP4 sample bounds");
                require(t.duration[sample] > 0 && t.duration[sample] <= timescale,
                        "Unsupported frame duration");
                require(t.timestamp[sample] ==
                            (sample ? uint64_t(t.timestamp[sample - 1]) + t.duration[sample - 1] : 0),
                        "Discontinuous MP4 timing");
                offsets_[sample] = off;
                off += size;
            }
        }
        require(sample == count, "Incomplete MP4 index");
        duration_us = (uint64_t(t.timestamp[count - 1]) + t.duration[count - 1]) * 1000000 / timescale;
    } catch (...) {
        close();
        throw;
    }
}
ExportSource::Sample ExportSource::sample(unsigned i) const {
    require(i < count, "Export sample out of range");
    const auto &t = mp4_.track[track_];
    std::vector<uint8_t> packet(t.entry_size[i]);
    read_at(fd_, offsets_[i], packet.data(), packet.size());
    Sample s;
    s.us = uint64_t(t.timestamp[i]) * 1000000 / timescale;
    // Round cumulative boundaries, not each frame, to avoid fractional-rate drift.
    s.ticks = ((uint64_t(t.timestamp[i]) + t.duration[i]) * 90000 + timescale / 2) / timescale -
              (uint64_t(t.timestamp[i]) * 90000 + timescale / 2) / timescale;
    require(s.ticks > 0, "Unsupported video timestamp precision");
    bool picture = false;
    for (size_t p = 0; p < packet.size();) {
        require(packet.size() - p >= length_, "Truncated MP4 NAL length");
        unsigned len = 0;
        for (unsigned j = 0; j < length_; ++j)
            len = (len << 8) | packet[p++];
        require(len >= 2 && len <= packet.size() - p, "Invalid MP4 NAL length");
        const auto type = (packet[p] >> 1) & 63;
        if (type <= 31) {
            s.nal = type;
            s.key |= hevc_irap(type);
            picture = true;
        }
        s.bytes.insert(s.bytes.end(), {0, 0, 0, 1});
        s.bytes.insert(s.bytes.end(), packet.begin() + p, packet.begin() + p + len);
        p += len;
    }
    require(picture, "Recording sample has no picture");
    require(i || s.key, "Recording does not start at a keyframe");
    return s;
}
} // namespace dvr
