#include "gallery.hpp"

#include <algorithm>
#include <pthread.h>

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dvr_library.hpp"

namespace {

// How the motion settles: each value closes the gap to its target by 1 - e^(-dt/tau)
// a frame, so it is quick to start and eases in, whatever the frame rate.
constexpr float kTauZoom   = 0.09f;   // the live picture shrinking into / filling from its tile
constexpr float kTauScroll = 0.07f;   // the strip following the selection
constexpr float kTauPlay   = 0.10f;   // a recording growing to the full screen
constexpr float kTauDialog = 0.05f;   // the delete question fading in
constexpr float kTauGap    = 0.07f;   // the strip closing up after a deletion
constexpr int   kSeekMs    = 5000;

float follow(float v, float target, float dt, float tau) {
    v += (target - v) * (1.0f - std::exp(-dt / tau));
    return std::fabs(target - v) < 0.002f ? target : v;
}

Gallery::Rect lerp(const Gallery::Rect& a, const Gallery::Rect& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
             a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

} // namespace

// What the strip holds before the card has been read: the live tile and, right
// of it, the stats tile.
std::shared_ptr<std::vector<Gallery::Item>> live_and_stats() {
    auto v = std::make_shared<std::vector<Gallery::Item>>();
    v->push_back(Gallery::Item{"", 0, 0, true, false});
    v->push_back(Gallery::Item{"", 0, 0, false, true});
    return v;
}

Gallery::Gallery() {
    items_ = live_and_stats();
}

int Gallery::live_index(const std::vector<Item>& items) {
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].live) return (int)i;
    return 0;
}

Gallery::~Gallery() { stop_scan(); }

bool Gallery::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return mode_ != Mode::Off;
}

void Gallery::stop_scan() {
    scan_cancel_ = true;
    if (scan_thread_.joinable()) scan_thread_.join();
}

void Gallery::open(const std::string& recording) {
    stop_scan();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (mode_ != Mode::Off && !closing_) return;   // already open
        items_ = live_and_stats();
        mode_ = Mode::Strip;
        closing_ = false;
        sel_ = 0;
        scroll_ = 0;
        recording_ = recording;
        confirming_ = confirm_delete_ = false;
        dialog_t_ = gap_ = 0;
        k_ = 0;
        k_target_ = 1;
        play_t_ = play_target_ = 0;
        play_name_.clear();
        paused_ = ended_ = false;
        seek_ms_ = 0;
        loading_ = true;
        last_us_ = 0;
        gen = ++scan_gen_;
    }
    scan_cancel_ = false;
    scan_thread_ = std::thread([this, recording, gen] { scan(recording, gen); });
}

// Read the card: every recording that plays, oldest first, then the live tile
// and the stats tile.
// Off the OSD thread, since each recording is opened to find how long it is.
void Gallery::scan(std::string recording, uint64_t gen) {
    pthread_setname_np(pthread_self(), "gallery-scan");
    std::vector<dvr_lib::Recording> recs = dvr_lib::list_recordings();   // newest first
    auto items = std::make_shared<std::vector<Item>>();
    const std::string folder = dvr_lib::dir();
    for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
        if (scan_cancel_) return;
        // .mp4 only (a raw .h265 has no container to play), and not the one
        // being written: its length is not there yet.
        if (!dvr_lib::ends_with(it->name, ".mp4") || it->name == recording) continue;
        const double dur = dvr_lib::mp4_duration(folder + "/" + it->name);
        if (dur <= 0) continue;
        items->push_back(Item{it->name, dur, it->size, false});
    }
    // The thumbnails nearest the live tile are the ones on screen first.
    for (auto it = items->rbegin(); it != items->rend(); ++it)
        dvr_lib::request_thumbnail(it->name);
    items->push_back(Item{"", 0, 0, true, false});
    items->push_back(Item{"", 0, 0, false, true});

    std::lock_guard<std::mutex> lk(m_);
    if (gen != scan_gen_ || scan_cancel_) return;
    const bool on_live = (*items_)[sel_].live, on_stats = (*items_)[sel_].stats;
    items_ = items;
    loading_ = false;
    if (on_live || on_stats) {             // nobody moved off these: stay on the same tile
        sel_ = on_live ? live_index(*items_) : (int)items_->size() - 1;
        scroll_ = (float)sel_;
    }
}

void Gallery::retarget_locked() {
    // Closing brings the live tile to the middle.
    sel_ = live_index(*items_);
    closing_ = true;
    k_target_ = 0;
}

bool Gallery::key(Key k) {
    std::lock_guard<std::mutex> lk(m_);
    if (mode_ == Mode::Off) return false;
    const int last = (int)items_->size() - 1;

    if (mode_ == Mode::Stats) {
        switch (k) {
            case kUp:   stats_window_ = std::max(0, stats_window_ - 1); break;   // closer in
            case kDown: stats_window_ = std::min(2, stats_window_ + 1); break;   // further back
            case kBack:
                play_target_ = 0;
                mode_ = Mode::Strip;
                break;
            default: break;
        }
        return true;
    }

    if (mode_ == Mode::Playing) {
        switch (k) {
            case kLeft:                                  // from the end, back into the recording and playing
                seek_ms_ -= kSeekMs;
                if (ended_) paused_ = false;
                ended_ = false;
                break;
            case kRight: if (!ended_) seek_ms_ += kSeekMs; break;   // nothing past the end
            case kEnter:
                if (ended_) { seek_ms_ = INT_MIN / 2; ended_ = false; paused_ = false; }   // replay
                else paused_ = !paused_;
                break;
            case kBack:
                play_name_.clear();
                play_target_ = 0;
                paused_ = ended_ = false;
                seek_ms_ = 0;
                mode_ = Mode::Strip;
                break;
            default: break;
        }
        return true;
    }

    // The strip. With the delete question up, it has the keys.
    if (confirming_) {
        switch (k) {
            case kLeft:  confirm_delete_ = false; break;
            case kRight: confirm_delete_ = true; break;
            case kEnter:
                if (confirm_delete_) delete_selected_locked();
                confirming_ = false;
                break;
            default: confirming_ = false; break;      // Back, up, down: keep it
        }
        return true;
    }
    switch (k) {
        case kLeft:  if (!closing_) sel_ = std::max(0, sel_ - 1); break;
        case kRight: if (!closing_) sel_ = std::min(last, sel_ + 1); break;
        case kEnter:
            if (closing_) break;
            if ((*items_)[sel_].live) { retarget_locked(); }
            else if ((*items_)[sel_].stats) {
                play_target_ = 1;                        // the screen grows from its tile
                mode_ = Mode::Stats;
            } else {
                play_name_ = (*items_)[sel_].name;
                play_target_ = 1;
                paused_ = ended_ = false;
                seek_ms_ = 0;
                mode_ = Mode::Playing;
            }
            break;
        case kBack: retarget_locked(); break;
        case kDown:
            // Only a recording; the live and stats tiles have nothing to delete.
            if (!closing_ && !loading_ && sel_ < last && !(*items_)[sel_].live && !(*items_)[sel_].stats) {
                confirming_ = true;
                confirm_delete_ = false;
            }
            break;
        default: break;
    }
    return true;
}

// Take the selected recording off the strip now, and delete its file on a
// thread of its own (an unlink of a big file on the card can take a while). The
// selection stays where it is, so it lands on the next newer one - or the live
// tile - which slides in as the gap closes.
void Gallery::delete_selected_locked() {
    const std::string name = (*items_)[sel_].name;
    auto items = std::make_shared<std::vector<Item>>(*items_);
    items->erase(items->begin() + sel_);
    items_ = items;
    gap_from_ = sel_;
    gap_ = 1.0f;
    if (sel_ > (int)items_->size() - 1) sel_ = (int)items_->size() - 1;
    const std::string active = recording_;
    std::thread([name, active] {
        pthread_setname_np(pthread_self(), "gallery-del");
        const int rc = dvr_lib::delete_recording(name, active);
        if (rc == 0) printf("gallery: deleted recording %s\n", name.c_str());
        else printf("gallery: could not delete %s (%s)\n", name.c_str(), strerror(rc));
        fflush(stdout);
    }).detach();
}

bool Gallery::update(uint64_t now_us) {
    std::lock_guard<std::mutex> lk(m_);
    if (mode_ == Mode::Off) { last_us_ = 0; return false; }
    float dt = last_us_ ? (float)(now_us - last_us_) / 1e6f : 1.0f / 60.0f;
    last_us_ = now_us;
    if (dt < 0) dt = 0;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;   // a stall is not a leap

    k_      = follow(k_, k_target_, dt, kTauZoom);
    scroll_ = follow(scroll_, (float)sel_, dt, kTauScroll);
    play_t_ = follow(play_t_, play_target_, dt, kTauPlay);
    dialog_t_ = follow(dialog_t_, confirming_ ? 1.0f : 0.0f, dt, kTauDialog);
    gap_ = follow(gap_, 0.0f, dt, kTauGap);

    if (closing_ && k_ <= 0.0f) {
        mode_ = Mode::Off;
        closing_ = false;
        play_t_ = play_target_ = 0;
        return false;
    }
    return k_ != k_target_ || scroll_ != (float)sel_ || play_t_ != play_target_ ||
           dialog_t_ != (confirming_ ? 1.0f : 0.0f) || gap_ != 0.0f;
}

Gallery::Snapshot Gallery::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    Snapshot s;
    s.mode = mode_;
    s.items = items_;
    s.sel = sel_;
    s.scroll = scroll_;
    s.k = k_;
    s.play_t = play_t_;
    s.loading = loading_;
    s.paused = paused_;
    s.ended = ended_;
    s.dialog = dialog_t_;
    s.delete_chosen = confirm_delete_;
    s.gap = gap_;
    s.gap_from = gap_from_;
    s.stats_window = stats_window_;
    return s;
}

std::string Gallery::play_name() const {
    std::lock_guard<std::mutex> lk(m_);
    return play_name_;
}

bool Gallery::play_paused() const {
    std::lock_guard<std::mutex> lk(m_);
    return paused_;
}

int Gallery::take_seek_ms() {
    std::lock_guard<std::mutex> lk(m_);
    const int v = seek_ms_;
    seek_ms_ = 0;
    return v;
}

void Gallery::playback_ended() {
    std::lock_guard<std::mutex> lk(m_);
    if (mode_ == Mode::Playing) { ended_ = true; paused_ = true; }
}

// Tiles are 60% of the screen's width, so the picture only shrinks to a little
// over half its size, and 66% apart: the neighbours either side of the selected
// one show 14% of the screen's width at its edges, which says there is more to
// scroll to.
void Gallery::strip_geometry(int w, int h, float& tile_w, float& tile_h,
                             float& pitch, float& centre_y) {
    tile_w = 0.60f * (float)w;
    tile_h = tile_w * (float)h / (float)w;
    pitch = 0.66f * (float)w;
    centre_y = 0.42f * (float)h;
}

Gallery::Rect Gallery::tile_rect(const Snapshot& s, int i, int w, int h, const Rect& full) {
    float tw, th, pitch, cy;
    strip_geometry(w, h, tw, th, pitch, cy);
    const float shift = (s.gap > 0 && i >= s.gap_from) ? s.gap : 0.0f;
    Rect r{ (float)w * 0.5f + ((float)i - s.scroll + shift) * pitch, cy, tw, th };
    if (s.items && i >= 0 && i < (int)s.items->size() && (*s.items)[i].live)
        r = lerp(full, r, s.k);
    if (i == s.sel && s.play_t > 0)
        r = lerp(r, Rect{ (float)w * 0.5f, (float)h * 0.5f, (float)w, (float)h }, s.play_t);
    return r;
}

std::string Gallery::stamp_label(const std::string& name) {
    for (size_t i = 0; i + 15 <= name.size(); i++) {
        bool ok = name[i + 8] == '_';
        for (size_t k = 0; ok && k < 15; k++)
            if (k != 8 && !isdigit((unsigned char)name[i + k])) ok = false;
        if (!ok) continue;
        const char* p = name.c_str() + i;
        char b[40];
        snprintf(b, sizeof(b), "%.4s-%.2s-%.2s  %.2s:%.2s:%.2s", p, p + 4, p + 6, p + 9, p + 11, p + 13);
        return b;
    }
    return "";
}

std::string Gallery::duration_label(double seconds) {
    const long t = seconds < 0 ? 0 : (long)(seconds + 0.5);
    char b[24];
    if (t >= 3600) snprintf(b, sizeof(b), "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else           snprintf(b, sizeof(b), "%ld:%02ld", t / 60, t % 60);
    return b;
}
