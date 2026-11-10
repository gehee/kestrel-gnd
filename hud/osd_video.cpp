// What the renderer tells the OSD (display::VideoListener): the video's
// figures and each picture's latency; and the link figures it keeps.
#include "hud/osd_detail.hpp"

// Console Color Definitions
#define RESET       "\033[0m"
#define BOLDCYAN    "\033[1m\033[36m"
#define BOLDYELLOW  "\033[1m\033[33m"
#define BOLDGREEN   "\033[1m\033[32m"
#define BOLDMAGENTA "\033[1m\033[35m"
#define BLUE        "\033[34m"
#define GREEN       "\033[32m"
#define YELLOW      "\033[33m"
#define RED         "\033[31m"
#define CYAN        "\033[36m"

static std::string get_braille_char(unsigned char dots) {
    // Unicode Braille Patterns start at U+2800 (represented in UTF-8 as E2 A0 80)
    std::string s;
    s += (char)0xE2;
    s += (char)(0xA0 | (dots >> 6));
    s += (char)(0x80 + (dots & 0x3F));
    return s;
}

// Helper for console graphing
static void print_metric_line(const char* label, double value, const char* unit, double max_scale, int width, const char* label_color, const char* bar_color, const char* value_color) {
    int fill = (int)((value / max_scale) * width);
    if (fill > width) fill = width;
    if (fill < 0) fill = 0;

    printf("%s%-5s%s [", label_color, label, RESET);
    printf("%s", bar_color);
    for (int i = 0; i < width; i++) {
        if (i < fill) printf("█");
        else printf(" ");
    }
    printf("%s] %s%6.2f %s%s\n", RESET, value_color, value, unit, RESET);
}

static void print_latency_dist(const std::vector<float>& values, double avg, double min, double max, double max_scale, int width, const char* bar_color) {
    std::vector<int> bins(width, 0);
    int max_bin_count = 0;
    if (!values.empty()) {
        for (float v : values) {
            int bin = (int)((v / max_scale) * width);
            if (bin >= width) bin = width - 1;
            if (bin < 0) bin = 0;
            bins[bin]++;
            if (bins[bin] > max_bin_count) max_bin_count = bins[bin];
        }
    }

    printf("Dist: [");
    printf("%s", bar_color);
    // Fixed: blocks[1] was a space, now it's a 1/8th block.
    const char* blocks[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (int i = 0; i < width; i++) {
        if (bins[i] == 0) {
            printf(" ");
        } else {
            int level = (bins[i] * 8) / max_bin_count;
            if (level > 8) level = 8;
            
            if (level == 0 && bins[i] > 0) {
                // Outlier/Single-frame marker
                printf("."); 
            } else {
                printf("%s", blocks[level]);
            }
        }
    }
    printf("%s] Avg: %s%6.2f%s | Max: %s%6.2f%s | Min: %s%6.2f%s\n", RESET, YELLOW, avg, RESET, RED, max, RESET, GREEN, min, RESET);
}

void OSD::set_video_resolution(int w, int h) {
    pthread_mutex_lock(&osd_mutex);
	osd_vars.video_width = w;
	osd_vars.video_height = h;
    pthread_mutex_unlock(&osd_mutex);
}


void OSD::set_slices_received(bool val) {
    pthread_mutex_lock(&osd_mutex);
    osd_vars.slices_received = val;
    pthread_mutex_unlock(&osd_mutex);
}

bool OSD::get_slices_received() {
    pthread_mutex_lock(&osd_mutex);
    bool val = osd_vars.slices_received;
    pthread_mutex_unlock(&osd_mutex);
    return val;
}






// Append one row correlating the RF link (RX MCS/throughput/SNR) and the result
// (received video Mbps, latency, loss, frame rate, display cadence). Pull
// /tmp/kestrel-adapt.csv after a run to graph it — debuggable without a network
// to the air. (The air's adaptation columns - tier, applied bitrate, water level -
// went: nothing here receives them any more.)
void OSD::log_csv_row() {
    FILE* fp = (FILE*)csv_fp;
    // A row a second is ~400 KB an hour, and /tmp is RAM: past 2 MB it starts
    // over, the 2 MB before kept as .1.
    if (fp && ftell(fp) > 2L * 1024 * 1024) {
        fclose(fp);
        csv_fp = fp = nullptr;
        rename("/tmp/kestrel-adapt.csv", "/tmp/kestrel-adapt.csv.1");
    }
    if (!fp) {
        fp = fopen("/tmp/kestrel-adapt.csv", "w");
        csv_fp = fp;
        if (fp)
            fprintf(fp, "t_ms,video_mbps,rx_mbps,rx_mcs,snr_db,lost,lat_avg_ms,lat_max_ms,fps,"
                        "gap_p99_ms,gap_max_ms,stutters,ldpc_err\n");
    }
    if (fp) {
        // Render cadence from hardware flip completions — the objective "did the
        // viewer see a hiccup" measure. stutters is cumulative; p99/max cover the
        // rolling ~2s gap window.
        RenderCadence rc = dev ? dev->get_render_cadence() : RenderCadence{0, 0, 0};
        fprintf(fp, "%llu,%.2f,%.2f,%d,%.1f,%u,%.1f,%.1f,%d,%.1f,%.1f,%u,%.4f\n",
            (unsigned long long)get_time_ms(),
            osd_vars.video_bandwidth,
            osd_vars.radio_link.rx_data_rate_kbps / 1000.0f,
            osd_vars.radio_link.rx_mcs_val < 0 ? -1 : ar_mcs_label(osd_vars.radio_link.rx_mcs_val),
            osd_vars.radio_link.snr,
            parts_.radio.state().frames_lost,
            osd_vars.total_latency_avg,
            osd_vars.total_latency_max,
            osd_vars.current_framerate,         // shown here (the renderer's count)
            rc.p99_ms,
            rc.max_ms,
            rc.stutters,
            osd_vars.radio_link.fec_error);
        fflush(fp);
    }
}

void OSD::update_stats(int current_framerate, latency_stats stats) {
    pthread_mutex_lock(&osd_mutex);
	osd_vars.proc_latency_avg = stats.proc_latency.avg;
	osd_vars.proc_latency_max = stats.proc_latency.max;
	osd_vars.proc_latency_min = stats.proc_latency.min;
    osd_vars.proc_latency_values = stats.proc_latency.values;

	osd_vars.decoding_latency_avg = stats.decoding_latency.avg;
	osd_vars.decoding_latency_max = stats.decoding_latency.max;
	osd_vars.decoding_latency_min = stats.decoding_latency.min;
    osd_vars.decoding_latency_values = stats.decoding_latency.values;

	osd_vars.display_latency_avg = stats.display_latency.avg;
	osd_vars.display_latency_max = stats.display_latency.max;
	osd_vars.display_latency_min = stats.display_latency.min;
    osd_vars.display_latency_values = stats.display_latency.values;

	osd_vars.tx_latency_avg = stats.tx_latency.avg;
	osd_vars.tx_latency_max = stats.tx_latency.max;
	osd_vars.tx_latency_min = stats.tx_latency.min;
    osd_vars.tx_latency_values = stats.tx_latency.values;


	osd_vars.total_latency_avg = stats.total_latency.avg;
	osd_vars.total_latency_max = stats.total_latency.max;
	osd_vars.total_latency_min = stats.total_latency.min;
    osd_vars.total_latency_values = stats.total_latency.values;

	osd_vars.frame_pace_avg = stats.frame_pace.avg;
	osd_vars.frame_pace_max = stats.frame_pace.max;
	osd_vars.frame_pace_min = stats.frame_pace.min;
    osd_vars.frame_pace_values = stats.frame_pace.values;


    osd_vars.proc_latency_worst = stats.proc_latency_worst;
    osd_vars.decoding_latency_worst = stats.decoding_latency_worst;
    osd_vars.display_latency_worst = stats.display_latency_worst;
    osd_vars.tx_latency_worst = stats.tx_latency_worst;
    osd_vars.latency_history.push_back(stats.total_latency.avg);
    if (osd_vars.latency_history.size() > 50) osd_vars.latency_history.erase(osd_vars.latency_history.begin());

	osd_vars.current_framerate = current_framerate;
    pthread_mutex_unlock(&osd_mutex);

    if (console_stats) {
        printf("\033[H\033[2J");

        const double MAX_LATENCY_SCALE = 45.0; 
        const int GRAPH_BAR_WIDTH = 40;        

        printf(BOLDCYAN "--- Performance Metrics ---\n" RESET);
        printf(BOLDYELLOW "Current Framerate: %s%s%d FPS%s\n", RESET, BOLDGREEN, current_framerate, RESET);
        printf(BOLDYELLOW "Video Bandwidth:   %s%s%.2f Mbps%s\n", RESET, BOLDGREEN, osd_vars.video_bandwidth, RESET);

        const char* link_color = RESET;
        if (osd_vars.link_stats.p_lost_rate > 0) link_color = RED;
        else if (osd_vars.link_stats.p_fec_recovered_rate > 0) link_color = YELLOW;
        
        printf(BOLDYELLOW "Link Stats:        %s%s%.2f Mbps | PPS: %u | FEC %d:%d | Loss: %u | Recovered: %u%s\n", 
            RESET, link_color,
            osd_vars.video_bandwidth, osd_vars.link_stats.p_all_rate, 
            osd_vars.link_stats.fec_k, osd_vars.link_stats.fec_n,
            osd_vars.link_stats.p_lost_rate, osd_vars.link_stats.p_fec_recovered_rate,
            RESET);
        printf("\n");

        printf(BOLDMAGENTA "RF - encoder out to the slice here (ms):%s\n", RESET);
        print_latency_dist(osd_vars.proc_latency_values, osd_vars.proc_latency_avg, osd_vars.proc_latency_min, osd_vars.proc_latency_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, BOLDCYAN);

        printf(BOLDMAGENTA "Decoding - the slice here to its rows decoded (ms):%s\n", RESET);
        print_latency_dist(osd_vars.decoding_latency_values, osd_vars.decoding_latency_avg, osd_vars.decoding_latency_min, osd_vars.decoding_latency_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, BOLDYELLOW);
        
        printf(BOLDMAGENTA "\nDisplay (VSync Aging) (ms):%s\n", RESET);
        print_latency_dist(osd_vars.display_latency_values, osd_vars.display_latency_avg, osd_vars.display_latency_min, osd_vars.display_latency_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, RED);
        
        printf(BOLDMAGENTA "\nCapture -> encoder out (ms):%s\n", RESET);
        print_latency_dist(osd_vars.tx_latency_values, osd_vars.tx_latency_avg, osd_vars.tx_latency_min, osd_vars.tx_latency_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, CYAN);
        
        printf(BOLDMAGENTA "\nGlass-to-glass estimate (ms):%s\n", RESET);
        print_latency_dist(osd_vars.total_latency_values, osd_vars.total_latency_avg, osd_vars.total_latency_min, osd_vars.total_latency_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, GREEN);

        printf(BOLDMAGENTA "\nFrame Pace (ms per frame):%s\n", RESET);
        print_latency_dist(osd_vars.frame_pace_values, osd_vars.frame_pace_avg, osd_vars.frame_pace_min, osd_vars.frame_pace_max, MAX_LATENCY_SCALE, GRAPH_BAR_WIDTH, GREEN);

        int width_chars = 80;
        int height_chars = 6;
        float max_ms = 70.0f; 
        std::vector<LatencyFrame> frames;
        pthread_mutex_lock(&osd_mutex);
        drain_latency_frames();
        osd_vars.latency_ring.copy_to(frames, current_framerate > 0 ? current_framerate * 5 : 600);
        pthread_mutex_unlock(&osd_mutex);

        if (!frames.empty()) {
            printf(BOLDCYAN "\nLatency History Timeline (Braille, 5s Window):\n" RESET);
            int num_samples = (current_framerate > 0) ? current_framerate * 5 : 600;
            std::vector<LatencyFrame> data;
            if ((int)frames.size() >= num_samples) {
                data.assign(frames.end() - num_samples, frames.end());
            } else {
                data = frames;
                while ((int)data.size() < num_samples) data.insert(data.begin(), {0,0,0,0,0,0});
            }

            for (int r = height_chars - 1; r >= 0; --r) {
                printf("%3.0fms %s│", (max_ms / height_chars) * (r + 1), RESET);
                float row_lat_base = (max_ms / height_chars) * r;

                for (int c = 0; c < width_chars; ++c) {
                    unsigned char dots = 0;
                    const char* sample_color = RESET;
                    
                    for (int sub_c = 0; sub_c < 2; ++sub_c) {
                        int dot_column = c * 2 + sub_c;
                        int sample_idx = (dot_column * (int)data.size()) / (width_chars * 2);
                        if (sample_idx >= (int)data.size()) sample_idx = (int)data.size() - 1;

                        const auto& f = data[sample_idx];
                        float total = f.processing_ms + f.net_ms + f.dec_ms + f.disp_ms;
                        float total_dots = (total / max_ms) * (height_chars * 4);
                        
                        int row_start_dot = r * 4;
                        for (int d = 0; d < 4; ++d) {
                            if (total_dots > (row_start_dot + d)) {
                                if (sub_c == 0) {
                                     if (d == 0) dots |= 0x40; else if (d == 1) dots |= 0x04; else if (d == 2) dots |= 0x02; else if (d == 3) dots |= 0x01;
                                } else {
                                     if (d == 0) dots |= 0x80; else if (d == 1) dots |= 0x20; else if (d == 2) dots |= 0x10; else if (d == 3) dots |= 0x08;
                                }
                                
                                float cur = 0;
                                if (row_lat_base < (cur += f.processing_ms)) sample_color = CYAN;
                                else if (row_lat_base < (cur += f.net_ms)) sample_color = BOLDCYAN;
                                else if (row_lat_base < (cur += f.dec_ms)) sample_color = BOLDYELLOW;
                                else sample_color = RED;
                            }
                        }
                    }
                    if (dots == 0) printf(" ");
                    else printf("%s%s%s", sample_color, get_braille_char(dots).c_str(), RESET);
                }
                printf("\n");
            }
            printf("        └");
            for (int i = 0; i < width_chars; ++i) printf("─");
            printf("\n");
            printf("Legend: %sCap%s %sEnc%s %sNet%s %sDec%s %sDisp%s\n", BOLDGREEN, RESET, CYAN, RESET, BOLDCYAN, RESET, BOLDYELLOW, RESET, RED, RESET);
        }

        printf(BOLDCYAN "---------------------------\n" RESET);
        fflush(stdout);
    }
}

// The renderer, every picture: into latency_in_, no lock (see drain_latency_frames).
void OSD::add_latency_frame(LatencyFrame frame) {
    static uint64_t last_frame_time_us = 0;
    static float smoothed_pace = 0.0f;
    uint64_t now_us = get_time_us();
    
    bool is_dummy = (frame.processing_ms == 0.0f && 
                     frame.net_ms == 0.0f && frame.dec_ms == 0.0f && frame.disp_ms == 0.0f);
    if (is_dummy) {
        frame.pace_ms = 0.0f;
        last_frame_time_us = 0;
        smoothed_pace = 0.0f;
    } else {
        float raw_pace = 0.0f;
        if (last_frame_time_us != 0) {
            raw_pace = (now_us - last_frame_time_us) / 1000.0f;
        }
        last_frame_time_us = now_us;
        
        if (smoothed_pace == 0.0f) {
            smoothed_pace = raw_pace;
        } else {
            smoothed_pace = 0.08f * raw_pace + 0.92f * smoothed_pace;
        }
        frame.pace_ms = smoothed_pace;
    }
    
    // Snapshot current bitrate alongside the latency frame: the link as our
    // radio has it, read without a lock.
    const gnd::RadioState ls = parts_.radio.state();
    const gnd::LinkStats &a = ls.link;
    const bool linked = a.state == 2;
    frame.video_mbps = ls.video_mbps;
    frame.rf_mbps = (float)a.rx_data_rate_kbps / 1000.0f;
    frame.t_us = now_us;
    frame.lost = ls.frames_lost;
    frame.snr = (int8_t)std::max(-127.0f, std::min(127.0f, a.snr));
    frame.mcs = (uint8_t)std::max(0, std::min(255, a.rx_mcs_val));
    frame.bw_idx = linked && a.bw_step >= 0 && a.bw_step < 6 ? (int8_t)a.bw_step : (int8_t)-1;
    frame.air_dbm = linked ? (uint8_t)std::max(0, std::min(39, air_live_dbm())) : 0;
    frame.freq_mhz = linked ? (uint16_t)std::max(0, a.tx_freq) : 0;
    frame.dist_m = linked ? (int16_t)std::max(-1, std::min(32767, ls.dist_m)) : (int16_t)-1;
    if (frame.dist_m < 0) frame.dist_m = -1;

    latency_in_.push(frame);    // full (the HUD's loop stuck for 8 s at 120 fps; it wakes at least each second): dropped, counted
    if (!is_dummy) latency_frames_total_.fetch_add(1, std::memory_order_relaxed);
}

// Under osd_mutex: the renderer's latency frames into the ring the HUD reads.
void OSD::drain_latency_frames() {
    LatencyFrame f;
    while (latency_in_.pop(f)) {
        osd_vars.latency_ring.push(f);
        osd_vars.latency_history_last_us = f.t_us;
    }
}

// True if a decoded FPV frame arrived within the last 1.5s — the "pipeline is
// actually producing pictures" signal for the reconnect retry in main.
bool OSD::video_decoding() {
    pthread_mutex_lock(&osd_mutex);
    uint64_t last = last_fpv_frame_us_;
    pthread_mutex_unlock(&osd_mutex);
    return last != 0 && (get_time_us() - last) < 1500000ULL;
}

// The renderer, every picture. Live video takes no lock here: only the
// reveal (the idle screen still up) needs osd_mutex.
void OSD::notify_video_frame(bool is_keyframe) {
    uint64_t now = get_time_us();
    // The frames that change what the HUD shows: video coming back after a
    // stall (the lock screen, stale-picture dimming and the red rails all key
    // off frame freshness), and the one that confirms video on this link.
    const uint64_t prev_frame_us = last_fpv_frame_us_.exchange(now);
    bool edge = prev_frame_us == 0 || now - prev_frame_us > 400000;
    if (frames_since_connect_.load(std::memory_order_relaxed) < ~0u &&
        frames_since_connect_.fetch_add(1, std::memory_order_relaxed) + 1 == kFramesForVideo)
        edge = true;
    if (video_state_ == VideoState::BACKGROUND) {
        pthread_mutex_lock(&osd_mutex);
        // The first connection's warp into live video waits for a decent
        // picture: a link comes up on 5 MHz and widens from there, and its
        // first second or two of video (a Mbit/s or so) is a smear that the
        // zoom would land on. Decoding goes on underneath the idle screen;
        // the warp starts once the video runs above kRevealMbps - or 2 below
        // the Max Bitrate cap when that is lower - or kRevealTimeoutUs after
        // the first clean picture if it never does.
        // Pictures stopping for a while (the link lost before the reveal)
        // start the wait over.
        if (reveal_wait_us_ && now - prev_frame_us > 1500000) reveal_wait_us_ = 0;
        const int cap_kbps = kArMaxBrVals[menu_ar_maxbr];   // 0 = AUTO, no cap
        const float reveal_mbps = cap_kbps > 0 ? std::min(kRevealMbps, cap_kbps / 1000.0f - 2.0f) : kRevealMbps;
        if (is_keyframe && !reveal_wait_us_) {
            reveal_wait_us_ = now;
            printf("[OSD] First clean keyframe decoded - live FPV once the video is above %.0f Mbit/s\n",
                   reveal_mbps);
        }
        const float mbps = osd_vars.video_bandwidth;
        const bool good = mbps > reveal_mbps;
        if (reveal_wait_us_ && (good || now - reveal_wait_us_ >= kRevealTimeoutUs)) {
            video_state_         = VideoState::TRANSITION;
            transition_start_us_ = now;
            printf("[OSD] Starting transition to live FPV after %.1f s: video %.1f Mbit/s%s\n",
                   (now - reveal_wait_us_) / 1e6, mbps, good ? "" : " (timed out)");
            edge = true;
        }
        pthread_mutex_unlock(&osd_mutex);
    }
    if (edge) {
        render_requested = true;
        hud_wake_.poke();
    }
}
