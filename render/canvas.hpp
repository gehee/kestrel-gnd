#pragma once
// What the HUD, menu and gallery are drawn with: the frame and its clip, the
// shapes (panels, text, arcs, bars, icons), images and video pictures, the
// pixel-space pieces, copies of what is drawn. The HUD draws through this and
// nothing else, so a box can supply its own way of drawing: CanvasGles on the
// GPU (render/canvas_gles), made by the box's platform; a CPU canvas for a
// box without one is another implementation of this.
//
// Coordinates and colours are as the OSD has always used them: its frustum
// space, colours 0..1, alpha 0..1. An image is the backend's handle for a
// picture it already holds (a GL texture name for CanvasGles).

#include <cstdint>
#include <vector>

#include "utils/math_utils.hpp"

namespace render {

using ImageId = uint32_t;

// A decoded picture as a decoder hands it over: a DMA-BUF, its planes'
// offsets and pitches (NV12: two, in the one buffer), its DRM format and
// modifier.
struct VideoFrame {
    int fd = -1;
    int width = 0, height = 0;
    uint32_t fourcc = 0;
    uint32_t offset[2] = {0, 0}, pitch[2] = {0, 0};
    uint64_t modifier = 0;
};

class Canvas {
public:
    virtual ~Canvas() = default;

    // ---- its thread, and what it draws with -------------------------------------
    // Drawing happens on the calling thread from now on: before init(), and
    // before the images are freed at the end, on whichever thread does that.
    virtual void bind_thread() = 0;
    // What it draws with, made on that thread before the first frame.
    virtual void init() = 0;
    // A string made ready now, so the frame that first draws it does not pay
    // for it.
    virtual void cache_text(const char* text) = 0;

    // ---- the frame, and where shapes land --------------------------------------
    // A frame starts at w x h pixels: all of it cleared to nothing, no clip.
    virtual void begin_frame(int w, int h) = 0;
    // The frame's size again (after drawing into an image).
    virtual void viewport(int w, int h) = 0;
    // Drawing kept inside a rectangle, pixels from the bottom left; or everywhere.
    virtual void clip(int x, int y, int w, int h) = 0;
    virtual void unclip() = 0;
    // What the clip leaves of the frame, cleared to nothing (a hole the
    // picture shows through).
    virtual void clear() = 0;
    // Shapes land over what is there, premultiplied (the HUD's frames are).
    virtual void blend_over() = 0;
    // What every shape's coordinates go through: a 4x4 matrix, column-major,
    // as the HUD's frustum and the wings' slants are made.
    virtual void set_transform(const float* mvp) = 0;
    // A fade every shape is multiplied by until it is set again (1: none).
    virtual void set_fade(float alpha) = 0;

    // ---- images -----------------------------------------------------------------
    // An image of w x h pixels as cairo keeps them (ARGB32: B, G, R, A bytes,
    // premultiplied), rows top first, smoothed when scaled; past its edges the
    // edge pixels, or with repeat the image again (it matters only where the
    // smoothing reaches over an edge). 0 if it cannot be made.
    virtual ImageId make_image(int w, int h, const void* pixels, bool repeat = false) = 0;
    // New pixels for an image, at a size of their own.
    virtual void update_image(ImageId image, int w, int h, const void* pixels) = 0;
    virtual void free_image(ImageId image) = 0;

    // ---- video pictures -----------------------------------------------------------
    // Whether pictures can be shown at all: taken as they are (no copy) by
    // what draws.
    virtual bool video_supported() const = 0;
    // An image a picture is imported into; it shows the last one.
    virtual ImageId make_video() = 0;
    // The picture into it; false if it cannot be (error: the backend's code).
    virtual bool import_video(ImageId video, const VideoFrame& frame, unsigned* error = nullptr) = 0;
    // A video image on a strip of four corners (x, y, z, then u, v) through
    // mvp, as it is or with the warp-zoom (warp_t > 0). The shapes are drawn
    // the canvas's way again after; their transform is not touched.
    virtual void video_quad(ImageId video, const float* v, const float* mvp, float warp_t) = 0;
    // An image likewise: with the warp-zoom (warp_t > 0), else as the shapes
    // draw an image, through mvp - which the shapes' transform then is.
    virtual void image_warp_quad(ImageId image, const float* v, const float* mvp, float warp_t) = 0;

    // ---- copies of what is drawn --------------------------------------------------
    // What the frame holds now, w x h from the bottom left, into `image`
    // (made if 0): premultiplied, as drawn. False if it could not be copied.
    virtual bool snapshot(ImageId& image, int w, int h) = 0;
    // Drawing goes into `image` (made if 0, w x h) instead of the frame,
    // cleared first - a layer kept between frames; false if it cannot be.
    // end_layer(): to the frame again, w x h.
    virtual bool begin_layer(ImageId& image, int w, int h) = 0;
    virtual void end_layer(int w, int h) = 0;
    // A copy (snapshot, layer) on a strip of four corners (x, y, z, u, v)
    // through mvp, as it was drawn, faded by alpha. The shapes are drawn the
    // canvas's way again after; their transform is not touched.
    virtual void copy_quad(ImageId image, const float* v, const float* mvp, float alpha) = 0;

    // ---- pixel space (the pages over the picture, the gallery) -----------------
    // Coloured triangles, per vertex x, y, then the premultiplied r, g, b, a
    // (or a strip of them), through mvp - which the shapes' transform is
    // afterwards too.
    virtual void triangles(const float* v, int n, const float* mvp, bool strip = false) = 0;
    // One colour over a strip of four corners (x, y, z, u, v), faded by alpha.
    virtual void solid_quad(const float* v, float r, float g, float b, float alpha) = 0;
    // A string with its left at x (align 0), centred on it (1) or its right at
    // it (2), its quad's bottom at gy and `scale` high, in rgb faded by a.
    // Returns its width; 0 and nothing drawn if it cannot be.
    virtual float text_quad(const char* text, float x, float gy, float scale, int align,
                            const float* rgb, float a) = 0;

    virtual void panel(float x, float y, float w, float h, float alpha,
                       float r = 0.1f, float g = 0.1f, float b = 0.15f) = 0;
    virtual void trapezoid(float x, float y, float w, float h, float top_scale, float slant,
                           float alpha, float r, float g, float b) = 0;
    virtual void poly(const std::vector<math::Vec2> &points, float alpha, float r, float g, float b) = 0;
    // uniform_x: drop the left-to-right fade and use the mid-width value
    // across the whole panel. The wing panels want the gradient (it points
    // at the screen edge); the centre menu does not - it made one side of
    // the menu wash out over video.
    virtual void hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b,
                           bool outline, int num_h = 8, int num_v = 10, float grid_y = -1.0f,
                           float grid_h = -1.0f, bool uniform_x = false) = 0;
    virtual void arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle,
                     float alpha, float r, float g, float b) = 0;
    virtual void grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle,
                          float end_angle, float alpha) = 0;
    virtual void glow_line(const std::vector<math::Vec2> &points, float width, float alpha,
                           float r, float g, float b) = 0;

    virtual void icon(ImageId image, float x, float y, float w, float h,
                      float r = 1.0f, float g = 1.0f, float b = 1.0f, float alpha = 1.0f) = 0;
    virtual void icon_quad(ImageId image, math::Vec2 bl, math::Vec2 br, math::Vec2 tr, math::Vec2 tl,
                           float r = 1.0f, float g = 1.0f, float b = 1.0f) = 0;
    // An image on four corners given as a strip - x, y, z, then u, v from the
    // image's top left, for each - tinted rgb and faded by alpha.
    virtual void image_quad(ImageId image, const float* v, float r, float g, float b, float alpha) = 0;

    virtual float text_width(const char *text, float scale) = 0;
    virtual void text(const char *text, float x, float y, float scale, bool right_align = false,
                      float r = 1.0f, float g = 1.0f, float b = 1.0f) = 0;
    // The first `reveal` (0..1) of the text, for text being typed out.
    virtual void text_wipe(const char *text, float x, float y, float scale, bool right_align,
                           float r, float g, float b, float reveal) = 0;

    // Up to six coloured segments end to end in one slanted bar.
    virtual void stacked_bar(float x, float y, float w, float h, float slant, bool reverse,
                             float val1, float r1, float g1, float b1,
                             float val2 = 0, float r2 = 0, float g2 = 0, float b2 = 0,
                             float val3 = 0, float r3 = 0, float g3 = 0, float b3 = 0,
                             float val4 = 0, float r4 = 0, float g4 = 0, float b4 = 0,
                             float val5 = 0, float r5 = 0, float g5 = 0, float b5 = 0,
                             float val6 = 0, float r6 = 0, float g6 = 0, float b6 = 0) = 0;
    virtual void signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val) = 0;
    virtual void seg_track(float x, float y, float w, float h, int segs, float filled, bool edge_left,
                           float r, float g, float b, float a_lo, float a_hi, float reveal = 1.0f) = 0;

    // A fading shape: columns xs[i] with their top and bottom and alpha.
    virtual void fade_wedge(const float *xs, const float *tops, const float *bots,
                            const float *alphas, int n, float r, float g, float b) = 0;
    // A hairline ramping from a0 to a1.
    virtual void fade_rule(float x0, float y0, float x1, float y1, float thick,
                           float a0, float r, float g, float b, float a1 = 0.0f) = 0;
    // Glowing rails along the picture's top and bottom edges.
    virtual void rail_glow(float fw, float fh, float depth, float alpha, float r, float g, float b) = 0;

    // Edge feathering for seg_track, fade_wedge and fade_rule: a border this
    // wide (frustum units, 0 = off) fading to nothing around each shape.
    virtual void set_feather(float width) = 0;
};

}  // namespace render
