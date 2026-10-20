// The radio's TDD frame found from its state word (the same file is
// kestrel-air's src/radio/frame_edge.h - keep the two equal).
//
// The AR8030 runs a fixed frame: the video part (the "br" entry, user 8 of
// its frame table: sent by the air unit, received by the ground), switches,
// the uplink slot, padding - whole 0.64 us ticks, by the video link's
// bandwidth and the frame mode (SET 16): 4650.88 / 5633.92 / 7964.80 us at
// 20 MHz in modes 2 / 0 / 1, 3250.56 / 4233.60 / 5398.40 at 40 MHz (measured
// 2026-10-14). Its TDD state word (0x0a110e6c, byte
// 0x0e6e through GET 0x01000064) says which entry runs now. Each read is timed
// by our clock before and after, and was taken somewhere in between: one that
// saw the video part went out before that part ended, one that saw what comes
// after came back after it. From many reads this finds the frame's period and
// the moment its video part ends (the edge), on our own clock, to some 20 us
// a second although a read takes 0.2-0.5 ms (and longer just after the edge,
// when the radio is busy). Plain C that is also C++.
#ifndef FRAME_EDGE_H
#define FRAME_EDGE_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FE_TICK_US 0.64     // the radio's frame unit

typedef struct {
    uint64_t t0;            // our clock when the read went out, us
    uint32_t rtt;           // until it came back, us
    uint8_t video;          // the video part was running
} fe_read;

// The state byte (0x0a110e6e): [3:0] the running entry's user, [5:4] its
// direction code (1 receive, 2 send; 0 and 3 the idle ends around an entry).
static inline int fe_is_video(uint8_t b) {
    const int code = (b >> 4) & 3;
    return (b & 15) == 8 && (code == 1 || code == 2);
}

static int fe_cmp_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// The round trip under which a read counts: its 30th percentile, at least 150 us.
static uint32_t fe_fast_cap(const fe_read *r, int n) {
    if (n <= 0) return 0;
    uint32_t *v = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
    if (!v) return 0;
    for (int i = 0; i < n; i++) v[i] = r[i].rtt;
    qsort(v, (size_t)n, sizeof(uint32_t), fe_cmp_u32);
    uint32_t c = v[n * 3 / 10];
    free(v);
    return c < 150 ? 150 : c;
}

// Where x falls in [-T/2, T/2).
static inline double fe_wrap(double x, double T) {
    x = fmod(x, T);
    if (x >= T / 2) x -= T;
    if (x < -T / 2) x += T;
    return x;
}

// Share of the fast reads (x: us after the first, v: saw video) a period
// sorts right (bins of the frame, each its majority's): ~1 at the frame's
// period, ~the video share elsewhere. A multiply and a cut, not fmod: musl's
// is a loop over the bits, and this runs some 7000 times on a second and a
// half of reads - the search took 2-3 s of the air unit's Cortex-A7 with it.
static double fe_purity(const double *x, const uint8_t *v, int n, double T, int bins) {
    int h[256][2];
    memset(h, 0, sizeof(int) * 2 * (size_t)bins);
    if (n <= 0) return 0;
    const double inv = 1.0 / T;
    for (int i = 0; i < n; i++) {
        const double f = x[i] * inv;
        int b = (int)((f - (double)(int)f) * bins);
        if (b >= bins) b = bins - 1;
        h[b][v[i]]++;
    }
    int good = 0;
    for (int b = 0; b < bins; b++) good += h[b][0] > h[b][1] ? h[b][0] : h[b][1];
    return (double)good / n;
}

// Following it: s, where the edge is from E, by the tightest bounds the reads
// set on it - only reads within FE_NEAR of it (the video part runs ~3 ms
// before it at 20 MHz, about half that at 40, and the next one starts 1.5 ms
// or more after it, so these cannot see another video part). One that saw
// video went out before the edge; one that saw something else came back
// after it. width: how far apart the two tightest are. 0: measured.
#define FE_NEAR 1200.0
static int fe_measure(const fe_read *r, int n, double E, double T, double *s, double *width) {
    double L = -1e18, U = 1e18;
    int nl = 0, nu = 0;
    for (int i = 0; i < n; i++) {
        const double x0 = fe_wrap((double)r[i].t0 - E, T), x1 = x0 + r[i].rtt;
        if (x0 < -FE_NEAR || x1 > FE_NEAR) continue;
        if (r[i].video) { if (x0 > L) L = x0; nl++; }
        else { if (x1 < U) U = x1; nu++; }
    }
    if (nl < 5 || nu < 5 || U - L <= 0 || U - L > 1500) return -1;
    *s = (L + U) / 2;
    *width = U - L;
    return 0;
}

// The periods searched, us: every frame above, with room.
#define FE_T_MIN 3000.0
#define FE_T_MAX 8500.0

// The period within 1.5 us of c that sorts the reads best (the middle of the
// best run), and how well.
static double fe_refine(const double *x, const uint8_t *v, int nf, double c, double *best_out) {
    double best = 0, T = c, t_hi = c;
    for (double t = c - 1.5; t <= c + 1.5; t += 0.01) {
        const double p = fe_purity(x, v, nf, t, 256);
        if (p > best) { best = p; T = t_hi = t; }
        else if (p == best && t - t_hi < 0.015) t_hi = t;
    }
    *best_out = best;
    return (T + t_hi) / 2;
}

// From reads over a second or more: the frame's period on our clock and the
// time of a video part's end near the newest read. 0: found.
static int fe_acquire(const fe_read *r, int n, double *T_out, double *E_out, double *purity_out) {
    if (n < 300) return -1;
    const uint32_t cap = fe_fast_cap(r, n);
    // The period search over the fast reads only (a thousand or so: it runs
    // some 5500 times).
    double *x = (double *)malloc((sizeof(double) + 1) * (size_t)n);
    if (!x) return -3;
    uint8_t *v = (uint8_t *)(x + n);
    int nf = 0;
    uint64_t t0 = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].rtt > cap) continue;
        if (!nf) t0 = r[i].t0;
        x[nf] = (double)(r[i].t0 - t0);
        v[nf++] = r[i].video ? 1 : 0;
    }
    double best = 0, T = 0;
    for (double t = FE_T_MIN; t <= FE_T_MAX; t += 1.0) {
        const double p = fe_purity(x, v, nf, t, 32);
        if (p > best) { best = p; T = t; }
    }
    // Two or three frames sort the reads about as well as one - better, on
    // these 1 us steps, when the nearest step to a short frame is 0.5 us off
    // (0.2 ms of drift over the reads at 3.25 ms) - so the shortest of T, T/2
    // and T/3 that sorts them. (40 MHz mode 2's 3250.56 was found as 6501.12,
    // half its edges, while the search started at 3.5 ms.)
    const double c = T;
    T = fe_refine(x, v, nf, c, &best);
    for (int k = 3; k >= 2; k--) {
        if (c / k < FE_T_MIN - 1.5) continue;
        double pk;
        const double Tk = fe_refine(x, v, nf, c / k, &pk);
        if (pk >= 0.95 && pk >= best - 0.02) { T = Tk; best = pk; break; }
    }
    free(x);
    if (purity_out) *purity_out = best;
    if (best < 0.95) return -2;
    // The video part's middle (it is one run) and length, so its end - by
    // time, not by reads: they come back faster in some parts than in others.
    const uint64_t base = r[0].t0;
    enum { B = 128 };
    int h[B][2];
    memset(h, 0, sizeof(h));
    double sx = 0, sy = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].rtt > cap) continue;
        const double ph = fmod((double)(r[i].t0 - base), T);
        h[(int)(ph / T * B) % B][r[i].video ? 1 : 0]++;
        if (!r[i].video) continue;
        sx += cos(ph / T * 2 * M_PI); sy += sin(ph / T * 2 * M_PI);
    }
    int vb = 0;
    for (int b = 0; b < B; b++) vb += h[b][1] > h[b][0];
    double mid = atan2(sy, sx) / (2 * M_PI) * T;
    if (mid < 0) mid += T;
    const double vlen = (double)vb / B * T;
    // Then the edge in each quarter of the reads: a line through those gives
    // the period's error (its slope) and the edge (its middle). Twice.
    const double t_mid = ((double)r[0].t0 + (double)r[n - 1].t0) / 2;
    double E = (double)base + mid + vlen / 2;
    E += floor((t_mid - E) / T + 0.5) * T;
    for (int pass = 0; pass < 2; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int k = 0;
        for (int q = 0; q < 4; q++) {
            const int a = n * q / 4, z = n * (q + 1) / 4;
            double s, w;
            if (z - a < 20 || fe_measure(r + a, z - a, E, T, &s, &w)) continue;
            const double x = (((double)r[a].t0 + (double)r[z - 1].t0) / 2 - t_mid) / T;   // frames
            sx += x; sy += s; sxx += x * x; sxy += x * s;
            k++;
        }
        if (k < 3) return -4;
        const double slope = (k * sxy - sx * sy) / (k * sxx - sx * sx), icpt = (sy - slope * sx) / k;
        T += slope;
        E += icpt;
    }
    E += floor(((double)r[n - 1].t0 - E) / T) * T;
    *T_out = T;
    *E_out = E;
    return 0;
}

#endif
