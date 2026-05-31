// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `dp` subcommand — list registered display processors and set or
 *         clear the non-destructive `PreferredPlugin` override.
 *
 *   dp list [--json]   Enumerate registered display-processor plug-ins with
 *                      their ProbeOrder + which one the loader would select.
 *   dp use <id>        Write the PreferredPlugin override (the loader tries
 *                      <id> before the ProbeOrder sort). Requires a service
 *                      restart / fresh process to take effect.
 *   dp reset           Clear the override (restore normal ProbeOrder).
 *
 * Vendor-neutral throughout (ADR-019): enumeration reads the discovery root
 * without loading any plug-in DLL, so no vendor symbols are touched.
 *
 * @author David Fattal
 */

#include "cli_common.h"

#include "target_plugin_loader.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

#define MAX_DPS 16

#define P(...) printf(__VA_ARGS__)


/*!
 * Predict which plug-in the loader would select: the PreferredPlugin
 * override if it names a registered plug-in, otherwise the lowest
 * ProbeOrder (first wins on ties). Returns the index into @p list, or -1.
 */
static int
predict_active(const struct target_plugin_desc *list, int n, const char *preferred)
{
	if (preferred != NULL && preferred[0] != '\0') {
		for (int i = 0; i < n; i++) {
			if (strcmp(list[i].id, preferred) == 0) {
				return i;
			}
		}
	}
	int best = -1;
	for (int i = 0; i < n; i++) {
		if (best < 0 || list[i].probe_order < list[best].probe_order) {
			best = i;
		}
	}
	return best;
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_plugin_desc list[MAX_DPS];
	int n = target_plugin_enumerate(list, MAX_DPS);

	char preferred[64] = {0};
	bool have_pref = target_plugin_get_preferred(preferred, sizeof(preferred));
	int active = predict_active(list, n, have_pref ? preferred : NULL);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		if (have_pref) {
			cJSON_AddStringToObject(root, "preferred", preferred);
		} else {
			cJSON_AddNullToObject(root, "preferred");
		}
		cJSON *arr = cJSON_AddArrayToObject(root, "plugins");
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "id", list[i].id);
			cJSON_AddStringToObject(p, "display_name", list[i].display_name);
			cJSON_AddStringToObject(p, "vendor", list[i].vendor);
			cJSON_AddStringToObject(p, "version", list[i].version);
			cJSON_AddNumberToObject(p, "probe_order", (double)list[i].probe_order);
			cJSON_AddStringToObject(p, "binary_path", list[i].binary_path);
			cJSON_AddBoolToObject(p, "active", i == active);
			cJSON_AddBoolToObject(p, "preferred", have_pref && strcmp(list[i].id, preferred) == 0);
			cJSON_AddItemToArray(arr, p);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return 0;
	}

	P(" :: Registered display processors\n");
	if (n == 0) {
		P("\t(none — discovery root absent or empty)\n");
		return 0;
	}
	P("\tPreferredPlugin override: %s\n", have_pref ? preferred : "<unset>");
	for (int i = 0; i < n; i++) {
		P("\t%s%s id='%s' name='%s' ProbeOrder=%u\n", i == active ? "* " : "  ",
		  (have_pref && strcmp(list[i].id, preferred) == 0) ? "[preferred]" : "           ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order);
		P("\t       %s\n", list[i].binary_path);
	}
	P("\t('*' = the plug-in the loader would select.)\n");
	return 0;
}

static int
cmd_use(const char *id)
{
	xrt_result_t xret = target_plugin_set_preferred(id);
	if (xret == XRT_SUCCESS) {
		P("PreferredPlugin set to '%s'.\n", id);
		P("Takes effect for processes started after this write — restart the DisplayXR\n");
		P("service (or launch a fresh app) for a running session to pick it up.\n");
		return 0;
	}
	if (xret == XRT_ERROR_NOT_AUTHORIZED) {
		P("FAIL: writing the override was denied — run from an elevated terminal (HKLM needs admin).\n");
		return 1;
	}
	P("FAIL: could not write the PreferredPlugin override (xret=%d).\n", (int)xret);
	return 1;
}

static int
cmd_reset(void)
{
	xrt_result_t xret = target_plugin_clear_preferred();
	if (xret == XRT_SUCCESS) {
		P("PreferredPlugin override cleared — normal ProbeOrder discovery restored.\n");
		P("Restart the DisplayXR service (or launch a fresh app) for a running session to pick it up.\n");
		return 0;
	}
	if (xret == XRT_ERROR_NOT_AUTHORIZED) {
		P("FAIL: clearing the override was denied — run from an elevated terminal (HKLM needs admin).\n");
		return 1;
	}
	P("FAIL: could not clear the PreferredPlugin override (xret=%d).\n", (int)xret);
	return 1;
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli dp <list|use <id>|reset>\n");
	P("  list [--json]   Enumerate registered display processors + active/preferred state.\n");
	P("  use <id>        Set the PreferredPlugin override (e.g. 'sim-display', 'leia-sr').\n");
	P("  reset           Clear the override (restore normal ProbeOrder discovery).\n");
	return 1;
}

int
cli_cmd_dp(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(argv[2], "use") == 0) {
		if (argc < 4) {
			P("error: 'dp use' needs a plug-in id.\n\n");
			return print_usage();
		}
		return cmd_use(argv[3]);
	}
	if (strcmp(argv[2], "reset") == 0) {
		return cmd_reset();
	}
	P("error: unknown 'dp' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
