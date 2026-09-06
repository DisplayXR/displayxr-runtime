// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Background neutrality analysis for the rear depth budget.
 * @author David Fattal
 * @ingroup aux_util
 */

#include "util/u_bg_neutrality.h"

#include <string.h>

//! Rec.601 luma, matching the perceptual weighting the eye applies to an edge.
static inline float
bg_luma(const uint8_t *px)
{
	// BGRA8: px[0]=B, px[1]=G, px[2]=R, px[3]=A (ignored).
	const float b = (float)px[0] * (1.0f / 255.0f);
	const float g = (float)px[1] * (1.0f / 255.0f);
	const float r = (float)px[2] * (1.0f / 255.0f);
	return 0.299f * r + 0.587f * g + 0.114f * b;
}

void
u_bg_neutrality_params_default(struct u_bg_neutrality_params *p)
{
	if (p == NULL) {
		return;
	}
	p->edge_threshold = 0.06f;
	p->max_edge_fraction = 0.003f;
	p->max_column_density = 0.20f;
}

bool
u_bg_neutrality_analyse(const uint8_t *bgra,
                        uint32_t w,
                        uint32_t h,
                        uint32_t stride,
                        const struct u_bg_roi *roi,
                        const struct u_bg_neutrality_params *p,
                        struct u_bg_neutrality_result *out)
{
	return u_bg_neutrality_analyse_masked(bgra, w, h, stride, roi, NULL, 0, p, out);
}

bool
u_bg_neutrality_analyse_masked(const uint8_t *bgra,
                               uint32_t w,
                               uint32_t h,
                               uint32_t stride,
                               const struct u_bg_roi *roi,
                               const uint8_t *mask,
                               uint32_t mask_stride,
                               const struct u_bg_neutrality_params *p,
                               struct u_bg_neutrality_result *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (bgra == NULL || w == 0 || h == 0 || stride < (uint64_t)w * 4u) {
		return false;
	}

	struct u_bg_roi r = {0, 0, w, h};
	if (roi != NULL) {
		r = *roi;
	}

	// A ROI that leaves the buffer is a caller bug, not a neutral background.
	if (r.w == 0 || r.h == 0 || r.x > w || r.y > h || (uint64_t)r.x + r.w > w || (uint64_t)r.y + r.h > h) {
		return false;
	}
	// One column holds no horizontal difference — there is nothing to measure,
	// and "nothing measured" must never read as "no cue".
	if (r.w < 2) {
		return false;
	}

	// The mask lives in the SAME grid as the pixels, so it is indexed by
	// absolute coordinates and a too-narrow pitch is a caller bug, never a
	// reason to measure the wrong bytes.
	if (mask != NULL) {
		if (mask_stride == 0) {
			mask_stride = w;
		}
		if (mask_stride < w) {
			return false;
		}
	}

	struct u_bg_neutrality_params params;
	if (p != NULL) {
		params = *p;
	} else {
		u_bg_neutrality_params_default(&params);
	}
	if (!(params.edge_threshold > 0.0f)) {
		params.edge_threshold = 0.06f;
	}
	if (!(params.max_edge_fraction > 0.0f)) {
		params.max_edge_fraction = 0.003f;
	}
	if (!(params.max_column_density > 0.0f)) {
		params.max_column_density = 0.20f;
	}

	// One difference sample per adjacent column pair, per row — but under a
	// mask only where BOTH pixels of the pair are selected, so the count is
	// accumulated rather than assumed.
	const uint32_t sample_cols = r.w - 1u;
	uint64_t total_samples = 0;

	uint64_t edge_samples = 0;
	float max_col_density = 0.0f;

	// Column-major accumulation would thrash the cache; walk rows and keep a
	// per-column running count in the caller-free way: a single pass with a
	// small stack-free accumulator is impossible without storage, so scan
	// column-by-column over rows instead. The preview is <= 512 px wide, so
	// either order is trivial work; this order needs no allocation.
	for (uint32_t cx = 0; cx < sample_cols; cx++) {
		const uint32_t x = r.x + cx;
		uint32_t col_edges = 0;
		uint32_t col_pairs = 0;
		for (uint32_t dy = 0; dy < r.h; dy++) {
			const uint32_t y = r.y + dy;
			if (mask != NULL) {
				const uint8_t *mrow = mask + (size_t)y * (size_t)mask_stride;
				// A pair straddling the silhouette edge is the app's
				// own border against the desktop, not a background
				// cue. Requiring BOTH sides is what keeps every
				// silhouette from measuring as busy by construction.
				if (mrow[x] == 0 || mrow[x + 1u] == 0) {
					continue;
				}
			}
			col_pairs++;
			const uint8_t *row = bgra + (size_t)y * (size_t)stride;
			const float y0 = bg_luma(row + (size_t)x * 4u);
			const float y1 = bg_luma(row + (size_t)(x + 1u) * 4u);
			float d = y1 - y0;
			if (d < 0.0f) {
				d = -d;
			}
			if (d > params.edge_threshold) {
				col_edges++;
			}
		}
		edge_samples += col_edges;
		total_samples += col_pairs;

		// A column too short to describe a vertical border must not be
		// allowed to set the column metric: one edge over two masked pairs
		// is a density of 0.5 read off two samples. Unmasked columns span
		// the whole ROI by construction, so this only ever bites the mask.
		if (mask != NULL && col_pairs < U_BG_NEUTRALITY_MIN_MASKED_COLUMN_PAIRS) {
			continue;
		}
		if (col_pairs > 0) {
			const float density = (float)((double)col_edges / (double)col_pairs);
			if (density > max_col_density) {
				max_col_density = density;
			}
		}
	}

	// Too little of the region survived the mask to draw a conclusion from.
	// "Could not measure" is not "measured and found quiet" — the policy above
	// reads false as no source, which clips, and clipping is today's behaviour.
	if (mask != NULL && total_samples < U_BG_NEUTRALITY_MIN_MASKED_SAMPLES) {
		memset(out, 0, sizeof(*out));
		return false;
	}

	out->edge_fraction = (total_samples > 0) ? (float)((double)edge_samples / (double)total_samples) : 0.0f;
	out->max_column_density = max_col_density;

	const float e_ratio = out->edge_fraction / params.max_edge_fraction;
	const float c_ratio = out->max_column_density / params.max_column_density;
	float energy = (e_ratio > c_ratio) ? e_ratio : c_ratio;
	if (energy < 0.0f) {
		energy = 0.0f;
	}
	if (energy > 1.0f) {
		energy = 1.0f;
	}
	out->cue_energy = energy;

	out->neutral =
	    (out->edge_fraction < params.max_edge_fraction) && (out->max_column_density < params.max_column_density);
	return true;
}
