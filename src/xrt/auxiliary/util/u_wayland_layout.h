// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which space Mutter's stage coordinates are in: the factor that turns
 *         them into device pixels.
 * @ingroup aux_util
 *
 * ## Mutter's two layout modes: the monitor scale is not always the factor
 *
 * @ref u_wayland_geom.h converts "logical" rects by a monitor's scale. For a
 * stage-coordinate publisher (the `window-geometry@displayxr.org` GNOME Shell
 * extension) "logical" means Mutter's STAGE coordinates, and Mutter has two
 * layout modes that put the stage in different spaces:
 *
 *   - LOGICAL (`layout-mode` 1): the stage is in logical px. A 3840x2160
 *     monitor at scale 2 is a 1920x1080 stage rect, and device px are stage px
 *     x the monitor scale. Every fractional-scaling desktop is in this mode
 *     (older GNOME enables it with the `scale-monitor-framebuffer`
 *     experimental feature; mutter 50 starts in it by default).
 *   - PHYSICAL (`layout-mode` 2): the stage is in DEVICE px. The same monitor
 *     at scale 2 is a 3840x2160 stage rect, and the scale only says how big
 *     clients draw. Ubuntu 24.04 / GNOME 46 at an integer scale, out of the
 *     box.
 *
 * Multiplying stage coordinates by the monitor scale is right in the first and
 * doubles everything in the second: a window on a 3840x2160 panel at 200 %
 * reads as 7680x4320 on a 7680x4320 output, so it is "not on the panel" and
 * never weaves. The factor is therefore resolved per layout mode —
 * @ref u_wl_stage_to_device_scale — and THAT is what goes into
 * `u_wl_monitor::scale` for a stage-coordinate publisher.
 *
 * A client's own `wl_output.mode / xdg_output.logical_size` needs no such
 * care: Mutter reports the stage size as the logical size in both modes, so
 * that ratio already is the stage factor (measured: 1.0 in PHYSICAL at 200 %).
 *
 * Separate from u_wayland_geom.h on purpose: that header is shared byte-for-byte
 * with displayxr-common (test_apps/common/dxr_linux_window_aux_guard.cmake), and
 * this rule is the runtime's to apply at its boundary.
 *
 * Pure arithmetic, no Wayland or D-Bus dependency, no platform guard. Pinned by
 * `tests/tests_aux_wayland_geom.cpp`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Mutter's monitor layout mode: which space its stage coordinates are in (see
 * the file comment). The values are Mutter's own `layout-mode`
 * (org.gnome.Mutter.DisplayConfig.GetCurrentState).
 *
 * @ingroup aux_util
 */
enum u_wl_layout_mode
{
	//! Not known: no publisher said, and no compositor answered.
	U_WL_LAYOUT_MODE_UNKNOWN = 0,
	//! Stage = logical px; device = stage x the monitor scale.
	U_WL_LAYOUT_MODE_LOGICAL = 1,
	//! Stage = device px, whatever the monitor scale.
	U_WL_LAYOUT_MODE_PHYSICAL = 2,
};

/*!
 * The layout mode as the `window-geometry@displayxr.org` payload spells it
 * (`"layout_mode": "logical" | "physical"`). Anything else is UNKNOWN.
 *
 * @ingroup aux_util
 */
static inline enum u_wl_layout_mode
u_wl_layout_mode_from_string(const char *s)
{
	if (s == NULL) {
		return U_WL_LAYOUT_MODE_UNKNOWN;
	}
	// No <string.h> needed; the two spellings are short.
	const char *logical = "logical";
	const char *physical = "physical";
	size_t i = 0;
	for (i = 0; s[i] != '\0' && s[i] == logical[i]; i++) {
	}
	if (s[i] == '\0' && logical[i] == '\0') {
		return U_WL_LAYOUT_MODE_LOGICAL;
	}
	for (i = 0; s[i] != '\0' && s[i] == physical[i]; i++) {
	}
	if (s[i] == '\0' && physical[i] == '\0') {
		return U_WL_LAYOUT_MODE_PHYSICAL;
	}
	return U_WL_LAYOUT_MODE_UNKNOWN;
}

/*!
 * Device px per STAGE px on one monitor: the factor that converts a
 * stage-coordinate publisher's rects (the GNOME Shell extension's) to device
 * pixels.
 *
 * In order of authority:
 *   1. @p published_device_scale, when the publisher states it (extension
 *      version 11+: the scale of the stage view painting that monitor, which
 *      IS the factor in either layout mode);
 *   2. PHYSICAL layout: 1.0 — the stage already is device px, and the
 *      monitor scale must NOT be applied;
 *   3. otherwise (LOGICAL, or not known): @p monitor_scale. Unknown keeps the
 *      behaviour every earlier consumer had, which is right on every
 *      fractionally-scaled desktop (those are always LOGICAL).
 *
 * @return the factor, or 0.0 when none of the three is usable.
 *
 * @ingroup aux_util
 */
static inline double
u_wl_stage_to_device_scale(double monitor_scale, double published_device_scale, enum u_wl_layout_mode mode)
{
	if (published_device_scale > 0.0) {
		return published_device_scale;
	}
	if (mode == U_WL_LAYOUT_MODE_PHYSICAL) {
		return 1.0;
	}
	return monitor_scale > 0.0 ? monitor_scale : 0.0;
}

#ifdef __cplusplus
}
#endif
