// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which real monitor the simulated panel adopts (see the header).
 * @ingroup drv_sim_display
 */

#include "sim_display_monitor_geometry.h"

#include <string.h>

bool
sim_display_connector_is_internal(const char *connector)
{
	if (connector == NULL || connector[0] == '\0') {
		return false;
	}
	// Drop the "cardN-" prefix the kernel puts on sysfs connector names.
	const char *name = connector;
	if (strncmp(name, "card", 4) == 0) {
		const char *dash = strchr(name, '-');
		if (dash != NULL) {
			name = dash + 1;
		}
	}
	return strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0 || strncmp(name, "DSI", 3) == 0;
}

static bool
has_pixels(const struct os_display_edid_monitor *m)
{
	return (m->native_width > 0 && m->native_height > 0) || (m->pixel_width > 0 && m->pixel_height > 0);
}

bool
sim_display_pick_monitor_geometry(const struct os_display_edid_list *list, struct sim_display_monitor_geometry *out)
{
	if (list == NULL || out == NULL) {
		return false;
	}
	uint32_t n = list->count;
	if (n > OS_DISPLAY_EDID_MAX_MONITORS) {
		n = OS_DISPLAY_EDID_MAX_MONITORS;
	}

	int32_t pick = -1;
	const char *reason = NULL;
	for (uint32_t i = 0; i < n && pick < 0; i++) {
		if (list->monitors[i].is_primary && has_pixels(&list->monitors[i])) {
			pick = (int32_t)i;
			reason = "primary";
		}
	}
	for (uint32_t i = 0; i < n && pick < 0; i++) {
		const struct os_display_edid_monitor *m = &list->monitors[i];
		if (has_pixels(m) && (sim_display_connector_is_internal(m->connector) ||
		                      sim_display_connector_is_internal(m->output_name))) {
			pick = (int32_t)i;
			reason = "internal";
		}
	}
	for (uint32_t i = 0; i < n && pick < 0; i++) {
		if (has_pixels(&list->monitors[i])) {
			pick = (int32_t)i;
			reason = "first";
		}
	}
	if (pick < 0) {
		return false;
	}

	const struct os_display_edid_monitor *m = &list->monitors[pick];
	memset(out, 0, sizeof(*out));
	out->index = (uint32_t)pick;
	out->reason = reason;
	if (m->native_width > 0 && m->native_height > 0) {
		out->pixel_width = m->native_width;
		out->pixel_height = m->native_height;
		out->pixel_is_native = true;
	} else {
		out->pixel_width = m->pixel_width;
		out->pixel_height = m->pixel_height;
	}
	if (m->physical_width_mm > 0 && m->physical_height_mm > 0) {
		out->physical_width_mm = m->physical_width_mm;
		out->physical_height_mm = m->physical_height_mm;
	}
	return true;
}
