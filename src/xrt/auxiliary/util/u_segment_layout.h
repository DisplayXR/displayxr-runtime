// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Segments of a window (or parts of a zone) placed in ONE frame: the
 *         pure geometry behind per-segment views (multi-screen M3, ADR-047 D3)
 *         and per-part zone views (multi-screen § 8.1).
 * @ingroup aux_util
 *
 * Header-only and backend-agnostic: the state tracker (oxr_segment_views.h,
 * which spells these `oxr_segment_*`) and the IPC server (a present-owner's
 * zone parts, framed service-side) share it. Host-tested through
 * `tests/tests_oxr_segment_views.cpp` and `tests/tests_weave_segments.cpp`.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_limits.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * One segment placed in the reference frame.
 *
 * Frames are display spaces: origin at a screen's centre, +X right, +Y up,
 * metres. "own" is the segment's own screen, "ref" the majority screen's.
 */
struct u_segment_place
{
	float own_cx, own_cy; //!< Segment centre in its own screen's display space
	float ref_cx, ref_cy; //!< Segment centre in the reference (majority) display space
	float w_m, h_m;       //!< Segment physical size, its own screen's metres
};

/*!
 * Every segment of a table, placed in one frame.
 */
struct u_segment_layout
{
	uint32_t count;
	uint32_t majority;
	struct u_segment_place seg[XRT_MAX_SEGMENTS];
	//! Centre of the union of all segments in the reference frame — where the
	//! whole window's centre sits, so a window-centred rig (XR_DXR_view_rig's
	//! display rig) can be offset per segment.
	float window_ref_cx, window_ref_cy;
	//! Size of that union, reference metres: the whole window's extent. A
	//! display rig maps the WINDOW's height to its virtual display height, so
	//! each segment gets that height scaled by its share (one m2v for all).
	float window_ref_w, window_ref_h;
};

static inline float
u_segment_pitch_x(const struct xrt_segment_metric *s)
{
	return s->screen_desktop_width > 0 ? s->screen_width_m / (float)s->screen_desktop_width : 0.0f;
}

static inline float
u_segment_pitch_y(const struct xrt_segment_metric *s)
{
	return s->screen_desktop_height > 0 ? s->screen_height_m / (float)s->screen_desktop_height : 0.0f;
}

/*!
 * Distance along one axis from the majority centre to a segment centre, in
 * metres: the majority pitch up to the majority edge, the segment's own pitch
 * beyond it. @p d is the centre-to-centre distance in window px, @p half the
 * majority segment's half extent in window px.
 */
static inline float
u_segment_seam_distance(float d, float half, float pitch_major, float pitch_own)
{
	const float ad = d < 0.0f ? -d : d;
	const float sign = d < 0.0f ? -1.0f : 1.0f;
	if (ad <= half) {
		return d * pitch_major;
	}
	return sign * (half * pitch_major + (ad - half) * pitch_own);
}

static inline uint32_t
u_segment_majority(const struct xrt_segment_metrics *m)
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

static inline bool
u_segment_layout_compute(const struct xrt_segment_metrics *m, struct u_segment_layout *out)
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
		const float px = u_segment_pitch_x(s);
		const float py = u_segment_pitch_y(s);
		const float scx = (float)s->screen_rect.offset.w + (float)s->screen_rect.extent.w * 0.5f;
		const float scy = (float)s->screen_rect.offset.h + (float)s->screen_rect.extent.h * 0.5f;
		struct u_segment_place *p = &out->seg[i];
		p->own_cx = (scx - (float)s->screen_desktop_width * 0.5f) * px;
		p->own_cy = -(scy - (float)s->screen_desktop_height * 0.5f) * py; // +Y up
		p->w_m = (float)s->window_rect.extent.w * px;
		p->h_m = (float)s->window_rect.extent.h * py;
	}

	const uint32_t mj = u_segment_majority(m);
	const struct xrt_segment_metric *ms = &m->seg[mj];
	const float mcx = (float)ms->window_rect.offset.w + (float)ms->window_rect.extent.w * 0.5f;
	const float mcy = (float)ms->window_rect.offset.h + (float)ms->window_rect.extent.h * 0.5f;
	const float half_x = (float)ms->window_rect.extent.w * 0.5f;
	const float half_y = (float)ms->window_rect.extent.h * 0.5f;

	float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
	for (uint32_t i = 0; i < m->count; i++) {
		const struct xrt_segment_metric *s = &m->seg[i];
		struct u_segment_place *p = &out->seg[i];
		if (i == mj) {
			p->ref_cx = p->own_cx;
			p->ref_cy = p->own_cy;
		} else {
			const float cx = (float)s->window_rect.offset.w + (float)s->window_rect.extent.w * 0.5f;
			const float cy = (float)s->window_rect.offset.h + (float)s->window_rect.extent.h * 0.5f;
			p->ref_cx =
			    out->seg[mj].own_cx +
			    u_segment_seam_distance(cx - mcx, half_x, u_segment_pitch_x(ms), u_segment_pitch_x(s));
			// Window px grow downward, display space grows up.
			p->ref_cy =
			    out->seg[mj].own_cy -
			    u_segment_seam_distance(cy - mcy, half_y, u_segment_pitch_y(ms), u_segment_pitch_y(s));
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

static inline void
u_segment_fill_display(const struct xrt_segment_metric *s, struct xrt_window_metrics *out)
{
	out->display_width_m = s->screen_width_m;
	out->display_height_m = s->screen_height_m;
	out->display_pixel_width = s->screen_desktop_width;
	out->display_pixel_height = s->screen_desktop_height;
	out->display_screen_left = s->screen_desktop_left;
	out->display_screen_top = s->screen_desktop_top;
	out->window_orientation.x = 0.0f;
	out->window_orientation.y = 0.0f;
	out->window_orientation.z = 0.0f;
	out->window_orientation.w = 1.0f;
}

static inline void
u_segment_window_metrics(const struct xrt_segment_metrics *m,
                         const struct u_segment_layout *l,
                         uint32_t i,
                         struct xrt_window_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (m == NULL || l == NULL || i >= m->count || i >= l->count) {
		return;
	}
	const struct xrt_segment_metric *s = &m->seg[i];
	u_segment_fill_display(s, out);
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
	// 0 for an OS window; a workspace tile's depth (ADR-052 D2).
	out->window_center_offset_z_m = m->window_depth_m;
	out->valid = true;
}

/*!
 * The factor a window-centred display rig's virtual display height takes for
 * entry @p i of @p l: its height over the union's, so every entry shares one
 * metres-to-virtual scale. 1 when the union is unknown.
 */
static inline float
u_segment_vdh_scale(const struct u_segment_layout *l, uint32_t i)
{
	if (l == NULL || i >= l->count || l->window_ref_h <= 0.0f) {
		return 1.0f;
	}
	return l->seg[i].h_m / l->window_ref_h;
}

#ifdef __cplusplus
}
#endif
