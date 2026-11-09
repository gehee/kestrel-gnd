// bus_bench: does base/bus.hpp cost the radio thread anything, and what does
// it save? A recorded H.265 stream (default the freestyle-1 flight: 100 fps,
// 2 slices, ~15 KB a picture) is replayed at its own rate through models of
// the ground's data paths, one mode after the other:
//
//   old       today's: each radio read appended to accum, a start-code copy
//             per NAL (unit), the picture gathered again for the DVR
//             (au_slices) and copied once more into a shared vector, the
//             decoder's own copy into its packet buffer; state through
//             OSD-style setters under one mutex, which the HUD holds while it
//             takes its snapshot and the renderer takes for every frame.
//   bus       each radio read written once into a pooled picture, views to
//             the sinks; state through Topics, the renderer's frames through
//             a Ring, no lock shared with the HUD.
//   bridge    bus plus a UDP forwarder of the slices (sendmmsg straight out
//             of the pictures; copied out only while the network is behind).
//   headless  bridge without the HUD or the renderer.
//
// The radio thread wakes when each slice is due (the slices of a picture are
// spread over --spread-us), does that slice's work and goes back to sleep.
// Printed per mode: the radio's time per slice (p50/p99/p99.9/max), its CPU,
// the renderer's time per frame in the HUD calls, the HUD's snapshot time,
// the bytes copied per second on the video path, and what was dropped.
//
// Usage: bus_bench [--file F.h265] [--seconds 20] [--fps 100] [--spread-us 5000]
//                  [--modes old,bus,bridge,headless] [--hud-hz 60]
//                  [--hud-hold-us 100] [--hud-draw-us 3000] [--rec]
//                  [--to 127.0.0.1:5600] [--stall-ms 0]
#include <algorithm>
#include "base/video_codec.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "base/bus.hpp"
#include "hud/gallery_stats.hpp"
#include "hud/latency_ring.hpp"

static uint64_t now_ns() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static uint64_t thread_cpu_ns() {
    timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void sleep_until(uint64_t ns) {
    timespec t{(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr) == EINTR) {}
}
static void spin_for(uint64_t ns) {
    const uint64_t end = now_ns() + ns;
    volatile uint64_t x = 0;
    while (now_ns() < end) x++;
}

// ---- the stream -------------------------------------------------------------
struct Nal { uint32_t off, len; uint8_t type; };   // off: the payload, after its start code
struct Chunk {                                     // what the radio delivers as one slice
    uint32_t off, len;                             // in the file, start codes included
    std::vector<Nal> nals;
    bool last = false, key = false;
};
struct Stream {
    std::vector<uint8_t> data;
    std::vector<std::vector<Chunk>> pics;
};

static bool load(const char* path, double seconds, int fps, Stream& s) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    s.data.resize(size);
    if (fread(s.data.data(), 1, size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);
    const uint8_t* d = s.data.data();
    std::vector<uint32_t> sc;                      // start code positions (the 00 00 01)
    for (long i = 0; i + 3 < size; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { sc.push_back((uint32_t)i); i += 2; }
    const size_t want = (size_t)(seconds * fps);
    std::vector<Chunk> pic;
    Chunk cur{};
    bool open = false;
    for (size_t k = 0; k < sc.size(); k++) {
        const uint32_t p = sc[k];
        const uint32_t begin = (p > 0 && d[p - 1] == 0) ? p - 1 : p;   // a 4-byte start code
        uint32_t end = k + 1 < sc.size() ? sc[k + 1] : (uint32_t)size;
        if (end > 0 && end < (uint32_t)size && d[end - 1] == 0) end--;
        const uint8_t type = (d[p + 3] >> 1) & 0x3f;
        const bool vcl = type < 32;
        const bool first = vcl && (d[p + 5] & 0x80);
        if (first && !pic.empty()) {
            pic.back().last = true;
            s.pics.push_back(std::move(pic));
            pic.clear();
            if (s.pics.size() >= want) break;
        }
        if (!open) { cur = Chunk{}; cur.off = begin; open = true; }
        cur.nals.push_back({p + 3, end - (p + 3), type});
        if (hevc_irap(type)) cur.key = true;
        cur.len = end - cur.off;
        if (vcl) { pic.push_back(cur); open = false; }
    }
    return !s.pics.empty();
}

// ---- results ------------------------------------------------------------------
struct Samples {
    std::vector<uint32_t> ns;
    void add(uint64_t v) { ns.push_back((uint32_t)std::min<uint64_t>(v, UINT32_MAX)); }
    double pct(double p) {
        if (ns.empty()) return 0;
        std::vector<uint32_t> v = ns;
        size_t k = std::min(v.size() - 1, (size_t)(p / 100.0 * v.size()));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k] / 1000.0;
    }
    double max() { return ns.empty() ? 0 : *std::max_element(ns.begin(), ns.end()) / 1000.0; }
};

struct Opts {
    std::string file = "../sim-flights/freestyle-1/video.h265";
    double seconds = 20;
    int fps = 100;
    uint64_t spread_us = 5000;
    int hud_hz = 60;
    uint64_t hud_hold_us = 100, hud_draw_us = 3000;
    bool rec = false;
    std::string to = "127.0.0.1:5600";
    int stall_ms = 0;
};

// What the radio publishes, as big as the real link figures (radio::LinkStats).
struct LinkStats { int32_t state, mcs, snr_x10, dist, kbps, bw_idx; uint32_t lost, seq; char peer[24]; int32_t pad[30]; };
struct VideoInfo { float reassembly_ms, video_mbps; uint32_t pictures, seq; };

// ---- the HUD side of today's code: one mutex for everything ---------------------
struct OldOsd {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c;
    OldOsd() {                                     // timed waits on CLOCK_MONOTONIC, as the HUD's
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&c, &a);
        pthread_condattr_destroy(&a);
    }
    bool render_requested = false;
    LinkStats artosyn{};
    float reassembly = 0, video_bw = 0;
    std::vector<float> bw_history;
    LatencyRing ring;
    uint64_t last_frame_us = 0, latency_last_us = 0;
    uint32_t frames = 0;
    void update_artosyn_stats(const LinkStats& v) { pthread_mutex_lock(&m); artosyn = v; pthread_mutex_unlock(&m); }
    void set_reassembly_latency(float v) { pthread_mutex_lock(&m); reassembly = v; pthread_mutex_unlock(&m); }
    void update_video_bandwidth(float bw) {
        pthread_mutex_lock(&m);
        video_bw = bw;
        bw_history.push_back(bw);
        if (bw_history.size() > 50) bw_history.erase(bw_history.begin());
        pthread_mutex_unlock(&m);
    }
    void signal_render() { pthread_mutex_lock(&m); render_requested = true; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
    void notify_video_frame() { pthread_mutex_lock(&m); last_frame_us = now_ns() / 1000; frames++; pthread_mutex_unlock(&m); }
    void add_latency_frame(const LatencyFrame& f) {
        pthread_mutex_lock(&m);
        ring.push(f);
        latency_last_us = f.t_us;
        pthread_mutex_unlock(&m);
    }
};

// ---- one run ------------------------------------------------------------------
struct Run {
    const char* mode;
    bool old_path, bridge, hud;
    Samples radio, rend, snap;
    uint64_t radio_cpu = 0, wall = 0, proc_cpu = 0;
    uint64_t copied = 0, pics = 0, slices = 0, pic_drops = 0, ring_drops = 0;
    uint64_t udp_pkts = 0, udp_bytes = 0, udp_copied = 0, udp_errors = 0;
    uint64_t hud_frames = 0;
};

static void udp_target(const std::string& to, sockaddr_in& a) {
    a = sockaddr_in{};
    a.sin_family = AF_INET;
    const size_t colon = to.find(':');
    a.sin_port = htons(colon == std::string::npos ? 5600 : atoi(to.c_str() + colon + 1));
    inet_pton(AF_INET, to.substr(0, colon).c_str(), &a.sin_addr);
}

static uint64_t proc_cpu_ns() {
    rusage r; getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000ull +
           (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000ull;
}

static void run_mode(const Stream& s, const Opts& o, Run& r) {
    std::atomic<bool> stop{false};
    const uint64_t frame_ns = 1000000000ull / o.fps;
    const uint64_t slice_gap = s.pics[0].size() > 1 ? o.spread_us * 1000 / (s.pics[0].size() - 1) : 0;
    const uint64_t decode_ns = 4000000;            // the renderer sees a picture 4 ms after its last slice

    OldOsd old;
    // The bus side.
    bus::Topic<LinkStats> link;
    bus::Topic<VideoInfo> video;
    bus::Ring<LatencyFrame, 256> lat_ring;
    std::atomic<uint64_t> last_frame_us{0};
    bus::Waker hud_wake;
    link.set_waker(&hud_wake);

    // Pictures for the renderer (both modes): the pts and when it is due.
    bus::Ring<uint64_t, 64> decoded;
    bus::Waker rend_wake;
    decoded.set_waker(&rend_wake);

    // The picture pool: 16 x 512 KB, 4 KB headroom.
    const uint32_t cap = 512 * 1024, head = 4096;
    std::vector<uint8_t> pool_mem((size_t)16 * (cap + head));
    bus::PicturePool pool(pool_mem.data(), 16, cap, head);

    // The DVR (--rec): today a mutex + deque of shared vectors, the bus a ring of pictures.
    std::mutex dvr_m; std::condition_variable dvr_c;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> dvr_q;
    bus::Ring<bus::PictureRef, 32> dvr_ring;
    bus::Waker dvr_wake;
    dvr_ring.set_waker(&dvr_wake);
    const int devnull = open("/dev/null", O_WRONLY);

    // The bridge: slices, as references.
    struct SliceItem { bus::PictureRef pic; uint32_t off = 0, len = 0; };
    bus::Ring<SliceItem, 64> br_ring;
    bus::Waker br_wake;
    br_ring.set_waker(&br_wake);

    std::vector<std::thread> threads;

    // HUD: wakes for link news or its refresh, takes its snapshot, draws.
    if (r.hud) threads.emplace_back([&] {
        LatencyRing own;                           // the bus HUD owns its history
        const uint64_t tick = 1000000000ull / o.hud_hz;
        uint64_t next = now_ns() + tick;
        LinkStats ls; VideoInfo vi;
        while (!stop.load()) {
            if (r.old_path) {
                pthread_mutex_lock(&old.m);
                timespec ts{(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
                while (!old.render_requested && !stop.load() &&
                       pthread_cond_timedwait(&old.c, &old.m, &ts) != ETIMEDOUT) {}
                old.render_requested = false;
                pthread_mutex_unlock(&old.m);
            } else {
                const uint64_t t = now_ns();
                if (t < next) hud_wake.wait((next - t) / 1000);
            }
            if (now_ns() >= next) next += tick;
            if (stop.load()) break;
            const uint64_t t0 = now_ns();
            if (r.old_path) {
                pthread_mutex_lock(&old.m);
                ls = old.artosyn;
                volatile float sink = old.reassembly + old.video_bw;
                (void)sink;
                const uint64_t nus = now_ns() / 1000;
                if (old.latency_last_us && nus - old.latency_last_us > 150000) old.latency_last_us = nus;
                float med = 0;
                if (!old.ring.empty()) { volatile float b = old.ring.back().net_ms; (void)b; }
                old.ring.median(med);
                spin_for(o.hud_hold_us * 1000);     // the rest of the work done under the lock
                pthread_mutex_unlock(&old.m);
            } else {
                link.read(ls);
                video.read(vi);
                LatencyFrame f;
                while (lat_ring.pop(f)) own.push(f);
                volatile uint64_t lf = last_frame_us.load(std::memory_order_relaxed);
                (void)lf;
                float med = 0;
                if (!own.empty()) { volatile float b = own.back().net_ms; (void)b; }
                own.median(med);
                spin_for(o.hud_hold_us * 1000);     // the same work, under no lock
            }
            r.snap.add(now_ns() - t0);
            spin_for(o.hud_draw_us * 1000);         // drawing: CPU, no lock
            r.hud_frames++;
        }
    });

    // Renderer: a picture "decoded" 4 ms after its last slice; tells the HUD.
    if (r.hud) threads.emplace_back([&] {
        uint64_t due = 0;
        float pace = 0;
        while (!stop.load()) {
            if (!decoded.pop(due)) { rend_wake.wait(20000); continue; }
            sleep_until(due);
            LatencyFrame f{};
            f.processing_ms = 1; f.net_ms = 9; f.dec_ms = 4; f.disp_ms = 5; f.pace_ms = pace;
            f.t_us = now_ns() / 1000; f.nslices = 2; f.slice_ms[0] = 18; f.slice_ms[1] = 21;
            const uint64_t t0 = now_ns();
            if (r.old_path) {
                old.notify_video_frame();
                old.add_latency_frame(f);
            } else {
                last_frame_us.store(f.t_us, std::memory_order_relaxed);
                lat_ring.push(f);
            }
            r.rend.add(now_ns() - t0);
            pace = 10;
        }
    });

    // DVR writer (--rec).
    if (o.rec) threads.emplace_back([&] {
        while (!stop.load()) {
            if (r.old_path) {
                std::unique_lock<std::mutex> lk(dvr_m);
                dvr_c.wait_for(lk, std::chrono::milliseconds(20), [&] { return !dvr_q.empty() || stop.load(); });
                while (!dvr_q.empty()) {
                    auto p = dvr_q.front(); dvr_q.pop_front();
                    lk.unlock();
                    if (write(devnull, p->data(), p->size()) < 0) {}
                    lk.lock();
                }
            } else {
                bus::PictureRef p;
                if (!dvr_ring.pop(p)) { dvr_wake.wait(20000); continue; }
                if (write(devnull, p->data(), p->len) < 0) {}
            }
        }
    });

    // Bridge: each slice out as ~1400-byte datagrams straight from the
    // picture; while sends fail (the network behind, or --stall-ms), slices
    // are copied out so the pool gets its pictures back.
    if (r.bridge) threads.emplace_back([&] {
        const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        sockaddr_in to; udp_target(o.to, to);
        struct Pkt { uint8_t hdr[12]; };
        std::vector<Pkt> hdrs(256);
        std::vector<iovec> iov(512);
        std::vector<mmsghdr> msgs(256);
        std::deque<std::vector<uint8_t>> backlog;
        uint16_t seq = 0;
        const uint64_t start = now_ns();
        auto stalled = [&] {
            if (!o.stall_ms) return false;
            return ((now_ns() - start) / 1000000ull) % 1000 < (uint64_t)o.stall_ms;
        };
        // Sends one slice's bytes; how many went (all or none here: a datagram
        // either goes or the batch stops).
        auto send_bytes = [&](const uint8_t* p, uint32_t len) -> uint32_t {
            if (stalled()) return 0;
            uint32_t done = 0;
            while (done < len) {
                int n = 0;
                uint32_t at = done;
                while (at < len && n < 256) {
                    const uint32_t take = std::min<uint32_t>(1400, len - at);
                    hdrs[n].hdr[0] = 0x80; hdrs[n].hdr[1] = 96;
                    hdrs[n].hdr[2] = seq >> 8; hdrs[n].hdr[3] = seq & 0xff; seq++;
                    iov[2 * n] = {hdrs[n].hdr, 12};
                    iov[2 * n + 1] = {(void*)(p + at), take};
                    msgs[n] = mmsghdr{};
                    msgs[n].msg_hdr.msg_name = &to;
                    msgs[n].msg_hdr.msg_namelen = sizeof(to);
                    msgs[n].msg_hdr.msg_iov = &iov[2 * n];
                    msgs[n].msg_hdr.msg_iovlen = 2;
                    at += take; n++;
                }
                const int sent = sendmmsg(fd, msgs.data(), n, 0);
                if (sent <= 0) { r.udp_errors++; break; }
                for (int i = 0; i < sent; i++) { done += (uint32_t)iov[2 * i + 1].iov_len; r.udp_pkts++; }
                if (sent < n) break;
            }
            r.udp_bytes += done;
            return done;
        };
        while (!stop.load()) {
            while (!backlog.empty()) {             // older first
                auto& b = backlog.front();
                const uint32_t n = send_bytes(b.data(), (uint32_t)b.size());
                if (n < b.size()) { b.erase(b.begin(), b.begin() + n); break; }
                backlog.pop_front();
            }
            SliceItem it;
            if (!br_ring.pop(it)) { br_wake.wait(backlog.empty() ? 20000 : 1000); continue; }
            const uint8_t* p = it.pic->data() + it.off;
            uint32_t sent = backlog.empty() ? send_bytes(p, it.len) : 0;
            if (sent < it.len) {                   // behind: own a copy, give the picture back
                std::vector<uint8_t> c(it.len - sent);
                bus::SliceView sv{&it.pic, it.off + sent, it.len - sent, false};
                sv.copy_to(c.data());
                r.udp_copied += c.size();
                backlog.push_back(std::move(c));
            }
        }
        close(fd);
    });

    // ---- the radio thread (this one) ----------------------------------------
    const uint64_t cpu0 = thread_cpu_ns(), proc0 = proc_cpu_ns(), wall0 = now_ns();
    std::vector<uint8_t> rx(16384);                  // what bb_socket_read fills
    std::vector<uint8_t> accum, au_slices, dest_buf(1024 * 1024);   // today's
    static thread_local std::vector<uint8_t> unit;
    std::vector<uint8_t> vps, sps, pps;
    bus::PictureRef pic;
    uint64_t t = now_ns() + 50000000ull;
    uint64_t next_stats = t, next_bw = t;
    uint64_t bytes_window = 0;
    LinkStats ls{}; VideoInfo vi{};
    uint32_t seq = 0;
    const size_t npics = s.pics.size();
    for (size_t i = 0; i < npics; i++, t += frame_ns) {
        const auto& chunks = s.pics[i];
        for (size_t k = 0; k < chunks.size(); k++) {
            const Chunk& c = chunks[k];
            sleep_until(t + k * slice_gap);
            const uint64_t t0 = now_ns();
            const uint8_t* src = s.data.data() + c.off;
            if (r.old_path) {
                // Radio reads, appended to accum.
                for (uint32_t at = 0; at < c.len; ) {
                    const uint32_t n = std::min<uint32_t>((uint32_t)rx.size(), c.len - at);
                    memcpy(rx.data(), src + at, n);
                    accum.insert(accum.end(), rx.data(), rx.data() + n);
                    r.copied += n;
                    at += n;
                }
                // Each NAL: the start code back on, a copy (unit).
                for (const Nal& nal : c.nals) {
                    const uint8_t* np = accum.data() + (nal.off - c.off);
                    unit.clear();
                    unit.reserve(nal.len + 4);
                    unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01});
                    unit.insert(unit.end(), np, np + nal.len);
                    r.copied += nal.len + 4;
                    if (nal.type == 32) vps = unit;
                    else if (nal.type == 33) sps = unit;
                    else if (nal.type == 34) pps = unit;
                    else if (nal.type < 32) {
                        au_slices.insert(au_slices.end(), unit.begin(), unit.end());   // for the DVR
                        r.copied += unit.size();
                        // The decoder's packet: parameter sets in front of a keyframe.
                        uint8_t* d = dest_buf.data();
                        if (nal.type >= 16 && nal.type <= 21) {
                            memcpy(d, vps.data(), vps.size()); d += vps.size();
                            memcpy(d, sps.data(), sps.size()); d += sps.size();
                            memcpy(d, pps.data(), pps.size()); d += pps.size();
                        }
                        memcpy(d, unit.data(), unit.size());
                        r.copied += unit.size();
                    }
                }
                accum.erase(accum.begin(), accum.begin() + c.len);
                if (c.last) {
                    auto au = std::make_shared<std::vector<uint8_t>>(au_slices);   // update_dvr's copy
                    r.copied += au_slices.size();
                    if (o.rec) {
                        std::lock_guard<std::mutex> lk(dvr_m);
                        dvr_q.push_back(std::move(au));
                        dvr_c.notify_one();
                    }
                    au_slices.clear();
                    old.set_reassembly_latency(0.4f);
                }
            } else {
                // Radio reads, written once into the picture.
                if (k == 0) {
                    pic = pool.acquire();
                    if (!pic) r.pic_drops++;
                    else { pic->pts = i; pic->keyframe = c.key; }
                }
                if (pic) {
                    const uint32_t off0 = pic->len;
                    for (uint32_t at = 0; at < c.len; ) {
                        const uint32_t n = std::min<uint32_t>((uint32_t)rx.size(), c.len - at);
                        memcpy(rx.data(), src + at, n);                  // bb_socket_read, as before
                        if (pic->len + n <= pic->cap) {
                            memcpy(pic->data() + pic->len, rx.data(), n);   // the one copy
                            pic->len += n;
                        }
                        r.copied += n;
                        at += n;
                    }
                    // Sinks: the decoder takes the view (a pointer into the picture).
                    bus::SliceView sv{&pic, off0, pic->len - off0, c.last};
                    volatile uint8_t first = sv.data()[0];
                    (void)first;
                    if (r.bridge && !br_ring.push(SliceItem{sv.retain(), sv.off, sv.len})) r.ring_drops++;
                    if (c.last) {
                        if (o.rec && !dvr_ring.push(pic)) r.ring_drops++;
                        pic.reset();
                        vi.reassembly_ms = 0.4f; vi.pictures++; vi.seq = ++seq;
                        video.publish(vi);
                    }
                }
            }
            bytes_window += c.len;
            r.slices++;
            if (c.last) {
                r.pics++;
                if (r.hud) decoded.push(now_ns() + decode_ns);
            }
            // The stats thread's figures, ten times a second; the bitrate once a second.
            const uint64_t tn = now_ns();
            if (tn >= next_stats) {
                next_stats += 100000000ull;
                ls.state = 2; ls.mcs = 7; ls.snr_x10 = 250; ls.kbps = 20000; ls.seq++;
                if (r.old_path) { old.update_artosyn_stats(ls); old.signal_render(); }
                else link.publish(ls);
            }
            if (tn >= next_bw) {
                next_bw += 1000000000ull;
                const float mbps = bytes_window * 8 / 1e6f;
                bytes_window = 0;
                if (r.old_path) old.update_video_bandwidth(mbps);
                else { vi.video_mbps = mbps; video.publish(vi); }
            }
            r.radio.add(now_ns() - t0);
        }
    }
    r.radio_cpu = thread_cpu_ns() - cpu0;
    r.wall = now_ns() - wall0;
    usleep(100000);
    stop = true;
    hud_wake.poke(); rend_wake.poke(); dvr_wake.poke(); br_wake.poke();
    dvr_c.notify_all();
    for (auto& th : threads) th.join();
    r.proc_cpu = proc_cpu_ns() - proc0;
    r.copied += bus::SliceView::copies().exchange(0);
    r.copied -= std::min(r.copied, r.udp_copied);   // counted apart
    r.ring_drops += dvr_ring.drops() + br_ring.drops();
    close(devnull);
}

int main(int argc, char** argv) {
    Opts o;
    std::string modes = "old,bus,bridge,headless";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--file") o.file = val();
        else if (a == "--seconds") o.seconds = atof(val());
        else if (a == "--fps") o.fps = atoi(val());
        else if (a == "--spread-us") o.spread_us = strtoull(val(), nullptr, 10);
        else if (a == "--modes") modes = val();
        else if (a == "--hud-hz") o.hud_hz = atoi(val());
        else if (a == "--hud-hold-us") o.hud_hold_us = strtoull(val(), nullptr, 10);
        else if (a == "--hud-draw-us") o.hud_draw_us = strtoull(val(), nullptr, 10);
        else if (a == "--rec") o.rec = true;
        else if (a == "--to") o.to = val();
        else if (a == "--stall-ms") o.stall_ms = atoi(val());
        else { fprintf(stderr, "bus_bench: unknown option %s (see the source's header)\n", a.c_str()); return 2; }
    }
    Stream s;
    if (!load(o.file.c_str(), o.seconds, o.fps, s)) { fprintf(stderr, "bus_bench: no pictures in %s\n", o.file.c_str()); return 1; }
    size_t bytes = 0, nsl = 0;
    for (auto& p : s.pics) for (auto& c : p) { bytes += c.len; nsl++; }
    printf("stream: %zu pictures, %.1f slices each, %.1f KB a picture, %.1f Mbit/s at %d fps\n",
           s.pics.size(), (double)nsl / s.pics.size(), bytes / 1024.0 / s.pics.size(),
           bytes * 8.0 / 1e6 / (s.pics.size() / (double)o.fps), o.fps);
    printf("HUD %d Hz, %llu us of snapshot work, %llu us drawing; DVR %s; bridge to %s%s\n\n", o.hud_hz,
           (unsigned long long)o.hud_hold_us, (unsigned long long)o.hud_draw_us, o.rec ? "recording" : "off",
           o.to.c_str(), o.stall_ms ? (" (network stalls " + std::to_string(o.stall_ms) + " ms a second)").c_str() : "");
    printf("%-9s | radio us/slice: p50   p99  p99.9    max | radio CPU | renderer us: p99    max | HUD snapshot us: p50   p99 | copied MB/s | drops\n", "mode");
    for (size_t at = 0; at <= modes.size(); ) {
        size_t e = modes.find(',', at);
        if (e == std::string::npos) e = modes.size();
        const std::string m = modes.substr(at, e - at);
        at = e + 1;
        if (m.empty()) continue;
        Run r{};
        r.mode = m.c_str();
        r.old_path = m == "old";
        r.bridge = m == "bridge" || m == "headless";
        r.hud = m != "headless";
        if (m != "old" && m != "bus" && m != "bridge" && m != "headless") { fprintf(stderr, "unknown mode %s\n", m.c_str()); continue; }
        run_mode(s, o, r);
        const double secs = r.wall / 1e9;
        printf("%-9s |              %5.1f %5.1f  %5.1f  %5.1f |   %5.2f%%  |            %5.1f %6.1f |                %5.1f %5.1f |  %9.1f | %llu pic, %llu ring\n",
               m.c_str(), r.radio.pct(50), r.radio.pct(99), r.radio.pct(99.9), r.radio.max(),
               100.0 * r.radio_cpu / r.wall, r.rend.pct(99), r.rend.max(), r.snap.pct(50), r.snap.pct(99),
               r.copied / 1e6 / secs, (unsigned long long)r.pic_drops, (unsigned long long)r.ring_drops);
        if (r.bridge)
            printf("%-9s   bridge: %llu datagrams, %.1f Mbit/s sent, %.2f MB copied out while behind, %llu send failures\n", "",
                   (unsigned long long)r.udp_pkts, r.udp_bytes * 8 / 1e6 / secs, r.udp_copied / 1e6,
                   (unsigned long long)r.udp_errors);
        printf("%-9s   process CPU %.1f%% of one core, %llu HUD frames\n", "", 100.0 * r.proc_cpu / r.wall,
               (unsigned long long)r.hud_frames);
    }
    return 0;
}
