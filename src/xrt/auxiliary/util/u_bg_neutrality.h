// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Background neutrality analysis for the rear depth budget.
 *
 * A pure function over a BGRA8 buffer: does this patch of desktop carry a
 * HORIZONTAL-disparity cue?
 *
 * A transparent-background app composites over the live desktop. Content it
 * draws BEHIND the zero-disparity plane has positive disparity yet occludes
 * desktop pixels that sit at zero disparity, and the eye reads that conflict
 * only where the background has horizontal luminance structure: vertical
 * edges, text, icons, window borders. A background that is uniform — or merely
 * horizontally uniform, e.g. a vertical gradient or horizontal stripes —
 * carries no such cue, and rear content over it is perceptually fine.
 *
 * So the metric deliberately looks at HORIZONTAL differences only. Vertical
 * differences are ignored by construction, not by oversight.
 *
 * No GPU, no OS calls, no logging, no allocation — the policy on top
 * (@ref u_rear_budget) owns time and hysteresis; this owns one frame's number.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Region of interest inside the preview, in preview pixels.
 *
 * v1 callers pass the whole preview (the desktop under the app canvas); v2
 * will narrow it to the app's reported content bounds.
 *
 * @ingroup aux_util
 */
struct u_bg_roi
{
	uint32_t x, y, w, h;
};

/*!
 * Thresholds for @ref u_bg_neutrality_analyse.
 *
 * @ingroup aux_util
 */
struct u_bg_neutrality_params
{
	//! |ΔY| in [0,1] luma that counts as an edge. Default 0.06.
	float edge_threshold;
	//! Neutral iff edge_samples / total_samples < this. Default 0.003.
	float max_edge_fraction;
	/*!
	 * Neutral iff the busiest COLUMN's edge density (edge rows / roi rows)
	 * is below this. Default 0.20 — an area metric alone cannot see a single
	 * vertical window border spanning the whole ROI, which is exactly the
	 * cue that matters most.
	 */
	float max_column_density;
};

/*!
 * Result of one analysis pass.
 *
 * @ingroup aux_util
 */
struct u_bg_neutrality_result
{
	//! Fraction of horizontal difference samples that crossed the threshold.
	float edge_fraction;
	//! Busiest column's edge density, in [0,1].
	float max_column_density;
	/*!
	 * Combined 0..1 scalar: the worse of the two metrics as a fraction of
	 * its own limit, clamped. < 1 ⟺ @ref neutral.
	 */
	float cue_energy;
	//! True when the patch carries no meaningful horizontal cue.
	bool neutral;
};

/*!
 * Fill @p p with the default thresholds.
 *
 * @ingroup aux_util
 */
void
u_bg_neutrality_params_default(struct u_bg_neutrality_params *p);

/*!
 * Analyse @p roi of a BGRA8, top-down buffer.
 *
 * Luma is Y = 0.299R + 0.587G + 0.114B in [0,1]; the difference sampled is
 * d(x,y) = |Y(x+1,y) - Y(x,y)| for every x in the ROI that has a right-hand
 * neighbour INSIDE the ROI. Alpha is ignored.
 *
 * @param bgra    Pixel data, BGRA8, top-down. Must not be NULL.
 * @param w       Buffer width in pixels.
 * @param h       Buffer height in pixels.
 * @param stride  Row pitch in bytes; must be >= w * 4.
 * @param roi     Region to analyse; NULL means the whole buffer.
 * @param p       Thresholds; NULL means @ref u_bg_neutrality_params_default.
 * @param out     Filled on success. Must not be NULL.
 *
 * @return false — and @p out zeroed — when the inputs are unusable: NULL
 *         buffer, stride below the row size, a ROI that leaves the buffer, or
 *         a ROI too small to hold a single horizontal difference sample (fewer
 *         than 2 columns or 1 row). "No answer" is never reported as neutral.
 *
 * @ingroup aux_util
 */
bool
u_bg_neutrality_analyse(const uint8_t *bgra,
                        uint32_t w,
                        uint32_t h,
                        uint32_t stride,
                        const struct u_bg_roi *roi,
                        const struct u_bg_neutrality_params *p,
                        struct u_bg_neutrality_result *out);

#ifdef __cplusplus
}
#endif
