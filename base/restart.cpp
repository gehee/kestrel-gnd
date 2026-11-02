#include "base/restart.hpp"

#include <atomic>
#include <cstdint>

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include "base/sd_log.hpp"

static std::atomic<void (*)()> g_restart_handler{nullptr};

void set_restart_handler(void (*fn)()) { g_restart_handler.store(fn); }

void kestrel_request_restart() {
    if (auto fn = g_restart_handler.load()) fn();
}

// The start state (restart_init), put back by restart_prepare_exec.
static uint64_t g_ignored_at_start = 0;   // bit s - 1: signal s was ignored
static bool g_sched_ok = false;
static int g_policy = SCHED_OTHER;
static struct sched_param g_param = {};
static bool g_cpus_ok = false;
static cpu_set_t g_cpus;

static void unblock_all() {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
}

void restart_init() {
    unblock_all();
    for (int s = 1; s < NSIG && s <= 64; s++) {
        struct sigaction sa;
        if (sigaction(s, nullptr, &sa) == 0 && sa.sa_handler == SIG_IGN)
            g_ignored_at_start |= 1ull << (s - 1);
    }
    g_policy = sched_getscheduler(0);
    g_sched_ok = g_policy >= 0 && sched_getparam(0, &g_param) == 0;
    g_cpus_ok = sched_getaffinity(0, sizeof g_cpus, &g_cpus) == 0;
}

void restart_prepare_exec() {
    sd_log::restore_output();   // the new process starts its own
    alarm(0);                   // it would outlive the exec and kill the new process
    // An ignored signal stays ignored across an exec, so ignoring is put back
    // to what it was at start (SIGPIPE was ours); a caught signal goes back
    // to the default by itself, so one not ignored at start stays caught.
    for (int s = 1; s < NSIG && s <= 64; s++) {
        if (s == SIGKILL || s == SIGSTOP) continue;
        struct sigaction now;
        if (sigaction(s, nullptr, &now) != 0) continue;   // the C library's own
        const bool ign = g_ignored_at_start & (1ull << (s - 1));
        if (ign == (now.sa_handler == SIG_IGN)) continue;
        struct sigaction sa = {};
        sa.sa_handler = ign ? SIG_IGN : SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(s, &sa, nullptr);
    }
    // The calling thread's, which the new process gets: any thread may exec
    // (an alarm lands on whichever), and a real-time one pinned to a core
    // would hand both to everything the new process starts.
    if (g_sched_ok) sched_setscheduler(0, g_policy, &g_param);
    if (g_cpus_ok) sched_setaffinity(0, sizeof g_cpus, &g_cpus);
    // Nothing above stderr has to survive: the splash is told through its
    // pidfile, and the service hands over only stdin, stdout and stderr. The
    // DRM fd must not (it carries master: the new image would come up with
    // no display), nor the decoder's buffers or the radio's sockets.
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (close_range(3, ~0u, 0) != 0)   // a kernel before 5.9
#endif
    {
        struct rlimit rl;
        int top = 1024;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
            top = rl.rlim_cur < 65536 ? (int)rl.rlim_cur : 65536;
        for (int fd = 3; fd < top; fd++) close(fd);
    }
    unblock_all();   // last: a pending signal finds the dispositions above
}
