#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace dvr {
// Owns one child process. No shell, no inherited hardware handles. Originals
// are read-only; only a finalized, successful result is published in exports/.
class ExportJob {
  public:
    struct State {
        bool busy = false;
        int percent = 0;
        uint64_t revision = 0;
        std::string file, message, output;
    };
    struct Media {
        std::function<void(bool)> suspend;
        std::function<bool()> released;
    };
    ExportJob(std::string directory, std::string executable, Media media);
    ~ExportJob();
    void start(const std::string &name, std::vector<std::string> options);
    void cancel();
    State state() const;
    bool available() const;

  private:
    void run(std::string name, std::vector<std::string> options);
    std::string directory_, executable_;
    Media media_;
    mutable std::mutex mutex_;
    State state_;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};
} // namespace dvr
