// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) building blocks — see u_stereo_camera.h.
 * @ingroup aux_util
 */

#include "util/u_stereo_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Ring.
 *
 */

void
u_stereo_camera_ring_init(struct u_stereo_camera_ring *r)
{
	memset(r, 0, sizeof(*r));
	r->latest = -1;
	r->pinned = -1;
	r->writing = -1;
	r->latest_acquired = true;
}

int32_t
u_stereo_camera_ring_begin_write(struct u_stereo_camera_ring *r)
{
	for (int32_t s = 0; s < U_STEREO_CAMERA_RING_SLOTS; s++) {
		if (s != r->latest && s != r->pinned) {
			r->writing = s;
			return s;
		}
	}
	// Unreachable with 3 slots: at most two are latest / pinned.
	r->writing = -1;
	return -1;
}

void
u_stereo_camera_ring_publish(struct u_stereo_camera_ring *r, int32_t slot, uint64_t seq)
{
	if (slot < 0 || slot >= U_STEREO_CAMERA_RING_SLOTS || slot == r->pinned) {
		r->writing = -1;
		return;
	}
	if (r->latest >= 0 && !r->latest_acquired) {
		r->skipped++;
	}
	r->latest = slot;
	r->latest_seq = seq;
	r->latest_acquired = false;
	r->writing = -1;
	r->published++;
}

void
u_stereo_camera_ring_abort_write(struct u_stereo_camera_ring *r)
{
	r->writing = -1;
}

bool
u_stereo_camera_ring_acquire(struct u_stereo_camera_ring *r, int32_t *out_slot, uint64_t *out_seq)
{
	if (r->latest < 0 || r->latest_acquired || r->latest_seq <= r->last_acquired_seq) {
		return false;
	}
	r->pinned = r->latest;
	r->last_acquired_seq = r->latest_seq;
	r->latest_acquired = true;
	r->acquired++;
	*out_slot = r->pinned;
	*out_seq = r->latest_seq;
	return true;
}

void
u_stereo_camera_ring_clear(struct u_stereo_camera_ring *r)
{
	r->pinned = -1;
	r->latest = -1;
	r->latest_acquired = true;
}


/*
 *
 * Decimator.
 *
 */

void
u_stereo_camera_decimator_init(struct u_stereo_camera_decimator *d, float max_rate, float source_rate)
{
	memset(d, 0, sizeof(*d));
	if (max_rate <= 0.0f || (source_rate > 0.0f && max_rate >= source_rate * 0.999f)) {
		d->period_ns = 0;
		return;
	}
	d->period_ns = (int64_t)(1e9 / (double)max_rate);
}

bool
u_stereo_camera_decimator_accept(struct u_stereo_camera_decimator *d, int64_t t_ns)
{
	if (d->period_ns <= 0) {
		return true;
	}
	if (!d->started) {
		d->started = true;
		d->next_due_ns = t_ns + d->period_ns;
		return true;
	}
	// A tenth of a period of slack absorbs source jitter without letting two
	// frames through for one due time.
	if (t_ns < d->next_due_ns - d->period_ns / 10) {
		return false;
	}
	d->next_due_ns += d->period_ns;
	if (d->next_due_ns <= t_ns) {
		// Fell behind (source paused / suspended): re-anchor, never burst.
		d->next_due_ns = t_ns + d->period_ns;
	}
	return true;
}


/*
 *
 * Layout + conversion.
 *
 */

static uint32_t
align64(uint32_t v)
{
	return (v + 63u) & ~63u;
}

bool
u_stereo_camera_layout(uint32_t format, uint32_t width, uint32_t height, struct u_stereo_camera_planes *out)
{
	memset(out, 0, sizeof(*out));
	if (width == 0 || height == 0) {
		return false;
	}
	switch (format) {
	case 1: // GRAY8
		out->plane_count = 1;
		out->pitch[0] = align64(width);
		out->size = (uint64_t)out->pitch[0] * height;
		return true;
	case 2: // NV12
		if ((width & 1u) != 0 || (height & 1u) != 0) {
			return false;
		}
		out->plane_count = 2;
		out->pitch[0] = align64(width);
		out->pitch[1] = align64(width);
		out->offset[1] = (uint64_t)out->pitch[0] * height;
		out->size = out->offset[1] + (uint64_t)out->pitch[1] * (height / 2);
		return true;
	case 3: // BGRA8
		out->plane_count = 1;
		out->pitch[0] = align64(width * 4u);
		out->size = (uint64_t)out->pitch[0] * height;
		return true;
	default: return false;
	}
}

static inline uint8_t
clamp_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline uint8_t
luma_601(int r, int g, int b)
{
	return clamp_u8(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
}

bool
u_stereo_camera_convert(uint32_t src_format,
                        const uint8_t *const src_planes[2],
                        const uint32_t src_pitches[2],
                        uint32_t dst_format,
                        uint8_t *dst,
                        const struct u_stereo_camera_planes *dl,
                        uint32_t width,
                        uint32_t height)
{
	if (src_planes == NULL || src_planes[0] == NULL || dst == NULL || dl == NULL || width == 0 || height == 0) {
		return false;
	}
	uint8_t *d0 = dst + dl->offset[0];
	uint8_t *d1 = dst + dl->offset[1];
	const uint8_t *s0 = src_planes[0];
	const uint8_t *s1 = src_planes[1];

	// Same format: row copy (pitches may differ).
	if (src_format == dst_format) {
		uint32_t row = src_format == 3 ? width * 4u : width;
		for (uint32_t y = 0; y < height; y++) {
			memcpy(d0 + (size_t)y * dl->pitch[0], s0 + (size_t)y * src_pitches[0], row);
		}
		if (src_format == 2) {
			if (s1 == NULL) {
				return false;
			}
			for (uint32_t y = 0; y < height / 2; y++) {
				memcpy(d1 + (size_t)y * dl->pitch[1], s1 + (size_t)y * src_pitches[1], width);
			}
		}
		return src_format >= 1 && src_format <= 3;
	}

	// GRAY8 / NV12 luma are copied verbatim between each other (a grey sensor's
	// value IS its luma); only the RGB legs apply BT.601.
	if ((src_format == 1 || src_format == 2) && dst_format == 1) {
		for (uint32_t y = 0; y < height; y++) {
			memcpy(d0 + (size_t)y * dl->pitch[0], s0 + (size_t)y * src_pitches[0], width);
		}
		return true;
	}
	if (src_format == 1 && dst_format == 2) {
		for (uint32_t y = 0; y < height; y++) {
			memcpy(d0 + (size_t)y * dl->pitch[0], s0 + (size_t)y * src_pitches[0], width);
		}
		for (uint32_t y = 0; y < height / 2; y++) {
			memset(d1 + (size_t)y * dl->pitch[1], 128, width);
		}
		return true;
	}
	if (src_format == 1 && dst_format == 3) {
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t *s = s0 + (size_t)y * src_pitches[0];
			uint8_t *o = d0 + (size_t)y * dl->pitch[0];
			for (uint32_t x = 0; x < width; x++) {
				o[4 * x + 0] = o[4 * x + 1] = o[4 * x + 2] = s[x];
				o[4 * x + 3] = 255;
			}
		}
		return true;
	}
	if (src_format == 2 && dst_format == 3) {
		if (s1 == NULL) {
			return false;
		}
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t *ys = s0 + (size_t)y * src_pitches[0];
			const uint8_t *uv = s1 + (size_t)(y / 2) * src_pitches[1];
			uint8_t *o = d0 + (size_t)y * dl->pitch[0];
			for (uint32_t x = 0; x < width; x++) {
				int c = (int)ys[x] - 16;
				int dd = (int)uv[x & ~1u] - 128;
				int e = (int)uv[(x & ~1u) + 1] - 128;
				o[4 * x + 2] = clamp_u8((298 * c + 409 * e + 128) >> 8);
				o[4 * x + 1] = clamp_u8((298 * c - 100 * dd - 208 * e + 128) >> 8);
				o[4 * x + 0] = clamp_u8((298 * c + 516 * dd + 128) >> 8);
				o[4 * x + 3] = 255;
			}
		}
		return true;
	}
	if (src_format == 3 && dst_format == 1) {
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t *s = s0 + (size_t)y * src_pitches[0];
			uint8_t *o = d0 + (size_t)y * dl->pitch[0];
			for (uint32_t x = 0; x < width; x++) {
				// Full-range luma: a GRAY8 consumer wants intensity, not video levels.
				o[x] =
				    (uint8_t)((29 * s[4 * x + 0] + 150 * s[4 * x + 1] + 77 * s[4 * x + 2] + 128) >> 8);
			}
		}
		return true;
	}
	if (src_format == 3 && dst_format == 2) {
		if ((width & 1u) || (height & 1u)) {
			return false;
		}
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t *s = s0 + (size_t)y * src_pitches[0];
			uint8_t *o = d0 + (size_t)y * dl->pitch[0];
			for (uint32_t x = 0; x < width; x++) {
				o[x] = luma_601(s[4 * x + 2], s[4 * x + 1], s[4 * x + 0]);
			}
		}
		for (uint32_t y = 0; y < height / 2; y++) {
			const uint8_t *a = s0 + (size_t)(2 * y) * src_pitches[0];
			const uint8_t *b = a + src_pitches[0];
			uint8_t *o = d1 + (size_t)y * dl->pitch[1];
			for (uint32_t x = 0; x < width; x += 2) {
				int bb = a[4 * x] + a[4 * x + 4] + b[4 * x] + b[4 * x + 4];
				int gg = a[4 * x + 1] + a[4 * x + 5] + b[4 * x + 1] + b[4 * x + 5];
				int rr = a[4 * x + 2] + a[4 * x + 6] + b[4 * x + 2] + b[4 * x + 6];
				bb = (bb + 2) / 4;
				gg = (gg + 2) / 4;
				rr = (rr + 2) / 4;
				o[x] = clamp_u8(128 + ((-38 * rr - 74 * gg + 112 * bb + 128) >> 8));
				o[x + 1] = clamp_u8(128 + ((112 * rr - 94 * gg - 18 * bb + 128) >> 8));
			}
		}
		return true;
	}
	return false;
}


/*
 *
 * Identity + probe.
 *
 */

static uint64_t
fnv1a64(uint64_t h, const char *s)
{
	for (; s != NULL && *s != '\0'; s++) {
		h ^= (uint8_t)*s;
		h *= 0x100000001b3ull;
	}
	return h;
}

void
u_stereo_camera_persistent_id(const char *device_identity, const char *consumer, char out[64])
{
	uint64_t a = fnv1a64(0xcbf29ce484222325ull, "dxr-stereo-camera|");
	a = fnv1a64(a, device_identity);
	a = fnv1a64(a, "|");
	a = fnv1a64(a, consumer);
	// Second lane with a different basis so the id is 128 bits wide.
	uint64_t b = fnv1a64(0x84222325cbf29ce4ull, consumer);
	b = fnv1a64(b, "|");
	b = fnv1a64(b, device_identity);
	snprintf(out, 64, "dxrcam-%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
}

bool
u_stereo_camera_estimate_disparity(const uint8_t *gray,
                                   uint32_t pitch,
                                   uint32_t eye_width,
                                   uint32_t height,
                                   uint32_t x0,
                                   uint32_t y0,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t max_disparity,
                                   float *out_disparity)
{
	if (gray == NULL || out_disparity == NULL || w == 0 || h == 0 || x0 + w > eye_width || y0 + h > height) {
		return false;
	}
	uint32_t dmax = max_disparity < x0 ? max_disparity : x0; // right x = left x - d >= 0
	uint64_t best = UINT64_MAX;
	uint32_t best_d = 0;
	uint64_t sad[1024];
	if (dmax >= 1024) {
		dmax = 1023;
	}
	for (uint32_t d = 0; d <= dmax; d++) {
		uint64_t s = 0;
		for (uint32_t y = y0; y < y0 + h; y++) {
			const uint8_t *l = gray + (size_t)y * pitch + x0;
			const uint8_t *r = gray + (size_t)y * pitch + eye_width + x0 - d;
			for (uint32_t x = 0; x < w; x++) {
				int diff = (int)l[x] - (int)r[x];
				s += (uint64_t)(diff < 0 ? -diff : diff);
			}
		}
		sad[d] = s;
		if (s < best) {
			best = s;
			best_d = d;
		}
	}
	float sub = 0.0f;
	if (best_d > 0 && best_d < dmax) {
		double a = (double)sad[best_d - 1], b = (double)sad[best_d], c = (double)sad[best_d + 1];
		double den = a - 2.0 * b + c;
		if (den > 0.0) {
			sub = (float)(0.5 * (a - c) / den);
		}
	}
	*out_disparity = (float)best_d + sub;
	return true;
}

bool
u_stereo_camera_estimate_offset(const uint8_t *gray,
                                uint32_t pitch,
                                uint32_t eye_width,
                                uint32_t height,
                                uint32_t x0,
                                uint32_t y0,
                                uint32_t w,
                                uint32_t h,
                                uint32_t max_disparity,
                                uint32_t max_dy,
                                float *out_dx,
                                float *out_dy)
{
	if (gray == NULL || out_dx == NULL || out_dy == NULL || w == 0 || h == 0 || x0 + w > eye_width ||
	    y0 + h > height || y0 < max_dy || y0 + h + max_dy > height || max_dy > 32) {
		return false;
	}
	uint32_t dmax = max_disparity < x0 ? max_disparity : x0;
	if (dmax > 255) {
		dmax = 255;
	}
	const uint32_t ny = 2 * max_dy + 1;
	// SSD surface over (d, dy); a quadratic cost makes the parabola fit unbiased.
	static const uint64_t k_inf = UINT64_MAX;
	uint64_t *ssd = (uint64_t *)malloc(sizeof(uint64_t) * (dmax + 1) * ny);
	if (ssd == NULL) {
		return false;
	}
	uint64_t best = k_inf;
	uint32_t bd = 0, by = 0;
	for (uint32_t j = 0; j < ny; j++) {
		int dy = (int)j - (int)max_dy;
		for (uint32_t d = 0; d <= dmax; d++) {
			uint64_t s = 0;
			for (uint32_t y = y0; y < y0 + h; y++) {
				const uint8_t *l = gray + (size_t)y * pitch + x0;
				const uint8_t *r = gray + (size_t)((int)y + dy) * pitch + eye_width + x0 - d;
				for (uint32_t x = 0; x < w; x++) {
					int diff = (int)l[x] - (int)r[x];
					s += (uint64_t)(diff * diff);
				}
			}
			ssd[j * (dmax + 1) + d] = s;
			if (s < best) {
				best = s;
				bd = d;
				by = j;
			}
		}
	}
	float sx = 0.0f, sy = 0.0f;
	if (bd > 0 && bd < dmax) {
		double a = (double)ssd[by * (dmax + 1) + bd - 1], b = (double)ssd[by * (dmax + 1) + bd],
		       c = (double)ssd[by * (dmax + 1) + bd + 1];
		double den = a - 2.0 * b + c;
		if (den > 0.0) {
			sx = (float)(0.5 * (a - c) / den);
		}
	}
	if (by > 0 && by + 1 < ny) {
		double a = (double)ssd[(by - 1) * (dmax + 1) + bd], b = (double)ssd[by * (dmax + 1) + bd],
		       c = (double)ssd[(by + 1) * (dmax + 1) + bd];
		double den = a - 2.0 * b + c;
		if (den > 0.0) {
			sy = (float)(0.5 * (a - c) / den);
		}
	}
	free(ssd);
	*out_dx = (float)bd + sx;
	// A left feature at row y is found at row y + dy in the right eye.
	*out_dy = (float)((int)by - (int)max_dy) + sy;
	return true;
}
