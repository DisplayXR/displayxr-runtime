// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Touch gesture recogniser + rig/aim math (see the header for the table).
 * @ingroup drv_touch_input
 */

#include "touch_gesture.h"

#include <math.h>
#include <string.h>


/*
 *
 * Tiny quaternion helpers. Local on purpose: this unit must link into the
 * ctest with nothing but xrt-interfaces, so no aux_math.
 *
 */

static void
quat_axis_angle(float x, float y, float z, double angle, struct xrt_quat *out)
{
	const double h = 0.5 * angle;
	const float s = (float)sin(h);
	out->x = x * s;
	out->y = y * s;
	out->z = z * s;
	out->w = (float)cos(h);
}

static void
quat_mul(const struct xrt_quat *a, const struct xrt_quat *b, struct xrt_quat *out)
{
	struct xrt_quat r;
	r.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
	r.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
	r.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
	r.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
	*out = r;
}

static void
quat_rotate(const struct xrt_quat *q, const float v[3], float out[3])
{
	// out = q v q*  via  t = 2 (q.xyz × v); out = v + q.w t + q.xyz × t
	const float qx = q->x, qy = q->y, qz = q->z, qw = q->w;
	const float tx = 2.0f * (qy * v[2] - qz * v[1]);
	const float ty = 2.0f * (qz * v[0] - qx * v[2]);
	const float tz = 2.0f * (qx * v[1] - qy * v[0]);
	out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
	out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
	out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

//! R = Ry(yaw) ∘ Rx(pitch).
static void
rig_orientation(const struct touch_gesture *g, struct xrt_quat *out)
{
	struct xrt_quat qy, qx;
	quat_axis_angle(0.0f, 1.0f, 0.0f, g->yaw, &qy);
	quat_axis_angle(1.0f, 0.0f, 0.0f, g->pitch, &qx);
	quat_mul(&qy, &qx, out);
}

//! Quaternion taking -Z onto the unit vector @p dir (the OpenXR aim convention).
static void
quat_look(const float dir[3], struct xrt_quat *out)
{
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const float d = fwd[0] * dir[0] + fwd[1] * dir[1] + fwd[2] * dir[2];
	if (d > 0.999999f) {
		out->x = out->y = out->z = 0.0f;
		out->w = 1.0f;
		return;
	}
	if (d < -0.999999f) {
		// Straight behind: half a turn about +Y.
		out->x = 0.0f;
		out->y = 1.0f;
		out->z = 0.0f;
		out->w = 0.0f;
		return;
	}
	// axis = fwd × dir, angle = acos(d)  →  q = (axis * sin(a/2), cos(a/2)), via the half-vector trick.
	const float ax = fwd[1] * dir[2] - fwd[2] * dir[1];
	const float ay = fwd[2] * dir[0] - fwd[0] * dir[2];
	const float az = fwd[0] * dir[1] - fwd[1] * dir[0];
	const float w = 1.0f + d;
	const float n = sqrtf(ax * ax + ay * ay + az * az + w * w);
	out->x = ax / n;
	out->y = ay / n;
	out->z = az / n;
	out->w = w / n;
}


/*
 *
 * Config + init.
 *
 */

void
touch_gesture_config_defaults(struct touch_gesture_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->display_width_m = 0.30f;
	cfg->display_height_m = 0.30f * 9.0f / 16.0f;
	cfg->viewer_x_m = 0.0f;
	cfg->viewer_y_m = 0.0f;
	cfg->viewer_z_m = 0.50f;
	cfg->sensitivity = 1.0f;
	cfg->tap_max_ns = 300LL * 1000 * 1000;
	cfg->double_tap_ns = 350LL * 1000 * 1000;
	cfg->slop_px = 0.0f; // resolved per surface size
}

static void
rig_reset(struct touch_gesture *g)
{
	g->yaw = 0.0f;
	g->pitch = 0.0f;
	g->d = g->cfg.viewer_z_m;
	g->pivot[0] = 0.0f;
	g->pivot[1] = 0.0f;
	g->pivot[2] = -g->cfg.viewer_z_m;
}

void
touch_gesture_init(struct touch_gesture *g, const struct touch_gesture_config *cfg)
{
	memset(g, 0, sizeof(*g));
	touch_gesture_config_defaults(&g->cfg);
	if (cfg != NULL) {
		if (cfg->display_width_m > 0.0f) {
			g->cfg.display_width_m = cfg->display_width_m;
		}
		if (cfg->display_height_m > 0.0f) {
			g->cfg.display_height_m = cfg->display_height_m;
		} else if (cfg->display_width_m > 0.0f) {
			g->cfg.display_height_m = cfg->display_width_m * 9.0f / 16.0f;
		}
		g->cfg.viewer_x_m = cfg->viewer_x_m;
		g->cfg.viewer_y_m = cfg->viewer_y_m;
		if (cfg->viewer_z_m > 0.0f) {
			g->cfg.viewer_z_m = cfg->viewer_z_m;
		}
		if (cfg->sensitivity > 0.0f) {
			g->cfg.sensitivity = cfg->sensitivity;
		}
		if (cfg->tap_max_ns > 0) {
			g->cfg.tap_max_ns = cfg->tap_max_ns;
		}
		if (cfg->double_tap_ns > 0) {
			g->cfg.double_tap_ns = cfg->double_tap_ns;
		}
		if (cfg->slop_px > 0.0f) {
			g->cfg.slop_px = cfg->slop_px;
		}
	}
	for (int i = 0; i < TOUCH_GESTURE_MAX_POINTERS; i++) {
		g->order[i] = -1;
	}
	g->mode = TOUCH_GESTURE_MODE_IDLE;
	rig_reset(g);
}


/*
 *
 * Pointer bookkeeping.
 *
 */

static float
slop_px(const struct touch_gesture *g)
{
	if (g->cfg.slop_px > 0.0f) {
		return g->cfg.slop_px;
	}
	uint32_t shorter = g->surface_w < g->surface_h ? g->surface_w : g->surface_h;
	float s = 0.02f * (float)shorter;
	return s < 8.0f ? 8.0f : s;
}

static int
pointer_find(const struct touch_gesture *g, uint32_t id)
{
	for (int i = 0; i < TOUCH_GESTURE_MAX_POINTERS; i++) {
		if (g->pointers[i].down && g->pointers[i].id == id) {
			return i;
		}
	}
	return -1;
}

static int
pointer_alloc(struct touch_gesture *g, uint32_t id)
{
	for (int i = 0; i < TOUCH_GESTURE_MAX_POINTERS; i++) {
		if (!g->pointers[i].down) {
			g->pointers[i].down = true;
			g->pointers[i].id = id;
			return i;
		}
	}
	return -1;
}

static void
order_push(struct touch_gesture *g, int idx)
{
	for (int i = 0; i < TOUCH_GESTURE_MAX_POINTERS; i++) {
		if (g->order[i] < 0) {
			g->order[i] = idx;
			g->down_count++;
			return;
		}
	}
}

static void
order_remove(struct touch_gesture *g, int idx)
{
	int w = 0;
	for (int r = 0; r < TOUCH_GESTURE_MAX_POINTERS; r++) {
		if (g->order[r] >= 0 && g->order[r] != idx) {
			g->order[w++] = g->order[r];
		}
	}
	for (; w < TOUCH_GESTURE_MAX_POINTERS; w++) {
		g->order[w] = -1;
	}
	if (g->down_count > 0) {
		g->down_count--;
	}
}

static const struct touch_gesture_pointer *
primary(const struct touch_gesture *g)
{
	return g->order[0] >= 0 ? &g->pointers[g->order[0]] : NULL;
}

static const struct touch_gesture_pointer *
secondary(const struct touch_gesture *g)
{
	return g->order[1] >= 0 ? &g->pointers[g->order[1]] : NULL;
}

static void
two_finger_reference(struct touch_gesture *g)
{
	const struct touch_gesture_pointer *a = primary(g);
	const struct touch_gesture_pointer *b = secondary(g);
	if (a == NULL || b == NULL) {
		return;
	}
	const float dx = b->x - a->x;
	const float dy = b->y - a->y;
	g->pinch_dist0 = sqrtf(dx * dx + dy * dy);
	g->pinch_d0 = g->d;
	g->pan_cx = 0.5f * (a->x + b->x);
	g->pan_cy = 0.5f * (a->y + b->y);
}

static void
lift_all(struct touch_gesture *g)
{
	for (int i = 0; i < TOUCH_GESTURE_MAX_POINTERS; i++) {
		g->pointers[i].down = false;
		g->order[i] = -1;
	}
	g->down_count = 0;
	g->mode = TOUCH_GESTURE_MODE_IDLE;
}


/*
 *
 * The rig moves.
 *
 */

static void
apply_orbit(struct touch_gesture *g, float dx, float dy)
{
	if (g->surface_w == 0 || g->surface_h == 0) {
		return;
	}
	const float s = g->cfg.sensitivity;
	g->yaw -= (float)(TOUCH_GESTURE_YAW_RAD_PER_WIDTH) * s * dx / (float)g->surface_w;
	g->pitch -= (float)(TOUCH_GESTURE_PITCH_RAD_PER_HEIGHT) * s * dy / (float)g->surface_h;
	if (g->pitch > (float)TOUCH_GESTURE_PITCH_LIMIT_RAD) {
		g->pitch = (float)TOUCH_GESTURE_PITCH_LIMIT_RAD;
	} else if (g->pitch < -(float)TOUCH_GESTURE_PITCH_LIMIT_RAD) {
		g->pitch = -(float)TOUCH_GESTURE_PITCH_LIMIT_RAD;
	}
	// Keep yaw bounded so a long session never loses float precision.
	const float two_pi = 6.28318530717958647692f;
	while (g->yaw > two_pi) {
		g->yaw -= two_pi;
	}
	while (g->yaw < -two_pi) {
		g->yaw += two_pi;
	}
}

static void
apply_two_finger(struct touch_gesture *g)
{
	const struct touch_gesture_pointer *a = primary(g);
	const struct touch_gesture_pointer *b = secondary(g);
	if (a == NULL || b == NULL || g->surface_w == 0 || g->surface_h == 0) {
		return;
	}

	// Dolly: pinch out (distance grows) brings the pivot closer.
	const float dx = b->x - a->x;
	const float dy = b->y - a->y;
	const float dist = sqrtf(dx * dx + dy * dy);
	if (g->pinch_dist0 > 1.0f && dist > 1.0f) {
		float d = g->pinch_d0 * g->pinch_dist0 / dist;
		const float lo = (float)TOUCH_GESTURE_DOLLY_MIN_FACTOR * g->cfg.viewer_z_m;
		const float hi = (float)TOUCH_GESTURE_DOLLY_MAX_FACTOR * g->cfg.viewer_z_m;
		if (d < lo) {
			d = lo;
		} else if (d > hi) {
			d = hi;
		}
		g->d = d;
	}

	// Pan: the centroid's motion, in display metres, carries the content —
	// so the CAMERA (and its pivot) moves the other way, in its own XY plane.
	const float cx = 0.5f * (a->x + b->x);
	const float cy = 0.5f * (a->y + b->y);
	const float mpp_x = g->cfg.display_width_m / (float)g->surface_w;
	const float mpp_y = g->cfg.display_height_m / (float)g->surface_h;
	const float local[3] = {-(cx - g->pan_cx) * mpp_x, (cy - g->pan_cy) * mpp_y, 0.0f};
	struct xrt_quat r;
	rig_orientation(g, &r);
	float world[3];
	quat_rotate(&r, local, world);
	g->pivot[0] += world[0];
	g->pivot[1] += world[1];
	g->pivot[2] += world[2];
	g->pan_cx = cx;
	g->pan_cy = cy;
}


/*
 *
 * Event feed.
 *
 */

static void
on_down(struct touch_gesture *g, const struct xrt_input_pointer_event *ev)
{
	int idx = pointer_find(g, ev->pointer_id);
	if (idx < 0) {
		idx = pointer_alloc(g, ev->pointer_id);
		if (idx < 0) {
			return; // more than TOUCH_GESTURE_MAX_POINTERS: ignore the extra
		}
		order_push(g, idx);
	}
	g->pointers[idx].x = ev->x;
	g->pointers[idx].y = ev->y;

	if (g->down_count == 1) {
		g->mode = TOUCH_GESTURE_MODE_PENDING;
		g->down_x = ev->x;
		g->down_y = ev->y;
		g->down_ns = ev->timestamp_ns;
		g->last_x = ev->x;
		g->last_y = ev->y;
		g->aim_x = ev->x;
		g->aim_y = ev->y;
		g->aim_valid = true;
		// A tap's pulse ends the moment a new finger lands: the new
		// contact decides what SELECT means from here.
		g->select_until_ns = 0;
	} else if (g->down_count == 2) {
		// A second finger ends whatever the first was doing — including a
		// hold's select — and starts the pinch/pan reference here.
		g->mode = TOUCH_GESTURE_MODE_TWO_FINGER;
		g->select_until_ns = 0;
		two_finger_reference(g);
	}
	// A third+ finger joins without changing anything: pinch/pan keep
	// reading the first two.
}

static void
on_move(struct touch_gesture *g, const struct xrt_input_pointer_event *ev)
{
	const int idx = pointer_find(g, ev->pointer_id);
	if (idx < 0) {
		return;
	}
	g->pointers[idx].x = ev->x;
	g->pointers[idx].y = ev->y;

	const struct touch_gesture_pointer *p = primary(g);
	const bool is_primary = p != NULL && p == &g->pointers[idx];

	switch (g->mode) {
	case TOUCH_GESTURE_MODE_PENDING: {
		if (!is_primary) {
			break;
		}
		g->aim_x = ev->x;
		g->aim_y = ev->y;
		const float ddx = ev->x - g->down_x;
		const float ddy = ev->y - g->down_y;
		const float s = slop_px(g);
		if (ddx * ddx + ddy * ddy > s * s) {
			// Moved past the slop before TAP_MAX: an orbit drag. The
			// motion since DOWN counts, so the first frame is not lost.
			g->mode = TOUCH_GESTURE_MODE_ORBIT;
			apply_orbit(g, ev->x - g->last_x, ev->y - g->last_y);
			g->last_x = ev->x;
			g->last_y = ev->y;
		}
		break;
	}
	case TOUCH_GESTURE_MODE_ORBIT:
		if (is_primary) {
			apply_orbit(g, ev->x - g->last_x, ev->y - g->last_y);
			g->last_x = ev->x;
			g->last_y = ev->y;
			g->aim_x = ev->x;
			g->aim_y = ev->y;
		}
		break;
	case TOUCH_GESTURE_MODE_SELECT_HOLD:
		// A select-drag: the aim ray follows, the rig stays put.
		if (is_primary) {
			g->aim_x = ev->x;
			g->aim_y = ev->y;
		}
		break;
	case TOUCH_GESTURE_MODE_TWO_FINGER: apply_two_finger(g); break;
	case TOUCH_GESTURE_MODE_IDLE:
	default: break;
	}
}

static void
on_up(struct touch_gesture *g, const struct xrt_input_pointer_event *ev)
{
	const int idx = pointer_find(g, ev->pointer_id);
	if (idx < 0) {
		return;
	}
	const struct touch_gesture_pointer *p = primary(g);
	const bool was_primary = p != NULL && p == &g->pointers[idx];
	const enum touch_gesture_mode mode = g->mode;

	if (was_primary) {
		// The ray ends where the finger lifted, so a tap's pulse fires there.
		g->aim_x = ev->x;
		g->aim_y = ev->y;
	}
	g->pointers[idx].down = false;
	order_remove(g, idx);

	if (g->down_count == 0) {
		g->mode = TOUCH_GESTURE_MODE_IDLE;
		if (mode == TOUCH_GESTURE_MODE_PENDING && was_primary) {
			// Up while still undecided = a TAP (tick() would already have
			// promoted a long one to a hold).
			if (g->last_tap_up_ns != 0 && ev->timestamp_ns - g->last_tap_up_ns <= g->cfg.double_tap_ns) {
				// Double tap: recenter. The durable level moves; the
				// first tap's pulse is cut so the pair reads as one
				// gesture, not a click plus a reset.
				g->recenter_ns = ev->timestamp_ns;
				g->last_tap_up_ns = 0;
				g->select_until_ns = 0;
				rig_reset(g);
			} else {
				g->last_tap_up_ns = ev->timestamp_ns;
				g->select_until_ns = ev->timestamp_ns + TOUCH_GESTURE_SELECT_PULSE_NS;
			}
		} else {
			// A drag, a hold or a pinch ended: not a tap, and it breaks a
			// double-tap sequence.
			g->last_tap_up_ns = 0;
		}
		return;
	}

	if (g->down_count == 1) {
		// Back to one finger after a pinch: it continues as a plain orbit
		// drag from where it is now (never a tap — it was part of a pinch).
		g->mode = TOUCH_GESTURE_MODE_ORBIT;
		const struct touch_gesture_pointer *np = primary(g);
		g->last_x = np->x;
		g->last_y = np->y;
		g->aim_x = np->x;
		g->aim_y = np->y;
		return;
	}

	// Still two or more: re-reference the pinch/pan on the surviving pair.
	two_finger_reference(g);
}

void
touch_gesture_feed(struct touch_gesture *g, const struct xrt_input_pointer_event *ev)
{
	if (g == NULL || ev == NULL) {
		return;
	}
	if (ev->surface_width > 0 && ev->surface_height > 0) {
		g->surface_w = ev->surface_width;
		g->surface_h = ev->surface_height;
	}
	switch (ev->action) {
	case XRT_INPUT_POINTER_ACTION_DOWN: on_down(g, ev); break;
	case XRT_INPUT_POINTER_ACTION_MOVE: on_move(g, ev); break;
	case XRT_INPUT_POINTER_ACTION_UP: on_up(g, ev); break;
	case XRT_INPUT_POINTER_ACTION_CANCEL:
		lift_all(g);
		g->last_tap_up_ns = 0;
		g->select_until_ns = 0;
		break;
	default: break;
	}
}

void
touch_gesture_tick(struct touch_gesture *g, int64_t now_ns)
{
	if (g == NULL) {
		return;
	}
	if (g->mode == TOUCH_GESTURE_MODE_PENDING && now_ns - g->down_ns >= g->cfg.tap_max_ns) {
		// Still past TAP_MAX: a press-and-hold. SELECT goes down and stays
		// down until the finger lifts; motion from here is a select-drag.
		g->mode = TOUCH_GESTURE_MODE_SELECT_HOLD;
		g->last_tap_up_ns = 0;
	}
	if (g->select_until_ns != 0 && now_ns >= g->select_until_ns) {
		g->select_until_ns = 0;
	}
}


/*
 *
 * Outputs.
 *
 */

void
touch_gesture_nav_pose(const struct touch_gesture *g, struct xrt_pose *out_pose)
{
	struct xrt_quat r;
	rig_orientation(g, &r);
	const float back[3] = {0.0f, 0.0f, g->d};
	float off[3];
	quat_rotate(&r, back, off);
	out_pose->orientation = r;
	out_pose->position.x = g->pivot[0] + off[0];
	out_pose->position.y = g->pivot[1] + off[1];
	out_pose->position.z = g->pivot[2] + off[2];
}

bool
touch_gesture_select(const struct touch_gesture *g, int64_t now_ns)
{
	if (g->mode == TOUCH_GESTURE_MODE_SELECT_HOLD) {
		return true;
	}
	return g->select_until_ns != 0 && now_ns < g->select_until_ns;
}

bool
touch_gesture_aim(const struct touch_gesture *g, struct xrt_pose *out_aim, struct xrt_pose *out_grip)
{
	if (!g->aim_valid || g->surface_w == 0 || g->surface_h == 0) {
		return false;
	}
	// Surface → display plane (rig-local: centre origin, +X right, +Y up).
	const float nx = 2.0f * g->aim_x / (float)g->surface_w - 1.0f;
	const float ny = 2.0f * g->aim_y / (float)g->surface_h - 1.0f;
	const float px = nx * 0.5f * g->cfg.display_width_m;
	const float py = -ny * 0.5f * g->cfg.display_height_m;

	float dir[3] = {px - g->cfg.viewer_x_m, py - g->cfg.viewer_y_m, 0.0f - g->cfg.viewer_z_m};
	const float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (n > 1e-6f) {
		dir[0] /= n;
		dir[1] /= n;
		dir[2] /= n;
	} else {
		dir[0] = 0.0f;
		dir[1] = 0.0f;
		dir[2] = -1.0f;
	}
	struct xrt_quat q;
	quat_look(dir, &q);

	if (out_aim != NULL) {
		out_aim->orientation = q;
		out_aim->position.x = g->cfg.viewer_x_m;
		out_aim->position.y = g->cfg.viewer_y_m;
		out_aim->position.z = g->cfg.viewer_z_m;
	}
	if (out_grip != NULL) {
		out_grip->orientation = q;
		out_grip->position.x = px;
		out_grip->position.y = py;
		out_grip->position.z = 0.0f;
	}
	return true;
}

int64_t
touch_gesture_recenter_timestamp(const struct touch_gesture *g)
{
	return g->recenter_ns;
}

enum touch_gesture_mode
touch_gesture_mode(const struct touch_gesture *g)
{
	return g->mode;
}
