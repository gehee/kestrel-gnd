#pragma once
/* Screen Mode Auto: which of the screen's modes kestrel-gnd starts it on.
 * Header-only, so the boot splash (splash.c, a program of its own) picks
 * exactly what kestrel-gnd will: a different mode at the hand-off is a timing
 * change, and the screen goes dark while it relocks.
 *
 * The camera plays no part (its frame rate can change at any time; the live
 * re-pick, modeset_pick_rate, follows it at the size Auto chose). Only the
 * modes the screen advertises, so only timings it says it takes. In order:
 *   1. 1920x1080 above 60 Hz        5. 1280x720 at 60 Hz or less
 *   2. 1280x720 above 60 Hz         6. any other 16:9 size
 *   3. another 16:9 size above 60   7. any other shape (16:10, 4:3 ...)
 *      Hz, 720 lines or more
 *   4. 1920x1080 at 60 Hz or less
 * the fastest first within each (1080p240 before 1080p144 before 1080p120),
 * then the closest to 1080 lines, then the larger. So 1080p100 rather than
 * 720p240, 720p100 rather than 1080p60, 1080p60 rather than 720p60.
 *
 * modeset_auto_order lists every usable mode in that order: kestrel-gnd tries
 * them until the kernel takes one (drm.c), so a mode the display controller
 * cannot drive costs a fallback, not a black screen. */
#include <stdint.h>
#include <stdlib.h>
#include <xf86drmMode.h>

/* A mode's refresh in Hz, from its timings (vrefresh is rounded). */
static inline double modeset_mode_hz(const drmModeModeInfo *m)
{
	return m->htotal && m->vtotal ? m->clock * 1000.0 / ((double)m->htotal * m->vtotal) : 0;
}

static inline int modeset_mode_usable(const drmModeModeInfo *m)
{
	return !(m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN));
}

/* 16:9 to 1%: 1366x768 and 1360x768 count, 1920x1200 does not. */
static inline int modeset_mode_16_9(const drmModeModeInfo *m)
{
	const long a = (long)m->hdisplay * 9, b = (long)m->vdisplay * 16;
	return labs(a - b) * 100 <= b;
}

/* Fast enough to prefer: above 60 Hz (a 60.05 Hz mode is still 60). */
static inline int modeset_mode_fast(const drmModeModeInfo *m)
{
	return modeset_mode_hz(m) > 60.5;
}

/* The group of the list at the top a mode is in, 1-7 (wide: the screen has
 * a 16:9 mode). */
static inline int modeset_auto_group(const drmModeModeInfo *m, int wide)
{
	const int fhd = m->hdisplay == 1920 && m->vdisplay == 1080;
	const int hd = m->hdisplay == 1280 && m->vdisplay == 720;

	if (wide && !modeset_mode_16_9(m))
		return 7;
	if (modeset_mode_fast(m))
		return fhd ? 1 : hd ? 2 : m->vdisplay >= 720 ? 3 : 6;
	return fhd ? 4 : hd ? 5 : 6;
}

/* Whether mode a comes before mode b in Auto's order. */
static inline int modeset_auto_before(const drmModeModeInfo *a, const drmModeModeInfo *b, int wide)
{
	const int ga = modeset_auto_group(a, wide), gb = modeset_auto_group(b, wide);
	if (ga != gb)
		return ga < gb;
	const double ha = modeset_mode_hz(a), hb = modeset_mode_hz(b);
	if (ha > hb + 0.5 || ha < hb - 0.5)
		return ha > hb;
	const int da = abs((int)a->vdisplay - 1080), db = abs((int)b->vdisplay - 1080);
	if (da != db)
		return da < db;
	return (uint32_t)a->hdisplay * a->vdisplay > (uint32_t)b->hdisplay * b->vdisplay;
}

/* The connector's usable modes (indexes) in Auto's order, at most max of
 * them. Returns how many. */
static inline int modeset_auto_order(const drmModeConnector *conn, int *order, int max)
{
	int wide = 0, n = 0;

	for (int i = 0; i < conn->count_modes; i++)
		if (modeset_mode_usable(&conn->modes[i]) && modeset_mode_16_9(&conn->modes[i]))
			wide = 1;
	for (int i = 0; i < conn->count_modes; i++) {
		if (!modeset_mode_usable(&conn->modes[i]))
			continue;
		int j = n < max ? n++ : max;
		for (; j > 0 && modeset_auto_before(&conn->modes[i], &conn->modes[order[j - 1]], wide); j--)
			if (j < max)
				order[j] = order[j - 1];
		if (j < max)
			order[j] = i;
	}
	return n;
}

/* Auto's mode: the first of modeset_auto_order. -1 if the screen has no
 * usable mode. */
static inline int modeset_pick_auto(const drmModeConnector *conn)
{
	int first;
	return modeset_auto_order(conn, &first, 1) ? first : -1;
}

/* The w x h mode whose refresh is the highest whole multiple of fps, to 0.05%
 * (a 100.002 Hz mode for 100 fps, not a 59.94 one for 60), then the closest;
 * only modes above 60 Hz if fast_only. -1 if there is none. */
static inline int modeset_pick_follow(const drmModeConnector *conn, uint32_t fps, uint16_t w, uint16_t h,
                                      int fast_only)
{
	int best = -1;
	uint32_t best_rate = 0;
	double best_err = 0;

	if (!fps)
		return -1;
	for (int i = 0; i < conn->count_modes; i++) {
		const drmModeModeInfo *m = &conn->modes[i];
		if (!modeset_mode_usable(m) || m->hdisplay != w || m->vdisplay != h)
			continue;
		if (fast_only && !modeset_mode_fast(m))
			continue;
		const double hz = modeset_mode_hz(m);
		const uint32_t k = (uint32_t)(hz / fps + 0.5);
		if (k < 1)
			continue;
		const uint32_t rate = k * fps;
		double err = (hz - rate) / rate;
		if (err < 0)
			err = -err;
		if (err > 0.0005)
			continue;
		if (best < 0 || rate > best_rate || (rate == best_rate && err < best_err)) {
			best = i;
			best_rate = rate;
			best_err = err;
		}
	}
	return best;
}

/* The refresh at w x h for the camera's fps, once it is known: above 60 Hz
 * if the screen has any there, the highest in step with fps (0: the
 * fastest), else the highest. *in_step (if given) says which. -1 if the
 * screen has no w x h mode. */
static inline int modeset_pick_rate(const drmModeConnector *conn, uint32_t fps, uint16_t w, uint16_t h,
                                    int *in_step)
{
	int fast = 0, best = -1;

	for (int i = 0; i < conn->count_modes; i++) {
		const drmModeModeInfo *m = &conn->modes[i];
		if (modeset_mode_usable(m) && m->hdisplay == w && m->vdisplay == h && modeset_mode_fast(m))
			fast = 1;
	}
	best = modeset_pick_follow(conn, fps, w, h, fast);
	if (in_step)
		*in_step = best >= 0;
	if (best >= 0)
		return best;
	for (int i = 0; i < conn->count_modes; i++) {
		const drmModeModeInfo *m = &conn->modes[i];
		if (!modeset_mode_usable(m) || m->hdisplay != w || m->vdisplay != h)
			continue;
		if (best < 0 || modeset_mode_hz(m) > modeset_mode_hz(&conn->modes[best]))
			best = i;
	}
	return best;
}
