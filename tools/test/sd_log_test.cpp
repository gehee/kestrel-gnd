// Test of utils/sd_log.cpp: the log reaches the card and is closed when turned
// off, and a process using it exits cleanly with its threads still running.
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
//   g++ -std=c++17 -I. -pthread -o sd_log_test utils/sd_log.cpp tools/test/sd_log_test.cpp && ./sd_log_test
// utils/sd_log.cpp first on the link line: the order that took the old code
// down every time.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/sd_log.hpp"

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

int main() {
    const std::string dir = mounted_dir();
    if (dir.empty()) {
        printf("sd_log_test: no tmpfs mount to stand in for the card - skipped\n");
        return 0;
    }
    test_log_reaches_card(dir);
    test_clean_exit_with_writer_running(dir);
    printf(fails ? "sd_log_test: %d FAILED\n" : "sd_log_test: all passed\n", fails);
    return fails ? 1 : 0;
}
