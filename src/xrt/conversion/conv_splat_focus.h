// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's own focus (screen-plane depth) of a splat cloud, read
 *         from the splats themselves.
 *
 * The fallback behind the camera block a lift caller receives (XR_DXR_lift
 * spec 6): when a provider reports no focus suggestion -- or no facts at all --
 * the runtime still places the subject on the screen plane, from the splats it
 * was handed, never from which provider made them.
 *
 * Method: the opaque splats are projected through the camera (the reported
 * intrinsics when there are any, else the cloud's own angular extent) into a
 * small nearest-surface inverse-depth image, and the screen plane is the
 * auto-convergence rule lifted video uses (u_lift_autoconv: the centre-weighted
 * 80th percentile of nearness blended with the dominant clump, far background
 * excluded). A cloud with no confident estimate falls back to the plain
 * 80th percentile of nearness. near / far are robust (2nd / 98th) percentiles
 * of the opaque splats' depths.
 *
 * Pure C, no I/O; host-tested (tests_conv_store).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! How the runtime's focus was found.
enum conv_splat_focus_method
{
	CONV_SPLAT_FOCUS_NONE = 0,
	CONV_SPLAT_FOCUS_CONVERGENCE = 1, //!< the u_lift_autoconv screen-plane rule
	CONV_SPLAT_FOCUS_PERCENTILE = 2,  //!< 80th percentile of nearness (rule had no confident estimate)
};

struct conv_splat_focus
{
	enum conv_splat_focus_method method;
	float point[3];  //!< in the cloud's own coordinates (on the optical axis)
	float subject_m; //!< depth placed on the screen plane, cloud units
	float near_m;    //!< 2nd percentile of the opaque splats' depth
	float far_m;     //!< 98th percentile
	uint32_t splats; //!< opaque splats in front of the camera that were used
};

/*!
 * Locate the vertex x / y / z (and optional opacity) of a binary
 * little-endian PLY. @p out_header is the byte offset of the vertex data.
 * False for anything else (ASCII, big endian, a vertex element that is not
 * first, list properties in it, a truncated body).
 */
bool
conv_ply_vertex_layout(const uint8_t *ply,
                       size_t size,
                       size_t *out_header,
                       uint64_t *out_count,
                       uint32_t *out_stride,
                       int32_t out_xyz_opacity_offset[4]);

/*!
 * The runtime's focus of the 3DGS PLY @p ply.
 *
 * @param rub        the cloud is +x right, +y up, +z back (else OpenCV: +z forward).
 * @param intrinsics fx, fy, cx, cy, width, height of the camera the splats were
 *                   unprojected through, or NULL (the cloud's extent frames it).
 * @return false (method NONE) when the blob is not a usable PLY or too few
 *         splats lie in front of the camera.
 */
bool
conv_splat_focus_from_ply(
    const uint8_t *ply, size_t size, bool rub, const float intrinsics[6], struct conv_splat_focus *out);

#ifdef __cplusplus
}
#endif
