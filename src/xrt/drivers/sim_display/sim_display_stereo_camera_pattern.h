// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display FAKE stereo camera (ADR-043): the synthetic SBS scene.
 *
 * Pure CPU rendering, no OS dependencies, so the unit tests pin it host-side
 * (tests_stereo_camera) next to the disparity probe that measures it.
 *
 * The scene is a random-dot stereogram with two planes at KNOWN integer
 * disparities (left x - right x, pixels): a textured background and a nearer
 * textured bar in the middle of the frame. A frame counter is burned into the
 * top-left of BOTH halves at zero disparity. The pair is parallel and
 * undistorted (the R1 fake reports NATIVELY_RECTIFIED); a distorted,
 * misaligned variant for the rectifier's tests is R2's.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sim_stereo_camera_scene
{
	uint32_t eye_width;
	uint32_t eye_height;
	uint32_t bg_disparity;  //!< background plane, pixels
	uint32_t bar_disparity; //!< near bar, pixels
	//! The bar's rectangle in LEFT-eye coordinates.
	uint32_t bar_x0, bar_y0, bar_x1, bar_y1;
};

/*!
 * Scene for a pinhole pair of focal @p fx_px and baseline @p baseline_mm, with
 * the background at @p bg_depth_m and the bar at @p bar_depth_m: disparities
 * are round(fx * B / Z).
 */
void
sim_stereo_camera_scene_init(struct sim_stereo_camera_scene *s,
                             uint32_t eye_width,
                             uint32_t eye_height,
                             double fx_px,
                             double baseline_mm,
                             double bg_depth_m,
                             double bar_depth_m);

/*!
 * Render frame @p frame_number into a GRAY8 SBS image (2*eye_width x
 * eye_height, @p pitch bytes per row, left eye left).
 */
void
sim_stereo_camera_render_gray(const struct sim_stereo_camera_scene *s,
                              uint64_t frame_number,
                              uint8_t *dst,
                              uint32_t pitch);

#ifdef __cplusplus
}
#endif
