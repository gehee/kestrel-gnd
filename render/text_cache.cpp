#include "render/text_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cairo.h>

#include "utils/prof.hpp"

namespace render {

// A cairo context set up exactly as ensure() renders, for
// measuring strings without rasterising them.
static cairo_t* text_measure_context() {
    static cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    static cairo_t* m = nullptr;
    // A cairo_t's error state is permanent (an invalid UTF-8 string sets it),
    // and a broken one measures everything as zero: start a fresh one.
    if (m && cairo_status(m) != CAIRO_STATUS_SUCCESS) { cairo_destroy(m); m = nullptr; }
    if (!m) {
        m = cairo_create(surf);
        cairo_select_font_face(m, "Chakra Petch SemiBold",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_matrix_t fm;
        cairo_matrix_init_scale(&fm, 40.0, 40.0);
        cairo_set_font_matrix(m, &fm);
    }
    return m;
}

void TextCache::ensure(const char* text) {
    // One lookup, not two: this used to count() here and operator[] again at the
    // end. On a hit, just stamp the entry so eviction knows it is warm.
    auto hit = text_cache.find(text);
    if (hit != text_cache.end()) {
        hit->second.last_used = ++text_cache_clock;
        return;
    }
    prof::count(prof::kCountTextMiss);
    {
        // Sized to the text. Every string used to get a 2048x64 texture and be
        // drawn as a quad of that shape - 32 times wider than tall, for "5" as
        // much as for a sentence - so the GPU blended, and the upload moved,
        // mostly transparent pixels: 512 KB per string. Measure first, then
        // make the surface just wide enough (the glyphs start at x = 2).
        cairo_t* measure = text_measure_context();
        cairo_text_extents_t ext;
        cairo_text_extents(measure, text, &ext);
        double right = std::max(ext.x_advance, ext.x_bearing + ext.width);
        int w = ((int)std::ceil(2.0 + right) + 4 + 15) & ~15;   // 2 px lead-in, 4 px margin, 16-aligned
        if (w < 16)   w = 16;
        if (w > 2048) w = 2048;
        int h = 64;
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(surface);
        
        // Chakra Petch, shipped in the rootfs overlay. The HUD and the menu were
        // designed in it; the goggle previously had only DejaVu Sans, so every
        // mockup was drawn in one face and rendered in another - which is most
        // of why the built menu never looked like the picture of it.
        //
        // "Rajdhani" here before it was a font that was never installed, so it
        // silently fell back to DejaVu and nobody noticed for a long time. The
        // family name below is the one fontconfig reports, so a missing font
        // shows up as a visibly wrong face rather than an invisible fallback.
        cairo_select_font_face(cr, "Chakra Petch SemiBold",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

        // Use shared OSD font configuration
        cairo_matrix_t font_matrix;
        // 0.75 was condensing DejaVu, which is a wide face. Chakra Petch is
        // already narrow by design; condensing it again closes the counters and
        // is exactly what made the menu look cramped next to the mockup.
        cairo_matrix_init_scale(&font_matrix, 1.0, 1.0);
        cairo_matrix_scale(&font_matrix, 40.0, 40.0);    
        cairo_set_font_matrix(cr, &font_matrix);
        
        cairo_text_extents_t extents;
        cairo_text_extents(cr, text, &extents);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, 2, 48); 
        cairo_show_text(cr, text);
        
        cairo_destroy(cr);
        unsigned char* data = cairo_image_surface_get_data(surface);
        
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Clamp: with the texture now ending just past the last glyph, the
        // default REPEAT would filter the left edge into the right one.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        
        // Evict BEFORE inserting so the new entry is never the eviction victim.
        // Pick the least-recently-used entry. The previous version took
        // text_cache.begin() - the lexicographically smallest key - so it
        // preferentially discarded the shortest, hottest strings and paid a full
        // cairo render plus glTexImage2D to rebuild them almost immediately. It
        // also built a std::string per comparison while scanning; comparing the
        // key against the char* directly needs no allocation.
        // We are on the miss path here, so the O(n) scan runs rarely.
        if (text_cache.size() >= 500) {
            auto victim = text_cache.end();
            for (auto it = text_cache.begin(); it != text_cache.end(); ++it) {
                if (it->first == text) continue;   // never evict what we are adding
                if (victim == text_cache.end() ||
                    it->second.last_used < victim->second.last_used)
                    victim = it;
            }
            if (victim != text_cache.end()) {
                glDeleteTextures(1, &victim->second.tex);
                text_cache.erase(victim);
            }
        }

        text_cache[text] = {tex, w, h, (float)extents.x_advance, std::string(text),
                            ++text_cache_clock};
        cairo_surface_destroy(surface);
    }
}

float TextCache::width(const char* text, float scale) {
    if (!text || !text[0]) return 0.0f;
    // A cached string knows its width; anything else is measured, not
    // rendered - the menu's word wrap tries many strings it never draws.
    auto it = text_cache.find(text);
    if (it != text_cache.end()) return (it->second.text_w / 64.0f) * scale;
    cairo_text_extents_t ext;
    cairo_text_extents(text_measure_context(), text, &ext);
    return ((float)ext.x_advance / 64.0f) * scale;
}

const TextCache::Entry* TextCache::get(const char* text) {
    ensure(text);
    auto it = text_cache.find(text);
    return it == text_cache.end() ? nullptr : &it->second;
}

}  // namespace render
