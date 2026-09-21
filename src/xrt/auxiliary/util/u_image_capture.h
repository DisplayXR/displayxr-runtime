// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Small pixel-buffer helpers shared by the atlas-capture PNG encoders.
 * @ingroup aux_util
 */

#pragma once

#include "util/u_debug.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

DEBUG_GET_ONCE_BOOL_OPTION(atlas_capture_raw_alpha, "DXR_ATLAS_CAPTURE_RAW_ALPHA", false)

/*!
 * Should the atlas capture write the atlas's TRUE alpha channel?
 *
 * Default false: @ref u_image_force_opaque_rgba8 stamps A=255, because a
 * human opening the PNG wants to see the picture and swapchain alpha is
 * undefined for display output (#425).
 *
 * `DXR_ATLAS_CAPTURE_RAW_ALPHA=1` turns the capture into an ORACLE instead of
 * a picture. Alpha in the atlas is load-bearing state, not decoration:
 *
 *  - the display processor lerps the desktop in under it (the #225
 *    compose-under gate), so a wrong alpha is a visible product bug; and
 *  - the OpenXR §10.6.2 rule that a layer with no
 *    `BLEND_TEXTURE_SOURCE_ALPHA_BIT` is an opaque cover (alpha forced to one)
 *    is testable ONLY by reading that channel back — 0 means the fix is
 *    missing, 255 means it is present.
 *
 * With the forced opacity always on, that acceptance check reads 255 whether
 * the fix works or not: a test that cannot fail. Hence the opt-in.
 *
 * Process-level, read once.
 *
 * @ingroup aux_util
 */
static inline bool
u_image_capture_raw_alpha(void)
{
	return debug_get_bool_option_atlas_capture_raw_alpha();
}

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
 * Gate every call on @ref u_image_capture_raw_alpha() being false, so an
 * oracle can opt into the real alpha channel.
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
