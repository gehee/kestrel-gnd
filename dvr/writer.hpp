#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// The DVR's file writes, on a thread of their own.
//
// The muxer writes each picture as it is handed one, on the thread that takes
// the pictures. Writing there meant the once-a-second fsync - which puts the
// recording on the card, so a power cut loses a second at most - held that
// thread for as long as the card took, 50 to 250 ms on the DVR partition. A
// screen recording captures into a few buffers only, and with none coming
// back the captures in between were dropped: the recording froze for a few
// frames every second. Here the muxer's writes are only queued; this thread
// writes them, in order, and does the syncing.
class DvrWriter {
    public:
        explicit DvrWriter(int fd, size_t max_queued = 64u << 20);
        ~DvrWriter();       // writes out everything queued, then syncs
        // Queue a write; 1 once any write has failed, as minimp4 expects.
        int write(int64_t offset, const void* buf, size_t size);
        bool failed() const { return failed_.load(); }
        bool finish();
    private:
        void run();
        struct Chunk { int64_t offset; std::vector<uint8_t> data; };
        const int fd_;
        const size_t max_queued_;
        std::mutex m_;
        std::condition_variable work_, room_;
        std::deque<Chunk> q_;
        size_t queued_ = 0;     // bytes in q_
        bool done_ = false;
        std::atomic<bool> failed_{false};
        std::thread t_;
};

