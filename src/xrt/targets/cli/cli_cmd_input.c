// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `input` subcommand — input providers (ADR-034): the four component
 *         verbs of ADR-054 (`list`, `enable|disable`, `use|reset`) plus the
 *         haptic round-trip test.
 *
 *   input list [--json]                 Registered input-provider plug-ins with
 *                                       ProbeOrder, Enabled (+ where it came
 *                                       from), the PreferredPlugin override,
 *                                       provenance (ADR-054 N4b) and which one
 *                                       the loader would rank first.
 *                                       Reads the discovery root without loading
 *                                       any provider DLL (mirror of `dp list`).
 *   input enable | disable <id> [--machine]
 *                                       `Enabled` on the provider's row: per user
 *                                       (HKCU, no admin) by default, machine-wide
 *                                       (HKLM, admin) with --machine. HKCU wins.
 *   input use <id> [--machine]          `PreferredPlugin` under the InputProviders
 *                                       root: ranked first, ahead of ProbeOrder.
 *   input reset [--machine]             Clear it.
 *   input use <id> --app <exe>          ADR-034 Amendment 5 (#2181): the user's
 *   input reset --app <exe>             per-app pin for executable <exe>
 *                                       (`SampleGame.exe`; a path is reduced to its
 *                                       basename): that app's controller / hand /
 *                                       gamepad roles prefer <id> for every family
 *                                       <id> supplies, ahead of the app's own
 *                                       request and of PreferredPlugin. HKCU
 *                                       `InputProviders\PerApp\<exe>\PreferredPlugin`
 *                                       / POSIX `input-preferred-<exe>`. Per user only.
 *   input emulated on|off [--machine]   The `EmulatedControllers` gate (ADR-054
 *                                       N7): `InputProviders\Policy\
 *                                       EmulatedControllers`, HKCU by default,
 *                                       HKLM with --machine (POSIX: the per-user
 *                                       `emulated_controllers` file). off = no
 *                                       qwerty left/right pair; the hand roles
 *                                       stay unassigned when no provider holds
 *                                       them. The qwerty head is not affected.
 *   input options <id> [--json]         The provider's settings: declared + stored,
 *                                       with value and source (N3c).
 *   input set <id> <KEY> <value> [--machine] / input unset <id> <KEY> [--machine]
 *                                       `Settings\<KEY>` on the provider's row (HKCU;
 *                                       HKLM with --machine; the POSIX per-user
 *                                       `<id>.settings.json`). DXR_INPUT_<ID>_<KEY>
 *                                       in the hosting process's env always wins.
 *   input haptic-test [seconds]         Headless haptics driver (#823 Phase 2).
 *
 * Registry writes follow `conv enable|use` (ADR-053 §7.1, the newer pattern):
 * HKCU by default, HKLM 64-bit view with `--machine`; the loader reads HKCU
 * over HKLM for both values (`target_input_plugin_loader.c`). Only `Enabled`
 * and `PreferredPlugin` are ever read from HKCU — the provider rows (`Binary`,
 * `ProbeOrder`, …) stay HKLM-only, so a per-user key cannot inject a DLL.
 *
 * POSIX: `use` / `reset` write the `input-preferred` file in the per-user
 * manifest dir (the DP loader's `preferred` pattern, spec §3.3);
 * `enable|disable` has no per-user store — the manifest's `plugin.enabled`
 * is the switch, as for conversion providers — and says so.
 *
 * Live re-select (N3d, #2051): every write ends with a poke of the running
 * service (`system_request_component_reselect`), which re-reads the policy
 * and re-ranks the arbiter — the hand / rig roles move on the next
 * xrSyncActions, no restart. The service's policy watch also catches a
 * write made without the CLI. What stays a restart: a provider this
 * service did not load (disabled or unregistered when it scanned); the
 * enable verb says so. In-process apps re-read at their next launch.
 *
 * Issues #823 (list, haptic-test), N3a (ADR-054 verbs).
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_component_options.h"
#include "cli_input_predict.h"
#include "cli_ipc.h"
#include "cli_query.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include "target_component_loader.h"
#include "target_input_plugin_loader.h"

#include "util/u_exe_basename.h"
#include "xrt/xrt_input_selection.h"

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
#define MAX_APP_PINS 64

//! Discovery root — the registry key the loader enumerates (Windows).
#define INPUT_REG_ROOT "Software\\DisplayXR\\InputProviders"

#define P(...) printf(__VA_ARGS__)


/*
 *
 * list
 *
 */

static bool
provider_registered(const char *id)
{
	struct target_input_plugin_desc list[MAX_PROVIDERS];
	const int n = target_input_plugin_enumerate(list, MAX_PROVIDERS);
	return cli_input_find(list, n, id) >= 0;
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_input_plugin_desc list[MAX_PROVIDERS];
	int n = target_input_plugin_enumerate(list, MAX_PROVIDERS);

	bool force_qwerty = target_input_plugin_get_force_qwerty();
	const char *emulated_source = "default";
	const bool emulated = target_input_plugin_get_emulated_controllers(&emulated_source);
	char preferred[64] = {0};
	const bool have_pref = target_input_plugin_get_preferred(preferred, sizeof(preferred));
	int active = cli_input_predict_active(list, n, have_pref ? preferred : NULL);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddBoolToObject(root, "force_qwerty", force_qwerty);
		// N7: the gate as data — {value, source}, the selftest's shape.
		cJSON *em = cJSON_AddObjectToObject(root, "emulated_controllers");
		cJSON_AddNumberToObject(em, "value", emulated ? 1 : 0);
		cJSON_AddStringToObject(em, "source", emulated_source);
		if (have_pref) {
			cJSON_AddStringToObject(root, "preferred", preferred);
		} else {
			cJSON_AddNullToObject(root, "preferred");
		}
		// HKLM `<root>\Policy\Lock` (Windows): per-user Enabled values are ignored.
		// The same `policy.policy_lock` key `conv list --json` emits.
		cJSON *pol = cJSON_AddObjectToObject(root, "policy");
		cJSON_AddBoolToObject(pol, "policy_lock",
		                      target_component_policy_lock(target_input_plugin_component_class()));
		// ADR-034 Am.5 (#2181): the user's per-app pins as STORED (Policy\Lock,
		// reported above, hides them from the resolver).
		{
			struct target_input_app_pin pins[MAX_APP_PINS];
			const int np = target_input_plugin_list_app_pins(pins, MAX_APP_PINS);
			cJSON *per_app = cJSON_AddArrayToObject(root, "per_app");
			for (int i = 0; i < np; i++) {
				cJSON *row = cJSON_CreateObject();
				cJSON_AddStringToObject(row, "app", pins[i].exe_basename);
				cJSON_AddStringToObject(row, "preferred", pins[i].provider_id);
				cJSON_AddBoolToObject(row, "registered", provider_registered(pins[i].provider_id) ||
				                                             strcmp(pins[i].provider_id,
				                                                    XRT_INPUT_PROVIDER_ID_EMULATED) == 0);
				cJSON_AddItemToArray(per_app, row);
			}
		}
		// TODO(#2181 phase 2): `holders` — the per-client LIVE holder per family,
		// from the service's input selection over DIAG IPC (and the ADR-051
		// status snapshot). Phase 1 ships the stored pins only.
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
			// ADR-054 N4b, as the lister classified the binary.
			cJSON_AddStringToObject(p, "provenance", target_component_provenance_str(list[i].provenance));
			cJSON_AddStringToObject(p, "provenance_reason", list[i].provenance_reason);
			cJSON_AddStringToObject(p, "signer", list[i].signer);
			cJSON_AddBoolToObject(p, "preferred", have_pref && strcmp(list[i].id, preferred) == 0);
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
	P("\tPreferredPlugin override: %s\n", have_pref ? preferred : "<unset>");
	if (have_pref && !provider_registered(preferred)) {
		P("\t  (not a registered provider — the loader ignores it)\n");
	}
	P("\tEmulated controllers: %s (%s)%s\n", emulated ? "on" : "off", emulated_source,
	  emulated ? "" : " — no qwerty left/right pair; hand roles unassigned when no provider holds them");
	if (n == 0) {
		P("\t(none — discovery root absent or empty; %s)\n",
		  emulated ? "qwerty keeps the hand roles" : "the hand roles stay unassigned");
		return 0;
	}
	if (target_component_policy_lock(target_input_plugin_component_class())) {
		P("\tPolicy\\Lock: SET (HKLM) — per-user Enabled values are ignored; the machine's win.\n");
	}
	if (force_qwerty) {
		P("\tForceQwerty override: SET — providers are skipped, qwerty claims the hand roles.\n");
	}
	P("\t  %-20s %-24s %10s  %-18s %-10s %s\n", "id", "name", "ProbeOrder", "Enabled", "Preferred", "Provenance");
	for (int i = 0; i < n; i++) {
		char enabled[32];
		snprintf(enabled, sizeof(enabled), "%s (%s)", list[i].enabled ? "yes" : "no", list[i].enabled_source);
		P("\t%s %-20s %-24s %10u  %-18s %-10s %s\n", (i == active && !force_qwerty) ? "*" : " ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order, enabled,
		  (have_pref && strcmp(list[i].id, preferred) == 0) ? "yes" : "-",
		  target_component_provenance_str(list[i].provenance));
		P("\t    %s\n", list[i].binary_path);
	}
	P("\t('*' = the provider the loader would rank first.)\n");
	{
		struct target_input_app_pin pins[MAX_APP_PINS];
		const int np = target_input_plugin_list_app_pins(pins, MAX_APP_PINS);
		if (np > 0) {
			P("\tPer-app pins (input use <id> --app <exe>):\n");
			for (int i = 0; i < np; i++) {
				P("\t  %-32s -> %s\n", pins[i].exe_basename, pins[i].provider_id);
			}
		}
	}
	return 0;
}


/*
 *
 * enable | disable | use | reset (N3a)
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

/*!
 * Open-or-create `<root>[\<sub>]` for writing in the hive the verb targets.
 * HKLM is the 64-bit view (the loader reads without redirection); HKCU has no
 * redirection to speak of.
 */
static enum store_write
reg_open_for_write(bool machine, const char *sub, bool create, HKEY *out_key)
{
	char path[256];
	if (sub != NULL && sub[0] != '\0') {
		snprintf(path, sizeof(path), "%s\\%s", INPUT_REG_ROOT, sub);
	} else {
		snprintf(path, sizeof(path), "%s", INPUT_REG_ROOT);
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
			return STORE_WRITE_OK; // nothing to clear
		}
	}
	if (rc != ERROR_SUCCESS) {
		*out_key = NULL;
		return rc == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

static enum store_write
reg_set_dword(bool machine, const char *sub, const char *value, uint32_t v)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, sub, true, &key);
	if (r != STORE_WRITE_OK) {
		return r;
	}
	DWORD d = v;
	const LSTATUS w = RegSetValueExA(key, value, 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
	RegCloseKey(key);
	if (w != ERROR_SUCCESS) {
		return w == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

static enum store_write
reg_set_string(bool machine, const char *sub, const char *value, const char *s)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, sub, true, &key);
	if (r != STORE_WRITE_OK) {
		return r;
	}
	const LSTATUS w = RegSetValueExA(key, value, 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
	RegCloseKey(key);
	if (w != ERROR_SUCCESS) {
		return w == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

static enum store_write
reg_clear_value(bool machine, const char *sub, const char *value)
{
	HKEY key = NULL;
	enum store_write r = reg_open_for_write(machine, sub, false, &key);
	if (r != STORE_WRITE_OK || key == NULL) {
		return r;
	}
	const LSTATUS rc = RegDeleteValueA(key, value);
	RegCloseKey(key);
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
		return rc == ERROR_ACCESS_DENIED ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

static enum store_write
store_set_enabled(const char *id, bool enabled, bool machine)
{
	return reg_set_dword(machine, id, "Enabled", enabled ? 1u : 0u);
}

static enum store_write
store_set_preferred(const char *id, bool machine)
{
	return reg_set_string(machine, NULL, "PreferredPlugin", id);
}

static enum store_write
store_clear_preferred(bool machine)
{
	return reg_clear_value(machine, NULL, "PreferredPlugin");
}

//! N7: `InputProviders\Policy\EmulatedControllers` in the hive the verb targets.
static enum store_write
store_set_emulated(bool on, bool machine)
{
	return reg_set_dword(machine, "Policy", "EmulatedControllers", on ? 1u : 0u);
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

#ifndef XRT_OS_ANDROID
//! Best-effort `mkdir -p` of the per-user dir: a fresh box has no
//! `DisplayXR/DisplayProcessors` under HOME / XDG yet, and a single-level
//! mkdir left the first `input use` / `input emulated` write failing with
//! ENOENT.
static void
user_dir_mkdir_p(const char *dir)
{
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
#endif

//! `<per-user dir>/input-preferred` — the DP loader's `preferred` file pattern.
static bool
preferred_file_path(char *out, size_t cap, bool create_dir)
{
	char dir[512];
#ifdef XRT_OS_ANDROID
	(void)dir;
	(void)create_dir;
	out[0] = '\0';
	return false; // no per-user store on Android (ProbeOrder in the file name is the only ranking)
#else
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	if (create_dir) {
		user_dir_mkdir_p(dir);
	}
	snprintf(out, cap, "%s/input-preferred", dir);
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
		return STORE_WRITE_OK; // nowhere it could live → nothing to clear
	}
	if (remove(path) != 0 && errno != ENOENT) {
		return errno == EACCES ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	return STORE_WRITE_OK;
}

//! N7: `<per-user dir>/emulated_controllers` — the loader's `force_qwerty` byte convention.
static bool
emulated_file_path(char *out, size_t cap, bool create_dir)
{
	char dir[512];
#ifdef XRT_OS_ANDROID
	(void)dir;
	(void)create_dir;
	out[0] = '\0';
	return false; // no per-user store on Android; the pair is always published
#else
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	if (create_dir) {
		user_dir_mkdir_p(dir);
	}
	snprintf(out, cap, "%s/emulated_controllers", dir);
	return true;
#endif
}

static enum store_write
store_set_emulated(bool on, bool machine)
{
	if (machine) {
		return STORE_WRITE_UNSUPPORTED;
	}
	char path[600];
	if (!emulated_file_path(path, sizeof(path), true)) {
		return STORE_WRITE_UNSUPPORTED;
	}
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return errno == EACCES ? STORE_WRITE_DENIED : STORE_WRITE_FAILED;
	}
	fprintf(f, "%s\n", on ? "1" : "0");
	fclose(f);
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
			  "      NNN-<name>-input-provider.json) instead.\n",
			  what);
		}
		return 1;
	case STORE_WRITE_FAILED:
	default: P("FAIL: %s failed.\n", what); return 1;
	}
}

/*!
 * #2051: poke the running service and say what happens now, per the input
 * class's lifetime rules (provider set fixed per process; roles re-ranked
 * live). @p enabling_id: the provider an `input enable` just enabled, so the
 * one case that stays a restart is named.
 */
static void
note_pickup(const char *enabling_id)
{
#ifdef CLI_HAVE_IPC
	xrt_result_t xret = XRT_SUCCESS;
	switch (cli_ipc_poke_component_reselect(TARGET_COMPONENT_CLASS_INPUT, &xret)) {
	case CLI_IPC_POKE_ACCEPTED:
		P("The running DisplayXR service re-ranks its input providers now: the hand / rig roles move on\n");
		P("the next xrSyncActions (a connected app sees a new roles generation).\n");
		if (enabling_id != NULL) {
			P("If '%s' was disabled when the service started it is not loaded yet — it joins at the\n",
			  enabling_id);
			P("next service start (the service log says which).\n");
		}
		P("In-process apps read the policy at their next launch.\n");
		return;
	case CLI_IPC_POKE_NO_WATCH:
		P("The running DisplayXR service has no live re-select — applies at its next start.\n");
		return;
	case CLI_IPC_POKE_REFUSED:
		P("The running DisplayXR service refused the re-select request (xrt_result=%d) — applies at its\n",
		  (int)xret);
		P("next start.\n");
		return;
	case CLI_IPC_POKE_UNREACHABLE:
	default: break;
	}
#else
	(void)enabling_id;
#endif
	P("No DisplayXR service reachable — applies when it next starts (in-process apps: their next launch).\n");
}

//! A provider reads its settings at init through the host iface (N3c): same pickup rule as above.
static void
note_settings_pickup(void)
{
	P("The provider reads its settings at init through the host iface — a running service /\n");
	P("session keeps what it read; restart the DisplayXR service (or launch a fresh app).\n");
}

//! The options | set | unset verbs over the input class (shared logic, cli_component_options.c).
static int
cmd_options_verbs(int argc, const char **argv)
{
	const struct cli_component_class k = {
	    .verb = "input",
	    .noun = "input provider",
	    .cls = target_input_plugin_component_class(),
	    .registered = provider_registered,
	    .describe = target_input_plugin_describe_settings,
	    .note_pickup = note_settings_pickup,
	    .posix_machine_hint = "the manifest's \"settings\" object (NNN-<id>-input-provider.json)",
	};
	return cli_component_options_dispatch(&k, argc, argv);
}

static int
cmd_enable(const char *id, bool enable, bool machine)
{
	if (!cli_input_id_valid(id)) {
		P("error: '%s' is not a provider id.\n", id != NULL ? id : "");
		return 1;
	}
	if (!provider_registered(id)) {
		// A per-user Enabled for an unregistered id would be an orphan row
		// that `input list` can never show: refuse, write nothing.
		P("FAIL: '%s' is not a registered input provider (see 'input list') — nothing written.\n", id);
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
	const bool locked = target_component_policy_lock(target_input_plugin_component_class());
	if (machine && !locked) {
		P("note: a per-user (HKCU) Enabled value for '%s' still wins for that user.\n", id);
	} else if (!machine && locked) {
		P("note: HKLM Policy\\Lock is set, so the per-user value is ignored.\n");
	}
	note_pickup(enable ? id : NULL);
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
		P("warning: '%s' is not a registered input provider; the preference is ignored until it is.\n", id);
	}
	const int rc = write_result(store_set_preferred(id, machine), "writing the PreferredPlugin override", machine);
	if (rc != 0) {
		return rc;
	}
#ifdef XRT_OS_WINDOWS
	P("%s PreferredPlugin set to '%s' — ranked first, ahead of ProbeOrder.\n",
	  machine ? "Machine-wide" : "Per-user", id);
	if (machine) {
		P("note: a per-user (HKCU) PreferredPlugin still wins for that user.\n");
	}
#else
	char path[600] = {0};
	(void)preferred_file_path(path, sizeof(path), false);
	P("Preferred input provider set to '%s' (%s) — ranked first, ahead of ProbeOrder.\n", id, path);
#endif
	note_pickup(NULL);
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
	note_pickup(NULL);
	return 0;
}

//! The value after @p flag, or NULL (absent, or followed by another flag / nothing).
static const char *
flag_value(int argc, const char **argv, const char *flag)
{
	for (int i = 0; i + 1 < argc; i++) {
		if (strcmp(argv[i], flag) == 0 && argv[i + 1][0] != '-') {
			return argv[i + 1];
		}
	}
	return NULL;
}

//! `--app <exe>`: reduce a path to its basename and validate it as a per-app key.
static bool
app_key_from_arg(const char *arg, char *out, size_t cap)
{
	if (arg == NULL || !u_exe_basename_from_path(arg, out, cap)) {
		P("error: '--app' needs an executable name such as SampleGame.exe (got '%s').\n", arg != NULL ? arg : "");
		return false;
	}
	return true;
}

static int
pin_write_result(enum target_input_pin_write r)
{
	switch (r) {
	case TARGET_INPUT_PIN_WRITE_OK: return 0;
	case TARGET_INPUT_PIN_WRITE_INVALID: P("FAIL: not a valid provider id / executable name — nothing written.\n"); return 1;
	case TARGET_INPUT_PIN_WRITE_DENIED: P("FAIL: the per-user store refused the write.\n"); return 1;
	case TARGET_INPUT_PIN_WRITE_UNSUPPORTED: P("FAIL: there is no per-user store on this platform.\n"); return 1;
	case TARGET_INPUT_PIN_WRITE_FAILED:
	default: P("FAIL: writing the per-app pin failed.\n"); return 1;
	}
}

/*!
 * `input use <id> --app <exe>` / `input reset --app <exe>` (ADR-034 Amendment
 * 5, #2181): the user's per-app pin. Per user only — `--machine` is refused
 * (there is no machine tier; `Policy\Lock` is the machine's say).
 */
static int
cmd_app_pin(const char *id, const char *app_arg, bool machine)
{
	if (machine) {
		P("FAIL: per-app pins are per user — drop --machine (HKLM Policy\\Lock is the machine's override).\n");
		return 1;
	}
	char app[XRT_INPUT_EXE_BASENAME_SIZE];
	if (!app_key_from_arg(app_arg, app, sizeof(app))) {
		return 1;
	}
	if (id != NULL) {
		if (!cli_input_id_valid(id)) {
			P("error: '%s' is not a provider id.\n", id);
			return 1;
		}
		if (!provider_registered(id) && strcmp(id, XRT_INPUT_PROVIDER_ID_EMULATED) != 0) {
			P("warning: '%s' is not a registered input provider; the pin is ignored until it is.\n", id);
		}
	}
	const int rc = pin_write_result(target_input_plugin_set_app_pin(app, id));
	if (rc != 0) {
		return rc;
	}
	if (id != NULL) {
		P("Per-app pin: %s -> '%s' — that app's controller / hand / gamepad roles prefer it for every\n", app,
		  id);
		P("family it supplies (other families keep the default walk; an absent provider falls through).\n");
	} else {
		P("Per-app pin for %s cleared — the app's own request / the default apply again.\n", app);
	}
	if (target_component_policy_lock(target_input_plugin_component_class())) {
		P("note: HKLM Policy\\Lock is set, so per-app pins are ignored on this machine.\n");
	}
	// TODO(#2181 phase 2): the per-client overlay applies the pin live; until it lands
	// the pin is stored and read by nothing that changes roles.
	note_pickup(NULL);
	return 0;
}

/*!
 * `input emulated on|off [--machine]` — the `EmulatedControllers` gate (ADR-054
 * N7, #2143). Same store rules as `enable|disable`: HKCU by default, the HKLM
 * 64-bit view with --machine, HKCU wins unless `Policy\Lock`; POSIX writes the
 * per-user `emulated_controllers` file and refuses --machine like the other
 * verbs. The pickup rule differs in one way, said below: the gate can only be
 * applied live to a pair that EXISTS (the provider set, qwerty's pair
 * included, is fixed per process).
 */
static int
cmd_emulated(const char *arg, bool machine)
{
	bool on;
	if (arg != NULL && strcmp(arg, "on") == 0) {
		on = true;
	} else if (arg != NULL && strcmp(arg, "off") == 0) {
		on = false;
	} else {
		P("error: 'input emulated' takes on|off.\n\n");
		return -1;
	}
	const int rc = write_result(store_set_emulated(on, machine),
	                            machine ? "writing the machine-wide EmulatedControllers value"
	                                    : "writing the per-user EmulatedControllers value",
	                            machine);
	if (rc != 0) {
		return rc;
	}
#ifdef XRT_OS_WINDOWS
	P("Emulated controllers: %s %s.\n", on ? "on" : "off",
	  machine ? "machine-wide (HKLM)" : "for this user (HKCU)");
	if (machine) {
		P("note: a per-user (HKCU) EmulatedControllers value still wins for that user (unless "
		  "InputProviders\\Policy\\Lock = 1).\n");
	}
#else
	char path[600] = {0};
	(void)emulated_file_path(path, sizeof(path), false);
	P("Emulated controllers: %s (%s).\n", on ? "on" : "off", path);
#endif
	if (on) {
		P("The qwerty left/right pair is published and is the hand-role floor again.\n");
	} else {
		P("The qwerty left/right pair is not published: the hand roles stay unassigned (XR_NULL_PATH)\n");
		P("when no input provider holds them. The qwerty head (look-around) is unaffected.\n");
	}
	P("A pair that exists in the running service is %s live; a pair the service did not create at\n",
	  on ? "re-admitted" : "withdrawn");
	P("start (it started with the gate off) joins at its next start.\n");
	note_pickup(NULL);
	return 0;
}


/*
 *
 * haptic-test
 *
 */

/*!
 * `input haptic-test [seconds]` — headless haptics driver for the
 * net_input round-trip (#823 Phase 2). Builds the runtime system
 * in-process (which hosts the active input provider, e.g. the net_input
 * hub), then fires a vibration on the left/right role devices twice a
 * second for the window. Pair with
 * `scripts/net_input_feeder.py --assert-haptic`: the feeder streams
 * poses in and must see the HAPTIC events come back. Exits 0 iff both
 * hand roles existed to fire at.
 */
static int
cmd_haptic_test(int argc, const char **argv)
{
	int seconds = 5;
	if (argc >= 4) {
		int v = atoi(argv[3]);
		if (v > 0 && v <= 300) {
			seconds = v;
		}
	}

	struct cli_query_result r;
	struct cli_query_handles h;
	cli_query_fill(&r, &h, NULL);
	if (!r.system_ok) {
		P("FAIL: system creation failed — cannot run haptic test.\n");
		cli_query_teardown(&h);
		return 1;
	}

	// Roles are re-resolved every iteration, not captured once: they are
	// arbitrated against provider presence (`target_input_arbiter.h`) and
	// for net_input "present" means "a feeder is connected" — which
	// normally happens AFTER the runtime is up, i.e. after this loop has
	// already started. Re-reading is also what a real app does (every
	// xrSyncActions), so this doubles as a live check of the flip.
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	struct xrt_device *left = NULL;
	struct xrt_device *right = NULL;

	P("Firing haptics on the hand-role devices for %d s (2 Hz)...\n", seconds);

	struct xrt_output_value value = {0};
	uint32_t tracked_samples = 0;
	uint32_t fired = 0;
	uint64_t last_generation = 0;
	for (int i = 0; i < seconds * 2; i++) {
		xrt_system_devices_get_roles(h.xsysd, &roles);
		left =
		    (roles.left >= 0 && (uint32_t)roles.left < h.xsysd->xdev_count) ? h.xsysd->xdevs[roles.left] : NULL;
		right = (roles.right >= 0 && (uint32_t)roles.right < h.xsysd->xdev_count) ? h.xsysd->xdevs[roles.right]
		                                                                          : NULL;
		if (roles.generation_id != last_generation) {
			last_generation = roles.generation_id;
			P("roles (generation %u): left='%s' right='%s'\n", (unsigned)roles.generation_id,
			  left != NULL ? left->str : "<none>", right != NULL ? right->str : "<none>");
		}
		if (left == NULL || right == NULL) {
			os_nanosleep(500 * 1000 * 1000);
			continue;
		}
		fired++;

		value.vibration.amplitude = 0.25f + 0.75f * (float)(i % 4) / 3.0f;
		value.vibration.frequency = 160.0f;
		value.vibration.duration_ns = 100 * 1000 * 1000;
		xrt_device_set_output(left, XRT_OUTPUT_NAME_SIMPLE_VIBRATION, &value);
		xrt_device_set_output(right, XRT_OUTPUT_NAME_SIMPLE_VIBRATION, &value);

		// Exercise the pose path too: predict the left grip slightly
		// into the future, exactly like a frame loop would.
		xrt_device_update_inputs(left);
		struct xrt_space_relation rel = {0};
		int64_t at = (int64_t)os_monotonic_get_ns() + 10 * 1000 * 1000;
		if (xrt_device_get_tracked_pose(left, XRT_INPUT_SIMPLE_GRIP_POSE, at, &rel) == XRT_SUCCESS &&
		    (rel.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0) {
			tracked_samples++;
			P("left grip @+10ms: (%.3f, %.3f, %.3f)\n", (double)rel.pose.position.x,
			  (double)rel.pose.position.y, (double)rel.pose.position.z);
		}

		os_nanosleep(500 * 1000 * 1000);
	}

	P("Done — fired %u haptic event(s) per hand; %u tracked pose sample(s).\n", fired, tracked_samples);
	cli_query_teardown(&h);
	if (fired == 0) {
		P("FAIL: the hand roles were never populated during the window.\n");
		return 1;
	}
	return 0;
}


/*
 *
 * Entry.
 *
 */

static int
print_usage(void)
{
	P("Usage: displayxr-cli input <list|enable|disable|use|reset|emulated|haptic-test>\n");
	P("  list [--json]                Registered input providers: ProbeOrder, Enabled (+ source),\n");
	P("                               the PreferredPlugin override, and the one ranked first ('*').\n");
	P("  enable | disable <id> [--machine]\n");
	P("                               Enabled on the provider's row: per user (HKCU, no admin) by\n");
	P("                               default, machine-wide (HKLM, admin) with --machine; HKCU wins.\n");
	P("  use <id> [--machine]         PreferredPlugin override: <id> is ranked first, ahead of\n");
	P("                               ProbeOrder (per user; --machine = HKLM).\n");
	P("  reset [--machine]            Clear the PreferredPlugin override.\n");
	P("  use <id> --app <exe>         Per-app pin: that app's controller / hand / gamepad roles prefer <id>\n");
	P("                               (families <id> supplies), ahead of the app's own request (per user).\n");
	P("  reset --app <exe>            Clear that app's pin.\n");
	P("  emulated on | off [--machine]\n");
	P("                               EmulatedControllers gate: off = no qwerty left/right pair, hand\n");
	P("                               roles unassigned when no provider holds them (per user; --machine\n");
	P("                               = HKLM). The qwerty head (look-around) stays either way.\n");
	cli_component_options_usage(&(const struct cli_component_class){
	    .verb = "input", .noun = "input provider", .cls = target_input_plugin_component_class()});
	P("  haptic-test [seconds]        Headless: fire vibrations on the hand-role devices (default 5 s).\n");
	P("                               Pair with scripts/net_input_feeder.py --assert-haptic.\n");
	P("A running service re-ranks at once (roles move on the next xrSyncActions); a provider it did not\n");
	P("load joins at its next start; settings are read at provider init (restart to apply). POSIX:\n");
	P("use/reset write the per-user input-preferred file; enable/disable = the manifest's plugin.enabled;\n");
	P("set/unset write the per-user <id>.settings.json (--machine = the manifest's settings object, by hand).\n");
	return 1;
}

int
cli_cmd_input(int argc, const char **argv)
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
			P("error: 'input %s' needs a provider id.\n\n", verb);
			return print_usage();
		}
		return cmd_enable(argv[3], strcmp(verb, "enable") == 0, machine);
	}
	const bool per_app = cli_has_flag(argc, argv, "--app");
	if (strcmp(verb, "use") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'input use' needs a provider id.\n\n");
			return print_usage();
		}
		if (per_app) {
			return cmd_app_pin(argv[3], flag_value(argc, argv, "--app"), machine);
		}
		return cmd_use(argv[3], machine);
	}
	if (strcmp(verb, "reset") == 0) {
		if (per_app) {
			return cmd_app_pin(NULL, flag_value(argc, argv, "--app"), machine);
		}
		return cmd_reset(machine);
	}
	if (strcmp(verb, "emulated") == 0) {
		const int rc = cmd_emulated(argc >= 4 ? argv[3] : NULL, machine);
		return rc < 0 ? print_usage() : rc;
	}
	if (strcmp(verb, "haptic-test") == 0) {
		return cmd_haptic_test(argc, argv);
	}
	const int rc = cmd_options_verbs(argc, argv); // options | set | unset (N3c)
	if (rc >= 0) {
		return rc;
	}
	P("error: unknown 'input' subcommand '%s'.\n\n", verb);
	return print_usage();
}
