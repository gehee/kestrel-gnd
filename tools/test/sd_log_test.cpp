// Test of base/sd_log.cpp: the log reaches the card and is closed when turned
// off, a process using it exits cleanly with its threads still running, and
// the logs kept are the newest by their number, not by the clock.
// Before sd_log's shared state was made never to be destroyed, exit() tore it
// down under them: depending on the order static destructors ran in, the
// writer swapped a freed string (heap corruption, SIGABRT in sd-log-write,
// as kestrel-gnd did on a VRX Pro) or exit() and the writer deadlocked on the
// destroyed condition variable and mutex (a hang). Run against the old code,
// this test aborts in both rounds.
//
// Each case runs in a child, as sd_log takes over the process's stdout. The
// "card" is a tmpfs mount (/dev/shm, /tmp or /run, whichever is a mount
// point), since sd_log only writes to a mounted partition.
//   g++ -std=c++17 -I. -pthread -o sd_log_test base/sd_log.cpp tools/test/sd_log_test.cpp && ./sd_log_test
// base/sd_log.cpp first on the link line: the order that took the old code
// down every time.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/sd_log.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// A directory that is the root of its own mount, as sd_log requires.
static std::string mounted_dir() {
    for (const char *d : {"/dev/shm", "/tmp", "/run"}) {
        struct stat st, parent;
        const std::string up = std::string(d) + "/..";
        if (stat(d, &st) == 0 && S_ISDIR(st.st_mode) && stat(up.c_str(), &parent) == 0 &&
            parent.st_dev != st.st_dev && access(d, W_OK) == 0)
            return d;
    }
    return "";
}

static std::set<std::string> logs_in(const std::string &dir) {
    std::set<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("fpvOS_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
                out.insert(n);
        }
        closedir(d);
    }
    return out;
}

static std::string read_file(const std::string &path) {
    std::string s;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

// Runs fn in a child; its exit status (or 128 + the signal that killed it).
// A child still there after 20 s is stuck and is killed by its alarm.
template <class F> static int in_child(F fn) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) { alarm(20); fn(); exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

// The log is written to the card, synced while on, and closed when off.
static void test_log_reaches_card(const std::string &dir) {
    const std::set<std::string> before = logs_in(dir);
    const int r = in_child([&] {
        sd_log::init(dir);
        sd_log::set_enabled(true);
        for (int i = 0; i < 50; i++) printf("sd_log_test line %d\n", i);
        fflush(stdout);
        usleep(2600 * 1000);                     // one 2 s sync
        sd_log::set_enabled(false);
        _exit(0);                                // nothing more through the pipe
    });
    CHECK(r == 0);
    std::set<std::string> made;
    for (const std::string &n : logs_in(dir)) if (!before.count(n)) made.insert(n);
    CHECK(made.size() == 1);
    for (const std::string &n : made) {
        const std::string text = read_file(dir + "/" + n);
        CHECK(text.find("sd-log: this log is also in") != std::string::npos);
        CHECK(text.find("sd_log_test line 0\n") != std::string::npos);
        CHECK(text.find("sd_log_test line 49\n") != std::string::npos);
        CHECK(text.find("sd-log: off") != std::string::npos);
        unlink((dir + "/" + n).c_str());
    }
}

// Exiting as returning from main does (static destructors run) with the log
// on and text pending. Before the fix this hung or aborted.
static void test_clean_exit_with_writer_running(const std::string &dir) {
    const std::set<std::string> before = logs_in(dir);
    for (int round = 0; round < 2; round++) {
        const int r = in_child([&] {
            sd_log::init(dir);
            sd_log::set_enabled(true);
            // Well past the short-string size, so `pending` holds heap memory.
            for (int i = 0; i < 200; i++) printf("pending text that the writer has not flushed yet %d\n", i);
            fflush(stdout);
            usleep(300 * 1000);                  // the reader has it all
        });
        CHECK(r == 0);
        if (r != 0) printf("  child exit status %d (134: SIGABRT, 142: hung, killed by its alarm)\n", r);
    }
    for (const std::string &n : logs_in(dir)) if (!before.count(n)) unlink((dir + "/" + n).c_str());
}

// Logs from a clock ahead of this one (as every log is once the goggle's
// clock is back at 2017): the oldest by number go, and a new log - which
// sorts first by name - stays at the next start.
static void test_prune_by_number(const std::string &dir) {
    const std::set<std::string> before = logs_in(dir);
    std::vector<std::string> old;
    for (int i = 1; i <= 30; i++) {
        char name[64];
        snprintf(name, sizeof name, "fpvOS_20991231_%06d.log", i);
        old.push_back(name);
        if (FILE *f = fopen((dir + "/" + name).c_str(), "w")) {
            fprintf(f, "sd-log: log #%d on this card\nold log\n", i);
            fclose(f);
        }
    }
    auto start = [&] {
        return in_child([&] {
            sd_log::init(dir);
            sd_log::set_enabled(true);
            sd_log::set_enabled(false);
            _exit(0);
        });
    };
    CHECK(start() == 0);
    std::set<std::string> first;
    for (const std::string &n : logs_in(dir))
        if (!before.count(n) && n.rfind("fpvOS_2099", 0) != 0) first.insert(n);
    CHECK(first.size() == 1);
    for (const std::string &n : first)
        CHECK(read_file(dir + "/" + n).rfind("sd-log: log #31 on this card\n", 0) == 0);
    CHECK(start() == 0);   // most likely the same second: the name gets _1
    const std::set<std::string> now = logs_in(dir);
    for (const std::string &n : first) CHECK(now.count(n) == 1);   // the newer one stayed
    CHECK(!now.count(old[0]) && !now.count(old[1]));             // the two oldest went
    CHECK(now.count(old[2]) == 1 && now.count(old[29]) == 1);
    for (const std::string &n : now) if (!before.count(n)) unlink((dir + "/" + n).c_str());
}

int main() {
    // One at a time on this machine: the logs of two runs (two build trees'
    // ctest) in the same folder would be pruned and counted by each other.
    const int lock = open("/tmp/kestrel_sd_log_test.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (lock >= 0) flock(lock, LOCK_EX);
    const std::string dir = mounted_dir();
    if (dir.empty()) {
        printf("sd_log_test: no tmpfs mount to stand in for the card - skipped\n");
        return 0;
    }
    test_log_reaches_card(dir);
    test_clean_exit_with_writer_running(dir);
    test_prune_by_number(dir);
    printf(fails ? "sd_log_test: %d FAILED\n" : "sd_log_test: all passed\n", fails);
    return fails ? 1 : 0;
}
