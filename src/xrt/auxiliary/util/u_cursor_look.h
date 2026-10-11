// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's DEFAULT LOOK (XR_DXR_cursor_depth v5,
 *         ADR-046 Amendment 2 A.6 / B): the current OS cursor where the
 *         platform lets us read it, else the classic 12 x 19 arrow.
 * @author David Fattal
 * @ingroup aux_util
 *
 * Graphics-API-agnostic: it produces an RGBA8 image (sRGB-encoded, STRAIGHT
 * alpha, top row first) plus its hotspot and the height the OS draws it at, in
 * panel pixels. Each compositor uploads the image into its own texture when
 * @ref u_cursor_look::serial changes, and draws it like an app sprite.
 *
 * Windows: GetCursorInfo() -> HCURSOR, rasterised with DrawIconEx() over black
 * and over white and recovered by u_cursor_draw_unblend(), which reads every
 * cursor kind (32-bit alpha, colour + mask, monochrome AND/XOR) the same way.
 * GetCursorInfo() is global (not per thread or process), so this is safe from
 * the compositor thread. Re-rasterised only when the HCURSOR or the DPI
 * changes. While the app has hidden its cursor with SetCursor(NULL) - which
 * it does exactly while the runtime draws, so on most drawn frames - the shape
 * is the CLASS cursor of the window under the pointer (what that window shows
 * when it does not override WM_SETCURSOR), else the last shape, else the
 * system arrow (IDC_ARROW). An app that changes its cursor per region in
 * WM_SETCURSOR and wants that shape drawn passes its own sprite instead.
 *
 * Elsewhere (and with DXR_CURSOR_DEFAULT_LOOK=arrow): the classic arrow,
 * 19 px tall at 96 DPI - the Metal backend reads NSCursor itself.
 *
 * ZERO COST UNLESS REQUESTED: nothing here runs, and nothing is allocated,
 * until a compositor is handed a default-look draw request.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The default look's current image. Zero-initialise; free with
 * u_cursor_look_fini().
 */
struct u_cursor_look
{
	//! width * height RGBA8: sRGB-encoded, STRAIGHT alpha, top row first.
	uint8_t *rgba;
	uint32_t width;
	uint32_t height;
	//! The click point, normalised (origin top-left, v down).
	float hot_u;
	float hot_v;
	//! The height the OS draws this image at, in panel pixels (at the DPI
	//! passed to u_cursor_look_update): the sprite's physical size is this
	//! times the panel's pixel pitch.
	float height_px;
	//! True when mirroring the OS cursor, false for the arrow.
	bool from_os;
	//! Bumped on every image change; 0 = no image yet. Compare against the
	//! serial a texture was uploaded at.
	uint32_t serial;

	// Private.
	uintptr_t os_handle;     //!< the HCURSOR the image mirrors
	uintptr_t failed_handle; //!< an HCURSOR that could not be read (not retried)
	uint32_t dpi;            //!< the DPI the image was made at
	int force_arrow;         //!< DXR_CURSOR_DEFAULT_LOOK: 0 = not read yet, 1 = no, 2 = arrow
	bool announced;
};

/*!
 * Refresh the default look for this frame. Cheap when nothing changed (one
 * GetCursorInfo() on Windows).
 *
 * @param dpi The panel's DPI (96 = 100 %); 0 means 96.
 * @return true when @p look holds an image to draw.
 */
bool
u_cursor_look_update(struct u_cursor_look *look, uint32_t dpi);

//! Free the image and zero the struct.
void
u_cursor_look_fini(struct u_cursor_look *look);

#ifdef __cplusplus
}
#endif
