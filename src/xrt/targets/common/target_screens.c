// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen list from the per-monitor DP registry (multi-screen M1).
 * @ingroup targets_common
 */

#include "target_screens.h"
#include "target_plugin_loader.h"

#include "xrt/xrt_plugin.h"

#include "os/os_display_edid.h"
#include "os/os_display_desktop.h"
#include "util/u_screen_info.h"

#include <stdio.h>
#include <string.h>


void
target_screens_system_from_info(const struct xrt_system_compositor_info *info, struct target_screens_system *out)
{
	memset(out, 0, sizeof(*out));
	if (info == NULL) {
		return;
	}
	out->info_valid = info->display_width_m > 0.0f && info->display_height_m > 0.0f;
	out->info.width_m = info->display_width_m;
	out->info.height_m = info->display_height_m;
	out->info.nominal_viewer_x_m = info->nominal_viewer_x_m;
	out->info.nominal_viewer_y_m = info->nominal_viewer_y_m;
	out->info.nominal_viewer_z_m = info->nominal_viewer_z_m;
	out->info.recommended_view_scale_x = info->recommended_view_scale_x;
	out->info.recommended_view_scale_y = info->recommended_view_scale_y;
	out->info.pixel_width = info->display_pixel_width;
	out->info.pixel_height = info->display_pixel_height;
	out->info.supported_eye_tracking_modes = info->supported_eye_tracking_modes;
	out->info.default_eye_tracking_mode = info->default_eye_tracking_mode;
	out->info.source = XRT_SCREEN_INFO_SOURCE_SYSTEM;

	out->desktop_left = info->display_screen_left;
	out->desktop_top = info->display_screen_top;
	out->desktop_width = info->display_desktop_width;
	out->desktop_height = info->display_desktop_height;
	out->native_width = info->display_desktop_native_width;
	out->native_height = info->display_desktop_native_height;
	out->desktop_scale = info->display_desktop_scale;
	out->is_primary = info->display_is_primary;
	(void)snprintf(out->device_name, sizeof(out->device_name), "%.*s", (int)(sizeof(out->device_name) - 1),
	               info->display_device_name);
}

//! The compositor scale of the desktop monitor named @p name (0 = unknown).
static float
desktop_scale_for(const struct os_display_desktop_info *mons, uint32_t count, const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return 0.0f;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(mons[i].device_name, name) == 0) {
			return (float)mons[i].scale;
		}
	}
	return 0.0f;
}

static void
finish_flags(struct xrt_screen *s, bool is_primary, bool is_default)
{
	s->flags = 0;
	if (is_primary) {
		s->flags |= XRT_SCREEN_FLAG_PRIMARY;
	}
	if (s->info.supported_eye_tracking_modes != 0) {
		s->flags |= XRT_SCREEN_FLAG_TRACKED;
	}
	if (is_default) {
		s->flags |= XRT_SCREEN_FLAG_SYSTEM_DEFAULT;
	}
}

static void
build_from_entry(const struct xrt_dp_registry_entry *e,
                 bool is_default,
                 bool use_sys_placement,
                 const struct target_screens_system *sys,
                 const struct os_display_desktop_info *desk,
                 uint32_t desk_count,
                 struct xrt_screen *s)
{
	memset(s, 0, sizeof(*s));
	s->id = e->monitor_id;
	s->confidence = e->confidence;
	s->desktop_left = e->screen_left;
	s->desktop_top = e->screen_top;
	s->desktop_width = e->pixel_width;
	s->desktop_height = e->pixel_height;
	(void)snprintf(s->plugin_id, sizeof(s->plugin_id), "%s", e->plugin_id);

	struct xrt_display_descriptor desc;
	struct os_display_edid_monitor mon;
	memset(&desc, 0, sizeof(desc));
	memset(&mon, 0, sizeof(mon));
	const bool have_record = target_plugin_get_monitor_record(e->monitor_id, &desc, &mon);
	bool is_primary = false;
	if (have_record) {
		s->native_width = mon.native_width;
		s->native_height = mon.native_height;
		s->physical_width_mm = mon.physical_width_mm;
		s->physical_height_mm = mon.physical_height_mm;
		(void)snprintf(s->device_name, sizeof(s->device_name), "%s",
		               mon.output_name[0] != '\0' ? mon.output_name : mon.connector);
		is_primary = mon.is_primary;
	}
	s->desktop_scale = desktop_scale_for(desk, desk_count, s->device_name);

	struct u_screen_info_inputs in;
	memset(&in, 0, sizeof(in));
	in.desc = have_record ? &desc : NULL;
	in.physical.struct_size = (uint32_t)sizeof(in.physical);
	in.physical.physical_width_mm = s->physical_width_mm;
	in.physical.physical_height_mm = s->physical_height_mm;
	in.physical.native_pixel_width = s->native_width;
	in.physical.native_pixel_height = s->native_height;
	in.desktop_width = s->desktop_width;
	in.desktop_height = s->desktop_height;
	in.iface = (const struct xrt_plugin_iface *)e->owning_iface;
	in.inst = (struct xrt_plugin_instance *)e->owning_instance;
	in.is_system_default = is_default;
	in.system_info = (sys != NULL && sys->info_valid) ? &sys->info : NULL;
	u_screen_info_resolve(&in, &s->info);

	// The system-default screen's placement is the one the runtime resolved
	// for the panel (#1301) — what XrDisplayDesktopInfoDXR reports — but only
	// when that resolution landed on THIS monitor (use_sys_placement), so a
	// screen never carries one monitor's id and another monitor's rect.
	if (is_default && use_sys_placement) {
		s->desktop_left = sys->desktop_left;
		s->desktop_top = sys->desktop_top;
		s->desktop_width = sys->desktop_width;
		s->desktop_height = sys->desktop_height;
		if (sys->native_width > 0 && sys->native_height > 0) {
			s->native_width = sys->native_width;
			s->native_height = sys->native_height;
		}
		if (sys->desktop_scale > 0.0f) {
			s->desktop_scale = sys->desktop_scale;
		}
		if (sys->device_name[0] != '\0') {
			(void)snprintf(s->device_name, sizeof(s->device_name), "%s", sys->device_name);
		}
		is_primary = sys->is_primary;
	}

	finish_flags(s, is_primary, is_default);
}

const struct xrt_dp_registry_entry *
target_screens_pick_default(const struct xrt_dp_factory_registry *reg,
                            const char *active_plugin_id,
                            const struct target_screens_system *sys,
                            bool *out_matches_sys)
{
	if (out_matches_sys != NULL) {
		*out_matches_sys = false;
	}
	if (reg == NULL || reg->entry_count == 0) {
		return NULL;
	}

	// The monitor the #1301 resolver placed the system panel on: the entry
	// whose desktop rect contains the resolved origin. Prefer one the active
	// plug-in won (two entries never overlap on a sane desktop, but a
	// DRM-only record sits at (0,0) next to a real one).
	if (sys != NULL && sys->desktop_width > 0 && sys->desktop_height > 0) {
		const struct xrt_dp_registry_entry *hit = NULL;
		for (uint32_t i = 0; i < reg->entry_count; i++) {
			const struct xrt_dp_registry_entry *e = &reg->entries[i];
			const int64_t l = e->screen_left;
			const int64_t t = e->screen_top;
			if (sys->desktop_left >= l && sys->desktop_left < l + (int64_t)e->pixel_width &&
			    sys->desktop_top >= t && sys->desktop_top < t + (int64_t)e->pixel_height) {
				const bool active = active_plugin_id != NULL && active_plugin_id[0] != '\0' &&
				                    strcmp(e->plugin_id, active_plugin_id) == 0;
				if (hit == NULL || active) {
					hit = e;
				}
				if (active) {
					break;
				}
			}
		}
		if (hit != NULL) {
			if (out_matches_sys != NULL) {
				*out_matches_sys = true;
			}
			return hit;
		}
	}

	// Nothing contains it (or nothing was resolved): the registry's own
	// "don't care which monitor" pick, without the system's placement.
	return xrt_dp_registry_primary_entry(reg, active_plugin_id);
}

void
target_screens_build(const struct xrt_dp_factory_registry *reg,
                     const char *active_plugin_id,
                     const struct target_screens_system *sys,
                     struct xrt_screen_list *out)
{
	memset(out, 0, sizeof(*out));

	if (reg == NULL || reg->entry_count == 0) {
		// No monitor enumeration on this platform (or it failed): one
		// synthesized screen, the system's, when the system knows it.
		if (sys == NULL || !sys->info_valid) {
			return;
		}
		struct xrt_screen *s = &out->screens[0];
		s->id = XRT_SCREEN_ID_SYNTHETIC_DEFAULT;
		s->desktop_left = sys->desktop_left;
		s->desktop_top = sys->desktop_top;
		s->desktop_width = sys->desktop_width;
		s->desktop_height = sys->desktop_height;
		s->native_width = sys->native_width;
		s->native_height = sys->native_height;
		s->desktop_scale = sys->desktop_scale;
		(void)snprintf(s->plugin_id, sizeof(s->plugin_id), "%s",
		               active_plugin_id != NULL ? active_plugin_id : "");
		(void)snprintf(s->device_name, sizeof(s->device_name), "%s", sys->device_name);
		s->info = sys->info;
		s->info.source = XRT_SCREEN_INFO_SOURCE_SYSTEM;
		finish_flags(s, sys->is_primary, true);
		out->count = 1;
		return;
	}

	struct os_display_desktop_info desk[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	memset(desk, 0, sizeof(desk));
	const uint32_t desk_count = os_display_desktop_enumerate(desk, OS_DISPLAY_DESKTOP_MAX_MONITORS);

	bool def_matches_sys = false;
	const struct xrt_dp_registry_entry *def = target_screens_pick_default(reg, active_plugin_id, sys, &def_matches_sys);

	uint32_t n = 0;
	if (def != NULL) {
		build_from_entry(def, true, def_matches_sys, sys, desk, desk_count, &out->screens[n++]);
	}
	for (uint32_t i = 0; i < reg->entry_count && n < XRT_SCREEN_LIST_MAX; i++) {
		const struct xrt_dp_registry_entry *e = &reg->entries[i];
		if (e == def) {
			continue;
		}
		build_from_entry(e, false, false, sys, desk, desk_count, &out->screens[n++]);
	}
	out->count = n;
}
