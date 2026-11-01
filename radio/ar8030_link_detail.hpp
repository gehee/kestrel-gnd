#pragma once
// What the files of the AR8030 link (Ar8030Link, ar8030_link.hpp) share and
// nothing else includes: the vendor SDK's client header and ar_libre's weak
// calls, the ioctl helper and its deadlines, the clocks.
//
// The link is one class, its methods in a file per concern:
//   ar8030_link.cpp        construction, radio::Link (commands, held), the loop
//   ar8030_link_bb.cpp     the baseband session: bring-up, its events and sockets
//   ar8030_link_bind.cpp   binding, and which air unit is paired
//   ar8030_link_air.cpp    the air unit: its handshake, settings, status, camera
//   ar8030_link_rf.cpp     the RF: channel, power, bandwidth, ranging, AUTO
//   ar8030_link_stats.cpp  the link's figures, and the air delay by the radio clock
//   ar8030_link_video.cpp  the video: the stream split into slices, the replay

#include "radio/ar8030_link.hpp"

#include <algorithm>
#include "base/sd_log.hpp"
#include "radio/settings.hpp"
#include "base/video_pool.hpp"
#include "base/prof.hpp"
#include "base/frame_mode.hpp"
#include "base/ltrace.hpp"
#include "radio/bb_watchdog.hpp"
#include "radio/ar8030_handshake.h"
#include "base/settings.hpp"
#include "region/zones.hpp"
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#include <set>
#include "base/scheduling_helper.hpp"

extern "C" {
#include "radio/bb_client.h"

// ar_libre's own client-library calls (Gee/ar_libre), which Artosyn's library
// does not have. Weak, so one binary runs on either: on Artosyn's they are
// NULL, and everything below behaves as it always has.
extern "C" const char* arlink_version(void) __attribute__((weak));
extern "C" uint64_t arlink_socket_rx_ns(int sockfd) __attribute__((weak));
extern "C" int arlink_socket_read_now(int sockfd, void *buf, uint32_t len) __attribute__((weak));
extern "C" int arlink_socket_poll_fd(int sockfd) __attribute__((weak));
extern "C" int arlink_request(uint32_t request_code, const void *in, uint32_t inlen, void *out,
                              uint32_t outcap, uint32_t *got, int timeout_ms) __attribute__((weak));
static inline bool on_ar_libre() { return arlink_version != nullptr; }

// A socket's own file to poll(), where ar_libre has one (-1: none, or
// Artosyn's library).
static inline int poll_fd(int sfd) {
    return sfd >= 0 && arlink_socket_read_now && arlink_socket_poll_fd ? arlink_socket_poll_fd(sfd) : -1;
}

// What a socket has queued, without waiting where ar_libre allows it; else
// bb_socket_read's shortest wait, 1 ms. <= 0: nothing.
static inline int read_queued(int sfd, void *buf, uint32_t len) {
    if (poll_fd(sfd) >= 0) return arlink_socket_read_now(sfd, buf, len);
    return bb_socket_read(sfd, buf, len, 1);
}

}

// radio/bb_sdk_shim.cpp - overrides the linked SDK's ioctl length table with
// the newer one extracted from stock ar_ldy_gnd.
extern "C" void bb_sdk_shim_report();

// Read chunk size. The baseband hands us H.265 in bursts; 256K keeps the
// syscall rate low without adding meaningful latency (a 1080p60 IDR at
// 40Mbit/s is ~80K, so a big frame still lands in one or two reads).
#define AR_READ_CHUNK (256 * 1024)

// Guard against a runaway accumulator if we never see a second start code
// (corrupt stream / wrong port). 4MB is far above any sane single NAL.
// The radio's names (radio/radio.hpp): CAM_*, RF_*, BIND_*.

#define AR_MAX_ACCUM (4 * 1024 * 1024)

static inline uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static inline uint64_t now_ms() { return now_us() / 1000ULL; }

// Every bb_ioctl() in this SDK waits forever. The public entry point hard-codes
// timeout = -1 (ar8030.c:253), and -1 selects the untimed pthread_cond_wait in
// bs_send_usbpack_and_wait() rather than the timed one beside it. So a single
// lost reply from the daemon parks the calling thread for the life of the
// process - and this loop polls status several times a second, which is why
// the failure looks like "it connected and then stopped" rather than a failure
// to connect. Caught in the act as AR8030_RX sitting in futex_wait_queue_me
// with both sockets open and the log frozen.
//
// bb_ioctl_ex() is the same call with a caller-supplied deadline, exported by
// the same vendor library we already link, so this needs no patched SDK. A
// timeout comes back as a plain failure, which every caller here already
// handles - and a call that fails is recoverable where one that never returns
// is not.
static const int kIoctlTimeoutMs = 1000;
// Bring-up requests get longer than an ordinary ioctl: the daemon has real
// work to do behind BB_INIT_REQ, and a deadline that expires while it is
// simply busy would turn a slow start into a failed one.
static const int kBringupReqTimeoutMs = 3000;
// Consecutive failed bring-ups before we stop asking nicely and restart the
// daemon. Two is roughly 15s of trying, which is long enough to ride out a
// daemon that is merely slow to start and short enough that a stuck one is
// cleared before the pilot gives up on it.
static const int kFailsBeforeDaemonRestart = 2;

// One request at a time (ar_ioctl): the stats thread and the radio's both
// talk to the baseband. A bb_ioctl_ex with kIoctlTimeoutMs, logged when it
// fails (ar8030_link_bb.cpp).
int ar_ioctl(bb_dev_handle_t *dev, uint32_t request, const void *in, void *out);

// Parse the paired air-unit MACs out of /factory/user_cfg.json. The file is a
// flat object holding "bb_mac_addr_N": [ "0xE5", "0xDA", "0x00", "0x08" ], the
// same values the stock firmware logged as candi_mac[N].
int ar_load_candidate_macs(bb_mac_t *out, int max);   // ar8030_link_bind.cpp

// What to tell the air unit. Stock's "+1 means auto" encoding really is what
// goes on the wire: a bbproxy capture of stock caught cmd 0x22 carrying 101 for
// its "100mW Auto" menu entry. We used to strip the +1, which asked the air for
// a fixed level and silently disabled its adaptation. See
// references/sky-commands.md.
static inline int sky_pwr_mw(const ar_pwr_level &lv) {
    return lv.mw;                 // kArPwrLevels already stores AUTO as N+1
}

static inline float pct_ms(std::vector<uint32_t> &v, int pct) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, v.size() * pct / 100)] / 1000.0f;
}
