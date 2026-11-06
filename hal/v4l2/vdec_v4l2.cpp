#include "hal/v4l2/vdec_v4l2.hpp"
#include "base/time_util.h"
#include "base/ltrace.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr unsigned max_buffers = 32, packet_bytes = 4 * 1024 * 1024;
// Native SDIO delivers several complete pictures in a single read. Capacity is
// burst tolerance, not a playout delay: the decoder drains immediately.
// Picture count alone does not measure latency: narrow-band SDIO can deliver
// dozens of tiny pictures together. Age/bytes are the normal backlog limits;
// this count cap only bounds metadata for pathological tiny access units.
constexpr size_t pending_frames = 128, pending_bytes = 8 * 1024 * 1024;
constexpr uint64_t pending_age_us = 150000;
constexpr auto output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
uint64_t now_us() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
int ioctl_retry(int fd, unsigned long op, void *p) {
    int r;
    do {
        r = ioctl(fd, op, p);
    } while (r < 0 && errno == EINTR);
    return r;
}
void checked(int fd, unsigned long op, void *p, const char *name) {
    if (ioctl_retry(fd, op, p) < 0)
        throw std::runtime_error(std::string(name) + ": " + strerror(errno));
}
struct Mapping {
    void *ptr = MAP_FAILED;
    size_t size = 0;
    int prime_fd = -1;
    ~Mapping() {
        if (prime_fd >= 0)
            close(prime_fd);
        if (ptr != MAP_FAILED)
            munmap(ptr, size);
    }
};
struct Device {
    int fd = -1;
    bool output_on = false, capture_on = false;
    ~Device() {
        // STREAMOFF precedes unmapping, including exceptions.
        auto t = output_type;
        if (output_on)
            ioctl_retry(fd, VIDIOC_STREAMOFF, &t);
        t = capture_type;
        if (capture_on)
            ioctl_retry(fd, VIDIOC_STREAMOFF, &t);
        if (fd >= 0)
            close(fd);
    }
};
unsigned allocate(int fd, v4l2_buf_type type, unsigned count,
                  std::array<std::shared_ptr<Mapping>, max_buffers> &maps) {
    v4l2_requestbuffers r{};
    r.type = type;
    r.memory = V4L2_MEMORY_MMAP;
    r.count = count;
    checked(fd, VIDIOC_REQBUFS, &r, "request buffers");
    if (!r.count || r.count > max_buffers)
        throw std::runtime_error("invalid V4L2 buffer count");
    for (unsigned i = 0; i < r.count; ++i) {
        v4l2_plane p{};
        v4l2_buffer b{};
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &p;
        checked(fd, VIDIOC_QUERYBUF, &b, "query buffer");
        maps[i] = std::make_shared<Mapping>();
        maps[i]->size = p.length;
        maps[i]->ptr = mmap(nullptr, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
        if (maps[i]->ptr == MAP_FAILED)
            throw std::runtime_error("map V4L2 buffer");
        if (type == capture_type) {
            v4l2_exportbuffer exported{};
            exported.type = type;
            exported.index = i;
            exported.flags = O_CLOEXEC | O_RDWR;
            checked(fd, VIDIOC_EXPBUF, &exported, "export decoded DMA-BUF");
            maps[i]->prime_fd = exported.fd;
        }
    }
    return r.count;
}
void queue(int fd, v4l2_buf_type type, unsigned index, size_t size, size_t used, uint64_t pts = 0) {
    v4l2_plane p{};
    p.length = size;
    p.bytesused = used;
    v4l2_buffer b{};
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    b.length = 1;
    b.m.planes = &p;
    b.timestamp.tv_sec = pts / 1000000;
    b.timestamp.tv_usec = pts % 1000000;
    checked(fd, VIDIOC_QBUF, &b, "queue buffer");
}
} // namespace
VdecV4l2::VdecV4l2(VideoCodec codec, std::string device, std::shared_ptr<hal::FrameSink> sink,
                   volatile bool *stop)
    : codec_(codec), device_(std::move(device)), sink_(std::move(sink)), stop_(stop) {}
VdecV4l2::~VdecV4l2() { cleanup(); }
void VdecV4l2::cleanup() { quit_ = true; }
void VdecV4l2::feed_packet_to_decoder(void *data, int len, int64_t pts, uint64_t recv, uint8_t nal,
                                      uint32_t enc, uint32_t proc, bool key) {
    if (len <= 0 || unsigned(len) > packet_bytes || quit_ || failed_)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool hevc = codec_ == VideoCodec::H265;
    int parameter = hevc ? (nal >= 32 && nal <= 34 ? nal - 32 : -1) : (nal == 7 ? 0 : nal == 8 ? 1 : -1);
    if (parameter >= 0) {
        if (len <= 65536)
            parameters_[parameter].assign(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + len);
        return;
    }
    if (hevc ? nal > 31 : nal != 1 && nal != 5)
        return;
    key = key || (hevc ? nal >= 16 && nal <= 21 : nal == 5);
    if (!ready_ && !key) {
        ++dropped_;
        return;
    }
    Packet p;
    if (key) {
        const unsigned count = hevc ? 3 : 2;
        for (unsigned i = 0; i < count; ++i)
            if (parameters_[i].empty()) {
                ready_ = false;
                return;
            }
        for (unsigned i = 0; i < count; ++i)
            p.bytes.insert(p.bytes.end(), parameters_[i].begin(), parameters_[i].end());
    }
    if (p.bytes.size() + size_t(len) > packet_bytes) {
        ready_ = false;
        ++dropped_;
        return;
    }
    p.bytes.insert(p.bytes.end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + len);
    size_t bytes = p.bytes.size();
    for (const auto& queued : packets_) bytes += queued.bytes.size();
    const uint64_t age = packets_.empty() ? 0 : now_us() - packets_.front().timing.decode_start_us;
    if (packets_.size() >= pending_frames || bytes > pending_bytes || age > pending_age_us) {
        const auto overflows = ++overflows_;
        if (overflows == 1 || overflows % 10 == 0)
            fprintf(stderr, "v4l2: queue overflow=%llu pending=%zu bytes=%zu oldest=%.1fms; waiting for keyframe\n",
                    (unsigned long long)overflows, packets_.size(), bytes, age / 1000.0);
        dropped_ += packets_.size();
        packets_.clear();
        ready_ = false;
        if (!key) { ++dropped_; return; }
    }
    p.pts = pts;
    p.timing = {recv, now_us(), enc, proc, key, next_slice_times_};
    packets_.push_back(std::move(p));
    if (packets_.size() > peak_pending_) peak_pending_ = packets_.size();
    ready_ = true;
}
void VdecV4l2::run_frame() {
    try {
        decode();
    } catch (const std::exception &e) {
        failed_ = true;
        ready_ = false;
        fprintf(stderr, "v4l2: %s\n", e.what());
    }
    sink_->request_flush();
    printf("v4l2: decoded=%llu dropped=%llu queue_overflows=%llu capture_errors=%llu\n",
           (unsigned long long)frames_.load(), (unsigned long long)dropped_.load(),
           (unsigned long long)overflows_.load(), (unsigned long long)capture_errors_.load());
}
void VdecV4l2::decode() {
    // Mappings outlive the device stream and any frames held by the presenter.
    std::array<std::shared_ptr<Mapping>, max_buffers> out{}, cap{};
    Device dev;
    dev.fd = open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev.fd < 0)
        throw std::runtime_error("cannot open decoder " + device_);
    v4l2_capability caps{};
    checked(dev.fd, VIDIOC_QUERYCAP, &caps, "decoder identity");
    printf("v4l2: driver=%s card=%s\n", caps.driver, caps.card);
    const auto flags = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
    if (!(flags & V4L2_CAP_VIDEO_M2M_MPLANE) || !(flags & V4L2_CAP_STREAMING))
        throw std::runtime_error("decoder requires streaming multiplanar M2M");
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    checked(dev.fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "subscribe source change");
    v4l2_format fmt{};
    fmt.type = output_type;
    fmt.fmt.pix_mp.pixelformat = codec_ == VideoCodec::H265 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.width = 1920;
    fmt.fmt.pix_mp.height = 1080;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = packet_bytes;
    checked(dev.fd, VIDIOC_S_FMT, &fmt, "compressed format");
    const unsigned nout = allocate(dev.fd, output_type, 4, out);
    std::array<bool, max_buffers> busy{}, held{};
    std::unordered_map<uint64_t, timing_stats> stamps;
    unsigned ncap = 0, stride = 0, coded_height = 0, width = 0, height = 0, left = 0, top = 0;
    uint64_t last_progress = now_us(), last_report = last_progress;
    while (!*stop_ && !quit_) {
        // Only this thread dequeues and requeues buffers. frame_ref pins each
        // mmap until the presenter releases it; no ioctl runs from a deleter.
        if (dev.capture_on)
            for (unsigned i = 0; i < ncap; ++i)
                if (held[i] && cap[i].use_count() == 1) {
                    queue(dev.fd, capture_type, i, cap[i]->size, 0);
                    held[i] = false;
                }
        if (dev.output_on)
            for (;;) {
                v4l2_plane p{};
                v4l2_buffer b{};
                b.type = output_type;
                b.memory = V4L2_MEMORY_MMAP;
                b.length = 1;
                b.m.planes = &p;
                if (ioctl_retry(dev.fd, VIDIOC_DQBUF, &b) < 0) {
                    if (errno == EAGAIN)
                        break;
                    throw std::runtime_error("dequeue compressed buffer");
                }
                if (b.index >= nout)
                    throw std::runtime_error("invalid compressed buffer index");
                busy[b.index] = false;
            }
        for (unsigned i = 0; i < nout; ++i)
            if (!busy[i]) {
                Packet p;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (packets_.empty())
                        break;
                    p = std::move(packets_.front());
                    packets_.pop_front();
                }
                const auto wait = now_us() - p.timing.decode_start_us;
                if (wait > peak_wait_us_) peak_wait_us_ = wait;
                if (p.bytes.size() > out[i]->size)
                    throw std::runtime_error("compressed frame exceeds driver buffer");
                memcpy(out[i]->ptr, p.bytes.data(), p.bytes.size());
                // Monotonically unique timestamps associate output with input even
                // when radio PTS is zero; frame timing remains the original values.
                uint64_t stamp = uint64_t(p.pts) + 1;
                stamps[stamp] = p.timing;
                if (stamps.size() > 128)
                    stamps.clear();
                const auto submit = now_us();
                queue(dev.fd, output_type, i, out[i]->size, p.bytes.size(), stamp);
                if (ltrace::on()) ltrace::rec(ltrace::kDecPut, submit, uint32_t(p.pts), now_us());
                busy[i] = true;
                if (!dev.output_on) {
                    auto t = output_type;
                    checked(dev.fd, VIDIOC_STREAMON, &t, "start compressed stream");
                    dev.output_on = true;
                    last_progress = now_us();
                }
            }
        if (!dev.output_on) {
            usleep(2000);
            continue;
        }
        pollfd poller{dev.fd, short(POLLIN | POLLPRI), 0};
        if (poll(&poller, 1, 5) < 0 && errno != EINTR)
            throw std::runtime_error("decoder poll");
        v4l2_event ev{};
        while (ioctl_retry(dev.fd, VIDIOC_DQEVENT, &ev) == 0)
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                // Reconfiguration needs a completely new capture generation. Until
                // that is implemented, fail visibly instead of recycling old buffers.
                if (dev.capture_on)
                    throw std::runtime_error("decoder resolution changed; restart receiver");
                fmt = {};
                fmt.type = capture_type;
                checked(dev.fd, VIDIOC_G_FMT, &fmt, "decoded format");
                fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
                checked(dev.fd, VIDIOC_S_FMT, &fmt, "I420 format");
                v4l2_selection sel{};
                sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                sel.target = V4L2_SEL_TGT_COMPOSE_DEFAULT;
                checked(dev.fd, VIDIOC_G_SELECTION, &sel, "visible rectangle");
                stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
                coded_height = fmt.fmt.pix_mp.height;
                width = sel.r.width;
                height = sel.r.height;
                left = sel.r.left;
                top = sel.r.top;
                if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 || fmt.fmt.pix_mp.num_planes != 1 ||
                    !stride || stride > 8192 || !coded_height || coded_height > 4320 || !width || !height ||
                    ((stride | coded_height | width | height | left | top) & 1) ||
                    uint64_t(left) + width > stride || uint64_t(top) + height > coded_height)
                    throw std::runtime_error("unsupported I420 geometry");
                v4l2_control c{};
                c.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
                checked(dev.fd, VIDIOC_G_CTRL, &c, "capture buffer minimum");
                if (c.value < 1 || c.value > int(max_buffers) - 3)
                    throw std::runtime_error("capture pool too large");
                ncap = allocate(dev.fd, capture_type, c.value + 3, cap);
                for (unsigned i = 0; i < ncap; ++i) {
                    if (cap[i]->size < size_t(stride) * coded_height * 3 / 2)
                        throw std::runtime_error("short capture allocation");
                    queue(dev.fd, capture_type, i, cap[i]->size, 0);
                }
                auto t = capture_type;
                checked(dev.fd, VIDIOC_STREAMON, &t, "start decoded stream");
                dev.capture_on = true;
                printf("v4l2: %ux%u stride=%u coded_height=%u buffers=%u\n", width, height, stride,
                       coded_height, ncap);
            }
        if (dev.capture_on)
            for (;;) {
                v4l2_plane p{};
                v4l2_buffer b{};
                b.type = capture_type;
                b.memory = V4L2_MEMORY_MMAP;
                b.length = 1;
                b.m.planes = &p;
                if (ioctl_retry(dev.fd, VIDIOC_DQBUF, &b) < 0) {
                    if (errno == EAGAIN)
                        break;
                    throw std::runtime_error("dequeue decoded frame");
                }
                if (b.index >= ncap || held[b.index])
                    throw std::runtime_error("invalid capture buffer index");
                held[b.index] = true;
                if (b.flags & V4L2_BUF_FLAG_ERROR) {
                    const auto errors = ++capture_errors_;
                    if (errors == 1 || errors % 10 == 0)
                        fprintf(stderr, "v4l2: capture errors=%llu sequence=%u; waiting for keyframe\n",
                                (unsigned long long)errors, b.sequence);
                    ++dropped_;
                    ready_ = false;
                    continue;
                }
                if (!p.bytesused)
                    continue;
                if (p.bytesused > cap[b.index]->size || p.data_offset > p.bytesused ||
                    p.bytesused - p.data_offset < size_t(stride) * coded_height * 3 / 2)
                    throw std::runtime_error("invalid decoded frame size");
                auto frame = std::make_shared<DecodedUnit>();
                const uint64_t stamp = uint64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
                frame->pts = stamp ? stamp - 1 : 0;
                frame->width = width;
                frame->height = height;
                frame->frame_ref = cap[b.index];
                frame->has_prime_fd = true;
                frame->prime_fd = cap[b.index]->prime_fd;
                frame->drm_pixel_format = V4L2_PIX_FMT_YUV420; // same fourcc as DRM YU12
                frame->pitches[0] = stride;
                frame->pitches[1] = frame->pitches[2] = stride / 2;
                frame->offsets[0] = p.data_offset + top * stride + left;
                frame->offsets[1] =
                    p.data_offset + stride * coded_height + (top / 2) * (stride / 2) + left / 2;
                frame->offsets[2] =
                    p.data_offset + stride * coded_height * 5 / 4 + (top / 2) * (stride / 2) + left / 2;
                auto *pixels = static_cast<uint8_t *>(cap[b.index]->ptr) + p.data_offset;
                frame->data[0] = pixels + top * stride + left;
                frame->data[1] = pixels + stride * coded_height + (top / 2) * (stride / 2) + left / 2;
                frame->data[2] = pixels + stride * coded_height * 5 / 4 + (top / 2) * (stride / 2) + left / 2;
                frame->linesize[0] = stride;
                frame->linesize[1] = frame->linesize[2] = stride / 2;
                frame->dec_end_ts = now_us();
                frame->tx_processing_delay_us = kAirUnknown;
                auto at = stamps.find(stamp);
                if (at != stamps.end()) {
                    const auto &st = at->second;
                    frame->recv_ts = st.recv_start_us;
                    frame->dec_start_ts = st.decode_start_us;
                    frame->tx_encode_delay_us = st.tx_encode_delay_us;
                    frame->tx_processing_delay_us = st.tx_processing_delay_us;
                    frame->is_keyframe = st.is_keyframe;
                    frame->slices = st.slices;
                    stamps.erase(at);
                }
                if (ltrace::on()) ltrace::rec(ltrace::kDecOut, frame->dec_end_ts, uint32_t(frame->pts), 0);
                last_frame_us_ = frame->dec_end_ts;
                sink_->queue_frame(frame);
                ++frames_;
                last_progress = now_us();
            }
        if (now_us() - last_report >= 5000000) {
            last_report = now_us();
            printf("v4l2: decoded=%llu dropped=%llu queue_overflows=%llu capture_errors=%llu peak_pending=%llu peak_wait=%.1fms\n",
                   (unsigned long long)frames_.load(), (unsigned long long)dropped_.load(),
                   (unsigned long long)overflows_.load(), (unsigned long long)capture_errors_.load(),
                   (unsigned long long)peak_pending_.load(), peak_wait_us_.load() / 1000.0);
        }
        // Silence on RF is normal; a decoder with outstanding work must make progress.
        if (now_us() - last_progress > 10000000 &&
            std::any_of(busy.begin(), busy.begin() + nout, [](bool b) { return b; }))
            throw std::runtime_error("decoder stalled for ten seconds");
    }
    sink_->request_flush();
}
