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
        void feed(const std::shared_ptr<std::vector<uint8_t>>& au, bool key);

        // A DVR recording (file name in the DVR folder) has just been closed:
        // make its gallery thumbnail now, before anyone opens the gallery.
        void recording_finished(const std::string& name);

    private:
        WebStream() {}
        struct Impl;
        Impl* impl_ = nullptr;
};

#endif
