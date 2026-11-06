#include "platform/panel_logic.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace platform;
void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}
struct Samples {
    PanelDebouncer debounce;
    uint64_t now = 0;
    std::vector<PanelEvent> events;
    void sample(PanelKey key, int count = 1, bool valid = true) {
        for (int i = 0; i < count; ++i, now += 20)
            if (auto e = debounce.sample(key, valid, now))
                events.push_back(*e);
    }
};
int main() {
    using K = PanelKey;
    Samples s;
    s.sample(K::Ok, 10);
    check(s.events.empty(), "boot-held OK must not activate UI");
    s.sample(K::None, 3);
    s.sample(K::Left, 5);
    check(s.events.size() == 1 && s.events[0].key == K::Left, "stable LEFT press");
    s.sample(K::Rec);
    s.sample(K::None, 3);
    check(s.events.size() == 1, "observed 464mV release transient must not start recording");
    s.sample(K::Ok, 30);
    check(s.events.size() == 2, "OK must not repeat");
    s.sample(K::Rec, 5);
    check(s.events.size() == 2, "require release before a different key");
    s.sample(K::None, 3);
    s.sample(K::Down, 30);
    check(s.events.size() == 4 && s.events.back().repeat, "direction repeats after delay");
    s.sample(K::None, 1, false);
    s.sample(K::Down, 40);
    check(s.events.size() == 4, "I/O recovery while held must not synthesize press or repeat");
    s.sample(K::None, 3);
    s.sample(K::Down, 3);
    check(s.events.size() == 5, "released input rearms after error");
    s.now += 1000;
    s.sample(K::Down, 30);
    check(s.events.size() == 5, "polling stall disarms a held key");
    Samples link;
    link.sample(K::None, 3);
    link.sample(K::Link, 10);
    check(link.events.empty(), "LINK acts on release");
    link.sample(K::None, 3);
    check(link.events.size() == 1 && link.events.back().key == K::Link, "short LINK release binds once");
    link.sample(K::Link, 410);
    link.sample(K::None, 3);
    check(link.events.size() == 2 && link.events.back().key == K::Upgrade, "long LINK cannot bind");
    link.sample(K::Link, 10);
    link.sample(K::None, 1, false);
    link.sample(K::None, 10);
    check(link.events.size() == 2, "invalid LINK read must not synthesize release action");
    // Physical captures, including the release transient, not just nominal centres.
    check(bsp::ascent_panel::decode(30 << 4) == K::Rec, "386mV REC");
    check(bsp::ascent_panel::decode(61 << 4) == K::Up, "786mV UP");
    check(bsp::ascent_panel::decode(77 << 4) == K::Left, "992mV LEFT");
    check(bsp::ascent_panel::decode(92 << 4) == K::Right, "1185mV RIGHT");
    check(bsp::ascent_panel::decode(108 << 4) == K::Ok, "1392mV OK");
    check(bsp::ascent_panel::decode(123 << 4) == K::Down, "1585mV DOWN");
    check(bsp::ascent_panel::decode(138 << 4) == K::Back, "1778mV BACK");
    check(bsp::ascent_panel::decode(0) == K::None, "idle");
    puts("panel debounce, recovery, repeats and LINK release tests passed");
}
