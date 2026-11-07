#ifndef DVR_H
#define DVR_H

#include <string>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <vector>
#include <cassert>
#include <cstdio>
#include <unistd.h>

#include "third_party/minimp4.h"
#include "dvr/settings.hpp"
#include "base/video_codec.hpp"
#include "dvr/h264_cfr.hpp"
#include "platform/platform.hpp"
#include "base/bus.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <sstream>
#include <thread>
#include <functional>


enum DvrFormat {
    RAW,
    MP4,
    FMP4
};

#include "dvr/writer.hpp"
#include "dvr/recording_target.hpp"

class DVR {
    private:
        std::atomic<bool>* should_stop;
        
        FILE *dvr_file;
        std::unique_ptr<DvrWriter> writer;   // the muxer's writes to dvr_file
        // set_card()'s, and the writer's word that the card stopped taking
        // the recording (nothing is muxed after it).
        std::string card_dir_;
        std::function<void(const char*)> on_write_failure_;
        std::atomic<bool> write_failed_{false};
        void make_writer();
        DvrFormat format;

        MP4E_mux_t *mux;
        // A muxer still open (init failed after opening it, or the thread
        // never ran): closed, and its writer with it. run_dvr nulls mux after
        // its own close, so this never closes one twice.
        void close_mux() {
            if (mux) {
                MP4E_close(mux);
                mp4_h26x_write_close(&mp4wr);
                mux = nullptr;
            }
            writer.reset();
        }
        mp4_h26x_writer_t mp4wr;

        std::mutex mtx;
        std::condition_variable cv;
        // Each entry carries the time it was queued. Sources queue a picture
        // the moment it is complete, so those times are the stream's real
        // cadence, and they become the samples' durations.
        // A picture is the radio's pool buffer itself (pic), or a copy of it
        // the queue owns (data): parameter sets, repaired pictures, and
        // pictures taken while the pool runs low.
        struct Queued {
            std::shared_ptr<std::vector<uint8_t>> data;
            bus::PictureRef pic;
            uint64_t us;
            const uint8_t* bytes() const { return pic ? pic->data() : data->data(); }
            size_t size() const { return pic ? pic->len : data->size(); }
        };
        std::queue<Queued> dvrQueue;
        size_t   queued_bytes = 0;     // in dvrQueue (under mtx)
        uint64_t queue_dropped = 0;    // pictures it had no room for (under mtx)
        bool     queue_take(const uint8_t* p, size_t n);   // under mtx: room for it?
        bool     is_param_set(const uint8_t* p, size_t n) const;
        uint64_t samples_written = 0;  // pictures the muxer accepted
        uint64_t samples_dropped = 0;  // pictures it refused (no VPS/SPS/PPS yet)
        VideoCodec codec;
        int video_framerate;

        Settings& settings_;   // a screen recording's bitrate (dvr_screen_kbps)
        std::string filename;
        bool record_screen;
        int64_t frame_pts;
        bool should_exit;
        // Screen recording's encoder, from the HAL (on the VRX Pro the RK3568
        // VEPU through MPP: ffmpeg there only offers h264_v4l2m2m, with no
        // /dev/video* node to open).
        std::unique_ptr<hal::ScreenEncoder> screen_enc = platform::make_screen_encoder();
        // Holds the screen recording at the display's refresh rate: scan-outs
        // that showed no new picture become skip frames (see h264_cfr.hpp).
        H264Cfr screen_cfr;
        std::vector<uint8_t> pkt_copy_;   // encoder output, copied to cached memory
        int     screen_fps = 60;
        display::Display* disp_dev = nullptr;
        // The screen rebuilt by the HAL's compositor (RGA on the VRX Pro) from
        // what the display scanned out, every rga_div-th refresh.
        int  rga_div = 2;
        // One slot of the recording in 90 kHz ticks: rga_div refreshes of the
        // screen's real rate, not of the rounded fps (144 Hz at 30 fps is a
        // slot every 5 refreshes, 28.8 fps).
        int  rga_slot_ticks = 1500;
        std::unique_ptr<hal::ScreenCompositor> rga = platform::make_screen_compositor();
        void run_screen_rga();
        // A screen encoder that writes no file: the web view's (to_file false).
        bool to_file = true;
        int  enc_bitrate = 0;    // bps, 0: the recording's 16 Mbit/s
        std::string comment_;    // set_comment()'s
        std::string comment_with(const char* kind, int w, int h, int fps) const;
        int  enc_max_bps = 0;    // > 0: variable rate, never above this
        int  enc_fps_cap = 60;   // the screen sampled at most this often
        double screen_fps_exact = 0;   // a file's slots a second, as measured (it may be stamped a usual rate)
        // The RGA recorder's slots: composited pictures (idx into the
        // encoder's inputs, -1 for the screen of the slot before), queued
        // from the sampler to the encoding thread.
        struct Shot { int idx; int64_t slot; };
        std::mutex qm;
        std::condition_variable qcv;
        std::deque<Shot> shots;
        std::deque<int> free_bufs;
        // Holders of each encoder input (under qm): the composite on its way
        // to the encoder, the latest one kept for a follower, the follower.
        int  buf_refs[hal::ScreenEncoder::kInputBuffers] = {0};
        void release_buf(int idx);
        // Following another RGA recorder (the web view's encoder while a
        // screen recording runs - the screen has one sampler, and RGA no time
        // for a second composite): no sampler and no composite of its own.
        // Every follow_k-th slot the leader hands it its latest composite,
        // which this encoder reads as it is (one picture, two streams).
        DVR* leader = nullptr;
        int  follow_k = 1;
        std::mutex follow_m;     // the leader's: guards follower, follower_draining
        DVR* follower = nullptr;
        bool follower_draining = false;   // unhooked, still reading our inputs
        std::condition_variable follow_cv;
        int  f_held = 0;         // the leader's, under qm: its inputs the follower holds
        void push_shot(int idx, int64_t slot);   // follower, from the leader's encoding thread
        void follower_done(int idx);             // leader: the follower has read input idx
        // The sink waits for a keyframe: the next slot is composited and
        // encoded even if the screen is unchanged (a still screen otherwise
        // makes only skip frames, and no keyframe for a new viewer).
        std::atomic<bool> want_fresh{false};
        // Every encoded picture also goes here (the web view), as written.
        // It answers true while it waits for a keyframe: the next picture is
        // then encoded as one (at most once a second).
        std::mutex sink_m;
        std::function<bool(const uint8_t*, size_t, int)> sink;
        bool     sink_wants_key = false;
        uint64_t last_forced_idr_us = 0;
        void to_sink(const uint8_t* d, size_t len, int dur) {
            std::lock_guard<std::mutex> lk(sink_m);
            if (sink) { sink_wants_key = sink(d, len, dur); want_fresh = sink_wants_key; }
        }
        void idr_if_wanted();

        static std::string format_timestamped_filename(const std::string& base_filename) {
            if (base_filename.empty()) return "";
            size_t dot_idx = base_filename.find_last_of('.');
            std::string base = base_filename;
            std::string ext = "";
            if (dot_idx != std::string::npos) {
                base = base_filename.substr(0, dot_idx);
                ext = base_filename.substr(dot_idx);
            }
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            struct tm buf;
            localtime_r(&in_time_t, &buf);
            std::stringstream ss;
            ss << base << "_" << std::put_time(&buf, "%Y%m%d_%H%M%S") << ext;
            return ss.str();
        }

    public:
        // stamp false: filename_ is the name to use, already timestamped
        // (timestamped()) - DvrRecorder names the file before it exists.
        DVR(Settings& settings, std::string filename_, VideoCodec codec_, int framerate_, DvrFormat fmt_ ,std::atomic<bool>* stop_signal, bool record_screen_ = false, bool to_file_ = true, bool stamp = true)
            : settings_(settings), filename(to_file_ ? (stamp ? format_timestamped_filename(filename_) : filename_) : std::string()), codec(codec_), video_framerate(framerate_), format(fmt_), should_stop(stop_signal), record_screen(record_screen_), to_file(to_file_) {
            dvr_file = nullptr;
            mux = nullptr;
            frame_pts = 0;
            should_exit = false;
            // Both modes write through minimp4 now, so both need the FILE*.
            // This used to be guarded by !record_screen because screen
            // recording went through ffmpeg's avio_open instead; with the MPP
            // encoder the muxer writes to this file (through a DvrWriter) and a
            // null one segfaults inside DVR::init().
            if (to_file && (dvr_file = fopen(filename.c_str(), "w")) == NULL) {
                printf("ERROR: unable to open %s\n", filename.c_str());
            }
        }

        // run_dvr closes the file itself; this is for a DVR whose thread never
        // ran (init() failed, the thread could not start).
        // The muxer first: it writes through the DvrWriter into dvr_file.
        ~DVR() {
            close_mux();
            if (dvr_file) fclose(dvr_file);
        }
        // A recording given up before its thread ran: the file closed and
        // removed, as if REC had not been pressed.
        void discard() {
            close_mux();
            if (dvr_file) { fclose(dvr_file); dvr_file = nullptr; }
            if (to_file && !filename.empty()) ::unlink(filename.c_str());
        }

        // DvrRecorder reports the open file's path in the OSD.
        const std::string& path() const { return filename; }
        // "<dir>/fpvOS.mp4" as "<dir>/fpvOS_YYYYMMDD_HHMMSS.mp4", now.
        static std::string timestamped(const std::string& base) { return format_timestamped_filename(base); }
        // Before init(): the recordings' folder, kept with room on the card
        // while this file is written (DvrWriter), and who to tell - on the
        // writer's thread - when the card stops taking it. --dvr: neither.
        void set_card(const std::string& dir, std::function<void(const char* why)> on_fail) {
            card_dir_ = dir;
            on_write_failure_ = std::move(on_fail);
        }
        // The card stopped taking this recording: what reached it ends early.
        bool write_failed() const { return write_failed_; }
        // Before init(): the screen encoder's bitrate and highest frame rate.
        // max_bps > 0: variable rate under that ceiling.
        void set_encoding(int bitrate_bps, int fps_cap, int max_bps = 0) {
            enc_bitrate = bitrate_bps; enc_max_bps = max_bps; enc_fps_cap = fps_cap > 0 ? fps_cap : 60;
        }
        // Before init(): the highest frame rate the screen is sampled at.
        void set_fps_cap(int fps) { if (fps > 0) enc_fps_cap = fps; }
        // Before init(): take the screen from l's sampler (an RGA recorder
        // already running) instead of sampling it.
        void set_leader(DVR* l) { leader = l; }
        // Every encoded picture of a screen encoder also to fn (nullptr: none).
        void set_sink(std::function<bool(const uint8_t*, size_t, int)> fn) {
            std::lock_guard<std::mutex> lk(sink_m);
            sink = std::move(fn);
            sink_wants_key = sink != nullptr;   // a new viewer starts on a keyframe
            want_fresh = sink_wants_key;
        }
        // After init(): the encoder's SPS/PPS (Annex-B), size and frame period.
        const std::vector<uint8_t>& screen_header() const { return screen_enc->header(); }
        int screen_width() const { return screen_enc->width(); }
        int screen_height() const { return screen_enc->height(); }
        int screen_period_ticks() const { return rga_slot_ticks; }

        int init(int frm_width, int frm_height);
        void run_dvr();
        void stop();
        // us: the picture's time - its capture stamp where the source has
        // one, on any one clock (the samples' durations are its differences),
        // else 0 for now.
        void enqueueDvrPacket(std::shared_ptr<std::vector<uint8_t>> frame, uint64_t us = 0);
        void enqueueDvrPacket(const bus::PictureRef& pic, uint64_t us);
        // The display a screen recording samples (vblanks, the screen tap).
        void set_display_dev(display::Display* d) { disp_dev = d; }
        // Before init(): text for the file's metadata (its comment, which
        // ffprobe shows as "comment") - what it was recorded with.
        void set_comment(std::string c) { comment_ = std::move(c); }
        // Queue depth, so producers can drop instead of growing it without
        // bound. A screen frame is ~8MB; letting these pile up OOM-killed the
        // process at 637MB RSS.
        size_t queue_depth();

        static void* run_dvr_thread(void* arg) {
            DVR* instance = static_cast<DVR*>(arg);
            instance->run_dvr();
            return nullptr;
        }
};

// Runtime recording control.
//
// DVR itself is construct-to-open / stop-to-close: it opens its file in the
// constructor and its thread drains a queue until stop(). That was fine when
// recording was a launch flag, but the rec button has to start and stop it
// while the app runs, so this owns the DVR's lifetime and gives the video
// sources one stable place to hand frames to.
//
// Frames are dropped (cheaply) when not recording, so sources can call feed()
// unconditionally.
class DvrRecorder : public RecordingTarget {
    public:
        static DvrRecorder& instance();

        // Called once at startup with everything needed to build a DVR later.
        void configure(Settings& settings, VideoCodec codec, int framerate, DvrFormat fmt,
                       std::atomic<bool>* stop_signal, const std::string& dir);

        bool toggle();                       // returns the new recording state
        bool is_recording() const;
        std::string current_file() const;    // basename of the open file, or ""
        // The last recording that ended by itself, the card no longer taking
        // it: why ("CARD FULL", "CARD ERROR") and when (get_time_us), or ""
        // if none has. For the HUD to say why REC went off.
        std::string last_failure(uint64_t* at_us = nullptr) const;
        void feed(std::shared_ptr<std::vector<uint8_t>> frame, uint64_t us = 0);
        // A picture in the radio's pool: queued as it is, no copy - or copied
        // while the pool runs low, so a slow card cannot starve the radio.
        void feed(const bus::PictureRef& pic, uint64_t us);

        // The muxer needs the frame size at open time. The legacy --dvr flow
        // got it from Renderer calling DVR::init() on a DVR that already
        // existed; a button-started DVR is created after the video is already
        // running, so the size has to be remembered here instead.
        void set_frame_size(int w, int h);

        // Screen mode: REC captures the composited screen (FPV + OSD) through
        // the hardware encoder instead of muxing the FPV elementary stream.
        // A mode on the one recorder, not a second recorder.
        void set_screen_mode(bool on);
        // Screen recordings are the size of the DISPLAY, not of the decoded
        // video. set_frame_size() is fed by the renderer with the video's
        // dimensions; using those to size the encoder while the capture buffer
        // is screen-sized read past the end of it and segfaulted in memcpy.
        void set_screen_size(int w, int h);
        // The display screen recordings and the web view sample (its vblanks
        // and screen tap). Set by main() once the DRM device is up.
        void set_display(display::Display* dev);
        // Once, at start: the build, for each recording's metadata.
        void set_build(std::string b) { std::lock_guard<std::mutex> lk(m_); build_ = std::move(b); }
        bool screen_mode() const;

        // VPS/SPS/PPS for the stream. A recording started mid-stream begins at
        // an IDR with no parameter sets ahead of it, so the muxer writes an
        // empty hvcC and the file decodes to black. These are replayed into the
        // queue first when recording starts.
        void set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps);
        void shutdown();                     // stop and join if recording
        // The web view wants the screen encoded (a phone is watching): a
        // screen encoder of its own runs, writing no file - or, while a
        // screen recording runs, the recording's feeds it. Any thread.
        void set_web_watching(bool on);
        // Set up the web view's encoder again if it is wanted but missing
        // (it could not start: the screen not known yet, no encoder).
        void web_retry();
        // The web view's ceiling, kbit/s (web_kbps, default 5000): its encoder
        // stays under it, and each viewer's socket is paced at it.
        int web_cap_kbps();
        // The phone view (web/webstream), set by main() before anything is
        // recorded: a recording finished (its gallery thumbnail is made then);
        // the web view's screen encoder, its stream's header and size, then
        // each frame it encodes (false: not taken).
        struct WebHooks {
            std::function<void(const std::string& file)> recording_finished;
            std::function<void(const std::vector<uint8_t>& header, int w, int h, int dur90k)> screen_source;
            std::function<bool(const uint8_t* p, size_t n, int dur)> feed_screen;
        };
        void set_web_hooks(WebHooks hooks) { web_hooks_ = std::move(hooks); }

    private:
        DvrRecorder() {}
        bool start_recording();
        void stop_recording();
        // stop_recording()'s second half, on a thread of its own.
        void finish_recording(std::shared_ptr<DVR> dvr, pthread_t tid, const std::string& file);
        // The writer's: the card stopped taking recording number seq.
        void recording_failed(uint64_t seq, const char* why);
        // Under ctl_: the web view's encoder to match web_want_ and the recording.
        void web_reconcile();
        void start_web(const std::shared_ptr<DVR>& leader);
        void stop_web();
        void attach_web(DVR* d);
        WebHooks             web_hooks_;
        bool                 web_want_ = false;
        bool                 shut_ = false;         // shutdown(): no encoder starts after it
        bool                 rec_screen_ = false;   // the running recording is a screen one
        std::shared_ptr<DVR> web_dvr_;
        std::shared_ptr<DVR> web_leader_;           // the recording web_dvr_ follows, if any
        pthread_t            web_tid_ = 0;

        std::mutex           ctl_;    // one start or stop at a time (see start_recording)
        mutable std::mutex   m_;      // the fields below, briefly
        std::shared_ptr<DVR> dvr_;
        pthread_t            tid_ = 0;
        std::atomic<bool>    running_{false};
        std::string          file_;
        // A stopped recording still being written out (finish_recording);
        // finished_cv_ says when it is done.
        bool                 finishing_ = false;
        std::condition_variable finished_cv_;
        uint64_t             seq_next_ = 0;      // under ctl_: recordings started
        uint64_t             running_seq_ = 0;   // the running one's number
        std::string          failure_;           // last_failure()'s
        uint64_t             failure_us_ = 0;

        Settings*     settings_ = nullptr;   // configure()'s: the recordings' and the web view's
        std::string   build_;                // set_build()'s
        // What a recording is made with, for its file's metadata: the build,
        // the recording, the screen and Refresh Sync as they are now, and
        // every setting.
        std::string recording_info(display::Display* disp);
        VideoCodec    codec_ = VideoCodec::H265;
        int           fps_   = 60;
        DvrFormat     fmt_   = DvrFormat::MP4;
        std::atomic<bool>* stop_ = nullptr;
        std::string   dir_   = "/media/dvr";
        bool          configured_ = false;
        int           fw_ = 1920, fh_ = 1080;  // frame size handed to the muxer
        std::atomic<bool> screen_{false};      // record the screen, not the FPV stream
        int sw_ = 0, sh_ = 0;                  // display size for screen recordings
        display::Display* disp_dev_ = nullptr;   // the display, once it is up
        std::vector<std::vector<uint8_t>> ps_;  // VPS/SPS/PPS, replayed on start
        // Whether this recording has been given ps_ yet. A recording started
        // before the stream's parameter sets were first seen gets them ahead
        // of the first picture after they arrive, instead of never - the
        // muxer drops every picture until it has them, and the file used to
        // close with no video in it at all.
        bool ps_sent_ = false;
};

#endif

