// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look (platform-neutral half).
 * @author David Fattal
 * @ingroup comp_util
 */

#include "comp_cursor_look.h"

#include "util/u_cursor_depth.h"
#include "util/u_logging.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

//! DXR_CURSOR_DEFAULT_LOOK=arrow forces the classic arrow.
static bool
force_arrow(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("DXR_CURSOR_DEFAULT_LOOK");
		cached = (e != NULL && strcmp(e, "arrow") == 0) ? 1 : 0;
	}
	return cached == 1;
}

//! The classic arrow (Amendment 2 A.6): 12 x 19, tip = hotspot.
static bool
use_arrow(struct comp_cursor_look *look)
{
	if (look->rgba != NULL && look->os_cursor == NULL) {
		return true; // already the arrow
	}
	const uint32_t h = 76; // 4 px per arrow px: crisp up to 4x scale
	const uint32_t w = (uint32_t)ceilf((float)h * U_CURSOR_ARROW_WIDTH_PX / U_CURSOR_ARROW_HEIGHT_PX);
	uint8_t *rgba = calloc((size_t)w * h, 4);
	if (rgba == NULL) {
		return false;
	}
	u_cursor_draw_arrow_rgba(rgba, w, h);
	free(look->rgba);
	look->rgba = rgba;
	look->w = w;
	look->h = h;
	if (look->os_cursor != NULL) {
		comp_cursor_look_platform_release(look->os_cursor);
		look->os_cursor = NULL;
	}
	look->aspect = (float)w / (float)h;
	look->hot_u = 0.0f;
	look->hot_v = 0.0f;
	look->height_pt = U_CURSOR_DRAW_DEFAULT_POINTS;
	look->generation++;
	U_LOG_W("cursor_draw: default look = the classic arrow (12x19, tip = hotspot)");
	return true;
}

bool
comp_cursor_look_update(struct comp_cursor_look *look, float backing_scale)
{
	if (!force_arrow() && comp_cursor_look_platform_mirror(look, backing_scale)) {
		return true;
	}
	// Could not read the OS cursor here: keep the last image, else the arrow.
	return look->rgba != NULL || use_arrow(look);
}

float
comp_cursor_look_backing_scale(void)
{
	return comp_cursor_look_platform_backing_scale();
}

void
comp_cursor_look_fini(struct comp_cursor_look *look)
{
	free(look->rgba);
	look->rgba = NULL;
	look->os_failed = NULL;
	if (look->os_cursor != NULL) {
		comp_cursor_look_platform_release(look->os_cursor);
		look->os_cursor = NULL;
	}
}

#if !defined(__APPLE__) && !defined(_WIN32)
/*
 * No OS-cursor mirror on this platform yet (Linux: XFixesGetCursorImage is
 * the follow-up). The arrow it is.
 */
bool
comp_cursor_look_platform_mirror(struct comp_cursor_look *look, float backing_scale)
{
	(void)look;
	(void)backing_scale;
	return false;
}

void
comp_cursor_look_platform_release(void *os_cursor)
{
	(void)os_cursor;
}

float
comp_cursor_look_platform_backing_scale(void)
{
	return 1.0f;
}
#endif
