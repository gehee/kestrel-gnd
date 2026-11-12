#pragma once
#include "dvr/mp4_api.hpp"
#include "hal/artosyn/ar9311_ge2d.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
namespace dvr {
static double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void ck(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(std::string(what) + ": " + strerror(errno));
}
static int io(int fd, unsigned long req, void *p) {
    int r;
    do {
        r = ioctl(fd, req, p);
    } while (r < 0 && errno == EINTR);
    return r;
}
constexpr auto OUT = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, CAP = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
struct Map {
    uint8_t *p = nullptr;
    size_t n = 0;
};
class AscentExportWriter {
  public:
    int fd = -1;
    bool out_on = false, cap_on = false;
    std::vector<Map> coded;
    int strides[3]{};
    unsigned planes = 0;
    FILE *file = nullptr;
    MP4E_mux_t *mux = nullptr;
    mp4_h26x_writer_t mp4{};
    unsigned frames = 0;
    size_t bytes = 0;
    ~AscentExportWriter() {
        if (fd >= 0) {
            auto t = OUT;
            if (out_on)
                io(fd, VIDIOC_STREAMOFF, &t);
            t = CAP;
            if (cap_on)
                io(fd, VIDIOC_STREAMOFF, &t);
            for (auto &m : coded)
                if (m.p && m.p != MAP_FAILED)
                    munmap(m.p, m.n);
            close(fd);
        }
        if (mux) {
            MP4E_close(mux);
            mp4_h26x_write_close(&mp4);
        }
        if (file)
            fclose(file);
    }
    static v4l2_buffer buf(v4l2_buf_type t, unsigned i, unsigned n, v4l2_plane *p) {
        v4l2_buffer b{};
        b.type = t;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = n;
        b.m.planes = p;
        return b;
    }
    void control(unsigned id, int value) {
        v4l2_control c{};
        c.id = id;
        c.value = value;
        ck(io(fd, VIDIOC_S_CTRL, &c) == 0, "encoder control");
    }
    AscentExportWriter() = default;
    AscentExportWriter(const AscentExportWriter &) = delete;
    AscentExportWriter &operator=(const AscentExportWriter &) = delete;
    void init(const char *node, const char *path, int w, int h, unsigned fps) {
        fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        ck(fd >= 0, "encoder open");
        v4l2_format f{};
        f.type = OUT;
        auto &p = f.fmt.pix_mp;
        p.width = w;
        p.height = h;
        p.pixelformat = V4L2_PIX_FMT_YUV420M;
        p.field = V4L2_FIELD_NONE;
        p.num_planes = 3;
        p.colorspace = V4L2_COLORSPACE_REC709;
        p.ycbcr_enc = V4L2_YCBCR_ENC_709;
        p.quantization = V4L2_QUANTIZATION_LIM_RANGE;
        ck(io(fd, VIDIOC_S_FMT, &f) == 0, "encoder raw format");
        fprintf(stderr, "raw accepted fmt=%08x planes=%u %ux%u\n", p.pixelformat, p.num_planes, p.width,
                p.height);
        ck(p.pixelformat == V4L2_PIX_FMT_YUV420M && p.num_planes == 3 && int(p.width) == w &&
               int(p.height) >= h && int(p.height) <= h + 64,
           "encoder substituted format");
        planes = p.num_planes;
        for (unsigned j = 0; j < planes; j++)
            strides[j] = p.plane_fmt[j].bytesperline;
        f = {};
        f.type = CAP;
        f.fmt.pix_mp.width = w;
        f.fmt.pix_mp.height = h;
        f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
        f.fmt.pix_mp.num_planes = 1;
        ck(io(fd, VIDIOC_S_FMT, &f) == 0, "encoder HEVC format");
        v4l2_streamparm sp{};
        sp.type = OUT;
        sp.parm.output.timeperframe = {1, fps};
        ck(io(fd, VIDIOC_S_PARM, &sp) == 0, "encoder fps");
        control(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1);
        control(V4L2_CID_MPEG_VIDEO_BITRATE, 8000000);
        control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, fps);
        control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
        control(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, 30);
        control(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);
        control(V4L2_CID_MPEG_VIDEO_AU_DELIMITER, 0);
        v4l2_requestbuffers rq{};
        rq.type = OUT;
        rq.memory = V4L2_MEMORY_DMABUF;
        rq.count = 1;
        ck(io(fd, VIDIOC_REQBUFS, &rq) == 0 && rq.count >= 1, "encoder imported buffers");
        v4l2_buffer b{};
        rq = {};
        rq.type = CAP;
        rq.memory = V4L2_MEMORY_MMAP;
        rq.count = 2;
        ck(io(fd, VIDIOC_REQBUFS, &rq) == 0 && rq.count >= 1 && rq.count <= 16, "encoder coded buffers");
        coded.resize(rq.count);
        for (unsigned i = 0; i < rq.count; i++) {
            v4l2_plane q{};
            b = buf(CAP, i, 1, &q);
            ck(io(fd, VIDIOC_QUERYBUF, &b) == 0, "coded query");
            coded[i].n = q.length;
            coded[i].p = (uint8_t *)mmap(nullptr, q.length, PROT_READ, MAP_SHARED, fd, q.m.mem_offset);
            ck(coded[i].p != MAP_FAILED, "coded map");
            ck(io(fd, VIDIOC_QBUF, &b) == 0, "coded queue");
        }
        int outfd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
        ck(outfd >= 0, "output open");
        file = fdopen(outfd, "wb");
        if (!file)
            close(outfd);
        ck(file, "output open");
        mux = MP4E_open(0, 0, file, [](int64_t offset, const void *data, size_t n, void *token) -> int {
            auto *f = (FILE *)token;
            return fseeko(f, offset, SEEK_SET) || fwrite(data, 1, n, f) != n;
        });
        ck(mux, "MP4 mux open");
        ck(mp4_h26x_write_init(&mp4, mux, w, h, 1) == MP4E_STATUS_OK, "MP4 track");
        auto t = OUT;
        ck(io(fd, VIDIOC_STREAMON, &t) == 0, "encoder output start");
        out_on = true;
        t = CAP;
        ck(io(fd, VIDIOC_STREAMON, &t) == 0, "encoder capture start");
        cap_on = true;
        fprintf(stderr, "encoder ready %dx%d stride=%d/%d/%d coded=%zu\n", w, h, strides[0], strides[1],
                strides[2], coded.size() * coded[0].n);
    }
    // Packets are taken as the encoder hands them out, not one per frame:
    // each keeps the duration of the picture it came from (no reordering).
    std::deque<unsigned> pending;
    bool take_packets(bool &last) {
        bool any = false;
        for (;;) {
            v4l2_plane q{};
            auto d = buf(CAP, 0, 1, &q);
            if (io(fd, VIDIOC_DQBUF, &d) < 0) {
                if (errno == EPIPE) // drained after the stop command
                    last = true;
                else
                    ck(errno == EAGAIN, "encoder coded dequeue");
                return any;
            }
            ck(d.index < coded.size() && q.bytesused >= q.data_offset && q.bytesused <= coded[d.index].n &&
                   !(d.flags & V4L2_BUF_FLAG_ERROR),
               "encoder packet invalid");
            const size_t n = q.bytesused - q.data_offset;
            if (d.flags & V4L2_BUF_FLAG_LAST)
                last = true;
            if (n) {
                ck(!pending.empty(), "encoder packet without a picture");
                ck(mp4_h26x_write_nal(&mp4, coded[d.index].p + q.data_offset, n, pending.front()) ==
                       MP4E_STATUS_OK,
                   "MP4 packet");
                pending.pop_front();
                frames++;
                bytes += n;
                any = true;
            } else
                ck(last, "encoder packet invalid");
            if (last)
                return any;
            ck(io(fd, VIDIOC_QBUF, &d) == 0, "encoder coded requeue");
        }
    }
    void wait(double deadline) {
        ck(seconds() < deadline, "encoder timeout");
        pollfd poller{fd, POLLIN | POLLOUT, 0};
        ck(poll(&poller, 1, 10) >= 0 || errno == EINTR, "encoder poll");
    }
    // Returns once the encoder has released src: the caller writes the next
    // picture into the same buffer.
    void frame(const ar_ge2d_image &src, size_t bytes, uint64_t pts, unsigned ticks) {
        v4l2_plane p[3]{};
        for (int j = 0; j < 3; j++) {
            ck(src.stride[j] == unsigned(strides[j]), "encoder stride mismatch");
            p[j].m.fd = src.fd[j];
            p[j].length = bytes;
            p[j].data_offset = src.offset[j];
            p[j].bytesused = src.offset[j] + src.stride[j] * (j ? src.height / 2 : src.height);
            ck(p[j].bytesused <= bytes, "encoder plane extent");
        }
        auto b = buf(OUT, 0, 3, p);
        b.memory = V4L2_MEMORY_DMABUF;
        b.timestamp.tv_sec = pts / 1000000;
        b.timestamp.tv_usec = pts % 1000000;
        ck(io(fd, VIDIOC_QBUF, &b) == 0, "encoder shared queue");
        pending.push_back(ticks);
        bool returned = false, last = false;
        const double deadline = seconds() + 5;
        for (;;) {
            if (!returned) {
                v4l2_plane q[3]{};
                auto d = buf(OUT, 0, 3, q);
                d.memory = V4L2_MEMORY_DMABUF;
                if (io(fd, VIDIOC_DQBUF, &d) == 0) {
                    ck(!(d.flags & V4L2_BUF_FLAG_ERROR), "encoder source rejected");
                    returned = true;
                } else
                    ck(errno == EAGAIN, "encoder raw dequeue");
            }
            take_packets(last);
            ck(!last, "encoder stopped early");
            if (returned)
                return;
            wait(deadline);
        }
    }
    void finish() {
        ck(mux && file, "export output missing");
        // Drain: the stop command makes the encoder hand out what it holds and
        // mark the last packet. Skipped when every picture is already written.
        if (!pending.empty()) {
            v4l2_encoder_cmd stop{};
            stop.cmd = V4L2_ENC_CMD_STOP;
            ck(io(fd, VIDIOC_ENCODER_CMD, &stop) == 0, "encoder drain");
            bool last = false;
            const double deadline = seconds() + 5;
            while (!pending.empty() && !last) {
                if (!take_packets(last) && !last)
                    wait(deadline);
            }
            ck(pending.empty(), "encoder lost pictures");
        }
        const auto result = MP4E_close(mux);
        mux = nullptr;
        mp4_h26x_write_close(&mp4);
        ck(result == MP4E_STATUS_OK, "MP4 finalize");
        ck(fflush(file) == 0 && !ferror(file) && fsync(fileno(file)) == 0, "Export storage write");
        auto *f = file;
        file = nullptr;
        ck(fclose(f) == 0, "Export storage close");
    }
};
} // namespace dvr
