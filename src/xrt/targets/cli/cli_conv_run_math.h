// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `conv run`'s input downscale: the new size, and the intrinsics that
 *         must follow it.
 *
 * `conv run` downscales the decoded photo to the capability's
 * `max_input_edge` (the runtime's rule) before the job. A focal length the
 * user GIVES (`--focal-px`) is in the input file's pixels, so it has to be
 * scaled by the same factor, per axis — handing a 1536 px image the focal
 * length of its 6016 px original tells the provider the lens is ~4x longer
 * than it is (a flatter, deeper scene). Pure arithmetic, unit-tested in
 * `tests/tests_cli_conv_run_math.cpp`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The downscaled size of a @p w x @p h image whose long side (of one of its
 * @p split_columns side-by-side views) must not exceed @p max_edge (0 = no
 * limit), each side rounded to the nearest pixel. The width stays a
 * multiple of @p split_columns. Returns false (and
 * @p out_w / @p out_h = the input) when no downscale is needed.
 */
static inline bool
cli_conv_downscale_dims(int w, int h, uint32_t max_edge, int split_columns, int *out_w, int *out_h)
{
	if (split_columns < 1) {
		split_columns = 1;
	}
	*out_w = w;
	*out_h = h;
	const int view_w = w / split_columns;
	const int long_side = view_w > h ? view_w : h;
	if (max_edge == 0 || long_side <= (int)max_edge) {
		return false;
	}
	// Rounded, not truncated: 6016 * (1536 / 6016) is 1535.9999 in doubles,
	// and truncating it cost the long side a pixel.
	const double s = (double)max_edge / (double)long_side;
	int nw = (int)((double)w * s + 0.5) / split_columns * split_columns;
	int nh = (int)((double)h * s + 0.5);
	*out_w = nw > split_columns ? nw : split_columns;
	*out_h = nh > 1 ? nh : 1;
	return true;
}

/*!
 * A pixel-unit intrinsic (focal length, principal point) of an axis that was
 * resampled from @p old_dim to @p new_dim pixels. A value <= 0 means unknown
 * and stays as it is (never invented).
 */
static inline float
cli_conv_scale_intrinsic(float v, int old_dim, int new_dim)
{
	if (!(v > 0.0f) || old_dim <= 0 || new_dim <= 0 || old_dim == new_dim) {
		return v;
	}
	return (float)((double)v * (double)new_dim / (double)old_dim);
}

#ifdef __cplusplus
}
#endif
