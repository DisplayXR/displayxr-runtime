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
#include "cli_input_predict.h"
#include "cli_ipc.h"
#include "cli_query.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include "target_component_loader.h"
#include "target_input_plugin_loader.h"

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
	char preferred[64] = {0};
	const bool have_pref = target_input_plugin_get_preferred(preferred, sizeof(preferred));
	int active = cli_input_predict_active(list, n, have_pref ? preferred : NULL);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddBoolToObject(root, "force_qwerty", force_qwerty);
		if (have_pref) {
			cJSON_AddStringToObject(root, "preferred", preferred);
		} else {
			cJSON_AddNullToObject(root, "preferred");
		}
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
	if (n == 0) {
		P("\t(none — discovery root absent or empty; qwerty keeps the hand roles)\n");
		return 0;
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

#else /* POSIX / Android */

static enum store_write
store_set_enabled(const char *id, bool enabled, bool machine)
{
	(void)id;
	(void)enabled;
	(void)machine;
	return STORE_WRITE_UNSUPPORTED;
}

//! `<per-user dir>/input-preferred` — the DP loader's `preferred` file pattern.
static bool
preferred_file_path(char *out, size_t cap, bool create_dir)
{
	char dir[512];
#ifdef XRT_OS_ANDROID
	(void)dir;
	(void)create_dir;
	out[0] = '\0';
	return false; // no per-user store on Android (no input providers there in v1 either)
#else
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	if (create_dir) {
		(void)mkdir(dir, 0755); // best-effort, single level
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
	if (machine) {
		P("note: a per-user (HKCU) Enabled value for '%s' still wins for that user.\n", id);
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
	P("Usage: displayxr-cli input <list|enable|disable|use|reset|haptic-test>\n");
	P("  list [--json]                Registered input providers: ProbeOrder, Enabled (+ source),\n");
	P("                               the PreferredPlugin override, and the one ranked first ('*').\n");
	P("  enable | disable <id> [--machine]\n");
	P("                               Enabled on the provider's row: per user (HKCU, no admin) by\n");
	P("                               default, machine-wide (HKLM, admin) with --machine; HKCU wins.\n");
	P("  use <id> [--machine]         PreferredPlugin override: <id> is ranked first, ahead of\n");
	P("                               ProbeOrder (per user; --machine = HKLM).\n");
	P("  reset [--machine]            Clear the PreferredPlugin override.\n");
	P("  haptic-test [seconds]        Headless: fire vibrations on the hand-role devices (default 5 s).\n");
	P("                               Pair with scripts/net_input_feeder.py --assert-haptic.\n");
	P("A running service re-ranks at once (roles move on the next xrSyncActions); a provider it did not\n");
	P("load joins at its next start. POSIX: use/reset write the per-user input-preferred file;\n");
	P("enable/disable = the manifest's plugin.enabled.\n");
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
	if (strcmp(verb, "use") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'input use' needs a provider id.\n\n");
			return print_usage();
		}
		return cmd_use(argv[3], machine);
	}
	if (strcmp(verb, "reset") == 0) {
		return cmd_reset(machine);
	}
	if (strcmp(verb, "haptic-test") == 0) {
		return cmd_haptic_test(argc, argv);
	}
	P("error: unknown 'input' subcommand '%s'.\n\n", verb);
	return print_usage();
}
