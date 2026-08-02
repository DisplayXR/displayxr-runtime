// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `input` subcommand — list registered input providers (ADR-034).
 *
 *   input list [--json]   Enumerate registered input-provider plug-ins
 *                         with ProbeOrder + which one the loader would
 *                         select. Mirror of `dp list` for the second
 *                         plug-in type; enumeration reads the discovery
 *                         root without loading any provider DLL.
 *
 * Issue #823.
 *
 * @author David Fattal
 */

#include "cli_common.h"

#include "target_input_plugin_loader.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

#define MAX_PROVIDERS 16

#define P(...) printf(__VA_ARGS__)


/*!
 * Predict which provider the loader would select: the lowest ProbeOrder
 * among enabled entries (first wins on ties). Returns the index into
 * @p list, or -1.
 */
static int
predict_active(const struct target_input_plugin_desc *list, int n)
{
	int best = -1;
	for (int i = 0; i < n; i++) {
		if (!list[i].enabled) {
			continue;
		}
		if (best < 0 || list[i].probe_order < list[best].probe_order) {
			best = i;
		}
	}
	return best;
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_input_plugin_desc list[MAX_PROVIDERS];
	int n = target_input_plugin_enumerate(list, MAX_PROVIDERS);

	bool force_qwerty = target_input_plugin_get_force_qwerty();
	int active = predict_active(list, n);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddBoolToObject(root, "force_qwerty", force_qwerty);
		cJSON *arr = cJSON_AddArrayToObject(root, "providers");
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "id", list[i].id);
			cJSON_AddStringToObject(p, "display_name", list[i].display_name);
			cJSON_AddStringToObject(p, "vendor", list[i].vendor);
			cJSON_AddStringToObject(p, "version", list[i].version);
			cJSON_AddNumberToObject(p, "probe_order", (double)list[i].probe_order);
			cJSON_AddStringToObject(p, "binary_path", list[i].binary_path);
			cJSON_AddBoolToObject(p, "enabled", list[i].enabled);
			cJSON_AddBoolToObject(p, "active", i == active && !force_qwerty);
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

	P(" :: Registered input providers\n");
	if (n == 0) {
		P("\t(none — discovery root absent or empty; qwerty keeps the hand roles)\n");
		return 0;
	}
	if (force_qwerty) {
		P("\tForceQwerty override: SET — providers are skipped, qwerty claims the hand roles.\n");
	}
	for (int i = 0; i < n; i++) {
		P("\t%s id='%s' name='%s' ProbeOrder=%u%s\n", (i == active && !force_qwerty) ? "* " : "  ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order,
		  list[i].enabled ? "" : " [disabled]");
		P("\t     %s\n", list[i].binary_path);
	}
	P("\t('*' = the provider the loader would select.)\n");
	return 0;
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli input <list>\n");
	P("  list [--json]   Enumerate registered input providers + predicted active one.\n");
	return 1;
}

int
cli_cmd_input(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	P("error: unknown 'input' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
