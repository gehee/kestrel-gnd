#include "dvr_library.hpp"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#include "settings.hpp"

// What the DVR folder holds, shared by the web gallery (webstream.cpp) and the
// goggle's own (osd_gallery.cpp): the list of recordings, how long each is, and
// a thumbnail of each. Moved out of webstream.cpp unchanged, so both galleries
// read a recording the same way and share one thumbnail cache.
namespace dvr_lib {

bool safe_name(const std::string& n) {
    if (n.empty() || n.size() > 128 || n[0] == '.') return false;
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-')) return false;
    return true;
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

namespace {

// Big-endian fields of the boxes read here.
uint32_t be32(const uint8_t* b) { return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
uint64_t be64(const uint8_t* b) { return ((uint64_t)be32(b) << 32) | be32(b + 4); }

// The child boxes of [b, b + n): calls f(type, body, body size) for each.
template <typename F> void each_box(const uint8_t* b, size_t n, F f) {
    size_t o = 0;
    while (o + 8 <= n) {
        const uint32_t size = be32(b + o);
        if (size < 8 || size > n - o) break;
        f(b + o + 4, b + o + 8, (size_t)size - 8);
        o += size;
    }
}

// The child boxes of the file's range [off, end), read by their headers only:
// calls f(type, body offset, body size) for each. A finished recording's moov
// holds every sample's size and offset - megabytes for a long one - and only
// a few small boxes in it are wanted.
template <typename F> void each_box_at(int fd, uint64_t off, uint64_t end, F f) {
    for (int guard = 0; guard < 1024 && off + 8 <= end; guard++) {
        uint8_t h[16];
        const ssize_t got = pread(fd, h, 16, (off_t)off);
        if (got < 8) return;
        uint64_t size = be32(h), hdr = 8;
        if (size == 1) {
            if (got < 16) return;
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - off;
        }
        if (size < hdr || size > end - off) return;
        f(h + 4, off + hdr, size - hdr);
        off += size;
    }
}

// The first bytes of a box's body: at most cap of them, none if the read fails.
std::vector<uint8_t> read_body(int fd, uint64_t off, uint64_t n, size_t cap) {
    std::vector<uint8_t> b((size_t)std::min<uint64_t>(n, cap));
    if (pread(fd, b.data(), b.size(), (off_t)off) != (ssize_t)b.size()) b.clear();
    return b;
}

// A fragmented MP4's length, from the last of its fragments: that fragment's
// decode time plus its samples' durations, in the track's timescale. -1 if the
// tail of the file holds no fragment that parses.
double last_fragment_end(int fd, uint64_t file_size, uint32_t scale, uint32_t trex_duration) {
    // A fragment is one picture, so the last moof lies within the last
    // picture's size of the end; a big I-frame is well under this.
    const uint64_t tail = std::min<uint64_t>(file_size, 4u << 20);
    std::vector<uint8_t> t(tail);
    if (pread(fd, t.data(), tail, (off_t)(file_size - tail)) != (ssize_t)tail) return -1;
    for (size_t o = tail >= 16 ? tail - 16 : 0; o-- > 0;) {
        if (memcmp(&t[o + 4], "moof", 4) != 0 || memcmp(&t[o + 12], "mfhd", 4) != 0) continue;
        const uint32_t size = be32(&t[o]);
        if (size < 16 || size > tail - o) continue;
        uint64_t start = 0, sum = 0;
        bool have_tfdt = false, have_trun = false;
        each_box(&t[o + 8], size - 8, [&](const uint8_t* type, const uint8_t* b, size_t n) {
            if (memcmp(type, "traf", 4) != 0) return;
            uint32_t def_duration = trex_duration;
            each_box(b, n, [&](const uint8_t* type, const uint8_t* b, size_t n) {
                if (n < 8) return;
                const uint32_t flags = be32(b) & 0xffffff;
                if (memcmp(type, "tfhd", 4) == 0) {
                    size_t f = 8;                        // version/flags, track_ID
                    if (flags & 0x01) f += 8;            // base-data-offset
                    if (flags & 0x02) f += 4;            // sample-description-index
                    if ((flags & 0x08) && f + 4 <= n) def_duration = be32(b + f);
                } else if (memcmp(type, "tfdt", 4) == 0) {
                    if (b[0] == 1 && n >= 12) start = be64(b + 4);
                    else start = be32(b + 4);
                    have_tfdt = true;
                } else if (memcmp(type, "trun", 4) == 0) {
                    const uint32_t count = be32(b + 4);
                    size_t f = 8;
                    if (flags & 0x001) f += 4;           // data-offset
                    if (flags & 0x004) f += 4;           // first-sample-flags
                    const size_t per = 4 * (!!(flags & 0x100) + !!(flags & 0x200) +
                                            !!(flags & 0x400) + !!(flags & 0x800));
                    if (!(flags & 0x100)) {
                        sum += (uint64_t)count * def_duration;
                    } else {
                        for (uint32_t i = 0; i < count && f + 4 <= n; i++, f += per) sum += be32(b + f);
                    }
                    have_trun = true;
                }
            });
        });
        if (have_tfdt && have_trun) return (double)(start + sum) / scale;
    }
    return -1;
}


} // namespace

// Length of an MP4 in seconds, or -1. A plain MP4 has it in its movie header,
// written last; a fragmented one writes its moov first, before there is a
// length to put in it, and says so again at the end - unless the recording was
// cut short - so without one it is read off the last fragment. The file is
// only walked by its top-level box headers until the moov: a recording can be
// hundreds of megabytes.
double mp4_duration(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    double secs = -1;
    uint64_t off = 0;
    if (fstat(fd, &st) == 0) {
        for (int guard = 0; guard < 64 && off + 8 <= (uint64_t)st.st_size; guard++) {
            uint8_t h[16];
            if (pread(fd, h, 16, (off_t)off) < 8) break;
            uint64_t size = be32(h);
            uint64_t hdr = 8;
            if (size == 1) {
                size = be64(h + 8);
                hdr = 16;
            } else if (size == 0) {
                size = (uint64_t)st.st_size - off;
            }
            if (size < hdr) break;
            if (memcmp(h + 4, "moov", 4) == 0) {
                // Walked by box headers: a long recording's moov is megabytes
                // (it was read whole, up to 1 MB, and a finished recording
                // over about 11 minutes was left out of the gallery).
                uint32_t track_scale = 0, trex_duration = 0;
                bool fragmented = false;
                each_box_at(fd, off + hdr, off + size, [&](const uint8_t* type, uint64_t bo, uint64_t bn) {
                    if (memcmp(type, "mvhd", 4) == 0) {
                        const std::vector<uint8_t> b = read_body(fd, bo, bn, 32);
                        if (b.size() < 32) return;
                        const uint32_t scale = b[0] == 1 ? be32(&b[20]) : be32(&b[12]);
                        const uint64_t dur = b[0] == 1 ? be64(&b[24]) : be32(&b[16]);
                        if (scale && dur) secs = (double)dur / scale;
                    } else if (memcmp(type, "trak", 4) == 0 && !track_scale) {
                        each_box_at(fd, bo, bo + bn, [&](const uint8_t* type, uint64_t bo, uint64_t bn) {
                            if (memcmp(type, "mdia", 4) != 0) return;
                            each_box_at(fd, bo, bo + bn, [&](const uint8_t* type, uint64_t bo, uint64_t bn) {
                                if (memcmp(type, "mdhd", 4) != 0) return;
                                const std::vector<uint8_t> b = read_body(fd, bo, bn, 24);
                                if (b.size() >= 24) track_scale = b[0] == 1 ? be32(&b[20]) : be32(&b[12]);
                            });
                        });
                    } else if (memcmp(type, "mvex", 4) == 0) {
                        fragmented = true;
                        each_box_at(fd, bo, bo + bn, [&](const uint8_t* type, uint64_t bo, uint64_t bn) {
                            if (memcmp(type, "trex", 4) != 0 || trex_duration) return;
                            const std::vector<uint8_t> b = read_body(fd, bo, bn, 16);
                            if (b.size() >= 16) trex_duration = be32(&b[12]);
                        });
                    }
                });
                if (secs < 0 && fragmented && track_scale)
                    secs = last_fragment_end(fd, (uint64_t)st.st_size, track_scale, trex_duration);
                break;
            }
            off += size;
        }
    }
    close(fd);
    return secs;
}

// ---- thumbnails -----------------------------------------------------------------
//
// One JPEG per recording, 640 px wide, a second in (or at the start of a
// shorter one), cached in <dvr>/.thumbs/<name>.jpg. Made by the image's own
// ffmpeg on a worker thread at the lowest priority, one at a time - about a
// second each on the goggle - and only when the gallery asks for one.

std::string dir() { return Settings::getInstance().getString("dvr_dir", "/media/dvr"); }
std::string thumb_dir() { return dir() + "/.thumbs"; }
std::string thumb_path(const std::string& name) { return thumb_dir() + "/" + name + ".jpg"; }

namespace {

bool run_ffmpeg_thumb(const std::string& in, const std::string& out, const char* at) {
    const std::string vf = "scale=640:-2";   // full-width cards on a 2x-3x phone screen
    const char* argv[] = {"nice", "-n", "19", "/usr/bin/ffmpeg", "-nostdin", "-loglevel", "error",
                          "-ss", at, "-i", in.c_str(), "-frames:v", "1", "-vf", vf.c_str(),
                          "-q:v", "6", "-y", out.c_str(), nullptr};
    pid_t pid;   // ::environ comes from <unistd.h> (g++ defines _GNU_SOURCE)
    if (posix_spawnp(&pid, "nice", nullptr, nullptr, const_cast<char* const*>(argv), ::environ) != 0)
        return false;
    int st = 0;
    waitpid(pid, &st, 0);
    struct stat o;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 && stat(out.c_str(), &o) == 0 && o.st_size > 0;
}

class Thumbnailer {
    public:
        // Queue a recording; a no-op if it is queued or being made already.
        void request(const std::string& name) {
            std::lock_guard<std::mutex> lk(m_);
            if (!queued_.insert(name).second) return;
            q_.push_back(name);
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            cv_.notify_one();
        }
    private:
        void run() {
            pthread_setname_np(pthread_self(), "dvr-thumb");
            // Made from a real-time thread (a recording stopping), and ffmpeg
            // would inherit that: `nice` does nothing to SCHED_FIFO, and a
            // real-time software decode right after every recording held up
            // the live picture's decoder threads, which are ordinary ones.
            struct sched_param sp = {};
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
            for (;;) {
                std::string name;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] { return !q_.empty(); });
                    name = q_.front();
                    q_.pop_front();
                }
                const std::string in = dir() + "/" + name, out = thumb_path(name);
                // Deleted while it waited (the goggle's gallery can do that): nothing
                // to make, and a thumbnail made now would be left with no recording.
                // Made already, from the recording as it is now: nothing to do
                // either - the goggle's gallery asks for every recording each time
                // it opens, and each one is a second of software decode.
                struct stat rec, thumb;
                if (stat(in.c_str(), &rec) != 0 ||
                    (stat(out.c_str(), &thumb) == 0 && thumb.st_size > 0 && thumb.st_mtime >= rec.st_mtime)) {
                    std::lock_guard<std::mutex> lk(m_);
                    queued_.erase(name);
                    continue;
                }
                mkdir(thumb_dir().c_str(), 0755);
                const std::string tmp = out + ".tmp.jpg";
                const bool ok = run_ffmpeg_thumb(in, tmp, "1") || run_ffmpeg_thumb(in, tmp, "0");
                if (ok) rename(tmp.c_str(), out.c_str());
                else { unlink(tmp.c_str()); printf("dvr: no thumbnail for %s\n", name.c_str()); }
                std::lock_guard<std::mutex> lk(m_);
                queued_.erase(name);
            }
        }
        std::mutex m_;
        std::condition_variable cv_;
        std::deque<std::string> q_;
        std::set<std::string> queued_;
        std::thread worker_;
};

Thumbnailer& thumbnailer() { static Thumbnailer t; return t; }

} // namespace

void request_thumbnail(const std::string& name) { thumbnailer().request(name); }

int delete_recording(const std::string& name, const std::string& active) {
    if (!safe_name(name) || !(ends_with(name, ".mp4") || ends_with(name, ".h265"))) return EINVAL;
    if (name == active) return EBUSY;
    const std::string path = dir() + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return ENOENT;
    if (unlink(path.c_str()) != 0) return errno;
    unlink(thumb_path(name).c_str());
    sync();
    return 0;
}

std::string stamp_of(const Recording& r) {
    for (size_t i = 0; i + 15 <= r.name.size(); i++) {
        bool ok = r.name[i + 8] == '_';
        for (size_t k = 0; ok && k < 15; k++)
            if (k != 8 && !isdigit((unsigned char)r.name[i + k])) ok = false;
        if (ok) return r.name.substr(i, 15);
    }
    char t[32];
    struct tm tmv;
    time_t mt = (time_t)r.mtime;
    gmtime_r(&mt, &tmv);
    strftime(t, sizeof(t), "%Y%m%d_%H%M%S", &tmv);
    return std::string(t);
}

// Newest first, by the time in the name (YYYYMMDD_HHMMSS): the prefix changed
// from kestrel_ to fpvOS_, and sorting on the whole name put every new
// recording below the old ones. A name without one sorts by its mtime.
std::vector<Recording> list_recordings() {
    std::vector<Recording> recs;
    const std::string folder = dir();
    if (DIR* d = opendir(folder.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (!safe_name(n) || !(ends_with(n, ".mp4") || ends_with(n, ".h265"))) continue;
            struct stat st;
            if (stat((folder + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            recs.push_back({n, (uint64_t)st.st_size, (int64_t)st.st_mtime});
        }
        closedir(d);
    }
    std::sort(recs.begin(), recs.end(), [](const Recording& a, const Recording& b) {
        const std::string sa = stamp_of(a), sb = stamp_of(b);
        return sa != sb ? sa > sb : a.name > b.name;
    });
    return recs;
}

} // namespace dvr_lib
