#include "hal/artosyn/wave5_strips.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "base/ltrace.hpp"
#include "base/time_util.h"

namespace hal {
namespace {
// fpvOS kernel patch 0037: where the decoder's display planes go in each
// capture buffer, in bytes.
constexpr uint32_t kCidY = V4L2_CID_CODEC_BASE | 0x1f01;
constexpr uint32_t kCidCb = V4L2_CID_CODEC_BASE | 0x1f02;
constexpr uint32_t kCidCr = V4L2_CID_CODEC_BASE | 0x1f03;
constexpr auto kOut = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
// A strip's compressed picture at most, and what may wait for its decoder
// (as VdecV4l2: burst tolerance, not a delay - past it, wait for a keyframe).
// The decoder's bitstream ring is 4 times its compressed buffer, both out of
// the codec's memory, which the whole-picture buffers and every strip
// decoder share - and its allocator rounds each up to a power of two: 512 KB
// (a 2 MB ring) per strip. 2 MB each left no room for a third decoder.
constexpr size_t kPacketBytes = 512 * 1024, kPendingBytes = 4 * 1024 * 1024, kPendingPackets = 64;
constexpr uint64_t kPendingAgeUs = 150000;
// Each set of whole-picture buffers its own DecodedUnit::buf_epoch, apart
// from the decoders' (counted from 1).
std::atomic<uint32_t> s_epoch{0};

int xioctl(int fd, unsigned long op, void* arg) {
    int r;
    do r = ioctl(fd, op, arg); while (r < 0 && errno == EINTR);
    return r;
}

// Strip k's decoder as the radio sees it: packets in, the decoding all done
// on Wave5Strips's thread.
class Feeder final : public Vdec {
public:
    Feeder(Wave5Strips& s, int k) : s_(s), k_(k) {}
    void feed_packet_to_decoder(void* data, int len, int64_t pts, uint64_t recv, uint8_t nal, uint32_t enc,
                                uint32_t proc, bool key) override {
        s_.feed(k_, static_cast<const uint8_t*>(data), len, pts, recv, nal, enc, proc, key,
                std::move(next_slice_times_));
    }
    void run_frame() override {}
    void cleanup() override {}
    bool keyframe_in() const override { return s_.keyframe_in(k_); }
    uint64_t last_frame_us() const override { return s_.last_frame_us(); }
private:
    Wave5Strips& s_;
    const int k_;
};
}  // namespace

bool StripLayout::make(int n, int ov, uint32_t w, const uint32_t visible[], const uint32_t coded[],
                       StripLayout* out) {
    // Every plane offset a multiple of 32 bytes (the decoder's rule): rows
    // of w and w/2 bytes at even rows, w a multiple of 64.
    if (n < 2 || n > kMax || ov < 0 || ov > 64 || (ov & 1) || !w || w % 64 || w > 4096) return false;
    StripLayout l;
    l.n = n;
    l.w = w;
    uint32_t y = 0, hp = 0;
    for (int k = 0; k < n; k++) {
        const uint32_t top = k ? ov : 0, bottom = k < n - 1 ? ov : 0;
        if (((visible[k] | coded[k]) & 1) || coded[k] < visible[k] || visible[k] <= top + bottom) return false;
        if (y < top) return false;
        l.origin[k] = y - top;
        hp = std::max(hp, l.origin[k] + coded[k]);
        y += visible[k] - top - bottom;
    }
    if (y > 4320) return false;
    l.h = y;
    l.hp = std::max(hp, y);
    l.plane[0] = 0;
    l.plane[1] = w * l.hp;
    l.plane[2] = w * l.hp + (w / 2) * (l.hp / 2);
    for (int k = 0; k < n; k++) {
        l.off[k][0] = l.origin[k] * w;
        l.off[k][1] = l.plane[1] + l.origin[k] / 2 * (w / 2);
        l.off[k][2] = l.plane[2] + l.origin[k] / 2 * (w / 2);
        for (int i = 0; i < 3; i++)
            if (l.off[k][i] & 31) return false;
    }
    *out = l;
    return true;
}

bool Wave5Strips::available(const std::string& device) {
    const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    v4l2_queryctrl q{};
    q.id = kCidY;
    const bool ok = xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0;
    close(fd);
    return ok;
}

Wave5Strips::Wave5Strips(VideoCodec codec, std::string device, std::shared_ptr<FrameSink> out, MainDecoder main)
    : codec_(codec), device_(std::move(device)), out_(std::move(out)), main_(std::move(main)) {
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    thread_ = std::thread([this] { run(); });
}

Wave5Strips::~Wave5Strips() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
    }
    wake();
    if (thread_.joinable()) thread_.join();
    if (wake_fd_ >= 0) close(wake_fd_);
}

std::unique_ptr<Vdec> Wave5Strips::decoder(int k) {
    if (k < 0 || k >= kMax) return nullptr;
    return std::unique_ptr<Vdec>(new Feeder(*this, k));
}

void Wave5Strips::wake() {
    if (wake_fd_ < 0) return;
    const uint64_t one = 1;
    if (write(wake_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN) { /* already woken */ }
}

void Wave5Strips::arriving(int k, int64_t pic, bool last, int ov) {
    if (k < 0 || k >= kMax || pic < 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    Said& s = said_[k][(uint64_t)pic % 32];
    s.pic = pic;
    s.last = last;
    s.ov = ov;
    if (last && k >= 1) {
        seen_n_ = k + 1;
        seen_ov_ = ov;
    }
}

void Wave5Strips::drop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        drop_req_ = true;
    }
    wake();
}

void Wave5Strips::stopped() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_req_ = true;
    }
    wake();
}

bool Wave5Strips::keyframe_in(int k) const {
    if (k < 0 || k >= kMax) return true;
    std::lock_guard<std::mutex> lk(mtx_);
    return in_[k].ready;
}

// As VdecV4l2: parameter sets kept and put before each keyframe, nothing
// before a keyframe, and a backlog past its limits dropped for the next one.
void Wave5Strips::feed(int k, const uint8_t* data, int len, int64_t pts, uint64_t recv, uint8_t nal, uint32_t enc,
                       uint32_t proc, bool key, SliceTimesPtr times) {
    if (k < 0 || k >= kMax || len <= 0 || (size_t)len > kPacketBytes) return;
    const bool hevc = codec_ == VideoCodec::H265;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (quit_) return;
        In& in = in_[k];
        const int param = hevc ? (nal >= 32 && nal <= 34 ? nal - 32 : -1) : (nal == 7 ? 0 : nal == 8 ? 1 : -1);
        if (param >= 0) {
            if (len <= 65536) in.params[param].assign(data, data + len);
            return;
        }
        if (hevc ? nal > 31 : nal != 1 && nal != 5) return;
        key = key || keyframe_nal(codec_, nal);
        if (!in.ready && !key) return;
        Packet p;
        if (key) {
            const unsigned count = hevc ? 3 : 2;
            for (unsigned i = 0; i < count; i++)
                if (in.params[i].empty()) {
                    in.ready = false;
                    return;
                }
            for (unsigned i = 0; i < count; i++) p.bytes.insert(p.bytes.end(), in.params[i].begin(), in.params[i].end());
        }
        if (p.bytes.size() + (size_t)len > kPacketBytes) {
            in.ready = false;
            return;
        }
        p.bytes.insert(p.bytes.end(), data, data + len);
        size_t bytes = p.bytes.size();
        for (const Packet& q : in.packets) bytes += q.bytes.size();
        const uint64_t now = get_time_us();
        const uint64_t age = in.packets.empty() ? 0 : now - in.packets.front().timing.decode_start_us;
        if (in.packets.size() >= kPendingPackets || bytes > kPendingBytes || age > kPendingAgeUs) {
            in.packets.clear();
            in.ready = false;
            if (!key) return;
        }
        p.pts = pts;
        p.timing = {recv, now, enc, proc, key, std::move(times)};
        in.packets.push_back(std::move(p));
        in.ready = true;
    }
    wake();
}

void Wave5Strips::begin() {
    active_ = true;
    flushed_ = false;
    n_ = ov_ = 0;
    begin_us_ = get_time_us();
    alloc_since_us_ = 0;
    main_.hold(true);
    printf("strips: decoded in place - the whole-picture decoder lets go of the hardware meanwhile\n");
}

void Wave5Strips::close_dev(Dev& d) {
    if (d.fd >= 0) {
        auto t = kOut;
        if (d.out_on) xioctl(d.fd, VIDIOC_STREAMOFF, &t);
        t = kCap;
        if (d.cap_on) xioctl(d.fd, VIDIOC_STREAMOFF, &t);
        if (d.out && d.out != MAP_FAILED) munmap(d.out, d.out_size);
        close(d.fd);
    }
    d = Dev();
}

void Wave5Strips::teardown(bool for_good, bool exiting) {
    for (Dev& d : dev_) close_dev(d);
    // The screen lets go of the pictures from these buffers first (a picture
    // it shows holds its buffer), then the buffers go.
    if (!slot_.empty() && !exiting) out_->request_flush();
    free_slots();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (In& in : in_) {
            in.packets.clear();
            in.ready = false;
        }
    }
    n_ = ov_ = 0;
    clean_ = false;
    newest_ = -1;
    alloc_since_us_ = 0;
    if (for_good && active_) {
        active_ = false;
        flushed_ = false;
        if (!exiting) {
            // The screen drops its framebuffers of buffers no one holds at
            // its next turn (2 ms); the memory is free once it has.
            usleep(50000);
            main_.hold(false);
        }
    }
}

bool Wave5Strips::open_dev(int k) {
    Dev& d = dev_[k];
    d.fd = open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d.fd < 0) {
        perror("strips: decoder");
        return false;
    }
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    v4l2_format f{};
    f.type = kOut;
    f.fmt.pix_mp.pixelformat = codec_ == VideoCodec::H265 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    // The size only sets the driver's least compressed buffer
    // (width x height x 3/8): under kPacketBytes, which is then the size.
    f.fmt.pix_mp.width = 1920;
    f.fmt.pix_mp.height = 720;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = kPacketBytes;
    v4l2_requestbuffers r{};
    r.type = kOut;
    r.memory = V4L2_MEMORY_MMAP;
    r.count = 1;
    v4l2_plane p{};
    v4l2_buffer b{};
    b.type = kOut;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 0;
    b.length = 1;
    b.m.planes = &p;
    if (xioctl(d.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) || xioctl(d.fd, VIDIOC_S_FMT, &f) ||
        xioctl(d.fd, VIDIOC_REQBUFS, &r) || r.count < 1 || xioctl(d.fd, VIDIOC_QUERYBUF, &b) ||
        p.length < 65536) {
        fprintf(stderr, "strips: strip %d's decoder: %s\n", k, strerror(errno));
        close_dev(d);
        return false;
    }
    d.out_size = p.length;
    d.out = mmap(nullptr, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, p.m.mem_offset);
    if (d.out == MAP_FAILED) {
        perror("strips: compressed buffer");
        d.out = nullptr;
        close_dev(d);
        return false;
    }
    return true;
}

// Into the decoder's one compressed buffer, stamped with its picture.
static bool queue_packet(int fd, void* map, size_t size, const std::vector<uint8_t>& bytes, int64_t pts, bool* on) {
    if (bytes.size() > size) {
        fprintf(stderr, "strips: a %zu-byte strip does not fit the decoder's %zu-byte buffer\n", bytes.size(), size);
        return false;
    }
    memcpy(map, bytes.data(), bytes.size());
    v4l2_plane p{};
    p.length = size;
    p.bytesused = bytes.size();
    v4l2_buffer b{};
    b.type = kOut;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 0;
    b.length = 1;
    b.m.planes = &p;
    const uint64_t stamp = (uint64_t)pts + 1;
    b.timestamp.tv_sec = stamp / 1000000;
    b.timestamp.tv_usec = stamp % 1000000;
    if (xioctl(fd, VIDIOC_QBUF, &b)) return false;
    if (!*on) {
        auto t = kOut;
        if (xioctl(fd, VIDIOC_STREAMON, &t)) return false;
        *on = true;
    }
    return true;
}

static bool queue_slot(int fd, unsigned index, int dmabuf, size_t bytes) {
    v4l2_plane p{};
    p.m.fd = dmabuf;
    p.length = bytes;
    v4l2_buffer b{};
    b.type = kCap;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = index;
    b.length = 1;
    b.m.planes = &p;
    return xioctl(fd, VIDIOC_QBUF, &b) == 0;
}

// Before its decoder has the buffers: its stream from the first keyframe on,
// for the decoder to read the size from (the source change) and - once the
// buffers are queued, all of them - to register them at its next picture.
// Fed until that picture is out: the registration needs a strip to decode
// with the buffers there. What it decodes until then is not shown.
void Wave5Strips::boot_feed(int k) {
    Dev& d = dev_[k];
    if (d.out_busy || d.registered) return;
    Packet p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (in_[k].packets.empty()) return;
        p = std::move(in_[k].packets.front());
        in_[k].packets.pop_front();
    }
    if (!queue_packet(d.fd, d.out, d.out_size, p.bytes, p.pts, &d.out_on)) {
        fprintf(stderr, "strips: strip %d's first pictures: %s\n", k, strerror(errno));
        teardown(false);
        return;
    }
    d.out_busy = true;
    if (!d.boot_fed++) d.first_fed_us = get_time_us();
}

// Registered, and every strip fed since out (or none for 100 ms): every
// buffer back - a capture STREAMOFF during a decode leaves the firmware
// failing - and from now on one buffer of ours a strip.
void Wave5Strips::boot_drained(int k, uint64_t now) {
    Dev& d = dev_[k];
    if (d.exact || !d.registered || d.out_busy) return;
    if (d.boot_out < d.boot_fed && now < d.boot_us + 100000) return;
    auto t = kCap;
    if (xioctl(d.fd, VIDIOC_STREAMOFF, &t) || xioctl(d.fd, VIDIOC_STREAMON, &t)) {
        fprintf(stderr, "strips: strip %d's decoder after its first pictures: %s\n", k, strerror(errno));
        teardown(false);
        return;
    }
    for (Slot& s : slot_) s.queued &= ~(1u << k);
    d.exact = true;
}

void Wave5Strips::source_change(int k) {
    Dev& d = dev_[k];
    if (d.cap_on) {
        printf("strips: strip %d's stream changed size - set up again\n", k);
        teardown(false);
        return;
    }
    v4l2_format f{};
    f.type = kCap;
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE_DEFAULT;
    v4l2_control c{};
    c.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    bool ok = xioctl(d.fd, VIDIOC_G_FMT, &f) == 0;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    ok = ok && xioctl(d.fd, VIDIOC_S_FMT, &f) == 0 && xioctl(d.fd, VIDIOC_G_SELECTION, &sel) == 0 &&
         xioctl(d.fd, VIDIOC_G_CTRL, &c) == 0;
    // The strips share one buffer of one pitch: the decoder's rows exactly
    // the picture's width, from its first row.
    const uint32_t w = sel.r.width;
    if (!ok || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 || f.fmt.pix_mp.num_planes != 1 ||
        f.fmt.pix_mp.plane_fmt[0].bytesperline != w || f.fmt.pix_mp.width != w || sel.r.left || sel.r.top ||
        !sel.r.height || sel.r.height > f.fmt.pix_mp.height || c.value < 1 || c.value > 16) {
        fprintf(stderr, "strips: strip %d's pictures (%ux%u in %ux%u, pitch %u) cannot be decoded in place\n", k,
                sel.r.width, sel.r.height, f.fmt.pix_mp.width, f.fmt.pix_mp.height,
                f.fmt.pix_mp.plane_fmt[0].bytesperline);
        teardown(true);
        retry_after_us_ = get_time_us() + 10000000;
        return;
    }
    d.width = w;
    d.visible = sel.r.height;
    d.coded = f.fmt.pix_mp.height;
    d.min_bufs = c.value;
    d.src = true;
}

void Wave5Strips::free_slots() {
    for (Slot& s : slot_) {
        if (s.map) munmap(s.map, layout_.bytes());
        if (s.fd >= 0) close(s.fd);
    }
    slot_.clear();
    // The buffers' memory stays while anything holds their DMA-BUFs (the
    // screen's framebuffers until it lets go).
    if (alloc_fd_ >= 0) {
        v4l2_requestbuffers r{};
        r.type = kOut;
        r.memory = V4L2_MEMORY_MMAP;
        r.count = 0;
        xioctl(alloc_fd_, VIDIOC_REQBUFS, &r);
        close(alloc_fd_);
        alloc_fd_ = -1;
    }
}

// The whole-picture buffers, from the codec's own memory: the compressed
// buffers of a decoder instance kept for that, never streamed (their size is
// the one asked for), exported as DMA-BUFs for the strips' decoders and the
// screen. Not the screen's dumb buffers: those come out of CMA, which the
// screen's planes all but fill, and then out of the receiver's RAM - the
// whole-picture decoder letting go of its buffers frees codec memory only.
bool Wave5Strips::alloc_slots(int count) {
    free_slots();
    const size_t bytes = layout_.bytes();
    alloc_fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (alloc_fd_ < 0) return false;
    v4l2_format f{};
    f.type = kOut;
    f.fmt.pix_mp.pixelformat = codec_ == VideoCodec::H265 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = layout_.w;
    f.fmt.pix_mp.height = layout_.h;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = bytes;
    v4l2_requestbuffers r{};
    r.type = kOut;
    r.memory = V4L2_MEMORY_MMAP;
    r.count = count;
    if (xioctl(alloc_fd_, VIDIOC_S_FMT, &f) || f.fmt.pix_mp.plane_fmt[0].sizeimage < bytes ||
        xioctl(alloc_fd_, VIDIOC_REQBUFS, &r) || r.count != (unsigned)count) {
        free_slots();
        return false;
    }
    slot_.resize(count);
    for (int i = 0; i < count; i++) {
        Slot& s = slot_[i];
        v4l2_plane p{};
        v4l2_buffer b{};
        b.type = kOut;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &p;
        v4l2_exportbuffer e{};
        e.type = kOut;
        e.index = i;
        e.flags = O_CLOEXEC | O_RDWR;
        if (xioctl(alloc_fd_, VIDIOC_QUERYBUF, &b) || p.length < bytes || xioctl(alloc_fd_, VIDIOC_EXPBUF, &e)) {
            fprintf(stderr, "strips: picture buffer %d: %s\n", i, strerror(errno));
            free_slots();
            return false;
        }
        s.fd = e.fd;
        void* map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, alloc_fd_, p.m.mem_offset);
        if (map == MAP_FAILED) {
            free_slots();
            return false;
        }
        s.map = static_cast<uint8_t*>(map);
        // Black, for rows no strip writes (none, but a picture given up).
        memset(s.map, 0x10, (size_t)layout_.w * layout_.hp);
        memset(s.map + layout_.plane[1], 0x80, (size_t)layout_.w * layout_.hp / 2);
        s.owner = std::make_shared<int>(0);
    }
    epoch_ = 0x80000000u | (++s_epoch & 0x7fffffffu);
    return true;
}

// Once every strip's size is known and the whole-picture decoder has let go:
// the buffers, then each decoder given its rows and every buffer (the
// firmware registers the buffers queued at its first picture).
bool Wave5Strips::setup() {
    for (int k = 0; k < n_; k++)
        if (dev_[k].fd < 0 || !dev_[k].src) return false;
    if (!main_.released()) return false;
    const uint64_t now = get_time_us();
    if (!flushed_) {
        out_->request_flush();   // its last picture off the screen: its buffers free
        flushed_ = true;
    }
    if (!alloc_since_us_) {
        uint32_t visible[kMax], coded[kMax];
        int min_bufs = 0;
        for (int k = 0; k < n_; k++) {
            visible[k] = dev_[k].visible;
            coded[k] = dev_[k].coded;
            min_bufs = std::max(min_bufs, dev_[k].min_bufs);
            if (dev_[k].width != dev_[0].width) {
                fprintf(stderr, "strips: strips of different widths (%u, %u) - not decoded in place\n",
                        dev_[0].width, dev_[k].width);
                teardown(true);
                retry_after_us_ = now + 10000000;
                return false;
            }
        }
        if (!StripLayout::make(n_, ov_, dev_[0].width, visible, coded, &layout_)) {
            fprintf(stderr, "strips: %d strips of %u rows (%d past each join) do not stack into one buffer\n", n_,
                    visible[0], ov_);
            teardown(true);
            retry_after_us_ = now + 10000000;
            return false;
        }
        alloc_since_us_ = now;
        // Buffers: one strip being decoded in each decoder, a picture on the
        // screen and the one before it (its scan-out may still read it), one
        // spare; and more than the decoder asks for - with no more than its
        // minimum queued the firmware never decodes the picture that
        // registers them (measured: 5 of a minimum of 5 stall, 6 run).
        slot_count_ = std::max(min_bufs + 1, n_ + 3);
    }
    // The whole-picture decoder's buffers come free as the screen lets go
    // of its framebuffers: tried until then.
    if (!alloc_slots(slot_count_)) {
        if (now - alloc_since_us_ > 3000000) {
            fprintf(stderr, "strips: no memory for %d %ux%u picture buffers (%.1f MB) - whole pictures only\n",
                    slot_count_, layout_.w, layout_.h, slot_count_ * layout_.bytes() / 1e6);
            teardown(true);
            retry_after_us_ = now + 30000000;
        }
        return false;
    }
    for (int k = 0; k < n_; k++) {
        Dev& d = dev_[k];
        v4l2_control c{};
        bool ok = true;
        const uint32_t ids[3] = {kCidY, kCidCb, kCidCr};
        for (int i = 0; i < 3 && ok; i++) {
            c.id = ids[i];
            c.value = (int)layout_.off[k][i];
            ok = xioctl(d.fd, VIDIOC_S_CTRL, &c) == 0;
        }
        v4l2_requestbuffers r{};
        r.type = kCap;
        r.memory = V4L2_MEMORY_DMABUF;
        r.count = slot_.size();
        ok = ok && xioctl(d.fd, VIDIOC_REQBUFS, &r) == 0 && r.count == slot_.size();
        for (unsigned i = 0; i < slot_.size() && ok; i++) {
            ok = queue_slot(d.fd, i, slot_[i].fd, layout_.bytes());
            slot_[i].queued |= 1u << k;
        }
        auto t = kCap;
        ok = ok && xioctl(d.fd, VIDIOC_STREAMON, &t) == 0;
        if (!ok) {
            fprintf(stderr, "strips: strip %d's decoder on the shared buffers: %s\n", k, strerror(errno));
            teardown(true);
            retry_after_us_ = now + 10000000;
            return false;
        }
        d.cap_on = true;
        d.boot_us = now;
    }
    printf("strips: %d strips of %ux%u (%d rows past each join) decoded in place into %zu %ux%u buffers "
           "(%.1f MB), %.0f ms after the first strip\n",
           n_, layout_.w, dev_[0].visible, ov_, slot_.size(), layout_.w, layout_.h,
           slot_.size() * layout_.bytes() / 1e6, (now - begin_us_) / 1000.0);
    return true;
}

// The buffer strip k of picture pic is written into: the picture's own, or a
// free one (or the oldest picture's that no decoder has now, given up for it).
int Wave5Strips::slot_for(int64_t pic, int k) {
    const unsigned bit = 1u << k;
    // The air unit counting from 0 again (its kestrel-air started again).
    if (newest_ >= 0 && pic + 64 < newest_) newest_ = -1;
    for (size_t i = 0; i < slot_.size(); i++)
        if (slot_[i].pic == pic && !((slot_[i].queued | slot_[i].got) & bit)) return (int)i;
    auto take = [&](size_t i) {
        Slot& s = slot_[i];
        s.pic = pic;
        s.got = 0;
        // Too late to be shown (a newer picture is): decoded all the same,
        // the decoder needs it for the strips after it.
        s.given_up = pic <= newest_;
        return (int)i;
    };
    for (size_t i = 0; i < slot_.size(); i++)
        if (slot_free(slot_[i])) return take(i);
    int best = -1;
    for (size_t i = 0; i < slot_.size(); i++) {
        const Slot& s = slot_[i];
        if (s.pic >= 0 && s.pic < pic && !s.queued && s.owner.use_count() == 1 &&
            (best < 0 || s.pic < slot_[best].pic))
            best = (int)i;
    }
    if (best < 0) return -1;
    if (!slot_[best].given_up) incomplete_++;
    return take(best);
}

// The next strip into strip k's decoder, with its picture's buffer.
bool Wave5Strips::exact_feed(int k) {
    Dev& d = dev_[k];
    if (!d.exact || d.flight >= 0 || d.out_busy) return false;
    int64_t pts;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (in_[k].packets.empty()) return false;
        pts = in_[k].packets.front().pts;
    }
    const int s = slot_for(pts, k);
    if (s < 0) {
        if (d.waiting != pts) {
            d.waiting = pts;
            waits_++;
        }
        return false;
    }
    d.waiting = -1;
    Packet p;
    {
        // The radio thread only adds behind it: the same packet.
        std::lock_guard<std::mutex> lk(mtx_);
        if (in_[k].packets.empty() || in_[k].packets.front().pts != pts) return false;
        p = std::move(in_[k].packets.front());
        in_[k].packets.pop_front();
    }
    if (!queue_slot(d.fd, (unsigned)s, slot_[s].fd, layout_.bytes()) ||
        !queue_packet(d.fd, d.out, d.out_size, p.bytes, p.pts, &d.out_on)) {
        fprintf(stderr, "strips: strip %d to its decoder: %s - set up again\n", k, strerror(errno));
        teardown(false);
        return false;
    }
    slot_[s].queued |= 1u << k;
    if (ltrace::on()) {
        ltrace::rec(ltrace::kStripIn, p.timing.decode_start_us, (uint32_t)p.pts,
                    (uint64_t)k | p.timing.recv_start_us << 8);
        ltrace::rec(ltrace::kStripFed, get_time_us(), (uint32_t)p.pts, (uint64_t)k);
    }
    d.out_busy = true;
    d.flight = p.pts;
    d.flight_slot = s;
    d.flight_us = get_time_us();
    d.timing = p.timing;
    return true;
}

void Wave5Strips::service(int k) {
    Dev& d = dev_[k];
    v4l2_event ev{};
    while (d.fd >= 0 && xioctl(d.fd, VIDIOC_DQEVENT, &ev) == 0)
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) source_change(k);
    if (d.fd < 0) return;
    for (;;) {
        v4l2_plane p{};
        v4l2_buffer b{};
        b.type = kOut;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = 1;
        b.m.planes = &p;
        if (xioctl(d.fd, VIDIOC_DQBUF, &b)) break;
        d.out_busy = false;
    }
    while (d.fd >= 0 && d.cap_on) {
        v4l2_plane p{};
        v4l2_buffer b{};
        b.type = kCap;
        b.memory = V4L2_MEMORY_DMABUF;
        b.length = 1;
        b.m.planes = &p;
        if (xioctl(d.fd, VIDIOC_DQBUF, &b)) break;
        const uint64_t stamp = (uint64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
        captured(k, b.index, stamp ? (int64_t)stamp - 1 : -1, (b.flags & V4L2_BUF_FLAG_ERROR) || !p.bytesused);
    }
}

void Wave5Strips::captured(int k, unsigned index, int64_t pic, bool error) {
    Dev& d = dev_[k];
    const unsigned bit = 1u << k;
    if (!d.exact) {
        // Written while every buffer was queued (the firmware registering
        // them), wherever it chose: not shown.
        if (index < slot_.size()) slot_[index].queued &= ~bit;
        d.registered = true;
        d.boot_out++;
        d.boot_us = get_time_us();
        return;
    }
    if (index >= slot_.size() || (int)index != d.flight_slot || pic != d.flight) {
        fprintf(stderr, "strips: strip %d gave picture %lld in buffer %u for picture %lld in buffer %d - set up again\n",
                k, (long long)pic, index, (long long)d.flight, d.flight_slot);
        teardown(false);
        return;
    }
    Slot& s = slot_[index];
    const uint64_t now = get_time_us();
    s.queued &= ~bit;
    if (ltrace::on()) ltrace::rec(ltrace::kStripOut, now, (uint32_t)pic, (uint64_t)k);
    decodes_++;
    decode_us_ += now - d.flight_us;
    d.flight = -1;
    d.flight_slot = -1;
    last_frame_us_ = now;
    if (error) {
        errors_++;
        if (!s.given_up) incomplete_++;
        s.given_up = true;
    } else {
        s.got |= bit;
        s.t[k] = d.timing;
        s.done_us[k] = now;
    }
    if (!s.given_up && s.got == (1u << n_) - 1) {
        finish(s);
        return;
    }
    if (s.given_up && !s.queued) s.pic = -1;
}

// Pictures older than a shown one that will not be: given up, their
// buffers free once no decoder has them.
void Wave5Strips::retire_before(int64_t pic) {
    for (Slot& s : slot_) {
        if (s.pic < 0 || s.pic >= pic) continue;
        if (!s.given_up) incomplete_++;
        s.given_up = true;
        if (!s.queued) s.pic = -1;
    }
}

void Wave5Strips::finish(Slot& s) {
    const StripLayout& l = layout_;
    auto du = std::make_shared<DecodedUnit>();
    const timing_stats& top = s.t[0];
    bool key = true;
    uint64_t done = 0;
    for (int k = 0; k < n_; k++) {
        key = key && s.t[k].is_keyframe;
        done = std::max(done, s.done_us[k]);
    }
    du->pts = (uint64_t)s.pic;
    du->width = l.w;
    du->height = l.h;
    du->recv_ts = top.recv_start_us;
    du->dec_start_ts = top.decode_start_us;
    du->dec_end_ts = done;
    du->tx_encode_delay_us = top.tx_encode_delay_us;
    du->tx_processing_delay_us = top.tx_processing_delay_us;
    // A keyframe where every strip is one, and the first whole picture after
    // a start too (each decoder began on a keyframe of its own; the screen
    // goes live on a keyframe) - as the other joiners.
    du->is_keyframe = key || !clean_;
    clean_ = true;
    du->has_prime_fd = true;
    du->prime_fd = s.fd;
    du->drm_pixel_format = DRM_FORMAT_YUV420;
    du->pitches[0] = l.w;
    du->pitches[1] = du->pitches[2] = l.w / 2;
    for (int i = 0; i < 3; i++) {
        du->offsets[i] = l.plane[i];
        du->data[i] = s.map + l.plane[i];
    }
    du->linesize[0] = (int)l.w;
    du->linesize[1] = du->linesize[2] = (int)(l.w / 2);
    du->frame_ref = s.owner;
    du->buf_epoch = epoch_;
    const SliceTimes* t0 = top.slices.get();
    if (t0 && t0->frame_cap_us && t0->height) {
        auto st = std::make_shared<SliceTimes>();
        st->height = l.h;
        st->frame_cap_us = t0->frame_cap_us;
        st->readout_us = t0->readout_us;
        st->exposure_us = t0->exposure_us;
        st->key = key;
        st->pts = s.pic;
        st->expected = n_;
        for (int k = 0; k < n_; k++) {
            const uint32_t y = k ? l.origin[k] + (uint32_t)ov_ : 0;
            const SliceTimes* t = s.t[k].slices.get();
            SliceTimes::Slice& sl = st->s[k];
            sl.row = y;
            sl.cap_us = t0->frame_cap_us + (l.h ? (uint64_t)y * t0->readout_us / l.h : 0);
            if (t && t->n.load() > 0) {
                sl.out_us = t->s[0].out_us;
                sl.here_us = t->s[0].here_us;
                sl.in_us.store(t->s[0].in_us.load());
            }
            sl.done_us.store(s.done_us[k]);
        }
        st->n.store(n_);
        st->complete.store(true);
        du->slices = st;
    }
    const int64_t pic = s.pic;
    newest_ = pic;
    s.pic = -1;
    joined_++;
    retire_before(pic);
    out_->queue_frame(std::move(du));
}

void Wave5Strips::log_stats() {
    const uint64_t now = get_time_us();
    if (!log_us_) log_us_ = now;
    if (now - log_us_ < 5000000) return;
    if (active_ && slot_.empty()) {
        // Still setting up: what it waits for.
        printf("strips: setting up %d strips -", n_);
        for (int k = 0; k < n_; k++) {
            const Dev& d = dev_[k];
            size_t queued;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                queued = in_[k].packets.size();
            }
            printf(" strip %d %s (%zu waiting)", k, d.fd < 0 ? "no decoder" : d.src ? "size known" : "no size yet",
                   queued);
        }
        printf("; whole-picture decoder %s\n", main_.released() ? "let go" : "still running");
    }
    if (joined_ || incomplete_ || decodes_)
        printf("strips: %lu pictures of %d strips in 5 s, %lu given up (a strip lost or late), %lu waits for a "
               "buffer, %lu decode errors; %.2f ms a strip\n",
               joined_, n_, incomplete_, waits_, errors_, decodes_ ? decode_us_ / 1000.0 / decodes_ : 0.0);
    if (loops_ > 100000) {   // ~4 a strip is the norm (2400 at 2 strips): flag a busy loop
        printf("strips: busy - %lu loops in 5 s; woken by packets %lu", loops_, woke_[0]);
        for (int k = 0; k < n_; k++) printf(", strip %d's decoder %lu (events 0x%x)", k, woke_[k + 1], woke_ev_[k + 1]);
        printf("\n");
    }
    loops_ = 0;
    for (auto& w : woke_) w = 0;
    for (auto& e : woke_ev_) e = 0;
    joined_ = incomplete_ = waits_ = errors_ = decodes_ = 0;
    decode_us_ = 0;
    log_us_ = now;
}

void Wave5Strips::run() {
    pthread_setname_np(pthread_self(), "strips");
    for (;;) {
        bool quit, stop, dropped, any = false;
        int n, ov;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit = quit_;
            stop = stop_req_;
            dropped = drop_req_;
            stop_req_ = drop_req_ = false;
            n = seen_n_;
            ov = seen_ov_;
            for (const In& in : in_) any = any || !in.packets.empty();
        }
        if (quit) break;
        const uint64_t now = get_time_us();
        if (stop) {
            if (active_) {
                teardown(true);
                printf("strips: whole pictures again - the whole-picture decoder has the hardware back\n");
            }
            std::lock_guard<std::mutex> lk(mtx_);
            seen_n_ = 0;
            for (In& in : in_) in.packets.clear();
            continue;
        }
        if (dropped)
            for (Slot& s : slot_) {
                if (s.pic < 0) continue;
                if (!s.given_up) incomplete_++;
                s.given_up = true;
                if (!s.queued) s.pic = -1;
            }
        if (!active_) {
            if (any && n >= 2 && now >= retry_after_us_) {
                begin();
            } else {
                if (any && n < 2) {
                    // No count yet: nothing of these strips kept.
                    std::lock_guard<std::mutex> lk(mtx_);
                    for (In& in : in_) in.packets.clear();
                }
                pollfd pf{wake_fd_, POLLIN, 0};
                poll(&pf, 1, 50);
                uint64_t v;
                if (read(wake_fd_, &v, sizeof(v)) < 0) { /* not woken */ }
                continue;
            }
        }
        if (n_ && n >= 2 && (n != n_ || ov != ov_)) {
            printf("strips: %d strips (%d rows past each join) now, not %d (%d) - set up again\n", n, ov, n_, ov_);
            teardown(false);
        }
        if (!n_ && n >= 2) {
            n_ = n;
            ov_ = ov;
        }
        // Decoders opened once the whole-picture one has closed: a decoder
        // instance closing while others run can leave them stalled.
        const bool main_gone = main_.released();
        for (int k = 0; k < n_ && main_gone; k++) {
            Dev& d = dev_[k];
            bool has;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                has = !in_[k].packets.empty();
            }
            if (d.fd < 0 && has && !open_dev(k)) {
                teardown(true);
                retry_after_us_ = now + 10000000;
                break;
            }
            if (d.fd >= 0 && !d.exact) boot_feed(k);
            // A decoder that finds no stream start in what it was given holds
            // that strip for good (its one compressed buffer): opened again,
            // from the next keyframe.
            if (d.fd >= 0 && !d.src && d.boot_fed && get_time_us() > d.first_fed_us + 500000) {
                fprintf(stderr, "strips: strip %d's decoder found no stream start in 0.5 s - opened again\n", k);
                close_dev(d);
                std::lock_guard<std::mutex> lk(mtx_);
                in_[k].packets.clear();
                in_[k].ready = false;
            }
        }
        if (n_ && slot_.empty()) setup();
        bool exact = n_ > 0 && !slot_.empty();
        for (int k = 0; k < n_ && exact; k++) exact = dev_[k].exact;
        // Strips to their decoders once every decoder is past its first
        // picture (which may land in any buffer).
        if (exact)
            for (int k = 0; k < n_; k++) exact_feed(k);
        const uint64_t t = get_time_us();   // after the feeding above stamped its strips
        for (int k = 0; k < n_; k++) {
            const Dev& d = dev_[k];
            // A strip that never comes out; or no first picture a second
            // after the buffers (strips coming all along).
            if ((d.cap_on && d.flight >= 0 && t > d.flight_us + 1000000) ||
                (d.cap_on && !d.registered && t > d.boot_us + 1000000)) {
                fprintf(stderr, "strips: strip %d's decoder stalled - set up again (", k);
                for (int j = 0; j < n_; j++)
                    fprintf(stderr, "%sstrip %d: %u in, %u out, input %s, %s", j ? "; " : "", j, dev_[j].boot_fed,
                            dev_[j].boot_out, dev_[j].out_busy ? "busy" : "free",
                            dev_[j].exact ? "running" : dev_[j].registered ? "draining" : "not registered");
                fprintf(stderr, ")\n");
                teardown(false);
                break;
            }
        }
        // The decoders' events and pictures, or a packet: whichever first.
        // A decoder without its capture stream would only poll as an error,
        // and so does one with no buffer queued on either side (the mem2mem
        // poll's POLLERR, at once): between strips in exact mode that is the
        // usual state, and polling it spun this thread at a whole core,
        // starving the radio's reads (lost video). Only decoders holding a
        // buffer are polled; the rest wait for a packet's wake or the timeout.
        pollfd pf[kMax + 1];
        int cnt = 0;
        pf[cnt++] = {wake_fd_, POLLIN, 0};
        for (int k = 0; k < n_; k++) {
            const Dev& d = dev_[k];
            if (d.fd < 0 || !d.cap_on) continue;
            bool held = d.out_busy;
            for (const Slot& s : slot_) held = held || (s.queued >> k & 1);
            if (held || !d.exact) pf[cnt++] = {d.fd, short(POLLIN | POLLPRI), 0};
        }
        int who[kMax + 1] = {0};
        for (int i = 1, k = 0; k < n_; k++)
            if (i < cnt && pf[i].fd == dev_[k].fd) who[i++] = k + 1;
        poll(pf, cnt, cnt > 1 ? 5 : 2);
        ++loops_;
        for (int i = 0; i < cnt; i++)
            if (pf[i].revents) { ++woke_[who[i]]; woke_ev_[who[i]] |= pf[i].revents; }
        uint64_t v;
        if (read(wake_fd_, &v, sizeof(v)) < 0) { /* not woken */ }
        for (int k = 0; k < n_; k++)
            if (dev_[k].fd >= 0) service(k);
        for (int k = 0; k < n_; k++)
            if (dev_[k].fd >= 0 && dev_[k].cap_on) boot_drained(k, get_time_us());
        for (Slot& s : slot_)
            if (s.given_up && s.pic >= 0 && !s.queued) s.pic = -1;
        log_stats();
    }
    teardown(true, true);
}

}  // namespace hal
