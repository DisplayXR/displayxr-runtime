// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: online VERTICAL-ALIGNMENT
 *         refinement of a service-rectified pair.
 *
 * Why: a device's stored calibration can be slightly off for the frames it
 * actually delivers. On a Leia SR laptop the rectifier (and OpenCV's own
 * stereoRectify, on the same frame and the same calibration) left a signed
 * vertical residual of +1.71 px, fitted as dy = +2.6 px at the centre with a
 * -0.47 px / 100 px slope in y (a small pitch / vertical-scale mismatch). The
 * x term was +0.04 px / 100 px (<= 0.15 px at the image edge, under the match
 * noise), so the model is
 *
 *     dy(y) = a + b * (y - cy)           (right row - left row, output pixels)
 *
 * with no roll term. It is measured on the RECTIFIED frames the service
 * delivers, so every measurement is a RESIDUAL of the correction already
 * applied, and the controller integrates it (a closed loop: new total =
 * applied + residual). The correction is folded into the rectification maps
 * as u_stereo_rectify_input::v_offset = a / f, v_slope = b (see there for the
 * math); frames and the rectified calibration therefore stay consistent.
 *
 * Two layers, both pure (no threads, no OS) so they are unit-tested:
 *
 *  1. **Measurement** — @ref u_stereo_vrefine_measure: Shi-Tomasi-selected
 *     patches of the left eye (one per grid cell, on a 2x-downscaled copy),
 *     zero-mean NCC search over disparity x vertical offset at half
 *     resolution, then a full-resolution NCC refinement +-2 px around it with
 *     parabolic sub-pixel peaks. A match is kept only with NCC >= min_ncc, a
 *     positive disparity, a peak strictly inside both windows, and a real
 *     vertical curvature (no aperture-problem edges).
 *  2. **Controller** — @ref u_stereo_vrefine: duty cycle, accumulation over
 *     frames, robust fit (median start, Tukey IRLS), clamps, the >= N matches
 *     over >= F frames gate and the deadband that decides when maps rebuild.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One matched feature (full-resolution output pixels).
struct u_stereo_vrefine_sample
{
	float x, y; //!< left-eye position (patch centre)
	float d;    //!< horizontal disparity, left x - right x
	float dy;   //!< right y - left y, SIGNED
	float ncc;
};

struct u_stereo_vrefine_measure_params
{
	uint32_t max_disparity; //!< full-res px; the search is -2..max
	uint32_t max_dy;        //!< full-res px; the search is +-max_dy
	float min_ncc;          //!< 0.90 is a good default
	uint32_t max_samples;   //!< patches tried per frame (grid cells)
};

//! Defaults for a pair of @p eye_width px per eye whose nearest subject gives
//! at most @p max_disparity px.
void
u_stereo_vrefine_measure_defaults(struct u_stereo_vrefine_measure_params *p, uint32_t max_disparity);

/*!
 * Match features between the halves of a rectified GRAY8 side-by-side image
 * (left eye x < eye_width). Allocates its own scratch (called off the frame
 * path). @return the number of samples written (<= @p cap).
 */
uint32_t
u_stereo_vrefine_measure(const uint8_t *gray,
                         uint32_t pitch,
                         uint32_t eye_width,
                         uint32_t height,
                         const struct u_stereo_vrefine_measure_params *p,
                         struct u_stereo_vrefine_sample *out,
                         uint32_t cap);

//! A robust fit of dy = a + b * (y - cy).
struct u_stereo_vrefine_fit
{
	double a, b;           //!< px, px per px
	double median_dy;      //!< signed median of the samples
	double sigma;          //!< robust residual scale, px
	uint32_t inliers;      //!< samples with a non-zero final weight
	bool slope_determined; //!< false = rows too clustered, b fixed at 0
};

/*!
 * Median start, then Tukey-biweight IRLS. @p cy is the pivot row. The slope is
 * only fitted when the samples span enough rows (std >= 40 px); else b = 0.
 * @return false with fewer than 8 samples.
 */
bool
u_stereo_vrefine_fit_samples(const struct u_stereo_vrefine_sample *s,
                             uint32_t n,
                             double cy,
                             struct u_stereo_vrefine_fit *out);


/*
 *
 * Controller.
 *
 */

#define U_STEREO_VREFINE_WINDOW_CAP 1024 //!< samples one window holds

struct u_stereo_vrefine_config
{
	uint32_t height;        //!< per-eye rows (the deadband's extent)
	double cy;              //!< the rectified principal point row
	uint32_t min_matches;   //!< per window, default 50
	uint32_t min_frames;    //!< per window, default 3
	uint32_t max_frames;    //!< a window that has not filled by then is dropped, default 12
	double deadband_px;     //!< rebuild when the correction moves by more, default 0.2
	double max_offset_px;   //!< |a| clamp, default 6
	double max_slope;       //!< |b| clamp, px per px, default 0.02 (2 px / 100 px)
	int64_t fast_period_ns; //!< sampling period while settling, default 250 ms
	int64_t fast_phase_ns;  //!< settling phase after a (re)start or an update, default 5 s
	int64_t slow_period_ns; //!< then one window every, default 30 s
};

void
u_stereo_vrefine_config_defaults(struct u_stereo_vrefine_config *c, uint32_t height, double cy);

enum u_stereo_vrefine_result
{
	U_STEREO_VREFINE_ACCUMULATING = 0, //!< window not full yet
	U_STEREO_VREFINE_STABLE = 1,       //!< window fitted, residual within the deadband
	U_STEREO_VREFINE_UPDATED = 2,      //!< window fitted, new correction: rebuild the maps
	U_STEREO_VREFINE_DROPPED = 3,      //!< window expired without enough matches
};

struct u_stereo_vrefine
{
	struct u_stereo_vrefine_config cfg;

	//! The correction (total, output px) the maps currently carry.
	double a, b;
	bool applied;     //!< a correction was ever applied since the last reset
	uint32_t updates; //!< UPDATED results since the last reset
	//! The previous correction, for u_stereo_vrefine_revert().
	double prev_a, prev_b;

	// The window.
	struct u_stereo_vrefine_sample win[U_STEREO_VREFINE_WINDOW_CAP];
	uint32_t win_n;
	uint32_t win_frames;

	// Schedule.
	int64_t phase_start_ns;
	int64_t next_due_ns;
	bool started;

	// Stats for logs / the probe.
	bool have_initial;
	double initial_dy; //!< signed median dy of the first fitted window (pre-correction)
	double initial_a, initial_b;
	double last_residual_dy; //!< signed median dy of the latest fitted window
	uint32_t last_matches;   //!< inliers of the latest fit
	uint32_t windows;        //!< fitted windows since the last reset
};

//! Zero correction, settling phase from the first due() call.
void
u_stereo_vrefine_init(struct u_stereo_vrefine *r, const struct u_stereo_vrefine_config *cfg);

//! Drop the correction and the window (calibration change): back to zero.
void
u_stereo_vrefine_reset(struct u_stereo_vrefine *r);

//! Keep the correction, drop the window, re-enter the settling phase (camera re-open).
void
u_stereo_vrefine_restart(struct u_stereo_vrefine *r, int64_t now_ns);

//! Should the frame at @p now_ns be measured?
bool
u_stereo_vrefine_due(struct u_stereo_vrefine *r, int64_t now_ns);

/*!
 * Feed one measured frame. On UPDATED, @ref a / @ref b hold the new total
 * (clamped) correction for the maps.
 */
enum u_stereo_vrefine_result
u_stereo_vrefine_push(struct u_stereo_vrefine *r, int64_t now_ns, const struct u_stereo_vrefine_sample *s, uint32_t n);

//! The maps could not be rebuilt with the last UPDATED correction: undo it.
void
u_stereo_vrefine_revert(struct u_stereo_vrefine *r);

#ifdef __cplusplus
}
#endif
