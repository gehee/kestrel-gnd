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
    Samples dual;
    dual.sample(K::None, 3);
    dual.sample(K::OkBack, 10);
    check(dual.events.empty(), "OkBack waits for release or hold");
    dual.sample(K::None, 3);
    check(dual.events.size() == 1 && dual.events.back().key == K::Ok, "OkBack tap is OK");
    dual.sample(K::OkBack, 24);
    check(dual.events.size() == 1, "OkBack under 500 ms is not Back yet");
    dual.sample(K::OkBack, 3);
    check(dual.events.size() == 2 && dual.events.back().key == K::Back, "OkBack held 500 ms is Back");
    dual.sample(K::OkBack, 100);
    dual.sample(K::None, 3);
    check(dual.events.size() == 2, "OkBack hold is one Back, no OK on release");
    dual.sample(K::OkBack, 10);
    dual.sample(K::None, 1, false);
    dual.sample(K::None, 10);
    check(dual.events.size() == 2, "invalid OkBack read must not synthesize OK");
    // Released between the last sample under 500 ms and the first over it.
    Samples edge;
    edge.sample(K::None, 3);
    edge.sample(K::OkBack, 25); // held samples at 0..480 ms: no Back yet
    check(edge.events.empty(), "OkBack at 480 ms held has not acted");
    edge.sample(K::None, 3);    // release first seen at 500 ms
    check(edge.events.size() == 1 && edge.events.back().key == K::Ok, "OkBack released at the 500 ms edge is still OK");
    // Physical captures, including the release transient, not just nominal centres.
    check(bsp::ascent_panel::gnd485::decode(30 << 4) == K::Rec, "386mV REC");
    check(bsp::ascent_panel::gnd485::decode(61 << 4) == K::Up, "786mV UP");
    check(bsp::ascent_panel::gnd485::decode(77 << 4) == K::Left, "992mV LEFT");
    check(bsp::ascent_panel::gnd485::decode(92 << 4) == K::Right, "1185mV RIGHT");
    check(bsp::ascent_panel::gnd485::decode(108 << 4) == K::Ok, "1392mV OK");
    check(bsp::ascent_panel::gnd485::decode(123 << 4) == K::Down, "1585mV DOWN");
    check(bsp::ascent_panel::gnd485::decode(138 << 4) == K::Back, "1778mV BACK");
    check(bsp::ascent_panel::gnd485::decode(0) == K::None, "idle");
    // Per board. Gnd485: no ADC reading is the tap/hold key, and its OK and
    // Back each act once on the press - held past 500 ms they do nothing more.
    namespace g485 = bsp::ascent_panel::gnd485;
    namespace sd = bsp::ascent_panel::steadydigital;
    for (unsigned word = 0; word < 65536; ++word)
        check(g485::decode(uint16_t(word)) != K::OkBack, "no Gnd485 reading is the tap/hold key");
    for (K key : {K::Ok, K::Back}) {
        Samples gnd;
        gnd.sample(K::None, 3);
        gnd.sample(g485::decode(uint16_t((key == K::Ok ? 108 : 138) << 4)), 3);
        check(gnd.events.size() == 1 && gnd.events[0].key == key, "Gnd485 OK/Back acts on the press");
        gnd.sample(key, 60);
        gnd.sample(K::None, 3);
        check(gnd.events.size() == 1, "Gnd485 OK/Back held 1.2 s: no second event, no Back from a hold");
    }
    // SteadyDigital: the back key alone is the tap/hold key; nothing is OK or
    // Back outright, the centre is REC and PAIR is LINK.
    unsigned tap_hold = 0;
    for (const auto &button : sd::buttons) {
        check(button.key != K::Ok && button.key != K::Back, "SteadyDigital has no plain OK or Back key");
        if (button.key == K::OkBack) {
            ++tap_hold;
            check(button.code == KEY_ESC, "SteadyDigital tap/hold is the back key");
        }
        if (button.code == KEY_ENTER)
            check(button.key == K::Rec, "SteadyDigital centre is REC");
        if (button.code == KEY_WPS_BUTTON)
            check(button.key == K::Link, "SteadyDigital PAIR is LINK");
    }
    check(tap_hold == 1, "SteadyDigital has exactly one tap/hold key");
    puts("panel debounce, recovery, repeats, LINK, OkBack and per-board key map tests passed");
}
