// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `perf` subcommand — read and write the persisted performance
 *         settings the runtime resolves inside each app's own process (#1252).
 *
 *   perf list [--json]        Every allow-listed lever: resolved value + source.
 *   perf set <name> <value>   Write one lever to the per-user store.
 *   perf reset [<name>]       Clear one lever, or all of them.
 *   perf path                 Print the per-user store's path.
 *
 * The Control Panel drives this rather than writing the file itself — one
 * writer, the same shape as `dp use` / `dp reset`, and the GUI stays free of
 * runtime knowledge (#378).
 *
 * **Writes go to the PER-USER store, which needs no elevation.** The machine
 * default (`HKLM\Software\DisplayXR\Settings`) is an admin/installer concern
 * and is deliberately not writable from here — a user-facing control that
 * silently needs a UAC prompt is worse than one that cannot reach the machine
 * store at all.
 *
 * The environment still outranks both stores, so a value set here never
 * overrides a launcher, a `.bat` or a perf-ladder arm. `perf list` says which
 * source won, so that is visible rather than surprising.
 */

#include "cli_common.h"

#include "util/u_setting.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

#define P(...) printf(__VA_ARGS__)


static int
cmd_list(int argc, const char **argv)
{
	char path[512] = {0};
	u_setting_user_path(path, sizeof(path));
	char written[32] = {0};
	const bool have_written = u_setting_user_written(written, sizeof(written)) != NULL;

	const uint32_t n = u_setting_managed_count();

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "user_file", path);
		if (have_written) {
			cJSON_AddStringToObject(root, "user_written", written);
		} else {
			cJSON_AddNullToObject(root, "user_written");
		}
		cJSON *arr = cJSON_AddArrayToObject(root, "levers");
		for (uint32_t i = 0; i < n; i++) {
			const char *name = u_setting_managed_name(i);
			char buf[128];
			enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
			const char *val = u_setting_get_raw(name, buf, sizeof(buf), &src);

			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "name", name);
			if (val != NULL && val[0] != '\0') {
				cJSON_AddStringToObject(o, "value", val);
			} else {
				cJSON_AddNullToObject(o, "value");
			}
			cJSON_AddStringToObject(o, "source", u_setting_source_str(src));
			cJSON_AddItemToArray(arr, o);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return 0;
	}

	P(" :: Performance settings (#1252)\n");
	P("\tstore: %s%s\n", path[0] != '\0' ? path : "<unavailable>", have_written ? "" : "  (not written yet)");
	if (have_written) {
		P("\tlast written: %s\n", written);
	}
	for (uint32_t i = 0; i < n; i++) {
		const char *name = u_setting_managed_name(i);
		char buf[128];
		enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
		const char *val = u_setting_get_raw(name, buf, sizeof(buf), &src);
		P("\t%-24s = %-12s [%s]\n", name, (val != NULL && val[0] != '\0') ? val : "<unset>",
		  u_setting_source_str(src));
	}
	P("\t(env outranks the stores, so a lever sourced 'env' ignores what is stored.)\n");
	return 0;
}

static int
cmd_set(const char *name, const char *value)
{
	if (!u_setting_is_managed(name)) {
		P("FAIL: '%s' is not a settable performance option.\n", name);
		P("Settable options (displayxr-cli perf list):\n");
		for (uint32_t i = 0; i < u_setting_managed_count(); i++) {
			P("  %s\n", u_setting_managed_name(i));
		}
		return 1;
	}
	if (!u_setting_user_set(name, value)) {
		P("FAIL: could not write the per-user settings file.\n");
		return 1;
	}

	P("%s = %s (per-user store).\n", name, value);

	// Tell the truth about what just happened rather than implying it took
	// effect: the value only reaches processes started from here on, and an
	// environment variable would still outrank it.
	char buf[128];
	enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
	u_setting_get_raw(name, buf, sizeof(buf), &src);
	if (src == U_SETTING_SOURCE_ENV) {
		P("NOTE: this process's environment sets %s, which OUTRANKS the store —\n", name);
		P("      a process launched from an environment that sets it will not see your value.\n");
	}
	P("Takes effect for apps started after this write; restart the DisplayXR service\n");
	P("(or relaunch your app) for a running session to pick it up.\n");
	return 0;
}

static int
cmd_reset(const char *name)
{
	if (name == NULL) {
		if (!u_setting_user_clear_all()) {
			P("FAIL: could not clear the per-user settings file.\n");
			return 1;
		}
		P("All performance settings cleared — the runtime's tuned defaults are back.\n");
	} else {
		if (!u_setting_is_managed(name)) {
			P("FAIL: '%s' is not a settable performance option.\n", name);
			return 1;
		}
		if (!u_setting_user_clear(name)) {
			P("FAIL: could not update the per-user settings file.\n");
			return 1;
		}
		P("%s cleared.\n", name);
	}
	P("Takes effect for apps started after this write.\n");
	return 0;
}

static int
cmd_path(void)
{
	char path[512];
	if (!u_setting_user_path(path, sizeof(path))) {
		P("FAIL: could not resolve the per-user settings path.\n");
		return 1;
	}
	P("%s\n", path);
	return 0;
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli perf <list|set <name> <value>|reset [name]|path>\n");
	P("  list [--json]        Allow-listed levers with their resolved value + source.\n");
	P("  set <name> <value>   Write one lever to the per-user store (no admin needed).\n");
	P("  reset [<name>]       Clear one lever, or every one.\n");
	P("  path                 Print the per-user store's path.\n");
	return 1;
}

int
cli_cmd_perf(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(argv[2], "set") == 0) {
		if (argc < 5) {
			P("error: 'perf set' needs a name and a value.\n\n");
			return print_usage();
		}
		return cmd_set(argv[3], argv[4]);
	}
	if (strcmp(argv[2], "reset") == 0) {
		return cmd_reset(argc >= 4 ? argv[3] : NULL);
	}
	if (strcmp(argv[2], "path") == 0) {
		return cmd_path();
	}
	P("error: unknown 'perf' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
