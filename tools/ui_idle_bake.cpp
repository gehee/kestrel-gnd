// Build-host-only renderer. The receiver loads one finished image per theme.
#include "hud/hud_theme.hpp"
#include <cairo.h>
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
Surface surface(cairo_surface_t* p) {
    Surface result(p, cairo_surface_destroy);
    if (cairo_surface_status(p) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error(cairo_status_to_string(cairo_surface_status(p)));
    return result;
}
Context context(cairo_surface_t* s) { return Context(cairo_create(s), cairo_destroy); }
void word(std::ostream& out, uint32_t v) {
    const char bytes[] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
    out.write(bytes, 4);
}
void color(cairo_t* cr, const float* rgb, double alpha = 1) {
    // Match the receiver's 8-bit theme conversion.
    cairo_set_source_rgba(cr, int(rgb[0] * 255) / 255., int(rgb[1] * 255) / 255., int(rgb[2] * 255) / 255., alpha);
}
Surface logo(bool tail, cairo_font_face_t* face) {
    // Same source layout as ui_scene_bake.py / the VRX Pro wordmark.
    auto s = surface(cairo_image_surface_create(CAIRO_FORMAT_A8, 2048, 256));
    auto cr = context(s.get());
    cairo_set_font_face(cr.get(), face);
    double size = 168;
    cairo_set_font_size(cr.get(), size);
    cairo_text_extents_t all{};
    cairo_text_extents(cr.get(), "fpvOS", &all);
    if (all.width > 1) {
        size *= 1024 / all.width; cairo_set_font_size(cr.get(), size);
        cairo_text_extents(cr.get(), "fpvOS", &all);
    }
    if (all.height > 256 * .52) {
        size *= 256 * .52 / all.height; cairo_set_font_size(cr.get(), size);
        cairo_text_extents(cr.get(), "fpvOS", &all);
    }
    double x = (2048 - all.width) / 2 - all.x_bearing;
    if (tail) {
        cairo_text_extents_t head{}; cairo_text_extents(cr.get(), "fpv", &head);
        x += head.x_advance;
    }
    cairo_set_source_rgba(cr.get(), 1, 1, 1, 1);
    cairo_move_to(cr.get(), x, (256 - all.height) / 2 - all.y_bearing);
    cairo_show_text(cr.get(), tail ? "OS" : "fpv");
    return s;
}
void mask(cairo_t* cr, cairo_surface_t* image, double x, double y, double scale, const float* rgb) {
    cairo_save(cr); cairo_translate(cr, x, y); cairo_scale(cr, scale, scale);
    color(cr, rgb); cairo_mask_surface(cr, image, 0, 0); cairo_restore(cr);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 6) throw std::runtime_error("usage: ui-idle-bake FONT BACKGROUND_PNG WIDTH HEIGHT OUTPUT");
        const int width = std::stoi(argv[3]), height = std::stoi(argv[4]);
        if (width < 640 || width > 4096 || height < 360 || height > 2160)
            throw std::runtime_error("unsupported idle image dimensions");
        if (!FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(argv[1])))
            throw std::runtime_error("cannot register idle font");
        // Use the supplied font file, including its weight, rather than a system substitute.
        int count = 0;
        FcPattern* pattern = FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(argv[1]), 0, nullptr, &count);
        if (!pattern) throw std::runtime_error("cannot read idle font");
        auto face = std::unique_ptr<cairo_font_face_t, decltype(&cairo_font_face_destroy)>(
            cairo_ft_font_face_create_for_pattern(pattern), cairo_font_face_destroy);
        FcPatternDestroy(pattern);
        auto background = surface(cairo_image_surface_create_from_png(argv[2]));
        auto head = logo(false, face.get()), tail = logo(true, face.get());
        std::ofstream out(argv[5], std::ios::binary | std::ios::trunc);
        out.write("KUIB", 4);
        for (uint32_t v : {1u, uint32_t(width), uint32_t(height), uint32_t(hud_theme_count())}) word(out, v);
        for (int i = 0; i < hud_theme_count(); ++i) {
            const auto& theme = hud_theme_at(i);
            char name[32]{};
            if (std::strlen(theme.name) >= sizeof(name)) throw std::runtime_error("idle theme name too long");
            std::strcpy(name, theme.name); out.write(name, sizeof(name));
            auto image = surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
            auto cr = context(image.get());
            // A static, opaque image: no per-menu transforms or shadow passes.
            cairo_set_source_rgb(cr.get(), 0, 0, 0); cairo_paint(cr.get());
            cairo_save(cr.get());
            cairo_scale(cr.get(), double(width) / cairo_image_surface_get_width(background.get()),
                        double(height) / cairo_image_surface_get_height(background.get()));
            cairo_set_source_surface(cr.get(), background.get(), 0, 0); cairo_paint(cr.get()); cairo_restore(cr.get());
            const double h = height * .3315, x = width * .5 - h * 4, y = height * .46 - h * .5, sh = h * .018;
            const float shadow1[] = {0, 0, .05f}, shadow2[] = {0, 0, .03f};
            for (auto* part : {head.get(), tail.get()}) {
                mask(cr.get(), part, x + sh * 2, y + sh * 2, h / 256, shadow1);
                mask(cr.get(), part, x + sh, y + sh, h / 256, shadow2);
            }
            mask(cr.get(), head.get(), x, y, h / 256, theme.text);
            mask(cr.get(), tail.get(), x, y, h / 256, theme.accent);
            cairo_set_font_face(cr.get(), face.get()); cairo_set_font_size(cr.get(), height * 45.8 / 1080);
            cairo_text_extents_t ready{}; cairo_text_extents(cr.get(), "Ready", &ready);
            const double tx = (width - ready.x_advance) / 2, ty = height * .867, ts = height * .00238;
            color(cr.get(), shadow1, .35); cairo_move_to(cr.get(), tx + 2 * ts, ty + 2 * ts); cairo_show_text(cr.get(), "Ready");
            color(cr.get(), shadow2, .60); cairo_move_to(cr.get(), tx + ts, ty + ts); cairo_show_text(cr.get(), "Ready");
            color(cr.get(), theme.accent); cairo_move_to(cr.get(), tx, ty); cairo_show_text(cr.get(), "Ready");
            if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("idle rasterization failed");
            cairo_surface_flush(image.get());
            // KUIB pixels are little-endian ARGB8888, independent of host endian.
            const auto* data = cairo_image_surface_get_data(image.get());
            const int stride = cairo_image_surface_get_stride(image.get());
            for (int row = 0; row < height; ++row)
                for (int col = 0; col < width; ++col) {
                    uint32_t pixel; std::memcpy(&pixel, data + row * stride + col * 4, 4); word(out, pixel);
                }
        }
        if (!out) throw std::runtime_error("idle asset write failed");
        std::printf("Baked %d static idle themes at %dx%d\n", hud_theme_count(), width, height);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "idle bake: %s\n", e.what()); return 1; }
}
