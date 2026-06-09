// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displays` subcommand — enumerate connected displays via EDID.
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

#include "os/os_display_edid.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_plugin.h"
#include "target_plugin_loader.h"

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
 * `displays --claims`: enumerate EDID, ask the registered plug-ins which
 * monitors they claim, and print the resolved monitor→plug-in registry
 * (#69 / ADR-015). Loads the active plug-in(s) — same exposure as
 * `selftest`/`info`. Plain `displays` stays vendor-blind (no plug-in load).
 */
static int
cli_cmd_displays_claims(const struct os_display_edid_list *list, bool json)
{
	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t dn = target_plugin_build_descriptors(list, descs, XRT_DP_REGISTRY_MAX_ENTRIES);

	struct xrt_dp_factory_registry reg = {0};
	target_plugin_resolve_displays(descs, dn, &reg);

	if (json) {
		cJSON *root = cJSON_CreateObject();
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
		PT("monitor 0x%016llx  %ux%u @ (%d,%d)\n", (unsigned long long)e->monitor_id, e->pixel_width,
		   e->pixel_height, e->screen_left, e->screen_top);
		PT("    plug-in='%s'  confidence=%s  apis=%s%s%s\n", e->plugin_id, confidence_label(e->confidence),
		   apis, e->serial[0] != '\0' ? "  serial=" : "", e->serial);
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
		PT("Note: EDID enumeration is Windows-only; other platforms report none.\n");
		return 0;
	}
	for (uint32_t i = 0; i < list.count; i++) {
		const struct os_display_edid_monitor *m = &list.monitors[i];
		char pnp[4];
		pnp_code(m->manufacturer_id, pnp);
		PT("[%u] %s %04X  %ux%u @ %uHz  pos (%d,%d)%s\n", i, pnp, m->product_id, m->pixel_width,
		   m->pixel_height, m->refresh_hz, m->screen_left, m->screen_top, m->is_primary ? "  [primary]" : "");
	}
	return 0;
}
