// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux desktop: pair each RandR monitor with its DRM/KMS connector
 *         and solve for the X11 global scale (see os_display_scale.h).
 * @ingroup aux_os
 *
 * The device mode behind each RandR monitor is joined in by the enumeration
 * itself (`os_display_connector_linux.c`, #1831): Mutter's current mode when the
 * session answers DisplayConfig, else DRM sysfs, where the X11 size counts as
 * the native one when it is itself one of the connector's modes and the
 * preferred mode does otherwise — so it can only err toward "device pixels"
 * (the pre-existing assumption), never toward a false "quantized".
 */

#include "os/os_display_scale.h"

#include <stdio.h>
#include <string.h>

bool
os_display_scale_query(struct os_display_scale_report *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->verdict.culprit = -1;

	struct os_display_desktop_info mons[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	const uint32_t mon_count = os_display_desktop_enumerate(mons, OS_DISPLAY_DESKTOP_MAX_MONITORS);
	if (mon_count == 0) {
		return false;
	}

	int64_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
	for (uint32_t i = 0; i < mon_count; i++) {
		const struct os_display_desktop_info *m = &mons[i];
		struct u_x11_output_sizes *o = &out->outputs[out->output_count++];
		snprintf(o->name, sizeof(o->name), "%s", m->device_name);
		o->x11_w = m->width;
		o->x11_h = m->height;

		const int64_t l = m->left, t = m->top, r = l + (int64_t)m->width, b = t + (int64_t)m->height;
		if (i == 0 || l < min_x)
			min_x = l;
		if (i == 0 || t < min_y)
			min_y = t;
		if (i == 0 || r > max_x)
			max_x = r;
		if (i == 0 || b > max_y)
			max_y = b;

		// The device mode comes from the connector join every monitor
		// enumeration now carries (os_display_connector_linux.c): Mutter's
		// current mode when it answers, else DRM sysfs.
		if (m->native_width > 0 && m->native_height > 0) {
			o->native_w = m->native_width;
			o->native_h = m->native_height;
			out->native_sum_w += o->native_w;
			out->drm_available = true;
		}
	}
	out->root_w = (uint32_t)(max_x - min_x);
	out->root_h = (uint32_t)(max_y - min_y);

	u_x11_scale_solve(out->outputs, out->output_count, &out->verdict);
	return true;
}

const char *
os_display_scale_state_str(enum u_x11_scale_state state)
{
	switch (state) {
	case U_X11_SCALE_DEVICE_PIXELS: return "device pixels";
	case U_X11_SCALE_QUANTIZED: return "quantized";
	case U_X11_SCALE_UNKNOWN:
	default: return "unknown";
	}
}
