// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen display-info derivation (multi-screen M1).
 *
 * Turns one registry monitor into its @ref xrt_screen_info. Header-only and
 * free of loader state so the rules are unit-testable
 * (`tests/tests_aux_screen_info.cpp`):
 *
 *  1. The system-default screen reports the system-level info verbatim — it
 *     IS what `xrGetSystemProperties` reports, so `xrEnumerateDisplaysDXR`
 *     and `XrDisplayInfoDXR` can never disagree about it.
 *  2. Otherwise the owning plug-in's `get_display_info_for_monitor` slot,
 *     when it implements one and answers.
 *  3. Otherwise derived from EDID: physical size from the mm, native pixels,
 *     view scale 1, no eye tracking, and the nominal viewer centred at the
 *     distance that gives this screen the SAME vertical FOV the system panel's
 *     nominal viewer has (z = h * z_ref / h_ref). With no reference panel the
 *     viewer sits at @ref U_SCREEN_INFO_DEFAULT_VIEWER_Z_M.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_screen.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Viewer distance used when neither the screen nor a reference panel gives one.
#define U_SCREEN_INFO_DEFAULT_VIEWER_Z_M 0.6f

/*!
 * Everything @ref u_screen_info_resolve looks at for one screen.
 */
struct u_screen_info_inputs
{
	//! The registry descriptor of the screen (for the plug-in slot); may be NULL.
	const struct xrt_display_descriptor *desc;

	//! EDID mm + the connector's device mode (0 = unknown); also handed to the slot.
	struct xrt_display_physical physical;

	//! Current desktop size, used when the device mode is unknown.
	uint32_t desktop_width;
	uint32_t desktop_height;

	//! The plug-in that won the screen, and its instance (both may be NULL).
	const struct xrt_plugin_iface *iface;
	struct xrt_plugin_instance *inst;

	//! This is the system-default screen.
	bool is_system_default;

	//! The system-level info (what XrSystemProperties reports), or NULL if
	//! the active plug-in has not described its panel. Copied for the
	//! system-default screen; the vertical-FOV reference for derived ones.
	const struct xrt_screen_info *system_info;
};

/*!
 * Build a @ref xrt_screen_info from the plug-in display-info struct (the
 * shape both `get_display_info` and `get_display_info_for_monitor` return).
 * A zero recommended view scale (the plug-in's "let the runtime derive") is
 * left 0 for the caller to resolve.
 */
static inline void
u_screen_info_from_plugin(const struct xrt_plugin_display_info *pdi, uint32_t source, struct xrt_screen_info *out)
{
	memset(out, 0, sizeof(*out));
	out->width_m = pdi->display_width_m;
	out->height_m = pdi->display_height_m;
	out->nominal_viewer_x_m = pdi->nominal_viewer_x_m;
	out->nominal_viewer_y_m = pdi->nominal_viewer_y_m;
	out->nominal_viewer_z_m = pdi->nominal_viewer_z_m;
	out->recommended_view_scale_x = pdi->recommended_view_scale_x;
	out->recommended_view_scale_y = pdi->recommended_view_scale_y;
	out->pixel_width = pdi->display_pixel_width;
	out->pixel_height = pdi->display_pixel_height;
	out->supported_eye_tracking_modes = pdi->supported_eye_tracking_modes;
	out->default_eye_tracking_mode = pdi->default_eye_tracking_mode;
	out->source = source;
}

/*!
 * Rule 3: derive a screen's info from EDID + the reference panel.
 */
static inline void
u_screen_info_derive(const struct u_screen_info_inputs *in, struct xrt_screen_info *out)
{
	memset(out, 0, sizeof(*out));
	out->source = XRT_SCREEN_INFO_SOURCE_DERIVED;

	out->width_m = (float)in->physical.physical_width_mm / 1000.0f;
	out->height_m = (float)in->physical.physical_height_mm / 1000.0f;

	if (in->physical.native_pixel_width > 0 && in->physical.native_pixel_height > 0) {
		out->pixel_width = in->physical.native_pixel_width;
		out->pixel_height = in->physical.native_pixel_height;
	} else {
		out->pixel_width = in->desktop_width;
		out->pixel_height = in->desktop_height;
	}

	out->recommended_view_scale_x = 1.0f;
	out->recommended_view_scale_y = 1.0f;

	// Same vertical FOV as the reference panel's nominal viewer: the viewer
	// distance scales with the screen height. x/y stay 0 (centred).
	const struct xrt_screen_info *ref = in->system_info;
	float z = U_SCREEN_INFO_DEFAULT_VIEWER_Z_M;
	if (ref != NULL && ref->nominal_viewer_z_m > 0.0f) {
		z = ref->nominal_viewer_z_m;
		if (ref->height_m > 0.0f && out->height_m > 0.0f) {
			z = out->height_m * (ref->nominal_viewer_z_m / ref->height_m);
		}
	}
	out->nominal_viewer_z_m = z;

	// No eye tracking: the runtime knows of no tracker for this screen.
	out->supported_eye_tracking_modes = 0;
	out->default_eye_tracking_mode = 0;
}

/*!
 * Resolve one screen's info by the rules in the file comment.
 *
 * @return the @ref xrt_screen_info_source that answered (also in out->source).
 */
static inline enum xrt_screen_info_source
u_screen_info_resolve(const struct u_screen_info_inputs *in, struct xrt_screen_info *out)
{
	// Rule 1: the system-default screen is the system info.
	if (in->is_system_default && in->system_info != NULL) {
		*out = *in->system_info;
		out->source = XRT_SCREEN_INFO_SOURCE_SYSTEM;
		return XRT_SCREEN_INFO_SOURCE_SYSTEM;
	}

	// Rule 2: the owning plug-in describes the monitor.
	if (in->desc != NULL && xrt_plugin_iface_has_display_info_for_monitor(in->iface)) {
		struct xrt_plugin_display_info pdi;
		memset(&pdi, 0, sizeof(pdi));
		pdi.struct_size = (uint32_t)sizeof(pdi);
		struct xrt_display_physical phys = in->physical;
		phys.struct_size = (uint32_t)sizeof(phys);
		if (in->iface->get_display_info_for_monitor(in->inst, in->desc, &phys, &pdi)) {
			u_screen_info_from_plugin(&pdi, XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR, out);
			// "0 = let the runtime derive". The system derives its baseline
			// from the head device's mode table, which describes the system
			// panel only; per-screen mode tables do not exist yet, so a
			// non-default screen renders at native (1.0).
			if (!(out->recommended_view_scale_x > 0.0f) || !(out->recommended_view_scale_y > 0.0f)) {
				out->recommended_view_scale_x = 1.0f;
				out->recommended_view_scale_y = 1.0f;
			}
			if (out->pixel_width == 0 || out->pixel_height == 0) {
				struct xrt_screen_info d;
				u_screen_info_derive(in, &d);
				out->pixel_width = d.pixel_width;
				out->pixel_height = d.pixel_height;
			}
			return XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR;
		}
	}

	// Rule 3: EDID-derived defaults.
	u_screen_info_derive(in, out);
	if (!(out->width_m > 0.0f) && out->pixel_width == 0) {
		out->source = XRT_SCREEN_INFO_SOURCE_NONE;
	}
	return (enum xrt_screen_info_source)out->source;
}

//! Short name of a @ref xrt_screen_info_source, for logs and the CLI. Never NULL.
static inline const char *
u_screen_info_source_str(uint32_t source)
{
	switch (source) {
	case XRT_SCREEN_INFO_SOURCE_SYSTEM: return "system";
	case XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR: return "plugin-monitor";
	case XRT_SCREEN_INFO_SOURCE_DERIVED: return "derived";
	case XRT_SCREEN_INFO_SOURCE_NONE:
	default: return "none";
	}
}

#ifdef __cplusplus
}
#endif
