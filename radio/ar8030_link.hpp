#ifndef AR8030_SOURCE_H
#define AR8030_SOURCE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hal/vdec.hpp"
#include "base/video_codec.hpp"
#include "system/air/power.hpp"
#include "radio/settings.hpp"
#include "base/bus.hpp"
#include "radio/link.hpp"
#include "radio/radio.hpp"
#include "radio/ar8030_config.hpp"
#include "radio/model.hpp"
#include "radio/streams.hpp"
#include "radio/video_out.hpp"
#include "base/slice_times.hpp"
#include "radio/frame_clock.hpp"
#include "radio/ar8030_sky.hpp"

// Direct AR8030 video source.
//
// The Artosyn AR8030 baseband does not hand us an RTP/UDP stream the way the
// ArtLynk link does: video arrives as a byte stream on a "BB socket" served by
// /ar8030soc/daemon, which brokers access to the driver (/dev/ar_mdev0).
// We read that stream, split it into NAL units on Annex-B start codes and feed
// them straight to the decoder - no UDP hop, no RTP reassembly.
//
// The radio kestrel-gnd has (radio::Link): it reports what it learns to its
// model (radio::Model), writes the air unit's streams into the model's
// feeds, and carries out the commands the model hands it.
class Ar8030Link : public radio::Link, private radio::VideoOut::Source {
    public:
        // The names it uses (radio/radio.hpp, and the parts' in system/).
        using CamField  = radio::CamField;
        using RfField   = radio::RfField;
        using RfCap     = gnd::RfCap;
        using BindState = gnd::BindState;
        using AirInfo   = air::Info;
        Ar8030Link(radio::Model& model, const Ar8030Config& cfg,
                     Settings& settings, const VideoCodec& codec_, std::shared_ptr<Vdec> vdec_,
                     std::atomic<bool>* stop_signal,
                     std::string host_ = "127.0.0.1", int host_port_ = 50000,
                     int slot_ = 0, int video_port_ = 3);
        ~Ar8030Link() override;

        // Thread entry point (matches Pipeline::run_thread usage in main.cpp).
        static void* run_thread(void* arg) {
            static_cast<Ar8030Link*>(arg)->run();
            return nullptr;
        }

        // ---- radio::Link ----
        void run() override;
        // Video goes to sinks (radio/video_out), called on the radio thread
        // with the pictures themselves (a reference, no copy). Before it runs.
        void add_video_sink(bus::VideoSink* s) { video_.add_sink(s); }
        void apply(const radio::Command& c) override;
        radio::State held() const override;

    private:
        bool native_sdio_;
        std::string pairing_file_;
        std::optional<std::array<uint8_t, 4>> peer_;
        radio::Model& model_;            // what it learns goes to its model, which takes its commands
        Settings& settings_;             // the settings file: each air unit's own, the last peer
        // The streams it carries from the air unit, into the model's feeds
        // (radio/streams.hpp): the FC serial bridge, a new session at every
        // link-up and for another aircraft; the IMU samples, read out of the
        // video's SEI.
        radio::FcFeed& fc_out_;
        radio::ImuFeed& imu_out_;
        // Its video out (radio/video_out.hpp): pictures, the decoder, the
        // sinks. What it learns of each picture's capture it says as the
        // source (below); vhdr_: what the air unit's header said ahead of
        // the next slice.
        radio::VideoOut video_;
        radio::VideoOut::SliceHeader vhdr_;
        void picture_start(uint64_t recv_us, radio::VideoOut::PictureTiming& t) override;
        void picture_done(const radio::VideoOut::PictureDone& d) override;
        bool ask_keyframe(const char* why) override;
        void request_keyframe() override { request_air_idr(); }
        std::atomic<bool> fc_linked_{false};
        void report_link_state(int ls);  // to the bus, a new FC session at a link-up (the event's, the poll's)
        const VideoCodec& codec;
        std::shared_ptr<Vdec> vdec;
        std::atomic<bool>* should_stop;

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
        // What it was made with (Ar8030Config, radio/ar8030_config.hpp), then
        // its own: the commands and its threads change them.
        unsigned freq_khz;
        int bandwidth;
        bool do_pair;
        int ap_index;   // which bb_mac_addr_N to use as the AP
        std::atomic<bool> chan_auto; // channel adaptation (scan for the AP)
        bool chan_manual_cli;   // --ar8030-chan-manual: pinned to freq_khz from the start
        // The channel page's AUTO (0) or AUTO HOP (1), saved as ar8030_chan_hop.
        // Both only with SYSTEM > Time Zone set (region_auto_apply): AUTO moves
        // the link to the quietest channel the country allows when it
        // connects; AUTO HOP only has the air unit hop by itself on them,
        // wherever it steers (the ground moves nothing).
        std::atomic<int> chan_hop;
        // FIXED: the channel picked on the channel page (MHz, 0: AUTO / AUTO
        // HOP), saved as ar8030_chan_mhz. At every link-up the link moves back
        // there (region_auto_apply): after a start or a lost link the ground
        // searched, and the air unit may have come up anywhere.
        std::atomic<int> chan_fixed_mhz;
        // How many channels the air unit was given to hop on; 0: it does not hop.
        std::atomic<int> region_hop_n{0};
        // TX power as the radios push it (Open8030 event 14), dBm; 0 = not
        // known: the air unit's as it announced it, and our radio's own.
        std::atomic<int> air_live_dbm_{0}, gnd_live_dbm_{0};
        // Open8030 at both ends: the air radio's resend map (poll_resends, from
        // the stats thread every 100 ms), the frames with blocks sent again
        // published as they are learnt (the resends channel). resend_known_:
        // our radio gives it.
        std::atomic<bool> resend_known_{false};
        // AUTO's pick is due: at link-up, or AUTO / AUTO HOP just chosen.
        std::atomic<bool> region_pick{false};
        // Something AUTO's list depends on changed (time zone, licence, Max
        // Bandwidth; the link-up is noted here): the list goes to the air
        // unit again, from the radio thread.
        std::atomic<bool> region_due{false};
        void region_changed() { region_due.store(true); }
        // Whether AUTO may take a channel (centre MHz) where the pilot is, at
        // the width the link runs: at least 20 MHz (a link starts on 5 and
        // widens), 40 only while it is on 40, never past Max Bandwidth.
        bool region_usable(int mhz);
        std::atomic<int> link_bw_mhz{20};   // the link's width now (bandwidth events)
        int tx_power_dbm;
        std::atomic<int> tx_power_mw;   // stock's encoding: N = hold N mW, N+1 = auto capped at N
        // The power chosen in the menu (saved per air unit); tx_power_mw is it
        // fitted to what the air unit connected now is offered (ar_pwr_fit),
        // so a Lite's 100mW AUTO never replaces a 500mW AUTO chosen for a Lite+.
        std::atomic<int> tx_power_want_mw;
        // The air unit's project number (TLV 0x11: 4 Lite, 6 RC, 7 Lite+), 0 until
        // it reports; picks the power levels it is offered (ar_pwr_offered).
        std::atomic<int> air_prj{0};
        void clear_air_info();
        // A setting the radio loads with (its firmware, a stock radio's
        // bandwidth cap) changed: saved now, then the radio and kestrel-gnd
        // restarted with it, from a shell of their own.
        void restart_with(const Setting<int>& setting, int value, const char* why);
        // kestrel-air's capabilities, named for the INFO tab, by the feature bit
        // (or AirInfo::said bit) that says the air unit has it on.
        struct AirCap { uint16_t bit; const char *key; const char *name; };
        static const AirCap kAirCaps[12];
        // What the air unit has on, by name: its feature bits and what it said.
        static std::vector<std::string> cap_names(const AirInfo &a);
        int air_floor_us;       // fastest capture -> first slice (air_floor_ms), see air_delay_for
        std::atomic<bool> tx_power_auto{false};   // the radio thread's, read by the stats thread
        void apply_tx_power(int mw); // PA output; stock uses 24
        bool skip_handshake;
        // Air-unit standby / low RF power: -1 leaves it alone, 0/1 is pushed
        // once after every handshake. Re-sending matters because our
        // SET_CONFIG is a byte-copy of a stock capture, so whatever standby
        // state that capture held is replayed at each link-up.
        int  standby_mode;
        // BB_SET_PRJ_DISPATCH cmd 138. Stock sends this at link bring-up and we
        // never did; see send_prj_rf_config(). -1 on either disables the call.
        int  prj_rf_bw;      // stock: 4
        int  prj_rf_pwr_mw;  // stock: 500
        // Air-side bandwidth, sky cmd 0x24. UNVERIFIED units - stock's GUI
        // stores MHz (5/10/20/40, 255=auto) but whether the sky command carries
        // MHz or a bb_bandwidth_e gear is not established. -1 disables.
        int  air_bw;
        std::atomic<int>  max_kbps;   // ar8030_max_kbps: the video bitrate cap sent at link-up, 0 = none
        std::atomic<bool> air_logs;   // hud_debug: the air unit's flight logs, sent at link-up
        bool stream_decode;   // KESTREL_STREAM_DECODE / stream_decode: 0 (default on)
        std::atomic<int>  max_bw_mhz; // ar8030_max_bw_mhz: the video link bandwidth cap sent at link-up, 5/10/20/40, 0 AUTO (stock: 20 or 40)
        std::atomic<int>  max_mcs;    // ar8030_max_mcs: Open8030 here, the highest MCS its receive policy may use (-1: none)
        std::atomic<int>  steady;     // ar8030_steady: Open8030 here, a block sent again steps its MCS down (0/1)
        // Open8030 here: the caps are due (at link-up, from the menu) while the
        // count in caps_req_'s bits 1.. is past caps_sent_, the one send_caps
        // last had taken; bit 0: from the menu, AUTO / off sent too, to lift
        // what was set. One word, so a change made while send_caps is at it (a
        // try of up to 2 s) is not cleared with the one it sent.
        std::atomic<unsigned> caps_req_{2};
        unsigned caps_sent_ = 0;             // the stats thread's
        void caps_now(bool live) {
            unsigned v = caps_req_.load();
            while (!caps_req_.compare_exchange_weak(v, (v + 2) | (live ? 1u : 0u))) {}
        }
        std::atomic<int>  radio_fw;   // ar8030_firmware: 0 stock, 1 Open8030 (bb_only.sh loads it with the radio)
        // ar8030_frame_mode (RADIO > Radio Frame): the TDD frame mode asked of
        // the radio at start and after every link-up, 2 short or 0 standard
        // (set_frame_mode()); frame_mode_changed: the menu changed it, send now.
        std::atomic<int>  frame_mode;
        std::atomic<bool> frame_mode_changed{false};
        // Replay stock's startup SET sequence verbatim. See the .cpp.
        int  replay_stock_rf;
        // BB socket port carrying the flight controller's MSP stream, or 0 to
        // leave it alone. Opt-in: bb_socket_open() has no timeout (section 33),
        // so a port the air unit does not serve can wedge bring-up.
        int  msp_bb_port;
        // --debug-bb-dump: raw copy of the port-2 telemetry stream, for finding out
        // what the air unit actually sends. Capped so a forgotten flag cannot
        // fill the rootfs.
        std::string dump_path; // isolate: configure + RX only, never TX
        // --debug-replay: video from a file instead of the baseband (run_replay).
        // --debug-video-dump: the video socket's bytes to a file, as they
        // arrive - something to replay. Capped like dump_path.
        std::string replay_path, video_dump_path;
        bool decode_enabled; // --ar8030-decode; off = link/OSD only
        // --ar8030-mode WxH@FPS: retune the camera after the handshake.
        // 0 leaves whatever the stock handshake configured.
        int mode_w, mode_h, mode_fps, mode_chn;
        // Menu -> radio mode change. The OSD menu runs on its own thread and
        // must not call into the BB SDK, so it just parks an index here and the
        // RX thread applies it on its next pass. -1 = nothing pending.
        std::atomic<int> pending_mode_index{-1};
        void request_mode_index(int idx) { pending_mode_index.store(idx); }
        bool probe_modes;    // --debug-probe-modes: sweep and report acks
        bool probe_rf;       // --debug-probe-rf: which RF ioctls work here

        // Camera settings parked by the OSD menu thread (field id, value):
        // radio::CamField. Same reason as pending_mode_index: the menu must
        // not call the SDK.
        void request_setting(int field, int value);

        // RF controls. Which of these the baseband actually honours is decided
        // at run time in configure_link() from the ioctl return codes - on this
        // board BB_SET_BANDWIDTH answers -7, so the menu simply does not offer
        // it. Bits are published to the OSD via OSD::set_rf_caps().
        void request_rf(int field, int value);

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
        std::atomic<int> bind_state{gnd::BIND_IDLE};      // gnd::BindState
        std::atomic<int> bind_secs_left{0};  // countdown while RUNNING
        std::atomic<unsigned> bind_peer{0};  // peer MAC, packed big-endian
        void request_bind() { bind_request.store(1); }
        bool bind_busy() { return bind_state.load() == gnd::BIND_RUNNING; }
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
        std::atomic<int> bind_request{0};
        void bind_step();                       // one non-blocking pass
        bool bind_begin();
        void bind_finish(bool ok, const uint8_t peer[4], bool cancelled = false);
        // Persist a newly bound air unit into /factory/user_cfg.json so the
        // pairing survives a reboot. Returns false if it could not be written;
        // the bind itself still holds for this session.
        bool save_paired_mac(const uint8_t mac[4]);
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
        // complete NALs (i.e. up to the *next* start code) - or up to the end
        // of the air unit's packet, when its header says where that is.
        std::vector<uint8_t> accum;
        uint64_t accum_base_ = 0;        // stream offset of accum[0]
        uint64_t pkt_end_pos_ = 0;       // stream offset just past the open packet's trailer, 0 = unknown
        // The slice checksum kestrel-air puts in each packet's extras (06 02
        // <XOR32>): checked once the payload is all here (check_slice_xor).
        bool     xor_pending_ = false;
        uint64_t xor_start_ = 0;         // stream offset of the payload
        uint32_t xor_len_ = 0, xor_want_ = 0;
        uint64_t xor_checked_ = 0, xor_bad_ = 0, xor_unchecked_ = 0;
        void     check_slice_xor();
        long long bytes_received = 0;
        uint64_t period_start = 0;
        uint64_t stats_frame_ms_ = 0;    // update_stats: the last frame's arrival (a gap starts a new period)
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
        uint64_t retune_down_ms_ = 0;          // the link dropped right after a bandwidth change (0: no)
        uint64_t link_lost_ms_ = 0;            // a link went down and has not come back (0: none)
        bool     lost_reopened_ = false;       // reopened for that loss: not again until a link
        uint64_t last_scan_ms = 0;
        std::atomic<int> link_freq_mhz{0};   // real link frequency, from BB_GET_STATUS (the stats thread)
        bool     scan_logged = false;
        uint64_t last_stat_ms = 0;
        int dumped_reads = 0;
        unsigned long long total_bytes = 0;
        unsigned long long nal_count = 0;
        unsigned long long ctrl_bytes = 0;  // telemetry drained off port 2
        unsigned long long msp_bytes = 0;   // FC MSP drained off msp_bb_port
        std::atomic<int> last_link_kbps{0};      // written by the stats thread
        std::atomic<int> last_bw_idx{-1};        // written by the stats thread
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

        // The radio queries behind the HUD run on a thread of their own, so
        // the video never waits for a radio reply: on the video thread the
        // 3 s batch held a picture up by 2-3 ms every time. It polls the link
        // state (400 ms), the link figures and distance (3 s) and the channel
        // scan; the video thread acts on what it finds through the atomics.
        std::thread stats_thread;
        std::mutex stats_mu;
        std::condition_variable stats_cv;
        bool stats_stop = false;
        void start_stats();
        void stop_stats();
        void stats_run();
        std::atomic<int> link_state_{-1};          // newest: poll or event
        std::atomic<unsigned> link_events_{0};     // link-state events so far
        std::atomic<uint64_t> bw_change_ms_{0};    // the last bandwidth-change event (12)
        std::atomic<uint64_t> frame_mode_due_ms_{0}; // a link came up: frame mode 2 owed from then (0: none)
        // The video link's bandwidth gear (bb_bandwidth_e), -1 = no link. From
        // the bandwidth-change events: BB_GET_STATUS reports the uplink's 2.5 MHz.
        std::atomic<int> video_bw_idx_{-1};
        std::atomic<int> stat_mcs_{-1}, stat_dist_{-1};
        std::atomic<int> stat_snr_x10_{0};   // BB_GET_1V1_INFO self SNR, dB x 10 (stats thread)
        std::atomic<int> stat_gains_{0};     // BB_GET_1V1_INFO gain_a | gain_b << 8 (per-antenna AGC)
        std::atomic<bool> stats_soon_{false};      // refresh the link figures early
        // The link figures as last published (publish_link_stats): what a
        // call that fails keeps, for a few passes of failing in a row.
        gnd::LinkStats link_st_last_;
        // The distance as last pushed (event 15), and the frames whose SNR
        // the HUD has had (poll_resends).
        std::atomic<uint64_t> dist_pushed_ms_{0};
        uint32_t snr_done_ = 0;
        bool snr_done_valid_ = false;
        int snr_sum_ = 0, snr_n_ = 0;  // the frames' SNR since the last pass (0.25 dB)
        int link_st_fails_[3] = {};                // BB_GET_1V1_INFO, BB_GET_STATUS, BB_GET_MCS
    public:
        // ar_libre's event callbacks (on its reader thread): raw payloads.
        void on_link_event(const uint8_t* p);
        void on_mcs_event(const uint8_t* p);
        void on_logged_event(int ev, const uint8_t* p);
    private:
        // When the data of the last video read came off USB, from the kernel
        // (ar_libre's arlink_socket_rx_ns), or 0 when not known.
        uint64_t read_rx_us = 0;

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
        uint64_t cap_last_local_us = 0; // when it came, on this clock
        uint32_t cap_wraps = 0;
        bool     cap_valid = false;
        unsigned hdr_fps = 0;           // from the header, for the calibration
        float    air_delay_ms = -1.0f;  // EWMA of it, for the stats line
        uint32_t air_delay_for(uint64_t recv_us);

        // Who the air unit says it is. kestrel-air announces itself in its
        // version message (cmd 0x04, bytes 1..4: "KA", protocol, feature bits);
        // the stock air app sends zeros there. Reset at every link-up, since the
        // next air unit may be the other kind. The feature bits are kestrel-air's
        // (sky.h there): 1 = per-slice air-side times in the header bytes 35..41,
        // 2 = intra refresh (keyframes only when asked for), 4 = the camera's IMU
        // (samples as SEI in the video), 32 = the flight controller's IMU instead
        // (MSP_RAW_IMU polled 20 a second, relayed as every MSP response is).
        static constexpr uint8_t kAirFeatLatInfo = 0x01;
        static constexpr uint8_t kAirFeatIntraRefresh = 0x02;
        static constexpr uint8_t kAirFeatCamImu = 0x04;
        // Atomic: the stats thread reads them too (the frame mode asked).
        std::atomic<bool> air_ver_seen_{false};
        std::atomic<bool> air_kestrel_{false};
        uint8_t  air_proto_ = 0;
        uint8_t  air_feat_ = 0;
        unsigned air_untagged_ = 0;      // sane headers in a row without the tag
        unsigned air_stock_hdrs_ = 0;    // headers since link-up, none tagged, nothing decided
        bool     air_hdr_tag_ = false;   // seen in a slice header, which is more reliable than the message
        void note_air_version(const uint8_t *pl, size_t n);
        void note_air_info(const uint8_t *pl, size_t n);
        void note_air_frame(const uint8_t *pl, size_t n);   // KA_MSG_FRAME
        // What the air unit says it is, the radio's own copy (its threads
        // only): published on Bus::air_info whenever it changes.
        bus::PiMutex air_info_mtx_;
        AirInfo air_info_;
        void publish_air_info();
        void note_air_prj(int prj, const char *from);   // what the air unit is: its power levels
        // The power levels this air unit is offered: its project's, and kestrel-air's
        // own (kAirFeatPwrLvl) once it has said it takes them.
        // The power levels it takes (ar_pwr_offered): 0 stock's, 1 kestrel-air's, 2 with 400 mW.
        int air_pwr_fine() const {
            if (!air_kestrel_ || !(air_feat_ & kAirFeatPwrLvl)) return 0;
            return (air_info_feat_ & kAirFeatPwr400) ? 2 : 1;
        }
        void refit_air_power();   // the chosen power fitted to them (the menu's choice is kept)
        void note_air_announce(bool ka, uint8_t proto, uint8_t feat);
        // Air-side times of each slice, us, over 5 s windows, from the header.
        std::vector<uint32_t> lat_enc_, lat_queue_, lat_write_;
        unsigned lat_depth_max_ = 0;
        uint64_t lat_log_ms_ = 0;
        void note_air_latency(const uint8_t *hdr);
        // The radio's AP clock (BB_GET_AP_TIME, ms), which the air unit also
        // stamps into bytes 26..29 of each header: our now_us() minus the AP
        // clock, in us, refreshed once a second. With it each picture's
        // capture is a moment on our clock, so the air delay is measured
        // instead of pinned to air_floor_us.
        static constexpr uint8_t kAirFeatApClock = 0x08;   // byte 2: the stamp's sub-ms part
        static constexpr uint8_t kAirFeatMaxBw = 0x10;     // takes the goggle's bandwidth cap (sky 0x41)
        static constexpr uint8_t kAirFeatFcImu = 0x20;     // the flight controller's IMU (the Lite+: none on the camera)
        static constexpr uint8_t kAirFeat864p120 = 0x40;   // 864p120: its camera's 1536x864 window at 120 fps
        static constexpr uint8_t kAirFeatPwrLvl = 0x80;    // TX power: 150, 200, 316 mW and their AUTO (151, 201, 317)
        // Said in its info (KA_MSG_INFO), the header's byte being full: 864p144
        // (864p144=1), its OS02K10's 1536x864 window at 144 fps.
        static constexpr uint16_t kAirFeat864p144 = 0x100;
        // And pwr400=1: TX power takes 400 mW (25 dBm) and its AUTO (401).
        static constexpr uint16_t kAirFeatPwr400 = 0x800;
        // No feature bits, only AirInfo::said, for the INFO tab: its frame
        // (KA_MSG_FRAME) came, and its info says the sensor's readout.
        static constexpr uint16_t kAirSaidFrameClock = 0x200;
        static constexpr uint16_t kAirSaidSensorTiming = 0x400;
        // Atomic: reset with the info by the link-down event too (ar_libre's thread).
        std::atomic<uint16_t> air_info_feat_{0};   // those bits, from its info (reset with the info)
        std::atomic<bool> air_info_seen_{false};   // its info has come since the link came up
        // The mode stored for this air unit, if it lacks it: the default asked
        // for. Bits said in the info only once it has come.
        void check_air_modes();
        // Written by the stats thread (poll_ap_time), read by the video thread.
        std::atomic<int64_t> ap_off_us_{0};
        std::atomic<bool>    ap_off_valid_{false};
        // The radio clock read off the radio's frame (radio/frame_clock.hpp):
        // our chip's frame edges, labelled by kestrel-air's KA_MSG_FRAME. The
        // coarse offset above drifts some 20 us a second between its rare
        // re-syncs, and the latency figures with it (RF 7 -> 10 ms in minutes).
        FrameClock frame_clock_;                // the radio clock to ~10-20 us, from the frame
        int64_t fc_min_d_ = INT64_MAX;          // fastest slice on it lately, us (frame count check)
        unsigned fc_n_ = 0, fc_count_gen_ = 0;
        bool fc_fresh_ = false;                 // the first window after a count: acted on alone
        int64_t fc_last_k_ = 0;                 // the window before's verdict (frames off)
        bool fc_wait_ = false;                  // a shift asked for, not applied yet
        unsigned fc_shifts_seen_ = 0;
        uint64_t fc_win_ms_ = 0;
        uint64_t ap_poll_ms_ = 0;
        uint64_t pic_cap_us_ = 0;        // the picture being received: its capture, our clock (0 = unknown)
        bool     pic_air_unknown_ = false;   // ... and the radio clock to time it by not checked yet
        uint64_t air_known_ms_ = 0;          // the last picture with an air figure
        bool     fc_verified_ = false;       // the frame clock's count checked by the slices since it was made
        uint32_t pic_enc_us_ = 0;        // and capture -> its first slice out of the encoder
        uint32_t hdr_height_ = 0, hdr_fps_ = 0;     // the picture size and rate the header says
        // The sensor's readout as the air unit says it (its info, "readout",
        // us; kestrel-air from 2026-10-12): used instead of the learned one,
        // and what splits a slice's Enc into the sensor's part and the rest.
        // 0: not said (or a new mode not said yet).
        std::atomic<uint32_t> air_readout_us_{0};
        std::atomic<uint32_t> air_exposure_us_{0};   // its exposure now ("exposure", us; 0: not said)
        // 5 s windows, us: per picture capture -> encoder out, -> first slice
        // here, -> last slice here (reassembled); per slice, handed to the
        // air unit's radio -> here.
        std::vector<uint32_t> ap_enc_, ap_first_, ap_all_, ap_link_;
        std::vector<uint32_t> ap_link_pos_[3];   // the same by the slice's place: first, middle, last
        std::vector<uint32_t> ap_enc_last_;      // capture -> the picture's last slice out of the encoder
        uint64_t ap_log_ms_ = 0;
        void poll_ap_time();
        void poll_resends();             // Open8030: the air radio's resend map, from the stats thread
        void rx_quality_diag(const FrameStampInfo &si, bool quality, uint64_t t);   // bench: our radio's frame errors every 5 s
        uint64_t resend_poll_ms_ = 0;
        uint32_t resend_done_frame_ = 0;     // our radio's frame of the newest taken
        bool     resend_done_valid_ = false;
        uint32_t frames_done_ = 0;           // the per-frame log: the newest frame written
        struct { uint32_t done = 0, valid = 0, err = 0, unknown = 0, p[4] = {}; bool have = false; uint64_t t0 = 0; } diag_;   // rx_quality_diag
        bool     frames_done_valid_ = false;
        // Its totals since our radio started: blocks sent again, frames known, frames stalled.
        std::atomic<uint32_t> rs_blocks_{0}, rs_known_{0}, rs_stalls_{0};
        void bw_poll();                  // /tmp/bw.req: the video link's bandwidth, an experiment
        void send_caps();                // Open8030: the menu's caps, from the stats thread
        void note_ap_delay(const uint8_t *hdr, size_t len, bool new_pic);
        // An intra refresh stream has no keyframes to join on: ask for one.
        uint64_t idr_asked_ms_ = 0;
        void request_air_idr();
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
        unsigned long long stale_pics = 0;
        // A new clock - the air unit rebooted - also reads as not newer, but
        // runs forward from where it restarted, uninterrupted by live
        // pictures. That many in a row and it is the live stream.
        static constexpr int kStaleAdopt = 16;

        bool connect_bb();
        void subscribe_events();    // stock does this before any socket
        void configure_link();      // candidates + freq + bandwidth
        // Start-streaming frames -> air unit. link_up: a new link (not a
        // re-send after silent video), which is when AUTO picks its channel.
        void send_air_handshake(bool link_up = false);
        void apply_video_mode();    // --ar8030-mode -> SET_CHN_RES
        void probe_video_modes();   // ask the camera which modes it accepts
        void probe_rf_ioctls();     // which BB_SET_* the local radio accepts
        void drain_video_socket();  // discard video while not in the RX loop
        int  wait_sky_status(uint8_t want_cmd, int timeout_ms);
        // 1 accepted, 0 rejected, -1 no ack within timeout_ms.
        int  read_sky_ack(uint8_t want_cmd, int timeout_ms);
        void drain_control_socket();  // keep the port-2 RX ring from overflowing
        void on_ctrl_bytes(const uint8_t *buf, int n);  // the air's reports, MSP, an awaited ack
        void drain_msp_socket();      // FC MSP passthrough -> OSD DisplayPort
        void apply_bandwidth();       // must run after the link is up (see .cpp)
        void send_prj_rf_config();    // BB_SET_PRJ_DISPATCH cmd 138 (see .cpp)
        void replay_stock_rf_setup(); // stock's startup SETs, byte-for-byte
        void set_frame_mode();        // SET 16 = 2 again at each link-up (see .cpp)
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
        void  identify_peer();          // the air unit actually linked, from BB_GET_STATUS
        void  load_sky_config();
        void  save_sky_config();
        // The TX power this air unit last ran with, kept beside its camera
        // settings. The channel is not kept: the air unit decides it.
        void  load_sky_link();
        void  save_sky_link();
        std::vector<uint8_t> build_config_frame(const uint8_t *tmpl, size_t len);
        void apply_pending_settings();
        void apply_pending_rf();
        // A camera setting sent and not acknowledged yet. Its ack is picked
        // out of whatever the control socket brings in (on_ctrl_bytes), read
        // by the main loop between pictures: the video goes on meanwhile. The
        // next setting goes once it is in, or after kSkyAckMs.
        struct SkyAckWait {
            int field = -1;              // -1: none awaited
            int value = 0;
            uint8_t cmd = 0;
            uint64_t until_ms = 0;
            int sends = 0;               // how many times it has gone out
            std::vector<uint32_t> list;  // a work channel list's payload, for sending it again
        };
        static constexpr int kSkyAckMs = 1000;
        // A setting not acknowledged goes again, up to this many sends in
        // all: one lost on the link (a 500mW AUTO, seen) was otherwise gone.
        static constexpr int kSkySends = 3;
        int sky_resend_sends_ = 0;       // the queue's front is a resend, sent this many times (0: not)
        SkyAckWait sky_ack_;
        bool remode_after_angle_ = false;   // the RX thread's: Rotate taken, the mode to send again
        std::deque<std::pair<int,int>> settings_q_;   // parked settings not sent yet, in order
        void sky_ack_done(int status);
        unsigned rf_caps = 0;
        bool ranging_ok = false;
        int  ranging_tries = 0;
        std::vector<std::pair<int,int>> pending_rf;
        std::mutex pending_mtx;
        std::vector<std::pair<int,int>> pending_settings;
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
        bool air_standby = false;      // VTX in low-power/standby mode
        bool air_status_seen = false;  // have we decoded one status frame yet

        // Move the RF link to freq_khz, both ends. Mirrors stock's
        // fpv_bb_set_freq() plus the sky 0x21 it pairs with.
        // Returns 0 on success, negative on failure (frequency not in the radio's table, or an ioctl refused).
        int  set_rf_channel(uint32_t freq_khz, bool hop_en);
        // Undo a pin: every channel back on the ground's work list, then
        // channel adaptation on - the ground searches, as at start. region:
        // AUTO's list is due again (not after AUTO's own move).
        int  search_all_channels(bool region = true);
        int  work_list_all();       // the radio's work list (its scan list too, on Open8030) = every channel
        void chan_follow_poll();    // a manual move under way: ours followed? (read loop, every 100 ms)
        uint32_t chan_follow_khz_ = 0;        // its target, 0: none
        int      chan_follow_stage_ = 0;      // 0 the air unit leads, 1 ours moved itself
        uint64_t chan_follow_start_ms_ = 0, chan_follow_until_ms_ = 0, chan_follow_next_ms_ = 0;
        uint32_t link_freq_khz();   // ground's own TX freq, 0 if unknown

        // Index of freq_khz in the radio's channel table, or -1. Also yields
        // the full table, which BB_SET_WORK_CHAN_LIST needs.
        int  chan_index_for_freq(uint32_t freq_khz, uint32_t *freqs, int *n_out, int32_t *power = nullptr);
        // AUTO / AUTO HOP in a known country, on this thread when region_due:
        // at each link-up, and whenever what the list depends on changes. AUTO:
        // a link on a channel off the list is moved to the quietest on it; at a
        // pick, to that one if clearly quieter. AUTO HOP: only the list and the
        // air unit's own hopping, no move. Without a country, or with nothing
        // allowed, the air unit keeps its channel, as before.
        void region_auto_apply();
        // The radio's channels AUTO may use where the pilot is, in table order (at most 64).
        std::vector<uint32_t> region_list(const uint32_t *freqs, int n);
        // AUTO's choice for a link on cur (0: stay); pick = link-up or AUTO just chosen.
        uint32_t region_auto_target(uint32_t cur, const uint32_t *freqs, const int32_t *power, int n,
                                    const std::vector<uint32_t> &want, bool pick);
        std::vector<uint32_t> region_hs_list_;  // AUTO HOP's list as this link's handshake sent it
        int ka_hs_feat_ = -1;   // kestrel-air settings in this link's handshake, for these features (-1: none)
        std::deque<std::vector<uint32_t>> region_q_lists_;   // each queued CAM_WORK_LIST's payload
        bool region_hop_told_ = false;         // told to hop (kept over a lost link)
        uint64_t region_retry_ms_ = 0;         // a failed move: not again before
        uint64_t region_move_ms_ = 0;          // AUTO's last move
        // Each channel's scan level (dBm, by table index; 0: none yet) taken
        // while the link was up and NOT on it: the link's own channel reads
        // our own link (and, while searching, the air unit's beacons), so its
        // live figure says nothing about what else is there. None for 6 s
        // after a link-up or a channel move (chan_settle_ms_).
        std::atomic<int> free_level_[128] = {};
        std::atomic<uint64_t> chan_settle_ms_{0};
    public:
        // What the air unit announced; false/0 for the stock air app and until
        // its version message has arrived (every few seconds after link-up).
        bool    air_is_kestrel() const { return air_kestrel_; }
        uint8_t air_features() const { return air_feat_; }
        // Set by the OSD when the scan screen opens, so we poll faster while
        // the user is looking at it (matching the old RPC client's cadence).
        std::atomic<bool> scan_active{false};
    private:
        // (was enter_pair_mode: a blocking 30s loop reachable only from the
        //  command line. Replaced by the bind_* state machine above, which the
        //  menu, the front-panel button and --ar8030-pair all drive.)
        void disconnect_bb();
        void run_replay();
        void consume(const uint8_t* data, size_t len);
        void consume_bytes(const uint8_t* data, size_t len);   // consume() without the flush
        void emit_nal(const uint8_t* nal, size_t len);
        void update_stats(size_t frame_size);
};

#endif
