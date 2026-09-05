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

	// One difference sample per adjacent column pair, per row.
	const uint32_t sample_cols = r.w - 1u;
	const uint64_t total_samples = (uint64_t)sample_cols * (uint64_t)r.h;

	uint64_t edge_samples = 0;
	uint32_t max_col_edges = 0;

	// Column-major accumulation would thrash the cache; walk rows and keep a
	// per-column running count in the caller-free way: a single pass with a
	// small stack-free accumulator is impossible without storage, so scan
	// column-by-column over rows instead. The preview is <= 512 px wide, so
	// either order is trivial work; this order needs no allocation.
	for (uint32_t cx = 0; cx < sample_cols; cx++) {
		const uint32_t x = r.x + cx;
		uint32_t col_edges = 0;
		for (uint32_t dy = 0; dy < r.h; dy++) {
			const uint8_t *row = bgra + (size_t)(r.y + dy) * (size_t)stride;
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
		if (col_edges > max_col_edges) {
			max_col_edges = col_edges;
		}
	}

	out->edge_fraction = (total_samples > 0) ? (float)((double)edge_samples / (double)total_samples) : 0.0f;
	out->max_column_density = (float)((double)max_col_edges / (double)r.h);

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
