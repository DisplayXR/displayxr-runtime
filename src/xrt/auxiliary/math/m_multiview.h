// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric and display-centric multiview math for 3D displays.
 *
 * Runtime-native port of test_apps/common/camera3d_view.c and display3d_view.c.
 * Uses xrt_* types (xrt_vec3, xrt_fov, xrt_pose) instead of OpenXR types.
 *
 * Camera-centric: the app defines a virtual camera (position, orientation,
 * vFOV) and eye tracking data produces per-view asymmetric frustum views.
 *
 * Display-centric: the physical display is the reference frame; Kooima
 * asymmetric frustum is computed from eye position relative to screen.
 *
 * Both paths share view spread/parallax factor processing.
 *
 * See also: docs/architecture/stereo3d-math.md for full pipeline derivation.
 *
 * @author David Fattal
 * @ingroup aux_math
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Screen dimensions for multiview computations.
 * @ingroup aux_math
 */
struct m_multiview_screen
{
	float width_m;  //!< Physical screen width in meters
	float height_m; //!< Physical screen height in meters
};

/*!
 * Camera-centric tunable parameters.
 *
 * half_tan_vfov = tan(vFOV/2); divide by zoom at call site.
 * inv_convergence_distance = 1/convergence_dist in 1/meters.
 *
 * @ingroup aux_math
 */
struct m_multiview_camera_tunables
{
	float ipd_factor;               //!< [0, 1] -- scales inter-view distance
	float parallax_factor;          //!< [0, 1] -- lerps center toward nominal
	float inv_convergence_distance; //!< 1/convergence_dist (1/meters)
	float half_tan_vfov;            //!< tan(vFOV/2), pre-divided by zoom
};

/*!
 * Compute camera-centric N-view FOV and world-space eye positions.
 *
 * Pipeline:
 *   1. Apply view spread factor + parallax factor
 *   2. eye_local = processed - (0, 0, nominal_z)
 *   3. Transform eye_local to world space via camera_pose
 *   4. Scale eye_local by inv_convergence_distance for projection shifts
 *   5. Compute asymmetric tangent half-angles from half_tan_vfov + aspect
 *   6. Convert tangents to xrt_fov angles
 *
 * @param raw_eyes       Array of N raw eye positions in display space
 * @param count          Number of eyes (must be >= 1)
 * @param nominal_viewer Nominal viewer position (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (for aspect ratio)
 * @param tunables       Camera tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param out_fovs       Output per-view FOVs [count]
 * @param out_eye_world  Output per-view world-space positions [count]
 *
 * @ingroup aux_math
 */
void
m_multiview_camera_compute(const struct xrt_vec3 *raw_eyes,
                           uint32_t count,
                           const struct xrt_vec3 *nominal_viewer,
                           const struct m_multiview_screen *screen,
                           const struct m_multiview_camera_tunables *tunables,
                           const struct xrt_pose *camera_pose,
                           struct xrt_fov *out_fovs,
                           struct xrt_vec3 *out_eye_world);

/*!
 * Apply view spread and parallax factors to N raw eye positions.
 *
 * View spread factor: scales each eye's offset from centroid (0=mono, 1=full).
 * Parallax factor: lerps eye centroid toward nominal viewer (0=no tracking, 1=full).
 *
 * Shared between camera-centric and display-centric paths.
 *
 * @param raw_eyes       Array of N raw eye positions
 * @param count          Number of eyes (must be >= 1)
 * @param nominal_viewer Nominal viewer position (or NULL for {0,0,0.5})
 * @param ipd_factor     View spread scaling factor [0, 1]
 * @param parallax_factor Parallax lerp factor [0, 1]
 * @param out_eyes       Output array of N processed eye positions
 *
 * @ingroup aux_math
 */
void
m_multiview_apply_eye_factors(const struct xrt_vec3 *raw_eyes,
                              uint32_t count,
                              const struct xrt_vec3 *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              struct xrt_vec3 *out_eyes);

/*!
 * Compute camera-centric FOV and world-space position for a single view.
 *
 * Takes a pre-processed eye position (after apply_eye_factors) and computes
 * the asymmetric frustum FOV and world-space eye position.
 *
 * @param processed_eye  Processed eye position (after apply_eye_factors)
 * @param nominal_z      Nominal viewer Z distance (e.g. 0.5)
 * @param screen         Physical screen dimensions (for aspect ratio)
 * @param tunables       Camera tunables (inv_convergence_distance, half_tan_vfov)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param out_fov        Output FOV for this view
 * @param out_eye_world  Output world-space position for this view
 *
 * @ingroup aux_math
 */
void
m_multiview_camera_compute_view(const struct xrt_vec3 *processed_eye,
                                float nominal_z,
                                const struct m_multiview_screen *screen,
                                const struct m_multiview_camera_tunables *tunables,
                                const struct xrt_pose *camera_pose,
                                struct xrt_fov *out_fov,
                                struct xrt_vec3 *out_eye_world);

/*!
 * Default camera tunables: ipd=1, parallax=1, inv_conv=2.0 (0.5m), half_tan=tan(18deg).
 * @ingroup aux_math
 */
struct m_multiview_camera_tunables
m_multiview_default_camera_tunables(void);

#ifdef __cplusplus
}
#endif
