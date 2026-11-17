#include "dvr/writer.hpp"
#include "dvr/stream_recorder.hpp"
#include "recording_fixture.hpp"
#include <filesystem>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

namespace {
std::mutex mutex;
std::condition_variable changed;
bool block_write = true, entered = false;
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t bytes, off_t offset) {
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [] { return !block_write; });
    }
    return __real_pwrite(fd, data, bytes, offset);
}
int main() {
    char path[] = "/tmp/kestrel-writer-XXXXXX";
    const int fd = mkstemp(path);
    check(fd >= 0, "temporary file");
    unlink(path);
    std::array<unsigned char, 32> data{};
    for (size_t i = 0; i < data.size(); ++i) data[i] = i;
    {
        DvrWriter writer(fd, 8);
        check(writer.write(0, data.data(), 8) == 0, "first write");
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [] { return entered; });
        }
        auto next = std::async(std::launch::async, [&] { return writer.write(8, data.data()+8, 1); });
        const bool bounded = next.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout;
        {
            std::lock_guard<std::mutex> lock(mutex);
            block_write = false;
        }
        changed.notify_all();
        const int result = next.get();
        // Larger than the entire budget: must split, complete and preserve offsets.
        check(writer.write(9, data.data()+9, 23) == 0, "oversized write");
        check(writer.finish(), "finish");
        check(bounded, "in-flight bytes must stay charged against the memory budget");
        check(result == 0, "blocked writer resumes");
    }
    std::array<unsigned char, 32> actual{};
    check(pread(fd, actual.data(), actual.size(), 0) == 32 && actual == data, "written bytes and offsets");
    {
        block_write = true; entered = false;
        DvrWriter writer(fd, 8);
        check(writer.write(0, data.data(), 8) == 0, "shutdown first write");
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [] { return entered; });
        }
        auto next = std::async(std::launch::async, [&] { return writer.write(8, data.data()+8, 1); });
        auto finish = std::async(std::launch::async, [&] { return writer.finish(); });
        const bool woke = next.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        {
            std::lock_guard<std::mutex> lock(mutex);
            block_write = false;
        }
        changed.notify_all();
        const int result = next.get();
        check(finish.get(), "shutdown drains accepted bytes");
        check(woke && result != 0, "finish wakes and rejects blocked producers");
    }
    bool rejected = false;
    try { DvrWriter invalid(fd, 0); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "zero memory budget rejected");
    close(fd);
    // Stall actual recorder writes, fill its input queue, and verify it
    // gives radio-owned pictures back before the SD stall is released.
    char directory[] = "/tmp/kestrel-recorder-stall-XXXXXX";
    check(mkdtemp(directory), "recorder directory");
    {
        block_write = true; entered = false;
        dvr::StreamRecorder recorder(directory, VideoCodec::H265, 64, 64, 60);
        recorder.set_parameter_sets(recording_ps);
        recorder.toggle();
        for (int i = 0; i < 1000 && !recorder.state().recording; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::vector<std::weak_ptr<std::vector<uint8_t>>> refs;
        // A large opaque slice exercises ownership, not decoder correctness.
        for (int i = 0; i < 200 && recorder.is_recording(); ++i) {
            auto frame = std::make_shared<std::vector<uint8_t>>(recording_frames[0]);
            frame->resize(256u << 10, 0x55);
            refs.push_back(frame);
            recorder.feed(std::move(frame), 1000000 + i * 16667);
        }
        size_t retained = 0;
        for (const auto& ref : refs) if (!ref.expired()) ++retained;
        const bool stopped = !recorder.is_recording();
        const std::string error = recorder.state().error;
        {
            std::lock_guard<std::mutex> lock(mutex);
            block_write = false;
        }
        changed.notify_all();
        recorder.stop();
        check(stopped && error == "Recording storage cannot keep up", "slow storage stops recording");
        check(retained <= 2, "failed queue immediately releases all but active frame references");
    }
    std::filesystem::remove_all(directory);
}
