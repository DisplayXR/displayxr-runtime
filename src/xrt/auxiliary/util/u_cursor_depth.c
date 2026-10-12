// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth-aware cursor placement (XR_DXR_cursor_depth, ADR-046).
 * @author David Fattal
 * @ingroup aux_util
 */

#include "util/u_cursor_depth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>

static inline struct xrt_vec3
v3_sub(struct xrt_vec3 a, struct xrt_vec3 b)
{
	return (struct xrt_vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline struct xrt_vec3
v3_add_scaled(struct xrt_vec3 a, struct xrt_vec3 b, float s)
{
	return (struct xrt_vec3){a.x + b.x * s, a.y + b.y * s, a.z + b.z * s};
}

static inline float
v3_dot(struct xrt_vec3 a, struct xrt_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline bool
v3_finite(struct xrt_vec3 a)
{
	return isfinite(a.x) && isfinite(a.y) && isfinite(a.z);
}

// v' = v + 2w(q x v) + 2 q x (q x v), for a unit quaternion (q, w).
static struct xrt_vec3
quat_rotate(struct xrt_quat q, struct xrt_vec3 v)
{
	struct xrt_vec3 u = {q.x, q.y, q.z};
	struct xrt_vec3 c1 = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
	struct xrt_vec3 c2 = {u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
	return (struct xrt_vec3){
	    v.x + 2.0f * (q.w * c1.x + c2.x),
	    v.y + 2.0f * (q.w * c1.y + c2.y),
	    v.z + 2.0f * (q.w * c1.z + c2.z),
	};
}

// Direction of the ray from a view through canvas point (u, v), in locate space.
// Every Kooima frustum's near window is the canvas, so (u, v) maps linearly onto
// the tangents: u = 0 -> left edge, v = 0 -> top edge.
static struct xrt_vec3
view_ray(const struct u_cursor_depth_view *view, float u, float v)
{
	const float tl = tanf(view->fov.angle_left);
	const float tr = tanf(view->fov.angle_right);
	const float tu = tanf(view->fov.angle_up);
	const float td = tanf(view->fov.angle_down);
	struct xrt_vec3 d = {tl + u * (tr - tl), tu - v * (tu - td), -1.0f};
	return quat_rotate(view->pose.orientation, d);
}

void
u_cursor_depth_tuning_defaults(struct u_cursor_depth_tuning *t)
{
	// 1.5% of the baseline: about 1 mm of crossed on-screen disparity at a
	// 63-65 mm IPD - reads as in front without looking detached. Spec v3
	// briefly used 0.005; ADR-046 Amendment 2 A.2 set 0.03, halved to 0.015
	// on 2026-10-11 after the Windows Leia panel check (0.03 floated visibly
	// off the content).
	t->margin = U_CURSOR_DEPTH_DEFAULT_MARGIN;
	// d = -0.6 is t = 0.625: the cursor may come ~3/8 of the way to the eye.
	t->min_disparity = -0.6f;
	// Never behind the glass (Amendment 2 A.1): the cursor is on the canvas
	// or in front of it. u_cursor_depth_target() also caps any configured
	// value at 0, so this cannot be raised.
	t->max_disparity = 0.0f;
	// Amendment 2 A.3: nothing is drawn (the OS cursor is the cursor) until
	// the filtered disparity is at least 0.5% of the baseline in front of
	// the canvas (~0.3 mm on screen at a 65 mm IPD).
	t->draw_threshold = U_CURSOR_DEPTH_DRAW_THRESHOLD;
	t->rise_tau_s = 0.03f;
	t->sink_tau_s = 0.25f;
	t->stale_s = 0.5f;
}

bool
u_cursor_depth_geometry_solve(const struct u_cursor_depth_view *a,
                              const struct u_cursor_depth_view *b,
                              float u,
                              float v,
                              struct u_cursor_depth_geometry *out)
{
	if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
		return false; // also rejects NaN
	}

	const struct xrt_vec3 p1 = a->pose.position;
	const struct xrt_vec3 p2 = b->pose.position;
	const struct xrt_vec3 d1 = view_ray(a, u, v);
	const struct xrt_vec3 d2 = view_ray(b, u, v);
	if (!v3_finite(p1) || !v3_finite(p2) || !v3_finite(d1) || !v3_finite(d2)) {
		return false;
	}

	// Closest points of p1 + s d1 and p2 + r d2. Kooima rays through the same
	// canvas point intersect exactly; the midpoint absorbs float noise.
	const struct xrt_vec3 w0 = v3_sub(p1, p2);
	const float aa = v3_dot(d1, d1);
	const float bb = v3_dot(d1, d2);
	const float cc = v3_dot(d2, d2);
	const float dd = v3_dot(d1, w0);
	const float ee = v3_dot(d2, w0);
	const float den = aa * cc - bb * bb;
	if (v3_dot(w0, w0) <= 0.0f || !(den > 1e-12f * aa * cc)) {
		return false; // coincident views (2D) or parallel rays
	}
	const float s = (bb * ee - cc * dd) / den;
	const float r = (aa * ee - bb * dd) / den;
	if (!(s > 0.0f && r > 0.0f)) {
		return false;
	}
	const struct xrt_vec3 q1 = v3_add_scaled(p1, d1, s);
	const struct xrt_vec3 q2 = v3_add_scaled(p2, d2, r);
	const struct xrt_vec3 canvas = {(q1.x + q2.x) * 0.5f, (q1.y + q2.y) * 0.5f, (q1.z + q2.z) * 0.5f};
	const struct xrt_vec3 eye = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f, (p1.z + p2.z) * 0.5f};

	const struct xrt_vec3 forward = quat_rotate(a->pose.orientation, (struct xrt_vec3){0.0f, 0.0f, -1.0f});
	const float eye_to_canvas = v3_dot(v3_sub(canvas, eye), forward);
	const float a_to_canvas = v3_dot(v3_sub(canvas, p1), forward);
	const float canvas_height = a_to_canvas * (tanf(a->fov.angle_up) - tanf(a->fov.angle_down));
	if (!(eye_to_canvas > 0.0f) || !(canvas_height > 0.0f) || !isfinite(canvas_height)) {
		return false;
	}

	out->eye = eye;
	out->canvas_point = canvas;
	out->forward = forward;
	out->orientation = a->pose.orientation;
	out->eye_to_canvas = eye_to_canvas;
	out->canvas_height = canvas_height;
	return true;
}

bool
u_cursor_depth_point_disparity(const struct u_cursor_depth_geometry *g, const struct xrt_vec3 *p, float *out_disparity)
{
	if (!v3_finite(*p)) {
		return false;
	}
	const float t = v3_dot(v3_sub(*p, g->eye), g->forward) / g->eye_to_canvas;
	if (!(t > 0.0f) || !isfinite(t)) {
		return false;
	}
	*out_disparity = 1.0f - 1.0f / t;
	return true;
}

float
u_cursor_depth_target(const struct u_cursor_depth_tuning *t, bool has_content, float content_disparity)
{
	if (!has_content || !isfinite(content_disparity)) {
		return 0.0f;
	}
	// Never behind the glass (ADR-046 Amendment 2 A.1): content on or behind
	// the canvas leaves the cursor ON the canvas; only content that comes out
	// of the screen pushes it forward, margin in front of it.
	if (!(content_disparity < 0.0f)) {
		return 0.0f;
	}
	float d = content_disparity - t->margin;
	if (d < t->min_disparity) {
		d = t->min_disparity;
	}
	// The upper clamp is [.., 0] whatever the tuning says: a max_disparity > 0
	// (or a margin <= 0 letting d reach above the content) can never put the
	// cursor behind the glass.
	const float max_d = (t->max_disparity < 0.0f) ? t->max_disparity : 0.0f;
	if (d > max_d) {
		d = max_d;
	}
	return d;
}

bool
u_cursor_depth_should_draw(const struct u_cursor_depth_tuning *t, float filtered_disparity)
{
	// A threshold at or behind the canvas would draw a sprite ON the glass,
	// where the OS cursor is the cursor (A.3): fall back to the default.
	const float thr = (t->draw_threshold < 0.0f && isfinite(t->draw_threshold)) ? t->draw_threshold
	                                                                            : U_CURSOR_DEPTH_DRAW_THRESHOLD;
	return isfinite(filtered_disparity) && filtered_disparity <= thr;
}

bool
u_cursor_depth_filter_will_prime(const struct u_cursor_depth_filter *f,
                                 const struct u_cursor_depth_tuning *t,
                                 uint64_t now_ns)
{
	return !f->primed || now_ns < f->last_ns || (double)(now_ns - f->last_ns) * 1e-9 > (double)t->stale_s;
}

float
u_cursor_depth_filter_step(struct u_cursor_depth_filter *f,
                           const struct u_cursor_depth_tuning *t,
                           float target,
                           uint64_t now_ns)
{
	if (u_cursor_depth_filter_will_prime(f, t, now_ns)) {
		f->primed = true;
		f->disparity = target;
		f->last_ns = now_ns;
		return target;
	}
	if (now_ns == f->last_ns) {
		return f->disparity;
	}

	const float dt = (float)((double)(now_ns - f->last_ns) * 1e-9);
	const float tau = target < f->disparity ? t->rise_tau_s : t->sink_tau_s;
	const float alpha = tau > 0.0f ? 1.0f - expf(-dt / tau) : 1.0f;
	f->disparity += alpha * (target - f->disparity);
	f->last_ns = now_ns;
	return f->disparity;
}

void
u_cursor_depth_place(const struct u_cursor_depth_geometry *g,
                     float disparity,
                     float height_fraction,
                     struct xrt_vec3 *out_position,
                     float *out_height)
{
	// d = 1 - 1/t  <=>  t = 1 / (1 - d). Callers clamp d well below 1.
	const float t = 1.0f / (1.0f - disparity);
	if (!(height_fraction > 0.0f) || !isfinite(height_fraction)) {
		height_fraction = U_CURSOR_DEPTH_DEFAULT_HEIGHT;
	}
	*out_position = v3_add_scaled(g->eye, v3_sub(g->canvas_point, g->eye), t);
	*out_height = height_fraction * g->canvas_height * t;
}

void
u_cursor_depth_place_anchored(const struct u_cursor_depth_geometry *g,
                              float disparity,
                              float height_fraction,
                              enum u_cursor_depth_anchor_mode mode,
                              struct u_cursor_depth_anchor *anchor,
                              float u,
                              float v,
                              bool reprimed,
                              uint64_t now_ns,
                              struct xrt_vec3 *out_position,
                              float *out_height)
{
	// SCREEN placement and the height, shared by every mode.
	u_cursor_depth_place(g, disparity, height_fraction, out_position, out_height);
	if (mode == U_CURSOR_DEPTH_ANCHOR_SCREEN) {
		return;
	}

	const float t = 1.0f / (1.0f - disparity);
	// How far C sits in front of the canvas plane, along the normal.
	const float lift = (1.0f - t) * g->eye_to_canvas;

	if (mode == U_CURSOR_DEPTH_ANCHOR_WORLD || anchor == NULL) {
		if (mode == U_CURSOR_DEPTH_ANCHOR_WORLD) {
			*out_position = v3_add_scaled(g->canvas_point, g->forward, -lift);
		}
		return; // HYBRID without state degrades to SCREEN
	}

	// HYBRID.
	bool moved = !anchor->has_anchor || reprimed || fabsf(u - anchor->last_u) > U_CURSOR_DEPTH_ANCHOR_MOVE_EPS ||
	             fabsf(v - anchor->last_v) > U_CURSOR_DEPTH_ANCHOR_MOVE_EPS || !(anchor->eye_to_canvas > 0.0f);
	if (!moved) {
		// The anchor-time line of sight only means something on the canvas it
		// was taken against. A head move leaves that plane in place; a camera
		// rig re-converging (or any rig re-pose) moves it, and the line then
		// points at a different canvas - EarthView's horizon framing went from
		// e0 = 1 to e = 3.42 under a still pointer, s = 1 - 0.375 * 3.42 < 0,
		// and the "cursor" sat behind the eye while willDraw said true.
		const float plane_shift = v3_dot(v3_sub(anchor->canvas_point, g->canvas_point), g->forward);
		const float facing = v3_dot(anchor->forward, g->forward);
		moved = !(fabsf(plane_shift) <= U_CURSOR_DEPTH_ANCHOR_PLANE_EPS * g->eye_to_canvas) ||
		        !(facing >= 1.0f - 1e-5f);
	}
	// Pointer still: the point on the anchor-time line of sight E0 -> S0 whose
	// distance in front of the glass is the CURRENT depth, lift. Same geometry
	// gives s = t (continuous at the stop); a lateral head move leaves s and so
	// C unchanged (world-fixed, parallax like content); a depth change slides C
	// along that line, so from E0 it stays on S0 (the click point).
	const float s = moved ? 0.0f : 1.0f - lift / anchor->eye_to_canvas;
	// s <= 0 is C on or behind E0 (the head moved back past e0 / (1 - t)):
	// never place it there - re-anchor on the current line of sight instead.
	if (moved || !(s > 0.0f) || !isfinite(s)) {
		// Screen placement (already in *out_position); remember this line of sight.
		anchor->eye = g->eye;
		anchor->canvas_point = g->canvas_point;
		anchor->eye_to_canvas = g->eye_to_canvas;
		anchor->forward = g->forward;
		anchor->last_u = u;
		anchor->last_v = v;
		anchor->last_ns = now_ns;
		anchor->has_anchor = true;
		return;
	}
	*out_position = v3_add_scaled(anchor->eye, v3_sub(anchor->canvas_point, anchor->eye), s);
}


/*
 *
 * Phase 3a: the depth-layer source.
 *
 */

bool
u_cursor_depth_patch_should_sample(const struct u_cursor_depth_patch_request *req)
{
	if (req == NULL || !req->requested) {
		return false;
	}
	if (!(req->u >= 0.0f && req->u <= 1.0f && req->v >= 0.0f && req->v <= 1.0f)) {
		return false; // also rejects NaN
	}
	return req->radius_u > 0.0f && req->radius_v > 0.0f && isfinite(req->radius_u) && isfinite(req->radius_v);
}

bool
u_cursor_depth_patch_rect(const struct u_cursor_depth_patch_request *req,
                          int32_t sub_x,
                          int32_t sub_y,
                          int32_t sub_w,
                          int32_t sub_h,
                          int32_t *out_x,
                          int32_t *out_y,
                          int32_t *out_w,
                          int32_t *out_h)
{
	if (sub_w <= 0 || sub_h <= 0) {
		return false;
	}
	const float half = (float)U_CURSOR_DEPTH_PATCH_MAX_DIM * 0.5f;
	float rx = req->radius_u * (float)sub_w;
	float ry = req->radius_v * (float)sub_h;
	rx = rx > half ? half : rx;
	ry = ry > half ? half : ry;
	const float cx = (float)sub_x + req->u * (float)sub_w;
	const float cy = (float)sub_y + req->v * (float)sub_h;
	int32_t x0 = (int32_t)floorf(cx - rx);
	int32_t y0 = (int32_t)floorf(cy - ry);
	int32_t x1 = (int32_t)ceilf(cx + rx);
	int32_t y1 = (int32_t)ceilf(cy + ry);
	x0 = x0 < sub_x ? sub_x : x0;
	y0 = y0 < sub_y ? sub_y : y0;
	x1 = x1 > sub_x + sub_w ? sub_x + sub_w : x1;
	y1 = y1 > sub_y + sub_h ? sub_y + sub_h : y1;
	x1 = x1 - x0 > U_CURSOR_DEPTH_PATCH_MAX_DIM ? x0 + U_CURSOR_DEPTH_PATCH_MAX_DIM : x1;
	y1 = y1 - y0 > U_CURSOR_DEPTH_PATCH_MAX_DIM ? y0 + U_CURSOR_DEPTH_PATCH_MAX_DIM : y1;
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	*out_x = x0;
	*out_y = y0;
	*out_w = x1 - x0;
	*out_h = y1 - y0;
	return true;
}

bool
u_cursor_depth_reduce_patch(const float *texels,
                            int32_t w,
                            int32_t h,
                            int32_t row_stride,
                            bool nearest_is_max,
                            int32_t *out_x,
                            int32_t *out_y,
                            float *out_raw)
{
	if (texels == NULL || w <= 0 || h <= 0 || row_stride < w) {
		return false;
	}
	bool found = false;
	float best = 0.0f;
	for (int32_t y = 0; y < h; y++) {
		const float *row = texels + (size_t)y * (size_t)row_stride;
		for (int32_t x = 0; x < w; x++) {
			const float d = row[x];
			if (!isfinite(d)) {
				continue;
			}
			if (!found || (nearest_is_max ? d > best : d < best)) {
				found = true;
				best = d;
				*out_x = x;
				*out_y = y;
			}
		}
	}
	if (found) {
		*out_raw = best;
	}
	return found;
}

bool
u_cursor_depth_linear_depth(const struct u_cursor_depth_layer_depth *d, float raw_depth, float *out_z)
{
	const float range = d->max_depth - d->min_depth;
	if (!(range > 0.0f) || !isfinite(range) || !isfinite(raw_depth) || !(d->near_z > 0.0f) || !(d->far_z > 0.0f) ||
	    d->near_z == d->far_z) {
		return false; // the > 0 tests also reject NaN
	}
	// s = 0 at the near_z end, 1 at the far_z end. A hair of slack absorbs
	// float noise at either end without accepting garbage.
	float s = (raw_depth - d->min_depth) / range;
	if (s < -1e-4f || s > 1.0f + 1e-4f) {
		return false;
	}
	s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);

	// The plane at the FAR end of the mapping is where a cleared depth buffer
	// sits: background, not content. Ordinary Z (near_z < far_z) clears to
	// s = 1, reversed Z (near_z > far_z) to s = 0.
	const bool reversed = d->near_z > d->far_z;
	const float eps = 1e-6f;
	if ((!reversed && s >= 1.0f - eps) || (reversed && s <= eps)) {
		return false;
	}

	// 1/z is linear in window depth for every perspective projection: D3D,
	// Vulkan and Metal [0,1] clip depth and GL [-1,1] NDC (with the default
	// glDepthRange) all land on the same window-depth hyperbola. An infinite
	// plane contributes 1/inf = 0.
	const double inv_n = isinf(d->near_z) ? 0.0 : 1.0 / (double)d->near_z;
	const double inv_f = isinf(d->far_z) ? 0.0 : 1.0 / (double)d->far_z;
	const double inv_z = inv_n + (double)s * (inv_f - inv_n);
	if (!(inv_z > 0.0)) {
		return false;
	}
	const double z = 1.0 / inv_z;
	if (!isfinite(z)) {
		return false;
	}
	*out_z = (float)z;
	return true;
}

bool
u_cursor_depth_point_from_depth_sample(const struct u_cursor_depth_view *view,
                                       const struct u_cursor_depth_layer_depth *d,
                                       float su,
                                       float sv,
                                       float raw_depth,
                                       struct xrt_vec3 *out_point)
{
	float z;
	if (!isfinite(su) || !isfinite(sv) || !u_cursor_depth_linear_depth(d, raw_depth, &z)) {
		return false;
	}
	// view_ray() is built in tangent space with z = -1 before rotation, so
	// scaling it by the view-space distance z lands exactly on the point.
	const struct xrt_vec3 dir = view_ray(view, su, sv);
	const struct xrt_vec3 p = v3_add_scaled(view->pose.position, dir, z);
	if (!v3_finite(p)) {
		return false;
	}
	*out_point = p;
	return true;
}


/*
 *
 * Spec v5: the runtime draws the cursor (ADR-046 Amendment 2 B).
 *
 */

bool
u_cursor_draw_should_draw(const struct u_cursor_draw_request *req)
{
	if (req == NULL || !req->requested || req->view_count < 2 || req->view_count > XRT_MAX_VIEWS) {
		return false;
	}
	for (uint32_t i = 0; i < req->view_count; i++) {
		const struct u_cursor_draw_view *v = &req->view[i];
		if (!isfinite(v->origin[0]) || !isfinite(v->origin[1]) || !isfinite(v->axis_x[0]) ||
		    !isfinite(v->axis_x[1]) || !isfinite(v->axis_y[0]) || !isfinite(v->axis_y[1])) {
			return false;
		}
	}
	return true;
}

void
u_cursor_draw_report_drew(const struct u_cursor_draw_request *req)
{
	if (req == NULL || req->report == NULL) {
		return;
	}
	req->report->answered = true;
	req->report->drew = true;
	req->report->reason = U_CURSOR_DRAW_REFUSAL_NONE;
}

void
u_cursor_draw_report_refused(const struct u_cursor_draw_request *req, enum u_cursor_draw_refusal reason)
{
	if (req == NULL || req->report == NULL) {
		return;
	}
	req->report->answered = true;
	req->report->drew = false;
	req->report->reason = reason == U_CURSOR_DRAW_REFUSAL_NONE ? U_CURSOR_DRAW_REFUSAL_NOT_REACHED : reason;
}

const char *
u_cursor_draw_refusal_name(enum u_cursor_draw_refusal reason)
{
	switch (reason) {
	case U_CURSOR_DRAW_REFUSAL_NONE: return "none";
	case U_CURSOR_DRAW_REFUSAL_NOT_REACHED: return "the frame never reached the cursor pass";
	case U_CURSOR_DRAW_REFUSAL_INVALID_REQUEST: return "invalid request";
	case U_CURSOR_DRAW_REFUSAL_ZONE_MISMATCH: return "zones frame / zone request mismatch";
	case U_CURSOR_DRAW_REFUSAL_ZONE_UNPLACEABLE: return "the zone cannot be placed in the target";
	case U_CURSOR_DRAW_REFUSAL_MULTI_SCREEN: return "multi-screen routed frame";
	case U_CURSOR_DRAW_REFUSAL_NO_DISPLAY_INFO: return "no display size / canvas height";
	case U_CURSOR_DRAW_REFUSAL_SPRITE_UNUSABLE: return "the sprite swapchain image cannot be sampled";
	case U_CURSOR_DRAW_REFUSAL_DEFAULT_LOOK: return "the default look could not be built or uploaded";
	case U_CURSOR_DRAW_REFUSAL_NO_PIPELINE: return "the cursor pipeline could not be created";
	case U_CURSOR_DRAW_REFUSAL_NO_COMPOSE_PASS: return "no compose pass to draw into";
	case U_CURSOR_DRAW_REFUSAL_NO_TILES: return "no tile to draw into";
	case U_CURSOR_DRAW_REFUSAL_FORCED: return "forced (DXR_CURSOR_DRAW_REFUSE)";
	default: return "unknown";
	}
}

void
u_cursor_draw_report_begin(struct u_cursor_draw_report *report)
{
	report->answered = false;
	report->drew = false;
	report->reason = U_CURSOR_DRAW_REFUSAL_NOT_REACHED;
}

bool
u_cursor_draw_latch_record(struct u_cursor_draw_latch *latch, const struct u_cursor_draw_report *report)
{
	if (report->answered && report->drew) {
		latch->refused = false;
		return false;
	}
	latch->refused = true;
	enum u_cursor_draw_refusal reason = report->answered ? report->reason : U_CURSOR_DRAW_REFUSAL_NOT_REACHED;
	if (reason <= U_CURSOR_DRAW_REFUSAL_NONE || reason >= U_CURSOR_DRAW_REFUSAL_COUNT) {
		reason = U_CURSOR_DRAW_REFUSAL_NOT_REACHED;
	}
	const uint32_t bit = 1u << (uint32_t)reason;
	if ((latch->warned & bit) != 0) {
		return false;
	}
	latch->warned |= bit;
	return true;
}

bool
u_cursor_draw_latch_allows(const struct u_cursor_draw_latch *latch)
{
	return !latch->refused;
}

bool
u_cursor_draw_project(const struct u_cursor_depth_view *view, const struct xrt_vec3 *p, float out_xy[2])
{
	// Into the view's own frame: the conjugate rotation of (p - eye).
	const struct xrt_quat inv = {-view->pose.orientation.x, -view->pose.orientation.y, -view->pose.orientation.z,
	                             view->pose.orientation.w};
	const struct xrt_vec3 l = quat_rotate(inv, v3_sub(*p, view->pose.position));
	if (!(l.z < -1e-9f) || !v3_finite(l)) {
		return false; // on or behind the eye (the < test also rejects NaN)
	}
	const float tl = tanf(view->fov.angle_left);
	const float tr = tanf(view->fov.angle_right);
	const float tu = tanf(view->fov.angle_up);
	const float td = tanf(view->fov.angle_down);
	if (!(tr - tl > 0.0f) || !(tu - td > 0.0f)) {
		return false;
	}
	// The inverse of view_ray(): the tangents map linearly onto the tile.
	const float tx = l.x / -l.z;
	const float ty = l.y / -l.z;
	out_xy[0] = (tx - tl) / (tr - tl);
	out_xy[1] = (tu - ty) / (tu - td);
	return isfinite(out_xy[0]) && isfinite(out_xy[1]);
}

bool
u_cursor_draw_view_from_placement(const struct u_cursor_depth_view *view,
                                  const struct xrt_vec3 *center,
                                  const struct xrt_quat *orientation,
                                  float unit,
                                  struct u_cursor_draw_view *out)
{
	if (!(unit > 0.0f) || !isfinite(unit)) {
		return false;
	}
	const struct xrt_vec3 ex = {1.0f, 0.0f, 0.0f};
	const struct xrt_vec3 ey = {0.0f, 1.0f, 0.0f};
	const struct xrt_vec3 px = v3_add_scaled(*center, quat_rotate(*orientation, ex), unit);
	const struct xrt_vec3 py = v3_add_scaled(*center, quat_rotate(*orientation, ey), unit);
	float o[2], ax[2], ay[2];
	if (!u_cursor_draw_project(view, center, o) || !u_cursor_draw_project(view, &px, ax) ||
	    !u_cursor_draw_project(view, &py, ay)) {
		return false;
	}
	out->origin[0] = o[0];
	out->origin[1] = o[1];
	out->axis_x[0] = ax[0] - o[0];
	out->axis_x[1] = ax[1] - o[1];
	out->axis_y[0] = ay[0] - o[0];
	out->axis_y[1] = ay[1] - o[1];
	return true;
}

bool
u_cursor_draw_placement_drawable(const struct u_cursor_depth_view *views,
                                 uint32_t view_count,
                                 const struct xrt_vec3 *center,
                                 const struct xrt_quat *orientation,
                                 float unit,
                                 struct u_cursor_draw_view *out_views)
{
	if (views == NULL || center == NULL || orientation == NULL || view_count < 2 || view_count > XRT_MAX_VIEWS) {
		return false;
	}
	bool hotspot_in_a_tile = false;
	for (uint32_t i = 0; i < view_count; i++) {
		struct u_cursor_draw_view m;
		if (!u_cursor_draw_view_from_placement(&views[i], center, orientation, unit, &m)) {
			return false; // behind (or on) an eye, or a degenerate view
		}
		// The sprite map must be finite and neither collapse to nothing nor
		// blow up (a point grazing the eye plane): one unit is a canvas
		// height, ~1 tile at the glass, so 1e-4 .. 1e4 tiles is generous.
		const float lx = sqrtf(m.axis_x[0] * m.axis_x[0] + m.axis_x[1] * m.axis_x[1]);
		const float ly = sqrtf(m.axis_y[0] * m.axis_y[0] + m.axis_y[1] * m.axis_y[1]);
		if (!(lx > 1e-4f && lx < 1e4f) || !(ly > 1e-4f && ly < 1e4f)) {
			return false;
		}
		if (m.origin[0] >= 0.0f && m.origin[0] <= 1.0f && m.origin[1] >= 0.0f && m.origin[1] <= 1.0f) {
			hotspot_in_a_tile = true;
		}
		if (out_views != NULL) {
			out_views[i] = m;
		}
	}
	return hotspot_in_a_tile;
}

bool
u_cursor_draw_into_zone(struct u_cursor_draw_request *req, uint32_t target_w, uint32_t target_h, float out_clip[4])
{
	out_clip[0] = 0.0f;
	out_clip[1] = 0.0f;
	out_clip[2] = 1.0f;
	out_clip[3] = 1.0f;
	if (!req->zone_valid) {
		return true;
	}
	if (target_w == 0 || target_h == 0 || req->zone_rect.extent.w <= 0 || req->zone_rect.extent.h <= 0) {
		return false;
	}
	// The zone's sub-rect of the tile, exactly as its layer is placed.
	const float zx = (float)req->zone_rect.offset.w / (float)target_w;
	const float zy = (float)req->zone_rect.offset.h / (float)target_h;
	const float zw = (float)req->zone_rect.extent.w / (float)target_w;
	const float zh = (float)req->zone_rect.extent.h / (float)target_h;

	const float x0 = zx > 0.0f ? zx : 0.0f;
	const float y0 = zy > 0.0f ? zy : 0.0f;
	const float x1 = zx + zw < 1.0f ? zx + zw : 1.0f;
	const float y1 = zy + zh < 1.0f ? zy + zh : 1.0f;
	if (!(x1 > x0) || !(y1 > y0)) {
		return false;
	}

	const uint32_t n = req->view_count < XRT_MAX_VIEWS ? req->view_count : XRT_MAX_VIEWS;
	for (uint32_t i = 0; i < n; i++) {
		struct u_cursor_draw_view *v = &req->view[i];
		v->origin[0] = zx + v->origin[0] * zw;
		v->origin[1] = zy + v->origin[1] * zh;
		v->axis_x[0] *= zw;
		v->axis_x[1] *= zh;
		v->axis_y[0] *= zw;
		v->axis_y[1] *= zh;
	}
	out_clip[0] = x0;
	out_clip[1] = y0;
	out_clip[2] = x1 - x0;
	out_clip[3] = y1 - y0;
	return true;
}

uint32_t
u_cursor_draw_canvas_px_h(const struct u_cursor_draw_request *req, uint32_t canvas_px_h)
{
	if (req->zone_valid && req->zone_rect.extent.h > 0) {
		return (uint32_t)req->zone_rect.extent.h;
	}
	return canvas_px_h;
}

void
u_cursor_draw_corners(
    const struct u_cursor_draw_view *v, float height, float aspect, float hot_u, float hot_v, float out_xy[4][2])
{
	// Texture UV order (0,0), (0,1), (1,0), (1,1). Sprite space: x right in
	// units of its height (width = aspect), y UP; the hotspot is the origin.
	static const float uv[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
	for (int k = 0; k < 4; k++) {
		const float sx = (uv[k][0] - hot_u) * aspect * height;
		const float sy = (hot_v - uv[k][1]) * height;
		out_xy[k][0] = v->origin[0] + sx * v->axis_x[0] + sy * v->axis_y[0];
		out_xy[k][1] = v->origin[1] + sx * v->axis_x[1] + sy * v->axis_y[1];
	}
}

/*
 * The classic arrow (the web SDK's CURSOR_ARROW), in px of its 19 px height,
 * y DOWN, tip at (0, 0) = the hotspot. This is the dark outline; the white
 * fill is it inset by one px.
 */
static const float k_arrow[7][2] = {
    {0.0f, 0.0f}, {0.0f, 16.0f}, {4.2f, 12.4f}, {6.9f, 18.6f}, {9.5f, 17.5f}, {6.8f, 11.5f}, {11.7f, 11.5f},
};

// Mitred inset by d (same units): every edge moves inward by exactly d.
static void
arrow_inset(float d, float out[7][2])
{
	const int n = 7;
	float area = 0.0f;
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		area += k_arrow[i][0] * k_arrow[j][1] - k_arrow[j][0] * k_arrow[i][1];
	}
	const float sgn = area > 0.0f ? 1.0f : -1.0f;
	float nrm[7][2];
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		const float dx = k_arrow[j][0] - k_arrow[i][0];
		const float dy = k_arrow[j][1] - k_arrow[i][1];
		float l = sqrtf(dx * dx + dy * dy);
		l = l > 0.0f ? l : 1.0f;
		nrm[i][0] = (-dy / l) * sgn;
		nrm[i][1] = (dx / l) * sgn;
	}
	for (int i = 0; i < n; i++) {
		const float *a = nrm[(i + n - 1) % n];
		const float *b = nrm[i];
		const float k = d / (1.0f + a[0] * b[0] + a[1] * b[1]);
		out[i][0] = k_arrow[i][0] + (a[0] + b[0]) * k;
		out[i][1] = k_arrow[i][1] + (a[1] + b[1]) * k;
	}
}

// Even-odd crossing test (the arrow is concave at its notch).
static bool
poly_contains(const float poly[7][2], float x, float y)
{
	bool in = false;
	for (int i = 0, j = 6; i < 7; j = i++) {
		const float yi = poly[i][1];
		const float yj = poly[j][1];
		if ((yi > y) != (yj > y)) {
			const float xc = poly[i][0] + (y - yi) / (yj - yi) * (poly[j][0] - poly[i][0]);
			if (x < xc) {
				in = !in;
			}
		}
	}
	return in;
}

void
u_cursor_draw_arrow_rgba(uint8_t *rgba, uint32_t w, uint32_t h)
{
	if (rgba == NULL || w == 0 || h == 0) {
		return;
	}
	float fill[7][2];
	arrow_inset(1.0f, fill);
	// Image px -> arrow px.
	const float s = U_CURSOR_ARROW_HEIGHT_PX / (float)h;
	// sRGB-encoded bytes: white fill, near-black outline (the web SDK's
	// CURSOR_FILL_COLOR / CURSOR_OUTLINE_COLOR).
	const float fill_c[3] = {255.0f, 255.0f, 255.0f};
	const float line_c[3] = {5.0f, 5.0f, 8.0f};
	const int ss = 4;
	const float n = (float)(ss * ss);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			float cov_fill = 0.0f;
			float cov_line = 0.0f;
			for (int sy = 0; sy < ss; sy++) {
				for (int sx = 0; sx < ss; sx++) {
					const float ax = ((float)x + ((float)sx + 0.5f) / (float)ss) * s;
					const float ay = ((float)y + ((float)sy + 0.5f) / (float)ss) * s;
					if (poly_contains((const float(*)[2])fill, ax, ay)) {
						cov_fill += 1.0f;
					} else if (poly_contains(k_arrow, ax, ay)) {
						cov_line += 1.0f;
					}
				}
			}
			cov_fill /= n;
			cov_line /= n;
			uint8_t *p = rgba + ((size_t)y * w + x) * 4;
			for (int c = 0; c < 3; c++) {
				// Premultiplied: each colour times its coverage, summed.
				p[c] = (uint8_t)lrintf(fill_c[c] * cov_fill + line_c[c] * cov_line);
			}
			p[3] = (uint8_t)lrintf(255.0f * (cov_fill + cov_line));
		}
	}
}

void
u_cursor_draw_unblend(const uint8_t *on_black, const uint8_t *on_white, uint32_t pixel_count, uint8_t *out)
{
	if (on_black == NULL || on_white == NULL || out == NULL) {
		return;
	}
	for (uint32_t i = 0; i < pixel_count; i++) {
		const uint8_t *b = on_black + (size_t)i * 4;
		const uint8_t *w = on_white + (size_t)i * 4;
		uint8_t *o = out + (size_t)i * 4;
		// How much of the background shows through, summed over 3 channels.
		// Negative where the cursor inverts (w < b): that is fully opaque.
		int through = 0;
		for (int c = 0; c < 3; c++) {
			through += (int)w[c] - (int)b[c];
		}
		int a = 255 - (through + 1) / 3;
		if (a > 255) {
			a = 255;
		}
		if (a <= 0) {
			o[0] = o[1] = o[2] = o[3] = 0;
			continue;
		}
		// Over black, the render IS colour x coverage: premultiplied already.
		for (int c = 0; c < 3; c++) {
			o[c] = b[c] > a ? (uint8_t)a : b[c];
		}
		o[3] = (uint8_t)a;
	}
}


/*
 *
 * Spec v7: measured disparity (ADR-046 Amendment 3, Phase 3b).
 *
 * The matcher is the web stereo-tile cursor's (displayxr-web
 * js/camera/disparity.js matchBlock), ported line for line so a stereo pair
 * shown by the page and by a native app gets the same answer; the left-right
 * check and the footprint's nearest-first order are the runtime's additions.
 *
 */

#define UCM_B U_CURSOR_MEASURE_BLOCK
#define UCM_ND (2 * U_CURSOR_MEASURE_RANGE + 1)

uint32_t
u_cursor_measure_format_bytes(enum u_cursor_measure_format fmt)
{
	switch (fmt) {
	case U_CURSOR_MEASURE_FORMAT_RGBA8:
	case U_CURSOR_MEASURE_FORMAT_BGRA8:
	case U_CURSOR_MEASURE_FORMAT_RGB10A2: return 4;
	case U_CURSOR_MEASURE_FORMAT_RGBA16F: return 8;
	default: return 0;
	}
}

bool
u_cursor_measure_should_sample(const struct u_cursor_measure_request *req)
{
	if (req == NULL || !req->requested) {
		return false;
	}
	if (!(req->u >= 0.0f && req->u <= 1.0f && req->v >= 0.0f && req->v <= 1.0f)) {
		return false; // off the canvas (also rejects NaN)
	}
	return req->height > 0.0f && isfinite(req->height) && req->view_index[0] != req->view_index[1];
}

void
u_cursor_measure_footprint_points(float tip_x, float tip_y, float height_px, float out_points[][2])
{
	// The box spans 1.5 h on each axis (ring diameter, plus the tip that
	// pokes out up-left), plus half a block each side.
	const float max_h = (float)(U_CURSOR_MEASURE_FOOTPRINT_MAX - UCM_B) / 1.5f;
	float h = height_px;
	if (!(h > 0.0f) || !isfinite(h)) {
		h = 0.0f;
	}
	if (h > max_h) {
		h = max_h;
	}
	out_points[0][0] = tip_x;
	out_points[0][1] = tip_y;
	// cursorArrowFootprint: a ring around the arrow's BODY, which hangs
	// down-right of the tip (web: u + 0.3 h / aspect, v + 0.5 h; px are square).
	const float cx = tip_x + 0.3f * h;
	const float cy = tip_y + 0.5f * h;
	const float r = 0.75f * h;
	out_points[1][0] = cx;
	out_points[1][1] = cy;
	for (int k = 0; k < 8; k++) {
		const float a = (float)k * 0.78539816339f; // pi / 4
		out_points[2 + k][0] = cx + r * cosf(a);
		out_points[2 + k][1] = cy + r * sinf(a);
	}
}

static int32_t
ucm_factor(int32_t sub_w)
{
	int32_t f = (sub_w + U_CURSOR_MEASURE_WORK_EYE_W - 1) / U_CURSOR_MEASURE_WORK_EYE_W;
	if (f < 1) {
		f = 1;
	}
	if (f > U_CURSOR_MEASURE_MAX_FACTOR) {
		f = U_CURSOR_MEASURE_MAX_FACTOR;
	}
	return f;
}

bool
u_cursor_measure_make_plan(const struct u_cursor_measure_request *req,
                           int32_t sub_w,
                           int32_t sub_h,
                           bool bottom_up,
                           struct u_cursor_measure_plan *out)
{
	if (!u_cursor_measure_should_sample(req) || out == NULL) {
		return false;
	}
	const int32_t f = ucm_factor(sub_w);
	const int32_t ww = sub_w / f; // work width / height of the whole sub-image
	const int32_t wh = sub_h / f;
	if (ww < 2 * UCM_B || wh < UCM_B + 2 * U_CURSOR_MEASURE_DY) {
		return false;
	}

	float pts[U_CURSOR_MEASURE_POINTS][2];
	u_cursor_measure_footprint_points(req->u * (float)sub_w / (float)f, req->v * (float)sub_h / (float)f,
	                                  req->height * (float)sub_h / (float)f, pts);
	float minx = pts[0][0], maxx = pts[0][0], miny = pts[0][1], maxy = pts[0][1];
	for (int i = 1; i < U_CURSOR_MEASURE_POINTS; i++) {
		minx = fminf(minx, pts[i][0]);
		maxx = fmaxf(maxx, pts[i][0]);
		miny = fminf(miny, pts[i][1]);
		maxy = fmaxf(maxy, pts[i][1]);
	}
	// Block top-left corners are round(centre - B/2) (u_cursor_measure_footprint):
	// cover them, then the search on both sides.
	int32_t x0 = (int32_t)floorf(minx - (float)UCM_B * 0.5f) - (U_CURSOR_MEASURE_RANGE + 1);
	int32_t x1 = (int32_t)ceilf(maxx + (float)UCM_B * 0.5f) + 1 + (U_CURSOR_MEASURE_RANGE + 1);
	int32_t y0 = (int32_t)floorf(miny - (float)UCM_B * 0.5f) - U_CURSOR_MEASURE_DY;
	int32_t y1 = (int32_t)ceilf(maxy + (float)UCM_B * 0.5f) + 1 + U_CURSOR_MEASURE_DY;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > ww ? ww : x1;
	y1 = y1 > wh ? wh : y1;
	// The rounding slack above can add a texel or two: hold the bound.
	if (x1 - x0 > U_CURSOR_MEASURE_MAX_WORK_W) {
		x1 = x0 + U_CURSOR_MEASURE_MAX_WORK_W;
	}
	if (y1 - y0 > U_CURSOR_MEASURE_MAX_WORK_H) {
		y1 = y0 + U_CURSOR_MEASURE_MAX_WORK_H;
	}
	if (x1 - x0 < UCM_B + 2 || y1 - y0 < UCM_B) {
		return false;
	}

	out->factor = f;
	out->work_w = x1 - x0;
	out->work_h = y1 - y0;
	out->x = x0 * f;
	out->w = out->work_w * f;
	out->top_y = y0 * f;
	out->h = out->work_h * f;
	out->y = bottom_up ? sub_h - (out->top_y + out->h) : out->top_y;
	out->sub_w = sub_w;
	out->sub_h = sub_h;
	out->bottom_up = bottom_up;
	return true;
}

static float
ucm_half_to_float(uint16_t hv)
{
	const uint32_t sign = (hv >> 15) & 1u;
	const uint32_t exp = (hv >> 10) & 0x1fu;
	const uint32_t man = hv & 0x3ffu;
	float v;
	if (exp == 0) {
		v = ldexpf((float)man, -24);
	} else if (exp == 31) {
		v = man != 0 ? 0.0f : 65504.0f; // NaN reads as black, inf as very bright
	} else {
		v = ldexpf((float)(man | 0x400u), (int)exp - 25);
	}
	return sign ? -v : v;
}

// One texel's luma, 0..255 (the web's 77/150/29 weights, on the stored values).
static float
ucm_texel_luma(const uint8_t *p, enum u_cursor_measure_format fmt)
{
	float r, g, b;
	switch (fmt) {
	case U_CURSOR_MEASURE_FORMAT_RGBA8:
		r = p[0];
		g = p[1];
		b = p[2];
		break;
	case U_CURSOR_MEASURE_FORMAT_BGRA8:
		b = p[0];
		g = p[1];
		r = p[2];
		break;
	case U_CURSOR_MEASURE_FORMAT_RGBA16F: {
		uint16_t hv[3];
		memcpy(hv, p, sizeof(hv));
		r = ucm_half_to_float(hv[0]) * 255.0f;
		g = ucm_half_to_float(hv[1]) * 255.0f;
		b = ucm_half_to_float(hv[2]) * 255.0f;
		break;
	}
	case U_CURSOR_MEASURE_FORMAT_RGB10A2: {
		uint32_t v;
		memcpy(&v, p, sizeof(v));
		r = (float)(v & 1023u) * (255.0f / 1023.0f);
		g = (float)((v >> 10) & 1023u) * (255.0f / 1023.0f);
		b = (float)((v >> 20) & 1023u) * (255.0f / 1023.0f);
		break;
	}
	default: return 0.0f;
	}
	const float l = (77.0f * r + 150.0f * g + 29.0f * b) / 256.0f;
	return l < 0.0f ? 0.0f : (l > 255.0f ? 255.0f : l);
}

// A copy -> top-down work-resolution luma (box average of factor^2 texels).
static bool
ucm_luma(const struct u_cursor_measure_plan *plan, const struct u_cursor_measure_patch *patch, uint8_t *out)
{
	const uint32_t bpp = u_cursor_measure_format_bytes(patch->format);
	if (patch->data == NULL || bpp == 0 || patch->row_pitch < (uint32_t)plan->w * bpp) {
		return false;
	}
	const int32_t f = plan->factor;
	const float inv = 1.0f / (float)(f * f);
	const uint8_t *base = (const uint8_t *)patch->data;
	if (patch->format == U_CURSOR_MEASURE_FORMAT_RGBA8 || patch->format == U_CURSOR_MEASURE_FORMAT_BGRA8) {
		// The common case, in integers: same weights, (77 R + 150 G + 29 B) / 256.
		const bool bgr = patch->format == U_CURSOR_MEASURE_FORMAT_BGRA8;
		const uint32_t wr = bgr ? 29u : 77u, wb = bgr ? 77u : 29u;
		const uint32_t div = 256u * (uint32_t)(f * f);
		for (int32_t j = 0; j < plan->work_h; j++) {
			for (int32_t i = 0; i < plan->work_w; i++) {
				uint32_t sum = 0;
				for (int32_t k = 0; k < f; k++) {
					const int32_t row = plan->bottom_up ? plan->h - 1 - (j * f + k) : j * f + k;
					const uint8_t *src = base + (size_t)row * patch->row_pitch + (size_t)i * f * 4;
					for (int32_t l = 0; l < f; l++, src += 4) {
						sum += wr * src[0] + 150u * src[1] + wb * src[2];
					}
				}
				out[j * plan->work_w + i] = (uint8_t)(sum / div);
			}
		}
		return true;
	}
	for (int32_t j = 0; j < plan->work_h; j++) {
		for (int32_t i = 0; i < plan->work_w; i++) {
			float sum = 0.0f;
			for (int32_t k = 0; k < f; k++) {
				int32_t row = j * f + k; // top-down row within the copy
				if (plan->bottom_up) {
					row = plan->h - 1 - row;
				}
				const uint8_t *src = base + (size_t)row * patch->row_pitch + (size_t)i * f * bpp;
				for (int32_t l = 0; l < f; l++) {
					sum += ucm_texel_luma(src + (size_t)l * bpp, patch->format);
				}
			}
			out[j * plan->work_w + i] = (uint8_t)(sum * inv);
		}
	}
	return true;
}

// Summed-area tables of one work image, (w + 1) x (h + 1): every candidate
// window's sum and sum of squares in four lookups instead of B^2 adds.
static void
ucm_integral(const uint8_t *px, int32_t w, int32_t h, uint32_t *s1, uint32_t *s2)
{
	const int32_t sw = w + 1;
	for (int32_t x = 0; x <= w; x++) {
		s1[x] = s2[x] = 0;
	}
	for (int32_t y = 0; y < h; y++) {
		uint32_t r1 = 0, r2 = 0;
		s1[(y + 1) * sw] = s2[(y + 1) * sw] = 0;
		for (int32_t x = 0; x < w; x++) {
			const uint32_t v = px[y * w + x];
			r1 += v;
			r2 += v * v;
			s1[(y + 1) * sw + x + 1] = s1[y * sw + x + 1] + r1;
			s2[(y + 1) * sw + x + 1] = s2[y * sw + x + 1] + r2;
		}
	}
}

static inline uint32_t
ucm_box(const uint32_t *s, int32_t w, int32_t x, int32_t y)
{
	const int32_t sw = w + 1;
	return s[(y + UCM_B) * sw + x + UCM_B] - s[y * sw + x + UCM_B] - s[(y + UCM_B) * sw + x] + s[y * sw + x];
}

// Normalised cross-correlation of the B x B block of `a` at (ax, ay) (mean
// ma, std sa) against `b` at (bx, by); -1 when that block leaves `b` or is
// flat (the web's ncc()). bs1 / bs2: b's summed-area tables.
static float
ucm_ncc(const uint8_t *a,
        const uint8_t *b,
        const uint32_t *bs1,
        const uint32_t *bs2,
        int32_t w,
        int32_t h,
        int32_t ax,
        int32_t ay,
        int32_t bx,
        int32_t by,
        float ma,
        float sa)
{
	if (bx < 0 || bx + UCM_B > w || by < 0 || by + UCM_B > h) {
		return -1.0f;
	}
	const float n = (float)(UCM_B * UCM_B);
	const float mb = (float)ucm_box(bs1, w, bx, by) / n;
	const float vb = (float)ucm_box(bs2, w, bx, by) / n - mb * mb;
	if (vb < 1.0f) {
		return -1.0f;
	}
	int32_t cross = 0;
	for (int32_t y = 0; y < UCM_B; y++) {
		const uint8_t *ra = a + (size_t)(ay + y) * w + ax;
		const uint8_t *rb = b + (size_t)(by + y) * w + bx;
		for (int32_t x = 0; x < UCM_B; x++) {
			cross += (int32_t)ra[x] * (int32_t)rb[x];
		}
	}
	return ((float)cross / n - ma * mb) / (sa * sqrtf(vb));
}

struct ucm_search
{
	const uint8_t *a, *b;
	const uint32_t *bs1, *bs2;
	int32_t w, h, x0, y0, sign, d_min, d_max;
	float mean, std;
	float per_d[UCM_ND];
	float best;
	int32_t bd, bdy;
};

static float
ucm_score(const struct ucm_search *s, int32_t d, int32_t dy)
{
	return ucm_ncc(s->a, s->b, s->bs1, s->bs2, s->w, s->h, s->x0, s->y0, s->x0 - s->sign * d, s->y0 + dy, s->mean, s->std);
}

static void
ucm_scan(struct ucm_search *s, int32_t d0, int32_t d1, int32_t step, int32_t dy0, int32_t dy1)
{
	for (int32_t dy = dy0; dy <= dy1; dy++) {
		const int32_t ry = s->y0 + dy;
		if (ry < 0 || ry + UCM_B > s->h) {
			continue;
		}
		for (int32_t d = d0; d <= d1; d += step) {
			const float c = ucm_score(s, d, dy);
			float *pd = &s->per_d[d - s->d_min];
			if (c > *pd) {
				*pd = c;
			}
			if (c > s->best) {
				s->best = c;
				s->bd = d;
				s->bdy = dy;
			}
		}
	}
}

static float
ucm_per_d(const struct ucm_search *s, int32_t d)
{
	return (d < s->d_min || d > s->d_max) ? -2.0f : s->per_d[d - s->d_min];
}

// A rival local maximum seen on the coarse grid, refined to its top (web peakAt).
static float
ucm_peak_at(const struct ucm_search *s, int32_t d)
{
	float v = -2.0f;
	for (int32_t dy = -U_CURSOR_MEASURE_DY; dy <= U_CURSOR_MEASURE_DY; dy++) {
		const int32_t ry = s->y0 + dy;
		if (ry < 0 || ry + UCM_B > s->h) {
			continue;
		}
		for (int32_t k = d - 1; k <= d + 1; k++) {
			const float c = ucm_score(s, k, dy);
			v = c > v ? c : v;
		}
	}
	return v;
}

static bool
ucm_match_block(const uint8_t *a,
                const uint8_t *b,
                const uint32_t *bs1,
                const uint32_t *bs2,
                int32_t w,
                int32_t h,
                int32_t x0,
                int32_t y0,
                int32_t sign,
                int32_t d_min,
                int32_t d_max,
                float *out_d,
                int32_t *out_dy,
                float *out_ncc)
{
	if (a == NULL || b == NULL || x0 < 0 || y0 < 0 || x0 + UCM_B > w || y0 + UCM_B > h || d_min > d_max ||
	    d_max - d_min + 1 > UCM_ND) {
		return false;
	}
	struct ucm_search s;
	memset(&s, 0, sizeof(s));
	s.a = a;
	s.b = b;
	s.bs1 = bs1;
	s.bs2 = bs2;
	s.w = w;
	s.h = h;
	s.x0 = x0;
	s.y0 = y0;
	s.sign = sign >= 0 ? 1 : -1;
	s.d_min = d_min;
	s.d_max = d_max;

	// Block statistics (web blockStats); a flat block cannot be matched.
	int32_t sum = 0, sum2 = 0;
	for (int32_t y = 0; y < UCM_B; y++) {
		const uint8_t *r = a + (size_t)(y0 + y) * w + x0;
		for (int32_t x = 0; x < UCM_B; x++) {
			sum += r[x];
			sum2 += (int32_t)r[x] * r[x];
		}
	}
	const float n = (float)(UCM_B * UCM_B);
	s.mean = (float)sum / n;
	const float var = (float)sum2 / n - s.mean * s.mean;
	s.std = sqrtf(var > 0.0f ? var : 0.0f);
	if (s.std < U_CURSOR_MEASURE_MIN_TEXTURE) {
		return false;
	}

	for (int32_t i = 0; i < UCM_ND; i++) {
		s.per_d[i] = -2.0f;
	}
	s.best = -2.0f;

	// Coarse (every 2nd disparity), then full resolution around the winner.
	const bool coarse = d_max - d_min > 8;
	ucm_scan(&s, d_min, d_max, coarse ? 2 : 1, -U_CURSOR_MEASURE_DY, U_CURSOR_MEASURE_DY);
	if (coarse && s.best > -1.0f) {
		const int32_t cd = s.bd, cy = s.bdy;
		ucm_scan(&s, cd - 2 > d_min ? cd - 2 : d_min, cd + 2 < d_max ? cd + 2 : d_max, 1,
		         cy - 1 > -U_CURSOR_MEASURE_DY ? cy - 1 : -U_CURSOR_MEASURE_DY,
		         cy + 1 < U_CURSOR_MEASURE_DY ? cy + 1 : U_CURSOR_MEASURE_DY);
	}
	if (s.best < -1.0f) {
		return false; // nothing in range could be scored
	}
	if (out_ncc != NULL) {
		*out_ncc = s.best;
	}

	// Uniqueness: a repeating texture matches equally well one period away.
	// Every rival local maximum >= 3 px off the winner is refined to its top
	// (the coarse grid under-reads it) and must lose by the margin.
	float second = -2.0f;
	for (int32_t d = d_min; d <= d_max; d++) {
		const float c = s.per_d[d - d_min];
		if (abs(d - s.bd) < U_CURSOR_MEASURE_RIVAL_PX || c <= second) {
			continue;
		}
		const bool local_max = c >= ucm_per_d(&s, d - 2) && c >= ucm_per_d(&s, d + 2);
		const float v = (local_max && c > 0.0f) ? ucm_peak_at(&s, d) : c;
		if (v > second) {
			second = v;
		}
	}
	if (d_max - d_min > 12 && second > -2.0f && s.best - second < U_CURSOR_MEASURE_UNIQUENESS) {
		return false;
	}

	// Sub-pixel along d at the best row (parabola).
	const float cm = ucm_score(&s, s.bd - 1, s.bdy);
	const float cp = ucm_score(&s, s.bd + 1, s.bdy);
	float sub = 0.0f;
	const float den = cm - 2.0f * s.best + cp;
	if (cm > -1.0f && cp > -1.0f && den < 0.0f) {
		sub = 0.5f * (cm - cp) / den;
		sub = sub < -0.5f ? -0.5f : (sub > 0.5f ? 0.5f : sub);
	}
	if (out_d != NULL) {
		*out_d = (float)s.bd + sub;
	}
	if (out_dy != NULL) {
		*out_dy = s.bdy;
	}
	return true;
}

bool
u_cursor_measure_match_block(const uint8_t *a,
                             const uint8_t *b,
                             int32_t w,
                             int32_t h,
                             int32_t x0,
                             int32_t y0,
                             int32_t sign,
                             int32_t d_min,
                             int32_t d_max,
                             float *out_d,
                             int32_t *out_dy,
                             float *out_ncc)
{
	if (a == NULL || b == NULL || w <= 0 || h <= 0) {
		return false;
	}
	const size_t n = (size_t)(w + 1) * (size_t)(h + 1);
	uint32_t *s = (uint32_t *)malloc(2 * n * sizeof(uint32_t));
	if (s == NULL) {
		return false;
	}
	ucm_integral(b, w, h, s, s + n);
	const bool ok = ucm_match_block(a, b, s, s + n, w, h, x0, y0, sign, d_min, d_max, out_d, out_dy, out_ncc);
	free(s);
	return ok;
}

struct ucm_cand
{
	float d, ncc;
	int32_t x, y, dy;
};

bool
u_cursor_measure_footprint(const uint8_t *a,
                           const uint8_t *b,
                           int32_t w,
                           int32_t h,
                           const float points[][2],
                           uint32_t point_count,
                           float *out_d,
                           struct u_cursor_measure_result *diag)
{
	struct ucm_cand cand[U_CURSOR_MEASURE_POINTS];
	uint32_t n = 0;
	if (a == NULL || b == NULL || w <= 0 || h <= 0) {
		return false;
	}
	if (point_count > U_CURSOR_MEASURE_POINTS) {
		point_count = U_CURSOR_MEASURE_POINTS;
	}
	// Both views' summed-area tables, once: shared by every block and both
	// directions of the left-right check.
	const size_t area = (size_t)(w + 1) * (size_t)(h + 1);
	uint32_t *tables = (uint32_t *)malloc(4 * area * sizeof(uint32_t));
	if (tables == NULL) {
		return false;
	}
	uint32_t *as1 = tables, *as2 = tables + area, *bs1 = tables + 2 * area, *bs2 = tables + 3 * area;
	ucm_integral(a, w, h, as1, as2);
	ucm_integral(b, w, h, bs1, bs2);
	bool found = false;
	for (uint32_t i = 0; i < point_count; i++) {
		const int32_t x = (int32_t)lroundf(points[i][0] - (float)UCM_B * 0.5f);
		const int32_t y = (int32_t)lroundf(points[i][1] - (float)UCM_B * 0.5f);
		if (x < 0 || x + UCM_B > w || y < 0 || y + UCM_B > h) {
			continue;
		}
		float d = 0.0f, c = -2.0f;
		int32_t dy = 0;
		if (!ucm_match_block(a, b, bs1, bs2, w, h, x, y, +1, -U_CURSOR_MEASURE_RANGE, U_CURSOR_MEASURE_RANGE, &d,
		                     &dy, &c) ||
		    c < U_CURSOR_MEASURE_MIN_NCC) {
			continue;
		}
		cand[n].d = d;
		cand[n].ncc = c;
		cand[n].x = x;
		cand[n].y = y;
		cand[n].dy = dy;
		n++;
	}
	if (diag != NULL) {
		diag->matched = n;
		diag->lr_rejected = 0;
	}
	// Nearest first (largest crossed d), so the first one that survives the
	// left-right check is the answer and the rest are never matched back.
	for (uint32_t i = 1; i < n; i++) {
		for (uint32_t j = i; j > 0 && cand[j].d > cand[j - 1].d; j--) {
			const struct ucm_cand t = cand[j];
			cand[j] = cand[j - 1];
			cand[j - 1] = t;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		// The block of B where A's block landed, matched back into A.
		const int32_t bx = cand[i].x - (int32_t)lroundf(cand[i].d);
		const int32_t by = cand[i].y + cand[i].dy;
		float d2 = 0.0f, c2 = -2.0f;
		int32_t dy2 = 0;
		if (ucm_match_block(b, a, as1, as2, w, h, bx, by, -1, -U_CURSOR_MEASURE_RANGE, U_CURSOR_MEASURE_RANGE, &d2,
		                    &dy2, &c2) &&
		    c2 >= U_CURSOR_MEASURE_MIN_NCC && fabsf(d2 - cand[i].d) <= U_CURSOR_MEASURE_LR_PX) {
			if (out_d != NULL) {
				*out_d = cand[i].d;
			}
			if (diag != NULL) {
				diag->ncc = cand[i].ncc;
			}
			found = true;
			break;
		}
		if (diag != NULL) {
			diag->lr_rejected++;
		}
	}
	free(tables);
	return found;
}

// FNV-1a style, a 64-bit word at a time: only change detection, not security.
static uint64_t
ucm_hash(uint64_t h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t i = 0;
	// Four independent lanes (no serial multiply chain), folded at the end.
	uint64_t l[4] = {h, h ^ 0x9e3779b97f4a7c15ULL, h ^ 0xc2b2ae3d27d4eb4fULL, h ^ 0x165667b19e3779f9ULL};
	for (; i + 32 <= len; i += 32) {
		uint64_t w[4];
		memcpy(w, p + i, sizeof(w));
		for (int k = 0; k < 4; k++) {
			l[k] = (l[k] ^ w[k]) * 1099511628211ULL;
			l[k] ^= l[k] >> 29;
		}
	}
	h = l[0];
	for (int k = 1; k < 4; k++) {
		h = (h ^ l[k]) * 1099511628211ULL;
	}
	for (; i + 8 <= len; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, sizeof(w));
		h = (h ^ w) * 1099511628211ULL;
		h ^= h >> 29;
	}
	for (; i < len; i++) {
		h = (h ^ p[i]) * 1099511628211ULL;
	}
	return h;
}

// The copy's rows exactly as read back (only the texels the plan covers).
static uint64_t
ucm_hash_patch(uint64_t h, const struct u_cursor_measure_plan *plan, const struct u_cursor_measure_patch *patch)
{
	const size_t row_bytes = (size_t)plan->w * u_cursor_measure_format_bytes(patch->format);
	const uint8_t *base = (const uint8_t *)patch->data;
	h = ucm_hash(h, &patch->format, sizeof(patch->format));
	for (int32_t r = 0; r < plan->h; r++) {
		h = ucm_hash(h, base + (size_t)r * patch->row_pitch, row_bytes);
	}
	return h;
}

bool
u_cursor_measure_run(const struct u_cursor_measure_request *req,
                     const struct u_cursor_measure_plan *plan,
                     const struct u_cursor_measure_patch *view_a,
                     const struct u_cursor_measure_patch *view_b,
                     struct u_cursor_measure_cache *cache,
                     struct u_cursor_measure_result *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!u_cursor_measure_should_sample(req) || plan == NULL || view_a == NULL || view_b == NULL ||
	    plan->factor < 1 || plan->work_w < 1 || plan->work_h < 1 || plan->work_w > U_CURSOR_MEASURE_MAX_WORK_W ||
	    plan->work_h > U_CURSOR_MEASURE_MAX_WORK_H || plan->sub_w <= 0 || plan->sub_h <= 0) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		const struct u_cursor_measure_patch *pt = i == 0 ? view_a : view_b;
		const uint32_t bpp = u_cursor_measure_format_bytes(pt->format);
		if (pt->data == NULL || bpp == 0 || pt->row_pitch < (uint32_t)plan->w * bpp) {
			return false;
		}
	}
	const int32_t f = plan->factor;

	// The tip and the footprint, in the work patch.
	const float tip_x = req->u * (float)plan->sub_w / (float)f - (float)(plan->x / f);
	const float tip_y = req->v * (float)plan->sub_h / (float)f - (float)(plan->top_y / f);
	const float height_px = req->height * (float)plan->sub_h / (float)f;

	// A paused video re-submits the same pixels: same copy, same pointer,
	// same answer - do not match it again (ADR-046 Amendment 3 point 4). The
	// raw copy is hashed before any conversion, so a reused frame costs only
	// this pass over the bytes.
	uint64_t hash = 14695981039346656037ULL;
	const float key[3] = {tip_x, tip_y, height_px};
	const int32_t dims[4] = {plan->work_w, plan->work_h, plan->factor, plan->bottom_up ? 1 : 0};
	hash = ucm_hash(hash, key, sizeof(key));
	hash = ucm_hash(hash, dims, sizeof(dims));
	hash = ucm_hash_patch(hash, plan, view_a);
	hash = ucm_hash_patch(hash, plan, view_b);
	if (cache != NULL && cache->primed && cache->hash == hash) {
		*out = cache->result;
		out->tag = req->tag;
		out->reused = true;
		return true;
	}

	uint8_t la[U_CURSOR_MEASURE_MAX_WORK_W * U_CURSOR_MEASURE_MAX_WORK_H];
	uint8_t lb[U_CURSOR_MEASURE_MAX_WORK_W * U_CURSOR_MEASURE_MAX_WORK_H];
	if (!ucm_luma(plan, view_a, la) || !ucm_luma(plan, view_b, lb)) {
		return false;
	}

	float pts[U_CURSOR_MEASURE_POINTS][2];
	u_cursor_measure_footprint_points(tip_x, tip_y, height_px, pts);
	float d = 0.0f;
	out->confident = u_cursor_measure_footprint(la, lb, plan->work_w, plan->work_h, (const float(*)[2])pts,
	                                            U_CURSOR_MEASURE_POINTS, &d, out);
	// Work px, x_A - x_B (> 0 crossed) -> source px, x_B - x_A (< 0 crossed).
	out->disparity_px = out->confident ? -d * (float)f : 0.0f;
	out->sub_w = (float)plan->sub_w;
	out->tag = req->tag;
	out->valid = true;

	if (cache != NULL) {
		cache->primed = true;
		cache->hash = hash;
		cache->result = *out;
	}
	return true;
}

bool
u_cursor_measure_to_baselines(const struct u_cursor_depth_view *a,
                              const struct u_cursor_depth_view *b,
                              const struct u_cursor_depth_geometry *g,
                              float measured_px,
                              float sub_w,
                              float *out_disparity,
                              float *out_px_per_baseline)
{
	if (a == NULL || b == NULL || g == NULL || !(sub_w > 0.0f) || !isfinite(measured_px)) {
		return false;
	}
	const float tw = tanf(a->fov.angle_right) - tanf(a->fov.angle_left);
	const float th = tanf(a->fov.angle_up) - tanf(a->fov.angle_down);
	if (!(tw > 0.0f) || !(th > 0.0f)) {
		return false;
	}
	// Every Kooima view frames the canvas, so its fov aspect is the canvas's.
	const float canvas_w = g->canvas_height * tw / th;
	const struct xrt_vec3 right = quat_rotate(g->orientation, (struct xrt_vec3){1.0f, 0.0f, 0.0f});
	const float baseline = v3_dot(v3_sub(b->pose.position, a->pose.position), right);
	if (!(canvas_w > 0.0f) || !isfinite(canvas_w) || !(fabsf(baseline) > 1e-9f) || !isfinite(baseline)) {
		return false;
	}
	const float d = (measured_px / sub_w) * canvas_w / baseline;
	const float ppb = sub_w * fabsf(baseline) / canvas_w;
	if (!isfinite(d) || !isfinite(ppb)) {
		return false;
	}
	if (out_disparity != NULL) {
		*out_disparity = d;
	}
	if (out_px_per_baseline != NULL) {
		*out_px_per_baseline = ppb;
	}
	return true;
}

float
u_cursor_measure_target_px(const struct u_cursor_depth_tuning *t,
                           bool confident,
                           float measured_px,
                           float px_per_baseline)
{
	if (!confident || !(px_per_baseline > 0.0f) || !isfinite(px_per_baseline) || !isfinite(measured_px)) {
		return 0.0f;
	}
	// measured < 0 ? max(measured - margin_px, cap_px) : 0 - A.1 in pixels.
	return u_cursor_depth_target(t, true, measured_px / px_per_baseline) * px_per_baseline;
}


/*
 *
 * Crosstalk softening of the lifted cursor (ADR-046 Amendment 2 B.7).
 *
 */

void
u_cursor_soften_params_default(struct u_cursor_soften_params *out)
{
	out->blur = true;
	out->alpha = true;
	out->blur_k = U_CURSOR_SOFTEN_DEFAULT_BLUR_K;
	out->blur_max_px = U_CURSOR_SOFTEN_DEFAULT_BLUR_MAX_PX;
	out->blur_max_frac = U_CURSOR_SOFTEN_DEFAULT_BLUR_MAX_FRAC;
	out->alpha_min = U_CURSOR_SOFTEN_DEFAULT_ALPHA_MIN;
	out->full_d = U_CURSOR_SOFTEN_DEFAULT_FULL_D;
}

// A finite float in [lo, hi] (hi_open: < hi), else false.
static bool
soften_parse_float(const char *s, float lo, float hi, bool lo_open, float *out)
{
	if (s == NULL || *s == '\0') {
		return false;
	}
	char *end = NULL;
	const double v = strtod(s, &end);
	while (end != NULL && (*end == ' ' || *end == '\t')) {
		end++; // cmd's `set X=1 && ...` keeps the trailing space
	}
	if (end == s || *end != '\0' || !isfinite(v) || v > hi || v < lo || (lo_open && v <= lo)) {
		return false;
	}
	*out = (float)v;
	return true;
}

bool
u_cursor_soften_params_parse(const char *mode,
                             const char *blur_k,
                             const char *blur_max_px,
                             const char *blur_max_frac,
                             const char *alpha_min,
                             const char *full_d,
                             struct u_cursor_soften_params *out)
{
	u_cursor_soften_params_default(out);
	bool changed = false;
	if (mode != NULL) {
		// Compare the leading word: cmd's `set X=off && ...` keeps a space.
		size_t n = strcspn(mode, " \t");
		if (n == 3 && strncmp(mode, "off", 3) == 0) {
			out->blur = out->alpha = false;
		} else if (n == 5 && strncmp(mode, "alpha", 5) == 0) {
			out->blur = false;
		} else if (n == 4 && strncmp(mode, "blur", 4) == 0) {
			out->alpha = false;
		}
		// "both" or anything else: the default (both on).
		changed = !out->blur || !out->alpha;
	}
	changed |= soften_parse_float(blur_k, 0.0f, 1000.0f, false, &out->blur_k);
	changed |= soften_parse_float(blur_max_px, 0.0f, 8.0f, false, &out->blur_max_px);
	changed |= soften_parse_float(blur_max_frac, 0.0f, 0.5f, false, &out->blur_max_frac);
	changed |= soften_parse_float(alpha_min, 0.0f, 1.0f, false, &out->alpha_min);
	changed |= soften_parse_float(full_d, 0.0f, 1000.0f, true, &out->full_d);
	return changed;
}

struct u_cursor_soften
u_cursor_draw_soften(float d, float ghost_px, const struct u_cursor_soften_params *p)
{
	return u_cursor_draw_soften_sized(d, ghost_px, 0.0f, p);
}

struct u_cursor_soften
u_cursor_draw_soften_sized(float d, float ghost_px, float sprite_px, const struct u_cursor_soften_params *p)
{
	struct u_cursor_soften s = {0.0f, 1.0f};
	if (p == NULL) {
		return s;
	}
	if (p->blur && isfinite(ghost_px) && p->blur_k > 0.0f && p->blur_max_px > 0.0f) {
		float cap = p->blur_max_px;
		if (p->blur_max_frac > 0.0f && isfinite(sprite_px) && sprite_px > 0.0f) {
			const float rel = p->blur_max_frac * sprite_px; // proportional to the drawn cursor
			cap = rel < cap ? rel : cap;
		}
		const float b = p->blur_k * fabsf(ghost_px);
		s.blur_px = b < cap ? b : cap;
	}
	if (p->alpha && isfinite(d) && p->full_d > 0.0f) {
		float x = fabsf(d) / p->full_d;
		x = x < 1.0f ? x : 1.0f;
		const float ease = x * x * (3.0f - 2.0f * x); // smoothstep(0, full_d, |d|)
		const float amin = p->alpha_min < 0.0f ? 0.0f : (p->alpha_min > 1.0f ? 1.0f : p->alpha_min);
		s.alpha = 1.0f - (1.0f - amin) * ease;
	}
	return s;
}

float
u_cursor_draw_ghost_px(const struct u_cursor_draw_request *req, float tile_px_w, float tile_px_h)
{
	if (req == NULL || !(tile_px_w > 0.0f) || !(tile_px_h > 0.0f)) {
		return 0.0f;
	}
	const uint32_t n = req->view_count < XRT_MAX_VIEWS ? req->view_count : XRT_MAX_VIEWS;
	float ghost = 0.0f;
	for (uint32_t i = 0; i + 1 < n; i++) {
		const float dx = (req->view[i + 1].origin[0] - req->view[i].origin[0]) * tile_px_w;
		const float dy = (req->view[i + 1].origin[1] - req->view[i].origin[1]) * tile_px_h;
		const float g = sqrtf(dx * dx + dy * dy);
		if (isfinite(g) && g > ghost) {
			ghost = g;
		}
	}
	return ghost;
}

void
u_cursor_draw_soften_quad(
    const float corners[4][2], float tile_px_w, float tile_px_h, float blur_px, struct u_cursor_soften_quad *out)
{
	static const float uv[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
	memcpy(out->corners, corners, sizeof(out->corners));
	memcpy(out->uv, uv, sizeof(out->uv));
	out->radius_uv[0] = out->radius_uv[1] = 0.0f;

	// The quad's u axis (TL -> TR) and v axis (TL -> BL), tile-normalised.
	const float ux = corners[2][0] - corners[0][0], uy = corners[2][1] - corners[0][1];
	const float vx = corners[1][0] - corners[0][0], vy = corners[1][1] - corners[0][1];
	const float w_px = sqrtf(ux * ux * tile_px_w * tile_px_w + uy * uy * tile_px_h * tile_px_h);
	const float h_px = sqrtf(vx * vx * tile_px_w * tile_px_w + vy * vy * tile_px_h * tile_px_h);
	if (!(blur_px > 0.0f) || !(w_px > 0.0f) || !(h_px > 0.0f) || !isfinite(w_px) || !isfinite(h_px)) {
		return;
	}
	const float ru = blur_px / w_px;
	const float rv = blur_px / h_px;
	out->radius_uv[0] = ru;
	out->radius_uv[1] = rv;
	for (int k = 0; k < 4; k++) {
		const float u = uv[k][0] > 0.5f ? 1.0f + ru : -ru;
		const float v = uv[k][1] > 0.5f ? 1.0f + rv : -rv;
		out->uv[k][0] = u;
		out->uv[k][1] = v;
		out->corners[k][0] = corners[0][0] + u * ux + v * vx;
		out->corners[k][1] = corners[0][1] + u * uy + v * vy;
	}
}
