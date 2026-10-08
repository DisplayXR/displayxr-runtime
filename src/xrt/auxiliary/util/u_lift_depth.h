// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift auxiliary depth (ADR-048 Addendum A): the transform that
 *         places a lifted frame's depth map in display space, aligned with the
 *         viewpoints its views were synthesized for.
 *
 * Model (XR_DXR_lift.md §4.3). The woven result presents the source image on
 * the lifted rect, seen from the viewpoint midpoint C (rect-relative), with
 * the convergence depth dc on the screen plane. A texel at normalised
 * coordinates (un, vn) (texel centre, u right, v down, [0, 1]) sits on the
 * rect at
 *     P = (W * (un - 0.5), H * (0.5 - vn), 0)
 * and a texel of decoded depth d lies on the ray from C through P, at the
 * fraction d / dc of the way:
 *     X = R + C + (P - C) * d / dc
 * (R = rect centre in display space). d = dc -> on the rect, d -> 0 -> at the
 * viewpoint, d > dc -> behind the screen. X is linear in (un*d, vn*d, d, 1),
 * so it is one 4x4 matrix — column-major, m[col * 4 + row], output w = 1:
 *
 *     col 0 (un*d): ( W/dc,                0,             0,      0 )
 *     col 1 (vn*d): ( 0,                  -H/dc,          0,      0 )
 *     col 2 (d)   : ( (-W/2 - Cx)/dc,  (H/2 - Cy)/dc,   -Cz/dc,   0 )
 *     col 3 (1)   : ( Rx + Cx,          Ry + Cy,         Rz + Cz,  1 )
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

/*!
 * Build the depth-to-display transform (see the file comment).
 *
 * @param viewpoint          C: the viewpoints' midpoint, rect-relative metres
 * @param rect_center        R: the rect centre in display space, metres
 * @param rect_w             W, metres
 * @param rect_h             H, metres
 * @param convergence_depth  dc, decoded depth on the screen plane
 * @param out_m              16 floats, column-major
 * @return false (out_m = identity) when W, H, dc or C.z is not positive /
 *         finite — the caller reports the transform invalid.
 */
bool
u_lift_depth_to_display(const float viewpoint[3],
                        const float rect_center[3],
                        float rect_w,
                        float rect_h,
                        float convergence_depth,
                        float out_m[16]);

/*!
 * Apply @p m to one texel: normalised texel centre (@p un, @p vn) at decoded
 * depth @p d -> display-space point @p out_xyz. Reference for the spec and
 * the tests (an app does the same multiply in its shader).
 */
void
u_lift_depth_transform_point(const float m[16], float un, float vn, float d, float out_xyz[3]);

/*!
 * Decode one sample: d = scale * s + offset (scale 0 is read as 1); INVERSE
 * (@p inverse true) returns 1 / d, or 0 when d is 0.
 */
float
u_lift_depth_decode(float sample, float scale, float offset, bool inverse);

#ifdef __cplusplus
}
#endif
