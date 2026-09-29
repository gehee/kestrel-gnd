// kestrel-splash — minimal DRM/KMS boot splash.
//
// Draws a full-screen image (background.png) with the centred "fpvOS"
// wordmark, so boot shows the product art instead of scrolling console text.
// Runs as the first init step. S70kestrel sends SIGUSR1 once it's about to
// start kestrel-gnd - this drops DRM master (drm/drm.c's modeset_open()
// calls drmSetMaster(), which fails while this still holds it) but keeps
// the last frame on screen, since kestrel-gnd needs a moment to open the
// device, build its own framebuffers and perform its own modeset. It exits
// for real only on SIGTERM, sent by kestrel-gnd itself (drm/drm.c's
// notify_splash_done()) right after that first modeset succeeds - so the
// hand-off is a single atomic buffer swap, this program's last frame
// straight to the OSD's first one, with nothing shown in between. A SIGALRM
// fallback exits anyway after 30s in case kestrel-gnd never gets that far
// (crashes before reaching its own modeset), so a fully broken kestrel-gnd
// still ends in a black screen eventually rather than an orphaned splash
// holding the display forever.
//
// Deliberately tiny: two dumb framebuffers, swapped with a vsync'd DRM page
// flip each tick (no GL, no video - the idle screen's background video only
// starts once kestrel-gnd itself takes over). A single scanned-out buffer
// drawn into live - what this used to do - raced the display controller's
// own read of that same memory: fine for a frame painted once and then held,
// but the progress bar moves every tick, and tore visibly doing so. The
// background + wordmark are painted once into an off-screen "base" surface;
// each tick just blits that into whichever of the two framebuffers is not
// currently on screen and redraws the bar over it, so there is no cairo
// re-layout work in the loop either way.
//
// The bare drmModePageFlip API used here queues one flip for the next
// vblank and delivers a completion event on the DRM fd - drmHandleEvent()
// dispatches it to the handler below, which just clears `pending` so the
// main loop knows the flip already on screen is the one it just drew, not
// the one before it, before drawing the next.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "utils/screen_id.h"
#include <cairo/cairo.h>

static volatile sig_atomic_t g_stop    = 0;
static volatile sig_atomic_t g_handoff = 0;
static void on_signal(int s)  { (void)s; g_stop = 1; }
static void on_handoff(int s) { (void)s; g_handoff = 1; }

typedef struct {
    uint32_t fb_id, handle;
    uint8_t* map;
    size_t   size;
    cairo_surface_t* surf;
    cairo_t* cr;
} FrameBuf;

static int frame_buf_create(int fd, int W, int H, FrameBuf* out) {
    struct drm_mode_create_dumb creq = {0};
    creq.width = W; creq.height = H; creq.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) { perror("kestrel-splash: create dumb"); return -1; }

    uint32_t fb_id;
    if (drmModeAddFB(fd, W, H, 24, 32, creq.pitch, creq.handle, &fb_id)) {
        perror("kestrel-splash: addfb"); return -1;
    }

    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = creq.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) { perror("kestrel-splash: map dumb"); return -1; }
    uint8_t* map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) { perror("kestrel-splash: mmap"); return -1; }
    memset(map, 0, creq.size);

    out->fb_id  = fb_id;
    out->handle = creq.handle;
    out->map    = map;
    out->size   = creq.size;
    out->surf   = cairo_image_surface_create_for_data(map, CAIRO_FORMAT_RGB24, W, H, creq.pitch);
    out->cr     = cairo_create(out->surf);
    return 0;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                               unsigned int usec, void* data) {
    (void)fd; (void)frame; (void)sec; (void)usec;
    *(int*)data = 0;
}

// kestrel-gnd's own screen_mode setting (main.cpp parses the same "WxH@R"
// string from the same file) - which mode it actually requests, not
// whichever one this connector happens to list first. Picking any other
// mode here means kestrel-gnd's first modeset changes the display's timing,
// not just its content, and a timing change means a hardware resync - a
// black flash no amount of care in the buffer hand-off can avoid, since the
// screen genuinely goes dark while the panel relocks to the new signal.
// Falls back to {0,0,0} (caller keeps its own default) if the setting is
// missing or unparsable - matching main.cpp's own fallback to "whatever the
// connector prefers" when screen_mode is unset.
static void read_gnd_screen_mode(uint16_t* w, uint16_t* h, uint32_t* vr) {
    *w = 0; *h = 0; *vr = 0;
    // The screen's own entry, screen_mode_<ID>, as main.cpp reads it: unset
    // is auto. screen_mode is only for a screen with no ID. A mode on trial
    // (screen_mode_try) is not the splash's business: kestrel-gnd drops it at
    // start unless it was a restart, and a restart does not show the splash.
    char id[16], key[40];
    if (screen_id(id, sizeof(id))) snprintf(key, sizeof(key), "screen_mode_%s:", id);
    else snprintf(key, sizeof(key), "screen_mode:");
    FILE* f = fopen("/etc/kestrel/kestrel-gnd.yaml", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) != 0) continue;
        char* p = line + strlen(key);
        while (*p == ' ' || *p == '"') p++;
        unsigned pw, ph, pr;
        if (sscanf(p, "%ux%u@%u", &pw, &ph, &pr) == 3) { *w = pw; *h = ph; *vr = pr; }
        break;
    }
    fclose(f);
}

// Ported from drm/drm.c's modeset_output_create() - not reimplemented from
// scratch, because "pick roughly the same mode" isn't good enough: any
// divergence between the two is a timing change at the exact moment this
// hands off to kestrel-gnd, and a timing change forces the display to
// resync, which is a black flash no buffer-handling care can avoid. Same
// three cases, same order, same tie-breaks:
//   1. screen_mode is set and exactly on offer - use it.
//   2. screen_mode is set but not on offer - the highest refresh at that
//      resolution not exceeding what was asked, else the sink's own
//      preferred mode, else the first.
//   3. screen_mode is unset - auto-select: highest refresh, resolution only
//      as a tie-break between modes tied on refresh.
// Interlaced modes are skipped throughout, same as drm.c.
static int pick_mode_index(drmModeConnector* conn, uint16_t mode_width,
                            uint16_t mode_height, uint32_t mode_vrefresh) {
    if (mode_width > 0 && mode_height > 0 && mode_vrefresh > 0) {
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].hdisplay == mode_width &&
                conn->modes[i].vdisplay == mode_height &&
                conn->modes[i].vrefresh == mode_vrefresh &&
                !(conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE)) {
                return i;
            }
        }
        int best = -1, best_rate = -1, pref = -1;
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) continue;
            if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) pref = i;
            if (conn->modes[i].hdisplay == mode_width &&
                conn->modes[i].vdisplay == mode_height &&
                (int)conn->modes[i].vrefresh <= (int)mode_vrefresh &&
                (int)conn->modes[i].vrefresh > best_rate) {
                best = i; best_rate = conn->modes[i].vrefresh;
            }
        }
        return (best >= 0) ? best : ((pref >= 0) ? pref : 0);
    }

    int fc = 0;
    uint32_t max_refresh = 0, max_area = 0;
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) continue;
        uint32_t refresh = conn->modes[i].vrefresh;
        uint32_t area    = (uint32_t)conn->modes[i].hdisplay * conn->modes[i].vdisplay;
        if (refresh > max_refresh || (refresh == max_refresh && area > max_area)) {
            max_refresh = refresh; max_area = area; fc = i;
        }
    }
    return fc;
}

// The "fpvOS" wordmark - same font, shadow treatment and "OS" colour as
// osd.cpp's render_idle_title()/BACKGROUND-state draw (KESTREL theme's
// accent - hud_theme.cpp: lime; text/data/accent/ground/quiet is the real
// field order, and an earlier version of this picked the "data" field, cyan,
// by mistake). Both halves are static here, same as the real idle screen -
// so the splash and the idle screen it hands off to read as one continuous
// frame right down to the colour, with nothing on the wordmark itself to
// un-match once the OSD takes over.
//
// Size and position are matched to the real idle screen by measurement, not
// by reproducing osd.cpp's GL texture-quad math (that sizes a fixed-aspect
// texture with its own headroom, from a frustum height this program has no
// equivalent of): a capture of the live idle screen at 1920x1080 puts the
// word at 29% of the frame width, vertically centred at 45% down.
static double draw_title(cairo_t* cr, int W, int H) {
    static const char* head = "fpv";
    static const char* tail = "OS";
    char full[8]; snprintf(full, sizeof(full), "%s%s", head, tail);

    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "Chakra Petch SemiBold",
                            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    double fs = 200.0;
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, full, &ext);
    fs *= (W * 0.29) / ext.width;           // scale so "fpvOS" spans ~29% of width
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, full, &ext);

    double cy = H * 0.45;                    // vertical centre of the title
    double tx = (W - ext.width) * 0.5 - ext.x_bearing;
    double ty = cy - ext.height * 0.5 - ext.y_bearing;

    cairo_text_extents_t head_ext;
    cairo_text_extents(cr, head, &head_ext);
    double tail_x = tx + head_ext.x_advance;

    // The shadow is cast by the whole word, so both halves drop it before
    // either is inked - same order render_idle_title uses.
    double sh = fs * 0.018;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.05, 0.65);
    for (double k = 2.0; k >= 1.0; k -= 1.0) {
        cairo_move_to(cr, tx + sh * k, ty + sh * k);      cairo_show_text(cr, head);
        cairo_move_to(cr, tail_x + sh * k, ty + sh * k);  cairo_show_text(cr, tail);
    }
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);              // "fpv" - theme text colour
    cairo_move_to(cr, tx, ty); cairo_show_text(cr, head);
    cairo_set_source_rgb(cr, 0.70, 1.00, 0.00);           // "OS" - theme accent colour
    cairo_move_to(cr, tail_x, ty); cairo_show_text(cr, tail);

    return fs;   // the title's font size sets the progress bar's scale
}

// An indeterminate progress bar: a dim track, and a bright segment sliding
// left to right across it on a loop. `t` is the position within one cycle,
// 0 (segment fully off the left edge) to 1 (fully off the right edge) - it
// slides in from outside the track and back out the other side, same as a
// standard indeterminate bar, so the segment is clipped to the track's own
// rectangle: without that, most of it draws outside the dim background
// meant to bound it, out over the plain photo.
static void draw_progress_sweep(cairo_t* cr, int W, int H, double title_fs, double t) {
    double track_w = title_fs * 2.4;
    double track_h = title_fs * 0.035;
    double track_x = (W - track_w) * 0.5;
    // Midway between the bottom edge and the footbridge in background.png,
    // which sits at ~70% down the frame - measured off a still, not derived
    // from anything in the image itself, so it only holds for this specific
    // background asset.
    double track_y = H * 0.85;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
    cairo_rectangle(cr, track_x, track_y, track_w, track_h);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_rectangle(cr, track_x, track_y, track_w, track_h);
    cairo_clip(cr);

    double seg_w = track_w * 0.36;
    double x = track_x - seg_w + t * (track_w + seg_w);
    cairo_set_source_rgba(cr, 0.70, 1.00, 0.00, 0.95);
    cairo_rectangle(cr, x, track_y, seg_w, track_h);
    cairo_fill(cr);

    cairo_restore(cr);   // drops the clip
}

int main(int argc, char** argv) {
    const char* png_path = (argc > 1) ? argv[1] : "/etc/kestrel/background.png";
    const char* card     = (argc > 2) ? argv[2] : "/dev/dri/card0";

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGUSR1, on_handoff);
    signal(SIGALRM, on_signal);   // safety net - see the header comment

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("kestrel-splash: open card"); return 1; }

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) { fprintf(stderr, "kestrel-splash: no DRM resources\n"); return 1; }

    // Pick the first connected, non-writeback connector - a writeback
    // connector reports itself connected and lists modes too (this board
    // offers one at 1920x1080@67, per drm/drm.c), and it is not a display;
    // picking it here while kestrel-gnd picks the real one would mean the
    // two were never going to agree on a mode no matter what pick_mode_index
    // decided.
    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
            c->connector_type != DRM_MODE_CONNECTOR_WRITEBACK) {
            conn = c; break;
        }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "kestrel-splash: no connected connector\n"); return 1; }

    uint16_t want_w, want_h; uint32_t want_vr;
    read_gnd_screen_mode(&want_w, &want_h, &want_vr);
    drmModeModeInfo mode = conn->modes[pick_mode_index(conn, want_w, want_h, want_vr)];
    int W = mode.hdisplay, H = mode.vdisplay;

    // Resolve a usable CRTC for this connector.
    uint32_t crtc_id = 0;
    drmModeEncoder* enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) crtc_id = enc->crtc_id;
    if (enc) drmModeFreeEncoder(enc);
    if (!crtc_id && res->count_crtcs) crtc_id = res->crtcs[0];

    // Two dumb buffers, swapped by page flip - see the header comment for why.
    FrameBuf fbs[2];
    if (frame_buf_create(fd, W, H, &fbs[0]) || frame_buf_create(fd, W, H, &fbs[1])) return 1;

    // Background + title are painted once into an off-screen base surface;
    // every animation tick just blits that (cheap: one same-size RGB24
    // composite, no re-layout) instead of redrawing the image and text.
    cairo_surface_t* base = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t* bcr = cairo_create(base);

    cairo_surface_t* img = cairo_image_surface_create_from_png(png_path);
    if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
        int iw = cairo_image_surface_get_width(img);
        int ih = cairo_image_surface_get_height(img);
        double sx = (double)W / iw, sy = (double)H / ih;
        double s = sx > sy ? sx : sy;
        cairo_save(bcr);
        cairo_translate(bcr, (W - iw * s) / 2.0, (H - ih * s) / 2.0);
        cairo_scale(bcr, s, s);
        cairo_set_source_surface(bcr, img, 0, 0);
        cairo_paint(bcr);
        cairo_restore(bcr);
    } else {
        cairo_set_source_rgb(bcr, 0, 0, 0);
        cairo_paint(bcr);
    }
    if (img) cairo_surface_destroy(img);

    double title_fs = draw_title(bcr, W, H);
    cairo_surface_flush(base);
    cairo_destroy(bcr);

    // Scan out fbs[0] first - the loop below always draws into whichever
    // buffer is NOT on screen, so it must know which one that is.
    int shown = 0;
    drmModeSetCrtc(fd, crtc_id, fbs[shown].fb_id, 0, 0, &conn->connector_id, 1, &mode);

    drmEventContext evctx = {0};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = page_flip_handler;

    // Paced by vsync alone (the page-flip wait below), not a fixed sleep -
    // stacking both would pace against two different clocks, and since the
    // wait already blocks until the next vblank, an added sleep on top of it
    // would only slow the loop down, not smooth it out further. One cycle
    // takes ~1.4s at the display's own refresh rate. Runs until init stops
    // us (SIGTERM) - the CRTC config and dumb buffers live only as long as
    // this process keeps the DRM fd open, so we must not exit early.
    const double refresh_hz  = mode.vrefresh > 0 ? (double)mode.vrefresh : 60.0;
    const int    cycle_ticks = (int)(1.4 * refresh_hz);
    int tick = 0;
    int dropped_master = 0;
    while (!g_stop) {
        if (g_handoff) {
            // kestrel-gnd is about to try for DRM master - let go of it, but
            // keep the fd, both buffers and the current CRTC config exactly
            // as they are, so the screen keeps showing this last frame
            // rather than going black. See the header comment for the rest
            // of the handshake.
            if (!dropped_master) { drmDropMaster(fd); alarm(30); dropped_master = 1; }
            pause();
            continue;
        }

        FrameBuf* next = &fbs[1 - shown];
        cairo_set_source_surface(next->cr, base, 0, 0);
        cairo_paint(next->cr);
        double t = (double)(tick % cycle_ticks) / cycle_ticks;
        draw_progress_sweep(next->cr, W, H, title_fs, t);
        cairo_surface_flush(next->surf);

        int pending = 1;
        if (drmModePageFlip(fd, crtc_id, next->fb_id, DRM_MODE_PAGE_FLIP_EVENT, &pending) == 0) {
            // Wait for the flip to land at the next vblank before touching
            // either buffer again - a bounded wait so a missed event (there
            // shouldn't be one) can't hang the splash forever instead of
            // handing off to kestrel-gnd.
            while (pending && !g_stop) {
                fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
                struct timeval tv = { 0, 200000 };
                int r = select(fd + 1, &fds, NULL, NULL, &tv);
                if (r > 0 && FD_ISSET(fd, &fds)) drmHandleEvent(fd, &evctx);
                else break;
            }
            shown = 1 - shown;
        } else {
            // Flip queued while one from a previous tick is still pending,
            // or some other transient failure - fall back to an immediate
            // modeset so a frame still reaches the screen instead of
            // silently stalling the animation. This path has no vsync wait
            // of its own, so it needs an explicit sleep - otherwise a
            // persistent failure here would spin the loop at full CPU.
            drmModeSetCrtc(fd, crtc_id, next->fb_id, 0, 0, &conn->connector_id, 1, &mode);
            shown = 1 - shown;
            usleep(1000000 / (unsigned)refresh_hz);
        }

        tick++;
    }

    // Leave the final frame on screen for the OSD hand-off; just unmap.
    cairo_surface_destroy(base);
    munmap(fbs[0].map, fbs[0].size);
    munmap(fbs[1].map, fbs[1].size);
    return 0;
}
