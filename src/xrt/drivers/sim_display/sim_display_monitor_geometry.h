// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which real monitor the simulated panel adopts, and its geometry.
 *
 * Desktop Linux (and any platform whose `os_display_edid_enumerate` fills the
 * connector's device mode and EDID millimetres): the simulated panel takes the
 * pixel size and physical size of a real monitor instead of the fixed
 * 1920x1080 / 0.344x0.194 m defaults, the same fix #369 made on Windows with
 * `EnumDisplaySettingsW`. Without it the service window (which presents 1:1
 * DEVICE pixels) and the display-info pixel density disagree, and the scene
 * renders zoomed in on a panel that is neither 1080p nor 34 cm wide.
 *
 * Pure: no I/O, no logging, no environment — the caller enumerates and applies
 * the env overrides, so host tests can pin the choice on fixtures.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "os/os_display_edid.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The monitor the simulated panel adopts.
 */
struct sim_display_monitor_geometry
{
	//! Index into the enumerated list.
	uint32_t index;
	//! The connector's device (native) mode, else the desktop mode; > 0.
	uint32_t pixel_width;
	uint32_t pixel_height;
	//! Whether @ref pixel_width came from the connector's device mode.
	bool pixel_is_native;
	//! EDID physical size, millimetres; both 0 when unknown.
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	//! Why this monitor: "primary", "internal", "first".
	const char *reason;
};

/*!
 * Is @p connector an internal panel connector ("card1-eDP-1", "LVDS-1",
 * "DSI-1")? Used to find the laptop screen when no monitor is primary (no X
 * server: DRM-only records carry no primary flag).
 */
bool
sim_display_connector_is_internal(const char *connector);

/*!
 * Pick the monitor the simulated panel adopts from @p list: the primary one;
 * else (no placement source, so nothing is primary) the internal panel; else
 * the first. Its pixels are the connector's device mode when known — the
 * service window presents device pixels 1:1, so a scaled desktop's logical
 * size would be wrong — else the desktop mode. A monitor with no pixel size
 * at all is skipped.
 *
 * @return false when @p list is NULL or holds no monitor with a pixel size.
 */
bool
sim_display_pick_monitor_geometry(const struct os_display_edid_list *list, struct sim_display_monitor_geometry *out);

#ifdef __cplusplus
}
#endif
