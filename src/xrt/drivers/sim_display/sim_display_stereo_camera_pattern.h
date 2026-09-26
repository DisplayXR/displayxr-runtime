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
 * top-left of BOTH halves at zero disparity. The default pair is parallel and
 * undistorted (the fake then reports NATIVELY_RECTIFIED).
 *
 * The DISTORTED variant (R2, SIM_DISPLAY_FAKE_STEREO_CAMERA_DISTORT=1) renders
 * the same scene through two raw cameras with known ground truth: per-eye
 * intrinsics that differ, RADTAN5 barrel distortion, and a small rotation of
 * each camera (pitch -> vertical misalignment, roll, yaw) — the kind of pair a
 * real tracker camera delivers. The rotations are split symmetrically
 * (left = S^T, right = S), which makes Bouguet rectification recover EXACTLY
 * the virtual parallel pair the scene is defined in, so after rectification
 * rows must align and the disparity must be f_rect * B / Z.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stdbool.h>
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
	//! Unrounded disparities (fx * B / Z), used by the distorted variant.
	double bg_disparity_f;
	double bar_disparity_f;
};

/*!
 * Ground truth of the DISTORTED fake: the virtual parallel camera the scene is
 * defined in, and the two raw cameras that look at it.
 */
struct sim_stereo_camera_truth
{
	uint32_t eye_width, eye_height;
	//! The virtual rectified camera (both eyes): the scene's pixel frame.
	double ideal_fx, ideal_cx, ideal_cy;
	double baseline_mm;
	double bg_depth_m, bar_depth_m;
	//! Raw per-eye intrinsics (pixels) + RADTAN5 (k1 k2 p1 p2 k3).
	double fx[2], fy[2], cx[2], cy[2];
	double dist[2][5];
	//! Half rotation: x_raw_left = S^T x_rect, x_raw_right = S (x_rect - C_right).
	double S[3][3];
	//! OpenCV extrinsics of the raw pair: x_R = R x_L + T, T in mm.
	double R[3][3];
	double T_mm[3];
};

//! Fill the truth for a pair of @p eye_width x @p eye_height whose virtual
//! parallel camera has focal @p ideal_fx and baseline @p baseline_mm.
void
sim_stereo_camera_truth_init(struct sim_stereo_camera_truth *t,
                             uint32_t eye_width,
                             uint32_t eye_height,
                             double ideal_fx,
                             double baseline_mm,
                             double bg_depth_m,
                             double bar_depth_m);

/*!
 * The distorted renderer: the truth + a per-raw-pixel lookup of where each
 * raw pixel looks in the virtual parallel frame (built once).
 */
struct sim_stereo_camera_distorted
{
	struct sim_stereo_camera_scene scene;
	struct sim_stereo_camera_truth truth;
	//! 2 floats per raw pixel, eye-major: virtual-frame (u, v); u < -1e8 = none.
	float *ideal_uv;
};

bool
sim_stereo_camera_distorted_init(struct sim_stereo_camera_distorted *d,
                                 uint32_t eye_width,
                                 uint32_t eye_height,
                                 double ideal_fx,
                                 double baseline_mm,
                                 double bg_depth_m,
                                 double bar_depth_m);

void
sim_stereo_camera_distorted_fini(struct sim_stereo_camera_distorted *d);

//! Render frame @p frame_number of the distorted pair (GRAY8 SBS).
void
sim_stereo_camera_render_gray_distorted(const struct sim_stereo_camera_distorted *d,
                                        uint64_t frame_number,
                                        uint8_t *dst,
                                        uint32_t pitch);

/*!
 * The continuous scene as the VIRTUAL parallel camera sees it: eye @p right at
 * virtual pixel (@p u, @p v), bilinear value noise (so resampling is honest).
 */
float
sim_stereo_camera_scene_sample(const struct sim_stereo_camera_scene *s, int right, double u, double v);

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
