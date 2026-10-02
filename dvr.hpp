#ifndef DVR_H
#define DVR_H

#include <string>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <vector>
#include <cassert>

#include "utils/minimp4.h"
#include "common.hpp"
#include "utils/mpp_encoder.hpp"
#include "utils/h264_cfr.hpp"
#include "rga_compositor.hpp"

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

// How often a writeback screen recording (dvr_capture: writeback, or no RGA)
// captures the screen, from dvr_screen_fps (default 60, at most 60). Each
// capture is an extra commit on the CRTC that presents the video, whose flips
// are refused while it is in flight, so fewer cost the live picture less. The
// RGA recorder costs it nothing and always records 60 frames a second (every
// other refresh at 120 Hz); the menu offers screen recording at 60 only.
int dvr_screen_capture_fps();

// What REC records when the settings do not say (dvr_screen): the screen -
// video and HUD as the pilot sees them - 60 times a second.
constexpr bool kDvrScreenDefault    = true;
constexpr int  kDvrScreenFpsDefault = 60;

enum DvrFormat {
    RAW,
    MP4,
    FMP4
};

// The DVR's file writes, on a thread of their own.
//
// The muxer writes each picture as it is handed one, on the thread that takes
// the pictures. Writing there meant the once-a-second fsync - which puts the
// recording on the card, so a power cut loses a second at most - held that
// thread for as long as the card took, 50 to 250 ms on the DVR partition. A
// screen recording captures into a few buffers only, and with none coming
// back the captures in between were dropped: the recording froze for a few
// frames every second. Here the muxer's writes are only queued; this thread
// writes them, in order, and does the syncing.
class DvrWriter {
    public:
        explicit DvrWriter(int fd);
        ~DvrWriter();       // writes out everything queued, then syncs
        // Queue a write; 1 once any write has failed, as minimp4 expects.
        int write(int64_t offset, const void* buf, size_t size);
    private:
        void run();
        struct Chunk { int64_t offset; std::vector<uint8_t> data; };
        const int fd_;
        std::mutex m_;
        std::condition_variable work_, room_;
        std::deque<Chunk> q_;
        size_t queued_ = 0;     // bytes in q_
        bool done_ = false;
        std::atomic<bool> failed_{false};
        std::thread t_;
};

class DVR {
    private:
        volatile bool *should_stop;
        
        FILE *dvr_file;
        std::unique_ptr<DvrWriter> writer;   // the muxer's writes to dvr_file
        DvrFormat format;

        MP4E_mux_t *mux;
        mp4_h26x_writer_t mp4wr;

        std::mutex mtx;
        std::condition_variable cv;
        // Each entry carries the time it was queued. Sources queue a picture
        // the moment it is complete, so those times are the stream's real
        // cadence, and they become the samples' durations.
        struct Queued { std::shared_ptr<std::vector<uint8_t>> data; uint64_t us; };
        std::queue<Queued> dvrQueue;
        uint64_t last_sample_us = 0;   // previous picture's queue time
        uint64_t samples_written = 0;  // pictures the muxer accepted
        uint64_t samples_dropped = 0;  // pictures it refused (no VPS/SPS/PPS yet)
        VideoCodec codec;
        int video_framerate;

        std::string filename;
        bool record_screen;
        int64_t frame_pts;
        bool should_exit;
        // Screen recording encodes on the RK3568 VEPU through MPP. ffmpeg on
        // this board only offers h264_v4l2m2m and there are no /dev/video*
        // nodes, so the ffmpeg encoder path can never open.
        MppH264Encoder screen_enc;
        // Holds the screen recording at the display's refresh rate: scan-outs
        // that showed no new picture become skip frames (see h264_cfr.hpp).
        H264Cfr screen_cfr;
        std::vector<uint8_t> pkt_copy_;   // encoder output, copied to cached memory
        int     screen_fps = 60;
        bool use_writeback = false;   // frames arrive already composited
        class DrmDevice* wb_dev = nullptr;
        // The RGA recorder (the default, dvr_capture: rga): the screen rebuilt
        // from what the display scanned out, every rga_div-th refresh.
        bool use_rga = false;
        int  rga_div = 2;
        // One slot of the recording in 90 kHz ticks: rga_div refreshes of the
        // screen's real rate, not of the rounded fps (144 Hz at 30 fps is a
        // slot every 5 refreshes, 28.8 fps).
        int  rga_slot_ticks = 1500;
        RgaCompositor rga;
        void run_screen_rga();

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
        DVR(std::string filename_, VideoCodec codec_, int framerate_, DvrFormat fmt_ ,volatile bool* stop_signal, bool record_screen_ = false) 
            : filename(format_timestamped_filename(filename_)), codec(codec_), video_framerate(framerate_), format(fmt_), should_stop(stop_signal), record_screen(record_screen_) {
            dvr_file = nullptr;
            frame_pts = 0;
            should_exit = false;
            // Both modes write through minimp4 now, so both need the FILE*.
            // This used to be guarded by !record_screen because screen
            // recording went through ffmpeg's avio_open instead; with the MPP
            // encoder the muxer writes to this file (through a DvrWriter) and a
            // null one segfaults inside DVR::init().
            if ((dvr_file = fopen(filename.c_str(), "w")) == NULL) {
                printf("ERROR: unable to open %s\n", filename.c_str());
            }
        }

        // DvrRecorder reports the open file's path in the OSD.
        const std::string& path() const { return filename; }

        int init(int frm_width, int frm_height);
        void run_dvr();
        void stop();
        void enqueueDvrPacket(std::shared_ptr<std::vector<uint8_t>> frame);
        // Composited capture: the display controller writes NV12 straight into
        // the encoder's buffer, so nothing is copied and the video plane is
        // included (glReadPixels only ever saw the OSD layer).
        bool using_writeback() const { return use_writeback; }
        void set_writeback_dev(class DrmDevice* d) { wb_dev = d; }
        void run_screen();
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
class DvrRecorder {
    public:
        static DvrRecorder& instance();

        // Called once at startup with everything needed to build a DVR later.
        void configure(VideoCodec codec, int framerate, DvrFormat fmt,
                       volatile bool* stop_signal, const std::string& dir);

        bool toggle();                       // returns the new recording state
        bool is_recording() const;
        std::string current_file() const;    // basename of the open file, or ""
        void feed(std::shared_ptr<std::vector<uint8_t>> frame);

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
        // Capture composited frames via the DRM writeback connector instead of
        // glReadPixels. Set by main() once the DRM device is up.
        void set_writeback(class DrmDevice* dev);
        bool screen_mode() const;

        // VPS/SPS/PPS for the stream. A recording started mid-stream begins at
        // an IDR with no parameter sets ahead of it, so the muxer writes an
        // empty hvcC and the file decodes to black. These are replayed into the
        // queue first when recording starts.
        void set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps);
        void shutdown();                     // stop and join if recording

    private:
        DvrRecorder() {}
        bool start_recording();
        void stop_recording();

        std::mutex           ctl_;    // one start or stop at a time (see start_recording)
        mutable std::mutex   m_;      // the fields below, briefly
        std::shared_ptr<DVR> dvr_;
        pthread_t            tid_ = 0;
        std::atomic<bool>    running_{false};
        std::string          file_;

        VideoCodec    codec_ = VideoCodec::H265;
        int           fps_   = 60;
        DvrFormat     fmt_   = DvrFormat::MP4;
        volatile bool* stop_ = nullptr;
        std::string   dir_   = "/media/dvr";
        bool          configured_ = false;
        int           fw_ = 1920, fh_ = 1080;  // frame size handed to the muxer
        std::atomic<bool> screen_{false};      // record the screen, not the FPV stream
        int sw_ = 0, sh_ = 0;                  // display size for screen recordings
        class DrmDevice* wb_dev_ = nullptr;    // non-null once writeback is usable
        std::vector<std::vector<uint8_t>> ps_;  // VPS/SPS/PPS, replayed on start
        // Whether this recording has been given ps_ yet. A recording started
        // before the stream's parameter sets were first seen gets them ahead
        // of the first picture after they arrive, instead of never - the
        // muxer drops every picture until it has them, and the file used to
        // close with no video in it at all.
        bool ps_sent_ = false;
};

#endif

