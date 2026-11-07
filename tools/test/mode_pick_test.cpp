// Host test of Screen Mode Auto (display/kms/mode_follow.h). The first mode,
// the screen's alone: 1080p above 60 Hz, 720p above 60 Hz, another 16:9 size
// above 60 Hz, 1080p60, 720p60, the fastest first. The order kestrel-gnd falls
// back along. And the live re-pick at a size for the camera's frame rate.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "display/kms/mode_follow.h"

static int fails;

struct M { int w, h; double hz; bool interlaced; };

// A mode with timings that give hz exactly enough (vrefresh rounded, as the
// kernel does).
static drmModeModeInfo mode(const M& d) {
    drmModeModeInfo m;
    std::memset(&m, 0, sizeof(m));
    m.hdisplay = (uint16_t)d.w;
    m.vdisplay = (uint16_t)d.h;
    m.htotal = (uint16_t)(d.w + 160);
    m.vtotal = (uint16_t)(d.h + 45);
    m.clock = (uint32_t)(d.hz * m.htotal * m.vtotal / 1000.0 + 0.5);
    m.vrefresh = (uint32_t)(modeset_mode_hz(&m) + 0.5);
    if (d.interlaced) m.flags |= DRM_MODE_FLAG_INTERLACE;
    return m;
}

struct Screen {
    std::vector<drmModeModeInfo> modes;
    drmModeConnector c;
    explicit Screen(const std::vector<M>& list) {
        for (const M& d : list) modes.push_back(mode(d));
        std::memset(&c, 0, sizeof(c));
        c.count_modes = (int)modes.size();
        c.modes = modes.data();
    }
    std::string name(int i) const {
        if (i < 0) return "none";
        char b[32];
        std::snprintf(b, sizeof(b), "%dx%d@%u", modes[i].hdisplay, modes[i].vdisplay, modes[i].vrefresh);
        return b;
    }
    std::string order(int max) const {
        int o[32];
        const int n = modeset_auto_order(&c, o, max);
        std::string s;
        for (int k = 0; k < n; k++) s += (k ? " " : "") + name(o[k]);
        return s;
    }
};

static void expect(int line, const std::string& got, const char* want) {
    if (got != want) { fails++; std::printf("FAIL line %d: got %s, want %s\n", line, got.c_str(), want); }
}

// Auto's first mode.
#define PICK(want, list) do { Screen s(list); expect(__LINE__, s.name(modeset_pick_auto(&s.c)), want); } while (0)
// Auto's whole order, "a b c".
#define ORDER(want, list) do { Screen s(list); expect(__LINE__, s.order(32), want); } while (0)
// The live re-pick at w x h for fps.
#define RATE(want, list, fps, w, h) do { Screen s(list); \
    expect(__LINE__, s.name(modeset_pick_rate(&s.c, fps, w, h, nullptr)), want); } while (0)

int main() {
    using V = std::vector<M>;

    // The user's rules: 1080p60 over 720p60, 720p100 over 1080p60, 1080p100+
    // over 720p100+.
    PICK("1920x1080@60", V({{1280, 720, 60, 0}, {1920, 1080, 60, 0}}));
    PICK("1280x720@100", V({{1920, 1080, 60, 0}, {1280, 720, 100, 0}, {1280, 720, 60, 0}}));
    PICK("1920x1080@100", V({{1280, 720, 240, 0}, {1920, 1080, 100, 0}, {1280, 720, 120, 0}}));

    // A 1080p240 monitor: the fastest 1080p, over 4K and fast small modes.
    const V mon = {{3840, 2160, 60, 0}, {1920, 1080, 240, 0}, {1920, 1080, 144, 0},
                   {1920, 1080, 120, 0}, {1920, 1080, 100, 0}, {1920, 1080, 60, 0},
                   {1280, 720, 360, 0}, {1024, 768, 300, 0}, {640, 480, 360, 0}};
    PICK("1920x1080@240", mon);
    ORDER("1920x1080@240 1920x1080@144 1920x1080@120 1920x1080@100 1280x720@360 "
          "1920x1080@60 3840x2160@60 640x480@360 1024x768@300", mon);
    PICK("1920x1080@180", V({{1920, 1080, 180, 0}, {1920, 1080, 165, 0}, {1920, 1080, 60, 0}}));

    // The goggle's headset (CPO1F41) and panel (BOE 1080).
    PICK("1920x1080@120", V({{1920, 1080, 120, 0}, {1920, 1080, 60, 0}, {1280, 720, 60, 0}}));
    PICK("1920x1080@120", V({{1920, 1080, 120, 0}, {1920, 1080, 100, 0}, {1920, 1080, 60, 0}}));

    // A TV: 1080p only at 60, 720p at 100; interlaced left out.
    ORDER("1280x720@100 1920x1080@60 1920x1080@50 1280x720@60",
          V({{1920, 1080, 60, 0}, {1920, 1080, 50, 0}, {1280, 720, 100, 0}, {1280, 720, 60, 0},
             {1920, 1080, 60, 1}}));

    // No fast 1080p or 720p: another 16:9 size above 60 Hz, 720 lines or
    // more, before 1080p60 (the closest to 1080 lines on a tie); under 720
    // lines it does not beat 1080p60.
    PICK("2560x1440@144", V({{2560, 1440, 144, 0}, {1920, 1080, 60, 0}, {1366, 768, 75, 0}}));
    PICK("1600x900@144", V({{2560, 1440, 144, 0}, {1600, 900, 144, 0}, {1920, 1080, 60, 0}}));
    PICK("1920x1080@60", V({{1920, 1080, 60, 0}, {1280, 720, 60, 0}, {854, 480, 120, 0}}));

    // 16:10 and 4:3 only count without any 16:9 mode.
    PICK("1920x1080@60", V({{1920, 1200, 144, 0}, {1600, 1200, 120, 0}, {1920, 1080, 60, 0}}));
    PICK("1280x800@144", V({{1920, 1200, 60, 0}, {1920, 1200, 120, 0}, {1280, 800, 144, 0}}));

    // Nothing above 60 Hz: 1080p, else 720p, else the size closest to 1080p.
    PICK("1920x1080@60", V({{3840, 2160, 60, 0}, {1920, 1080, 60, 0}, {1920, 1080, 50, 0}, {1280, 720, 60, 0}}));
    PICK("1280x720@60", V({{3840, 2160, 60, 0}, {1280, 720, 60, 0}}));
    PICK("2560x1440@60", V({{3840, 2160, 60, 0}, {2560, 1440, 60, 0}}));

    // 59.94 and 60: 60 first. Interlaced only: nothing usable.
    PICK("1920x1080@60", V({{1920, 1080, 59.94, 0}, {1920, 1080, 60.0, 0}}));
    PICK("none", V({{1920, 1080, 60, 1}}));

    // The live re-pick for the camera at the size Auto chose: above 60 Hz,
    // in step if it can, else the fastest.
    RATE("1920x1080@100", mon, 100, 1920, 1080);
    RATE("1920x1080@240", mon, 120, 1920, 1080);
    RATE("1920x1080@240", mon, 60, 1920, 1080);    // 240 is in step with 60
    RATE("1920x1080@240", mon, 90, 1920, 1080);    // none in step: the fastest
    RATE("1920x1080@240", mon, 0, 1920, 1080);     // Refresh Sync FOLLOW: the fastest
    RATE("1920x1080@144", V({{1920, 1080, 60, 0}, {1920, 1080, 144, 0}}), 60, 1920, 1080);

    // The order cut at max is still the order's start.
    {
        Screen s(mon);
        expect(__LINE__, s.order(3), "1920x1080@240 1920x1080@144 1920x1080@120");
    }

    std::printf(fails ? "mode_pick_test: %d failed\n" : "mode_pick_test: all passed\n", fails);
    return fails ? 1 : 0;
}
