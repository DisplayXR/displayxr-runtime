// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-058 late placement of converted content: the viewpoint delta
 *         between the eyes a result was produced for and the eyes of the
 *         frame it is shown in, the bounded-correction policy, and a CPU
 *         reference of late view synthesis.
 *
 * A lifted result is synthesized for the eyes S3 snapped when its frame was
 * converted, then shown until the next result lands. ADR-058 re-places it at
 * the eyes of every display frame. This module is the pure half of that: it
 * decides WHETHER and HOW MUCH to correct, and resolves the late synthesis
 * inputs; the GPU stage per backend (a later step, once ADR-058 is accepted)
 * hands those inputs to the same conv_viewsyn_ops the conversion thread uses
 * today. Nothing here runs on a frame path yet.
 *
 * Two geometries a result can be re-placed from:
 *
 *  - COMPOSED: a depth-only provider + the runtime's reference view
 *    synthesis (S5). The source keeps the S5 inputs it was synthesized with
 *    (u_conv_viewsyn_params: depth units, the policy's range / convergence /
 *    budget, the rect); late synthesis is S5 again with the viewpoints
 *    replaced. Any depth kind S5 normalises is accepted: the disparity is the
 *    budget policy's, not a metric scale, so nothing is invented.
 *  - DISPLAY: METRIC auxiliary depth with a valid depthToDisplay
 *    (XR_DXR_lift.md §4.3): a texel of depth d lies on the ray from the
 *    viewpoint midpoint C through its rect position, at the fraction d / dc.
 *    That is S5's parallax exactly (q = dc / d - 1, source camera = C), so it
 *    is the same renderer. Late synthesis re-derives it for the CURRENT
 *    viewpoints -- what depthToDisplay would say had the result been produced
 *    for them -- keeping dc and the rect. RELATIVE depth, an invalid
 *    transform or a missing range HOLDS: the runtime never invents a scale
 *    (ADR-058 decision 3).
 *
 * What a late synthesis changes. Both geometries put the source camera at the
 * viewpoints' midpoint (the spec's C, S5's centroid), and S5's source uv is
 * x_s = p' + (E - C) q: a common (x, y) translation of every viewpoint leaves
 * every view unchanged. Late placement therefore corrects distance (z) and
 * inter-view (IPD / view-spread) changes, not lateral translation, which a
 * stale result already shows correctly. (A world-fixed forward warp, ADR-058
 * phase 2's wording, would add look-around a zero-latency provider never
 * shows; not done here.)
 *
 * Correction policy (ADR-058 decision 4): the largest per-view viewpoint
 * delta is compared with a limit (default 0.10 m, DXR_LIFT_REPROJECT_MAX_M,
 * fed in by the caller); within it the result is placed at the current eyes
 * at full disparity, beyond it the disparity is eased toward the plane
 * (smoothstep from 1 at the limit to 0 at twice the limit) rather than warped
 * into tearing.
 *
 * Frames: viewpoints are rect-relative metres (ADR-048 D1, +z toward the
 * viewer), the frame of XrLiftResultViewpointsDXR::viewpoints and of S5. The
 * content is attached to its rect, so the delta that matters is the eye's
 * motion relative to the rect.
 *
 * C11, no allocation, no clock, no GPU; host-tested by
 * tests/tests_aux_lift_reproject.cpp (unit) and
 * tests/tests_lift_late_viewsyn_conformance.cpp (the 4 fps conformance run).
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_conv_viewsyn.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Default correction limit, metres of viewpoint motion (ADR-058 decision 4).
#define U_LIFT_REPROJECT_MAX_M_DEFAULT 0.10f

//! The service environment knob a caller reads and feeds to
//! u_lift_reproject_policy_set_max_m (this module never reads the environment).
#define U_LIFT_REPROJECT_MAX_M_ENV "DXR_LIFT_REPROJECT_MAX_M"

#define U_LIFT_REPROJECT_MAX_VIEWS U_CONV_VIEWSYN_MAX_VIEWS


/*
 *
 * (a) Viewpoint delta.
 *
 */

struct u_lift_reproject_delta
{
	uint32_t view_count;
	float d_m[U_LIFT_REPROJECT_MAX_VIEWS][3]; //!< current - recorded, rect-relative metres
	float len_m[U_LIFT_REPROJECT_MAX_VIEWS];  //!< |d_m[i]|
	float max_m;                              //!< largest len_m (0 for no views)
};

/*!
 * Per-view delta between the viewpoints a result was produced for
 * (@p recorded) and the current ones (@p current), both rect-relative metres.
 * @return false (out zeroed) when @p view_count is 0 or above the maximum, or
 *         a coordinate is not finite.
 */
bool
u_lift_reproject_compute_delta(const float (*recorded)[3],
                               const float (*current)[3],
                               uint32_t view_count,
                               struct u_lift_reproject_delta *out);


/*
 *
 * (b) Correction policy.
 *
 */

struct u_lift_reproject_policy
{
	float max_m; //!< correction limit, metres of viewpoint motion
};

void
u_lift_reproject_policy_defaults(struct u_lift_reproject_policy *p);

/*!
 * Override the limit (the caller feeds DXR_LIFT_REPROJECT_MAX_M). Accepts a
 * finite value > 0; anything else is rejected and the policy is unchanged.
 */
bool
u_lift_reproject_policy_set_max_m(struct u_lift_reproject_policy *p, float max_m);

/*!
 * Disparity scale in [0, 1] for a viewpoint delta of @p delta_m: 1 up to the
 * limit, then smoothstep down to 0 at twice the limit; non-increasing in
 * @p delta_m. A non-finite delta eases fully (0).
 */
float
u_lift_reproject_ease(const struct u_lift_reproject_policy *p, float delta_m);

enum u_lift_reproject_verdict
{
	U_LIFT_REPROJECT_HOLD = 0,  //!< show the result as recorded (today's behaviour)
	U_LIFT_REPROJECT_PLACE = 1, //!< place at the current eyes, full disparity
	U_LIFT_REPROJECT_EASE = 2,  //!< place at the current eyes, disparity scaled by ease < 1
};

struct u_lift_reproject_decision
{
	enum u_lift_reproject_verdict verdict;
	float ease; //!< disparity scale applied (1 for PLACE, 0 for HOLD)
	struct u_lift_reproject_delta delta;
};


/*
 *
 * (c) Late synthesis.
 *
 */

enum u_lift_reproject_geometry
{
	U_LIFT_REPROJECT_COMPOSED = 0, //!< the S5 inputs of a composed stream
	U_LIFT_REPROJECT_DISPLAY = 1,  //!< METRIC aux depth placed by depthToDisplay
};

/*! DISPLAY geometry: XrLiftDepthResultDXR's alignment + the result's rect. */
struct u_lift_reproject_display
{
	bool transform_valid;
	float depth_to_display[16]; //!< column-major, XR_DXR_lift.md §4.3
	float rect_w_m, rect_h_m;   //!< XrLiftResultViewpointsDXR::rectSize
	float near_m, far_m;        //!< decoded depth bounds, metres (nearDepth / farDepth); both > 0
};

/*!
 * What the ring keeps per result for late placement (ADR-058 decision 2: the
 * source image + decoded depth, not the views) and the geometry it was
 * produced in.
 */
struct u_lift_reproject_source
{
	enum u_lift_reproject_geometry geometry;
	/*!
	 * COMPOSED: exactly the S5 params the result was synthesized with
	 * (viewpoints = the RECORDED ones). DISPLAY: the depth units in
	 * params.depth, the recorded view_count + viewpoints, and the look fields
	 * (u_conv_viewsyn_params_defaults); the geometry, range, convergence and
	 * budget are derived from @ref display.
	 */
	struct u_conv_viewsyn_params params;
	struct u_lift_reproject_display display;

	/* The images (CPU reference only; a GPU stage keeps its own). */
	const float *color; //!< RGBA floats, row 0 = top
	uint32_t color_w, color_h;
	const float *depth; //!< raw samples, row 0 = top
	uint32_t depth_w, depth_h;
};

/*!
 * DISPLAY -> the S5 params that reproduce depthToDisplay's placement for
 * @p viewpoints: dc and the rect centre R are recovered from the matrix and
 * the rect size; the source camera is the viewpoints' midpoint (S5's
 * centroid, the spec's C), convergence = dc, range = [near_m, far_m] (widened
 * to include dc), and the budget is the one that makes S5's parallax
 * q = dc / d - 1 (the texel at depth d sits at the fraction d / dc of the way
 * from C to the rect). With @p viewpoints equal to the ones the transform was
 * built for, the placement is the transform's.
 *
 * @param out_rect_center  optional: the rect centre R in display space
 * @return false (HOLD) for non-METRIC depth (RELATIVE / affine / pixel
 *         disparity / unknown), an invalid or degenerate transform, a
 *         missing range, or viewpoints at or behind the glass.
 */
bool
u_lift_reproject_display_params(const struct u_conv_viewsyn_params *base,
                                const struct u_lift_reproject_display *d,
                                const float (*viewpoints)[3],
                                uint32_t view_count,
                                struct u_conv_viewsyn_params *out,
                                float out_rect_center[3]);

/*!
 * Decide and resolve one late synthesis: the S5 params to render @p src's
 * content at @p current (rect-relative metres, one per view). PLACE / EASE
 * fill @p out_params (viewpoints = @p current, budget scaled by the ease);
 * HOLD leaves it untouched and the caller shows the result as recorded.
 * HOLD when the source cannot be resolved (above; COMPOSED with an UNKNOWN
 * depth kind), the view counts differ, or either geometry is invalid.
 *
 * This is the call a backend's late stage makes before conv_viewsyn_ops.
 */
enum u_lift_reproject_verdict
u_lift_reproject_late_params(const struct u_lift_reproject_policy *pol,
                             const struct u_lift_reproject_source *src,
                             const float (*current)[3],
                             uint32_t current_count,
                             struct u_conv_viewsyn_params *out_params,
                             struct u_lift_reproject_decision *out_decision);

/*!
 * CPU reference late synthesis: u_lift_reproject_late_params, then
 * u_conv_viewsyn_ref_render into @p out (view-major RGBA floats, see there).
 * With @p current equal to the recorded viewpoints of a COMPOSED source it is
 * bit-for-bit the reference render of the original params. HOLD (or a render
 * failure, reported as HOLD) leaves @p out untouched.
 */
enum u_lift_reproject_verdict
u_lift_reproject_render(const struct u_lift_reproject_policy *pol,
                        const struct u_lift_reproject_source *src,
                        const float (*current)[3],
                        uint32_t current_count,
                        uint32_t out_w,
                        uint32_t out_h,
                        float *out,
                        struct u_lift_reproject_decision *out_decision);


/*
 *
 * Re-run rule (the presenter's cache, ADR-058 phase 1).
 *
 */

//! A view's offset from the viewpoints' midpoint moved this far (a head
//! turn, an IPD / view-spread change): re-synthesize.
#define U_LIFT_REPROJECT_RERUN_SPREAD_M 0.0005f
//! The midpoint's distance from the glass moved this far: re-synthesize
//! (the composed budget scales with 1 / distance).
#define U_LIFT_REPROJECT_RERUN_DISTANCE_M 0.005f
//! The easing factor moved this much: re-synthesize.
#define U_LIFT_REPROJECT_RERUN_EASE 0.02f

/*!
 * Whether a late synthesis at @p cur (with ease @p cur_ease) would differ
 * visibly from the cached one made at @p prev (@p prev_ease). A common (x, y)
 * translation of viewpoints parallel to the glass never asks for one: both
 * geometries are invariant under it (x_s = p' + (E - C) q), so the cached
 * views are already right; with a turned head (views at different depths) it
 * shifts each view by about move * (dz / z) * q, which is bounded by the
 * spread threshold too. True for invalid input (re-synthesize when in doubt).
 */
bool
u_lift_reproject_needs_rerun(
    const float (*prev)[3], const float (*cur)[3], uint32_t view_count, float prev_ease, float cur_ease);


/*
 *
 * Result age (ADR-058 decision 4's stats split; internal until the
 * XrLiftStreamStatsDXR append lands with the ADR).
 *
 */

struct u_lift_reproject_ages
{
	uint64_t content_age_ns;   //!< now - when the shown content was submitted
	uint64_t placement_age_ns; //!< now - the eyes the shown views were placed for
};

/*!
 * Ages at @p now_ns of a result whose content was submitted at
 * @p content_ns and placed for eyes sampled at @p placement_ns (0 = unknown:
 * placement is then no fresher than the content). Times in the future read
 * as age 0.
 */
void
u_lift_reproject_compute_ages(uint64_t now_ns,
                              uint64_t content_ns,
                              uint64_t placement_ns,
                              struct u_lift_reproject_ages *out);

#ifdef __cplusplus
}
#endif
