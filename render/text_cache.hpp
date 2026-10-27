#pragma once
// Strings rendered once with cairo into a GL texture each, kept for reuse:
// the HUD draws the same few hundred strings every frame.

#include <GLES2/gl2.h>
#include <cstdint>
#include <map>
#include <string>

namespace render {

class TextCache {
public:
    struct Entry {
        GLuint tex;
        int w, h;
        float text_w;
        std::string text;
        // Touched on every hit so eviction can pick the genuinely coldest
        // entry. The old code evicted text_cache.begin() - the alphabetically
        // first key - which threw away hot short strings ("0", "1", ...) and
        // re-rendered them through cairo on the very next frame.
        uint64_t last_used = 0;
    };

    // Renders `text` into the cache if it is not there yet.
    void ensure(const char* text);
    // The entry for `text`, rendered first if need be; null if it can't be.
    const Entry* get(const char* text);
    // Rendered width of `text` at `scale`, in the units text drawing uses for
    // right-alignment. Measured, not rendered, for a string not cached.
    float width(const char* text, float scale);

private:
    std::map<std::string, Entry> text_cache;
    uint64_t text_cache_clock = 0;
};

}  // namespace render
