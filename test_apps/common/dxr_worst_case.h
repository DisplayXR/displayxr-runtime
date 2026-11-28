// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Size the projection swapchain ONCE, at the worst case the runtime states.
 *
 * ADR-066 Amendment 2 + app rule INV-4.2 ("size at the stated worst case"):
 * swapchains are never recreated. The runtime publishes the largest atlas and
 * the largest per-view rect any rendering mode of any screen it claimed can ask
 * for, as XrDisplayRenderingWorstCaseDXR chained on XrSystemProperties
 * (XR_DXR_display_info v28). An app allocates its projection swapchain at that
 * size once; every later 3D-mode change inside the envelope (a live 3D-mode
 * switch, a window moving to another screen) is then a re-read of the
 * rendering-mode table and a retile into the SAME swapchain. A change outside
 * it applies at the next app start. The table-changed event's
 * RECREATE_SWAPCHAINS bit stays reserved: never act on it.
 *
 * Sizing from the start-up rendering-mode table alone is what this replaces:
 * the start-up table shows one screen's 2D + current 3D entry, so a later
 * switch to a larger declared entry (e.g. sim_display's Quad) or a move to a
 * larger screen would not fit.
 *
 * Usage, in the swapchain-creation code, after computing today's size from the
 * start-up table (keep that computation: it is the fallback for an older
 * runtime):
 *
 *     XrDisplayRenderingWorstCaseDXR wc;
 *     if (DxrApplyRenderingWorstCase(instance, systemId, displayInfoEnabled,
 *                                    perView, &w, &h, &wc)) {
 *         log "swapchain at the runtime's worst case ..."
 *     } else {
 *         log once "runtime states no worst case (display_info < v28) ..."
 *     }
 *
 * Tiling stays per frame from the live table; only the allocation moves.
 *
 * Header-only and C-compatible, like dxr_view_config.h: the Windows cubes get
 * the same behaviour from displayxr::common's CreateSwapchain (>= v2.37.0,
 * displayxr-common#95); the macOS / Linux / Android cubes carry their own
 * session code and call this.
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include <openxr/openxr.h>

#if defined(__has_include)
#if __has_include(<openxr/XR_DXR_display_info.h>)
#include <openxr/XR_DXR_display_info.h>
#endif
#endif

/*
 * A tree vendoring a display_info header older than v28 still compiles: the
 * type value is fixed by the DXR author-ID block, and the runtime leaving the
 * struct zeroed is what decides whether the app falls back.
 */
#ifndef XR_TYPE_DISPLAY_RENDERING_WORST_CASE_DXR
#define XR_TYPE_DISPLAY_RENDERING_WORST_CASE_DXR ((XrStructureType)1004999452)
typedef struct XrDisplayRenderingWorstCaseDXR {
	XrStructureType type;
	void *XR_MAY_ALIAS next;
	uint32_t maxAtlasWidthPixels;
	uint32_t maxAtlasHeightPixels;
	uint32_t maxViewWidthPixels;
	uint32_t maxViewHeightPixels;
	uint32_t maxViewCount;
} XrDisplayRenderingWorstCaseDXR;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Read the runtime's worst-case statement. Returns 1 and fills @p out when the
 * runtime states one (a v28+ runtime with a display driver that described a
 * screen), 0 otherwise (@p out is then zeroed apart from its type). Pass
 * @p displayInfoEnabled = 0 when the instance did not enable XR_DXR_display_info:
 * chaining an extension struct the instance did not enable is invalid usage,
 * so nothing is chained and 0 is returned.
 */
static inline int
DxrQueryRenderingWorstCase(XrInstance instance,
			   XrSystemId systemId,
			   int displayInfoEnabled,
			   XrDisplayRenderingWorstCaseDXR *out)
{
	XrSystemProperties props;
	XrDisplayRenderingWorstCaseDXR wc;

	if (out == NULL) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	out->type = XR_TYPE_DISPLAY_RENDERING_WORST_CASE_DXR;
	if (!displayInfoEnabled || instance == XR_NULL_HANDLE || systemId == XR_NULL_SYSTEM_ID) {
		return 0;
	}

	memset(&wc, 0, sizeof(wc));
	wc.type = XR_TYPE_DISPLAY_RENDERING_WORST_CASE_DXR;
	memset(&props, 0, sizeof(props));
	props.type = XR_TYPE_SYSTEM_PROPERTIES;
	props.next = &wc;
	if (XR_FAILED(xrGetSystemProperties(instance, systemId, &props))) {
		return 0;
	}
	*out = wc;
	out->next = NULL;
	return wc.maxAtlasWidthPixels > 0 && wc.maxAtlasHeightPixels > 0 && wc.maxViewWidthPixels > 0 &&
	       wc.maxViewHeightPixels > 0;
}

/*!
 * Size a projection swapchain at the stated worst case.
 *
 * @p perView = 0: ONE atlas swapchain holding every view (tiled):
 *                 maxAtlasWidthPixels x maxAtlasHeightPixels.
 * @p perView = 1: one image per view (an array slice, or a swapchain per view):
 *                 maxViewWidthPixels x maxViewHeightPixels.
 *
 * On entry @p ioW x @p ioH is what the app computed from the start-up
 * rendering-mode table. When the runtime states a worst case they are REPLACED
 * by it (it covers every entry the runtime can send, so a start-up envelope
 * such as recommendedImageRect x 2 must not inflate it) and 1 is returned. An
 * app with a bound the runtime cannot know (Android: both device orientations,
 * the statement is for the current one) takes the per-dimension maximum itself
 * afterwards. When it does not (an older runtime, or no display driver described
 * a screen) they are left untouched and 0 is returned: the caller keeps today's
 * size and should say so once in its log. @p outWc (optional) receives the
 * statement either way, for the log line.
 */
static inline int
DxrApplyRenderingWorstCase(XrInstance instance,
			   XrSystemId systemId,
			   int displayInfoEnabled,
			   int perView,
			   uint32_t *ioW,
			   uint32_t *ioH,
			   XrDisplayRenderingWorstCaseDXR *outWc)
{
	XrDisplayRenderingWorstCaseDXR wc;
	uint32_t w, h;
	const int stated = DxrQueryRenderingWorstCase(instance, systemId, displayInfoEnabled, &wc);

	if (outWc != NULL) {
		*outWc = wc;
	}
	if (!stated || ioW == NULL || ioH == NULL) {
		return 0;
	}
	w = perView ? wc.maxViewWidthPixels : wc.maxAtlasWidthPixels;
	h = perView ? wc.maxViewHeightPixels : wc.maxAtlasHeightPixels;
	*ioW = w;
	*ioH = h;
	return 1;
}

#ifdef __cplusplus
}
#endif
