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
header_write(char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "ply\nformat binary_little_endian 1.0\nelement vertex %d\n", SIM_FAKE_PLY_SPLATS);
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

size_t
sim_fake_ply_write(uint8_t *dst, size_t cap, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t row_pitch)
{
	char hdr[1024];
	size_t hlen = header_write(hdr, sizeof(hdr));
	size_t need = hlen + (size_t)SIM_FAKE_PLY_SPLATS * SIM_FAKE_PLY_FLOATS_PER_SPLAT * 4u;
	if (dst == NULL || cap < need) {
		return need;
	}
	memcpy(dst, hdr, hlen);
	uint8_t *p = dst + hlen;

	const float aspect = (w > 0 && h > 0) ? (float)w / (float)h : 1.0f;
	const float sh_c0 = 0.28209479177387814f; // 3DGS: colour = 0.5 + SH_C0 * f_dc
	for (int layer = 0; layer < 2; layer++) {
		const float z = layer == 0 ? 0.0f : 0.5f;      // back layer sits behind
		const float shade = layer == 0 ? 1.0f : 0.45f; // and darker
		const float scale = logf(layer == 0 ? 0.03f : 0.05f);
		for (int gy = 0; gy < SIM_FAKE_PLY_GRID_Y; gy++) {
			for (int gx = 0; gx < SIM_FAKE_PLY_GRID_X; gx++) {
				float u = ((float)gx + 0.5f) / (float)SIM_FAKE_PLY_GRID_X;
				float v = ((float)gy + 0.5f) / (float)SIM_FAKE_PLY_GRID_Y;
				float rgb[3] = {0.5f, 0.5f, 0.5f};
				if (rgba != NULL && w > 0 && h > 0) {
					uint32_t px = (uint32_t)(u * (float)w);
					uint32_t py = (uint32_t)(v * (float)h);
					px = px >= w ? w - 1 : px;
					py = py >= h ? h - 1 : py;
					const uint8_t *s = rgba + (size_t)py * row_pitch + (size_t)px * 4u;
					rgb[0] = (float)s[0] / 255.0f;
					rgb[1] = (float)s[1] / 255.0f;
					rgb[2] = (float)s[2] / 255.0f;
				}
				float f[SIM_FAKE_PLY_FLOATS_PER_SPLAT] = {
				    (u - 0.5f) * aspect, // x
				    (0.5f - v),          // y (up)
				    z,                   // z
				    0.0f,
				    0.0f,
				    0.0f,                            // normal
				    (rgb[0] * shade - 0.5f) / sh_c0, // f_dc
				    (rgb[1] * shade - 0.5f) / sh_c0,
				    (rgb[2] * shade - 0.5f) / sh_c0,
				    2.0f, // opacity logit (~0.88)
				    scale,
				    scale,
				    scale, // log scale
				    1.0f,
				    0.0f,
				    0.0f,
				    0.0f, // rotation (w x y z), identity
				};
				for (int k = 0; k < SIM_FAKE_PLY_FLOATS_PER_SPLAT; k++) {
					put_f32(p, f[k]);
					p += 4;
				}
			}
		}
	}
	return need;
}
