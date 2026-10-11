// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The workspace on N screens (ADR-052) — see comp_workspace_screens.h.
 * @ingroup comp_util
 */

#include "util/comp_workspace_screens.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


/*
 *
 * Screen set.
 *
 */

//! Fallback claim confidence (`XRT_DISPLAY_CLAIM_FALLBACK`); kept local so this file needs no plug-in header.
#define WS_CLAIM_FALLBACK 10u

static bool
ws_pitch(const struct comp_ws_screen *s, float *mx, float *my)
{
	if (s == NULL || s->desktop.w == 0 || s->desktop.h == 0 || !(s->width_m > 0.0f) || !(s->height_m > 0.0f)) {
		return false;
	}
	*mx = s->width_m / (float)s->desktop.w;
	*my = s->height_m / (float)s->desktop.h;
	return true;
}

uint32_t
comp_ws_screens_build(const struct xrt_screen_list *list,
                      uint32_t dp_capable_mask,
                      enum comp_ws_policy policy,
                      struct comp_ws_screen_set *out)
{
	memset(out, 0, sizeof(*out));
	out->primary_index = UINT32_MAX;
	if (list == NULL) {
		return 0;
	}

	uint32_t n = 0;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < COMP_WS_MAX_SCREENS; i++) {
		const struct xrt_screen *s = &list->screens[i];
		if (s->desktop_width == 0 || s->desktop_height == 0) {
			continue;
		}
		struct comp_ws_screen *w = &out->screens[n];
		w->id = s->id;
		w->desktop.x = s->desktop_left;
		w->desktop.y = s->desktop_top;
		w->desktop.w = s->desktop_width;
		w->desktop.h = s->desktop_height;
		w->width_m = s->info.width_m;
		w->height_m = s->info.height_m;
		if (!(w->width_m > 0.0f) || !(w->height_m > 0.0f)) {
			w->width_m = (float)s->physical_width_mm / 1000.0f;
			w->height_m = (float)s->physical_height_mm / 1000.0f;
		}
		w->nominal_viewer_m[0] = s->info.nominal_viewer_x_m;
		w->nominal_viewer_m[1] = s->info.nominal_viewer_y_m;
		w->nominal_viewer_m[2] = s->info.nominal_viewer_z_m;
		snprintf(w->plugin_id, sizeof(w->plugin_id), "%s", s->plugin_id);
		if ((s->flags & XRT_SCREEN_FLAG_TRACKED) != 0) {
			w->flags |= COMP_WS_SCREEN_TRACKED;
		}
		if ((s->flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0 && out->primary_index == UINT32_MAX) {
			out->primary_index = n;
		}

		const bool claimed = s->plugin_id[0] != '\0';
		const bool above_fallback = s->confidence > WS_CLAIM_FALLBACK;
		const bool can_dp = i < 32 && (dp_capable_mask & (1u << i)) != 0;
		if (claimed && can_dp && (above_fallback || policy == COMP_WS_POLICY_CLAIMED_INCL_FALLBACK)) {
			w->flags |= COMP_WS_SCREEN_3D;
		}
		n++;
	}
	out->count = n;
	if (n == 0) {
		return 0;
	}
	if (out->primary_index == UINT32_MAX) {
		out->primary_index = 0;
	}

	struct comp_ws_screen *p = &out->screens[out->primary_index];
	p->flags |= COMP_WS_SCREEN_PRIMARY | COMP_WS_SCREEN_PRESENTER;
	if (p->plugin_id[0] != '\0') {
		p->flags |= COMP_WS_SCREEN_3D; // the panel the session's own DP weaves
	}

	// A screen with no physical size borrows the primary's pitch (and vice
	// versa as a last resort), so every listed screen can be mapped.
	float pmx = 0.0f, pmy = 0.0f;
	bool have_pitch = ws_pitch(p, &pmx, &pmy);
	if (!have_pitch) {
		for (uint32_t i = 0; i < n && !have_pitch; i++) {
			have_pitch = ws_pitch(&out->screens[i], &pmx, &pmy);
		}
	}
	for (uint32_t i = 0; i < n && have_pitch; i++) {
		struct comp_ws_screen *w = &out->screens[i];
		if (!(w->width_m > 0.0f) || !(w->height_m > 0.0f)) {
			w->width_m = pmx * (float)w->desktop.w;
			w->height_m = pmy * (float)w->desktop.h;
		}
	}

	if (policy != COMP_WS_POLICY_SINGLE) {
		for (uint32_t i = 0; i < n; i++) {
			if ((out->screens[i].flags & COMP_WS_SCREEN_3D) != 0) {
				out->screens[i].flags |= COMP_WS_SCREEN_PRESENTER;
			}
		}
	}
	return n;
}

const struct comp_ws_screen *
comp_ws_screens_find(const struct comp_ws_screen_set *set, uint64_t id)
{
	if (set == NULL || set->count == 0) {
		return NULL;
	}
	if (id == 0) {
		return set->primary_index < set->count ? &set->screens[set->primary_index] : NULL;
	}
	for (uint32_t i = 0; i < set->count && i < COMP_WS_MAX_SCREENS; i++) {
		if (set->screens[i].id == id) {
			return &set->screens[i];
		}
	}
	return NULL;
}

uint32_t
comp_ws_screens_presenter_count(const struct comp_ws_screen_set *set)
{
	uint32_t n = 0;
	for (uint32_t i = 0; set != NULL && i < set->count && i < COMP_WS_MAX_SCREENS; i++) {
		if ((set->screens[i].flags & COMP_WS_SCREEN_PRESENTER) != 0) {
			n++;
		}
	}
	return n;
}

bool
comp_ws_screens_equal(const struct comp_ws_screen_set *a, const struct comp_ws_screen_set *b)
{
	if (a->count != b->count || a->primary_index != b->primary_index) {
		return false;
	}
	for (uint32_t i = 0; i < a->count && i < COMP_WS_MAX_SCREENS; i++) {
		const struct comp_ws_screen *x = &a->screens[i];
		const struct comp_ws_screen *y = &b->screens[i];
		if (x->id != y->id || x->flags != y->flags || x->desktop.x != y->desktop.x ||
		    x->desktop.y != y->desktop.y || x->desktop.w != y->desktop.w || x->desktop.h != y->desktop.h ||
		    x->width_m != y->width_m || x->height_m != y->height_m || strcmp(x->plugin_id, y->plugin_id) != 0) {
			return false;
		}
	}
	return true;
}


/*
 *
 * Placement mapping.
 *
 */

bool
comp_ws_map_pose(const struct comp_ws_screen *from,
                 const struct comp_ws_screen *to,
                 const struct xrt_pose *in,
                 float width_m,
                 float height_m,
                 struct xrt_pose *out,
                 float *out_width_m,
                 float *out_height_m)
{
	if (from == NULL || to == NULL || in == NULL || out == NULL) {
		return false;
	}
	if (from == to || from->id == to->id) {
		*out = *in;
		*out_width_m = width_m;
		*out_height_m = height_m;
		return true;
	}
	float fmx, fmy, tmx, tmy;
	if (!ws_pitch(from, &fmx, &fmy) || !ws_pitch(to, &tmx, &tmy)) {
		return false;
	}

	// Centre: from-space metres -> desktop px (double: desktop coordinates
	// reach ~10^4 and the round trip must not drift) -> to-space metres.
	const double fcx = (double)from->desktop.x + (double)from->desktop.w * 0.5;
	const double fcy = (double)from->desktop.y + (double)from->desktop.h * 0.5;
	const double tcx = (double)to->desktop.x + (double)to->desktop.w * 0.5;
	const double tcy = (double)to->desktop.y + (double)to->desktop.h * 0.5;
	const double dx = fcx + (double)in->position.x / (double)fmx; // +x right
	const double dy = fcy - (double)in->position.y / (double)fmy; // +y up, desktop y down

	*out = *in;
	out->position.x = (float)((dx - tcx) * (double)tmx);
	out->position.y = (float)((tcy - dy) * (double)tmy);
	*out_width_m = (float)((double)width_m / (double)fmx * (double)tmx);
	*out_height_m = (float)((double)height_m / (double)fmy * (double)tmy);
	return true;
}

//! v rotated by unit quaternion q.
static void
ws_rotate(const struct xrt_quat *q, float vx, float vy, float vz, float *ox, float *oy, float *oz)
{
	// t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
	const float tx = 2.0f * (q->y * vz - q->z * vy);
	const float ty = 2.0f * (q->z * vx - q->x * vz);
	const float tz = 2.0f * (q->x * vy - q->y * vx);
	*ox = vx + q->w * tx + (q->y * tz - q->z * ty);
	*oy = vy + q->w * ty + (q->z * tx - q->x * tz);
	*oz = vz + q->w * tz + (q->x * ty - q->y * tx);
}

bool
comp_ws_footprint(const struct comp_ws_screen *s,
                  const struct xrt_pose *pose,
                  float width_m,
                  float height_m,
                  struct comp_seg_rect *out)
{
	float mx, my;
	if (s == NULL || pose == NULL || out == NULL || !ws_pitch(s, &mx, &my) || !(width_m > 0.0f) ||
	    !(height_m > 0.0f)) {
		return false;
	}

	struct xrt_quat q = pose->orientation;
	const float qn = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (qn > 1e-6f) {
		q.x /= qn;
		q.y /= qn;
		q.z /= qn;
		q.w /= qn;
	} else {
		q.x = q.y = q.z = 0.0f;
		q.w = 1.0f;
	}

	const float hw = width_m * 0.5f;
	const float hh = height_m * 0.5f;
	const float cx[4] = {-hw, hw, -hw, hw};
	const float cy[4] = {hh, hh, -hh, -hh};
	double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
	const double scx = (double)s->desktop.x + (double)s->desktop.w * 0.5;
	const double scy = (double)s->desktop.y + (double)s->desktop.h * 0.5;
	for (int i = 0; i < 4; i++) {
		float rx, ry, rz;
		ws_rotate(&q, cx[i], cy[i], 0.0f, &rx, &ry, &rz);
		const double px = scx + ((double)pose->position.x + rx) / (double)mx;
		const double py = scy - ((double)pose->position.y + ry) / (double)my;
		min_x = px < min_x ? px : min_x;
		max_x = px > max_x ? px : max_x;
		min_y = py < min_y ? py : min_y;
		max_y = py > max_y ? py : max_y;
	}

	const double l = floor(min_x + 0.5);
	const double t = floor(min_y + 0.5);
	const double r = floor(max_x + 0.5);
	const double b = floor(max_y + 0.5);
	if (r <= l || b <= t) {
		return false;
	}
	out->x = (int32_t)l;
	out->y = (int32_t)t;
	out->w = (uint32_t)(r - l);
	out->h = (uint32_t)(b - t);
	return true;
}


/*
 *
 * Window -> presenter split.
 *
 */

uint32_t
comp_ws_split(const struct comp_ws_screen_set *set,
              const struct comp_seg_rect *footprint,
              struct comp_ws_part out[COMP_SEGMENTS_MAX])
{
	if (set == NULL || footprint == NULL || out == NULL || footprint->w == 0 || footprint->h == 0) {
		return 0;
	}

	struct comp_segments_screen scr[COMP_WS_MAX_SCREENS];
	uint32_t map[COMP_WS_MAX_SCREENS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < set->count && i < COMP_WS_MAX_SCREENS; i++) {
		const struct comp_ws_screen *w = &set->screens[i];
		if ((w->flags & COMP_WS_SCREEN_PRESENTER) == 0) {
			continue;
		}
		memset(&scr[n], 0, sizeof(scr[n]));
		scr[n].id = w->id;
		scr[n].desktop = w->desktop;
		scr[n].is_primary = (w->flags & COMP_WS_SCREEN_PRIMARY) != 0;
		scr[n].has_dp_factory = (w->flags & COMP_WS_SCREEN_3D) != 0;
		snprintf(scr[n].plugin_id, sizeof(scr[n].plugin_id), "%s", w->plugin_id);
		map[n] = i;
		n++;
	}

	struct comp_segment_table t;
	comp_segments_compute(footprint, NULL, scr, n, &t);
	for (uint32_t i = 0; i < t.count && i < COMP_SEGMENTS_MAX; i++) {
		const struct comp_segment *g = &t.seg[i];
		struct comp_ws_part *p = &out[i];
		p->screen_index = map[g->screen_index];
		p->screen_id = g->screen_id;
		p->is_primary = g->is_primary;
		p->window_rect = g->window_rect;
		p->screen_rect = g->screen_rect;
	}
	return t.count;
}


bool
comp_ws_presenter_frame(const struct comp_ws_screen *primary,
                        const struct comp_ws_screen *presenter,
                        struct comp_ws_frame *out)
{
	float mx, my;
	if (primary == NULL || presenter == NULL || out == NULL || !ws_pitch(primary, &mx, &my)) {
		return false;
	}
	out->cx_px =
	    (float)((double)primary->desktop.x + (double)primary->desktop.w * 0.5 - (double)presenter->desktop.x);
	out->cy_px =
	    (float)((double)primary->desktop.y + (double)primary->desktop.h * 0.5 - (double)presenter->desktop.y);
	out->ppm_x = 1.0f / mx;
	out->ppm_y = 1.0f / my;
	return true;
}


/*
 *
 * Per-screen views (ADR-052 D2).
 *
 */

//! A pose orientation that keeps the window parallel to the screens (identity up to sign).
static bool
ws_untilted(const struct xrt_quat *q)
{
	const float n = sqrtf(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
	if (!(n > 1e-6f)) {
		return true; // a zero quaternion is read as identity everywhere else
	}
	const float eps = 1e-4f * n;
	return fabsf(q->x) <= eps && fabsf(q->y) <= eps && fabsf(q->z) <= eps;
}

bool
comp_ws_frame_window(const struct comp_ws_screen_set *set,
                     const struct xrt_pose *pose,
                     float width_m,
                     float height_m,
                     uint32_t max_segments,
                     struct comp_ws_view_framing *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (set == NULL || pose == NULL || set->primary_index >= set->count) {
		return false;
	}
	out->frame_screen_index = set->primary_index;
	const struct comp_ws_screen *prim = &set->screens[set->primary_index];
	if (!comp_ws_footprint(prim, pose, width_m, height_m, &out->footprint)) {
		return false;
	}
	out->part_count = comp_ws_split(set, &out->footprint, out->parts);
	if (out->part_count == 0) {
		return true; // on no presenter: nothing draws it, the primary frames it
	}

	// Majority: most pixels; an exact tie goes to the primary, else keeps the
	// earlier (leftmost) part — oxr_segment_views_majority's rule, so the
	// server and a client reading the table pick the same screen.
	uint32_t best = 0;
	uint64_t best_area = 0;
	bool best_primary = false;
	for (uint32_t i = 0; i < out->part_count; i++) {
		const struct comp_ws_part *p = &out->parts[i];
		const uint64_t area = (uint64_t)p->window_rect.w * (uint64_t)p->window_rect.h;
		if (i == 0 || area > best_area || (area == best_area && p->is_primary && !best_primary)) {
			best = i;
			best_area = area;
			best_primary = p->is_primary;
		}
	}
	out->majority = best;
	out->frame_screen_index = out->parts[best].screen_index;
	out->off_primary = !out->parts[best].is_primary;
	out->per_segment = out->part_count >= 2 && out->part_count <= max_segments && ws_untilted(&pose->orientation);
	return true;
}

bool
comp_ws_window_metrics(const struct comp_ws_screen_set *set,
                       const struct comp_ws_screen *s,
                       const struct xrt_pose *pose,
                       float width_m,
                       float height_m,
                       struct xrt_window_metrics *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	const struct comp_ws_screen *prim = comp_ws_screens_find(set, 0);
	float pmx, pmy, smx, smy;
	if (s == NULL || pose == NULL || prim == NULL || !ws_pitch(prim, &pmx, &pmy) || !ws_pitch(s, &smx, &smy)) {
		return false;
	}
	struct xrt_pose p;
	float w = 0.0f, h = 0.0f;
	if (!comp_ws_map_pose(prim, s, pose, width_m, height_m, &p, &w, &h) || !(w > 0.0f) || !(h > 0.0f)) {
		return false;
	}

	out->display_width_m = s->width_m;
	out->display_height_m = s->height_m;
	out->display_pixel_width = s->desktop.w;
	out->display_pixel_height = s->desktop.h;
	out->display_screen_left = s->desktop.x;
	out->display_screen_top = s->desktop.y;

	// The window in desktop px (the footprint of an untilted window): the same
	// pixels whichever screen frames it, the metres are that screen's.
	const double wpx = (double)w / (double)smx;
	const double hpx = (double)h / (double)smy;
	const double cx = (double)s->desktop.x + (double)s->desktop.w * 0.5 + (double)p.position.x / (double)smx;
	const double cy = (double)s->desktop.y + (double)s->desktop.h * 0.5 - (double)p.position.y / (double)smy;
	out->window_pixel_width = (uint32_t)floor(wpx + 0.5);
	out->window_pixel_height = (uint32_t)floor(hpx + 0.5);
	out->window_screen_left = (int32_t)floor(cx - wpx * 0.5 + 0.5);
	out->window_screen_top = (int32_t)floor(cy - hpx * 0.5 + 0.5);

	out->window_width_m = w;
	out->window_height_m = h;
	out->window_center_offset_x_m = p.position.x;
	out->window_center_offset_y_m = p.position.y;
	out->window_center_offset_z_m = p.position.z;
	out->window_orientation = p.orientation;
	out->valid = out->window_pixel_width > 0 && out->window_pixel_height > 0;
	return out->valid;
}

uint32_t
comp_ws_segment_table(const struct comp_ws_screen_set *set,
                      const struct comp_ws_view_framing *f,
                      float depth_m,
                      struct xrt_segment_metrics *out)
{
	if (out == NULL) {
		return 0;
	}
	memset(out, 0, sizeof(*out)); // memcmp'd by the change-only publish: padding too
	if (set == NULL || f == NULL || !f->per_segment || f->part_count > XRT_MAX_SEGMENTS) {
		return 0;
	}
	for (uint32_t i = 0; i < f->part_count; i++) {
		const struct comp_ws_part *p = &f->parts[i];
		if (p->screen_index >= set->count) {
			memset(out, 0, sizeof(*out));
			return 0;
		}
		const struct comp_ws_screen *s = &set->screens[p->screen_index];
		struct xrt_segment_metric *g = &out->seg[i];
		g->screen_id = p->screen_id;
		g->window_rect.offset.w = p->window_rect.x;
		g->window_rect.offset.h = p->window_rect.y;
		g->window_rect.extent.w = (int32_t)p->window_rect.w;
		g->window_rect.extent.h = (int32_t)p->window_rect.h;
		g->screen_rect.offset.w = p->screen_rect.x;
		g->screen_rect.offset.h = p->screen_rect.y;
		g->screen_rect.extent.w = (int32_t)p->screen_rect.w;
		g->screen_rect.extent.h = (int32_t)p->screen_rect.h;
		g->screen_desktop_left = s->desktop.x;
		g->screen_desktop_top = s->desktop.y;
		g->screen_desktop_width = s->desktop.w;
		g->screen_desktop_height = s->desktop.h;
		g->screen_width_m = s->width_m;
		g->screen_height_m = s->height_m;
		g->nominal_viewer_x_m = s->nominal_viewer_m[0];
		g->nominal_viewer_y_m = s->nominal_viewer_m[1];
		g->nominal_viewer_z_m = s->nominal_viewer_m[2];
		g->is_primary = p->is_primary;
		g->has_dp = (s->flags & COMP_WS_SCREEN_3D) != 0;
		g->woven = g->has_dp;
		g->have_eyes = false;
	}
	out->count = f->part_count;
	out->canvas.offset.w = 0;
	out->canvas.offset.h = 0;
	out->canvas.extent.w = (int32_t)f->footprint.w;
	out->canvas.extent.h = (int32_t)f->footprint.h;
	out->window_screen_left = f->footprint.x;
	out->window_screen_top = f->footprint.y;
	out->window_pixel_width = f->footprint.w;
	out->window_pixel_height = f->footprint.h;
	out->window_depth_m = depth_m;
	return out->count;
}


/*
 *
 * Cursor.
 *
 */

void
comp_ws_desktop_to_workspace(
    const struct comp_ws_screen_set *set, int32_t desktop_x, int32_t desktop_y, int32_t *out_ws_x, int32_t *out_ws_y)
{
	int32_t ox = 0, oy = 0;
	if (set != NULL && set->primary_index < set->count) {
		ox = set->screens[set->primary_index].desktop.x;
		oy = set->screens[set->primary_index].desktop.y;
	}
	*out_ws_x = desktop_x - ox;
	*out_ws_y = desktop_y - oy;
}

int32_t
comp_ws_screen_at(
    const struct comp_ws_screen_set *set, int32_t ws_x, int32_t ws_y, int32_t *out_local_x, int32_t *out_local_y)
{
	if (set == NULL || set->primary_index >= set->count) {
		return -1;
	}
	const struct comp_seg_rect *p = &set->screens[set->primary_index].desktop;
	const int64_t dx = (int64_t)ws_x + p->x;
	const int64_t dy = (int64_t)ws_y + p->y;
	for (uint32_t i = 0; i < set->count && i < COMP_WS_MAX_SCREENS; i++) {
		const struct comp_seg_rect *d = &set->screens[i].desktop;
		// Half-open, like the seam rule of the split.
		if (dx >= d->x && dx < (int64_t)d->x + d->w && dy >= d->y && dy < (int64_t)d->y + d->h) {
			if (out_local_x != NULL) {
				*out_local_x = (int32_t)(dx - d->x);
			}
			if (out_local_y != NULL) {
				*out_local_y = (int32_t)(dy - d->y);
			}
			return (int32_t)i;
		}
	}
	return -1;
}

enum comp_ws_policy
comp_ws_policy_from_env(const char *dxr_segments, const char *dxr_workspace_screens)
{
	if (dxr_segments != NULL && dxr_segments[0] == '0') {
		return COMP_WS_POLICY_SINGLE;
	}
	if (dxr_workspace_screens != NULL) {
		if (strcmp(dxr_workspace_screens, "single") == 0 || strcmp(dxr_workspace_screens, "0") == 0) {
			return COMP_WS_POLICY_SINGLE;
		}
		if (strcmp(dxr_workspace_screens, "fallback") == 0) {
			return COMP_WS_POLICY_CLAIMED_INCL_FALLBACK;
		}
	}
	return COMP_WS_POLICY_CLAIMED;
}

void
comp_ws_pointer_owner_note_down(struct comp_ws_pointer_owner *o, uint64_t screen_id, uint32_t timestamp_ms)
{
	if (o == NULL) {
		return;
	}
	// Wrap-safe: a noted DOWN more than half the tick range "ahead" is older.
	if (o->valid && (int32_t)(timestamp_ms - o->timestamp_ms) < 0) {
		return;
	}
	o->screen_id = screen_id;
	o->timestamp_ms = timestamp_ms;
	o->valid = true;
}

uint64_t
comp_ws_pointer_owner_screen(const struct comp_ws_pointer_owner *o)
{
	return (o != NULL && o->valid) ? o->screen_id : 0;
}

void *
comp_ws_synth_press_note_down(
    struct comp_ws_synth_press *p, void *target, uint32_t button, uint32_t timestamp_ms, int32_t x, int32_t y)
{
	if (p == NULL || target == NULL) {
		return NULL;
	}
	void *owed = (p->target != NULL && p->target != target) ? p->target : NULL;
	p->target = target;
	p->button = button;
	p->timestamp_ms = timestamp_ms;
	p->x = x;
	p->y = y;
	return owed;
}

void *
comp_ws_synth_press_settle(struct comp_ws_synth_press *p, bool button_held, bool up_forwarded, void *focus_target)
{
	if (p == NULL || p->target == NULL) {
		return NULL;
	}
	void *target = p->target;
	if (up_forwarded) {
		p->target = NULL; // the real UP reached it
		return NULL;
	}
	if (!button_held || focus_target != target) {
		p->target = NULL;
		return target;
	}
	return NULL; // still dragging on the window it was given to
}

uint64_t
comp_ws_overlay_screen(uint64_t overlay_screen_id, const uint64_t *presenter_ids, uint32_t presenter_count)
{
	if (overlay_screen_id == 0 || presenter_ids == NULL) {
		return 0;
	}
	for (uint32_t i = 0; i < presenter_count && i < COMP_WS_MAX_SCREENS; i++) {
		if (presenter_ids[i] == overlay_screen_id) {
			return overlay_screen_id;
		}
	}
	return 0; // not a live secondary presenter: the primary draws it
}
