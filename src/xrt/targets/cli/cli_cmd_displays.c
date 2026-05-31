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

int
cli_cmd_displays(int argc, const char **argv)
{
	struct os_display_edid_list list = {0};
	os_display_edid_enumerate(&list);

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
