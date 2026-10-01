#ifndef AR8030_SOURCE_H
#define AR8030_SOURCE_H

#include <atomic>
#include <mutex>
#include <utility>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../vdec/vdec.hpp"
#include "../osd.hpp"
#include "../dvr.hpp"
#include "../webstream.hpp"
#include "../common.hpp"
#include "ar8030_sky.hpp"

// Direct AR8030 video source.
//
// The Artosyn AR8030 baseband does not hand us an RTP/UDP stream the way the
// ArtLynk link does: video arrives as a byte stream on a "BB socket" served by
// /ar8030soc/daemon, which brokers access to the driver (/dev/ar_mdev0).
// We read that stream, split it into NAL units on Annex-B start codes and feed
// them straight to the decoder - no UDP hop, no RTP reassembly.
//
// Replaces Pipeline when --ar8030 is given.
class Ar8030Source {
    public:
        Ar8030Source(const VideoCodec& codec_, std::shared_ptr<Vdec> vdec_,
                     std::shared_ptr<DVR> dvr_, std::shared_ptr<OSD> osd_,
                     volatile bool* stop_signal,
                     std::string host_ = "127.0.0.1", int host_port_ = 50000,
                     int slot_ = 0, int video_port_ = 3);
        ~Ar8030Source();

        // Thread entry point (matches Pipeline::run_thread usage in main.cpp).
        static void* run_thread(void* arg) {
            static_cast<Ar8030Source*>(arg)->run();
            return nullptr;
        }

        void run();

    private:
        const VideoCodec& codec;
        std::shared_ptr<Vdec> vdec;
        std::shared_ptr<DVR> dvr;
        std::shared_ptr<OSD> osd;
        volatile bool* should_stop;

        std::string host;
        int host_port;
        int slot;
        int video_port;

        void* bb_host = nullptr;   // bb_host_t*
        void* bb_dev = nullptr;    // bb_dev_handle_t*
        int   sockfd = -1;       // video (port 3)
        int   ctrl_sockfd = -1;  // telemetry/control (port 2)
        int   msp_sockfd  = -1;  // flight-controller MSP passthrough (see msp_bb_port)
        int   connect_fails = 0;
        int   read_timeouts = 0;
        bool  link_reported = false;
    public:
        // Overridable from the command line (--ar8030-freq / --ar8030-bw).
        static unsigned freq_khz;
        static int bandwidth;
        static bool do_pair;
        static int ap_index;   // which bb_mac_addr_N to use as the AP
        static bool chan_auto; // channel adaptation (scan for the AP)
        static bool chan_manual_cli;   // --ar8030-chan-manual: pinned to freq_khz from the start
        static int tx_power_dbm;
        static int tx_power_mw;   // stock's encoding: N = hold N mW, N+1 = auto capped at N
        static int panel_latency_us;   // the screen's share (vblank to light), 0 = not counted
        static int air_floor_us;       // fastest capture -> first slice (air_floor_ms), see air_delay_for
        static bool tx_power_auto;
        void apply_tx_power(int mw); // PA output; stock uses 24
        static bool skip_handshake;
        // Air-unit standby / low RF power: -1 leaves it alone, 0/1 is pushed
        // once after every handshake. Re-sending matters because our
        // SET_CONFIG is a byte-copy of a stock capture, so whatever standby
        // state that capture held is replayed at each link-up.
        static int  standby_mode;
        // BB_SET_PRJ_DISPATCH cmd 138. Stock sends this at link bring-up and we
        // never did; see send_prj_rf_config(). -1 on either disables the call.
        static int  prj_rf_bw;      // stock: 4
        static int  prj_rf_pwr_mw;  // stock: 500
        // Air-side bandwidth, sky cmd 0x24. UNVERIFIED units - stock's GUI
        // stores MHz (5/10/20/40, 255=auto) but whether the sky command carries
        // MHz or a bb_bandwidth_e gear is not established. -1 disables.
        static int  air_bw;
        // Replay stock's startup SET sequence verbatim. See the .cpp.
        static int  replay_stock_rf;
        // BB socket port carrying the flight controller's MSP stream, or 0 to
        // leave it alone. Opt-in: bb_socket_open() has no timeout (section 33),
        // so a port the air unit does not serve can wedge bring-up.
        static int  msp_bb_port;
        // --debug-bb-dump: raw copy of the port-2 telemetry stream, for finding out
        // what the air unit actually sends. Capped so a forgotten flag cannot
        // fill the rootfs.
        static std::string dump_path; // isolate: configure + RX only, never TX
        // --debug-replay: video from a file instead of the baseband (run_replay).
        // --debug-video-dump: the video socket's bytes to a file, as they
        // arrive - something to replay. Capped like dump_path.
        static std::string replay_path, video_dump_path;
        static bool decode_enabled; // --ar8030-decode; off = link/OSD only
        // --ar8030-mode WxH@FPS: retune the camera after the handshake.
        // 0 leaves whatever the stock handshake configured.
        static int mode_w, mode_h, mode_fps, mode_chn;
        // Menu -> radio mode change. The OSD menu runs on its own thread and
        // must not call into the BB SDK, so it just parks an index here and the
        // RX thread applies it on its next pass. -1 = nothing pending.
        static std::atomic<int> pending_mode_index;
        static void request_mode_index(int idx) { pending_mode_index.store(idx); }
        static bool probe_modes;    // --debug-probe-modes: sweep and report acks
        static bool probe_rf;       // --debug-probe-rf: which RF ioctls work here

        // Camera settings parked by the OSD menu thread (field id, value).
        // Same reason as pending_mode_index: the menu must not call the SDK.
        enum CamField { CAM_EV = 1, CAM_SAT, CAM_CONTRAST, CAM_SHARPNESS,
                        CAM_SCENE,
                        // This ordinal (cmd_cb id 0x306) was Anti-Flicker
                        // (sky cmd 0x1F) - dropped because stock's own Camera
                        // menu has no such row at all (confirmed against a
                        // photo of it: Scene, EV, Saturation, Sharpness,
                        // Contrast, WB, Rotate, Ratio, 3D DNR only). Reused
                        // here for "Focus Mode", sky cmd 0x0E
                        // (CMD_SET_CHN_FOCUS) - present in stock's Display
                        // tab (also confirmed by photo) and already had its
                        // own wire command and SkyConfig field, just never
                        // wired to the menu.
                        CAM_FOCUS, CAM_AWB,
                        // Air-unit standby / low-RF-power mode. Not a camera
                        // setting, but it travels the same sky socket, so it
                        // rides the same queue rather than growing a second one.
                        CAM_STANDBY,
                        // Air-unit RF bandwidth, sky cmd 0x24.
                        CAM_BW,
                        // Air-unit TX power in mW, sky cmd 0x22.
                        CAM_PWR,
                        // Camera rotation - stock's "Rotate" row, sky cmd 0x06.
                        CAM_ANGLE,
                        // Image aspect ratio - WAS stock's "Ratio" row, sky cmd
                        // 0x07. Menu row removed (verification pass found this
                        // air unit acks the SET but, unlike every other camera
                        // field, never reflects a change in its own status
                        // echo - looks unsupported on this hardware). The
                        // ordinal stays reserved and unused rather than being
                        // deleted: main.cpp computes CamField from a literal
                        // cmd_cb id as (cmd - 0x300), so CAM_3DNR's 0x30D only
                        // lines up with CAM_3DNR's ordinal as long as nothing
                        // shifts into this slot.
                        CAM_RATIO_UNUSED,
                        // 3D DNR, sky cmd 0x1B. body[35] of the config carries it
                        // too, but that is the air's echo in its status report -
                        // writing it there is acked and ignored.
                        CAM_3DNR };
        static void request_setting(int field, int value);

        // RF controls. Which of these the baseband actually honours is decided
        // at run time in configure_link() from the ioctl return codes - on this
        // board BB_SET_BANDWIDTH answers -7, so the menu simply does not offer
        // it. Bits are published to the OSD via OSD::set_rf_caps().
        enum RfCap { RF_CAP_POWER = 1, RF_CAP_MCS = 2, RF_CAP_LNA = 4,
                     RF_CAP_BW = 8, RF_CAP_CHAN = 16 };
        enum RfField { RF_TX_POWER = 1, RF_MCS, RF_LNA, RF_BW, RF_CHAN, RF_HOP };
        static void request_rf(int field, int value);

        // ---- binding (pairing) -------------------------------------------
        // Binding is a baseband operation, so it has to run on the RX thread
        // like every other ioctl here. The menu thread and the front-panel
        // button thread only park a request, exactly as pending_mode_index
        // does, and read the result back through atomics so the OSD can draw
        // the state without taking a lock.
        //
        // It is a state machine stepped once per RX pass rather than a
        // blocking 30-second loop: the RX thread is also what keeps the OSD
        // repainting when no video is arriving, so blocking it would freeze
        // the very indicator that exists to show binding is happening.
        // BIND_LINKED: the window was closed because the radio started
        // hearing an air unit on its own, so there was nothing left to find.
        // Distinct from BIND_FAILED, which means the window ran out in
        // silence - opposite situations that must not share a caption.
        enum BindState { BIND_IDLE = 0, BIND_RUNNING, BIND_OK, BIND_FAILED,
                         BIND_LINKED };
        static std::atomic<int> bind_state;      // BindState
        static std::atomic<int> bind_secs_left;  // countdown while RUNNING
        static std::atomic<unsigned> bind_peer;  // peer MAC, packed big-endian
        static void request_bind() { bind_request.store(1); }
        static bool bind_busy() { return bind_state.load() == BIND_RUNNING; }
        // How long the radio listens for an air unit, and how long a finished
        // result stays on screen before the indicator clears itself.
        //
        // Stock hardcodes 30s. That is too short here, and the reason is the
        // air unit rather than the radio: it only accepts pairing during a
        // window that opens 25-40s AFTER it boots, so a 30s ground window has
        // to be started at exactly the right moment in the air unit's boot to
        // overlap at all. Measured by hand, that is close to unhittable.
        //
        // 120s lets the natural sequence work instead: press bind, then power
        // cycle the air unit and let it come up inside the window. A second
        // press cancels, so a long window costs nothing.
        static const int kBindWindowS      = 120;
        static const int kBindResultHoldMs = 6000;
    private:
        static std::atomic<int> bind_request;
        void bind_step();                       // one non-blocking pass
        bool bind_begin();
        void bind_finish(bool ok, const uint8_t peer[4], bool cancelled = false);
        // Persist a newly bound air unit into /factory/user_cfg.json so the
        // pairing survives a reboot. Returns false if it could not be written;
        // the bind itself still holds for this session.
        static bool save_paired_mac(const uint8_t mac[4]);
        uint64_t bind_started_ms_   = 0;
        uint64_t bind_next_poll_ms_ = 0;
        uint64_t bind_done_ms_      = 0;
        // Whether the air unit was already talking when the window opened.
        // If it was, this is a deliberate bind of a SECOND air unit while the
        // first is connected, and traffic is not a reason to stop.
        bool     bind_had_data_     = false;
        int      bind_role_         = 1;        // BB_ROLE_DEV until GET_STATUS says otherwise
        // Stability filter for the pair result. Stock requires the same peer
        // address to come back unchanged 11 polls running before it accepts
        // it, restarting the count on any change - early in a window the radio
        // reports addresses that then vanish.
        static const int kBindStableReads = 10; // accept on the 11th agreeing read
        uint8_t  bind_seen_[8][4]   = {};
        int      bind_seen_n_[8]    = {};

        // Annex-B accumulator: bb_socket_read() returns arbitrary chunks, so a
        // NAL can straddle reads. We keep a rolling buffer and only emit
        // complete NALs (i.e. up to the *next* start code).
        std::vector<uint8_t> accum;
        int64_t frame_pts = 0;
        uint64_t frames_seen = 0;
        long long bytes_received = 0;
        uint64_t period_start = 0;
        uint64_t last_tick_ms = 0;
        uint64_t last_data_ms = 0;   // last read that returned video bytes
        // Last byte off the telemetry socket, and when we last re-sent the
        // handshake because of the gap between the two. An air unit that is
        // talking on port 2 but silent on port 3 is present and linked but has
        // lost its streaming state - which is a different fault from an air
        // unit that is simply off, and the only one re-handshaking can fix.
        uint64_t last_ctrl_ms = 0;
        uint64_t last_rehandshake_ms = 0;
        int      rehandshakes = 0;
        // The radio's own view of the link, polled in the run loop. The
        // handshake has to ride a link that is actually up: sent before the
        // radios have associated it goes into the air and nothing answers.
        int      last_link_state = -1;
        uint64_t last_link_poll_ms = 0;
        uint64_t last_scan_ms = 0;
        int      link_freq_mhz = 0;   // real link frequency, from BB_GET_STATUS
        bool     scan_logged = false;
        uint64_t last_stat_ms = 0;
        int dumped_reads = 0;
        unsigned long long total_bytes = 0;
        unsigned long long nal_count = 0;
        unsigned long long ctrl_bytes = 0;  // telemetry drained off port 2
        unsigned long long msp_bytes = 0;   // FC MSP drained off msp_bb_port
        int last_link_kbps = 0;
        int last_bw_idx = -1;
        int last_bw_rx_idx = -1;  // rx_status's gear, kept only for the log
        bool air_cfg_logged = false;
        uint8_t  last_air_angle = 0;
        uint16_t last_air_w = 0, last_air_h = 0;
        uint8_t  last_air_fps = 0;
        uint8_t  last_air_dnr3d = 0;
        uint8_t  last_air_focus = 0;
        void*              dump_fp = nullptr;
        unsigned long long dump_written = 0;
        void*              video_dump_fp = nullptr;
        unsigned long long video_dump_written = 0;
        // Access-unit assembly for sliced pictures (see emit_nal): the air
        // unit sends each picture as two slice NALs; group them into one packet/PTS.
        std::vector<uint8_t> au_slices;
        int64_t  au_pts = 0;
        uint8_t  au_nal_type = 0;
        bool     au_open = false;
        // Set when the link comes back: drop video until the stream is arriving
        // at the rate it was captured, then restart from the next keyframe.
        //
        // A reconnecting air unit dumps its startup buffer as fast as the link
        // will carry it - pictures captured 16.7ms apart arriving 0-4ms apart -
        // so the first seconds are history played at up to 16x. Waiting for a
        // keyframe alone does not help: that burst is new-stream data and opens
        // with one. Cadence is the signal that actually separates catching up
        // from being live, and both halves of it are already in every picture
        // header.
        bool     resync_          = false;
        int      resync_live_     = 0;   // consecutive pictures at real-time pace
        uint64_t resync_start_ms_ = 0;
        uint64_t resync_prev_us_  = 0;   // arrival of the previous picture
        uint32_t resync_prev_cap_ = 0;   // air-side capture stamp of the same
        // Three in a row, so one straggler in the burst cannot end it early.
        static constexpr int      kResyncLive  = 3;
        // ...and a ceiling, so a stream that never settles still shows a
        // picture rather than leaving the goggle on the lock screen for good.
        static constexpr uint64_t kResyncMaxMs = 5000;
        // Arrival time of the FIRST slice of the access unit currently being
        // assembled. flush_access_unit() used to hand the decoder the arrival
        // time of the NEXT picture's first slice (the read that triggered the
        // flush), so every end-to-end latency measured against it was offset
        // by a frame interval and understated. The AU's own first-byte arrival
        // is the correct zero point for "glass to glass" on the ground side.
        uint64_t au_first_recv_us = 0;
        // Arrival time of the LAST slice appended to the AU. Reassembly is
        // au_last - au_first; using the flush-triggering read instead measures
        // first-slice-to-next-picture's-first-slice, i.e. the frame interval.
        uint64_t au_last_recv_us = 0;
        // End of picture: flush_access_unit() as the picture's last slice
        // arrives rather than when the next picture's first does.
        int      ctb_addr_bits = 0;          // slice address width, from the SPS
        uint8_t  pps_dep_slices[64] = {0};   // dependent_slice_segments_enabled_flag per PPS
        int      au_max_addr = -1;           // largest slice start in the picture being built
        int      last_slice_addr = -1;       // learned start of a picture's last slice
        int      last_addr_cand = -1, last_addr_streak = 0;
        uint32_t late_slices = 0, late_window_pics = 0;
        uint64_t late_slices_total = 0;
        static constexpr int      kLearnRun = 32;
        static constexpr uint32_t kLateMax  = 4;
        void     learn_last_slice(int max_addr);
        void     late_slice();

        // Capture -> arrival, per picture. The video header's capture stamp
        // is the air unit's CLOCK_MONOTONIC in microseconds (checked against
        // the encoder's PTS in /proc/umap/venc), so arrival - capture is the
        // true delay plus a constant clock offset. The offset comes out as
        // the smallest such difference seen lately, which is the offset plus
        // the fastest a picture ever makes it; that fastest time is a
        // property of the air pipeline and was measured with both clocks
        // synced through a host (air_floor_us, air_floor_ms). A window, not an
        // all-time minimum: the two crystals drift apart by ~7 ppm.
        static constexpr int kAirWinSlots = 8;          // x 500 ms
        int64_t  air_win_min[kAirWinSlots];
        uint64_t air_win_slot[kAirWinSlots] = {};
        uint64_t cap_us64 = 0;          // last header's stamp, unwrapped
        uint32_t cap_last32 = 0;
        uint32_t cap_wraps = 0;
        bool     cap_valid = false;
        unsigned hdr_fps = 0;           // from the header, for the calibration
        uint32_t au_air_us = 0;         // this picture's capture -> arrival
        float    air_delay_ms = -1.0f;  // EWMA of it, for the stats line
        uint32_t air_delay_for(uint64_t recv_us);
        uint16_t last_hseq = 0;          // header frame counter, for loss
        bool     hseq_valid = false;
        unsigned long long frames_lost = 0;

        // Stale pictures. An air unit whose encoder is driven in a way its
        // own app does not expect (intra refresh switched on underneath it)
        // sends pictures from seconds ago - its encoder ring over again,
        // header and all - between the live ones. Mixed into the live stream
        // they put two pictures' slices in one access unit, and MPP answered
        // with a hardware timeout and reset on nearly every picture.
        // The header's capture stamp says which pictures those are: anything
        // not newer than the newest picture shown (stale_picture()).
        bool     stale_picture(uint32_t cap32);
        uint32_t live_cap_ = 0;          // capture stamp of the newest picture shown
        bool     live_cap_valid_ = false;
        uint32_t stale_prev_cap_ = 0;    // the last picture refused, and how many
        int      stale_run_ = 0;         // refused in a row that run forward
        bool     hdr_stale_ = false;     // the NALs after the last header are stale
        uint16_t hdr_seq_ = 0;           // the last header's counter, stale or not
        bool     hdr_seq_valid_ = false;
        int      slice_hseq_ = -1;       // counter of the header ahead of the next slice
        int      au_hseq_ = -1;          // ...and of the picture being built
        unsigned long long stale_pics = 0, foreign_slices = 0;
        uint16_t sps_ids_ok_ = 0;        // SPS ids a valid SPS has been seen for
        // A new clock - the air unit rebooted - also reads as not newer, but
        // runs forward from where it restarted, uninterrupted by live
        // pictures. That many in a row and it is the live stream.
        static constexpr int kStaleAdopt = 16;

        bool connect_bb();
        void subscribe_events();    // stock does this before any socket
        void configure_link();      // candidates + freq + bandwidth
        void send_air_handshake();  // start-streaming frames -> air unit
        void apply_video_mode();    // --ar8030-mode -> SET_CHN_RES
        void probe_video_modes();   // ask the camera which modes it accepts
        void probe_rf_ioctls();     // which BB_SET_* the local radio accepts
        void drain_video_socket();  // discard video while not in the RX loop
        int  wait_sky_status(uint8_t want_cmd, int timeout_ms);
        bool read_sky_ack(uint8_t want_cmd, int timeout_ms);
        void drain_control_socket();  // keep the port-2 RX ring from overflowing
        void drain_msp_socket();      // FC MSP passthrough -> OSD DisplayPort
        void apply_bandwidth();       // must run after the link is up (see .cpp)
        void send_prj_rf_config();    // BB_SET_PRJ_DISPATCH cmd 138 (see .cpp)
        void replay_stock_rf_setup(); // stock's startup SETs, byte-for-byte
        sky::Proto sky_proto;       // owns the outgoing seq counter

        // Camera settings are re-sent to the air unit in the SET_CONFIG frames
        // of every handshake, so whatever we send there is what the VTX runs
        // after a power cycle. Replaying stock's captured frames therefore threw
        // away any change made from the menu. We now keep a live copy, persist
        // it per air unit (keyed by the AP MAC from /factory/user_cfg.json, so
        // two paired VTXs keep separate settings), and build the SET_CONFIG
        // frames from it.
        sky::SkyConfig sky_cfg;
        std::string    sky_cfg_key;      // "sky_E5DA0008" - empty until known
        void  set_sky_key_from_mac(const uint8_t mac[4]);
        void  load_sky_config();
        void  save_sky_config();
        // The TX power this air unit last ran with, kept beside its camera
        // settings. The channel is not kept: the air unit decides it.
        void  load_sky_link();
        void  save_sky_link();
        std::vector<uint8_t> build_config_frame(const uint8_t *tmpl, size_t len);
        void apply_pending_settings();
        void apply_pending_rf();
        unsigned rf_caps = 0;
        bool ranging_ok = false;
        int  ranging_tries = 0;
        static std::vector<std::pair<int,int>> pending_rf;
        static std::mutex pending_mtx;
        static std::vector<std::pair<int,int>> pending_settings;
        void report_link_status();  // one-shot link state log
        int  current_rx_mcs();      // link MCS right now, -1 if unavailable
        int  current_link_state();  // 0 not linked, 1 connecting, 2 linked, -1 unknown
        void enable_ranging();      // BB_CFG_DISTC - time-of-flight distance
        int  current_distance();    // BB_GET_DISTC_RESULT, -1 = no fix yet
        // Fill the OSD's link panel. This used to come from ArtosynRpcClient,
        // but that is a second SDK client on the same daemon and cannot run
        // beside us (section 22.1) - so we publish the same fields in-process
        // from the ioctls we already own, otherwise the panel reads "IDLE"
        // while video is plainly decoding.
        void publish_link_stats();
        // Spectrum scan for the channel-scan screen. Same story as the link
        // panel: this used to come from ArtosynRpcClient, which cannot run
        // beside us, so we poll BB_GET_CHAN_INFO in-process instead.
        void publish_chan_scan();
        void scan_air_status(const uint8_t *buf, int n);  // inbound cmd 0x03
        // Every distinct validated VPS/SPS/PPS seen, replayed into a new
        // recording so its hvcC is complete.
        std::vector<std::vector<uint8_t>> dvr_param_sets;
        unsigned long long   ps_dropped = 0;   // corrupt param-set NALs rejected
        bool air_standby = false;      // VTX in low-power/standby mode
        bool air_status_seen = false;  // have we decoded one status frame yet

        // Move the RF link to freq_khz, both ends. Mirrors stock's
        // fpv_bb_set_freq() plus the sky 0x21 it pairs with.
        // Returns 0 on success, negative on failure (frequency not in the radio's table, or an ioctl refused).
        int  set_rf_channel(uint32_t freq_khz, bool hop_en);
        // Undo a pin: every channel back on the ground's work list, then
        // channel adaptation on - the ground searches, as at start.
        int  search_all_channels();
        uint32_t link_freq_khz();   // ground's own TX freq, 0 if unknown

        // Index of freq_khz in the radio's channel table, or -1. Also yields
        // the full table, which BB_SET_WORK_CHAN_LIST needs.
        int  chan_index_for_freq(uint32_t freq_khz, uint32_t *freqs, int *n_out);
    public:
        // Set by the OSD when the scan screen opens, so we poll faster while
        // the user is looking at it (matching the old RPC client's cadence).
        static std::atomic<bool> scan_active;
    private:
        // (was enter_pair_mode: a blocking 30s loop reachable only from the
        //  command line. Replaced by the bind_* state machine above, which the
        //  menu, the front-panel button and --ar8030-pair all drive.)
        void disconnect_bb();
        void run_replay();
        void consume(const uint8_t* data, size_t len);
        void emit_nal(const uint8_t* nal, size_t len);
        void flush_access_unit(uint64_t recv_us);
        void update_stats(size_t frame_size);
        void update_dvr(std::shared_ptr<std::vector<uint8_t>> frame);
};

#endif
