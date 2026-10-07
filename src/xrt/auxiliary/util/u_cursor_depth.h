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


/*
 *
 * Phase 3a: the depth-layer source (XR_DXR_cursor_depth spec v2).
 *
 * The app submits depth with its projection layer (XR_KHR_composition_layer_
 * depth) and asks the runtime to find the nearest content under the cursor in
 * it. The compositor copies a cursor-sized patch of the two outermost views'
 * depth and reduces it to the nearest texel (u_cursor_depth_reduce_patch);
 * the state tracker turns that texel into a point in the layer's space
 * (u_cursor_depth_point_from_depth_sample) and feeds it to the v1 placement.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0). A compositor reads nothing,
 * allocates nothing and copies nothing unless the request it was handed
 * passes u_cursor_depth_patch_should_sample(): a zero-initialised request -
 * which is what every session that never chained XrCursorDepthSourceDXR has -
 * never does.
 *
 */

//! Most views a patch request covers (the outermost pair).
#define U_CURSOR_DEPTH_PATCH_VIEWS 2

/*!
 * Largest patch side, in texels. A ~1.5x-sprite footprint is ~50 texels on a
 * 4K panel's tile; clamping it smaller would shrink the footprint and let a
 * nearer edge beside the hotspot cut through the sprite again. 2 views x 64 x
 * 64 x 4 bytes = 32 KB per read, the most a request can ever copy.
 */
#define U_CURSOR_DEPTH_PATCH_MAX_DIM 64

/*!
 * What the state tracker asks a compositor to read at the next commit.
 *
 * Zero-initialised means "not requested".
 */
struct u_cursor_depth_patch_request
{
	//! True only for a commit whose frame chained the request.
	bool requested;
	//! Echoed in the result so the state tracker can match its frame records.
	uint64_t tag;
	//! Cursor hotspot, canvas-normalised (origin top-left, v down).
	float u, v;
	//! Footprint half-size, canvas-normalised, per axis (> 0).
	float radius_u, radius_v;
	//! Reversed-Z layer (nearZ > farZ): the nearest texel is the LARGEST value.
	bool nearest_is_max;
	//! Projection-layer view indices to read (the outermost active pair).
	uint32_t view_index[U_CURSOR_DEPTH_PATCH_VIEWS];
};

/*!
 * One view's answer: the nearest texel of the patch.
 */
struct u_cursor_depth_patch_sample
{
	bool valid;
	//! Texel centre, normalised to the view's depth sub-image (origin top-left).
	float su, sv;
	//! The raw window depth stored in that texel.
	float raw_depth;
};

/*!
 * What the compositor hands back, asynchronously (one or more frames later).
 */
struct u_cursor_depth_patch_result
{
	bool valid;
	uint64_t tag;
	struct u_cursor_depth_patch_sample view[U_CURSOR_DEPTH_PATCH_VIEWS];
};

/*!
 * The XrCompositionLayerDepthInfoKHR range values of one view.
 */
struct u_cursor_depth_layer_depth
{
	float min_depth; //!< window depth at distance near_z
	float max_depth; //!< window depth at distance far_z
	float near_z;    //!< distance of min_depth (may exceed far_z: reversed Z; may be +inf)
	float far_z;     //!< distance of max_depth (may be +inf)
};

/*!
 * The compositor-side gate. False for a zero-initialised (never requested)
 * request, and for one whose cursor is off the canvas or whose footprint is
 * degenerate - the compositor must then do no work at all.
 */
bool
u_cursor_depth_patch_should_sample(const struct u_cursor_depth_patch_request *req);

/*!
 * The texel rectangle to copy for one view: the footprint around (u, v)
 * mapped into the sub-image rect, clamped to it and to
 * U_CURSOR_DEPTH_PATCH_MAX_DIM per side.
 *
 * @return false if the rectangle is empty.
 */
bool
u_cursor_depth_patch_rect(const struct u_cursor_depth_patch_request *req,
                          int32_t sub_x,
                          int32_t sub_y,
                          int32_t sub_w,
                          int32_t sub_h,
                          int32_t *out_x,
                          int32_t *out_y,
                          int32_t *out_w,
                          int32_t *out_h);

/*!
 * Reduce a copied patch of float depth texels to the nearest one. NaN and
 * infinite texels are skipped.
 *
 * @param row_stride  texels between the starts of two rows (>= w)
 * @return false if no texel was usable.
 */
bool
u_cursor_depth_reduce_patch(const float *texels,
                            int32_t w,
                            int32_t h,
                            int32_t row_stride,
                            bool nearest_is_max,
                            int32_t *out_x,
                            int32_t *out_y,
                            float *out_raw);

/*!
 * Window depth -> distance along the view's -Z axis, for the hyperbolic
 * mapping every perspective projection writes: 1/z is linear in the window
 * depth, 1/near_z at min_depth and 1/far_z at max_depth. Covers ordinary Z,
 * reversed Z (near_z > far_z) and an infinite far (or, reversed, near) plane.
 *
 * @return false for the far end of the mapping itself (a cleared depth
 *         buffer: background, not content), values outside
 *         [min_depth, max_depth], and degenerate ranges.
 */
bool
u_cursor_depth_linear_depth(const struct u_cursor_depth_layer_depth *d, float raw_depth, float *out_z);

/*!
 * A depth texel -> the 3D point it shows, in the space of @p view's pose.
 * (su, sv) are normalised to the view's sub-image (origin top-left, v down),
 * which every Kooima frustum maps linearly onto its fov tangents.
 */
bool
u_cursor_depth_point_from_depth_sample(const struct u_cursor_depth_view *view,
                                       const struct u_cursor_depth_layer_depth *d,
                                       float su,
                                       float sv,
                                       float raw_depth,
                                       struct xrt_vec3 *out_point);

#ifdef __cplusplus
}
#endif
