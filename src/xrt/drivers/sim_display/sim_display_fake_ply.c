// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's fake Gaussian-splat PLY (see sim_display_fake_ply.h).
 * @ingroup drv_sim_display
 */

#include "sim_display_fake_ply.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const k_props[SIM_FAKE_PLY_FLOATS_PER_SPLAT] = {
    "x",       "y",       "z",       "nx",      "ny",    "nz",    "f_dc_0", "f_dc_1", "f_dc_2",
    "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2",  "rot_3",
};

static size_t
header_write(char *buf, size_t cap, float focal, uint32_t w, uint32_t h)
{
	int n =
	    snprintf(buf, cap,
	             "ply\nformat binary_little_endian 1.0\n"
	             "comment dxr-lift-meta {\"focalPx\":%.3f,\"w\":%u,\"h\":%u,\"pivotZ\":%.1f,\"axes\":\"opencv\"}\n"
	             "element vertex %d\n",
	             (double)focal, w, h, (double)SIM_FAKE_PLY_Z_FRONT, SIM_FAKE_PLY_SPLATS);
	size_t len = n > 0 ? (size_t)n : 0;
	for (int i = 0; i < SIM_FAKE_PLY_FLOATS_PER_SPLAT; i++) {
		n = snprintf(buf != NULL && len < cap ? buf + len : NULL, buf != NULL && len < cap ? cap - len : 0,
		             "property float %s\n", k_props[i]);
		len += n > 0 ? (size_t)n : 0;
	}
	n = snprintf(buf != NULL && len < cap ? buf + len : NULL, buf != NULL && len < cap ? cap - len : 0,
	             "end_header\n");
	len += n > 0 ? (size_t)n : 0;
	return len;
}

static void
put_f32(uint8_t *dst, float v)
{
	// PLY binary_little_endian: every supported host is little-endian, but be
	// explicit so the format never depends on it.
	uint32_t u;
	memcpy(&u, &v, sizeof(u));
	dst[0] = (uint8_t)(u & 0xff);
	dst[1] = (uint8_t)((u >> 8) & 0xff);
	dst[2] = (uint8_t)((u >> 16) & 0xff);
	dst[3] = (uint8_t)((u >> 24) & 0xff);
}

float
sim_fake_ply_focal(float focal_px, uint32_t w)
{
	if (focal_px > 0.0f) {
		return focal_px;
	}
	const float half = SIM_FAKE_PLY_DEFAULT_HFOV_DEG * 0.5f * 3.14159265358979f / 180.0f;
	return (float)(w > 0 ? w : 1) * 0.5f / tanf(half);
}

size_t
sim_fake_ply_write(
    uint8_t *dst, size_t cap, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t row_pitch, float focal_px)
{
	const float f = sim_fake_ply_focal(focal_px, w);
	char hdr[1024];
	size_t hlen = header_write(hdr, sizeof(hdr), f, w, h);
	size_t need = hlen + (size_t)SIM_FAKE_PLY_SPLATS * SIM_FAKE_PLY_FLOATS_PER_SPLAT * 4u;
	if (dst == NULL || cap < need) {
		return need;
	}
	memcpy(dst, hdr, hlen);
	uint8_t *p = dst + hlen;

	const float fw = (float)(w > 0 ? w : 1), fh = (float)(h > 0 ? h : 1);
	const float cx = fw * 0.5f, cy = fh * 0.5f;
	const float sh_c0 = 0.28209479177387814f; // 3DGS: colour = 0.5 + SH_C0 * f_dc
	const float opacity_logit = 2.0f;         // sigmoid(2) ~ 0.88
	for (int layer = 0; layer < 2; layer++) {
		const float z = layer == 0 ? SIM_FAKE_PLY_Z_FRONT : SIM_FAKE_PLY_Z_BACK;
		const float shade = layer == 0 ? 1.0f : 0.6f; // the hidden layer is darker
		// ln(0.6 · (w/N) · Z/f): 0.6 of one cell's world width, so neighbours overlap.
		const float scale = logf(0.6f * (fw / (float)SIM_FAKE_PLY_GRID_X) * z / f);
		for (int gy = 0; gy < SIM_FAKE_PLY_GRID_Y; gy++) {
			const float v = ((float)gy + 0.5f) / (float)SIM_FAKE_PLY_GRID_Y;
			for (int gx = 0; gx < SIM_FAKE_PLY_GRID_X; gx++) {
				const float u = ((float)gx + 0.5f) / (float)SIM_FAKE_PLY_GRID_X;
				const float px_f = u * fw, py_f = v * fh; // the cell centre, in pixels
				float rgb[3] = {0.5f, 0.5f, 0.5f};
				if (rgba != NULL && w > 0 && h > 0) {
					uint32_t px = (uint32_t)px_f, py = (uint32_t)py_f;
					px = px >= w ? w - 1 : px;
					py = py >= h ? h - 1 : py;
					const uint8_t *s = rgba + (size_t)py * row_pitch + (size_t)px * 4u;
					rgb[0] = (float)s[0] / 255.0f;
					rgb[1] = (float)s[1] / 255.0f;
					rgb[2] = (float)s[2] / 255.0f;
				}
				const float fv[SIM_FAKE_PLY_FLOATS_PER_SPLAT] = {
				    (px_f - cx) * z / f, // X (right)
				    (py_f - cy) * z / f, // Y (DOWN, OpenCV)
				    z,                   // Z (forward)
				    0.0f,
				    0.0f,
				    0.0f,                            // normal
				    (rgb[0] * shade - 0.5f) / sh_c0, // f_dc
				    (rgb[1] * shade - 0.5f) / sh_c0,
				    (rgb[2] * shade - 0.5f) / sh_c0,
				    opacity_logit,
				    scale,
				    scale,
				    scale, // log scale
				    1.0f,
				    0.0f,
				    0.0f,
				    0.0f, // rotation (w x y z), identity
				};
				for (int k = 0; k < SIM_FAKE_PLY_FLOATS_PER_SPLAT; k++) {
					put_f32(p, fv[k]);
					p += 4;
				}
			}
		}
	}
	return need;
}
