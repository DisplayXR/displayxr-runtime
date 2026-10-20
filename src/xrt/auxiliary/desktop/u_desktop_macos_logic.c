// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS desktop control: the capture backend's geometry decisions as
 *         pure functions. See u_desktop_macos_logic.h.
 */

#include "u_desktop_macos_logic.h"
#include "u_desktop_capture.h"

double
u_desktop_macos_display_scale(uint32_t pixel_w, uint32_t point_w)
{
	if (pixel_w == 0 || point_w == 0) {
		return 1.0;
	}
	double s = (double)pixel_w / (double)point_w;
	if (!(s >= 1.0)) {
		return 1.0;
	}
	return s > 8.0 ? 8.0 : s;
}

//! Device-pixel length of @p points at @p scale (at least 1).
static uint32_t
px_len(int32_t points, double scale)
{
	// ceil without libm (this file is compiled on every OS).
	double v = (double)points * scale - 1e-6;
	if (!(v > 0.0)) {
		return 1u;
	}
	uint32_t u = (uint32_t)v;
	return (double)u < v ? u + 1u : (u == 0 ? 1u : u);
}

bool
u_desktop_macos_capture_plan(const struct u_desktop_rect *src,
                             const struct u_desktop_macos_display *d,
                             uint32_t n,
                             struct u_desktop_macos_capture_part *parts,
                             uint32_t cap,
                             uint32_t *nparts,
                             uint32_t *img_w,
                             uint32_t *img_h)
{
	*nparts = 0;
	*img_w = 0;
	*img_h = 0;
	if (src->w <= 0 || src->h <= 0) {
		return false;
	}
	double scale = 0.0;
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		struct u_desktop_rect part;
		if (!u_desktop_rect_intersect(src, &d[i].bounds, &part)) {
			continue;
		}
		double s = d[i].scale >= 1.0 ? d[i].scale : 1.0;
		scale = s > scale ? s : scale;
		if (k < cap) {
			struct u_desktop_macos_capture_part *p = &parts[k];
			p->display = i;
			p->rect = part;
			p->local =
			    (struct u_desktop_rect){part.x - d[i].bounds.x, part.y - d[i].bounds.y, part.w, part.h};
			p->px_w = px_len(part.w, s);
			p->px_h = px_len(part.h, s);
		}
		k++;
	}
	if (k == 0) {
		return false;
	}
	uint32_t w = px_len(src->w, scale), h = px_len(src->h, scale);
	if (w > U_DESKTOP_CAPTURE_MAX_NATIVE || h > U_DESKTOP_CAPTURE_MAX_NATIVE) {
		return false;
	}
	*nparts = k < cap ? k : cap;
	*img_w = w;
	*img_h = h;
	return true;
}

bool
u_desktop_macos_capture_withholds(const struct u_desktop_macos_window_info *w, uint32_t own_pid)
{
	if (!(w->alpha > 0.0) || w->frame.w <= 0 || w->frame.h <= 0) {
		return false; // not drawn: nothing of it is in the picture
	}
	return w->pid == own_pid || w->sharing == 0;
}
