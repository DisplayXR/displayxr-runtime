// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display FAKE stereo camera: the synthetic SBS scene.
 * @ingroup drv_sim_display
 */

#include "sim_display_stereo_camera_pattern.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

//! 5x7 digits, one byte per row, bit 4 = leftmost column.
static const uint8_t k_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};

#define COUNTER_DIGITS 8
#define COUNTER_SCALE 3
#define COUNTER_X 8
#define COUNTER_Y 8

static inline uint32_t
hash3(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

//! Value noise on 2-pixel cells: detail a block matcher locks onto, without
//! single-pixel aliasing.
static inline uint8_t
texel(uint32_t u, uint32_t v, uint32_t seed, uint8_t lo, uint8_t span)
{
	return (uint8_t)(lo + hash3(u >> 1, v >> 1, seed) % span);
}

void
sim_stereo_camera_scene_init(struct sim_stereo_camera_scene *s,
                             uint32_t eye_width,
                             uint32_t eye_height,
                             double fx_px,
                             double baseline_mm,
                             double bg_depth_m,
                             double bar_depth_m)
{
	memset(s, 0, sizeof(*s));
	s->eye_width = eye_width;
	s->eye_height = eye_height;
	s->bg_disparity = (uint32_t)lround(fx_px * (baseline_mm / 1000.0) / bg_depth_m);
	s->bar_disparity = (uint32_t)lround(fx_px * (baseline_mm / 1000.0) / bar_depth_m);
	s->bar_x0 = eye_width * 3 / 8;
	s->bar_x1 = eye_width * 5 / 8;
	s->bar_y0 = eye_height / 4;
	s->bar_y1 = eye_height * 3 / 4;
}

static void
render_eye(const struct sim_stereo_camera_scene *s, uint8_t *dst, uint32_t pitch, uint32_t x_off, int right)
{
	for (uint32_t y = 0; y < s->eye_height; y++) {
		uint8_t *row = dst + (size_t)y * pitch + x_off;
		bool in_bar_rows = y >= s->bar_y0 && y < s->bar_y1;
		for (uint32_t x = 0; x < s->eye_width; x++) {
			// A feature at scene coordinate u shows at x = u in the left eye and at
			// x = u - d in the right eye, so the right eye samples u = x + d.
			uint32_t ub = right ? x + s->bg_disparity : x;
			uint32_t ubar = right ? x + s->bar_disparity : x;
			if (in_bar_rows && ubar >= s->bar_x0 && ubar < s->bar_x1) {
				row[x] = texel(ubar, y, 0xBA5u, 128, 128);
			} else {
				row[x] = texel(ub, y, 0x0B6u, 0, 112);
			}
		}
	}
}

static void
render_counter(uint8_t *dst, uint32_t pitch, uint32_t x_off, uint32_t eye_w, uint32_t eye_h, uint64_t n)
{
	const uint32_t cw = 6 * COUNTER_SCALE, ch = 9 * COUNTER_SCALE;
	const uint32_t box_w = COUNTER_DIGITS * cw + COUNTER_SCALE, box_h = ch;
	if (COUNTER_X + box_w > eye_w || COUNTER_Y + box_h > eye_h) {
		return;
	}
	for (uint32_t y = 0; y < box_h; y++) {
		memset(dst + (size_t)(COUNTER_Y + y) * pitch + x_off + COUNTER_X, 0, box_w);
	}
	char digits[COUNTER_DIGITS];
	for (int i = COUNTER_DIGITS - 1; i >= 0; i--) {
		digits[i] = (char)(n % 10);
		n /= 10;
	}
	for (uint32_t i = 0; i < COUNTER_DIGITS; i++) {
		const uint8_t *g = k_digits[(int)digits[i]];
		uint32_t gx = COUNTER_X + COUNTER_SCALE + i * cw;
		for (uint32_t r = 0; r < 7; r++) {
			for (uint32_t c = 0; c < 5; c++) {
				if (((g[r] >> (4 - c)) & 1u) == 0) {
					continue;
				}
				for (uint32_t sy = 0; sy < COUNTER_SCALE; sy++) {
					uint8_t *p =
					    dst + (size_t)(COUNTER_Y + COUNTER_SCALE + r * COUNTER_SCALE + sy) * pitch +
					    x_off + gx + c * COUNTER_SCALE;
					memset(p, 255, COUNTER_SCALE);
				}
			}
		}
	}
}

void
sim_stereo_camera_render_gray(const struct sim_stereo_camera_scene *s,
                              uint64_t frame_number,
                              uint8_t *dst,
                              uint32_t pitch)
{
	render_eye(s, dst, pitch, 0, 0);
	render_eye(s, dst, pitch, s->eye_width, 1);
	render_counter(dst, pitch, 0, s->eye_width, s->eye_height, frame_number);
	render_counter(dst, pitch, s->eye_width, s->eye_width, s->eye_height, frame_number);
}
