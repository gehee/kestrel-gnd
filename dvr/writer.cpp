#include "writer.hpp"
#include "base/time_util.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size()-suffix.size(), suffix.size(), suffix)==0;
}
// A recording purge_oldest_recordings may delete.
bool purgeable(const std::string& name, const std::string& keep) {
    if (name.rfind("fpvOS_", 0) != 0 && name.rfind("kestrel_", 0) != 0 && name.rfind("recording_", 0) != 0) return false;
    return (ends_with(name, ".mp4") || ends_with(name, ".h265")) && name != keep;
}
uint64_t size_of(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
}
// dir's purgeable recordings, oldest first: their mtime and path.
std::vector<std::pair<time_t, std::string>> recordings_in(const std::string& dir, const std::string& keep) {
    std::vector<std::pair<time_t, std::string>> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const std::string name = ent->d_name;
        if (!purgeable(name, keep)) continue;
        const std::string path = dir + "/" + name;
        struct stat fst;
        if (stat(path.c_str(), &fst) != 0 || !S_ISREG(fst.st_mode)) continue;
        files.emplace_back(fst.st_mtime, path);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}
// A recording's companions: its gallery thumbnail and its telemetry sidecar.
std::vector<std::string> companions(const std::string& dir, const std::string& path) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    return {dir + "/.thumbs/" + name + ".jpg", path + ".telemetry.jsonl"};
}
}
// The free space kept on the card while recording: a minute of the recording
// at its rate so far, and at least what REC start makes (no rate known yet).
// Looked at every 32 MB written or every 5 s, whichever comes first.
constexpr uint64_t kMinFreeBytes    = kDvrMinFreeBytes;
constexpr uint64_t kHeadroomUs      = 60000000;
constexpr uint64_t kSpaceCheckBytes = 32ull << 20;
constexpr uint64_t kSpaceCheckUs    = 5000000;

DvrWriter::DvrWriter(int fd, std::string dir, std::string name, std::function<void(const char*)> on_fail, size_t max_queued)
	: fd_(fd), max_queued_(max_queued), dir_(std::move(dir)), name_(std::move(name)), on_fail_(std::move(on_fail)) {
	start_us_ = checked_us_ = get_time_us();
	t_ = std::thread([this] { run(); });
}

// A minute of the recording at its rate so far, kMinFreeBytes at least.
uint64_t DvrWriter::headroom() const {
	const uint64_t el = get_time_us() - start_us_;
	const uint64_t room = el >= 2000000 ? (uint64_t)((double)written_ * kHeadroomUs / el) : 0;
	return std::max(kMinFreeBytes, room);
}

DvrWriter::DvrWriter(int fd, size_t max_queued)
    : DvrWriter(fd, "", "", nullptr, max_queued) {}

DvrWriter::~DvrWriter() { finish(); }
bool DvrWriter::finish() {
	{
		std::lock_guard<std::mutex> lk(m_);
		done_ = true;
	}
	work_.notify_one();
	if (t_.joinable()) t_.join();
	return !failed_;
}

int DvrWriter::write(int64_t offset, const void* buf, size_t size) {
	if (failed_) return 1;   // the card stopped taking it: nothing more is queued
	const uint8_t* p = static_cast<const uint8_t*>(buf);
	{
		std::unique_lock<std::mutex> lk(m_);
		room_.wait(lk, [this] { return queued_ < max_queued_ || failed_ || done_; });
		if (failed_ || done_) return 1;
		q_.push_back({offset, std::vector<uint8_t>(p, p + size)});
		queued_ += size;
	}
	work_.notify_one();
	return failed_ ? 1 : 0;
}

void DvrWriter::run() {
	pthread_setname_np(pthread_self(), "dvr-writer");
	uint64_t last_sync_us = get_time_us();
	for (;;) {
		std::deque<Chunk> batch;
		bool done;
		{
			std::unique_lock<std::mutex> lk(m_);
			work_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !q_.empty() || done_; });
			batch.swap(q_);
			queued_ = 0;
			done = done_;
		}
		room_.notify_all();
		for (const Chunk& c : batch) {
			size_t off = 0;
			int room_tries = 0;
			while (off < c.data.size() && !failed_) {
				ssize_t n = pwrite(fd_, c.data.data() + off, c.data.size() - off, (off_t)(c.offset + off));
				if (n > 0) { off += (size_t)n; written_ += (uint64_t)n; continue; }
				const int err = n < 0 ? errno : 0;
				if (err == EINTR) continue;
				// The card full: the oldest finished recordings make way and the
				// write goes again - a few times, as one still open elsewhere (the
				// gallery playing it) frees nothing until it is closed.
				if (err == ENOSPC && !dir_.empty() && room_tries++ < 3 &&
				    purge_oldest_recordings(dir_, headroom(), name_) > 0)
					continue;
				// Said once: nothing is written after it, and the recording is
				// ended (on_fail) - it used to drop everything without a word,
				// REC still on the HUD.
				printf("DVR: write failed: %s - the recording ends here\n", err ? strerror(err) : "no progress");
				failed_ = true;
				room_.notify_all();
				if (on_fail_) on_fail_(err == ENOSPC ? "CARD FULL" : "CARD ERROR");
			}
		}
		// Room on the card, now and then, from the oldest finished recordings:
		// it was made at REC start only, and once the card was full every
		// recording stopped at the 300 MB made then.
		const uint64_t now = get_time_us();
		if (!dir_.empty() && !failed_ &&
		    (written_ - checked_at_ >= kSpaceCheckBytes || now - checked_us_ >= kSpaceCheckUs)) {
			checked_at_ = written_;
			checked_us_ = now;
			purge_oldest_recordings(dir_, headroom(), name_);
		}
		// Once a second, push what has been written onto the card: until then
		// it is only in the page cache, and on the exFAT DVR partition even the
		// file's length is not updated - a power cut would lose all of it. With
		// fragmented MP4 this bounds the loss to about the last second.
		if (done || now - last_sync_us >= 1000000) {
			last_sync_us = now;
			if (fsync(fd_) && !failed_.exchange(true)) {
				room_.notify_all();
				if (on_fail_) on_fail_("CARD ERROR");
			}
		}
		if (done) {
			std::lock_guard<std::mutex> lk(m_);
			if (q_.empty()) return;
		}
	}
}

uint64_t purge_oldest_recordings(const std::string& dir, uint64_t min_free_bytes, const std::string& keep) {
    struct statvfs st;
    if (statvfs(dir.c_str(), &st) != 0) return 0;
    uint64_t avail = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    if (avail >= min_free_bytes) return 0;

    uint64_t total = 0;
    for (const auto& f : recordings_in(dir, keep)) {
        if (avail >= min_free_bytes) break;
        uint64_t freed = size_of(f.second);
        if (remove(f.second.c_str()) == 0) {
            printf("DVR: low on space, removed oldest recording %s\n", f.second.c_str());
            for (const auto& c : companions(dir, f.second)) {
                const uint64_t n = size_of(c);
                if (n && remove(c.c_str()) == 0) freed += n;
            }
            avail += freed;
            total += freed;
        } else {
            printf("DVR: low on space, failed to remove %s: %s\n", f.second.c_str(), strerror(errno));
        }
    }
    return total;
}

uint64_t reclaimable_recording_bytes(const std::string& dir, const std::string& keep) {
    uint64_t total = 0;
    for (const auto& f : recordings_in(dir, keep)) {
        total += size_of(f.second);
        for (const auto& c : companions(dir, f.second)) total += size_of(c);
    }
    return total;
}
