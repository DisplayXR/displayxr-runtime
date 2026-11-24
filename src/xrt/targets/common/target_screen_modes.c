// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Every claimed screen's rendering-mode table (ADR-047 Amendment 10).
 * @ingroup targets_common
 */

#include "target_screen_modes.h"
#include "target_plugin_loader.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_screen.h"

#include "os/os_display_edid.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_rendering_modes.h"
#include "util/u_screen_modes.h"
#include "util/u_setting.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool
target_screen_modes_key_listed(const char *spec, const char *key)
{
	return u_screen_modes_key_listed(spec, key);
}

//! The registry entry of monitor @p id, NULL when none.
static const struct xrt_dp_registry_entry *
reg_entry(const struct xrt_dp_factory_registry *reg, uint64_t id)
{
	for (uint32_t i = 0; reg != NULL && i < reg->entry_count && i < XRT_DP_REGISTRY_MAX_ENTRIES; i++) {
		if (reg->entries[i].monitor_id == id) {
			return &reg->entries[i];
		}
	}
	return NULL;
}

//! The head device's list in array order, its slot 1 at index 1.
static void
copy_head_list(struct xrt_device *head, struct u_screen_mode_table *t, uint32_t px_w, uint32_t px_h)
{
	if (head == NULL || head->rendering_mode_count == 0) {
		return;
	}
	const uint32_t env = u_device_rendering_modes_envelope_count(head);
	const uint32_t n = env > head->rendering_mode_count ? env : head->rendering_mode_count;
	u_screen_modes_table_set_list(t, head->rendering_modes, n, n > 1 ? 1u : 0u, px_w, px_h);
}

uint32_t
target_screen_modes_build(const struct xrt_screen_list *list,
                          const struct xrt_dp_factory_registry *reg,
                          struct xrt_device *head,
                          struct u_screen_mode_table *out,
                          uint32_t cap)
{
	if (list == NULL || out == NULL) {
		return 0;
	}
	char exclude[512];
	const char *spec = u_setting_get_raw(TARGET_SCREEN_MODES_SIZE_EXCLUDE_ENV, exclude, sizeof(exclude), NULL);

	uint32_t n = 0;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < cap; i++) {
		const struct xrt_screen *s = &list->screens[i];
		struct u_screen_mode_table *t = &out[n];
		memset(t, 0, sizeof(*t));
		t->screen_id = s->id;
		snprintf(t->key, sizeof(t->key), "%s", s->key);
		snprintf(t->plugin_id, sizeof(t->plugin_id), "%s", s->plugin_id);

		// Pixels in the panel's axes: the plug-in's per-monitor answer (or the
		// system's), else the connector's current mode, else the desktop rect.
		uint32_t px_w = s->info.pixel_width;
		uint32_t px_h = s->info.pixel_height;
		const uint32_t cur_w = s->native_width > 0 ? s->native_width : s->desktop_width;
		const uint32_t cur_h = s->native_height > 0 ? s->native_height : s->desktop_height;
		if (px_w == 0 || px_h == 0) {
			px_w = cur_w;
			px_h = cur_h;
		}
		// Rotated: the current mode is the panel's size with the axes swapped.
		t->rotated = px_w != px_h && px_w == cur_h && px_h == cur_w;

		const bool is_default = (s->flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0;
		bool have = false;
		if (is_default) {
			copy_head_list(head, t, px_w, px_h);
			t->list_source = U_SCREEN_MODES_LIST_HEAD;
			have = t->count > 0;
		} else {
			const struct xrt_dp_registry_entry *e = reg_entry(reg, s->id);
			const struct xrt_plugin_iface *iface =
			    e != NULL ? (const struct xrt_plugin_iface *)e->owning_iface : NULL;
			if (xrt_plugin_iface_has_rendering_modes_for_monitor(iface)) {
				struct xrt_display_descriptor desc;
				struct os_display_edid_monitor mon;
				memset(&desc, 0, sizeof(desc));
				memset(&mon, 0, sizeof(mon));
				const bool have_record = target_plugin_get_monitor_record(s->id, &desc, &mon);
				struct xrt_display_physical phys;
				memset(&phys, 0, sizeof(phys));
				phys.struct_size = (uint32_t)sizeof(phys);
				phys.physical_width_mm = s->physical_width_mm;
				phys.physical_height_mm = s->physical_height_mm;
				phys.native_pixel_width = s->native_width;
				phys.native_pixel_height = s->native_height;
				struct xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES];
				memset(modes, 0, sizeof(modes));
				uint32_t count = 0;
				uint32_t def3d = 0;
				if (have_record &&
				    iface->get_rendering_modes_for_monitor(
				        (struct xrt_plugin_instance *)e->owning_instance, &desc, &phys, modes,
				        XRT_MAX_RENDERING_MODES, &count, &def3d) &&
				    count > 0) {
					u_screen_modes_table_set_list(t, modes, count, def3d, px_w, px_h);
					t->list_source = U_SCREEN_MODES_LIST_PLUGIN;
					have = true;
				}
			}
			if (!have) {
				copy_head_list(head, t, px_w, px_h);
				t->list_source = U_SCREEN_MODES_LIST_HEAD_COPY;
				have = t->count > 0;
			}
		}
		if (!have) {
			continue;
		}
		t->sized = !target_screen_modes_key_listed(spec, t->key);
		n++;
	}
	return n;
}

bool
target_screen_modes_choice(const char *key, char *out_name, size_t cap, uint32_t *out_source)
{
	enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
	if (key == NULL || key[0] == '\0' || u_setting_get_mode_3d_for_screen(key, out_name, cap, &src) == NULL) {
		return false;
	}
	if (out_source != NULL) {
		*out_source = src == U_SETTING_SOURCE_MACHINE ? XRT_STATUS_MODE_3D_SOURCE_MACHINE
		                                              : XRT_STATUS_MODE_3D_SOURCE_USER;
	}
	return true;
}

static void
apply_stored_choices(struct u_screen_mode_table *tables, uint32_t n, bool promote)
{
	for (uint32_t i = 0; i < n; i++) {
		struct u_screen_mode_table *t = &tables[i];
		if (t->list_source == U_SCREEN_MODES_LIST_HEAD) {
			continue; // the head device applies its own screen's choice
		}
		char name[U_SCREEN_MODES_NAME_MAX] = {0};
		uint32_t src = XRT_STATUS_MODE_3D_SOURCE_USER;
		if (!target_screen_modes_choice(t->key, name, sizeof(name), &src)) {
			name[0] = '\0';
		}
		if (promote && u_screen_modes_table_promote_choice(t, name)) {
			U_LOG_W(
			    "screen modes: 0x%016llx '%s': the stored 3D mode '%s' is declared only at start-up - "
			    "declared for this instance (ADR-047 Amendment 10)",
			    (unsigned long long)t->screen_id, t->key, name);
		}
		(void)u_screen_modes_table_apply_choice(t, name, src);
	}
}

static const char *
list_source_str(uint32_t s)
{
	switch (s) {
	case U_SCREEN_MODES_LIST_HEAD: return "the head device";
	case U_SCREEN_MODES_LIST_PLUGIN: return "its plug-in";
	case U_SCREEN_MODES_LIST_HEAD_COPY: return "the head device's list (its plug-in lists none)";
	default: return "nothing";
	}
}

void
target_screen_modes_publish(const struct xrt_screen_list *list,
                            const struct xrt_dp_factory_registry *reg,
                            struct xrt_device *head,
                            bool initial)
{
	struct u_screen_mode_table *tables = calloc(U_SCREEN_MODES_MAX_SCREENS, sizeof(*tables));
	struct u_screen_mode_table *before = calloc(U_SCREEN_MODES_MAX_SCREENS, sizeof(*before));
	if (tables == NULL || before == NULL) {
		free(tables);
		free(before);
		return;
	}
	const uint32_t n = target_screen_modes_build(list, reg, head, tables, U_SCREEN_MODES_MAX_SCREENS);
	apply_stored_choices(tables, n, initial);
	const uint32_t nb = u_screen_modes_get_all(before, U_SCREEN_MODES_MAX_SCREENS);
	u_screen_modes_set_choice_resolver(target_screen_modes_choice);
	u_screen_modes_publish(tables, n);

	for (uint32_t i = 0; i < n; i++) {
		const struct u_screen_mode_table *t = &tables[i];
		bool same = false;
		for (uint32_t k = 0; k < nb && !same; k++) {
			same = before[k].screen_id == t->screen_id && before[k].count == t->count &&
			       before[k].slot1 == t->slot1 && before[k].pixel_width == t->pixel_width &&
			       before[k].pixel_height == t->pixel_height && before[k].sized == t->sized;
		}
		if (same) {
			continue;
		}
		const struct xrt_rendering_mode *s1 = u_screen_modes_table_slot(t, 1);
		U_LOG_W(
		    "screen modes: 0x%016llx '%s' (%s) %ux%u%s - %u entries from %s; 3D mode '%s' (%u views, %ux%u "
		    "tiles, scale %.2f x %.2f)%s (ADR-047 Amendment 10)",
		    (unsigned long long)t->screen_id, t->key, t->plugin_id, t->pixel_width, t->pixel_height,
		    t->rotated ? " rotated" : "", t->count, list_source_str(t->list_source),
		    s1 != NULL ? s1->mode_name : "-", s1 != NULL ? s1->view_count : 0u,
		    s1 != NULL ? s1->tile_columns : 0u, s1 != NULL ? s1->tile_rows : 0u,
		    s1 != NULL ? (double)s1->view_scale_x : 0.0, s1 != NULL ? (double)s1->view_scale_y : 0.0,
		    t->sized ? "" : "; NOT sized for (" TARGET_SCREEN_MODES_SIZE_EXCLUDE_ENV ")");
	}
	free(tables);
	free(before);
}

void
target_screen_modes_fill_status(const struct xrt_screen_list *list,
                                const struct xrt_dp_factory_registry *reg,
                                struct xrt_device *head,
                                struct xrt_status_snapshot *out)
{
	if (out == NULL) {
		return;
	}
	uint32_t atlas_w = 0, atlas_h = 0;
	(void)u_screen_modes_get_instance_worst(&atlas_w, &atlas_h);
	struct u_screen_mode_table *local = NULL;
	uint32_t local_n = 0;
	struct u_screen_mode_table *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return;
	}
	for (uint32_t i = 0; i < out->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
		struct xrt_status_screen *sc = &out->screens[i];
		if (sc->roles.runtime_default && sc->mode_3d.valid) {
			continue; // the head device's row
		}
		bool have = u_screen_modes_get(sc->id, t);
		if (have && t->list_source == U_SCREEN_MODES_LIST_HEAD && sc->mode_3d.valid) {
			continue;
		}
		if (!have && list != NULL) {
			if (local == NULL) {
				local = calloc(U_SCREEN_MODES_MAX_SCREENS, sizeof(*local));
				if (local != NULL) {
					local_n = target_screen_modes_build(list, reg, head, local,
					                                    U_SCREEN_MODES_MAX_SCREENS);
					apply_stored_choices(local, local_n, false);
				}
			}
			for (uint32_t k = 0; local != NULL && k < local_n && !have; k++) {
				if (local[k].screen_id == sc->id) {
					*t = local[k];
					have = true;
				}
			}
		}
		if (!have) {
			continue;
		}
		const bool fits = u_screen_modes_table_fits(t, atlas_w, atlas_h) || !t->sized;
		u_screen_modes_fill_status(t, fits, &sc->mode_3d);
	}
	free(local);
	free(t);
}
