// Unit test of the board support: the VRX Pro's tables, the front-panel and
// bind readers that use them (utils/vrx_buttons.hpp), and the supply-voltage
// arithmetic. The ADC channels are stood in for by a temp file holding a raw
// count, which is all the readers ever see of the real sysfs attribute.
//   cmake -DKESTREL_TESTS=ON ... && ctest     (or run the binary on the unit)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bsp/bsp.hpp"
#include "utils/vrx_buttons.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// A fake ADC channel: a file the readers open by path.
struct FakeAdc {
    std::string path;
    FakeAdc() {
        char tmpl[] = "/tmp/bsp_test_adc_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) close(fd);
        path = tmpl;
    }
    ~FakeAdc() { unlink(path.c_str()); }
    void set(int raw) const {
        if (FILE *f = fopen(path.c_str(), "w")) { fprintf(f, "%d\n", raw); fclose(f); }
    }
    // The raw count that reads back as `mv` on `adc`.
    static int raw_for(const bsp::AdcChannel &adc, int mv) {
        return (int)(((long)mv * adc.full_raw + adc.ref_mv - 1) / adc.ref_mv);
    }
};

// Polls the panel until the debounce has settled; what the third poll says.
static VrxButtons::Key settle(VrxButtons &b, uint64_t &t) {
    VrxButtons::Key k = VrxButtons::VB_NONE;
    for (int i = 0; i < 3; i++) { k = b.poll(t); t += 25; }
    return k;
}

static void test_vrx_pro_tables() {
    const bsp::Board &b = bsp::vrx_pro();
    CHECK(std::string(b.name) == "vrx_pro");

    // Every panel key once, and no two match windows overlap.
    const bsp::AdcLadder &l = b.buttons;
    CHECK(l.n_keys == 7);
    int seen[7] = {};
    for (size_t i = 0; i < l.n_keys; i++) {
        CHECK(l.keys[i].key >= 0 && l.keys[i].key < 7);
        seen[l.keys[i].key]++;
        CHECK(l.keys[i].mv - l.tolerance_mv > l.floor_mv);
        CHECK(l.keys[i].mv + l.tolerance_mv <= l.adc.ref_mv + l.tolerance_mv);
        for (size_t j = i + 1; j < l.n_keys; j++) {
            int d = l.keys[i].mv - l.keys[j].mv; if (d < 0) d = -d;
            CHECK(d > 2 * l.tolerance_mv);
        }
    }
    for (int k = 0; k < 7; k++) CHECK(seen[k] == 1);

    // The bind key's press window is well below its idle level.
    CHECK(b.bind.press_max_mv > 0 && b.bind.press_max_mv < b.bind.adc.ref_mv / 2);

    // Stock's supply scaling: 10 samples, 16:1, 300 mV trim. Raw 443 is what
    // a unit on a 3S pack read on 2026-10-26, which its HUD showed as 12.1 V.
    CHECK(b.supply.average == 10);
    CHECK(bsp::adc_mv(b.supply.adc, 443) == 778);
    CHECK(bsp::supply_mv(b.supply, 778) == 12148);
    CHECK(bsp::supply_mv(b.supply, bsp::adc_mv(b.supply.adc, 1023)) == 28468);  // full scale, 6S

    CHECK(std::string(b.radio.open8030_fw) == "/lib/firmware/open8030-vrxpro-usb.img");
}

static void test_pc_board_has_nothing() {
    const bsp::Board &b = bsp::pc();
    CHECK(std::string(b.name) == "pc");
    VrxButtons panel(b.buttons);
    VrxBindButton bind(b.bind);
    CHECK(!panel.available());
    CHECK(!bind.available());
    CHECK(*b.supply.adc.path == 0);
    CHECK(*b.radio.open8030_fw == 0);
}

static void test_panel() {
    FakeAdc adc;
    bsp::AdcLadder l = bsp::vrx_pro().buttons;
    l.adc.path = adc.path.c_str();
    VrxButtons b(l);
    CHECK(b.available());
    uint64_t t = 1000;

    // Idle (a unit reads 11 counts, about 19 mV) is no key, however long.
    adc.set(11);
    for (int i = 0; i < 10; i++) { CHECK(b.poll(t) == VrxButtons::VB_NONE); t += 25; }

    // Each key at its centre, after the debounce, then released.
    for (size_t i = 0; i < l.n_keys; i++) {
        adc.set(FakeAdc::raw_for(l.adc, l.keys[i].mv));
        CHECK(settle(b, t) == (VrxButtons::Key)l.keys[i].key);
        adc.set(11);
        settle(b, t);
    }

    // The debounce: a press reports on the third poll, not before.
    const int ok_mv = 1400;
    adc.set(FakeAdc::raw_for(l.adc, ok_mv));
    CHECK(b.poll(t) == VrxButtons::VB_NONE); t += 25;
    CHECK(b.poll(t) == VrxButtons::VB_NONE); t += 25;
    CHECK(b.poll(t) == VrxButtons::VB_OK);   t += 25;
    // OK held: no repeat, ever.
    for (int i = 0; i < 80; i++) { CHECK(b.poll(t) == VrxButtons::VB_NONE); t += 25; }
    adc.set(11); settle(b, t);

    // Just inside and just outside a key's window.
    adc.set(FakeAdc::raw_for(l.adc, ok_mv + l.tolerance_mv - 3));
    CHECK(settle(b, t) == VrxButtons::VB_OK);
    adc.set(11); settle(b, t);
    adc.set(FakeAdc::raw_for(l.adc, ok_mv + l.tolerance_mv + 5));
    CHECK(settle(b, t) == VrxButtons::VB_NONE);
    adc.set(11); settle(b, t);

    // At the floor nothing is down.
    adc.set(FakeAdc::raw_for(l.adc, l.floor_mv) - 1);
    CHECK(settle(b, t) == VrxButtons::VB_NONE);

    // UP held: one press, then repeats after 450 ms and every 140 ms.
    adc.set(FakeAdc::raw_for(l.adc, 800));
    CHECK(settle(b, t) == VrxButtons::VB_UP);
    const uint64_t pressed = t - 25;
    int repeats = 0;
    while (t < pressed + 450 + 140 * 3 + 10) {
        if (b.poll(t) == VrxButtons::VB_UP) repeats++;
        t += 10;
    }
    CHECK(repeats == 4);   // at 450, 590, 730, 870 ms
    adc.set(11); settle(b, t);
}

static void test_bind() {
    FakeAdc adc;
    bsp::AdcButton bb = bsp::vrx_pro().bind;
    bb.adc.path = adc.path.c_str();
    VrxBindButton b(bb);
    CHECK(b.available());

    adc.set(1023);   // idle high
    for (int i = 0; i < 10; i++) CHECK(!b.pressed());

    // A press is reported once, on the third read, and never again while held.
    adc.set(10);
    CHECK(!b.pressed());
    CHECK(!b.pressed());
    CHECK(b.pressed());
    for (int i = 0; i < 50; i++) CHECK(!b.pressed());

    // Released and pressed again: a second bind.
    adc.set(1023);
    for (int i = 0; i < 3; i++) CHECK(!b.pressed());
    adc.set(10);
    int presses = 0;
    for (int i = 0; i < 5; i++) presses += b.pressed();
    CHECK(presses == 1);

    // A channel that can't be read is never pressed.
    bsp::AdcButton gone = bb;
    gone.adc.path = "/nonexistent/in_voltage0_raw";
    VrxBindButton missing(gone);
    CHECK(!missing.available());
    for (int i = 0; i < 5; i++) CHECK(!missing.pressed());
}

int main() {
    test_vrx_pro_tables();
    test_pc_board_has_nothing();
    test_panel();
    test_bind();
    std::printf(fails ? "bsp_test: %d FAILED\n" : "bsp_test: all passed\n", fails);
    return fails ? 1 : 0;
}
