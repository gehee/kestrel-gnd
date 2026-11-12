#include "export_job.hpp"
#include "base/shell.hpp"
#include "dvr_library.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace dvr {
namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
struct Fd {
    int n = -1;
    ~Fd() {
        if (n >= 0)
            close(n);
    }
};
// Partial files (.hud-XXXXXX) left by a worker that outlived a crashed
// kestrel-gnd: one export runs at a time, so outside run() every one is stale.
void remove_partials(const std::string &folder) noexcept {
    struct stat st{};
    if (lstat(folder.c_str(), &st) || !S_ISDIR(st.st_mode)) // never through a symlink
        return;
    try {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(folder, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() == 11 && name.compare(0, 5, ".hud-") == 0 && entry.is_regular_file(ec))
                std::filesystem::remove(entry.path(), ec);
        }
    } catch (const std::exception &) {
    }
}
} // namespace
ExportJob::ExportJob(std::string d, std::string e, Media media)
    : directory_(std::move(d)), executable_(std::move(e)), media_(std::move(media)) {
    remove_partials(directory_ + "/exports");
}
ExportJob::~ExportJob() {
    cancel();
    if (thread_.joinable())
        thread_.join();
}
ExportJob::State ExportJob::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
bool ExportJob::available() const { return access(executable_.c_str(), X_OK) == 0; }
void ExportJob::cancel() {
    cancel_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.busy)
        state_.message = "Canceling export";
}
void ExportJob::start(const std::string &name, std::vector<std::string> options) {
    require(dvr_lib::safe_name(name) && dvr_lib::ends_with(name, ".mp4"), "Not a recording filename");
    require(available(), "Export worker unavailable");
    require(!state().busy, "An export is already running");
    if (thread_.joinable())
        thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.busy = true;
        state_.percent = 0;
        state_.file = name;
        state_.output.clear();
        state_.message = "Preparing export";
    }
    cancel_ = false;
    try {
        thread_ = std::thread(
            [this, name, options = std::move(options)]() mutable { run(name, std::move(options)); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.busy = false;
        throw;
    }
}
void ExportJob::run(std::string name, std::vector<std::string> options) {
    std::string temporary, published, message;
    pid_t child = -1;
    bool suspended = false;
    try {
        require(!cancel_, "Export canceled");
        // Refuse symlink output directories. A private partial file is invisible
        // to the gallery and removed on every failure/cancel path.
        const auto folder = directory_ + "/exports";
        if (mkdir(folder.c_str(), 0755) < 0 && errno != EEXIST)
            throw std::runtime_error("Cannot create exports folder");
        struct stat st{};
        require(!lstat(folder.c_str(), &st) && S_ISDIR(st.st_mode), "Invalid exports folder");
        remove_partials(folder);
        std::string pattern = folder + "/.hud-XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back(0);
        Fd file;
        file.n = mkstemp(path.data());
        require(file.n >= 0, "Cannot create export file");
        temporary = path.data();
        fcntl(file.n, F_SETFD, FD_CLOEXEC);
        const auto suffix = temporary.substr(temporary.size() - 6);
        published =
            folder + "/" + name.substr(0, std::min<size_t>(name.size() - 4, 180)) + "-hud-" + suffix + ".mp4";
        suspended = true;
        media_.suspend(true);
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!media_.released()) {
            require(!cancel_, "Export canceled");
            require(Clock::now() < deadline, "Live decoder did not release export resources");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(!cancel_, "Export canceled");
        int pipes[2];
        require(pipe2(pipes, O_CLOEXEC) == 0, "Cannot create export status pipe");
        Fd read, write;
        read.n = pipes[0];
        write.n = pipes[1];
        posix_spawn_file_actions_t actions;
        require(!posix_spawn_file_actions_init(&actions), "Cannot prepare export process");
        struct Actions {
            posix_spawn_file_actions_t *p;
            ~Actions() { posix_spawn_file_actions_destroy(p); }
        } cleanup{&actions};
        require(!posix_spawn_file_actions_adddup2(&actions, write.n, STDOUT_FILENO),
                "Cannot connect export status");
        require(!spawn_close_descriptors(&actions), "Cannot isolate export descriptors");
        std::vector<std::string> args{executable_, directory_ + "/" + name, temporary};
        args.insert(args.end(), options.begin(), options.end());
        std::vector<char *> argv;
        for (auto &a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        const int error = posix_spawn(&child, executable_.c_str(), &actions, nullptr, argv.data(), environ);
        if (error) {
            child = -1;
            throw std::runtime_error(std::string("Cannot start export: ") + strerror(error));
        }
        close(write.n);
        write.n = -1;
        fcntl(read.n, F_SETFL, O_NONBLOCK);
        std::string pending, worker_error;
        bool done = false, sent_term = false, exited = false;
        int status = 0;
        auto last_progress = Clock::now(), cancel_at = last_progress;
        auto consume = [&] {
            char bytes[1024];
            for (;;) {
                auto n = ::read(read.n, bytes, sizeof(bytes));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                pending.append(bytes, n);
                require(pending.size() < 16384, "Invalid export status");
                size_t end;
                while ((end = pending.find('\n')) != std::string::npos) {
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    int percent;
                    if (sscanf(line.c_str(), "EXPORT PROGRESS %d", &percent) == 1) {
                        require(percent >= 0 && percent <= 100, "Invalid export progress");
                        last_progress = Clock::now();
                        std::lock_guard<std::mutex> lock(mutex_);
                        state_.percent = percent;
                        state_.message = cancel_ ? "Canceling export" : "Exporting with HUD";
                    } else if (line == "EXPORT DONE")
                        done = true;
                    else if (line.compare(0, 13, "EXPORT ERROR ") == 0)
                        worker_error = line.substr(13);
                }
            }
        };
        while (!exited) {
            consume();
            if (cancel_ && !sent_term) {
                kill(child, SIGTERM);
                sent_term = true;
                cancel_at = Clock::now();
            }
            if (sent_term && Clock::now() - cancel_at > std::chrono::seconds(15))
                kill(child, SIGKILL);
            require(Clock::now() - last_progress < std::chrono::minutes(5), "Export stopped making progress");
            const auto r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                child = -1;
                exited = true;
            } else
                require(r >= 0 || errno == EINTR, "Cannot wait for export worker");
            if (!exited) {
                pollfd p{read.n, POLLIN, 0};
                poll(&p, 1, 50);
            }
        }
        consume();
        require(!cancel_, "Export canceled");
        if (!worker_error.empty())
            throw std::runtime_error(worker_error);
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && done, "Export worker failed");
        require(!fstat(file.n, &st) && st.st_size > 16, "Export produced no video");
        // Atomic no-overwrite publication also works on FAT/exFAT cards.
        require(syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, published.c_str(),
                        RENAME_NOREPLACE) == 0,
                "Cannot publish exported video");
        temporary.clear();
        Fd dir;
        dir.n = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir.n >= 0)
            fsync(dir.n);
        message = "Export saved: exports/" + std::filesystem::path(published).filename().string();
    } catch (const std::exception &e) {
        message = cancel_ ? "Export canceled" : std::string("Export failed: ") + e.what();
        published.clear();
    }
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    if (!temporary.empty())
        unlink(temporary.c_str());
    if (suspended)
        media_.suspend(false);
    fprintf(stderr, "export: %s\n", message.c_str());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.busy = false;
        state_.message = message;
        state_.output = published;
        if (!published.empty())
            state_.percent = 100;
        ++state_.revision;
    }
}
} // namespace dvr
