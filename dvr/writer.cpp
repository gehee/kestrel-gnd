#include "writer.hpp"
#include "base/time_util.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

// Queued this far, a write waits for the card: ten seconds of the heaviest
// recording, far more than a sync ever takes.
DvrWriter::DvrWriter(int fd, size_t max_queued) : fd_(fd), max_queued_(max_queued) {
	t_ = std::thread([this] { run(); });
}

DvrWriter::~DvrWriter() {
    finish();
}
bool DvrWriter::finish() {
	{
		std::lock_guard<std::mutex> lk(m_);
		done_ = true;
	}
	work_.notify_one();
	if(t_.joinable()) t_.join();
	return !failed_;
}

int DvrWriter::write(int64_t offset, const void* buf, size_t size) {
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
			while (off < c.data.size() && !failed_) {
				ssize_t n = pwrite(fd_, c.data.data() + off, c.data.size() - off, (off_t)(c.offset + off));
				if (n > 0) off += (size_t)n;
				else if (!(n < 0 && errno == EINTR)) {
					printf("DVR: write failed: %s\n", n < 0 ? strerror(errno) : "no progress");
					failed_ = true;
					room_.notify_all();
				}
			}
		}
		// Once a second, push what has been written onto the card: until then
		// it is only in the page cache, and on the exFAT DVR partition even the
		// file's length is not updated - a power cut would lose all of it. With
		// fragmented MP4 this bounds the loss to about the last second.
		const uint64_t now = get_time_us();
		if (done || now - last_sync_us >= 1000000) {
			last_sync_us = now;
			if (fsync(fd_)) {failed_=true; room_.notify_all();}
		}
		if (done) {
			std::lock_guard<std::mutex> lk(m_);
			if (q_.empty()) return;
		}
	}
}

