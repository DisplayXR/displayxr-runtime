// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric and display-centric stereo math for 3D displays.
 *
 * Runtime-native port of test_apps/common/camera3d_view.c and display3d_view.c.
 * Uses xrt_* types (xrt_vec3, xrt_fov, xrt_pose) instead of OpenXR types.
 *
 * Camera-centric: the app defines a virtual camera (position, orientation,
 * vFOV) and eye tracking data produces per-eye asymmetric frustum views.
 *
 * Display-centric: the physical display is the reference frame; Kooima
 * asymmetric frustum is computed from eye position relative to screen.
 *
 * Both paths share IPD/parallax factor processing.
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
 * Screen dimensions for stereo3d computations.
 * @ingroup aux_math
 */
struct m_stereo3d_screen
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
struct m_stereo3d_camera_tunables
{
	float ipd_factor;               //!< [0, 1] -- scales inter-eye distance
	float parallax_factor;          //!< [0, 1] -- lerps center toward nominal
	float inv_convergence_distance; //!< 1/convergence_dist (1/meters)
	float half_tan_vfov;            //!< tan(vFOV/2), pre-divided by zoom
};

/*!
 * Compute camera-centric stereo FOV and world-space eye positions.
 *
 * Pipeline:
 *   1. Apply IPD factor + parallax factor
 *   2. eye_local = processed - (0, 0, nominal_z)
 *   3. Transform eye_local to world space via camera_pose
 *   4. Scale eye_local by inv_convergence_distance for projection shifts
 *   5. Compute asymmetric tangent half-angles from half_tan_vfov + aspect
 *   6. Convert tangents to xrt_fov angles
 *
 * @param raw_left       Raw left eye in display space
 * @param raw_right      Raw right eye in display space
 * @param nominal_viewer Nominal viewer position (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (for aspect ratio)
 * @param tunables       Camera tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param out_fovs       Output per-eye FOVs [2]
 * @param out_eye_world  Output per-eye world-space positions [2]
 *
 * @ingroup aux_math
 */
void
m_stereo3d_camera_compute(const struct xrt_vec3 *raw_left,
                          const struct xrt_vec3 *raw_right,
                          const struct xrt_vec3 *nominal_viewer,
                          const struct m_stereo3d_screen *screen,
                          const struct m_stereo3d_camera_tunables *tunables,
                          const struct xrt_pose *camera_pose,
                          struct xrt_fov *out_fovs,
                          struct xrt_vec3 *out_eye_world);

/*!
 * Apply IPD and parallax factors to raw eye positions.
 *
 * IPD factor: scales inter-eye vector around center (0=mono, 1=full).
 * Parallax factor: lerps eye center toward nominal viewer (0=no tracking, 1=full).
 *
 * Shared between camera-centric and display-centric paths.
 *
 * @param raw_left        Raw left eye position
 * @param raw_right       Raw right eye position
 * @param nominal_viewer  Nominal viewer position (or NULL for {0,0,0.5})
 * @param ipd_factor      IPD scaling factor [0, 1]
 * @param parallax_factor Parallax lerp factor [0, 1]
 * @param out_left        Output processed left eye position
 * @param out_right       Output processed right eye position
 *
 * @ingroup aux_math
 */
void
m_stereo3d_apply_eye_factors(const struct xrt_vec3 *raw_left,
                             const struct xrt_vec3 *raw_right,
                             const struct xrt_vec3 *nominal_viewer,
                             float ipd_factor,
                             float parallax_factor,
                             struct xrt_vec3 *out_left,
                             struct xrt_vec3 *out_right);

/*!
 * Default camera tunables: ipd=1, parallax=1, inv_conv=2.0 (0.5m), half_tan=tan(18deg).
 * @ingroup aux_math
 */
struct m_stereo3d_camera_tunables
m_stereo3d_default_camera_tunables(void);

#ifdef __cplusplus
}
#endif
