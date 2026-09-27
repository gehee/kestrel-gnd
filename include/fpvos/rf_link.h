/*
 * fpvOS RF link plugin interface
 *
 * SPDX-License-Identifier: MIT
 *
 * This header is the contract between fpvOS (the host: kestrel) and an RF
 * link plugin (the vendor's radio + air-unit stack). It is deliberately
 * licensed MIT so a manufacturer can include it in a closed-source plugin
 * without taking on any obligation from fpvOS's own GPLv3 code.
 *
 * A plugin is a shared object, e.g. librflink_<vendor>.so, installed under
 * /usr/lib/fpvos/rflink/. It exports exactly one symbol:
 *
 *     const rf_link_ops *fpvos_rflink_entry(uint32_t host_abi_version);
 *
 * The host dlopen()s every plugin in that directory, calls the entry point,
 * checks the ABI version, and asks each plugin whether its hardware is
 * present (probe). The first plugin that claims the hardware - or the one
 * named in the host's configuration - becomes the link for this boot.
 *
 * Design rules, in priority order:
 *
 *   1. NOTHING BLOCKS FOREVER. Every call that can wait takes a timeout_ms.
 *      The host runs the link on one thread and must always be able to stop,
 *      reconfigure and shut down; a plugin that parks that thread makes the
 *      whole goggle unkillable. Return RF_ERR_TIMEOUT and let the host decide.
 *
 *   2. Plain C, stable layout. No C++ types across the boundary. Every struct
 *      begins with a `size` field the caller sets to sizeof(struct); a newer
 *      host or plugin can then add fields at the END of a struct without
 *      breaking the other side, which only reads what it knows.
 *
 *   3. The host owns the video pipeline. The plugin hands over the encoded
 *      bitstream exactly as the link delivers it; splitting NAL units,
 *      assembling pictures, decoding, recording and drawing are the host's
 *      job. A plugin has nothing to do with pixels.
 *
 *   4. Advertise, don't assume. Say what you support in rf_link_info.caps;
 *      the host shows menus and gauges only for what is really there.
 */
#ifndef FPVOS_RF_LINK_H
#define FPVOS_RF_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump the MAJOR when a change would break an existing plugin. Additive
 * changes (new caps, new fields at the end of a struct, new optional ops at
 * the end of rf_link_ops) bump the MINOR only. */
#define RF_LINK_ABI_MAJOR 1
#define RF_LINK_ABI_MINOR 0
#define RF_LINK_ABI_VERSION ((RF_LINK_ABI_MAJOR << 16) | RF_LINK_ABI_MINOR)

/* The one symbol a plugin must export. */
#define RF_LINK_ENTRY_SYMBOL "fpvos_rflink_entry"

/* Put this on your fpvos_rflink_entry() definition. Plugins are normally
 * built with -fvisibility=hidden so nothing but the entry point leaks out of
 * the .so; this makes the entry point visible again so dlsym() can find it. */
#if defined(__GNUC__) || defined(__clang__)
#  define RF_LINK_EXPORT __attribute__((visibility("default")))
#else
#  define RF_LINK_EXPORT
#endif

/* Where the host looks for plugins. */
#define RF_LINK_PLUGIN_DIR "/usr/lib/fpvos/rflink"

/* ------------------------------------------------------------------------
 * Return codes. 0 is success; negatives are errors.
 * ---------------------------------------------------------------------- */
enum {
    RF_OK              =  0,
    RF_ERR_GENERIC     = -1,
    RF_ERR_TIMEOUT     = -2,   /* the operation did not complete in timeout_ms */
    RF_ERR_UNSUPPORTED = -3,   /* this plugin does not implement the request */
    RF_ERR_INVALID     = -4,   /* bad argument (value out of range, etc.)     */
    RF_ERR_NOT_LINKED  = -5,   /* needs an associated link and there is none  */
    RF_ERR_BUSY        = -6,
    RF_ERR_NO_DEVICE   = -7,   /* the radio hardware is not present/ready     */
};

/* Opaque per-plugin instance, allocated by the plugin in open(). */
typedef struct rf_link rf_link;

/* ------------------------------------------------------------------------
 * Capabilities. The host reads these from rf_link_info.caps and never calls
 * an op whose capability is not advertised (calling one anyway must return
 * RF_ERR_UNSUPPORTED, never crash).
 * ---------------------------------------------------------------------- */
enum {
    RF_CAP_SET_CHANNEL    = 1u << 0,  /* set_channel()                         */
    RF_CAP_SET_BANDWIDTH  = 1u << 1,  /* set_bandwidth()                       */
    RF_CAP_SET_TX_POWER   = 1u << 2,  /* set_tx_power()                        */
    RF_CAP_CHANNEL_HOP    = 1u << 3,  /* rf_config.channel_hop is honoured     */
    RF_CAP_PAIR           = 1u << 4,  /* pair()                                */
    RF_CAP_MSP_CHANNEL    = 1u << 5,  /* RF_CHAN_MSP carries the FC's MSP feed */
    RF_CAP_CONTROL_CHANNEL= 1u << 6,  /* RF_CHAN_CONTROL is readable/writable  */
    RF_CAP_DISTANCE       = 1u << 7,  /* rf_stats.distance_m is real           */
    RF_CAP_SPECTRUM_SCAN  = 1u << 8,  /* scan_channels()                       */
    RF_CAP_AIR_SETTINGS   = 1u << 9,  /* set/get_air_setting()                 */
    RF_CAP_VIDEO_MODE     = 1u << 10, /* set_video_mode()                      */
    RF_CAP_CAPTURE_TS     = 1u << 11, /* rf_video_meta.capture_ts_us is real   */
    RF_CAP_AIR_TEMP       = 1u << 12, /* rf_stats.air_temp_c is real           */
};

/* ------------------------------------------------------------------------
 * Descriptions
 * ---------------------------------------------------------------------- */
typedef enum {
    RF_CODEC_UNKNOWN = 0,
    RF_CODEC_H264    = 1,
    RF_CODEC_H265    = 2,
} rf_codec;

/* Static description of the plugin, returned by open(). Strings are owned by
 * the plugin and must stay valid until close(). */
typedef struct {
    uint32_t    size;          /* = sizeof(rf_link_info)                     */
    uint32_t    abi_version;   /* RF_LINK_ABI_VERSION the plugin was built to*/
    const char *name;          /* short id, e.g. "ar8030" - used in config    */
    const char *display_name;  /* e.g. "Artosyn AR8030 (Caddx Ascent)"       */
    const char *vendor;
    const char *version;       /* plugin's own version string                */
    uint32_t    caps;          /* RF_CAP_* bitmask                           */
    rf_codec    codec;         /* what the video channel carries             */
} rf_link_info;

/* ------------------------------------------------------------------------
 * Link configuration, passed to connect().
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t    size;          /* = sizeof(rf_config)                        */
    uint32_t    freq_khz;      /* 0 = let the radio choose / scan            */
    uint32_t    bandwidth_khz; /* 0 = plugin default                          */
    uint32_t    tx_power_mw;   /* 0 = plugin default                          */
    uint8_t     channel_hop;   /* 1 = allow the link to hop / auto-select     */
    uint8_t     pair_mode;     /* 1 = come up in pairing mode                 */
    uint8_t     reserved[2];
    /* Plugin-private configuration file (channel tables, candidate MACs,
     * baseband config...). Its format is the plugin's business; the host only
     * passes the path through from its own settings. May be NULL. */
    const char *config_path;
} rf_config;

/* ------------------------------------------------------------------------
 * Link telemetry, filled by get_stats(). Everything the goggle's HUD draws
 * about the link comes from here. Unknown values: set the *_valid style
 * sentinels noted per field, and do not advertise the matching RF_CAP.
 * ---------------------------------------------------------------------- */
typedef enum {
    RF_STATE_IDLE       = 0,   /* no peer                                    */
    RF_STATE_CONNECTING = 1,   /* associating / acquiring                    */
    RF_STATE_LINKED     = 2,   /* associated, data flowing                   */
} rf_link_state;

typedef struct {
    uint32_t      size;            /* = sizeof(rf_stats)                     */
    rf_link_state state;
    int32_t       rssi_dbm;        /* INT32_MIN if unknown                   */
    float         snr_db;          /* NAN if unknown                         */
    int32_t       rx_mcs;          /* -1 if unknown                          */
    int32_t       tx_mcs;          /* -1 if unknown                          */
    uint32_t      rx_kbps;         /* video/data throughput received         */
    uint32_t      tx_kbps;
    uint32_t      tx_freq_khz;     /* ground TX / air RX frequency, 0 unknown*/
    uint32_t      rx_freq_khz;     /* ground RX / air TX frequency, 0 unknown*/
    uint32_t      bandwidth_khz;   /* channel bandwidth, 0 unknown. Always kHz,
                                      never an index: 2.5 MHz is 2500.       */
    uint32_t      tx_power_mw;     /* the ground radio's own uplink power    */
    uint32_t      air_tx_power_mw; /* the AIR unit's video TX power - what a
                                      pilot means by "VTX power". 0 unknown  */
    uint8_t       power_auto;      /* 1 = power adaptation active            */
    uint8_t       reserved[3];
    float         fec_error_ratio; /* pre-FEC codeword error ratio 0..1,
                                      NAN if unknown                         */
    int32_t       gain_cur;        /* receive gain step, -1 unknown          */
    int32_t       gain_max;
    float         link_quality;    /* plugin's own 0..1 composite, NAN if
                                      unknown; the host may derive its own   */
    float         distance_m;      /* time-of-flight range, NAN if unknown   */
    float         air_temp_c;      /* air unit temperature, NAN if unknown   */
    float         ground_temp_c;   /* this radio's temperature, NAN if unknown*/
    /* Air-unit clock, if the link exposes one, for end-to-end delay
     * estimation against rf_video_meta.capture_ts_us. 0 if unknown. */
    uint64_t      air_clock_us;
    uint64_t      air_clock_local_us; /* host monotonic time of that sample   */
} rf_stats;

/* One channel of a spectrum scan (scan_channels()). */
typedef struct {
    uint32_t freq_khz;
    int32_t  noise_dbm;        /* measured noise/energy on that channel      */
    uint8_t  in_use;           /* 1 if this is the current link channel      */
    uint8_t  reserved[3];
} rf_scan_entry;

/* ------------------------------------------------------------------------
 * Video delivery. read_video() copies the next chunk of the encoded bitstream
 * into the caller's buffer, exactly as the link delivered it: any chunking,
 * NAL units may straddle calls. The host reassembles. If the link knows more
 * about the chunk, say so in rf_video_meta; otherwise leave fields at their
 * "unknown" value.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t size;             /* = sizeof(rf_video_meta)                    */
    uint64_t recv_ts_us;       /* host monotonic time the bytes arrived; the
                                  plugin fills this from CLOCK_MONOTONIC     */
    uint64_t capture_ts_us;    /* air-side capture timestamp, in the air
                                  unit's clock (see rf_stats.air_clock_us).
                                  0 if unknown / RF_CAP_CAPTURE_TS unset     */
    uint32_t frame_seq;        /* link-level frame counter for loss detection,
                                  0 if unknown                               */
    uint8_t  keyframe_hint;    /* 1 if the link flags this as a keyframe     */
    uint8_t  frame_start;      /* 1 if this chunk begins a new picture       */
    uint8_t  reserved[2];
} rf_video_meta;

/* ------------------------------------------------------------------------
 * Data channels: byte pipes beside the video. Semantics are fixed by id so
 * the host knows what to do with the bytes; framing inside them is whatever
 * the link carries.
 * ---------------------------------------------------------------------- */
typedef enum {
    RF_CHAN_TELEMETRY = 1,     /* link/air status, plugin-private format     */
    RF_CHAN_MSP       = 2,     /* the flight controller's MSP stream, byte-
                                  exact, for the host's Betaflight OSD       */
    RF_CHAN_CONTROL   = 3,     /* host<->air control, plugin-private format  */
} rf_channel_id;

/* ------------------------------------------------------------------------
 * Air-unit settings (RF_CAP_AIR_SETTINGS). Typed knobs the host can put in
 * its menu without knowing the wire protocol. A plugin implements the subset
 * it supports and returns RF_ERR_UNSUPPORTED for the rest; the host probes
 * once at start-up with get_air_setting() to learn which rows to show.
 *
 * Values are plain integers in the unit noted. Enumerated settings use the
 * plugin's own ordinal; the host shows them as "<name> N" unless the plugin
 * provides labels via air_setting_label().
 * ---------------------------------------------------------------------- */
typedef enum {
    RF_AIR_EV          = 1,    /* exposure compensation, plugin range        */
    RF_AIR_SATURATION  = 2,
    RF_AIR_CONTRAST    = 3,
    RF_AIR_SHARPNESS   = 4,
    RF_AIR_SCENE       = 5,    /* enumerated                                 */
    RF_AIR_FOCUS       = 6,    /* enumerated                                 */
    RF_AIR_WHITE_BAL   = 7,    /* enumerated                                 */
    RF_AIR_ROTATION    = 8,    /* degrees: 0 / 90 / 180 / 270                */
    RF_AIR_DNR3D       = 9,    /* 0/1                                        */
    RF_AIR_STANDBY     = 10,   /* 0/1 - air unit low-RF-power / sensor off   */
    RF_AIR_BANDWIDTH   = 11,   /* kHz                                        */
    RF_AIR_TX_POWER    = 12,   /* mW                                         */
} rf_air_setting;

/* Video mode request (RF_CAP_VIDEO_MODE). */
typedef struct {
    uint32_t size;             /* = sizeof(rf_video_mode)                    */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
} rf_video_mode;

/* ------------------------------------------------------------------------
 * Events. The plugin may call the callback from ANY thread, including its
 * own internal ones; the host's callback is cheap and thread-safe. Never
 * call back into the plugin from inside the callback.
 * ---------------------------------------------------------------------- */
typedef enum {
    RF_EVENT_LINK_UP        = 1,
    RF_EVENT_LINK_DOWN      = 2,
    RF_EVENT_PAIRED         = 3,   /* detail: peer identifier string         */
    RF_EVENT_AIR_STATUS     = 4,   /* something in the air unit changed;
                                      the host re-reads settings/stats       */
    RF_EVENT_ERROR          = 5,   /* detail: human-readable message         */
} rf_event;

typedef void (*rf_event_fn)(void *user, rf_event ev, const char *detail);

/* ------------------------------------------------------------------------
 * The operations table. The plugin returns a pointer to a static instance
 * from fpvos_rflink_entry(). Unimplemented OPTIONAL ops may be NULL; the
 * host checks for NULL before calling. REQUIRED ops must be present.
 *
 * Threading: the host calls every op below from a single "link thread",
 * one at a time, EXCEPT get_stats(), which it may call from the display
 * thread while another op is in flight. Make get_stats() safe to call
 * concurrently with everything else (a lock or an atomic snapshot is fine).
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t size;             /* = sizeof(rf_link_ops)                      */
    uint32_t abi_version;      /* RF_LINK_ABI_VERSION                        */

    /* --- lifecycle (REQUIRED) ---------------------------------------- */

    /* Is this plugin's hardware present on this goggle? Must be quick and
     * side-effect free; called for every installed plugin at boot. */
    int       (*probe)(void);

    /* Bring up the driver/daemon and return an instance, or NULL. Fills
     * *info with the plugin description (pointer stays valid until close). */
    rf_link  *(*open)(const rf_link_info **info);
    void      (*close)(rf_link *l);

    /* Register the event sink. May be called before connect(). */
    void      (*set_event_fn)(rf_link *l, rf_event_fn fn, void *user);

    /* --- link control (REQUIRED: connect/disconnect/get_stats) ------- */

    /* Associate with the air unit. Returns when the link is up or after
     * timeout_ms (RF_ERR_TIMEOUT). The plugin keeps trying to associate in
     * the background after a timeout; LINK_UP arrives as an event. */
    int       (*connect)(rf_link *l, const rf_config *cfg, uint32_t timeout_ms);
    void      (*disconnect)(rf_link *l);

    /* Snapshot the link. Cheap; the host polls it several times a second. */
    int       (*get_stats)(rf_link *l, rf_stats *out);

    /* OPTIONAL, gated by RF_CAP_SET_*. Apply while linked; the plugin moves
     * BOTH ends if the link protocol needs that (e.g. tell the air unit). */
    int       (*set_channel)(rf_link *l, uint32_t freq_khz, uint32_t timeout_ms);
    int       (*set_bandwidth)(rf_link *l, uint32_t bw_khz, uint32_t timeout_ms);
    int       (*set_tx_power)(rf_link *l, uint32_t mw, uint32_t timeout_ms);

    /* OPTIONAL, RF_CAP_PAIR. Enter pairing/bind mode for timeout_ms. Returns
     * RF_OK once paired (and fires RF_EVENT_PAIRED), RF_ERR_TIMEOUT if not. */
    int       (*pair)(rf_link *l, uint32_t timeout_ms);

    /* --- video (REQUIRED) -------------------------------------------- */

    /* Copy up to `cap` bytes of encoded video into `buf`. Returns the byte
     * count (>0), 0 if nothing arrived within timeout_ms, or a negative
     * RF_ERR_*. `meta` may be NULL if the host does not want it. */
    int       (*read_video)(rf_link *l, uint8_t *buf, size_t cap,
                            uint32_t timeout_ms, rf_video_meta *meta);

    /* --- data channels (OPTIONAL, see RF_CAP_MSP_CHANNEL / _CONTROL_) -- */

    int       (*read_channel)(rf_link *l, rf_channel_id ch,
                              uint8_t *buf, size_t cap, uint32_t timeout_ms);
    int       (*write_channel)(rf_link *l, rf_channel_id ch,
                               const uint8_t *buf, size_t len, uint32_t timeout_ms);

    /* --- air unit (OPTIONAL, RF_CAP_AIR_SETTINGS / RF_CAP_VIDEO_MODE) -- */

    int       (*set_air_setting)(rf_link *l, rf_air_setting id, int32_t value,
                                 uint32_t timeout_ms);
    int       (*get_air_setting)(rf_link *l, rf_air_setting id, int32_t *value);
    /* Label for an enumerated value, or NULL. Owned by the plugin. */
    const char *(*air_setting_label)(rf_link *l, rf_air_setting id, int32_t value);
    int       (*set_video_mode)(rf_link *l, const rf_video_mode *mode,
                                uint32_t timeout_ms);

    /* --- diagnostics (OPTIONAL, RF_CAP_SPECTRUM_SCAN) ------------------ */

    /* Fill up to `max` entries; sets *count. Used by the channel-scan screen.*/
    int       (*scan_channels)(rf_link *l, rf_scan_entry *out, size_t max,
                               size_t *count, uint32_t timeout_ms);

    /* New optional ops are appended here in future MINOR versions. A host
     * built against a newer header checks `size` before touching them. */
} rf_link_ops;

/* The entry point every plugin exports. `host_abi_version` is the host's
 * RF_LINK_ABI_VERSION; return NULL if you cannot work with that host (the
 * MAJOR differs). Returning a table is a promise that its `abi_version` is
 * the one it was really built against. */
typedef const rf_link_ops *(*fpvos_rflink_entry_fn)(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif

#endif /* FPVOS_RF_LINK_H */
