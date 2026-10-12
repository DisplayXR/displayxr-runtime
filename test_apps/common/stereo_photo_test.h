// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A synthetic STEREO PHOTO for the measured-disparity cursor
 *         (XR_DXR_cursor_depth v7, ADR-046 Amendment 3).
 *
 * Stereo content with no depth - the case the measured-disparity source is
 * for. Each view is generated from one "cyclopean" scene whose layers carry a
 * disparity, so the correct answer under the cursor is known exactly.
 * Regions, view-normalised, with their disparity as a fraction of the view
 * width (> 0 = crossed, in front of the glass):
 *
 *   sky        y < 0.35            flat gradient, 0       -> no texture: OS cursor
 *   ground     y >= 0.35           value noise,  -0.012   -> behind the glass: OS cursor
 *   stripes    x .06-.34 y .50-.90 period 1.25% w, +0.010 -> repetitive: rejected, OS cursor
 *   foreground x .44-.80 y .40-.85 value noise,  +0.030   -> the arrow, in front of it
 *
 * Cursor UVs that land squarely in each: foreground 0.6,0.6 - sky 0.5,0.15 -
 * stripes 0.18,0.68 - ground 0.9,0.6. No channel exceeds 235, so a drawn
 * default-look arrow's pure-white fill is unambiguous in a capture.
 *
 * Header-only, used by cube_handle_metal_macos and cube_handle_vk_macos
 * (DISPLAYXR_CURSOR_DEPTH_SOURCE=measured / DISPLAYXR_STEREO_PHOTO=1).
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace dxr_test {

//! Disparity of the foreground / ground / stripes, as a fraction of the view width.
constexpr float kPhotoForegroundD = 0.030f;
constexpr float kPhotoGroundD = -0.012f;
constexpr float kPhotoStripesD = 0.010f;

inline float
PhotoHash(int32_t x, int32_t y, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xffffu) / 65535.0f;
}

//! Smooth value noise on a 3 px lattice (the runtime unit tests' texture).
inline float
PhotoNoise(float x, float y, uint32_t seed)
{
	const float fx = x / 3.0f, fy = y / 3.0f;
	const int32_t ix = (int32_t)std::floor(fx), iy = (int32_t)std::floor(fy);
	float tx = fx - (float)ix, ty = fy - (float)iy;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	const float a = PhotoHash(ix, iy, seed), b = PhotoHash(ix + 1, iy, seed);
	const float c = PhotoHash(ix, iy + 1, seed), d = PhotoHash(ix + 1, iy + 1, seed);
	const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
	return top + (bottom - top) * ty;
}

/*!
 * One pixel (R, G, B) of view @p eye of @p eyes, the view @p w x @p h px:
 * each layer's texture is seen at x - shift, shift = D * (0.5 - eye / (eyes -
 * 1)), so view 0 (the leftmost) sees crossed content shifted right.
 */
inline void
PhotoPixel(int eye, int eyes, float x, float y, float w, float h, float rgb[3])
{
	const float k = eyes > 1 ? 0.5f - (float)eye / (float)(eyes - 1) : 0.0f;
	const float ys = y / h;
	float r = 110.0f, g = 160.0f, b = 215.0f - 40.0f * ys; // sky
	if (ys >= 0.35f) {                                     // ground, behind the glass
		const float xc = x - (kPhotoGroundD * w) * k;
		const float n = PhotoNoise(xc, y, 11);
		r = 50.0f + 90.0f * n;
		g = 70.0f + 120.0f * n;
		b = 40.0f + 60.0f * n;
	}
	{ // stripes, crossed but repetitive
		const float xc = x - (kPhotoStripesD * w) * k;
		if (xc >= 0.06f * w && xc < 0.34f * w && ys >= 0.50f && ys < 0.90f) {
			const float period = std::fmax(6.0f, std::round(0.0125f * w));
			r = g = b = 128.0f + 90.0f * std::sin(2.0f * 3.14159265f * xc / period);
		}
	}
	{ // textured foreground, crossed
		const float xc = x - (kPhotoForegroundD * w) * k;
		if (xc >= 0.44f * w && xc < 0.80f * w && ys >= 0.40f && ys < 0.85f) {
			const float n = PhotoNoise(xc, y, 12);
			r = 60.0f + 170.0f * n;
			g = 40.0f + 120.0f * n;
			b = 30.0f + 80.0f * PhotoNoise(xc + 400.0f, y, 13);
		}
	}
	rgb[0] = std::fmin(r, 235.0f);
	rgb[1] = std::fmin(g, 235.0f);
	rgb[2] = std::fmin(b, 235.0f);
}

/*!
 * One view's image, @p img_w x @p h, 4 bytes per pixel, BGRA (@p bgra) or
 * RGBA. The scene is laid out for a view @p view_w wide; @p img_w may exceed
 * it (a pan margin for the "playing video" mode).
 */
inline void
PhotoFill(int eye, int eyes, uint32_t view_w, uint32_t img_w, uint32_t h, bool bgra, std::vector<uint8_t> &out)
{
	out.resize((size_t)img_w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < img_w; x++) {
			float c[3];
			PhotoPixel(eye, eyes, (float)x + 0.5f, (float)y + 0.5f, (float)view_w, (float)h, c);
			uint8_t *p = &out[((size_t)y * img_w + x) * 4];
			p[0] = (uint8_t)(bgra ? c[2] : c[0]);
			p[1] = (uint8_t)c[1];
			p[2] = (uint8_t)(bgra ? c[0] : c[2]);
			p[3] = 255;
		}
	}
}

} // namespace dxr_test
