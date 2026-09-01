// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Resolve the desktop rect and stable device name of the monitor a
 *         given virtual-desktop point falls on.
 * @ingroup aux_os
 *
 * This is deliberately narrower than @ref os_display_edid.h: no SetupAPI, no
 * EDID, no registry. Just `MonitorFromPoint` + `GetMonitorInfoW` on Windows,
 * so it cannot fail the way the EDID correlation can. The runtime uses it to
 * turn the vendor plug-in's panel origin
 * (`xrt_system_compositor_info::display_screen_left/top`) into the full
 * monitor geometry that `XR_DXR_display_info` publishes to apps, so a client
 * can place its window on the 3D panel instead of the primary monitor.
 *
 * See runtime issue #1301 and docs/specs/extensions/XR_DXR_display_info.md.
 *
 * Other platforms: stub that reports failure. The per-platform equivalents
 * (macOS `CGDirectDisplayID`, Linux XRandR output) are the follow-on.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Size of the device-name buffer, in bytes, including the NUL.
 *
 * Windows `CCHDEVICENAME` is 32 wide chars (`\\.\DISPLAY1`), which is well
 * inside this; the headroom is for the platform identifiers that land later
 * (a macOS display UUID is 37 bytes).
 */
#define OS_DISPLAY_DEVICE_NAME_SIZE 128

/*!
 * Desktop geometry and identity of one monitor.
 */
struct os_display_desktop_info
{
	//! Monitor left edge in virtual-desktop coordinates. Signed: monitors
	//! left of the primary are negative.
	int32_t left;

	//! Monitor top edge in virtual-desktop coordinates. Signed: monitors
	//! above the primary are negative.
	int32_t top;

	//! Monitor width in physical pixels, in the mode it is running now.
	//! This is NOT necessarily the panel's native resolution.
	uint32_t width;

	//! Monitor height in physical pixels, in the mode it is running now.
	uint32_t height;

	//! True if this is the primary monitor (the one the desktop origin is on).
	bool is_primary;

	//! Stable OS device name, NUL-terminated UTF-8. Windows: the GDI name,
	//! e.g. `\\.\DISPLAY1`. Empty string when the platform has no equivalent.
	char device_name[OS_DISPLAY_DEVICE_NAME_SIZE];

	/*!
	 * @name The same monitor measured in the CALLER's DPI space
	 *
	 * Identical to @ref width / @ref height when the calling process is
	 * per-monitor-DPI-aware, and divided by the monitor's scale factor when it
	 * is not (a 3840x2160 panel at 250% reports 1536x864 to an unaware
	 * process).
	 *
	 * These exist for exactly ONE job: comparing against another value the
	 * same process obtained through a DPI-sensitive API — notably the panel
	 * dimensions a display-processor plug-in reports, since a plug-in is a DLL
	 * and inherits its host's awareness. Comparing such a value against
	 * @ref width / @ref height would disagree purely because of DPI and say
	 * nothing about whether the monitors match.
	 *
	 * NEVER publish these or place a window with them. Use @ref width /
	 * @ref height for anything that leaves the process.
	 * @{
	 */
	uint32_t width_in_caller_dpi;
	uint32_t height_in_caller_dpi;
	/*! @} */
};

/*!
 * Resolve the monitor containing a virtual-desktop point.
 *
 * (0, 0) resolves to the primary monitor, which is both the "panel is at the
 * desktop origin" and the "plug-in expressed no preference" reading — they
 * want the same answer, so the caller need not distinguish them.
 *
 * A point that falls in no monitor (a gap in a ragged arrangement, or a stale
 * origin after a hotplug) resolves to the nearest monitor rather than failing,
 * so the caller always gets a placeable rect.
 *
 * @param x Point X in virtual-desktop coordinates.
 * @param y Point Y in virtual-desktop coordinates.
 * @param[out] out_info Receives the monitor geometry. Zeroed on failure.
 *
 * @return true on success. false when the platform has no implementation, or
 * when the Win32 query failed — callers should treat the geometry as unknown
 * and publish zeros rather than guessing.
 */
bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info);

#ifdef __cplusplus
}
#endif
