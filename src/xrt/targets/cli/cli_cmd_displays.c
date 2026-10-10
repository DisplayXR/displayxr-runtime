// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displays` subcommand — enumerate connected displays via EDID
 * (Windows: SetupAPI; desktop Linux: RandR joined to DRM sysfs).
 *
 * Vendor-neutral list of every connected monitor (manufacturer/product,
 * resolution, position, primary), independent of which display processor is
 * active. Built on the runtime's `os_display_edid` helper (aux_os) — no vendor
 * symbols (ADR-019). This is the read-only diagnostic surface from #380; the
 * per-display DP routing/validation lives in #69 / ADR-015.
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h"

#include "os/os_display_edid.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "target_plugin_loader.h"
#include "util/u_setting.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdio.h>

#define P(...) printf(__VA_ARGS__)
#define PT(...) printf("\t" __VA_ARGS__)

/*!
 * Decode an EDID manufacturer id (stored little-endian; the EDID spec packs it
 * big-endian as three 5-bit letters) into its 3-char PNP code, e.g. "AUO".
 * Writes "???" if the decoded letters aren't A–Z.
 */
static void
pnp_code(uint16_t mfr_raw, char out[4])
{
	uint16_t v = (uint16_t)((mfr_raw >> 8) | (mfr_raw << 8)); // -> big-endian spec value
	int c0 = ((v >> 10) & 0x1F) + 'A' - 1;
	int c1 = ((v >> 5) & 0x1F) + 'A' - 1;
	int c2 = (v & 0x1F) + 'A' - 1;
	out[0] = (c0 >= 'A' && c0 <= 'Z') ? (char)c0 : '?';
	out[1] = (c1 >= 'A' && c1 <= 'Z') ? (char)c1 : '?';
	out[2] = (c2 >= 'A' && c2 <= 'Z') ? (char)c2 : '?';
	out[3] = '\0';
}

//! Human label for an xrt_display_claim_confidence value.
static const char *
confidence_label(uint32_t c)
{
	switch (c) {
	case (uint32_t)XRT_DISPLAY_CLAIM_FALLBACK: return "FALLBACK";
	case (uint32_t)XRT_DISPLAY_CLAIM_EDID: return "EDID";
	case (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED: return "VERIFIED";
	default: return "?";
	}
}

//! Decode a supported-API bitmask into a "vk|d3d11|gl" style string.
static void
apis_to_str(const struct xrt_dp_registry_entry *e, char *out, size_t cap)
{
	out[0] = '\0';
	size_t len = 0;
	const struct {
		void *fn;
		const char *name;
	} apis[] = {
	    {e->dp_factory_vk, "vk"},       {e->dp_factory_d3d11, "d3d11"}, {e->dp_factory_d3d12, "d3d12"},
	    {e->dp_factory_gl, "gl"},       {e->dp_factory_metal, "metal"},
	};
	for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]); i++) {
		if (apis[i].fn == NULL) {
			continue;
		}
		int n = snprintf(out + len, cap - len, "%s%s", len > 0 ? "|" : "", apis[i].name);
		if (n > 0 && (size_t)n < cap - len) {
			len += (size_t)n;
		}
	}
	if (out[0] == '\0') {
		snprintf(out, cap, "(none)");
	}
}

/*!
 * Off-Windows, bring the system up headlessly (no compositor) and tear it down
 * again, so the active plug-in's `get_display_info` has been read — the
 * builder notes it with `target_plugin_note_active_panel`, which places a
 * plug-in-without-`probe_displays` back-compat claim on its panel's monitor
 * exactly as the runtime does at instance create. Windows keeps the plain
 * loader path (its back-compat claim is the primary monitor either way).
 */
static void
note_active_panel_headless(void)
{
#ifndef XRT_OS_WINDOWS
	struct cli_query_handles h = {0};
	if (xrt_instance_create(NULL, &h.xi) == XRT_SUCCESS) {
		(void)xrt_instance_create_system(h.xi, &h.xsys, &h.xsysd, &h.xso, NULL);
	}
	cli_query_teardown(&h);
#endif
}

//! The enumerated record behind a registry entry, by monitor id.
static const struct os_display_edid_monitor *
monitor_for_id(const struct os_display_edid_list *list,
               const struct xrt_display_descriptor *descs,
               uint32_t dn,
               uint64_t monitor_id)
{
	for (uint32_t i = 0; i < dn && i < list->count; i++) {
		if (descs[i].monitor_id == monitor_id) {
			return &list->monitors[i];
		}
	}
	return NULL;
}

/*!
 * `displays --claims`: enumerate EDID, ask the registered plug-ins which
 * monitors they claim, and print the resolved monitor→plug-in registry
 * (#69 / ADR-015). Loads the registered plug-ins — same exposure as
 * `selftest`/`info`. Plain `displays` stays vendor-blind (no plug-in load).
 */
static int
cli_cmd_displays_claims(const struct os_display_edid_list *list, bool json)
{
	note_active_panel_headless();

	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t dn = target_plugin_build_descriptors(list, descs, XRT_DP_REGISTRY_MAX_ENTRIES);

	struct xrt_dp_factory_registry reg = {0};
	target_plugin_resolve_displays(descs, dn, &reg);

	if (json) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddNumberToObject(root, "schema", (double)CLI_JSON_SCHEMA);
		cJSON_AddNumberToObject(root, "monitor_count", (double)dn);
		cJSON_AddNumberToObject(root, "claimed_count", (double)reg.entry_count);
		cJSON *arr = cJSON_AddArrayToObject(root, "claims");
		for (uint32_t i = 0; i < reg.entry_count; i++) {
			const struct xrt_dp_registry_entry *e = &reg.entries[i];
			char idhex[19];
			snprintf(idhex, sizeof(idhex), "0x%016llx", (unsigned long long)e->monitor_id);
			char apis[64];
			apis_to_str(e, apis, sizeof(apis));
			cJSON *c = cJSON_CreateObject();
			cJSON_AddStringToObject(c, "monitor_id", idhex);
			cJSON_AddStringToObject(c, "plugin_id", e->plugin_id);
			cJSON_AddStringToObject(c, "confidence", confidence_label(e->confidence));
			cJSON_AddNumberToObject(c, "confidence_value", (double)e->confidence);
			cJSON_AddStringToObject(c, "supported_apis", apis);
			cJSON_AddStringToObject(c, "serial", e->serial);
			cJSON_AddNumberToObject(c, "pixel_width", (double)e->pixel_width);
			cJSON_AddNumberToObject(c, "pixel_height", (double)e->pixel_height);
			cJSON_AddNumberToObject(c, "screen_left", (double)e->screen_left);
			cJSON_AddNumberToObject(c, "screen_top", (double)e->screen_top);
			// Display dashboard phase 7: the stable screen key and the
			// per-screen preference (additive; schema stays 1).
			char key[64] = {0};
			if (target_plugin_get_monitor_key(e->monitor_id, key, sizeof(key), NULL, 0)) {
				cJSON_AddStringToObject(c, "key", key);
			} else {
				cJSON_AddNullToObject(c, "key");
			}
			cJSON_AddBoolToObject(c, "forced", e->forced);
			if (e->forced) {
				cJSON_AddStringToObject(c, "forced_source",
				                        u_setting_source_str((enum u_setting_source)e->forced_source));
			} else {
				cJSON_AddNullToObject(c, "forced_source");
			}
			if (e->preferred_plugin[0] != '\0') {
				cJSON_AddStringToObject(c, "preferred_plugin", e->preferred_plugin);
			} else {
				cJSON_AddNullToObject(c, "preferred_plugin");
			}
			const struct os_display_edid_monitor *m = monitor_for_id(list, descs, dn, e->monitor_id);
			if (m != NULL) {
				char pnp[4];
				pnp_code(m->manufacturer_id, pnp);
				char prod[8];
				snprintf(prod, sizeof(prod), "%04X", m->product_id);
				cJSON_AddStringToObject(c, "manufacturer", pnp);
				cJSON_AddStringToObject(c, "product", prod);
				cJSON_AddNumberToObject(c, "edid_serial", (double)m->serial_number);
				cJSON_AddNumberToObject(c, "physical_width_mm", (double)m->physical_width_mm);
				cJSON_AddNumberToObject(c, "physical_height_mm", (double)m->physical_height_mm);
				cJSON_AddStringToObject(c, "connector", m->connector);
				cJSON_AddStringToObject(c, "output_name", m->output_name);
				cJSON_AddStringToObject(c, "display_name", m->display_name);
				cJSON_AddBoolToObject(c, "primary", m->is_primary);
			}
			cJSON_AddItemToArray(arr, c);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return 0;
	}

	P(" :: Per-display DP claims (resolved registry, #69)\n");
	if (reg.entry_count == 0) {
		PT("(no monitor was claimed by any registered plug-in; %u monitor(s) enumerated)\n", dn);
		return 0;
	}
	for (uint32_t i = 0; i < reg.entry_count; i++) {
		const struct xrt_dp_registry_entry *e = &reg.entries[i];
		char apis[64];
		apis_to_str(e, apis, sizeof(apis));
		const struct os_display_edid_monitor *m = monitor_for_id(list, descs, dn, e->monitor_id);
		PT("monitor 0x%016llx  %ux%u @ (%d,%d)%s\n", (unsigned long long)e->monitor_id, e->pixel_width,
		   e->pixel_height, e->screen_left, e->screen_top,
		   (m != NULL && m->origin_unknown) ? " (origin unknown)" : "");
		if (m != NULL) {
			char pnp[4];
			pnp_code(m->manufacturer_id, pnp);
			PT("    %s %04X serial=0x%08X  %ux%u mm%s%s%s%s%s\n", pnp, m->product_id, m->serial_number,
			   m->physical_width_mm, m->physical_height_mm, m->output_name[0] != '\0' ? "  output=" : "",
			   m->output_name, m->connector[0] != '\0' ? "  connector=" : "", m->connector,
			   m->is_primary ? "  [primary]" : "");
			if (m->display_name[0] != '\0' || m->native_width > 0) {
				PT("    '%s'  native %ux%u\n", m->display_name, m->native_width, m->native_height);
			}
		}
		PT("    plug-in='%s'  confidence=%s  apis=%s%s%s\n", e->plugin_id, confidence_label(e->confidence),
		   apis, e->serial[0] != '\0' ? "  serial=" : "", e->serial);
		char key[64] = {0};
		if (target_plugin_get_monitor_key(e->monitor_id, key, sizeof(key), NULL, 0)) {
			PT("    key='%s'%s%s%s%s\n", key, e->forced ? "  forced by a per-screen preference (" : "",
			   e->forced ? u_setting_source_str((enum u_setting_source)e->forced_source) : "",
			   e->forced ? ")" : "",
			   (!e->forced && e->preferred_plugin[0] != '\0') ? "  (per-screen preference ignored)" : "");
		}
	}
	if (reg.entry_count < dn) {
		PT("(%u of %u monitor(s) claimed by no plug-in)\n", dn - reg.entry_count, dn);
	}
	return 0;
}

int
cli_cmd_displays(int argc, const char **argv)
{
	struct os_display_edid_list list = {0};
	os_display_edid_enumerate(&list);

	if (cli_has_flag(argc, argv, "--claims")) {
		return cli_cmd_displays_claims(&list, cli_has_flag(argc, argv, "--json"));
	}

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddNumberToObject(root, "schema", (double)CLI_JSON_SCHEMA);
		cJSON_AddNumberToObject(root, "count", (double)list.count);
		cJSON *arr = cJSON_AddArrayToObject(root, "displays");
		for (uint32_t i = 0; i < list.count; i++) {
			const struct os_display_edid_monitor *m = &list.monitors[i];
			char pnp[4];
			pnp_code(m->manufacturer_id, pnp);
			char prod[8];
			snprintf(prod, sizeof(prod), "%04X", m->product_id);
			cJSON *d = cJSON_CreateObject();
			cJSON_AddNumberToObject(d, "index", (double)i);
			cJSON_AddStringToObject(d, "manufacturer", pnp);
			cJSON_AddStringToObject(d, "product", prod);
			cJSON_AddNumberToObject(d, "manufacturer_id_raw", (double)m->manufacturer_id);
			cJSON_AddNumberToObject(d, "product_id_raw", (double)m->product_id);
			cJSON_AddNumberToObject(d, "pixel_width", (double)m->pixel_width);
			cJSON_AddNumberToObject(d, "pixel_height", (double)m->pixel_height);
			cJSON_AddNumberToObject(d, "refresh_hz", (double)m->refresh_hz);
			cJSON_AddNumberToObject(d, "screen_left", (double)m->screen_left);
			cJSON_AddNumberToObject(d, "screen_top", (double)m->screen_top);
			cJSON_AddBoolToObject(d, "primary", m->is_primary);
			cJSON_AddNumberToObject(d, "edid_serial", (double)m->serial_number);
			cJSON_AddNumberToObject(d, "physical_width_mm", (double)m->physical_width_mm);
			cJSON_AddNumberToObject(d, "physical_height_mm", (double)m->physical_height_mm);
			cJSON_AddNumberToObject(d, "native_width", (double)m->native_width);
			cJSON_AddNumberToObject(d, "native_height", (double)m->native_height);
			cJSON_AddStringToObject(d, "connector", m->connector);
			cJSON_AddStringToObject(d, "output_name", m->output_name);
			cJSON_AddStringToObject(d, "display_name", m->display_name);
			cJSON_AddStringToObject(d, "join", os_display_edid_join_str(m->join));
			cJSON_AddBoolToObject(d, "origin_unknown", m->origin_unknown);
			cJSON_AddItemToArray(arr, d);
		}
		cJSON *diag = cJSON_AddObjectToObject(root, "diag");
		cJSON_AddNumberToObject(diag, "error", (double)list.diag_error);
		cJSON_AddNumberToObject(diag, "gdi_count", (double)list.diag_gdi_count);
		cJSON_AddNumberToObject(diag, "setupdi_count", (double)list.diag_setupdi_count);
		cJSON_AddNumberToObject(diag, "edid_read_count", (double)list.diag_edid_read_count);
		cJSON_AddNumberToObject(diag, "displayconfig_count", (double)list.diag_displayconfig_count);
		cJSON_AddNumberToObject(diag, "win32_error", (double)list.diag_win32_error);

		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return 0;
	}

	P(" :: Connected displays (EDID)\n");
	if (list.count == 0) {
		PT("(none enumerated; diag_error=%d gdi=%u setupdi=%u edid_reads=%u win32err=%u)\n",
		   (int)list.diag_error, list.diag_gdi_count, list.diag_setupdi_count, list.diag_edid_read_count,
		   list.diag_win32_error);
		PT("Note: EDID enumeration is implemented on Windows, desktop Linux and macOS; other platforms "
		   "report none.\n");
		return 0;
	}
	for (uint32_t i = 0; i < list.count; i++) {
		const struct os_display_edid_monitor *m = &list.monitors[i];
		char pnp[4];
		pnp_code(m->manufacturer_id, pnp);
		PT("[%u] %s %04X  %ux%u @ %uHz  pos (%d,%d)%s%s\n", i, pnp, m->product_id, m->pixel_width,
		   m->pixel_height, m->refresh_hz, m->screen_left, m->screen_top, m->is_primary ? "  [primary]" : "",
		   m->origin_unknown ? "  (origin unknown)" : "");
		if (m->connector[0] != '\0' || m->output_name[0] != '\0') {
			PT("    serial=0x%08X  %ux%u mm  native %ux%u  output='%s' connector='%s' join=%s\n",
			   m->serial_number, m->physical_width_mm, m->physical_height_mm, m->native_width,
			   m->native_height, m->output_name, m->connector, os_display_edid_join_str(m->join));
		}
		if (m->display_name[0] != '\0') {
			PT("    name='%s'\n", m->display_name);
		}
	}
	return 0;
}
