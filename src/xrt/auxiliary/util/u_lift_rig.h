// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift app rig (ADR-048 Addendum A): map tracked eyes through
 *         an app's XR_DXR_view_rig display / camera rig into the PHYSICAL
 *         viewpoints a lift conversion synthesizes for.
 *
 * The lifted rect is the screen. The math is NOT re-implemented here: both
 * rigs run through displayxr-common's shared core (dxr_view_math.h) — the very
 * functions xrLocateViews runs (oxr_session.c, via displayxr_math_xrt) — and
 * only the last step, expressing the rig's eye back in physical rect-relative
 * metres, is local:
 *
 *  - display rig: dxr_display3d_compute_views with the rect as the screen and
 *    virtual_display_height = the rect height (so m2v = 1). eye_display is
 *    then the processed eye (ipd / parallax factors, step 1a / 1b) scaled by
 *    the perspective factor — already physical metres.
 *  - camera rig: dxr_camera3d_compute_views (identity pose) gives the eye in
 *    world units, l = m2v * (processed - nominal). The camera's off-axis
 *    frustum (half-tangents ht * aspect, ht, sheared by l * invd) is EXACTLY
 *    the Kooima frustum onto the rect of the physical viewer
 *        E = Z0 * (l * invd + (0, 0, 1)),   Z0 = rect_h / (2 * ht)
 *    — so E is the viewpoint (tests_aux_lift_rig_depth checks the frustum
 *    equality against the shared core). invd = 0 (convergence at infinity)
 *    collapses every eye to (0, 0, Z0): parallel cameras, no lift parallax.
 *
 * Pure: no locks, no clock, no GPU.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_LIFT_RIG_NONE 0u
#define U_LIFT_RIG_DISPLAY 1u
#define U_LIFT_RIG_CAMERA 2u

/*!
 * An app rig as the lift path consumes it (XrDisplayRigDXR / XrCameraRigDXR,
 * boundary-converted the way xrLocateViews converts them: verticalFov ->
 * half-tangent, convergenceDiopters = inverse convergence distance, 0 / unset
 * metersToVirtual -> 1). The rig pose and virtualDisplayHeight do not appear:
 * they only place the app's virtual world, never the physical viewpoints.
 */
struct u_lift_rig
{
	uint32_t type;                  //!< U_LIFT_RIG_*
	float ipd_factor;               //!< both rigs
	float parallax_factor;          //!< both rigs
	float perspective_factor;       //!< display rig
	float inv_convergence_distance; //!< camera rig, 1 / world units; 0 = infinity
	float half_tan_vfov;            //!< camera rig, tan(verticalFov / 2)
	float m2v;                      //!< camera rig, metres -> world units
};

/*!
 * Clamp every field into the range xrLocateViews clamps it to (oxr_session.c
 * view_rig_update_from_chain): factors [0, 1e4], perspective [0.1, 10],
 * convergence [0, 20], half-tangent of a [0.01, 3.13] rad FOV, m2v
 * [1e-4, 1e5] with <= 0 -> 1. Non-finite values take the neutral default.
 * Returns true if anything was clamped. Unknown types become NONE.
 */
bool
u_lift_rig_sanitize(struct u_lift_rig *rig);

/*!
 * Map @p count rect-relative eyes (display axes, metres, +z toward the
 * viewer; 3 floats each) through @p rig into physical rect-relative
 * viewpoints (@p out may alias @p in).
 *
 * @param rig        sanitized rig (NONE: @p out = @p in)
 * @param nominal_z  nominal viewer distance, metres (<= 0 = 0.5)
 * @param rect_w     lifted rect width, metres
 * @param rect_h     lifted rect height, metres
 * @return false (and @p out untouched) when the rect size is unknown or
 *         @p count is 0 / above 8 — the caller keeps the unrigged eyes.
 */
bool
u_lift_rig_apply(const struct u_lift_rig *rig,
                 float nominal_z,
                 float rect_w,
                 float rect_h,
                 const float *in_xyz,
                 uint32_t count,
                 float *out_xyz);

#ifdef __cplusplus
}
#endif
