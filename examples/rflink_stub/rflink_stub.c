/*
 * rflink_stub.c - the smallest possible fpvOS RF link plugin.
 *
 * SPDX-License-Identifier: MIT
 *
 * It claims no hardware (probe() only says yes when FPVOS_RFLINK_STUB=1 is
 * set), delivers no video, and reports a link that goes "up" the moment you
 * connect. Its only purpose is to show every required entry point compiling
 * and loading against include/fpvos/rf_link.h, so a manufacturer can start
 * from a known-good skeleton. Build with the CMakeLists.txt beside it; the
 * result is librflink_stub.so.
 */
#define _POSIX_C_SOURCE 200809L
#include <fpvos/rf_link.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The opaque instance the header forward-declares. Whatever you need per
 * link goes in here; the host never looks inside. */
struct rf_link {
    rf_link_state state;
    rf_event_fn   ev;
    void         *ev_user;
};

/* Static description handed back from open(). Advertise caps for what you
 * actually implement - the stub implements nothing optional, so 0. */
static const rf_link_info k_info = {
    .size         = sizeof(rf_link_info),
    .abi_version  = RF_LINK_ABI_VERSION,
    .name         = "stub",
    .display_name = "fpvOS stub link (no hardware)",
    .vendor       = "fpvOS",
    .version      = "1.0",
    .caps         = 0,
    .codec        = RF_CODEC_H265,
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* --- lifecycle ---------------------------------------------------------- */

/* A real plugin checks for its radio here (a USB/SDIO id, a device node).
 * The stub must never hijack a real goggle, so it only answers yes on
 * request. Keep this fast and side-effect free: it runs for every
 * installed plugin at boot. */
static int stub_probe(void)
{
    const char *e = getenv("FPVOS_RFLINK_STUB");
    return e && e[0] == '1';
}

static rf_link *stub_open(const rf_link_info **info)
{
    rf_link *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->state = RF_STATE_IDLE;
    if (info) *info = &k_info;
    return l;
}

static void stub_close(rf_link *l)
{
    free(l);
}

static void stub_set_event_fn(rf_link *l, rf_event_fn fn, void *user)
{
    l->ev = fn;
    l->ev_user = user;
}

/* --- link control ------------------------------------------------------- */

static int stub_connect(rf_link *l, const rf_config *cfg, uint32_t timeout_ms)
{
    (void)cfg; (void)timeout_ms;          /* a real plugin associates here */
    l->state = RF_STATE_LINKED;
    if (l->ev) l->ev(l->ev_user, RF_EVENT_LINK_UP, "stub");
    return RF_OK;
}

static void stub_disconnect(rf_link *l)
{
    if (l->state == RF_STATE_LINKED && l->ev)
        l->ev(l->ev_user, RF_EVENT_LINK_DOWN, "stub");
    l->state = RF_STATE_IDLE;
}

/* Fill every field, using the documented "unknown" sentinels for what you
 * cannot measure. The host draws its gauges straight from this. */
static int stub_get_stats(rf_link *l, rf_stats *o)
{
    if (!o || o->size < sizeof(rf_stats)) return RF_ERR_INVALID;
    memset(o, 0, sizeof *o);
    o->size            = sizeof *o;
    o->state           = l->state;
    o->rssi_dbm        = INT32_MIN;
    o->snr_db          = NAN;
    o->rx_mcs          = -1;
    o->tx_mcs          = -1;
    o->fec_error_ratio = NAN;
    o->gain_cur        = -1;
    o->gain_max        = -1;
    o->link_quality    = NAN;
    o->distance_m      = NAN;
    o->air_temp_c      = NAN;
    o->ground_temp_c   = NAN;
    return RF_OK;
}

/* --- video -------------------------------------------------------------- */

/* A real plugin blocks on its link socket for at most timeout_ms and copies
 * whatever arrived. The stub has nothing to deliver, so it just honours the
 * timeout and returns 0 - which is exactly what "no data yet" looks like. */
static int stub_read_video(rf_link *l, uint8_t *buf, size_t cap,
                           uint32_t timeout_ms, rf_video_meta *meta)
{
    (void)l; (void)buf; (void)cap;
    uint32_t w = timeout_ms < 50 ? timeout_ms : 50;
    struct timespec ts = { (time_t)(w / 1000), (long)(w % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    if (meta && meta->size >= sizeof(rf_video_meta))
        meta->recv_ts_us = now_us();
    return 0;
}

/* --- the table ---------------------------------------------------------- */

/* Optional ops are left NULL; the host checks before calling. Add them as
 * you implement them and raise the matching RF_CAP_* bit in k_info. */
static const rf_link_ops k_ops = {
    .size         = sizeof(rf_link_ops),
    .abi_version  = RF_LINK_ABI_VERSION,
    .probe        = stub_probe,
    .open         = stub_open,
    .close        = stub_close,
    .set_event_fn = stub_set_event_fn,
    .connect      = stub_connect,
    .disconnect   = stub_disconnect,
    .get_stats    = stub_get_stats,
    .read_video   = stub_read_video,
};

/* The one exported symbol. Refuse a host whose ABI MAJOR differs from the
 * one this plugin was built against. RF_LINK_EXPORT keeps it visible under
 * -fvisibility=hidden, which the CMakeLists uses so nothing else leaks. */
RF_LINK_EXPORT
const rf_link_ops *fpvos_rflink_entry(uint32_t host_abi_version)
{
    if ((host_abi_version >> 16) != RF_LINK_ABI_MAJOR) return NULL;
    return &k_ops;
}
