// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth-aware cursor placement (XR_DXR_cursor_depth, ADR-046).
 *
 * Turns "the cursor is at canvas point (u,v) and the nearest content under it
 * is point P" into "draw a sprite of height h at position C", then smooths C
 * over time.
 *
 * Everything is solved from the views the runtime has just handed the app —
 * never from rig internals — so the result is correct for display rigs,
 * camera rigs, any m2v scale, display zones and any view count >= 2:
 *
 *  - the canvas point S under the cursor is where the two outermost view rays
 *    through (u,v) meet (Kooima frusta all frame the same canvas);
 *  - E, the cyclopean eye, is the midpoint of those two view positions;
 *  - a point's depth is t = its distance in front of E along the display
 *    normal, over S's (t = 1 on the canvas, t < 1 in front of it);
 *  - the policy works in DISPARITY d = 1 - 1/t, in units of the eye baseline
 *    (on-screen disparity = baseline * d): 0 on the canvas, < 0 in front.
 *    Disparity, not distance, is what the eye compares, so margins, clamps
 *    and slew rates are expressed there;
 *  - the sprite goes at C = E + t (S - E) — on the cyclopean ray, so it never
 *    slides sideways as it rises — with height scaled by t so its apparent
 *    size is constant.
 *
 * Pure C: time is a parameter, never read here.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Default sprite height, as a fraction of the canvas height.
#define U_CURSOR_DEPTH_DEFAULT_HEIGHT 0.03f

/*!
 * One located view: the pose and fov exactly as xrLocateViews returns them.
 */
struct u_cursor_depth_view
{
	struct xrt_pose pose;
	struct xrt_fov fov;
};

/*!
 * The cursor's line of sight, solved for one locate.
 */
struct u_cursor_depth_geometry
{
	struct xrt_vec3 eye;          //!< E: cyclopean eye (midpoint of the outer views)
	struct xrt_vec3 canvas_point; //!< S: the canvas point under the cursor
	struct xrt_vec3 forward;      //!< display normal pointing AWAY from the viewer
	struct xrt_quat orientation;  //!< display-plane orientation (the views')
	float eye_to_canvas;          //!< dot(S - E, forward), > 0
	float canvas_height;          //!< canvas height in locate-space units
};

/*!
 * Placement policy. Disparities are in eye-baseline units (see file comment).
 */
struct u_cursor_depth_tuning
{
	//! How far in front of the content the cursor floats (disparity, > 0).
	float margin;
	//! Most crossed disparity the cursor may reach (comfort clamp, < 0).
	float min_disparity;
	//! Most uncrossed disparity the cursor may reach (> 0).
	float max_disparity;
	//! Time constant when rising toward the viewer, seconds (fast: never lag behind content).
	float rise_tau_s;
	//! Time constant when sinking away from the viewer, seconds (slow: no flicker at edges).
	float sink_tau_s;
	//! A gap between steps longer than this re-primes (snaps) the filter, seconds.
	float stale_s;
};

/*!
 * The time filter's state. Zero-initialise; the first step snaps.
 */
struct u_cursor_depth_filter
{
	bool primed;
	float disparity;
	uint64_t last_ns;
};

void
u_cursor_depth_tuning_defaults(struct u_cursor_depth_tuning *t);

/*!
 * Solve the cursor's line of sight from two located views (normally view 0 and
 * the last active view). (u, v) is canvas-normalised, origin top-left, v down.
 *
 * @return false on degenerate input: coincident or parallel views, (u,v)
 *         outside [0,1], non-finite values, or a canvas not in front of E.
 */
bool
u_cursor_depth_geometry_solve(const struct u_cursor_depth_view *a,
                              const struct u_cursor_depth_view *b,
                              float u,
                              float v,
                              struct u_cursor_depth_geometry *out);

/*!
 * Disparity of a point (eye-baseline units, 0 on the canvas, < 0 in front).
 *
 * @return false if the point is not in front of the eye.
 */
bool
u_cursor_depth_point_disparity(const struct u_cursor_depth_geometry *g, const struct xrt_vec3 *p, float *out_disparity);

/*!
 * The unfiltered target: the content's disparity minus the margin, clamped;
 * 0 (the canvas) when there is no content under the cursor.
 */
float
u_cursor_depth_target(const struct u_cursor_depth_tuning *t, bool has_content, float content_disparity);

/*!
 * Advance the filter toward @p target and return the new disparity. Calls with
 * a time not after the previous one do not advance it (a second locate in the
 * same frame is idempotent).
 */
float
u_cursor_depth_filter_step(struct u_cursor_depth_filter *f,
                           const struct u_cursor_depth_tuning *t,
                           float target,
                           uint64_t now_ns);

/*!
 * Where to draw a sprite at @p disparity, and how tall, for a requested height
 * of @p height_fraction of the canvas.
 */
void
u_cursor_depth_place(const struct u_cursor_depth_geometry *g,
                     float disparity,
                     float height_fraction,
                     struct xrt_vec3 *out_position,
                     float *out_height);

#ifdef __cplusplus
}
#endif
