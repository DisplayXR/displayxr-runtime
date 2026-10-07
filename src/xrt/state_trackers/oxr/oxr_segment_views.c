// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-segment views geometry (multi-screen M3) — see oxr_segment_views.h.
 * @ingroup oxr_main
 */

#include "oxr_segment_views.h"

#include <string.h>


static float
pitch_x(const struct xrt_segment_metric *s)
{
	return s->screen_desktop_width > 0 ? s->screen_width_m / (float)s->screen_desktop_width : 0.0f;
}

static float
pitch_y(const struct xrt_segment_metric *s)
{
	return s->screen_desktop_height > 0 ? s->screen_height_m / (float)s->screen_desktop_height : 0.0f;
}

/*!
 * Distance along one axis from the majority centre to a segment centre, in
 * metres: the majority pitch up to the majority edge, the segment's own pitch
 * beyond it. @p d is the centre-to-centre distance in window px, @p half the
 * majority segment's half extent in window px.
 */
static float
seam_distance(float d, float half, float pitch_major, float pitch_own)
{
	const float ad = d < 0.0f ? -d : d;
	const float sign = d < 0.0f ? -1.0f : 1.0f;
	if (ad <= half) {
		return d * pitch_major;
	}
	return sign * (half * pitch_major + (ad - half) * pitch_own);
}

uint32_t
oxr_segment_views_majority(const struct xrt_segment_metrics *m)
{
	if (m == NULL || m->count == 0) {
		return 0;
	}
	uint32_t best = 0;
	uint64_t best_area = 0;
	bool best_primary = false;
	for (uint32_t i = 0; i < m->count && i < XRT_MAX_SEGMENTS; i++) {
		const struct xrt_rect *r = &m->seg[i].window_rect;
		const uint64_t area =
		    (uint64_t)(r->extent.w > 0 ? r->extent.w : 0) * (uint64_t)(r->extent.h > 0 ? r->extent.h : 0);
		const bool primary = m->seg[i].is_primary;
		// Strictly larger wins; an exact tie goes to the primary screen, else
		// keeps the earlier (leftmost) segment.
		if (i == 0 || area > best_area || (area == best_area && primary && !best_primary)) {
			best = i;
			best_area = area;
			best_primary = primary;
		}
	}
	return best;
}

bool
oxr_segment_views_layout(const struct xrt_segment_metrics *m, struct oxr_segment_layout *out)
{
	if (m == NULL || out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (m->count == 0 || m->count > XRT_MAX_SEGMENTS) {
		return false;
	}

	for (uint32_t i = 0; i < m->count; i++) {
		const struct xrt_segment_metric *s = &m->seg[i];
		if (s->screen_width_m <= 0.0f || s->screen_height_m <= 0.0f || s->screen_desktop_width == 0 ||
		    s->screen_desktop_height == 0 || s->window_rect.extent.w <= 0 || s->window_rect.extent.h <= 0) {
			return false;
		}
		const float px = pitch_x(s);
		const float py = pitch_y(s);
		const float scx = (float)s->screen_rect.offset.w + (float)s->screen_rect.extent.w * 0.5f;
		const float scy = (float)s->screen_rect.offset.h + (float)s->screen_rect.extent.h * 0.5f;
		struct oxr_segment_place *p = &out->seg[i];
		p->own_cx = (scx - (float)s->screen_desktop_width * 0.5f) * px;
		p->own_cy = -(scy - (float)s->screen_desktop_height * 0.5f) * py; // +Y up
		p->w_m = (float)s->window_rect.extent.w * px;
		p->h_m = (float)s->window_rect.extent.h * py;
	}

	const uint32_t mj = oxr_segment_views_majority(m);
	const struct xrt_segment_metric *ms = &m->seg[mj];
	const float mcx = (float)ms->window_rect.offset.w + (float)ms->window_rect.extent.w * 0.5f;
	const float mcy = (float)ms->window_rect.offset.h + (float)ms->window_rect.extent.h * 0.5f;
	const float half_x = (float)ms->window_rect.extent.w * 0.5f;
	const float half_y = (float)ms->window_rect.extent.h * 0.5f;

	float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
	for (uint32_t i = 0; i < m->count; i++) {
		const struct xrt_segment_metric *s = &m->seg[i];
		struct oxr_segment_place *p = &out->seg[i];
		if (i == mj) {
			p->ref_cx = p->own_cx;
			p->ref_cy = p->own_cy;
		} else {
			const float cx = (float)s->window_rect.offset.w + (float)s->window_rect.extent.w * 0.5f;
			const float cy = (float)s->window_rect.offset.h + (float)s->window_rect.extent.h * 0.5f;
			p->ref_cx = out->seg[mj].own_cx + seam_distance(cx - mcx, half_x, pitch_x(ms), pitch_x(s));
			// Window px grow downward, display space grows up.
			p->ref_cy = out->seg[mj].own_cy - seam_distance(cy - mcy, half_y, pitch_y(ms), pitch_y(s));
		}
		const float l = p->ref_cx - p->w_m * 0.5f;
		const float r = p->ref_cx + p->w_m * 0.5f;
		const float b = p->ref_cy - p->h_m * 0.5f;
		const float t = p->ref_cy + p->h_m * 0.5f;
		if (i == 0 || l < x0) {
			x0 = l;
		}
		if (i == 0 || r > x1) {
			x1 = r;
		}
		if (i == 0 || b < y0) {
			y0 = b;
		}
		if (i == 0 || t > y1) {
			y1 = t;
		}
	}
	out->count = m->count;
	out->majority = mj;
	out->window_ref_cx = (x0 + x1) * 0.5f;
	out->window_ref_cy = (y0 + y1) * 0.5f;
	out->window_ref_w = x1 - x0;
	out->window_ref_h = y1 - y0;
	return true;
}

static void
fill_display(const struct xrt_segment_metric *s, struct xrt_window_metrics *out)
{
	out->display_width_m = s->screen_width_m;
	out->display_height_m = s->screen_height_m;
	out->display_pixel_width = s->screen_desktop_width;
	out->display_pixel_height = s->screen_desktop_height;
	out->display_screen_left = s->screen_desktop_left;
	out->display_screen_top = s->screen_desktop_top;
	out->window_orientation = (struct xrt_quat){0.0f, 0.0f, 0.0f, 1.0f};
}

void
oxr_segment_views_window_metrics(const struct xrt_segment_metrics *m,
                                 const struct oxr_segment_layout *l,
                                 uint32_t i,
                                 struct xrt_window_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (m == NULL || l == NULL || i >= m->count || i >= l->count) {
		return;
	}
	const struct xrt_segment_metric *s = &m->seg[i];
	fill_display(s, out);
	out->window_pixel_width = (uint32_t)s->window_rect.extent.w;
	out->window_pixel_height = (uint32_t)s->window_rect.extent.h;
	// The segment's desktop origin: its own screen's origin + its screen rect.
	out->window_screen_left = s->screen_desktop_left + s->screen_rect.offset.w;
	out->window_screen_top = s->screen_desktop_top + s->screen_rect.offset.h;
	out->window_width_m = l->seg[i].w_m;
	out->window_height_m = l->seg[i].h_m;
	// The canvas centre in the REFERENCE frame: the eye set handed to the
	// Kooima block is in that frame too, so eye - centre = the eye relative
	// to the segment as it sits on its own screen.
	out->window_center_offset_x_m = l->seg[i].ref_cx;
	out->window_center_offset_y_m = l->seg[i].ref_cy;
	out->window_center_offset_z_m = 0.0f;
	out->valid = true;
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
	fill_display(s, out);
	const float px = pitch_x(s);
	const float py = pitch_y(s);
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
	out->valid = true;
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
