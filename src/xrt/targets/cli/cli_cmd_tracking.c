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
 *   tracking calibrate [solve|apply] ...  Mirror-alignment camera-pose calibration
 *                                        (cli_cmd_tracking_calibrate.c).
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

#include "xrt/xrt_component_license.h"
#include "cli_input_predict.h" // cli_input_id_valid: the same id rule

#include "xrt/xrt_config_os.h"

#include "target_component_loader.h"
#include "target_tracking_plugin_loader.h"

#include "xrt/xrt_display_status.h"
#include "util/u_setting.h"

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

static void
list_screens(cJSON *root);

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
			// ADR-054 N5: the licence state the component reports (negotiate
			// only, like `options`); `not_applicable` when it reports none.
			struct xrt_component_license_state lic;
			(void)target_tracking_plugin_license_state(list[i].id, &lic);
			cli_component_license_json(p, &lic);
			cJSON_AddBoolToObject(p, "preferred", have_pref && strcmp(list[i].id, preferred) == 0);
			cJSON_AddBoolToObject(p, "first", i == first);
			cJSON_AddItemToArray(arr, p);
		}
		// ADR-066 Part B: every screen's viewer-tracking setting and availability.
		list_screens(root);
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
		list_screens(NULL);
		return 0;
	}
	P("\t  %-20s %-24s %10s  %-18s %-10s %-17s %s\n", "id", "name", "ProbeOrder", "Enabled", "Preferred",
	  "Provenance", "Licence");
	for (int i = 0; i < n; i++) {
		// ADR-054 N5: what the component says about its licence ("-" = nothing to report).
		struct xrt_component_license_state lic;
		char lic_cell[40];
		(void)target_tracking_plugin_license_state(list[i].id, &lic);
		char enabled[32];
		snprintf(enabled, sizeof(enabled), "%s (%s)", list[i].enabled ? "yes" : "no", list[i].enabled_source);
		P("\t%s %-20s %-24s %10u  %-18s %-10s %-17s %s\n", i == first ? "*" : " ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order, enabled,
		  (have_pref && strcmp(list[i].id, preferred) == 0) ? "yes" : "-",
		  target_component_provenance_str(list[i].provenance),
		  cli_component_license_cell(&lic, lic_cell, sizeof(lic_cell)));
		P("\t    %s\n", list[i].binary_path);
	}
	P("\t('*' = the provider the loader would rank first. It serves a screen only when that display does not\n");
	P("\t track the viewer itself, declares EXTERNAL_OK and its viewer-tracking setting asks for it.)\n");
	list_screens(NULL);
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

/*
 *
 * Per-screen viewer tracking (ADR-066 Part B).
 *
 */

static const char *
flag_value(int argc, const char **argv, const char *flag)
{
	for (int i = 2; i + 1 < argc; i++) {
		if (strcmp(argv[i], flag) == 0 && argv[i + 1][0] != '-') {
			return argv[i + 1];
		}
	}
	return NULL;
}

//! The first positional argument after the verb that is neither a flag nor a flag's value.
static const char *
positional(int argc, const char **argv)
{
	for (int i = 3; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strcmp(argv[i], "--screen") == 0) {
				i++; // its value
			}
			continue;
		}
		return argv[i];
	}
	return NULL;
}

static bool
key_ieq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char ca = *a, cb = *b;
		ca = (char)((ca >= 'A' && ca <= 'Z') ? ca + ('a' - 'A') : ca);
		cb = (char)((cb >= 'A' && cb <= 'Z') ? cb + ('a' - 'A') : cb);
		if (ca != cb) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static int
machine_write_result(enum u_setting_write_result r, const char *what)
{
	switch (r) {
	case U_SETTING_WRITE_OK: return 0;
	case U_SETTING_WRITE_DENIED:
		P("FAIL: %s was denied — run from an elevated terminal (HKLM needs admin).\n", what);
		return 1;
	case U_SETTING_WRITE_UNSUPPORTED:
		P("FAIL: %s — there is no machine-wide store on this platform (drop --machine).\n", what);
		return 1;
	case U_SETTING_WRITE_FAILED:
	default: P("FAIL: %s failed.\n", what); return 1;
	}
}

//! After a per-screen write: tell the running service, and say when it applies.
static void
note_screen_pickup(void)
{
	int xret = 0;
	switch (cli_poke_service_reprobe_quiet(&xret)) {
	case 0:
		P("The running DisplayXR service re-evaluated the screens: the views of running apps follow now.\n");
		P("(The tracking capability an app was told at its start stays as it was until its next launch.)\n");
		break;
	case 1:
		P("The running DisplayXR service refused the re-probe request (xrt_result=%d) — applies at the next "
		  "session.\n",
		  xret);
		break;
	default:
		P("No DisplayXR service reachable — applies at the next app launch (in-process apps read it at "
		  "start).\n");
		break;
	}
}

static int
cmd_screen_set(const char *key, const char *value, bool machine)
{
	const enum u_setting_viewer_tracking v = u_setting_viewer_tracking_parse(value);
	if (v == U_SETTING_VIEWER_TRACKING_UNSET) {
		P("error: '%s' is not a viewer-tracking setting: use off | 3d | always ('on' = 3d).\n",
		  value != NULL ? value : "");
		return 1;
	}
	if (strcmp(key, "all") == 0) {
		P("error: 'tracking set --screen' needs one screen key ('all' is for 'tracking reset --screen').\n");
		return 1;
	}
	char cur[64];
	char legacy[64];
	if (!cli_screen_key_lookup(key, cur, sizeof(cur), legacy, sizeof(legacy))) {
		P("note: no connected screen has the key '%s' (see 'tracking list'); the setting is stored anyway.\n",
		  key);
		legacy[0] = '\0';
	} else if (!key_ieq(cur, key)) {
		P("note: '%s' is that screen's legacy key; storing the setting under its current key '%s'.\n", key,
		  cur);
		key = cur;
	}
	if (machine) {
		const int rc = machine_write_result(u_setting_machine_set_viewer_tracking_for_screen(key, v),
		                                    "writing the machine-wide viewer-tracking setting");
		if (rc != 0) {
			return rc;
		}
		if (legacy[0] != '\0') {
			(void)u_setting_machine_set_viewer_tracking_for_screen(legacy, U_SETTING_VIEWER_TRACKING_UNSET);
		}
		P("Machine-wide viewer tracking for screen '%s': %s (a per-user value for it still wins).\n", key,
		  u_setting_viewer_tracking_str(v));
	} else {
		if (!u_setting_user_set_viewer_tracking_for_screen(key, v)) {
			P("FAIL: could not write the per-user settings file (invalid key, or I/O error).\n");
			return 1;
		}
		if (legacy[0] != '\0') {
			(void)u_setting_user_set_viewer_tracking_for_screen(legacy, U_SETTING_VIEWER_TRACKING_UNSET);
		}
		char path[512] = {0};
		(void)u_setting_user_path(path, sizeof(path));
		P("Viewer tracking for screen '%s': %s (%s).\n", key, u_setting_viewer_tracking_str(v), path);
	}
	if (v != U_SETTING_VIEWER_TRACKING_OFF) {
		P("It takes effect only where it is offered — 'tracking list' shows each screen's availability.\n");
	}
	note_screen_pickup();
	return 0;
}

static int
cmd_screen_reset(const char *key, bool machine)
{
	const bool all = strcmp(key, "all") == 0;
	char other[64] = {0};
	if (!all) {
		char cur[64];
		char legacy[64];
		if (cli_screen_key_lookup(key, cur, sizeof(cur), legacy, sizeof(legacy)) && legacy[0] != '\0') {
			snprintf(other, sizeof(other), "%s", key_ieq(cur, key) ? legacy : cur);
		}
	}
	if (machine) {
		const int rc = machine_write_result(
		    u_setting_machine_set_viewer_tracking_for_screen(all ? NULL : key, U_SETTING_VIEWER_TRACKING_UNSET),
		    "clearing the machine-wide viewer-tracking setting");
		if (rc != 0) {
			return rc;
		}
		if (other[0] != '\0') {
			(void)u_setting_machine_set_viewer_tracking_for_screen(other, U_SETTING_VIEWER_TRACKING_UNSET);
		}
	} else {
		bool ok = all ? u_setting_user_clear_viewer_tracking_per_screen()
		              : u_setting_user_set_viewer_tracking_for_screen(key, U_SETTING_VIEWER_TRACKING_UNSET);
		if (ok && other[0] != '\0') {
			ok = u_setting_user_set_viewer_tracking_for_screen(other, U_SETTING_VIEWER_TRACKING_UNSET);
		}
		if (!ok) {
			P("FAIL: could not update the per-user settings file.\n");
			return 1;
		}
	}
	P("%s viewer-tracking setting%s cleared%s%s%s (back to the default: off).\n",
	  machine ? "Machine-wide" : "Per-user", all ? "s" : "", all ? "" : " for screen '", all ? "" : key,
	  all ? "" : "'");
	note_screen_pickup();
	return 0;
}

static cJSON *
screen_viewer_json(const struct xrt_status_screen *s)
{
	const struct xrt_status_viewer_tracking *vt = &s->eye_tracking.viewer_tracking;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "key", s->key);
	cJSON_AddStringToObject(o, "device_name", s->device_name);
	cJSON_AddStringToObject(o, "friendly_name", s->friendly_name);
	if (s->eye_tracking.viewer_policy[0] != '\0') {
		cJSON_AddStringToObject(o, "viewer_policy", s->eye_tracking.viewer_policy);
	}
	if (!vt->evaluated) {
		cJSON_AddNullToObject(o, "viewer_tracking");
		return o;
	}
	cJSON *v = cJSON_AddObjectToObject(o, "viewer_tracking");
	cJSON_AddStringToObject(v, "setting", vt->setting);
	cJSON_AddStringToObject(v, "setting_source", vt->setting_source);
	cJSON_AddStringToObject(v, "effective", vt->effective);
	cJSON *t = cJSON_AddObjectToObject(v, "tracked");
	cJSON_AddBoolToObject(t, "2d", vt->tracked_2d);
	cJSON_AddBoolToObject(t, "3d", vt->tracked_3d);
	if (s->mode.valid) {
		cJSON_AddStringToObject(v, "current_mode", s->mode.is_3d ? "3d" : "2d");
		cJSON_AddBoolToObject(v, "active", s->mode.is_3d ? vt->tracked_3d : vt->tracked_2d);
	} else {
		cJSON_AddNullToObject(v, "current_mode");
		cJSON_AddNullToObject(v, "active");
	}
	if (vt->provider[0] != '\0') {
		cJSON_AddStringToObject(v, "provider", vt->provider);
	} else {
		cJSON_AddNullToObject(v, "provider");
	}
	cJSON_AddBoolToObject(v, "available", vt->available);
	if (!vt->available && vt->reason[0] != '\0') {
		cJSON_AddStringToObject(v, "reason", vt->reason);
	} else {
		cJSON_AddNullToObject(v, "reason");
	}
	if (vt->available && vt->reason[0] != '\0') {
		cJSON_AddStringToObject(v, "detail", vt->reason);
	} else {
		cJSON_AddNullToObject(v, "detail");
	}
	return o;
}

//! `tracking list`'s per-screen part. @p arr non-NULL = JSON (one object per screen), else text.
static void
list_screens(cJSON *root)
{
	struct xrt_status_snapshot *snap = (struct xrt_status_snapshot *)calloc(1, sizeof(*snap));
	if (snap == NULL) {
		return;
	}
	const bool from_service = cli_status_fetch(snap);
	if (root != NULL) {
		cJSON_AddStringToObject(root, "screens_source", from_service ? "service" : "headless");
		cJSON *arr = cJSON_AddArrayToObject(root, "screens");
		for (uint32_t i = 0; i < snap->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
			cJSON_AddItemToArray(arr, screen_viewer_json(&snap->screens[i]));
		}
		free(snap);
		return;
	}
	P("\n :: Viewer tracking per screen (tracking set --screen <key> off|3d|always; from the %s)\n",
	  from_service ? "running service" : "headless runtime");
	if (snap->screen_count == 0) {
		P("\t(no screens)\n");
	}
	for (uint32_t i = 0; i < snap->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
		const struct xrt_status_screen *s = &snap->screens[i];
		const struct xrt_status_viewer_tracking *vt = &s->eye_tracking.viewer_tracking;
		P("\t%-28s %-10s", s->key[0] ? s->key : "?", s->device_name[0] ? s->device_name : "?");
		if (!vt->evaluated) {
			P(" (not evaluated)\n");
			continue;
		}
		P(" setting %-7s (%s) -> %-6s", vt->setting, vt->setting_source, vt->effective);
		if (vt->tracked_2d || vt->tracked_3d) {
			P(" provider %s [%s%s%s]", vt->provider, vt->tracked_2d ? "2D" : "",
			  vt->tracked_2d && vt->tracked_3d ? "+" : "", vt->tracked_3d ? "3D" : "");
		}
		if (vt->available) {
			P("  available%s%s\n", vt->reason[0] ? ": " : "", vt->reason);
		} else {
			P("  unavailable: %s\n", vt->reason[0] ? vt->reason : "?");
		}
	}
	free(snap);
}

//! The class's `activate` slot behind the shared `activate` verb (ADR-054 N5).
static int
tracking_activate(
    const char *id, const char *token, struct xrt_component_license_state *out_state, char *err, size_t err_cap)
{
	return (int)target_tracking_plugin_activate(id, token, out_state, err, err_cap);
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
	    .activate = tracking_activate,
	};
	return cli_component_options_dispatch(&k, argc, argv);
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli tracking <list|enable|disable|use|prefer|reset|options|set|unset|calibrate>\n");
	P("  list [--json]                Registered viewer-tracking providers: ProbeOrder, Enabled (+ source),\n");
	P("                               provenance, the PreferredPlugin override, the one ranked first ('*').\n");
	P("  enable | disable <id> [--machine]\n");
	P("                               Enabled on the provider's row: per user (HKCU) by default,\n");
	P("                               machine-wide (HKLM, admin) with --machine; HKCU wins.\n");
	P("  use | prefer <id> [--machine] PreferredPlugin override: <id> is ranked first, ahead of ProbeOrder.\n");
	P("  reset [--machine]            Clear the PreferredPlugin override.\n");
	P("  set --screen <key> off|3d|always [--machine]\n");
	P("                               Viewer tracking on that screen (ADR-066 Part B): off (default; the\n");
	P("                               camera stays closed), 3d (only while the app is in the 3D mode),\n");
	P("                               always (2D and 3D). 'on' = 3d; 'tracking on|off --screen <key>' too.\n");
	P("                               Offered only where the display does not track the viewer itself and a\n");
	P("                               provider has a usable camera: 'tracking list' says why not.\n");
	P("  reset --screen <key>|all [--machine]\n");
	P("                               Clear one / every per-screen viewer-tracking setting.\n");
	P("  calibrate --screen <key> --watch --json | solve --in <f> | apply --screen <k> --camera <k> --pose <p>\n");
	P("                               Camera-pose calibration (mirror alignment); 'tracking calibrate help'.\n");
	cli_component_options_usage(&(const struct cli_component_class){.verb = "tracking",
	                                                                .noun = "viewer-tracking provider",
	                                                                .cls = target_tracking_plugin_component_class(),
	                                                                .activate = tracking_activate});
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
	// ADR-066 Part B: the per-screen viewer-tracking setting (`--screen` routes these verbs).
	const bool screen_verb = strcmp(verb, "set") == 0 || strcmp(verb, "on") == 0 || strcmp(verb, "off") == 0 ||
	                         strcmp(verb, "reset") == 0;
	if (screen_verb && cli_has_flag(argc, argv, "--screen")) {
		const char *screen = flag_value(argc, argv, "--screen");
		if (screen == NULL) {
			P("error: '--screen' needs a screen key (see 'tracking list').\n\n");
			return print_usage();
		}
		if (strcmp(verb, "set") == 0) {
			const char *value = positional(argc, argv);
			if (value == NULL) {
				P("error: 'tracking set --screen <key>' needs off | 3d | always.\n\n");
				return print_usage();
			}
			return cmd_screen_set(screen, value, machine);
		}
		if (strcmp(verb, "on") == 0 || strcmp(verb, "off") == 0) {
			return cmd_screen_set(screen, verb, machine); // shorthands: on = 3d
		}
		if (strcmp(verb, "reset") == 0) {
			return cmd_screen_reset(screen, machine);
		}
		P("error: 'tracking %s' takes no --screen (per-screen verbs: set, on, off, reset).\n\n", verb);
		return print_usage();
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
	if (strcmp(verb, "calibrate") == 0) {
		return cli_cmd_tracking_calibrate(argc, argv);
	}
	const int rc = cmd_options_verbs(argc, argv);
	if (rc >= 0) {
		return rc;
	}
	P("error: unknown 'tracking' subcommand '%s'.\n\n", verb);
	return print_usage();
}
