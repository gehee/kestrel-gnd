// Where each strip's decoder writes its rows of the whole picture
// (hal/artosyn/wave5_strips: hal::StripLayout), checked without the hardware:
// 2, 3 and 4 strips of a 1080p picture as kestrel-air cuts it (16 rows past
// each join, each strip's coded height rounded up to 8 rows) - the shown rows
// stack to 1080, each strip's first shown row lands where the strip above
// stops showing, every plane offset is 32-byte aligned (the decoder's rule),
// the planes are tall enough for every coded row, and layouts the decoders
// could not share are refused.
#include "hal/artosyn/wave5_strips.hpp"
#include <cstdio>
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    const uint32_t W = 1920, H = 1080;
    const int ov = 16;
    for (int n = 2; n <= 4; n++) {
        uint32_t visible[4], coded[4];
        for (int k = 0; k < n; k++) {
            visible[k] = H / n + (k ? ov : 0) + (k < n - 1 ? ov : 0);
            coded[k] = (visible[k] + 7) / 8 * 8;
        }
        hal::StripLayout l;
        CHECK(hal::StripLayout::make(n, ov, W, visible, coded, &l));
        CHECK(l.w == W && l.h == H && l.n == n);
        uint32_t y = 0;
        for (int k = 0; k < n; k++) {
            const uint32_t top = k ? ov : 0;
            CHECK(l.origin[k] + top == y);                 // its first shown row where the one above stops
            CHECK(l.origin[k] + coded[k] <= l.hp);         // every coded row inside the planes
            CHECK(l.off[k][0] == l.origin[k] * W);
            CHECK(l.off[k][1] == W * l.hp + l.origin[k] / 2 * (W / 2));
            CHECK(l.off[k][2] == W * l.hp * 5 / 4 + l.origin[k] / 2 * (W / 2));
            for (int i = 0; i < 3; i++) CHECK(!(l.off[k][i] & 31));
            y += H / n;
        }
        CHECK(y == H);
        CHECK(l.plane[0] == 0 && l.plane[1] == W * l.hp && l.plane[2] == W * l.hp * 5 / 4);
        CHECK(l.hp % 2 == 0 && l.hp >= H && l.hp < H + 16);
        CHECK(l.bytes() == (size_t)W * l.hp * 3 / 2);
    }
    // 864p (1536 wide, 2 strips) lays out too.
    {
        const uint32_t v[2] = {448, 448}, c[2] = {448, 448};
        hal::StripLayout l;
        CHECK(hal::StripLayout::make(2, 16, 1536, v, c, &l) && l.h == 864 && l.origin[1] == 416);
    }
    // Refused: one strip, odd rows, an odd overlap, a width whose chroma rows
    // break the alignment, a strip that is all overlap, fewer coded rows
    // than shown.
    const uint32_t v[2] = {556, 556}, c[2] = {560, 560};
    hal::StripLayout l;
    CHECK(!hal::StripLayout::make(1, 16, W, v, c, &l));
    const uint32_t odd[2] = {555, 556};
    CHECK(!hal::StripLayout::make(2, 16, W, odd, c, &l));
    CHECK(!hal::StripLayout::make(2, 15, W, v, c, &l));
    CHECK(!hal::StripLayout::make(2, 16, 1952, v, c, &l));
    const uint32_t thin[2] = {16, 556};
    CHECK(!hal::StripLayout::make(2, 16, W, thin, c, &l));
    const uint32_t short_coded[2] = {552, 560};
    CHECK(!hal::StripLayout::make(2, 16, W, v, short_coded, &l));
    if (fails) return 1;
    std::puts("wave5 strips: 2/3/4 strips stack to 1080 rows in one buffer, every plane aligned, bad layouts refused");
    return 0;
}
