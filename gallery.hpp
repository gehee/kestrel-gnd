#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The goggle's gallery: Back on the live picture shrinks it into a tile on a
// strip of every recording on the card, the newest next to it, so the strip
// reads oldest to newest from left to right and the live picture is its right
// end. Left and right walk along it; Enter on a recording plays it, Enter on
// the live tile (or Back) goes back to the picture. Down on a recording asks
// whether to delete it; the answer starts on KEEP. Right of the live tile is the
// STATS tile; Enter on it opens the stats screen, where up and down change the
// time window and Back returns to the strip.
//
// This file is the logic only - the key handling, the animation and where
// each tile sits. It draws nothing and touches no GL (osd_gallery.cpp draws),
// which is what lets it be tested off the goggle.
//
// Threading: key() runs on the input thread, everything else on the OSD thread;
// one mutex covers the state, and snapshot() hands the drawing a copy.
class Gallery {
    public:
        enum Key { kUp, kDown, kLeft, kRight, kEnter, kBack };
        enum class Mode { Off, Strip, Playing, Stats };

        struct Item {
            std::string name;       // file name in the DVR folder; "" for the live tile
            double      duration = 0;   // seconds
            uint64_t    size = 0;
            bool        live = false;
            bool        stats = false;      // the stats screen's tile, right of the live one
        };

        struct Rect { float x, y, w, h; };   // centre-based pixels, y up

        // What the drawing needs from one instant.
        struct Snapshot {
            Mode  mode = Mode::Off;
            std::shared_ptr<const std::vector<Item>> items;
            int   sel = 0;
            float scroll = 0;       // the strip's position, in tiles (follows sel)
            float k = 0;            // 0: the live picture fills the screen, 1: the strip
            float play_t = 0;       // 0: the strip, 1: the selected tile fills the screen
            bool  loading = false;  // the card is still being read
            bool  paused = false;
            bool  ended = false;
            float dialog = 0;       // 0..1: the "delete this recording?" question is up
            bool  delete_chosen = false;   // ... and DELETE (not KEEP) is the one lit
            float gap = 0;          // tiles from gap_from on are this many places to the right,
            int   gap_from = 0;     //   closing up after a deletion
            int   stats_window = 0; // 0: the last 10 s, 1: 1 min, 2: 3 min
        };

        Gallery();
        ~Gallery();

        // True from Back on the live picture until the picture is full-screen
        // again, so the OSD knows when to draw the strip instead of the HUD.
        bool active() const;

        // Show the gallery (from the live picture). `recording` is the file the
        // DVR is writing now, which is left out. Starts reading the card.
        void open(const std::string& recording);

        // A key. Only while active(); the return says whether it was used.
        bool key(Key k);

        // Move the animation on to `now_us`. True while something is still
        // moving, which is the OSD's cue to ask for another frame at once.
        bool update(uint64_t now_us);

        Snapshot snapshot() const;

        // What the OSD should be playing: "" for nothing. Changes only on a
        // key, so the OSD compares it against what it is playing.
        std::string play_name() const;
        bool play_paused() const;
        // Seeks asked for since the last call, in milliseconds (signed).
        int  take_seek_ms();
        // The player reached the end of the recording.
        void playback_ended();

        // Where the strip's tiles are, for a screen of w x h: tile `i` of
        // the snapshot, and the rectangle the live picture fills when the
        // gallery is not showing (`full`, normally the whole screen).
        static Rect tile_rect(const Snapshot& s, int i, int w, int h, const Rect& full);
        // The tile geometry that rect is made from.
        static void strip_geometry(int w, int h, float& tile_w, float& tile_h,
                                   float& pitch, float& centre_y);

        // Which item is the live picture's (the one with `live` set).
        static int live_index(const std::vector<Item>& items);

        // "2026-10-03  16:40:12" from a name like fpvOS_20261003_164012.mp4;
        // "" when the name carries no time.
        static std::string stamp_label(const std::string& name);
        // "1:07", "12:34", "1:02:03".
        static std::string duration_label(double seconds);

    private:
        void scan(std::string recording, uint64_t gen);
        void stop_scan();
        void retarget_locked();
        void delete_selected_locked();

        mutable std::mutex m_;
        Mode  mode_ = Mode::Off;
        bool  closing_ = false;
        std::shared_ptr<const std::vector<Item>> items_;
        int   sel_ = 0;
        float scroll_ = 0, k_ = 0, k_target_ = 0, play_t_ = 0, play_target_ = 0;
        bool  loading_ = false;
        std::string play_name_;
        bool  paused_ = false, ended_ = false;
        int   seek_ms_ = 0;
        uint64_t last_us_ = 0;

        std::string recording_;     // the one being written when the gallery opened
        bool  confirming_ = false, confirm_delete_ = false;
        float dialog_t_ = 0, gap_ = 0;
        int   gap_from_ = 0;
        int   stats_window_ = 0;

        std::thread scan_thread_;
        std::atomic<bool> scan_cancel_{false};
        uint64_t scan_gen_ = 0;
};
