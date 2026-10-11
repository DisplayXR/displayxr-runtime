// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look, shared by every compositor.
 * @author David Fattal
 * @ingroup comp_util
 *
 * XR_DXR_cursor_depth v5 (ADR-046 Amendment 2 A.6 / B): when the app hands
 * the runtime no sprite, the runtime draws its DEFAULT LOOK - the current OS
 * cursor's shape where the runtime can read it, else the classic arrow
 * (u_cursor_draw_arrow_rgba: 12 x 19, white fill, dark outline, tip =
 * hotspot).
 *
 * This module owns the CPU side of that choice, so the Metal, Vulkan and GL
 * compositors cannot drift apart on it: which look, its pixels (premultiplied,
 * sRGB-encoded RGBA8, top row first), its aspect, hotspot and size in OS
 * points. A compositor only uploads the pixels whenever @ref
 * comp_cursor_look::generation changes and samples them.
 *
 * OS-cursor mirroring is macOS-only today ([NSCursor currentCursor], read
 * only when the commit runs on the main thread - AppKit is main-thread-only).
 * Elsewhere, and whenever the shape cannot be read, the look is the arrow.
 * `DXR_CURSOR_DEFAULT_LOOK=arrow` forces the arrow everywhere.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0): nothing here runs, and
 * nothing is allocated, until a compositor is handed a draw request with no
 * app sprite.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The default look's current image. Zero-initialise; release with
 * comp_cursor_look_fini().
 */
struct comp_cursor_look
{
	//! Premultiplied, sRGB-encoded RGBA8, top row first (w * h * 4 bytes).
	uint8_t *rgba;
	uint32_t w, h;
	//! Width / height.
	float aspect;
	//! Hotspot, normalised (origin top-left, v down).
	float hot_u, hot_v;
	//! Height in OS points (the arrow: U_CURSOR_DRAW_DEFAULT_POINTS).
	float height_pt;
	//! Bumped every time the pixels change: re-upload when it moves.
	uint64_t generation;
	//! The mirrored OS cursor (platform object, retained), NULL for the arrow.
	void *os_cursor;
};

/*!
 * Refresh the default look for this commit: re-read the OS cursor when the
 * platform allows it here, else keep the last image, else the arrow.
 *
 * @param backing_scale The output's backing pixels per OS point (raster
 *        resolution of a mirrored OS cursor); <= 0 picks the platform's.
 * @return false only if no image could be produced at all.
 */
bool
comp_cursor_look_update(struct comp_cursor_look *look, float backing_scale);

/*!
 * Backing pixels per OS point of the main screen (macOS), 1 elsewhere. Read on
 * the main thread and cached; off the main thread before the first read, 2 on
 * macOS. The panel's metres per OS point is its pixel pitch times this.
 */
float
comp_cursor_look_backing_scale(void);

//! Free the image and release a mirrored OS cursor.
void
comp_cursor_look_fini(struct comp_cursor_look *look);


/*
 *
 * Internal - the platform half (comp_cursor_look_macos.m on Apple).
 *
 */

/*!
 * Mirror the current OS cursor into @p look if the platform can read it now
 * and it is not already the mirrored one.
 *
 * @return true if @p look now holds the OS cursor (changed or not), false if
 *         it could not be read here (the caller falls back).
 */
bool
comp_cursor_look_platform_mirror(struct comp_cursor_look *look, float backing_scale);

//! Release a mirrored platform cursor object.
void
comp_cursor_look_platform_release(void *os_cursor);

//! @copydoc comp_cursor_look_backing_scale
float
comp_cursor_look_platform_backing_scale(void);

#ifdef __cplusplus
}
#endif
