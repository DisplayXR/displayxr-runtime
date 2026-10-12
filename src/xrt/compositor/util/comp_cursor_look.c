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
#include "os/os_time.h"

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

bool
comp_cursor_draw_debug_refuse(const struct u_cursor_draw_request *req)
{
	// -1 = not read yet, 0 = off, 1 = always, else the period in ms.
	static long mode = -1;
	if (mode < 0) {
		long m = 0;
		const char *e = getenv("DXR_CURSOR_DRAW_REFUSE");
		if (e != NULL && strcmp(e, "1") == 0) {
			m = 1;
		}
		const char *p = getenv("DXR_CURSOR_DRAW_REFUSE_PERIOD_MS");
		if (p != NULL) {
			const long ms = strtol(p, NULL, 10);
			if (ms > 1) {
				m = ms;
			}
		}
		if (m != 0) {
			U_LOG_W(
			    "cursor_draw: DXR_CURSOR_DRAW_REFUSE%s - compositors refuse the runtime-drawn cursor "
			    "%s (verification hook, #1952)",
			    m > 1 ? "_PERIOD_MS" : "", m > 1 ? "half of every period" : "always");
		}
		mode = m;
	}
	if (mode == 0) {
		return false;
	}
	if (mode > 1) {
		const uint64_t period_ns = (uint64_t)mode * 1000000ull;
		if ((os_monotonic_get_ns() % period_ns) >= period_ns / 2) {
			return false;
		}
	}
	u_cursor_draw_report_refused(req, U_CURSOR_DRAW_REFUSAL_FORCED);
	return true;
}

const struct u_cursor_soften_params *
comp_cursor_soften_params(void)
{
	static struct u_cursor_soften_params params;
	static bool read = false;
	if (!read) {
		const bool changed = u_cursor_soften_params_parse(
		    getenv("DXR_CURSOR_SOFTEN"), getenv("DXR_CURSOR_SOFTEN_BLUR_K"),
		    getenv("DXR_CURSOR_SOFTEN_BLUR_MAX_PX"), getenv("DXR_CURSOR_SOFTEN_BLUR_MAX_FRAC"),
		    getenv("DXR_CURSOR_SOFTEN_ALPHA_MIN"), getenv("DXR_CURSOR_SOFTEN_FULL_D"), &params);
		read = true;
		U_LOG_W(
		    "cursor_draw: crosstalk softening %s%s%s (blur k %.3f, max %.3f x sprite height, ceiling %.2f px; "
		    "alpha min %.2f at |d| %.3f)%s "
		    "(DXR_CURSOR_SOFTEN*, ADR-046 Amendment 2 B.7, one-time notice)",
		    params.blur || params.alpha ? "" : "OFF", params.blur ? "blur" : "",
		    params.blur && params.alpha ? "+alpha" : (params.alpha ? "alpha" : ""), params.blur_k,
		    params.blur_max_frac, params.blur_max_px, params.alpha_min, params.full_d,
		    changed ? " [env override]" : "");
	}
	return &params;
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
