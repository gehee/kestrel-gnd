#ifndef WEBSTREAM_HPP
#define WEBSTREAM_HPP

// Live FPV video in a web browser.
//
// A small HTTP server inside kestrel. GET / serves a page that plays the feed;
// GET /stream.mp4 is the feed itself: the air unit's own H.264/H.265 access
// units, wrapped in fragmented MP4 (one fragment per picture) and streamed
// until the client disconnects. Nothing is decoded or re-encoded here - the
// phone's hardware decoder does the work - so the goggle's cost is a copy per
// viewer.
//
// The page plays it through Media Source Extensions (ManagedMediaSource on
// iPhone, iOS 17.1+). Not WebCodecs: that API only exists on HTTPS pages, and
// this is plain HTTP on the goggle's own network.
//
// The video path must never wait on a viewer. feed() only queues a pointer,
// and returns at once when nobody is watching; all muxing and socket I/O runs
// on the server's own thread. A viewer that falls behind skips pictures up to
// the next keyframe instead of backing up the queue.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "common.hpp"

class WebStream {
    public:
        static WebStream& instance();

        // Start serving on `port`. Returns false if the port cannot be bound.
        bool start(int port, VideoCodec codec);
        void stop();

        // The stream's VPS/SPS/PPS, each Annex-B with its start code. A new
        // viewer's muxer is given them once, before its first keyframe.
        void set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps);
        void set_frame_size(int w, int h);

        // One whole access unit (all slices of a picture), Annex-B.
        // seq: the air unit's picture counter (16 bits), -1 when not known; a
        // jump in it is a picture that never reached us.
        void feed(const std::shared_ptr<std::vector<uint8_t>>& au, bool key, int seq = -1);
        // Pictures were lost before they reached us (the radio): every viewer
        // waits for a keyframe, as the ones after them refer to them.
        void gap();

        // A viewer is waiting for a keyframe to start on (just connected, or
        // fell behind). With intra refresh the air unit sends none by itself,
        // so the video source asks it for one while this holds.
        bool wants_keyframe() const;

        // The screen as the goggle shows it, re-encoded (web_source "screen",
        // the default): H.264 from a DVR screen encoder at a bitrate a phone's
        // WiFi carries, with a keyframe every 2 s, sent to the viewers instead
        // of the air unit's stream. The encoder runs while someone watches
        // (DvrRecorder::set_web_watching). header: its SPS/PPS, Annex-B;
        // dur90k: one picture, 90 kHz ticks. A different source (a screen
        // recording's encoder taking over) restarts the viewers on it.
        void set_screen_source(const std::vector<uint8_t>& header, int w, int h, int dur90k);
        // One encoded picture of it, Annex-B. True while a viewer waits for a
        // keyframe (the encoder then makes the next picture one).
        bool feed_screen(const uint8_t* p, size_t n, int dur);

        // A DVR recording (file name in the DVR folder) has just been closed:
        // make its gallery thumbnail now, before anyone opens the gallery.
        void recording_finished(const std::string& name);

    private:
        WebStream() {}
        struct Impl;
        Impl* impl_ = nullptr;
};

#endif
