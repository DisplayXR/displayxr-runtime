// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift viewpoint policy (ADR-048): the pure math the service's
 *         lift thread applies to TRACKED eyes before a conversion.
 *
 * Input: the tracked eye pair (or N eyes), ALREADY rebased to the centre of the
 * lifted rect (display axes, metres, +z toward the viewer). Output: the
 * viewpoints the vendor module synthesizes for. In order:
 *
 *  1. ipd factor   - scale each eye's offset from the pair's midpoint
 *                    (display rig step 1a, docs/architecture/kooima-projection.md);
 *  2. parallax     - lerp the midpoint toward the nominal viewer
 *                    (0, 0, nominal_z) by 1 - parallax_factor (step 1b);
 *  3. axis mask    - X: midpoint y = 0 and z = nominal_z; XY: z = nominal_z;
 *  4. recenter     - the per-stream ease-back filter (see u_lift_recenter):
 *                    after a hold the camera returns to the scene camera
 *                    origin; new head motion gives temporary look-around;
 *  5. clamp        - midpoint x / y to +- max_offset_m (0 = unclamped).
 *
 * No locks, no clock, no GPU: the caller passes time in, so the filter is
 * unit-testable (tests/tests_aux_lift_viewpoint.cpp).
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Values match XR_LIFT_AXIS_MODE_*_DXR / XRT_DP_LIFT_AXIS_*.
#define U_LIFT_AXIS_X 1u
#define U_LIFT_AXIS_XY 2u
#define U_LIFT_AXIS_XYZ 3u

//! Values match XR_LIFT_RECENTER_MODE_*_DXR.
#define U_LIFT_RECENTER_OFF 0u
#define U_LIFT_RECENTER_EASE_BACK 1u

#define U_LIFT_RECENTER_HOLD_DEFAULT_S 1.0f
#define U_LIFT_RECENTER_TAU_DEFAULT_S 2.0f
//! Midpoint offset (metres, live axes) below which the viewer counts as "at
//! the reference": the hold timer does not run.
#define U_LIFT_RECENTER_THRESHOLD_M 0.005f
//! Upper bound on the filter's time step: a stalled stream (paused, module
//! busy for seconds) resumes smoothly instead of jumping.
#define U_LIFT_RECENTER_MAX_DT_S 0.25f
//! Nominal viewing distance used when the display reports none.
#define U_LIFT_NOMINAL_Z_DEFAULT_M 0.5f
//! Most eyes u_lift_viewpoint_apply processes (extra input eyes are ignored).
#define U_LIFT_VIEWPOINT_MAX 16

//! The policy knobs (XrLiftViewControlDXR, normalised).
struct u_lift_view_control
{
	float ipd_factor;       //!< [0, 1]
	float parallax_factor;  //!< [0, 1]
	uint32_t axis_mode;     //!< U_LIFT_AXIS_*
	float max_offset_m;     //!< <= 0 = unclamped
	uint32_t recenter_mode; //!< U_LIFT_RECENTER_*
	float hold_s;           //!< >= 0
	float tau_s;            //!< > 0
};

//! Per-stream recenter state. Zero-initialised = reset.
struct u_lift_recenter
{
	bool valid;       //!< false = the next sample re-initialises
	uint64_t last_ns; //!< time of the previous sample
	float ref[3];     //!< the reference viewer midpoint (follows the head)
	float beyond_s;   //!< how long the offset has stayed beyond the threshold
	bool easing;      //!< hold elapsed: the reference is moving toward the head
};

//! The defaults (ADR-048): ipd 1, parallax 1, X, unclamped, EASE_BACK, 1 s, 2 s.
void
u_lift_view_control_default(struct u_lift_view_control *vc);

/*!
 * Clamp / default every field of @p vc into its valid range: factors to
 * [0, 1], unknown axis mode -> X, unknown recenter mode -> EASE_BACK,
 * hold < 0 -> default, tau <= 0 -> default, non-finite values -> defaults.
 */
void
u_lift_view_control_sanitize(struct u_lift_view_control *vc);

//! Forget the filter state (stream create, tracking loss).
void
u_lift_recenter_reset(struct u_lift_recenter *r);

/*!
 * Rebase display-space points to a rect centre: p - centre, in place.
 * Translation only: a lifted rect lies flat on the panel.
 */
void
u_lift_viewpoint_rebase(float *xyz, uint32_t count, const float centre[3]);

/*!
 * Run the policy on @p count eyes (@p in_xyz, 3 floats each, rect-relative).
 *
 * @param vc              sanitized policy
 * @param r               per-stream filter state (updated); NULL = no recentering
 * @param nominal_z       nominal viewing distance, metres (<= 0 = default 0.5)
 * @param now_ns          monotonic time of this sample
 * @param in_xyz          input eyes
 * @param count           number of eyes (<= U_LIFT_VIEWPOINT_MAX used)
 * @param out_xyz         count * 3 floats (may alias @p in_xyz)
 * @param out_baseline_m  optional: distance between the first and last output eye
 * @param out_mid         optional: the output midpoint (3 floats)
 */
void
u_lift_viewpoint_apply(const struct u_lift_view_control *vc,
                       struct u_lift_recenter *r,
                       float nominal_z,
                       uint64_t now_ns,
                       const float *in_xyz,
                       uint32_t count,
                       float *out_xyz,
                       float *out_baseline_m,
                       float *out_mid);

#ifdef __cplusplus
}
#endif
