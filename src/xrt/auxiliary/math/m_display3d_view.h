// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unified display-centric multiview math for 3D displays
 *
 * Canonical implementation of Kooima asymmetric frustum projection, view matrix
 * construction, and multiview factor processing. Any project that needs
 * display-centric views should use this library instead of reimplementing
 * the math. Pure C, depends only on xrt_defines.h and <math.h>.
 *
 * Reference: Robert Kooima, "Generalized Perspective Projection" (2009)
 *
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/Metal).
 * DirectX callers should transpose when loading into row-major XMMATRIX.
 *
 * @ingroup aux_math
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Structs ---

typedef struct Display3DTunables {
	float ipd_factor;              //!< [0, 1] — scales inter-view distance (0=mono, 1=full)
	float parallax_factor;         //!< [0, 1] — lerps view center toward nominal (0=no tracking, 1=full)
	float perspective_factor;      //!< [0.1, 10] — scales eye XYZ only (changes object perspective)
	float virtual_display_height;  //!< Virtual display height in app units (always required;
	                               //!< use physical display height for 1:1 meters)
} Display3DTunables;

typedef struct Display3DScreen {
	float width_m;  //!< Physical screen width (meters)
	float height_m; //!< Physical screen height (meters)
} Display3DScreen;

typedef struct Display3DView {
	float view_matrix[16];       //!< Column-major 4x4 view matrix
	float projection_matrix[16]; //!< Column-major 4x4 projection matrix
	struct xrt_fov fov;          //!< Equivalent asymmetric FOV angles (radians)
	struct xrt_vec3 eye_display; //!< Modified eye position in display space (after all factors)
	struct xrt_vec3 eye_world;   //!< Eye position in world space (after display pose transform)
	struct xrt_quat orientation; //!< Display/camera orientation (same for all views)
} Display3DView;

// --- Functions ---

/*!
 * All-in-one: compute multiview view+projection from raw eye tracking data.
 *
 * Pipeline:
 *   1. Apply view spread factor (scale inter-view vector, keep center fixed)
 *   2. Apply parallax factor (lerp center toward nominal viewer)
 *   3. Apply perspective * m2v to eye XYZ (view matrix + Kooima eye)
 *   4. Apply m2v to screen W/H (Kooima screen dims)
 *   5. Transform display-space eye -> world-space via display_pose
 *   6. Build view matrix from world-space eye + display orientation
 *   7. Build Kooima projection matrix from display-space scaled eye + scaled screen
 *   8. Compute FOV angles from same
 *
 * @param raw_eyes       Array of N raw view positions in DISPLAY space (from xrLocateViews)
 * @param count          Number of views (must be >= 1)
 * @param nominal_viewer Nominal viewer position in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions
 * @param tunables       View factors (or NULL for defaults: all 1.0)
 * @param display_pose   Display pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out_views      Output array of N views
 *
 * @ingroup aux_math
 */
void
display3d_compute_views(const struct xrt_vec3 *raw_eyes,
                        uint32_t count,
                        const struct xrt_vec3 *nominal_viewer,
                        const Display3DScreen *screen,
                        const Display3DTunables *tunables,
                        const struct xrt_pose *display_pose,
                        float near_z,
                        float far_z,
                        Display3DView *out_views);

/*!
 * Compute Kooima FOV angles only (no matrices). Useful for runtime-side
 * computation where the app builds its own projection.
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @return xrt_fov with asymmetric frustum angles (radians)
 *
 * @ingroup aux_math
 */
struct xrt_fov
display3d_compute_fov(struct xrt_vec3 eye_pos, float screen_width_m, float screen_height_m);

/*!
 * Compute Kooima projection matrix (column-major float[16]).
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @param near_z           Near clip plane distance
 * @param far_z            Far clip plane distance
 * @param out_matrix       Output float[16], column-major
 *
 * @ingroup aux_math
 */
void
display3d_compute_projection(struct xrt_vec3 eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix);

/*!
 * Default tunables (all factors = 1.0).
 *
 * @ingroup aux_math
 */
Display3DTunables
display3d_default_tunables(void);

#ifdef __cplusplus
}
#endif
