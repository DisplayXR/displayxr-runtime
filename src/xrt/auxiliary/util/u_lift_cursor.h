// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth-aware cursor on lifted content (ADR-046 Amendment 1, Phase 3c):
 *         content depth -> cursor disparity -> where each view draws the sprite.
 *
 * Pure math, no graphics API, so it is host-tested (tests_aux_lift_cursor).
 *
 * Frame: the lifted rect, as in ADR-048 — origin at the rect centre, display
 * axes (+x right, +y up, +z toward the viewer), metres; the screen is z = 0.
 * The viewpoints are the ones the lifted views were synthesized for, so a
 * sprite projected from each of them lines up with the lifted picture.
 *
 * Placement is ADR-046 §2-§3 specialised to a known screen plane: E is the
 * midpoint of the viewpoints, S the cursor's point on the rect, and a point at
 * height z in front of the screen on the ray E->S sits at t = (E.z - z) / E.z,
 * disparity d = 1 - 1/t (eye-baseline units, < 0 in front). The policy
 * (margin, clamp, rise/sink smoothing) is u_cursor_depth's, unchanged.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_cursor_depth.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Most views u_lift_cursor_project handles (XRT_LIFT_MAX_VIEWS).
#define U_LIFT_CURSOR_MAX_VIEWS 8

/*!
 * Content height in front of the screen (metres) of one depth sample, through
 * the module's relief mapping z = relief_scale / depth + relief_offset.
 *
 * @param sample          the raw texel value.
 * @param encoding_inverse true = the decoded value is 1/depth.
 * @return false for a sample that decodes to no usable depth (<= 0, NaN, inf).
 */
bool
u_lift_cursor_relief_z(float sample,
                       bool encoding_inverse,
                       float value_scale,
                       float value_offset,
                       float relief_scale,
                       float relief_offset,
                       float *out_z);

/*!
 * The nearest content (largest z) among @p count raw samples; false if none
 * decodes. Same arguments as u_lift_cursor_relief_z.
 */
bool
u_lift_cursor_nearest_z(const float *samples,
                        uint32_t count,
                        bool encoding_inverse,
                        float value_scale,
                        float value_offset,
                        float relief_scale,
                        float relief_offset,
                        float *out_z);

/*!
 * Midpoint of @p count viewpoints (xyz triples). False if none is in front of
 * the screen.
 */
bool
u_lift_cursor_eye_midpoint(const float *viewpoints, uint32_t count, float out_e[3]);

/*!
 * Disparity (eye-baseline units) of content @p z metres in front of the
 * screen, seen from @p e. False if the content is not in front of the eye.
 */
bool
u_lift_cursor_disparity_of_z(const float e[3], float z, float *out_disparity);

/*!
 * Where each view draws the cursor: the point C at @p disparity on the
 * cyclopean ray from @p e through the cursor's rect point (@p sx, @p sy, 0),
 * projected onto the screen from each viewpoint. Writes @p count (x, y) pairs
 * into @p out_xy (rect-centre metres). False on degenerate input; then nothing
 * should be drawn.
 */
bool
u_lift_cursor_project(const float e[3],
                      float sx,
                      float sy,
                      float disparity,
                      const float *viewpoints,
                      uint32_t count,
                      float *out_xy);

#ifdef __cplusplus
}
#endif
