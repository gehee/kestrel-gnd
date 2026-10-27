#ifndef WEBSTREAM_HPP
#define WEBSTREAM_HPP

// Live FPV video in a web browser.
//
// A small HTTP server inside kestrel. GET / serves a page that plays the feed;
// GET /stream.mp4 is the feed itself: the screen as the goggle shows it, HUD
// included, re-encoded to H.264 by a DVR screen encoder at a rate a phone's
// WiFi carries, wrapped in fragmented MP4 (one fragment per picture) and
// streamed until the client disconnects.
//
// The page plays it through Media Source Extensions (ManagedMediaSource on
// iPhone, iOS 17.1+). Not WebCodecs: that API only exists on HTTPS pages, and
// this is plain HTTP on the goggle's own network.
//
// The encoder must never wait on a viewer. feed_screen() only queues a
// pointer, and returns at once when nobody is watching; all muxing and socket I/O runs
// on the server's own thread. A viewer that falls behind skips pictures up to
// the next keyframe instead of backing up the queue.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/common.hpp"

class WebStream {
    public:
        static WebStream& instance();

        // Start serving on `port`. Returns false if the port cannot be bound.
        bool start(int port);
        void stop();

        // The screen as the goggle shows it, re-encoded: H.264 from a DVR
        // screen encoder at a bitrate a phone's WiFi carries, with a keyframe
        // every 2 s. The encoder runs while someone watches
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
