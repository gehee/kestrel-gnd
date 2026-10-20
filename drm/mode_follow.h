#pragma once
/* Screen Mode Auto in step with the camera: the mode picked for a frame rate.
 * Header-only, so the boot splash (splash.c, a program of its own) picks
 * exactly what kestrel-gnd will: a different mode at the hand-off is a timing
 * change, and the screen goes dark while it relocks. */
#include <stdint.h>
#include <xf86drmMode.h>

/* A mode's refresh in Hz, from its timings (vrefresh is rounded). */
static inline double modeset_mode_hz(const drmModeModeInfo *m)
{
	return m->htotal && m->vtotal ? m->clock * 1000.0 / ((double)m->htotal * m->vtotal) : 0;
}

/* The connector's mode whose refresh is the highest whole multiple of fps, to
 * 0.05% (a 100.002 Hz mode for 100 fps, not a 59.94 one for 60), the largest
 * at that rate, then the closest; only w x h ones if w and h are non-zero.
 * -1 if there is none. */
static inline int modeset_pick_follow(const drmModeConnector *conn, uint32_t fps, uint16_t w, uint16_t h)
{
	int best = -1;
	uint32_t best_rate = 0, best_area = 0;
	double best_err = 0;

	if (!fps)
		return -1;
	for (int i = 0; i < conn->count_modes; i++) {
		const drmModeModeInfo *m = &conn->modes[i];
		if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
			continue;
		if (w && h && (m->hdisplay != w || m->vdisplay != h))
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
		const uint32_t area = (uint32_t)m->hdisplay * m->vdisplay;
		if (best < 0 || rate > best_rate ||
		    (rate == best_rate && (area > best_area || (area == best_area && err < best_err)))) {
			best = i;
			best_rate = rate;
			best_area = area;
			best_err = err;
		}
	}
	return best;
}
