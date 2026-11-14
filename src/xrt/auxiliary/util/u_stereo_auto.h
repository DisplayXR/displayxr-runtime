// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Automatic 2D and automatic re-convergence for a LEGACY stereo
 *         session, measured on the submitted eye pair: the pure half.
 * @ingroup aux_util
 *
 * A legacy OpenXR app (no XR_DXR_display_info) cannot switch the panel to 2D
 * for its menus or loading screens, and cannot be asked to move its screen
 * plane: an app such as a third-party VR injector driving an existing game may
 * cache the field of view it read once at startup, so a changed off-axis
 * frustum from the runtime's camera rig is ignored. Both decisions can still be
 * taken from the PICTURES it submits, which needs no cooperation from the app:
 *
 *  - **Automatic 2D.** Both eyes show one flat picture (identical, or identical
 *    up to a single horizontal shift — a menu drawn at a fixed depth): ask for
 *    hardware 2D and show one eye. 3D again as soon as the eyes differ.
 *  - **Automatic re-convergence.** Horizontal image translation of the two
 *    views before the display processor (left eye by −Δ/2, right eye by +Δ/2,
 *    black at the uncovered edge): every disparity d becomes d + Δ. Δ puts the
 *    centre-weighted screen-plane estimate of the scene (the same rule as the
 *    lift auto-convergence, util/u_lift_autoconv.h) at zero disparity, kept
 *    inside the comfort bounds and the shift cap, smoothed over time and
 *    snapped on scene cuts.
 *
 * Everything here is CPU, graphics-API free and host-tested
 * (tests_aux_stereo_auto). A compositor reduces both eyes of a frame to a small
 * luma grid a few times per second (a GPU downsample + an asynchronous
 * readback, never a stall), feeds @ref u_stereo_auto_probe off its frame path,
 * and applies the outputs: the 2D wish through its normal display-mode path
 * (with util/u_display_mode_hold.h so the session's own choice is restored),
 * the shift in its crop stage before the display processor.
 *
 * Units: a disparity is x_right − x_left as a fraction of the EYE image width,
 * positive = behind the screen. A shift is the change it adds to every
 * disparity.
 */

#pragma once

#include "util/u_camera_profile.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_STEREO_AUTO_GRID_X 256u //!< cells per eye, horizontally (fine: shifts are searched)
#define U_STEREO_AUTO_GRID_Y 24u  //!< cells per eye, vertically
#define U_STEREO_AUTO_CELLS (U_STEREO_AUTO_GRID_X * U_STEREO_AUTO_GRID_Y)

#define U_STEREO_AUTO_BLOCKS_X 32u //!< disparity blocks per eye, horizontally
#define U_STEREO_AUTO_BLOCKS_Y 6u  //!< disparity blocks per eye, vertically
#define U_STEREO_AUTO_BLOCKS (U_STEREO_AUTO_BLOCKS_X * U_STEREO_AUTO_BLOCKS_Y)

//! Default probe period: ten probes a second.
#define U_STEREO_AUTO_PROBE_PERIOD_NS (100ull * 1000ull * 1000ull)

//! Luma on a 0..255 scale from display-encoded channels in 0..1.
static inline float
u_stereo_auto_luma(float r, float g, float b)
{
	return (r * 2.0f + g * 5.0f + b) * (255.0f / 8.0f);
}

/*!
 * One eye of a luma plane (0..255) reduced to the grid by area averaging.
 * @p eye_x0 / @p eye_w select the eye's columns; @p stride is in floats.
 * Works for any eye size, also one smaller than the grid. @p out holds
 * U_STEREO_AUTO_CELLS floats, row major.
 */
void
u_stereo_auto_grid_from_luma(
    const float *luma, uint32_t stride, uint32_t height, uint32_t eye_x0, uint32_t eye_w, float *out);

/*
 * The flat-picture detector.
 */

struct u_stereo_flat_params
{
	double unshifted_max;  //!< eyes this close unshifted are flat outright (luma 0..255)
	double residual_max;   //!< the best shift must leave less than this ...
	double residual_ratio; //!< ... and less than this fraction of the unshifted difference
	double max_shift_frac; //!< widest shift searched, fraction of the eye width
	uint32_t enter_probes; //!< consecutive flat probes before 2D
	uint32_t leave_probes; //!< consecutive depth probes before 3D again
};

void
u_stereo_flat_params_default(struct u_stereo_flat_params *p);

struct u_stereo_flat_measurement
{
	bool flat;         //!< the verdict of this one probe
	double unshifted;  //!< mean absolute luma difference at shift 0
	double best;       //!< smallest difference found
	double best_shift; //!< where, in cells (the right eye sampled at x − shift)
};

//! Mean absolute luma difference of @p left and @p right sampled at x − @p shift cells.
double
u_stereo_flat_eye_difference(const float *left, const float *right, double shift);

void
u_stereo_flat_measure(const float *left,
                      const float *right,
                      const struct u_stereo_flat_params *p,
                      struct u_stereo_flat_measurement *out);

/*
 * The disparity estimate.
 */

struct u_stereo_disparity_estimate
{
	bool valid;
	float screen;                        //!< the disparity the screen plane belongs at (centre-weighted rule)
	float near_d;                        //!< p5 of the confident blocks (the nearest content)
	float far_d;                         //!< p95 of the confident blocks (the farthest content)
	uint32_t blocks;                     //!< confident blocks used
	float block_d[U_STEREO_AUTO_BLOCKS]; //!< per block, NaN = not confident
};

/*!
 * Block-match the two grids (U_STEREO_AUTO_BLOCKS_X x _Y blocks, sub-cell
 * refined) and place the screen plane: the centre-weighted blend of the p80
 * of nearness and the dominant clump with the far background excluded
 * (u_lift_autoconv_find_subject on the blocks' normalised nearness), or a
 * centre-weighted p78 of nearness when that finds no confident mode.
 * Untextured and ambiguous blocks are not used.
 */
bool
u_stereo_auto_estimate(const float *left,
                       const float *right,
                       double max_shift_frac,
                       struct u_stereo_disparity_estimate *out);

/*!
 * The shift that puts @p e's screen plane at zero, then keeps the far content
 * within cfg->max_behind_fraction and the near content within
 * cfg->max_front_fraction (when both cannot hold, the middle of the two), and
 * finally within ±cfg->max_shift_fraction.
 */
float
u_stereo_auto_target_shift(const struct u_stereo_disparity_estimate *e, const struct u_legacy_stereo_auto *cfg);

/*
 * The controller: detector + hysteresis + temporal policy.
 */

struct u_stereo_auto_params
{
	struct u_stereo_flat_params flat;
	float tau_s;    //!< follower time constant inside a shot
	float deadband; //!< target changes smaller than this are not followed (fraction)
	float cut_luma; //!< mean absolute change of the left grid between probes that is a cut
};

void
u_stereo_auto_params_default(struct u_stereo_auto_params *p);

//! Zero-initialise, then @ref u_stereo_auto_init.
struct u_stereo_auto
{
	struct u_legacy_stereo_auto cfg;
	struct u_stereo_auto_params p;
	uint32_t flat_streak;
	uint32_t depth_streak;
	bool want_2d; //!< the hysteresis verdict
	bool have_shift;
	bool snap_pending; //!< a cut (or a return from 2D) waits for the next estimate
	float shift;       //!< smoothed shift, fraction of the eye width
	bool have_prev;
	float prev_left[U_STEREO_AUTO_CELLS];
	uint64_t last_ns;
	uint64_t probes;
	uint32_t cuts;
};

void
u_stereo_auto_init(struct u_stereo_auto *a, const struct u_legacy_stereo_auto *cfg);

struct u_stereo_auto_result
{
	bool want_2d; //!< what the display should be now (only ever true with cfg.auto_2d)
	struct u_stereo_flat_measurement flat;
	bool estimated; //!< a disparity estimate was attempted (convergence on, eyes not flat)
	struct u_stereo_disparity_estimate est;
	float target_shift; //!< the shift this probe asks for (valid with est.valid)
	float shift;        //!< the smoothed shift to apply (0 with convergence off)
	bool cut;           //!< this probe saw a scene cut (the shift snapped)
};

/*!
 * One probe of the two eye grids, taken at @p now_ns. Applies the flat
 * hysteresis (2D after enter_probes flat probes, 3D on the leave_probes-th
 * probe with depth) and the shift policy: the first estimate and a scene cut
 * snap, otherwise an exponential follower outside the dead band; a flat probe
 * or one without a confident estimate holds. Leaving 2D re-snaps.
 */
void
u_stereo_auto_probe(
    struct u_stereo_auto *a, const float *left, const float *right, uint64_t now_ns, struct u_stereo_auto_result *out);

#ifdef __cplusplus
}
#endif
