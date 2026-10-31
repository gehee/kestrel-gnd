// The OSD's images - icons, the idle screen's background and title, the
// background clip's - made through its canvas, and each frame handed to the
// HUD's layer.
#include "hud/osd_detail.hpp"
#include "icons/logo.h"   // its data: included here only

// PNG loading into an image
typedef struct {
    const unsigned char* data;
    unsigned int size;
    unsigned int offset;
} cairo_png_reader_t;

cairo_status_t read_png_from_mem(void *closure, unsigned char *data, unsigned int length) {
    cairo_png_reader_t *reader = (cairo_png_reader_t *)closure;
    if (reader->offset + length > reader->size) return CAIRO_STATUS_READ_ERROR;
    memcpy(data, reader->data + reader->offset, length);
    reader->offset += length;
    return CAIRO_STATUS_SUCCESS;
}

render::ImageId OSD::load_image_from_png(const unsigned char* png, unsigned int length) {
    cairo_png_reader_t reader = { png, length, 0 };
    cairo_surface_t *surface = cairo_image_surface_create_from_png_stream(read_png_from_mem, &reader);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Failed to load PNG to Cairo surface\n");
        return 0;
    }

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    render::ImageId tex = canvas_->make_image(w, h, data, true);

    cairo_surface_destroy(surface);
    return tex;
}

static pthread_t        s_prefetch_thread;
static bool             s_prefetch_started = false;
static cairo_surface_t* s_bg_surface    = nullptr; // decoded background.png (null on failure)
// The wordmark, in two layers. "fpv" is the subject and stays in the type
// colour; "OS" takes the theme's accent - the same hue the HUD spends on live
// state, which is the one colour on this screen that already means something
// is running.
//
// Two textures rather than one coloured bitmap, because the accent is not
// known at render time: it follows whichever theme is selected, and a theme is
// chosen by looking at it. Both layers hold the whole wordmark's geometry with
// only their own half inked, so drawing them over each other reassembles the
// word exactly - no second layout to keep in step with the first.
static cairo_surface_t* s_title_surface    = nullptr; // "fpv"
static cairo_surface_t* s_title_os_surface = nullptr; // "OS"
static const int        s_title_w = 2048, s_title_h = 256;
static const char*      kTitleHead = "fpv";
static const char*      kTitleTail = "OS";

static void render_idle_title(cairo_surface_t* surf, int part) {
    cairo_t* cr = cairo_create(surf);
    // The face the rest of the OSD is drawn in, so the app's name is set in
    // the same voice as everything it says.
    cairo_select_font_face(cr, "Chakra Petch SemiBold",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    char full[16];
    snprintf(full, sizeof(full), "%s%s", kTitleHead, kTitleTail);

    // Fit to the surface rather than trust a point size: the size that suited
    // a seven-letter name is not the size that suits a five-letter one, and
    // the quad on screen is sized from this texture's aspect.
    double size = 168.0;
    cairo_text_extents_t all;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, full, &all);
    if (all.width > 1.0) {
        size *= (s_title_w * 0.50) / all.width;
        cairo_set_font_size(cr, size);
        cairo_text_extents(cr, full, &all);
    }
    if (all.height > s_title_h * 0.52) {
        size *= (s_title_h * 0.52) / all.height;
        cairo_set_font_size(cr, size);
        cairo_text_extents(cr, full, &all);
    }

    // Centre the whole word, then place each half where the whole word put it.
    double tx = (s_title_w - all.width)  * 0.5 - all.x_bearing;
    double ty = (s_title_h - all.height) * 0.5 - all.y_bearing;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    if (part == 0) {
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, kTitleHead);
    } else {
        cairo_text_extents_t head;
        cairo_text_extents(cr, kTitleHead, &head);
        cairo_move_to(cr, tx + head.x_advance, ty);
        cairo_show_text(cr, kTitleTail);
    }
    cairo_destroy(cr);
}

static std::string s_assets_dir = "/etc/kestrel";
void osd_set_assets_dir(const std::string& dir) { s_assets_dir = dir; }
std::string osd_asset(const char* name) { return s_assets_dir + "/" + name; }

static void* osd_prefetch_func(void*) {
    const std::string png = osd_asset("background.png");
    cairo_surface_t* s = cairo_image_surface_create_from_png(png.c_str());
    if (s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
        s_bg_surface = s;
    } else {
        if (s) cairo_surface_destroy(s);
        fprintf(stderr, "[OSD] prefetch: cannot decode %s\n", png.c_str());
    }
    for (int part = 0; part < 2; part++) {
        cairo_surface_t* ts = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                        s_title_w, s_title_h);
        if (ts && cairo_surface_status(ts) == CAIRO_STATUS_SUCCESS) {
            render_idle_title(ts, part);
            (part == 0 ? s_title_surface : s_title_os_surface) = ts;
        } else if (ts) {
            cairo_surface_destroy(ts);
        }
    }
    return nullptr;
}

void osd_prefetch_assets_async() {
    if (s_prefetch_started) return;
    if (pthread_create(&s_prefetch_thread, nullptr, osd_prefetch_func, nullptr) == 0)
        s_prefetch_started = true; // init_images() joins; falls back to sync load otherwise
}

// An already-decoded Cairo ARGB32 surface as an image (load_image_from_file's
// parameters).
static render::ImageId image_from_cairo_surface(render::Canvas* canvas_, cairo_surface_t* surface) {
    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    render::ImageId tex = canvas_->make_image(w, h, data, true);
    return tex;
}

void OSD::init_images() {
    // The buffers the frames go to (the HUD's layer); none, nothing is drawn.
    layer_ready_ = layer_ && layer_->init();
    if (!layer_ready_) return;

    // Initialize base icons (small embedded PNGs).
    fps_tex  = load_image_from_png(framerate_icon, framerate_icon_length);
    lat_tex  = load_image_from_png(latency_icon, latency_icon_length);
    net_tex  = load_image_from_png(bandwidth_icon, bandwidth_icon_length);
    logo_tex = load_image_from_png(kestrel_logo_png, kestrel_logo_png_len);
    clock_tex = load_image_from_png(clock_icon, clock_icon_length);

    // background.png: consume the copy decoded by the prefetch thread (which
    // overlapped EGL init); fall back to a synchronous load if unavailable.
    if (s_prefetch_started) { pthread_join(s_prefetch_thread, nullptr); s_prefetch_started = false; }
    if (s_bg_surface) {
        bg_tex = image_from_cairo_surface(canvas_.get(), s_bg_surface);
        cairo_surface_destroy(s_bg_surface);
        s_bg_surface = nullptr;
    } else {
        bg_tex = load_image_from_file(osd_asset("background.png").c_str());
    }

    // Software decode's image (BGRA - the fallback)
    uint32_t black = 0xFF000000u;
    bg_video_tex_ = canvas_->make_image(1, 1, &black);

    // Hardware decode's video image: each picture taken as it is (zero-copy)
    bg_ext_tex_ = canvas_->make_video();

    // The gallery's recording playback gets one of its own, so the idle
    // background and a recording never share a picture.
    clip_ext_tex_ = canvas_->make_video();
    live_ext_tex_ = canvas_->make_video();

    // Idle title textures — the "fpvOS" wordmark rendered at high resolution so
    // it stays crisp when scaled up on screen (no magnification blur from the
    // small draw_text cache surface). One layer per half of the word, so the
    // accent on "OS" can follow the theme without re-rendering anything.
    // Prefer the copies rendered by the prefetch thread.
    for (int part = 0; part < 2; part++) {
        cairo_surface_t*& src = (part == 0) ? s_title_surface : s_title_os_surface;
        cairo_surface_t* surf = src;
        src = nullptr;
        if (!surf) { // prefetch unused/failed — render synchronously now
            surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s_title_w, s_title_h);
            render_idle_title(surf, part);
        }

        unsigned char* data = cairo_image_surface_get_data(surf);
        render::ImageId& tex = (part == 0) ? idle_title_tex_ : idle_title_os_tex_;
        tex = canvas_->make_image(s_title_w, s_title_h, data);
        idle_title_w_ = s_title_w;
        idle_title_h_ = s_title_h;
        cairo_surface_destroy(surf);
    }

    if (bg_player_ && bg_video_enabled) bg_player_->start();
}

render::ImageId OSD::load_image_from_file(const char* filename) {
    cairo_surface_t *surface = cairo_image_surface_create_from_png(filename);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Failed to load PNG file %s to Cairo surface\n", filename);
        return 0;
    }

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    render::ImageId tex = canvas_->make_image(w, h, data, true);

    cairo_surface_destroy(surface);
    return tex;
}

// The end of a frame, the HUD's and the gallery's: to the display, through
// the HUD's layer (display/hud_layer.hpp), with the picture's rectangle.
void OSD::present_frame() {
    const display::Mode m = dev->mode();
    if (frame_hook_) frame_hook_(m.w, m.h);

    // The display size, whatever REC records: a screen recording and the web
    // view's encoder (a phone watching) are both made at this size.
    DvrRecorder::instance().set_screen_size(m.w, m.h);

    layer_->present(frame_tile_);
}

// Draw background.png full-screen with optional warp-zoom transition.
// warp_t == 0: static full-frame display.
// warp_t in (0,1]: same zoom + alpha-fade curves as the warp shader so the
// PNG participates in the BACKGROUND→LIVE transition identically to the video.
void OSD::draw_bg_png(float fw, float fh, const float* mvp, float warp_t) {
    if (!bg_tex) return;

    // Mirror the warp shader's curves exactly:
    //   zoom  = 1 + t² × 8   (geometry expansion ≡ UV zoom into centre)
    //   alpha = max(0, 1 − t² × 1.35)
    float t2   = warp_t * warp_t;
    float zoom  = 1.0f + t2 * 8.0f;
    float alpha = std::max(0.0f, 1.0f - t2 * 1.35f);

    float qw = fw * zoom;   // expanded quad half-extents
    float qh = fh * zoom;

    float verts[] = {
        -qw, -qh, 0.0f,  0.0f, 1.0f,
         qw, -qh, 0.0f,  1.0f, 1.0f,
        -qw,  qh, 0.0f,  0.0f, 0.0f,
         qw,  qh, 0.0f,  1.0f, 0.0f,
    };

    canvas_->set_transform(mvp);
    canvas_->set_fade(alpha);
    canvas_->image_quad(bg_tex, verts, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_->set_fade(1.0f);   // opaque again for what follows
}

// Import a player's newest decoded picture (an NV12 DMA-BUF) as the external
// texture `tex`. The one place this is done, for the idle background and for
// a recording in the gallery.
bool OSD::import_drm_frame(BgVideoPlayer* player, render::ImageId tex) {
    if (!player || !canvas_->video_supported()) return false;
    BgDrmFrame drm;
    if (!player->get_latest_drm_frame(drm) || !drm.valid) return false;
    render::VideoFrame f;
    f.fd = drm.fd;
    f.width = drm.width;
    f.height = drm.height;
    f.fourcc = drm.drm_fmt;
    f.offset[0] = 0;
    f.pitch[0] = drm.stride_y;
    f.offset[1] = drm.offset_uv;
    f.pitch[1] = drm.stride_uv;
    f.modifier = drm.modifier;
    unsigned err = 0;
    if (!canvas_->import_video(tex, f, &err)) {
        fprintf(stderr, "[OSD] eglCreateImageKHR failed: 0x%x\n", err);
        return false;
    }
    // drm.ref drops here - the image holds its own reference to the memory
    return true;
}

void OSD::draw_bg_video(float fw, float fh, const float* mvp, float warp_t) {
    bool use_ext = bg_player_ && bg_player_->is_drm() &&
                   bg_ext_tex_ && canvas_->video_supported();
    if (!use_ext && !bg_video_tex_) return;

    int screen_w = dev->mode().w;
    int screen_h = dev->mode().h;
    float screen_asp = (float)screen_w / (float)screen_h;
    float video_asp  = (bg_player_ && bg_player_->height() > 0)
                       ? (float)bg_player_->width() / (float)bg_player_->height()
                       : screen_asp;

    // Crop-to-fill UV so bg video covers the screen without black bars
    float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    if (video_asp > screen_asp) {
        float s = screen_asp / video_asp;
        u0 = 0.5f - s * 0.5f;
        u1 = 0.5f + s * 0.5f;
    } else if (video_asp < screen_asp) {
        float s = video_asp / screen_asp;
        v0 = 0.5f - s * 0.5f;
        v1 = 0.5f + s * 0.5f;
    }

    float verts[] = {
        -fw, -fh, 0.0f,  u0, v1,
         fw, -fh, 0.0f,  u1, v1,
        -fw,  fh, 0.0f,  u0, v0,
         fw,  fh, 0.0f,  u1, v0,
    };

    if (use_ext) canvas_->video_quad(bg_ext_tex_, verts, mvp, warp_t);   // zero-copy: the decoder's buffer
    else         canvas_->image_warp_quad(bg_video_tex_, verts, mvp, warp_t);   // software frames
}
