// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-segment views geometry (multi-screen M3) — see oxr_segment_views.h.
 * @ingroup oxr_main
 */

#include "oxr_segment_views.h"

#include "util/u_weave_rect_parts.h"

#include <string.h>


uint32_t
oxr_segment_views_majority(const struct xrt_segment_metrics *m)
{
	return u_segment_majority(m);
}

bool
oxr_segment_views_layout(const struct xrt_segment_metrics *m, struct oxr_segment_layout *out)
{
	return u_segment_layout_compute(m, out);
}

void
oxr_segment_views_window_metrics(const struct xrt_segment_metrics *m,
                                 const struct oxr_segment_layout *l,
                                 uint32_t i,
                                 struct xrt_window_metrics *out)
{
	u_segment_window_metrics(m, l, i, out);
}

void
oxr_segment_views_whole_window_metrics(const struct xrt_segment_metrics *m, uint32_t i, struct xrt_window_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (m == NULL || i >= m->count || m->window_pixel_width == 0 || m->window_pixel_height == 0) {
		return;
	}
	const struct xrt_segment_metric *s = &m->seg[i];
	if (s->screen_desktop_width == 0 || s->screen_desktop_height == 0 || s->screen_width_m <= 0.0f ||
	    s->screen_height_m <= 0.0f) {
		return;
	}
	u_segment_fill_display(s, out);
	const float px = u_segment_pitch_x(s);
	const float py = u_segment_pitch_y(s);
	out->window_pixel_width = m->window_pixel_width;
	out->window_pixel_height = m->window_pixel_height;
	out->window_screen_left = m->window_screen_left;
	out->window_screen_top = m->window_screen_top;
	out->window_width_m = (float)m->window_pixel_width * px;
	out->window_height_m = (float)m->window_pixel_height * py;
	const float wcx = (float)(m->window_screen_left - s->screen_desktop_left) + (float)m->window_pixel_width * 0.5f;
	const float wcy = (float)(m->window_screen_top - s->screen_desktop_top) + (float)m->window_pixel_height * 0.5f;
	out->window_center_offset_x_m = (wcx - (float)s->screen_desktop_width * 0.5f) * px;
	out->window_center_offset_y_m = -(wcy - (float)s->screen_desktop_height * 0.5f) * py;
	out->window_center_offset_z_m = m->window_depth_m; // 0 for an OS window (ADR-052 D2)
	out->valid = true;
}

uint32_t
oxr_segment_views_zone_segment(const struct xrt_segment_metrics *m, const struct xrt_rect *zone, uint32_t fallback)
{
	const int32_t k = u_weave_rect_majority_segment(m, zone);
	if (k < 0) {
		return fallback;
	}
	const struct xrt_segment_metric *s = &m->seg[k];
	if (s->screen_width_m <= 0.0f || s->screen_height_m <= 0.0f || s->screen_desktop_width == 0 ||
	    s->screen_desktop_height == 0) {
		return fallback;
	}
	return (uint32_t)k;
}

uint32_t
oxr_segment_views_zone_parts(const struct xrt_segment_metrics *m,
                             const struct xrt_rect *zone,
                             struct xrt_segment_metrics *parts,
                             struct xrt_rect zone_rel[XRT_MAX_SEGMENTS],
                             struct oxr_segment_layout *layout)
{
	if (parts == NULL || layout == NULL) {
		return 0;
	}
	const uint32_t n = u_weave_zone_part_table(m, zone, parts, zone_rel);
	if (n < 2) {
		return 0;
	}
	// Every part needs its screen's size and desktop rect (the layout checks).
	if (!oxr_segment_views_layout(parts, layout)) {
		return 0;
	}
	return n;
}

uint32_t
oxr_segment_views_assign(uint32_t segment_count,
                         uint32_t per_segment,
                         uint32_t reported_view_count,
                         uint32_t out_first[XRT_MAX_SEGMENTS],
                         uint32_t out_count[XRT_MAX_SEGMENTS])
{
	if (segment_count == 0 || segment_count > XRT_MAX_SEGMENTS || per_segment == 0) {
		return 0;
	}
	const uint32_t total = segment_count * per_segment;
	if (total > reported_view_count || total > XRT_MAX_VIEWS) {
		return 0;
	}
	for (uint32_t k = 0; k < segment_count; k++) {
		out_first[k] = k * per_segment;
		out_count[k] = per_segment;
	}
	return total;
}
