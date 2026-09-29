#include "dvr_recover.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "utils/minimp4.h"

namespace {

uint32_t be32(const uint8_t* b) { return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
uint64_t be64(const uint8_t* b) { return ((uint64_t)be32(b) << 32) | be32(b + 4); }
void put32(uint8_t* b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
void put64(uint8_t* b, uint64_t v) { put32(b, (uint32_t)(v >> 32)); put32(b + 4, (uint32_t)v); }

using Bytes = std::vector<uint8_t>;

void add32(Bytes& o, uint32_t v) { uint8_t b[4]; put32(b, v); o.insert(o.end(), b, b + 4); }

// A box around `body`: its size, its type, the body.
Bytes box(const char* type, const Bytes& body) {
    Bytes o;
    add32(o, (uint32_t)(body.size() + 8));
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), body.begin(), body.end());
    return o;
}

// The child boxes of [b, b + n): f(type, body, body size) for each, in order.
template <typename F> bool each_box(const uint8_t* b, size_t n, F f) {
    size_t o = 0;
    while (o + 8 <= n) {
        const uint32_t size = be32(b + o);
        if (size < 8 || size > n - o) return false;
        f(std::string((const char*)b + o + 4, 4), b + o + 8, (size_t)size - 8);
        o += size;
    }
    return o == n;
}

struct Sample { uint64_t off; uint32_t size, duration; bool sync; };

// sample_is_non_sync_sample, in a sample's flags
constexpr uint32_t kNonSync = 0x00010000;

struct Trex { uint32_t duration = 0, size = 0, flags = 0; };

// The samples of one moof at file offset `at`, appended to `out`; false if
// it holds anything this cannot place.
bool read_moof(const uint8_t* b, size_t n, uint64_t at, const Trex& trex, std::vector<Sample>& out) {
    bool ok = true, any = false;
    each_box(b, n, [&](const std::string& type, const uint8_t* b, size_t n) {
        if (type != "traf") return;
        uint64_t base = at;                    // default-base-is-moof, or the first traf's default
        Trex def = trex;
        each_box(b, n, [&](const std::string& type, const uint8_t* b, size_t n) {
            if (n < 8) { ok = false; return; }
            const uint32_t flags = be32(b) & 0xffffff;
            if (type == "tfhd") {
                size_t f = 8;
                if (flags & 0x01) { if (f + 8 > n) { ok = false; return; } base = be64(b + f); f += 8; }
                if (flags & 0x02) f += 4;
                if (flags & 0x08) { if (f + 4 > n) { ok = false; return; } def.duration = be32(b + f); f += 4; }
                if (flags & 0x10) { if (f + 4 > n) { ok = false; return; } def.size = be32(b + f); f += 4; }
                if (flags & 0x20) { if (f + 4 > n) { ok = false; return; } def.flags = be32(b + f); f += 4; }
            } else if (type == "trun") {
                const uint32_t count = be32(b + 4);
                size_t f = 8;
                uint64_t off = base;
                uint32_t first_flags = 0;
                if (flags & 0x001) { if (f + 4 > n) { ok = false; return; } off = base + (int32_t)be32(b + f); f += 4; }
                if (flags & 0x004) { if (f + 4 > n) { ok = false; return; } first_flags = be32(b + f); f += 4; }
                for (uint32_t i = 0; i < count; i++) {
                    Sample s{off, def.size, def.duration, false};
                    uint32_t sflags = (i == 0 && (flags & 0x004)) ? first_flags : def.flags;
                    if (flags & 0x100) { if (f + 4 > n) { ok = false; return; } s.duration = be32(b + f); f += 4; }
                    if (flags & 0x200) { if (f + 4 > n) { ok = false; return; } s.size = be32(b + f); f += 4; }
                    if (flags & 0x400) { if (f + 4 > n) { ok = false; return; } sflags = be32(b + f); f += 4; }
                    if (flags & 0x800) f += 4;     // composition offsets: the DVR writes none
                    s.sync = !(sflags & kNonSync);
                    out.push_back(s);
                    off += s.size;
                    any = true;
                }
            }
        });
    });
    return ok && any;
}

// Fill in an hvcC record's SPS-derived fields from its first SPS.
void fix_hvcc(uint8_t* c, size_t n) {
    if (n < 23) return;
    size_t o = 23;
    for (int a = 0, arrays = c[22]; a < arrays && o + 3 <= n; a++) {
        const int type = c[o] & 0x3f, count = (c[o + 1] << 8) | c[o + 2];
        o += 3;
        for (int k = 0; k < count && o + 2 <= n; k++) {
            const size_t len = (c[o] << 8) | c[o + 1];
            if (o + 2 + len > n) return;
            unsigned char ptl[12];
            int chroma, luma8, chroma8;
            if (type == 33 && MP4E_hevc_sps_config(c + o + 2, (int)len, ptl, &chroma, &luma8, &chroma8)) {
                memcpy(c + 1, ptl, 12);
                c[16] = 0xfc | chroma;
                c[17] = 0xf8 | luma8;
                c[18] = 0xf8 | chroma8;
                return;
            }
            o += 2 + len;
        }
    }
}

// The sample table of a plain file, after the fragmented file's sample
// description (stsd, kept).
Bytes plain_stbl(const Bytes& stsd, const std::vector<Sample>& s) {
    Bytes stts{0, 0, 0, 0}, stsc{0, 0, 0, 0}, stsz{0, 0, 0, 0}, co{0, 0, 0, 0}, stss{0, 0, 0, 0};
    // Durations, run-length coded.
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (const Sample& x : s) {
        if (!runs.empty() && runs.back().second == x.duration) runs.back().first++;
        else runs.push_back({1, x.duration});
    }
    add32(stts, (uint32_t)runs.size());
    for (auto& r : runs) { add32(stts, r.first); add32(stts, r.second); }
    // One sample a chunk: each sat in its own mdat.
    add32(stsc, 1); add32(stsc, 1); add32(stsc, 1); add32(stsc, 1);
    add32(stsz, 0); add32(stsz, (uint32_t)s.size());
    for (const Sample& x : s) add32(stsz, x.size);
    const bool wide = !s.empty() && s.back().off + s.back().size > 0xffffffffull;
    add32(co, (uint32_t)s.size());
    for (const Sample& x : s) {
        if (wide) { add32(co, (uint32_t)(x.off >> 32)); }
        add32(co, (uint32_t)x.off);
    }
    uint32_t syncs = 0;
    for (const Sample& x : s) syncs += x.sync;
    Bytes out = box("stsd", stsd);
    for (const Bytes& b : {box("stts", stts), box("stsc", stsc), box("stsz", stsz), box(wide ? "co64" : "stco", co)})
        out.insert(out.end(), b.begin(), b.end());
    if (syncs != s.size()) {
        add32(stss, syncs);
        for (size_t i = 0; i < s.size(); i++) if (s[i].sync) add32(stss, (uint32_t)(i + 1));
        Bytes b = box("stss", stss);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

// A header box's duration field, set in place: mvhd, tkhd and mdhd have it
// at these offsets (version 0 / version 1).
void set_duration(Bytes& body, size_t v0, size_t v1, uint64_t d) {
    if (body.empty()) return;
    if (body[0] == 1) { if (v1 + 8 <= body.size()) put64(&body[v1], d); }
    else if (v0 + 4 <= body.size()) put32(&body[v0], (uint32_t)std::min<uint64_t>(d, 0xffffffffu));
}

// The plain moov: the fragmented one without mvex, with its durations and
// its sample table filled in. Empty if it does not have the one track the
// DVR writes.
Bytes plain_moov(const uint8_t* m, size_t n, const std::vector<Sample>& s) {
    uint32_t movie_scale = 0, media_scale = 0;
    int traks = 0;
    each_box(m, n, [&](const std::string& t, const uint8_t* b, size_t n) {
        if (t == "mvhd" && n >= 24) movie_scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
        if (t != "trak") return;
        traks++;
        each_box(b, n, [&](const std::string& t, const uint8_t* b, size_t n) {
            if (t != "mdia") return;
            each_box(b, n, [&](const std::string& t, const uint8_t* b, size_t n) {
                if (t == "mdhd" && n >= 24) media_scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
            });
        });
    });
    if (traks != 1 || !movie_scale || !media_scale) return {};
    uint64_t media = 0;
    for (const Sample& x : s) media += x.duration;
    const uint64_t movie = media * movie_scale / media_scale;

    bool ok = true;
    // A container's children, each kept, dropped or rebuilt.
    std::function<Bytes(const uint8_t*, size_t)> rebuild = [&](const uint8_t* b, size_t n) {
        Bytes out;
        bool have_stsd = false;
        Bytes stsd;
        each_box(b, n, [&](const std::string& t, const uint8_t* cb, size_t cn) {
            Bytes body(cb, cb + cn);
            if (t == "mvex") return;                                   // no fragments any more
            if (t == "trak" || t == "mdia" || t == "minf") {
                body = rebuild(cb, cn);
            } else if (t == "stbl") {
                body.clear();
                each_box(cb, cn, [&](const std::string& t, const uint8_t* sb, size_t sn) {
                    if (t == "stsd") { stsd.assign(sb, sb + sn); have_stsd = true; }
                });
                if (!have_stsd) { ok = false; return; }
                // The HEVC decoder configuration, completed from its SPS.
                for (size_t i = 8; i + 4 <= stsd.size(); i++) {
                    if (memcmp(&stsd[i], "hvcC", 4) != 0) continue;
                    const uint32_t size = be32(&stsd[i - 4]);
                    if (size >= 8 && i - 4 + size <= stsd.size()) fix_hvcc(&stsd[i + 4], size - 8);
                    break;
                }
                body = plain_stbl(stsd, s);
            } else if (t == "mvhd") {
                set_duration(body, 16, 24, movie);
            } else if (t == "tkhd") {
                set_duration(body, 20, 28, movie);
            } else if (t == "mdhd") {
                set_duration(body, 16, 24, media);
            }
            Bytes bx = box(t.c_str(), body);
            out.insert(out.end(), bx.begin(), bx.end());
        });
        return out;
    };
    Bytes moov = box("moov", rebuild(m, n));
    return ok ? moov : Bytes{};
}

bool pread_all(int fd, void* b, size_t n, uint64_t off) { return pread(fd, b, n, (off_t)off) == (ssize_t)n; }
bool pwrite_all(int fd, const void* b, size_t n, uint64_t off) { return pwrite(fd, b, n, (off_t)off) == (ssize_t)n; }

// A top-level box header at `off`: its type and size, 0 if there is none.
uint64_t box_at(int fd, uint64_t off, uint64_t file_size, char type[5]) {
    uint8_t h[16];
    if (off + 8 > file_size || !pread_all(fd, h, std::min<uint64_t>(16, file_size - off), off)) return 0;
    memcpy(type, h + 4, 4);
    type[4] = 0;
    uint64_t size = be32(h);
    if (size == 1) size = off + 16 <= file_size ? be64(h + 8) : 0;
    else if (size == 0) size = file_size - off;
    return size >= 8 ? size : 0;
}

} // namespace

bool dvr_finalize_fragmented(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool done = false;
    do {
        if (fstat(fd, &st) != 0) break;
        const uint64_t file_size = (uint64_t)st.st_size;
        char type[5];
        // ftyp, then the fragmented file's moov - one with an mvex.
        const uint64_t ftyp = box_at(fd, 0, file_size, type);
        if (!ftyp || strcmp(type, "ftyp") != 0) break;
        const uint64_t moov_pos = ftyp;
        const uint64_t moov_size = box_at(fd, moov_pos, file_size, type);
        if (!moov_size || strcmp(type, "moov") != 0 || moov_size > (1u << 20) || moov_pos + moov_size > file_size) break;
        Bytes moov(moov_size - 8);
        if (!pread_all(fd, moov.data(), moov.size(), moov_pos + 8)) break;
        bool fragmented = false;
        Trex trex;
        each_box(moov.data(), moov.size(), [&](const std::string& t, const uint8_t* b, size_t n) {
            if (t != "mvex") return;
            fragmented = true;
            each_box(b, n, [&](const std::string& t, const uint8_t* b, size_t n) {
                if (t == "trex" && n >= 24) { trex.duration = be32(b + 12); trex.size = be32(b + 16); trex.flags = be32(b + 20); }
            });
        });
        if (!fragmented) break;

        // The fragments, up to the first that is not all there.
        std::vector<Sample> samples;
        uint64_t off = moov_pos + moov_size, first_moof = 0, data_end = off, plain_moov_at = 0;
        bool first_is_mdat = false;
        Bytes moof;
        while (off < file_size) {
            const uint64_t size = box_at(fd, off, file_size, type);
            if (!size || off + size > file_size) break;               // cut short here
            if (!strcmp(type, "moof")) {
                if (size > (1u << 20) || size < 16) break;
                moof.resize(size - 8);
                std::vector<Sample> frag;
                if (!pread_all(fd, moof.data(), moof.size(), off + 8) ||
                    !read_moof(moof.data(), moof.size(), off, trex, frag))
                    break;
                bool whole = true;
                for (const Sample& x : frag) whole = whole && x.off >= off + size && x.off + x.size <= file_size;
                if (!whole) break;                                    // its picture never made it
                if (!first_moof) first_moof = off;
                for (const Sample& x : frag) data_end = std::max(data_end, x.off + x.size);
                samples.insert(samples.end(), frag.begin(), frag.end());
            } else if (!strcmp(type, "mdat")) {
                if (off == moov_pos + moov_size) first_is_mdat = true;
                data_end = std::max(data_end, off + size);
            } else if (!strcmp(type, "moov")) {
                plain_moov_at = off;                                   // one of ours, from a run cut short
                break;
            } else if (strcmp(type, "free") != 0 && strcmp(type, "skip") != 0) {
                break;
            }
            off += size;
        }

        uint8_t free_type[4] = {'f', 'r', 'e', 'e'};
        if (samples.empty()) {
            // The one step a finalize can have stopped before: its mdat and
            // plain moov are there, the fragmented moov still says otherwise.
            if (first_is_mdat && plain_moov_at && pwrite_all(fd, free_type, 4, moov_pos + 4)) {
                fsync(fd);
                printf("DVR: finished finalizing %s\n", path.c_str());
                done = true;
            }
            break;
        }

        Bytes plain = plain_moov(moov.data(), moov.size(), samples);
        if (plain.empty()) break;
        const bool cut = data_end < file_size && !plain_moov_at;

        // 1. Everything after the last whole fragment goes, and the plain moov
        //    takes its place. Until 3 the file is still the fragmented one,
        //    now with a moov after the fragments that a new run truncates.
        if (ftruncate(fd, (off_t)data_end) != 0 || !pwrite_all(fd, plain.data(), plain.size(), data_end)) break;
        fsync(fd);
        // 2. The fragments, one mdat. 3. The fragmented moov, padding.
        uint8_t mdat[16];
        put32(mdat, 1);
        memcpy(mdat + 4, "mdat", 4);
        put64(mdat + 8, data_end - first_moof);
        if (!pwrite_all(fd, mdat, sizeof(mdat), first_moof) || !pwrite_all(fd, free_type, 4, moov_pos + 4)) break;
        fsync(fd);

        printf("DVR: finalized %s, which was left fragmented - %zu pictures%s\n", path.c_str(), samples.size(),
               cut ? ", the last, cut off, dropped" : "");
        done = true;
    } while (false);
    close(fd);
    return done;
}

void dvr_recover_recordings(const std::string& dir, const std::function<std::string()>& busy) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n[0] != '.' && n.compare(n.size() - 4, 4, ".mp4") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (busy && busy() == n) continue;
        dvr_finalize_fragmented(dir + "/" + n);
    }
}
