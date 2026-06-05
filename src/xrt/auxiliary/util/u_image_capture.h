// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Small pixel-buffer helpers shared by the atlas-capture PNG encoders.
 * @ingroup aux_util
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Force every pixel's alpha to 255 (opaque) in a tightly-or-strided RGBA8
 * buffer, in place.
 *
 * The atlas-capture readback copies the swapchain's alpha channel verbatim,
 * but that alpha is *undefined for display output* — the compositor / display
 * processor (weaver) never reads it, so it is typically 0. Left as-is the
 * encoded PNG is fully transparent and renders black in normal image viewers
 * (issue #425). The captured atlas is opaque display content, so the encoder
 * forces A=255 before @c stbi_write_png.
 *
 * @param pixels       Base of an RGBA8 buffer (byte order R,G,B,A per pixel).
 * @param width        Pixels per row.
 * @param height       Number of rows.
 * @param stride_bytes Bytes per row (>= width*4; equals width*4 when tight).
 */
static inline void
u_image_force_opaque_rgba8(uint8_t *pixels, uint32_t width, uint32_t height, size_t stride_bytes)
{
	if (pixels == NULL) {
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *row = pixels + (size_t)y * stride_bytes;
		for (uint32_t x = 0; x < width; x++) {
			row[(size_t)x * 4 + 3] = 255;
		}
	}
}

#ifdef __cplusplus
}
#endif
