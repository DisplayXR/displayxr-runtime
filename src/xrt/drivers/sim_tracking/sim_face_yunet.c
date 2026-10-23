// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): YuNet pre- and
 *         post-processing (see sim_face_yunet.h).
 * @ingroup drv_sim_tracking
 */

#include "sim_face_yunet.h"

#include "util/u_stereo_uvc.h"

#include <math.h>
#include <string.h>

static const uint32_t k_strides[SIM_YUNET_STRIDES] = {8, 16, 32};

bool
sim_yunet_make_plan(uint32_t src_w, uint32_t src_h, uint32_t max_net_w, struct sim_yunet_plan *out)
{
	if (out == NULL || src_w == 0 || src_h == 0 || max_net_w == 0 || src_w > 16384 || src_h > 16384) {
		return false;
	}
	const uint32_t net_w = src_w < max_net_w ? src_w : max_net_w;
	const float src_per_net = (float)src_w / (float)net_w;
	uint32_t net_h = (uint32_t)((float)src_h / src_per_net + 0.5f);
	if (net_h == 0) {
		net_h = 1;
	}
	out->src_w = src_w;
	out->src_h = src_h;
	out->net_w = net_w;
	out->net_h = net_h;
	out->pad_w = (net_w + 31u) / 32u * 32u;
	out->pad_h = (net_h + 31u) / 32u * 32u;
	out->src_per_net = src_per_net;
	return true;
}

size_t
sim_yunet_tensor_floats(const struct sim_yunet_plan *plan)
{
	return (size_t)3 * plan->pad_w * plan->pad_h;
}

static inline float
luma_at(const struct u_stereo_uvc_raw_frame *f, uint32_t x, uint32_t y)
{
	const uint8_t *row = f->planes[0] + (size_t)y * f->pitches[0];
	// YUY2 packs Y0 U Y1 V: luma is every other byte.
	return (float)(f->pixel == U_STEREO_UVC_PIXEL_YUY2 ? row[(size_t)x * 2u] : row[x]);
}

bool
sim_yunet_fill_tensor(const struct u_stereo_uvc_raw_frame *frame, const struct sim_yunet_plan *plan, float *tensor)
{
	if (frame == NULL || plan == NULL || tensor == NULL || frame->planes[0] == NULL ||
	    (frame->pixel != U_STEREO_UVC_PIXEL_NV12 && frame->pixel != U_STEREO_UVC_PIXEL_YUY2) ||
	    frame->width != plan->src_w || frame->height != plan->src_h) {
		return false;
	}
	const size_t plane = (size_t)plan->pad_w * plan->pad_h;
	memset(tensor, 0, plane * sizeof(float));
	const float max_x = (float)(plan->src_w - 1);
	const float max_y = (float)(plan->src_h - 1);
	for (uint32_t y = 0; y < plan->net_h; y++) {
		// Pixel-centre mapping, clamped to the frame.
		float sy = ((float)y + 0.5f) * plan->src_per_net - 0.5f;
		sy = sy < 0.0f ? 0.0f : (sy > max_y ? max_y : sy);
		const uint32_t y0 = (uint32_t)sy;
		const uint32_t y1 = y0 + 1 < plan->src_h ? y0 + 1 : y0;
		const float fy = sy - (float)y0;
		float *dst = tensor + (size_t)y * plan->pad_w;
		for (uint32_t x = 0; x < plan->net_w; x++) {
			float sx = ((float)x + 0.5f) * plan->src_per_net - 0.5f;
			sx = sx < 0.0f ? 0.0f : (sx > max_x ? max_x : sx);
			const uint32_t x0 = (uint32_t)sx;
			const uint32_t x1 = x0 + 1 < plan->src_w ? x0 + 1 : x0;
			const float fx = sx - (float)x0;
			const float top = luma_at(frame, x0, y0) * (1.0f - fx) + luma_at(frame, x1, y0) * fx;
			const float bot = luma_at(frame, x0, y1) * (1.0f - fx) + luma_at(frame, x1, y1) * fx;
			dst[x] = top * (1.0f - fy) + bot * fy;
		}
	}
	memcpy(tensor + plane, tensor, plane * sizeof(float));
	memcpy(tensor + 2 * plane, tensor, plane * sizeof(float));
	return true;
}

static inline float
clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

bool
sim_yunet_decode(const struct sim_yunet_plan *plan,
                 const struct sim_yunet_head heads[SIM_YUNET_STRIDES],
                 float score_min,
                 struct sim_face_landmarks *out)
{
	if (plan == NULL || heads == NULL || out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	float best = -1.0f;
	for (int h = 0; h < SIM_YUNET_STRIDES; h++) {
		const uint32_t s = k_strides[h];
		const uint32_t cols = plan->pad_w / s;
		const uint32_t rows = plan->pad_h / s;
		const struct sim_yunet_head *hd = &heads[h];
		if (hd->cls == NULL || hd->obj == NULL || hd->kps == NULL || hd->rows != (size_t)cols * rows) {
			memset(out, 0, sizeof(*out));
			return false;
		}
		for (size_t i = 0; i < hd->rows; i++) {
			const float score = sqrtf(clamp01(hd->cls[i]) * clamp01(hd->obj[i]));
			if (score < score_min || score <= best) {
				continue;
			}
			// One viewer: the single most confident face wins.
			const float c = (float)(i % cols);
			const float r = (float)(i / cols);
			const float *k = hd->kps + i * 10u;
			best = score;
			out->found = true;
			out->confidence = score;
			for (int e = 0; e < 2; e++) {
				out->eye_px[e][0] = (k[2 * e] + c) * (float)s * plan->src_per_net;
				out->eye_px[e][1] = (k[2 * e + 1] + r) * (float)s * plan->src_per_net;
			}
		}
	}
	return true;
}
