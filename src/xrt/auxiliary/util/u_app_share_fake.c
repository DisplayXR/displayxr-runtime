// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 synthetic app-share source; see u_app_share_fake.h.
 * @ingroup aux_util
 */

#include "util/u_app_share_fake.h"

#include "os/os_time.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TEX_SIZE 512 //!< the scrolling texture tile (power of two)
#define SCROLL_PX 2  //!< per frame

struct u_app_share_fake
{
	uint32_t eye_w, eye_h;
	int64_t period_ns;
	int64_t next_ns;
	uint64_t sequence;
	uint8_t *nv12; //!< 2*eye_w x eye_h Y, then 2*eye_w x eye_h/2 UV
	uint8_t *tex;  //!< TEX_SIZE^2 luma blocks
	int32_t *row_shift;
};

static uint32_t
hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

float
u_app_share_fake_disparity_px(uint32_t eye_w, uint32_t eye_h, uint32_t y)
{
	uint32_t y0 = 0, y1 = 0;
	u_app_share_fake_band(eye_h, &y0, &y1);
	if (y >= y0 && y < y1) {
		return 0.0f;
	}
	const float cy = (float)eye_h * 0.5f;
	const float d_max = U_APP_SHARE_FAKE_MAX_DISPARITY_FRAC * (float)eye_w;
	return roundf(d_max * ((float)y + 0.5f - cy) / cy);
}

void
u_app_share_fake_band(uint32_t eye_h, uint32_t *out_y0, uint32_t *out_y1)
{
	uint32_t band = (uint32_t)((float)eye_h * U_APP_SHARE_FAKE_BAND_FRAC);
	band = band < 8 ? 8 : band;
	*out_y0 = (eye_h - band) / 2;
	*out_y1 = *out_y0 + band;
}

struct u_app_share_fake *
u_app_share_fake_create(uint32_t eye_w, uint32_t eye_h, float fps)
{
	if (eye_w < 16 || eye_h < 16 || (eye_w & 1u) != 0 || (eye_h & 1u) != 0 || !(fps > 0.0f)) {
		return NULL;
	}
	struct u_app_share_fake *f = (struct u_app_share_fake *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return NULL;
	}
	f->eye_w = eye_w;
	f->eye_h = eye_h;
	f->period_ns = (int64_t)(1e9 / (double)fps);
	const size_t w = 2u * eye_w;
	f->nv12 = (uint8_t *)malloc(w * eye_h + w * (eye_h / 2));
	f->tex = (uint8_t *)malloc((size_t)TEX_SIZE * TEX_SIZE);
	f->row_shift = (int32_t *)malloc(sizeof(int32_t) * eye_h);
	if (f->nv12 == NULL || f->tex == NULL || f->row_shift == NULL) {
		u_app_share_fake_destroy(&f);
		return NULL;
	}
	// 4x4 blocks of hashed luma 32..223: rich enough for block matching.
	for (uint32_t v = 0; v < TEX_SIZE; v++) {
		for (uint32_t u = 0; u < TEX_SIZE; u++) {
			f->tex[v * TEX_SIZE + u] =
			    (uint8_t)(32u + hash32((u >> 2) * 73856093u ^ (v >> 2) * 19349663u) % 192u);
		}
	}
	for (uint32_t y = 0; y < eye_h; y++) {
		f->row_shift[y] = (int32_t)u_app_share_fake_disparity_px(eye_w, eye_h, y);
	}
	// Chroma: neutral, the band tinted so it is visible in a dump.
	uint8_t *uv = f->nv12 + w * eye_h;
	uint32_t y0 = 0, y1 = 0;
	u_app_share_fake_band(eye_h, &y0, &y1);
	for (uint32_t r = 0; r < eye_h / 2; r++) {
		const bool in_band = 2 * r >= y0 && 2 * r < y1;
		for (size_t x = 0; x < w; x += 2) {
			uv[r * w + x + 0] = in_band ? 90 : 128;
			uv[r * w + x + 1] = in_band ? 170 : 128;
		}
	}
	return f;
}

void
u_app_share_fake_destroy(struct u_app_share_fake **f_ptr)
{
	struct u_app_share_fake *f = *f_ptr;
	if (f == NULL) {
		return;
	}
	free(f->nv12);
	free(f->tex);
	free(f->row_shift);
	free(f);
	*f_ptr = NULL;
}

static void
render(struct u_app_share_fake *f)
{
	const uint32_t ew = f->eye_w, eh = f->eye_h, w = 2 * ew;
	const uint32_t phase = (uint32_t)(f->sequence * SCROLL_PX);
	uint32_t y0 = 0, y1 = 0;
	u_app_share_fake_band(eh, &y0, &y1);
	for (uint32_t y = 0; y < eh; y++) {
		uint8_t *row = f->nv12 + (size_t)y * w;
		const uint8_t *trow = f->tex + (size_t)(y & (TEX_SIZE - 1)) * TEX_SIZE;
		if (y == y0 || y == y0 + 1 || y + 1 == y1 || y + 2 == y1) {
			memset(row, 235, w); // the marker band's frame, identical in both eyes
			continue;
		}
		// Left: T(x + phase). Right: T(x - d + phase), so a feature at left x sits at right x + d.
		const int32_t d = f->row_shift[y];
		for (uint32_t x = 0; x < ew; x++) {
			row[x] = trow[(x + phase) & (TEX_SIZE - 1)];
			row[ew + x] = trow[(uint32_t)((int32_t)(x + phase) - d) & (TEX_SIZE - 1)];
		}
	}
}

bool
u_app_share_fake_next(struct u_app_share_fake *f, int64_t timeout_ns, struct u_app_share_fake_frame *out)
{
	int64_t now = (int64_t)os_monotonic_get_ns();
	if (f->next_ns == 0) {
		f->next_ns = now;
	}
	if (f->next_ns - now > timeout_ns) {
		os_nanosleep(timeout_ns > 0 ? timeout_ns : 0);
		return false;
	}
	if (f->next_ns > now) {
		os_nanosleep(f->next_ns - now);
		now = (int64_t)os_monotonic_get_ns();
	}
	// Never "catch up" with a burst after a stall: one frame, then the next period.
	f->next_ns = (now - f->next_ns > f->period_ns ? now : f->next_ns) + f->period_ns;
	f->sequence++;
	render(f);
	const uint32_t w = 2 * f->eye_w;
	out->sequence = f->sequence;
	out->time_ns = now;
	out->width = w;
	out->height = f->eye_h;
	out->planes[0] = f->nv12;
	out->planes[1] = f->nv12 + (size_t)w * f->eye_h;
	out->pitches[0] = w;
	out->pitches[1] = w;
	return true;
}
