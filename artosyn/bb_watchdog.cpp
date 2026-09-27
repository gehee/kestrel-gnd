// Bring-up watchdog for the AR8030 baseband.
//
// bb_socket_open() in the vendor SDK sends its request to the daemon and then
// waits on a plain pthread_cond_wait with no timeout and no retry
// (app/ar8030/session_socket.c:249). A lost reply blocks that thread forever,
// and it holds a mutex inside the library while it does - so the wedge cannot
// be unwound from inside the process. Nothing we do in our own code can make
// the call return.
//
// What CAN be done is notice it quickly and start over from a clean process.
// This thread watches for the video socket to open; if it has not within a
// short deadline, it restarts the daemon and re-execs us through
// /proc/self/exe. The stuck thread dies with the old process image, the SDK's
// state is built fresh, and the daemon is no longer waiting on a reply nobody
// is listening for.
//
// Two things this deliberately does NOT do:
//
//   It does not trigger on "no video socket yet". A call that RETURNS an error
//   is not the fault this exists for - when the daemon is down, bb_socket_open
//   fails promptly and the source's own retry loop reconnects the moment it
//   comes back. Timing the socket instead of the call conflates the two, and
//   the first version of this did exactly that: it restarted the daemon, the
//   retry loop recovered, and then it re-exec'd anyway and threw away a working
//   link. What it watches now is time spent INSIDE the call, which is the
//   symptom that is actually unrecoverable.
//
//   It does not wait a fixed settle after restarting. /root/bb-restart.sh
//   waits 75s because that is comfortably longer than the daemon has ever
//   needed, but most recoveries are ready far sooner. Each attempt waits a
//   little longer than the last, so a quick recovery costs seconds and a slow
//   one still converges - rather than every recovery costing the worst case.
//
// And before any of that, a lighter rung: the daemon is a TCP RPC server this
// thread can reach on its own connection, SDK mutex or no SDK mutex, and
// BB_FORCE_CLS_SOCKET_ALL makes it tear down the half-opened socket the parked
// call is waiting on. That frees the call without a re-exec or a daemon
// restart and without dropping the RF link. It is tried first on the first
// wedge; the ladder above is what happens if it does not work. See
// try_force_close_all().

#include "ar8030_source.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

// Last sign of life from the AR8030 thread: set when the bring-up starts and
// again on every pass of its run loop. Zero means it has not started yet.
//
// This began as "when did we enter bb_socket_open", which turned out to watch
// too little. A run was caught with the bring-up complete, both sockets open,
// and AR8030_RX parked in futex_wait_queue_me - an untimed condition wait in
// some later SDK call. The bring-up bracket could not see it, so the process
// sat connected and dead. What matters is not which call is in flight but
// whether the loop is still going round.
std::atomic<uint64_t> g_alive{0};
std::atomic<bool> g_socket_open{false};
std::atomic<bool> g_stop{false};

uint64_t now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

char **g_argv = nullptr;

// There is no duration that cleanly separates a slow bb_socket_open() from a
// parked one. Measured healthy calls: about a second usually, five seconds
// sometimes, and fifteen seconds more than once. Two deadlines were set here
// on the assumption that a long call meant a hung one - five, then fifteen -
// and both fired on calls that were about to succeed, restarting the daemon
// underneath a working bring-up, dropping the RF link and leaving the air unit
// blinking. That is a far worse outcome than waiting.
//
// The intervention is OFF by default. Three deadlines were chosen for it and
// all three were wrong in the same direction, which is the argument for not
// choosing a fourth:
//
//   5s  - fired on calls that returned at 5s
//   15s - fired on calls that returned at 15s
//   10s - after the bracket was widened from bb_socket_open to the whole of
//         connect_bb(), it re-exec'd the process 28 times in a single startup
//         before it happened to get through. A normal bring-up takes ~25s.
//
// Each time the measurement that "justified" the next value was contaminated
// by the previous one: the durations logged were the deadlines themselves,
// because the daemon restart is what released the call. And each time the
// remedy was spent on a bring-up that was going to succeed - restarting the
// daemon underneath it, dropping the RF link, leaving the air unit blinking,
// or in the last case simply throwing the process away and starting again.
//
// Set against that, there is still no confirmed case of a bring-up that would
// never have completed on its own. A containment that has repeatedly caused
// the fault it was built to catch, and has never been shown to fix one, should
// not run by default. What stays is the measurement: every bring-up slower
// than two seconds logs its duration, which is how the next deadline - if
// there is ever evidence for one - should be picked.
//
// Arm it with --bbwatchdog=N or bb_watchdog: N in kestrel-gnd.yaml. The
// KESTREL_BB_WATCHDOG_S environment variable is still read, but only reaches
// here if nothing else supplied a value, which main() always does.
// ...and then the evidence turned over again. With the intervention off, a
// bring-up parked at `subscribe event 12` and was still parked 150s later; the
// patched daemon in the same state logged ZERO retries, so the request never
// reached its socket state machine at all. The parks are real, they are not in
// the daemon's socket bookkeeping, and the re-exec is what has been getting
// past them all along. The 28 re-execs were not false positives - they were 28
// genuine parks across a long session, each one cleared by starting over.
//
// So it is armed, at 10s. What was actually wrong was the conclusion, not the
// deadline. Kept above in full because two of those three readings were mine
// and both were confidently argued.
// OFF by default. Giving every bb_ioctl a deadline (ar_ioctl in
// ar8030_source.cpp) removed the park this existed to contain - the one that
// fired several times a second through BB_GET_STATUS polling - and with this
// disabled the app now connects, links and streams where it used to sit with
// both sockets open and a frozen log.
//
// It stays in the tree because bb_socket_open() still has no timeout, and no
// API to give it one, so a lost reply there is still unrecoverable; likewise
// the deinit paths. Arm it if that symptom appears: a goggle that hangs and
// only a power cycle clears.
//
// bb_watchdog: 1 in kestrel-gnd.yaml, or --bbwatchdog=1, selects the deadline
// below. The run loop goes round about twice a second, so twenty seconds of
// silence is not a slow pass, it is a parked thread - and it is deliberately
// far above the ~3s a healthy bring-up takes, because every deadline chosen
// close to a measured value turned out to sit below some legitimate case.
constexpr int kOpenDeadlineOff  = 0;
constexpr int kOpenDeadlineArmed = 20;
constexpr int kOpenDeadlineDefault = kOpenDeadlineOff;
constexpr int kMaxAttempts   = 4;

// Attempts are counted across the life of the process and reset by a
// successful open, so a wedge an hour into a flight gets its own budget.
std::atomic<int> g_attempts{0};

// Every descriptor above stderr has to go before exec. The DRM fd carries
// master, and exec does not close it - the new image would open the card,
// fail to become master, and come up with no display at all, which looks
// nothing like the baseband problem we were recovering from.
void close_inherited_fds() {
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        return;
    }
    int dfd = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd > 2 && fd != dfd) close(fd);
    }
    closedir(d);
}

// --- Rung 0: force-close every daemon socket, without touching the SDK ----
//
// The wedge is a bb_socket_open() reply that never comes, waited for on an
// untimed condition variable with the SDK's mutex held. So no SDK call from
// ANY thread can help - it would just queue behind that mutex. But the daemon
// is a plain TCP RPC server on 127.0.0.1:50000, speaking the same usbpack
// framing tools/bbpair.py speaks, and this thread can open its OWN connection
// to it. BB_FORCE_CLS_SOCKET_ALL (BB_REQUEST(SET, 20), no payload) tells the
// daemon to tear down every socket on the device - including the half-opened
// node the parked call is waiting on - which is what lets that call return.
//
// The per-socket BB_FORCE_CLS_SOCKET (SET 31) does not clear a stuck state;
// the ar8030-transport project found the _ALL form does, given a short retry
// (their recipe: 5 attempts, 500ms apart), and this rung copies it. It costs
// a few seconds, drops no process and no RF link, so it runs before the
// re-exec. If the run loop has not come back round within the grace period,
// the ladder below proceeds exactly as it always did. Whether it worked is
// logged either way - that is the evidence the next tuning should come from.
// Disable with KESTREL_BB_FORCE_CLS=0.
constexpr uint32_t kBbForceClsSocketAll = (2u << 24) | 20u;  // BB_REQUEST(BB_REQ_SET, 20)
constexpr int kForceClsTries  = 5;
constexpr int kForceClsGapMs  = 500;
constexpr int kForceClsGraceS = 4;

// One request/reply on a fresh connection. Returns the daemon's status word,
// or -1 if the daemon could not be reached or did not answer in time.
int daemon_rpc_raw(uint32_t reqid, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(50000);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }

    // usbpack frame: AA | plen LE32 | reqid BE32 | msgid BE32 | sta BE32 |
    // ~xor(17 header bytes) | payload | BB. No payload here, so 19 bytes.
    uint8_t f[19];
    memset(f, 0, sizeof f);
    f[0] = 0xAA;
    f[5] = (uint8_t)(reqid >> 24); f[6] = (uint8_t)(reqid >> 16);
    f[7] = (uint8_t)(reqid >> 8);  f[8] = (uint8_t)reqid;
    uint8_t x = 0;
    for (int i = 0; i < 17; i++) x ^= f[i];
    f[17] = (uint8_t)~x;
    f[18] = 0xBB;
    if (send(fd, f, sizeof f, 0) != (ssize_t)sizeof f) { close(fd); return -1; }

    uint8_t r[64];
    size_t n = 0;
    while (n < sizeof r) {
        ssize_t k = recv(fd, r + n, sizeof r - n, 0);
        if (k <= 0) break;
        n += (size_t)k;
        if (n >= 18 && r[n - 1] == 0xBB) break;
    }
    close(fd);
    if (n < 18 || r[0] != 0xAA) return -1;
    return (int32_t)(((uint32_t)r[13] << 24) | ((uint32_t)r[14] << 16) |
                     ((uint32_t)r[15] << 8)  |  (uint32_t)r[16]);
}

// True if the parked run loop came back round after the force-close.
bool try_force_close_all() {
    if (const char *e = getenv("KESTREL_BB_FORCE_CLS"))
        if (atoi(e) == 0) return false;
    const uint64_t stuck_at = g_alive.load();
    fprintf(stderr, "[bb-watchdog] rung 0: BB_FORCE_CLS_SOCKET_ALL over a fresh daemon "
                    "connection (%d tries, %dms apart)\n", kForceClsTries, kForceClsGapMs);
    for (int i = 0; i < kForceClsTries && !g_stop.load(); i++) {
        int sta = daemon_rpc_raw(kBbForceClsSocketAll, 1000);
        fprintf(stderr, "[bb-watchdog]   try %d: %s (sta=%d)\n", i + 1,
                sta < 0 ? "no reply" : "acked", sta);
        usleep(kForceClsGapMs * 1000);
        if (g_alive.load() > stuck_at) break;   // the loop moved; stop hammering
    }
    // Grace for the released call to return and the loop to stamp g_alive.
    for (int i = 0; i < kForceClsGraceS && !g_stop.load(); i++) {
        if (g_alive.load() > stuck_at) {
            fprintf(stderr, "[bb-watchdog] rung 0 cleared the park - run loop is going "
                            "round again; no re-exec, link kept\n");
            return true;
        }
        sleep(1);
    }
    fprintf(stderr, "[bb-watchdog] rung 0 did not clear it - escalating\n");
    return false;
}

// True while bb_socket_open() has been in flight longer than the deadline.
// -1 until resolved. --bbwatchdog wins over the environment variable, which
// wins over the built-in default.
int g_deadline_override = -1;

int deadline_s() {
    static int v = -1;
    if (v < 0) {
        if (g_deadline_override >= 0) {
            v = g_deadline_override;
        } else if (const char *e = getenv("KESTREL_BB_WATCHDOG_S")) {
            v = atoi(e);
        } else {
            v = kOpenDeadlineDefault;
        }
        if (v == 1) v = kOpenDeadlineArmed;   // "on", without naming a number
        if (v < 0)  v = 0;
    }
    return v;
}

bool wedged() {
    int d = deadline_s();
    if (d <= 0) return false;                 // observing only
    uint64_t last = g_alive.load();
    return last != 0 && now_s() - last >= (uint64_t)d;
}

void *watchdog_main(void *) {
    if (const char *s = getenv("KESTREL_BB_ATTEMPT")) g_attempts.store(atoi(s));
    if (deadline_s() <= 0) {
        fprintf(stderr, "[bb-watchdog] disabled; arm with --bbwatchdog=1 "
                        "or bb_watchdog: 1 in kestrel-gnd.yaml\n");
        return nullptr;
    }

    // This watches for as long as the process runs. An earlier version handled
    // one event and returned, so a wedge that happened later - including one
    // its own daemon restart had caused - had nobody looking for it, and the
    // process sat in a call that was never coming back with the watchdog
    // already gone home.
    for (;;) {
    int attempt = g_attempts.load();

    // Nothing here has a deadline of its own. The source may retry a failing
    // connect for as long as it likes - that path recovers by itself - and at
    // boot S60ar8030 backgrounds its own bring-up, so a slow start is normal.
    // We are waiting for one specific thing: a call that went in and did not
    // come out.
    while (!g_stop.load() && !wedged()) sleep(1);
    if (g_stop.load()) return nullptr;

    if (attempt >= kMaxAttempts) {
        fprintf(stderr, "[bb-watchdog] video socket still not open after %d restarts - "
                        "leaving the process for the supervisor\n", attempt);
        return nullptr;
    }

    // Lightest remedy first, and only on the first wedge: ask the daemon to
    // force-close every socket (see try_force_close_all above). If the parked
    // call returns and the loop goes round again, nothing was spent - no
    // re-exec, no daemon restart, the RF link never dropped.
    if (attempt == 0 && try_force_close_all()) {
        continue;
    }
    g_attempts.store(attempt + 1);
    fprintf(stderr, "[bb-watchdog] bring-up parked %ds - re-exec so the daemon frees "
                    "the socket (attempt %d/%d)\n",
            deadline_s(), attempt + 1, kMaxAttempts);

    // Do NOT restart the daemon first. Reading its source settles what the
    // stuck socket actually needs: when the client's RPC connection closes,
    // sock_node_rpc.c sets the node to sock_need_send_close, and
    // sock_dev_need_write frees any node that never finished opening
    // (`if (!psock->has_opened) bn_set_free`). Dropping our connection is the
    // whole remedy - and exiting drops it.
    //
    // So the re-exec IS the fix, and restarting the daemon was a heavier
    // remedy for a problem that did not need one: it tears down the radio for
    // every client, costs a settle, and drops an RF link that was not at
    // fault. It is kept below only as a fallback for the case where starting
    // over does not help, which would mean the fault is in the chip rather
    // than in the daemon's bookkeeping.
    if (attempt >= 1) {
        int settle = 10 * attempt;
        fprintf(stderr, "[bb-watchdog] re-exec did not clear it - restarting the daemon "
                        "and settling %ds\n", settle);
        if (system("/etc/init.d/S60ar8030 restart >/dev/null 2>&1") != 0)
            fprintf(stderr, "[bb-watchdog] S60ar8030 restart returned non-zero\n");
        for (int i = 0; i < settle && !g_stop.load(); i++) sleep(1);
        if (g_stop.load()) return nullptr;
    }

    // Last check before throwing the process away: did the bring-up finish
    // while we were deciding? A parked call can come back on its own - the
    // reply that was late arrives, or the daemon restart above releases it -
    // and re-execing then discards a link that is already up. Seen doing
    // exactly that: "video socket open" followed 55 lines later by "re-exec".
    //
    // The test is a SUCCESSFUL bring-up, not merely a call that returned. One
    // that returned an error leaves nothing worth keeping, and the source's
    // own retry loop owns that case.
    if (g_socket_open.load()) {
        fprintf(stderr, "[bb-watchdog] bring-up completed while deciding - "
                        "keeping it, still watching\n");
        g_attempts.store(attempt);   // that attempt cost nothing; do not spend it
        continue;
    }

    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "%d", g_attempts.load());
    setenv("KESTREL_BB_ATTEMPT", nbuf, 1);

    fprintf(stderr, "[bb-watchdog] re-exec\n");
    fflush(nullptr);
    close_inherited_fds();
    execv("/proc/self/exe", g_argv);

    // Only reached if exec itself failed, which means the image is gone or
    // unreadable - there is nothing left to try.
    fprintf(stderr, "[bb-watchdog] execv failed: %s\n", strerror(errno));
    _exit(90);
    }   // for (;;)
    return nullptr;
}

}  // namespace

void bb_watchdog_set_deadline(int seconds) { g_deadline_override = seconds; }
bool bb_watchdog_deadline_set() { return g_deadline_override >= 0; }

void bb_watchdog_start(char **argv) {
    g_argv = argv;
    pthread_t t;
    if (pthread_create(&t, nullptr, watchdog_main, nullptr) == 0)
        pthread_detach(t);
}

void bb_watchdog_open_begin() {
    // Cleared per bring-up: the flag answers "did THIS one get through", and a
    // value left over from a previous connect would tell the watchdog a park
    // had succeeded.
    g_socket_open.store(false);
    g_alive.store(now_s());
}

void bb_watchdog_alive() { g_alive.store(now_s()); }

void bb_watchdog_open_end() {
    // Report how long a slow bring-up took, so the deadline stays chosen from
    // measurement rather than from memory of one bad afternoon.
    uint64_t since = g_alive.exchange(now_s());
    if (since) {
        uint64_t took = now_s() - since;
        // Say whether we let go on our own or were let go, so this can never
        // again be read as evidence about how long a healthy call takes.
        if (took >= 2)
            fprintf(stderr, "[bb-watchdog] bring-up returned after %llus%s\n",
                    (unsigned long long)took,
                    g_attempts.load() ? " (following our daemon restart)" : "");
    }
}

void bb_watchdog_socket_open() {
    g_attempts.store(0);
    if (!g_socket_open.exchange(true)) {
        // A successful bring-up clears the attempt count, so a wedge months of
        // uptime later starts its own budget rather than inheriting a spent one.
        unsetenv("KESTREL_BB_ATTEMPT");
    }
}

void bb_watchdog_stop() { g_stop.store(true); }
