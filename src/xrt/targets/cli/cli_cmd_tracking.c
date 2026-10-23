// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `tracking` subcommand — viewer-tracking providers (ADR-054 §4.1, N6 Part 2):
 *         the ADR-054 component verbs, mirroring `input`.
 *
 *   tracking list [--json]               Registered providers: ProbeOrder, Enabled (+ source),
 *                                        provenance, the PreferredPlugin override and the row
 *                                        the loader would rank first ('*'). Loads nothing.
 *   tracking enable | disable <id> [--machine]
 *                                        `Enabled` on the row: HKCU by default, HKLM with
 *                                        --machine; HKCU wins unless `Policy\Lock`. POSIX: the
 *                                        manifest's `plugin.enabled` is the switch (said so).
 *   tracking use | prefer <id> [--machine] `PreferredPlugin` under the TrackingProviders root;
 *                                        POSIX: the per-user `tracking-preferred` file.
 *   tracking reset [--machine]           Clear it.
 *   tracking options <id> [--json] / set <id> <KEY> <value> [--machine] / unset <id> <KEY>
 *                                        The provider's settings (`DXR_TRACKING_<ID>_<KEY>` in
 *                                        the hosting process's environment always wins).
 *
 * Pickup: the viewer source and the composed `XR_DXR_display_info` advertisement are
 * static per instance, so every write applies at the next app launch (in-process).
 *
 * Contract: docs/specs/runtime/tracking-provider-discovery.md.
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_component_options.h"
#include "cli_input_predict.h" // cli_input_id_valid: the same id rule

#include "xrt/xrt_config_os.h"

#include "target_component_loader.h"
#include "target_tracking_plugin_loader.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define MAX_PROVIDERS 16
#define TRACKING_REG_ROOT "Software\\DisplayXR\\TrackingProviders"
#define P(...) printf(__VA_ARGS__)

static bool
provider_registered(const char *id)
{
	struct target_tracking_plugin_desc list[MAX_PROVIDERS];
	const int n = target_tracking_plugin_enumerate(list, MAX_PROVIDERS);
	for (int i = 0; i < n; i++) {
		if (strcmp(list[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

//! The row the loader would rank first: the preferred row when enabled, else the first enabled by ProbeOrder.
static int
predict_first(const struct target_tracking_plugin_desc *list, int n, const char *preferred)
{
	if (preferred != NULL && preferred[0] != '\0') {
		for (int i = 0; i < n; i++) {
			if (list[i].enabled && strcmp(list[i].id, preferred) == 0) {
				return i;
			}
		}
	}
	for (int i = 0; i < n; i++) {
		if (list[i].enabled) {
			return i;
		}
	}
	return -1;
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_tracking_plugin_desc list[MAX_PROVIDERS];
	const int n = target_tracking_plugin_enumerate(list, MAX_PROVIDERS);
	char preferred[64] = {0};
	const bool have_pref = target_tracking_plugin_get_preferred(preferred, sizeof(preferred));
	const int first = predict_first(list, n, have_pref ? preferred : NULL);
	const bool lock = target_component_policy_lock(target_tracking_plugin_component_class());

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		if (have_pref) {
			cJSON_AddStringToObject(root, "preferred", preferred);
		} else {
			cJSON_AddNullToObject(root, "preferred");
		}
		cJSON *pol = cJSON_AddObjectToObject(root, "policy");
		cJSON_AddBoolToObject(pol, "policy_lock", lock);
		cJSON_AddBoolToObject(root, "disabled_by_env", target_tracking_plugin_disabled_by_env());
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
			cJSON_AddStringToObject(p, "enabled_source", list[i].enabled_source);
			cJSON_AddStringToObject(p, "provenance", target_component_provenance_str(list[i].provenance));
			cJSON_AddStringToObject(p, "provenance_reason", list[i].provenance_reason);
			cJSON_AddStringToObject(p, "signer", list[i].signer);
			cJSON_AddBoolToObject(p, "preferred", have_pref && strcmp(list[i].id, preferred) == 0);
			cJSON_AddBoolToObject(p, "first", i == first);
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

	P(" :: Registered viewer-tracking providers\n");
	P("\tPreferredPlugin override: %s\n", have_pref ? preferred : "<unset>");
	if (have_pref && !provider_registered(preferred)) {
		P("\t  (not a registered provider — the loader ignores it)\n");
	}
	if (target_tracking_plugin_disabled_by_env()) {
		P("\tDXR_TRACKING_PROVIDERS=0 in this environment — no provider is loaded in this process.\n");
	}
	if (lock) {
		P("\tPolicy\\Lock: SET (HKLM) — per-user Enabled values and the per-user PreferredPlugin are "
		  "ignored.\n");
	}
	if (n == 0) {
		P("\t(none — the views of a display that does not track stay on the nominal viewer)\n");
		return 0;
	}
	P("\t  %-20s %-24s %10s  %-18s %-10s %s\n", "id", "name", "ProbeOrder", "Enabled", "Preferred", "Provenance");
	for (int i = 0; i < n; i++) {
		char enabled[32];
		snprintf(enabled, sizeof(enabled), "%s (%s)", list[i].enabled ? "yes" : "no", list[i].enabled_source);
		P("\t%s %-20s %-24s %10u  %-18s %-10s %s\n", i == first ? "*" : " ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order, enabled,
		  (have_pref && strcmp(list[i].id, preferred) == 0) ? "yes" : "-",
		  target_component_provenance_str(list[i].provenance));
		P("\t    %s\n", list[i].binary_path);
	}
	P("\t('*' = the provider the loader would rank first. It serves a screen only when that display does not\n");
	P("\t track the viewer itself and declares EXTERNAL_OK — see 'displayxr-cli status' for the viewer source.)\n");
	return 0;
}


/*
 *
 * Store writes.
 *
 */

enum store_write
{
	STORE_WRITE_OK,
	STORE_WRITE_DENIED,
	STORE_WRITE_UNSUPPORTED,
	STORE_WRITE_FAILED,
};

#ifdef XRT_OS_WINDOWS

static enum store_write
reg_open_for_write(bool machine, const char *sub, bool create, HKEY *out_key)
{
	char path[256];
	if (sub != NULL && sub[0] != '\0') {
		snprintf(path, sizeof(path), "%s\\%s", TRACKING_REG_ROOT, sub);
	} else {
		snprintf(path, sizeof(path), "%s", TRACKING_REG_ROOT);
	}
	const HKEY hive = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	const REGSAM sam = KEY_SET_VALUE | (machine ? KEY_WOW64_64KEY : 0);
	LSTATUS rc;
	if (create) {
		rc = RegCreateKeyExA(hive, path, 0, NULL, 0, sam, NULL, out_key, NULL);
	} else {
		rc = RegOpenKeyExA(hive, path, 0, sam, out_key);
		if (rc == ERROR_FILE_NOT_FOUND) {
			*out_key = NULL;
			return STORE_WRITE_OK;
		}
	}
	if (rc != ERROR_SUCCESS) {
		*out_key = NULL;
		return rc == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

static enum store_write
store_set_enabled(const char *id, bool enabled, bool machine)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, id, true, &key);
	if (r != STORE_WRITE_OK) {
		return r;
	}
	DWORD d = enabled ? 1u : 0u;
	const LSTATUS w = RegSetValueExA(key, "Enabled", 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
	RegCloseKey(key);
	return w == ERROR_SUCCESS ? STORE_WRITE_OK
	                          : (w == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED);
}

static enum store_write
store_set_preferred(const char *id, bool machine)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, NULL, true, &key);
	if (r != STORE_WRITE_OK) {
		return r;
	}
	const LSTATUS w = RegSetValueExA(key, "PreferredPlugin", 0, REG_SZ, (const BYTE *)id, (DWORD)strlen(id) + 1);
	RegCloseKey(key);
	return w == ERROR_SUCCESS ? STORE_WRITE_OK
	                          : (w == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED);
}

static enum store_write
store_clear_preferred(bool machine)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, NULL, false, &key);
	if (r != STORE_WRITE_OK || key == NULL) {
		return r;
	}
	const LSTATUS rc = RegDeleteValueA(key, "PreferredPlugin");
	RegCloseKey(key);
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
		return rc == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

#else /* POSIX / Android */

static enum store_write
store_set_enabled(const char *id, bool enabled, bool machine)
{
	(void)id;
	(void)enabled;
	(void)machine;
	return STORE_WRITE_UNSUPPORTED;
}

static bool
preferred_file_path(char *out, size_t cap, bool create_dir)
{
#ifdef XRT_OS_ANDROID
	(void)create_dir;
	out[0] = '\0';
	return false;
#else
	char dir[512];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	if (create_dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s", dir);
		for (char *p = path + 1; *p != '\0'; p++) {
			if (*p == '/') {
				*p = '\0';
				(void)mkdir(path, 0755);
				*p = '/';
			}
		}
		(void)mkdir(path, 0755);
	}
	snprintf(out, cap, "%s/tracking-preferred", dir);
	return true;
#endif
}

static enum store_write
store_set_preferred(const char *id, bool machine)
{
	if (machine) {
		return STORE_WRITE_UNSUPPORTED;
	}
	char path[600];
	if (!preferred_file_path(path, sizeof(path), true)) {
		return STORE_WRITE_UNSUPPORTED;
	}
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return errno == EACCES ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	fprintf(f, "%s\n", id);
	fclose(f);
	return STORE_WRITE_OK;
}

static enum store_write
store_clear_preferred(bool machine)
{
	if (machine) {
		return STORE_WRITE_UNSUPPORTED;
	}
	char path[600];
	if (!preferred_file_path(path, sizeof(path), false)) {
		return STORE_WRITE_OK;
	}
	if (remove(path) != 0 && errno != ENOENT) {
		return errno == EACCES ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

#endif

static int
write_result(enum store_write r, const char *what, bool machine)
{
	switch (r) {
	case STORE_WRITE_OK: return 0;
	case STORE_WRITE_DENIED:
		P("FAIL: %s was denied — run from an elevated terminal (HKLM needs admin).\n", what);
		return 1;
	case STORE_WRITE_UNSUPPORTED:
		if (machine) {
			P("FAIL: %s — there is no machine-wide store on this platform (drop --machine).\n", what);
		} else {
			P("FAIL: %s — not supported on this platform: there is no per-user Enabled store.\n"
			  "      Set \"enabled\": false in the provider's manifest (`plugin.enabled`,\n"
			  "      NNN-<id>-tracking-provider.json) instead.\n",
			  what);
		}
		return 1;
	case STORE_WRITE_FAILED:
	default: P("FAIL: %s failed.\n", what); return 1;
	}
}

static void
note_pickup(void)
{
	P("The viewer source and the composed XR_DXR_display_info tracking advertisement are fixed per\n");
	P("OpenXR instance: this applies at the next app launch.\n");
}

static void
note_settings_pickup(void)
{
	P("The provider reads its settings when it starts (on demand, the first time a session needs a\n");
	P("viewer) — launch a fresh app to apply.\n");
}

static int
cmd_enable(const char *id, bool enable, bool machine)
{
	if (!cli_input_id_valid(id)) {
		P("error: '%s' is not a provider id.\n", id != NULL ? id : "");
		return 1;
	}
	if (!provider_registered(id)) {
		P("FAIL: '%s' is not a registered viewer-tracking provider (see 'tracking list') — nothing written.\n",
		  id);
		return 1;
	}
	const int rc = write_result(
	    store_set_enabled(id, enable, machine),
	    machine ? "writing the machine-wide Enabled value" : "writing the per-user Enabled value", machine);
	if (rc != 0) {
		return rc;
	}
	P("%s: %s %s.\n", id, enable ? "enabled" : "disabled",
	  machine ? "machine-wide (HKLM)" : "for this user (HKCU)");
	if (!machine && target_component_policy_lock(target_tracking_plugin_component_class())) {
		P("note: HKLM Policy\\Lock is set, so the per-user value is ignored.\n");
	}
	note_pickup();
	return 0;
}

static int
cmd_use(const char *id, bool machine)
{
	if (!cli_input_id_valid(id)) {
		P("error: '%s' is not a provider id.\n", id != NULL ? id : "");
		return 1;
	}
	if (!provider_registered(id)) {
		P("warning: '%s' is not a registered viewer-tracking provider; the preference is ignored until it "
		  "is.\n",
		  id);
	}
	const int rc = write_result(store_set_preferred(id, machine), "writing the PreferredPlugin override", machine);
	if (rc != 0) {
		return rc;
	}
	P("%s PreferredPlugin set to '%s' — ranked first, ahead of ProbeOrder.\n",
	  machine ? "Machine-wide" : "Per-user", id);
	note_pickup();
	return 0;
}

static int
cmd_reset(bool machine)
{
	const int rc = write_result(store_clear_preferred(machine), "clearing the PreferredPlugin override", machine);
	if (rc != 0) {
		return rc;
	}
	P("%s PreferredPlugin override cleared — normal ProbeOrder ranking restored.\n",
	  machine ? "Machine-wide" : "Per-user");
	note_pickup();
	return 0;
}

static int
cmd_options_verbs(int argc, const char **argv)
{
	const struct cli_component_class k = {
	    .verb = "tracking",
	    .noun = "viewer-tracking provider",
	    .cls = target_tracking_plugin_component_class(),
	    .registered = provider_registered,
	    .describe = target_tracking_plugin_describe_settings,
	    .note_pickup = note_settings_pickup,
	    .posix_machine_hint = "the manifest's \"settings\" object (NNN-<id>-tracking-provider.json)",
	};
	return cli_component_options_dispatch(&k, argc, argv);
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli tracking <list|enable|disable|use|prefer|reset|options|set|unset>\n");
	P("  list [--json]                Registered viewer-tracking providers: ProbeOrder, Enabled (+ source),\n");
	P("                               provenance, the PreferredPlugin override, the one ranked first ('*').\n");
	P("  enable | disable <id> [--machine]\n");
	P("                               Enabled on the provider's row: per user (HKCU) by default,\n");
	P("                               machine-wide (HKLM, admin) with --machine; HKCU wins.\n");
	P("  use | prefer <id> [--machine] PreferredPlugin override: <id> is ranked first, ahead of ProbeOrder.\n");
	P("  reset [--machine]            Clear the PreferredPlugin override.\n");
	cli_component_options_usage(&(const struct cli_component_class){
	    .verb = "tracking", .noun = "viewer-tracking provider", .cls = target_tracking_plugin_component_class()});
	P("A provider serves a screen only when that display does not track the viewer itself and declares\n");
	P("EXTERNAL_OK; 'displayxr-cli status' shows each screen's viewer source. Everything applies at the next\n");
	P("app launch. POSIX: use/reset write the per-user tracking-preferred file; enable/disable = the\n");
	P("manifest's plugin.enabled.\n");
	return 1;
}

int
cli_cmd_tracking(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	const char *verb = argv[2];
	const bool machine = cli_has_flag(argc, argv, "--machine");
	if (strcmp(verb, "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(verb, "enable") == 0 || strcmp(verb, "disable") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'tracking %s' needs a provider id.\n\n", verb);
			return print_usage();
		}
		return cmd_enable(argv[3], strcmp(verb, "enable") == 0, machine);
	}
	if (strcmp(verb, "use") == 0 || strcmp(verb, "prefer") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'tracking %s' needs a provider id.\n\n", verb);
			return print_usage();
		}
		return cmd_use(argv[3], machine);
	}
	if (strcmp(verb, "reset") == 0) {
		return cmd_reset(machine);
	}
	const int rc = cmd_options_verbs(argc, argv);
	if (rc >= 0) {
		return rc;
	}
	P("error: unknown 'tracking' subcommand '%s'.\n\n", verb);
	return print_usage();
}
