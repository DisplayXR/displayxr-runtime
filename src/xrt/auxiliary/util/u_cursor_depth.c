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
