// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen (per-monitor) display information (multi-screen M1).
 *
 * One @ref xrt_screen per monitor in the per-monitor DP registry
 * (`xrt_dp_factory_registry`), carrying the display info that until M1 only
 * existed once, for the active plug-in's panel, in `xrt_system_compositor_info`
 * (physical size, nominal viewer, recommended view scale, eye tracking). It is
 * what `XR_DXR_display_info` v22 `xrEnumerateDisplaysDXR` reports, and what a
 * session bound with `XrSessionDisplayBindingDXR` reads its display-scoped
 * Kooima inputs from.
 *
 * Deliberately NOT part of `xrt_system_compositor_info`: that struct crosses
 * IPC by value, so the list travels in its own message
 * (`system_enumerate_displays`) and is produced on demand by
 * `xrt_instance::enumerate_displays`.
 *
 * Plain data, fixed size, no pointers: safe to copy across IPC.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max screens in a @ref xrt_screen_list (= XRT_DP_REGISTRY_MAX_ENTRIES).
#define XRT_SCREEN_LIST_MAX 16

//! Size of @ref xrt_screen::plugin_id and @ref xrt_screen::device_name, incl. the NUL.
#define XRT_SCREEN_NAME_MAX 64

/*!
 * The id a screen gets when the runtime could not enumerate monitors at all
 * (empty registry: macOS, Android, a Windows box whose EDID read failed) and
 * so synthesizes the single system-default screen. Non-zero, because 0 means
 * "runtime decides" in `XrSessionDisplayBindingDXR`.
 */
#define XRT_SCREEN_ID_SYNTHETIC_DEFAULT ((uint64_t)1)

/*!
 * Where a screen's @ref xrt_screen_info came from (diagnostics + tests).
 */
enum xrt_screen_info_source
{
	//! Nothing could be said about the screen (no size, no reference).
	XRT_SCREEN_INFO_SOURCE_NONE = 0,
	//! The system-level info: this is the system-default screen, so its block
	//! IS what `xrGetSystemProperties` reports (the active plug-in's
	//! `get_display_info`, as applied to the system).
	XRT_SCREEN_INFO_SOURCE_SYSTEM = 1,
	//! The owning plug-in's `get_display_info_for_monitor` slot.
	XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR = 2,
	//! Derived by the runtime from EDID mm + pixel size (no eye tracking,
	//! view scale 1, nominal viewer at the system panel's vertical FOV).
	XRT_SCREEN_INFO_SOURCE_DERIVED = 3,
};

/*!
 * Bits for @ref xrt_screen::flags. Values match `XrDisplayFlagsDXR`.
 * @{
 */
//! The OS desktop's primary monitor.
#define XRT_SCREEN_FLAG_PRIMARY (1u << 0)
//! The screen has live eye tracking (`info.supported_eye_tracking_modes != 0`).
#define XRT_SCREEN_FLAG_TRACKED (1u << 1)
//! The system-default screen: the one `xrt_system_compositor_info` describes.
#define XRT_SCREEN_FLAG_SYSTEM_DEFAULT (1u << 2)
/*! @} */

/*!
 * The display-info block of one screen — the per-screen counterpart of the
 * `display_*` / `nominal_viewer_*` / `recommended_view_scale_*` /
 * `*_eye_tracking_mode*` fields of `xrt_system_compositor_info`.
 */
struct xrt_screen_info
{
	//! Physical size of the visible area, metres; 0 = unknown.
	float width_m;
	float height_m;

	//! Nominal viewer relative to the screen centre (+X right, +Y up, +Z toward the viewer), metres.
	float nominal_viewer_x_m;
	float nominal_viewer_y_m;
	float nominal_viewer_z_m;

	//! Recommended per-view scale (render px = window px * scale).
	float recommended_view_scale_x;
	float recommended_view_scale_y;

	//! Native panel resolution, pixels; 0 = unknown.
	uint32_t pixel_width;
	uint32_t pixel_height;

	//! Eye-tracking modes (bit 0 MANAGED, bit 1 MANUAL; 0 = none) and the default (0 MANAGED, 1 MANUAL).
	uint32_t supported_eye_tracking_modes;
	uint32_t default_eye_tracking_mode;

	//! @ref xrt_screen_info_source.
	uint32_t source;
};

/*!
 * One screen (monitor) as the runtime knows it.
 */
struct xrt_screen
{
	//! Registry monitor id (EDID-derived; see `xrt_display_descriptor::monitor_id`), or
	//! @ref XRT_SCREEN_ID_SYNTHETIC_DEFAULT. Never 0.
	uint64_t id;

	//! `XRT_SCREEN_FLAG_*`.
	uint32_t flags;

	//! Winning claim confidence (`enum xrt_display_claim_confidence`); 0 when synthesized.
	uint32_t confidence;

	//! Current desktop rect, in the space the OS places windows in.
	int32_t desktop_left;
	int32_t desktop_top;
	uint32_t desktop_width;
	uint32_t desktop_height;

	//! The connector's device mode; 0 = unknown.
	uint32_t native_width;
	uint32_t native_height;

	//! Desktop compositor scale for this output; 0 = unknown.
	float desktop_scale;

	//! EDID physical size, mm; 0 = unknown.
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;

	//! Discovery id of the plug-in that won the screen ("" = none).
	char plugin_id[XRT_SCREEN_NAME_MAX];

	//! OS output name (X11: RandR output, else the DRM connector); "" = unknown.
	char device_name[XRT_SCREEN_NAME_MAX];

	//! Display info of this screen.
	struct xrt_screen_info info;
};

/*!
 * Fixed-capacity screen list — the payload of the `system_enumerate_displays`
 * IPC message and the out-param of `xrt_instance::enumerate_displays`. The
 * system-default screen, when there is one, is `screens[0]`.
 */
struct xrt_screen_list
{
	uint32_t count;
	uint32_t _pad;
	struct xrt_screen screens[XRT_SCREEN_LIST_MAX];
};

/*!
 * Find a screen by id; NULL when absent (or @p id is 0).
 */
static inline const struct xrt_screen *
xrt_screen_list_find(const struct xrt_screen_list *list, uint64_t id)
{
	if (list == NULL || id == 0) {
		return NULL;
	}
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
		if (list->screens[i].id == id) {
			return &list->screens[i];
		}
	}
	return NULL;
}

#ifdef __cplusplus
}
#endif
