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
 * The normalised sibling of @ref u_bg_roi: a rect in [0,1] with the origin
 * top-left (u right, v DOWN).
 *
 * Exists because a region is derived in a resolution-independent space and only
 * then mapped into preview pixels — the app's content bounds and the frame's 3D
 * display-zone rects are both normalised to the app window's client rect, and
 * they have to be intersected with each other BEFORE either meets the preview's
 * pixel grid.
 *
 * @ingroup aux_util
 */
struct u_bg_rect_norm
{
	float u0, v0, u1, v1;
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
	 * Neutral iff the busiest COLUMN's edge density (edge rows / rows
	 * SAMPLED in that column — the ROI's rows, or the masked ones under a
	 * mask) is below this. Default 0.20 — an area metric alone cannot see a
	 * single vertical window border spanning the whole ROI, which is exactly
	 * the cue that matters most.
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
 * Fewest masked difference samples @ref u_bg_neutrality_analyse_masked will
 * draw a conclusion from.
 *
 * A silhouette mask can narrow the measured set to a handful of pixels — a
 * sliver of a character's arm against the preview grid — and a fraction over
 * a handful of samples is noise, not a verdict. Below this the answer is
 * "could not measure", which the policy above treats as no source at all;
 * reporting it as neutral would open the budget on the strength of nothing.
 *
 * Applies ONLY when a mask is supplied: unmasked callers keep exactly the v1/v2
 * rule (>= 2 columns, >= 1 row).
 *
 * @ingroup aux_util
 */
#define U_BG_NEUTRALITY_MIN_MASKED_SAMPLES 64u

/*!
 * Fewest masked pairs a COLUMN must hold before its edge density is allowed to
 * set @ref u_bg_neutrality_result::max_column_density.
 *
 * The column metric exists to catch one vertical window border running the
 * height of the region — a shape that needs a column to be tall to mean
 * anything. Under a mask most columns are short (the top of a head, the gap
 * between two legs), and one edge over two masked pairs is a density of 0.5:
 * three times the default limit, read off two samples.
 *
 * @ingroup aux_util
 */
#define U_BG_NEUTRALITY_MIN_MASKED_COLUMN_PAIRS 4u

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

/*!
 * @ref u_bg_neutrality_analyse, restricted to the pixels @p mask selects
 * (XR_DXR_depth_budget v3).
 *
 * A rectangle around a character is roughly two thirds background the model
 * never covers, and any horizontal structure in that surplus closes the budget.
 * The app already knows its own silhouette, so v3 measures only under it.
 *
 * The mask is in the SAME pixel grid as @p bgra (one byte per pixel, nonzero =
 * measure here), so a caller can keep one preview-sized mask and vary the ROI
 * independently. Restricting the mask to a ROI is still worth doing — it is
 * what bounds the scan — but the mask, not the rect, decides what counts.
 *
 * Masked metric, differing from the unmasked one only in what a "sample" is:
 *
 * - a horizontal difference sample at (x, y) counts only when BOTH pixels of
 *   the pair, (x, y) and (x+1, y), are masked. A pair straddling the silhouette
 *   edge is the app's OWN border against the desktop, not a background cue, and
 *   counting it would make every silhouette look busy;
 * - `edge_fraction` = edges / masked samples (not / ROI area);
 * - a column's density is its edges over ITS masked pairs, and a column holding
 *   fewer than @ref U_BG_NEUTRALITY_MIN_MASKED_COLUMN_PAIRS masked pairs is
 *   skipped entirely — one edge over two pairs is a density of 0.5 and would
 *   pin `max_column_density` on nothing;
 * - fewer than @ref U_BG_NEUTRALITY_MIN_MASKED_SAMPLES masked samples in total
 *   returns false. Never neutral.
 *
 * `cue_energy` and `neutral` are formed from those two numbers exactly as in
 * the unmasked case.
 *
 * @param mask        One byte per pixel of the @p w x @p h buffer, nonzero =
 *                    analyse. NULL is identical to @ref u_bg_neutrality_analyse.
 * @param mask_stride Row pitch of @p mask in bytes; 0 means @p w. Must be >= w.
 *
 * @ingroup aux_util
 */
bool
u_bg_neutrality_analyse_masked(const uint8_t *bgra,
                               uint32_t w,
                               uint32_t h,
                               uint32_t stride,
                               const struct u_bg_roi *roi,
                               const uint8_t *mask,
                               uint32_t mask_stride,
                               const struct u_bg_neutrality_params *p,
                               struct u_bg_neutrality_result *out);

#ifdef __cplusplus
}
#endif
