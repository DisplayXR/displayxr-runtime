// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU synthetic outputs (see sim_conv_synth.h).
 * @ingroup drv_sim_conversion
 */

#include "sim_conv_synth.h"

#include <math.h>
#include <string.h>

bool
sim_conv_synth_view_shifts(uint32_t view_count,
                           float strength,
                           const float *viewpoints_xyz,
                           uint32_t viewpoint_count,
                           float rect_width_m,
                           float out_shift[SIM_CONV_SYNTH_MAX_VIEWS])
{
	const float s = strength > 0.0f ? strength : 1.0f;
	uint32_t n = view_count > SIM_CONV_SYNTH_MAX_VIEWS ? SIM_CONV_SYNTH_MAX_VIEWS : view_count;
	for (uint32_t i = 0; i < SIM_CONV_SYNTH_MAX_VIEWS; i++) {
		out_shift[i] = 0.0f;
	}
	if (n == 0) {
		return false;
	}
	const bool explicit_vp =
	    viewpoints_xyz != NULL && rect_width_m > 0.0f && (viewpoint_count >= n || (viewpoint_count >= 2 && n > 1));
	for (uint32_t i = 0; i < n; i++) {
		if (explicit_vp) {
			float x;
			if (viewpoint_count >= n) {
				x = viewpoints_xyz[i * 3 + 0];
			} else {
				// A pair (or fewer than N): spread N views linearly across it.
				const float x0 = viewpoints_xyz[0];
				const float x1 = viewpoints_xyz[(viewpoint_count - 1) * 3 + 0];
				x = x0 + (x1 - x0) * (float)i / (float)(n - 1);
			}
			out_shift[i] = SIM_CONV_SYNTH_PARALLAX_GAIN * s * x / rect_width_m;
		} else {
			out_shift[i] = ((float)i - (float)(n - 1) * 0.5f) * SIM_CONV_SYNTH_DEFAULT_STEP * s;
		}
	}
	return explicit_vp;
}

void
sim_conv_synth_view_rgba(const uint8_t *src,
                         uint32_t src_stride,
                         uint32_t width,
                         uint32_t height,
                         const float *depth,
                         uint32_t depth_stride,
                         float shift,
                         bool swap_rb,
                         uint8_t *dst,
                         uint32_t dst_stride)
{
	if (src == NULL || dst == NULL || width == 0 || height == 0) {
		return;
	}
	const float shift_px = shift * (float)width;
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *srow = src + (size_t)y * src_stride;
		const float *drow =
		    depth != NULL ? (const float *)((const uint8_t *)depth + (size_t)y * depth_stride) : NULL;
		uint8_t *orow = dst + (size_t)y * dst_stride;
		for (uint32_t x = 0; x < width; x++) {
			float f = 1.0f;
			if (drow != NULL) {
				float d = drow[x];
				d = d != d ? 1.0f : d; // NaN = no data: no parallax
				d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
				f = 1.0f - d;
			}
			long sx = (long)x + (long)floorf(shift_px * f + 0.5f);
			sx = sx < 0 ? 0 : (sx >= (long)width ? (long)width - 1 : sx);
			const uint8_t *sp = srow + (size_t)sx * 4u;
			uint8_t *op = orow + (size_t)x * 4u;
			if (swap_rb) {
				op[0] = sp[2];
				op[1] = sp[1];
				op[2] = sp[0];
				op[3] = sp[3];
			} else {
				memcpy(op, sp, 4);
			}
		}
	}
}

void
sim_conv_synth_depth_ramp(float *dst, uint32_t dst_stride, uint32_t width, uint32_t height, float scale, float offset)
{
	if (dst == NULL || height == 0) {
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		float *row = (float *)((uint8_t *)dst + (size_t)y * dst_stride);
		const float v = offset + scale * (((float)y + 0.5f) / (float)height);
		for (uint32_t x = 0; x < width; x++) {
			row[x] = v;
		}
	}
}

void
sim_conv_synth_fill_r32f(float *dst, uint32_t dst_stride, uint32_t width, uint32_t height, float value)
{
	sim_conv_synth_depth_ramp(dst, dst_stride, width, height, 0.0f, value);
}
