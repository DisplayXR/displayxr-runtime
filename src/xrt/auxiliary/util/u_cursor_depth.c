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
	// 3% of the baseline is ~2 mm of crossed on-screen disparity for a 65 mm
	// IPD: enough to read as "in front", small enough not to look detached.
	t->margin = 0.03f;
	// d = -0.6 is t = 0.625: the cursor may come ~3/8 of the way to the eye.
	t->min_disparity = -0.6f;
	t->max_disparity = 0.6f;
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
	float d = content_disparity - t->margin;
	if (d < t->min_disparity) {
		d = t->min_disparity;
	}
	if (d > t->max_disparity) {
		d = t->max_disparity;
	}
	return d;
}

float
u_cursor_depth_filter_step(struct u_cursor_depth_filter *f,
                           const struct u_cursor_depth_tuning *t,
                           float target,
                           uint64_t now_ns)
{
	if (!f->primed || now_ns < f->last_ns || (double)(now_ns - f->last_ns) * 1e-9 > (double)t->stale_s) {
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
