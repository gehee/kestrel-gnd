#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
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
        DvrWriter(int fd, std::string dir, std::string name,
                  std::function<void(const char*)> on_fail, size_t max_queued = 64u << 20);
        ~DvrWriter();       // writes out everything queued, then syncs
        // Queue a write; 1 once any write has failed, as minimp4 expects.
        int write(int64_t offset, const void* buf, size_t size);
        bool failed() const { return failed_.load(); }
        bool finish();
    private:
        void run();
        uint64_t headroom() const;
        struct Chunk { int64_t offset; std::vector<uint8_t> data; };
        const int fd_;
        const size_t max_queued_;
        const std::string dir_, name_;
        const std::function<void(const char*)> on_fail_;
        uint64_t written_ = 0, checked_at_ = 0;
        uint64_t start_us_ = 0, checked_us_ = 0;
        std::mutex m_;
        std::condition_variable work_, room_;
        std::deque<Chunk> q_;
        size_t queued_ = 0;     // bytes in q_
        bool done_ = false;
        std::atomic<bool> failed_{false};
        std::thread t_;
};


// The free space kept on the recordings' card: what REC start makes, and the
// least a recording's writer keeps (DvrWriter::headroom).
inline constexpr uint64_t kDvrMinFreeBytes = 300ull << 20;

// Deletes the oldest recordings in dir - fpvOS_*.mp4 / .h265, and kestrel_* and
// recording_*, what earlier builds (the VRX Pro's, the Ascent's) named them -
// with their thumbnail and telemetry sidecar, until free space clears
// min_free_bytes or nothing is left; never `keep`, never anything else (the SD
// logs and the radio's .frames.csv share the folder and the prefix). Returns
// the bytes freed.
uint64_t purge_oldest_recordings(const std::string& dir, uint64_t min_free_bytes,
                                 const std::string& keep = "");
// The bytes purge_oldest_recordings could free in dir, `keep` aside.
uint64_t reclaimable_recording_bytes(const std::string& dir, const std::string& keep = "");
