// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `dp` subcommand — display processors (ADR-019): the four component
 *         verbs of ADR-054 (`list`, `enable|disable`, `use|reset`), globally
 *         or per screen.
 *
 *   dp list [--json]   Enumerate registered display-processor plug-ins with
 *                      their ProbeOrder, Enabled (+ where it came from), the
 *                      PreferredPlugin override, provenance, which one the
 *                      loader would select, and every screen's effective /
 *                      preferred plug-in.
 *   dp enable | disable <id> [--machine]
 *                      `Enabled` on the plug-in's row: per user (HKCU, no
 *                      admin) by default, machine-wide (HKLM, admin) with
 *                      --machine. HKCU wins. A disabled plug-in is never
 *                      loaded by any scan (N3b).
 *   dp use <id> [--machine]
 *                      The global PreferredPlugin override (the loader tries
 *                      <id> before the ProbeOrder sort): per user (HKCU) by
 *                      default, HKLM with --machine.
 *   dp reset [--machine]
 *                      Clear the global override (restore normal ProbeOrder).
 *   Every write asks the running service to re-probe (#2051): a secondary
 *   screen's segment DP follows now, the primary screen and in-process apps
 *   at their next session (no live swap, plugin-discovery.md §4.2).
 *   dp options <id> [--json]
 *                      The plug-in's settings: declared (describe_settings /
 *                      manifest settings_schema) + stored, with the effective
 *                      value and its source (N3c, ADR-054 decision 7).
 *   dp set <id> <KEY> <value> [--machine] / dp unset <id> <KEY> [--machine]
 *                      `Settings\<KEY>` on the plug-in's row: HKCU by default,
 *                      HKLM with --machine (POSIX: the per-user
 *                      `<id>.settings.json`). DXR_DP_<ID>_<KEY> in the hosting
 *                      process's environment always wins. The plug-in reads
 *                      through the host iface at init, so a write applies to
 *                      the next session / service start.
 *
 *   dp use <id> --screen <key> [--machine]
 *   dp reset --screen <key>|all [--machine]
 *                      The per-screen preference (display dashboard phase 7):
 *                      <id> weaves the screen whose stable key is <key> (see
 *                      `dp list` / `displays --claims`). Per-user by default
 *                      (no admin; the dashboard writes the same store),
 *                      machine-wide (HKLM, admin) with --machine. The running
 *                      service is asked to re-probe right away.
 *
 * Registry writes follow `input enable|use` (N3a, the newer pattern): HKCU
 * by default, HKLM 64-bit view with `--machine`; the loader reads HKCU over
 * HKLM for both values (`target_plugin_loader.c`). Only `Enabled` and
 * `PreferredPlugin` are ever read from HKCU — the plug-in rows (`Binary`,
 * `ProbeOrder`, …) stay HKLM-only, so a per-user key cannot inject a DLL
 * (plugin-discovery.md §2.1).
 *
 * POSIX: `use` / `reset` write the per-user `preferred` file (spec §3.3,
 * `target_plugin_set_preferred`); `enable|disable` has no per-user store —
 * the manifest's `plugin.enabled` is the switch, as for input and
 * conversion providers — and says so.
 *
 * Every write asks the running service to re-probe (#2051), the same call
 * the per-screen preference makes: a secondary screen's segment DP follows
 * at once, the primary screen and in-process apps at their next session.
 *
 * Vendor-neutral throughout (ADR-019): enumeration reads the discovery root
 * without loading any plug-in DLL, so no vendor symbols are touched. `dp list`
 * also builds the headless status snapshot for its `screens[]`, which loads the
 * registered plug-ins (the same exposure as `status` / `displays --claims`).
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_component_options.h"
#include "cli_dp_predict.h"
#include "cli_ipc.h"

#include "target_component_loader.h"
#include "target_plugin_loader.h"
#include "target_screen_pin.h"

#include "os/os_display_edid.h"
#include "util/u_logging.h"
#include "util/u_setting.h"
#include "util/u_status_snapshot.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_results.h"

#ifdef CLI_HAVE_IPC
#include "xrt/xrt_instance.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client.h"
#include "ipc_client_generated.h"
#endif

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MAX_DPS 16

//! Discovery root — the registry key the loader enumerates (Windows).
#define DP_REG_ROOT "Software\\DisplayXR\\DisplayProcessors"

#define P(...) printf(__VA_ARGS__)


//! The value of `--name <value>` in argv[2..], or NULL.
static const char *
flag_value(int argc, const char **argv, const char *name)
{
	for (int i = 2; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return NULL;
}

static void
add_str_or_null(cJSON *o, const char *key, const char *s)
{
	if (s == NULL || s[0] == '\0') {
		cJSON_AddNullToObject(o, key);
	} else {
		cJSON_AddStringToObject(o, key, s);
	}
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_plugin_desc list[MAX_DPS];
	int n = target_plugin_enumerate(list, MAX_DPS);

	char preferred[64] = {0};
	bool have_pref = target_plugin_get_preferred(preferred, sizeof(preferred));
	int active = cli_dp_predict_active(list, n, have_pref ? preferred : NULL);

	// Every screen's effective and preferred plug-in, as a process starting
	// now resolves them (the headless snapshot: same registry, same rules).
	struct xrt_status_snapshot *snap = calloc(1, sizeof(*snap));
	if (snap != NULL) {
		cli_status_build_headless(snap);
	}
	const uint32_t ns = snap != NULL ? snap->screen_count : 0;

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
			cJSON_AddBoolToObject(p, "enabled", list[i].enabled);
			cJSON_AddStringToObject(p, "enabled_source", list[i].enabled_source);
			// ADR-054 N4b, as the lister classified the binary.
			cJSON_AddStringToObject(p, "provenance", target_component_provenance_str(list[i].provenance));
			cJSON_AddStringToObject(p, "provenance_reason", list[i].provenance_reason);
			cJSON_AddStringToObject(p, "signer", list[i].signer);
			cJSON_AddBoolToObject(p, "active", i == active);
			cJSON_AddBoolToObject(p, "preferred", have_pref && strcmp(list[i].id, preferred) == 0);
			cJSON_AddItemToArray(arr, p);
		}
		cJSON *sarr = cJSON_AddArrayToObject(root, "screens");
		for (uint32_t i = 0; i < ns && i < XRT_STATUS_MAX_SCREENS; i++) {
			const struct xrt_status_screen *s = &snap->screens[i];
			cJSON *o = cJSON_CreateObject();
			add_str_or_null(o, "key", s->key);
			cJSON_AddStringToObject(o, "device_name", s->device_name);
			cJSON_AddStringToObject(o, "friendly_name", s->friendly_name);
			add_str_or_null(o, "effective_plugin", s->claim.plugin_id);
			add_str_or_null(o, "preferred_plugin", s->claim.preferred_plugin);
			add_str_or_null(o, "preferred_source", u_status_pref_source_str(s->claim.preferred_source));
			cJSON_AddBoolToObject(o, "forced", s->claim.forced);
			cJSON_AddStringToObject(o, "apply", u_status_apply_str(s->claim.apply));
			// Every plug-in that claimed this screen, at any confidence: what
			// can drive it (the dashboard's per-screen selector). Absent when
			// the resolve did not ask every plug-in (POSIX active-only shortcut).
			struct target_plugin_screen_candidate cands[MAX_DPS];
			uint32_t nc = 0;
			if (target_plugin_get_monitor_candidates(s->id, cands, MAX_DPS, &nc)) {
				cJSON *carr = cJSON_AddArrayToObject(o, "candidates");
				for (uint32_t k = 0; k < nc; k++) {
					cJSON *c = cJSON_CreateObject();
					cJSON_AddStringToObject(c, "plugin_id", cands[k].plugin_id);
					cJSON_AddNumberToObject(c, "confidence", (double)cands[k].confidence);
					cJSON_AddItemToArray(carr, c);
				}
			}
			cJSON_AddItemToArray(sarr, o);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		free(snap);
		return 0;
	}

	P(" :: Registered display processors\n");
	if (n == 0) {
		P("\t(none — discovery root absent or empty)\n");
	} else {
		P("\tPreferredPlugin override: %s\n", have_pref ? preferred : "<unset>");
		const int pref_at = have_pref ? cli_dp_find(list, n, preferred) : -1;
		if (have_pref && pref_at < 0) {
			P("\t  (not a registered plug-in — the loader ignores it)\n");
		} else if (pref_at >= 0 && !list[pref_at].enabled) {
			P("\t  (disabled — the loader falls through to ProbeOrder)\n");
		}
		P("\t  %-20s %-24s %10s  %-18s %-10s %s\n", "id", "name", "ProbeOrder", "Enabled", "Preferred",
		  "Provenance");
		for (int i = 0; i < n; i++) {
			char enabled[32];
			snprintf(enabled, sizeof(enabled), "%s (%s)", list[i].enabled ? "yes" : "no",
			         list[i].enabled_source);
			P("\t%s %-20s %-24s %10u  %-18s %-10s %s\n", i == active ? "*" : " ", list[i].id,
			  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order, enabled,
			  (have_pref && strcmp(list[i].id, preferred) == 0) ? "yes" : "-",
			  target_component_provenance_str(list[i].provenance));
			P("\t    %s\n", list[i].binary_path);
		}
		P("\t('*' = the plug-in the loader would select; a disabled one is never tried.)\n");
	}

	P(" :: Screens (per-screen preference: dp use <id> --screen <key>)\n");
	if (ns == 0) {
		P("\t(no screen enumerated)\n");
	}
	for (uint32_t i = 0; i < ns && i < XRT_STATUS_MAX_SCREENS; i++) {
		const struct xrt_status_screen *s = &snap->screens[i];
		const char *src = u_status_pref_source_str(s->claim.preferred_source);
		P("\t%s  %s '%s'\n", s->key[0] != '\0' ? s->key : "(no key)", s->device_name, s->friendly_name);
		P("\t    effective='%s'%s  preferred=%s%s%s%s  applies %s\n",
		  s->claim.plugin_id[0] != '\0' ? s->claim.plugin_id : "(unclaimed)",
		  s->claim.forced ? " (forced)" : "",
		  s->claim.preferred_plugin[0] != '\0' ? s->claim.preferred_plugin : "<none>", src != NULL ? " (" : "",
		  src != NULL ? src : "", src != NULL ? ")" : "", u_status_apply_str(s->claim.apply));
	}
	free(snap);
	return 0;
}

/*
 *
 * enable | disable | use | reset (N3b) — the global values.
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
		snprintf(path, sizeof(path), "%s\\%s", DP_REG_ROOT, sub);
	} else {
		snprintf(path, sizeof(path), "%s", DP_REG_ROOT);
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

static enum store_write
store_from_xret(xrt_result_t xret)
{
	if (xret == XRT_SUCCESS) {
		return STORE_WRITE_OK;
	}
	if (xret == XRT_ERROR_NOT_AUTHORIZED) {
		return STORE_WRITE_DENIED;
	}
	return xret == XRT_ERROR_NOT_IMPLEMENTED ? STORE_WRITE_UNSUPPORTED : STORE_WRITE_FAILED;
}

//! The loader's own per-user `preferred` file (spec §3.3); no machine store.
static enum store_write
store_set_preferred(const char *id, bool machine)
{
	if (machine) {
		return STORE_WRITE_UNSUPPORTED;
	}
	return store_from_xret(target_plugin_set_preferred(id));
}

static enum store_write
store_clear_preferred(bool machine)
{
	if (machine) {
		return STORE_WRITE_UNSUPPORTED;
	}
	return store_from_xret(target_plugin_clear_preferred());
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
			  "      Set \"enabled\": false in the plug-in's manifest (`plugin.enabled`,\n"
			  "      NNN-<id>.json in the DisplayProcessors dir) instead.\n",
			  what);
		}
		return 1;
	case STORE_WRITE_FAILED:
	default: P("FAIL: %s failed.\n", what); return 1;
	}
}

static bool
plugin_registered(const char *id);

//! #2051: ask the running service to re-probe (its policy watch also catches a
//! write made without the CLI); says when the change applies otherwise.
static void
poke_service(void);
//! A plug-in reads its settings at init through the host iface (N3c): same pickup rule as above.
static void
note_settings_pickup(void)
{
	P("The plug-in reads its settings at init through the host iface — a running service /\n");
	P("session keeps what it read; restart the DisplayXR service (or launch a fresh app).\n");
}

//! The options | set | unset verbs over the display-processor class (shared logic, cli_component_options.c).
static int
cmd_options_verbs(int argc, const char **argv)
{
	const struct cli_component_class k = {
	    .verb = "dp",
	    .noun = "display processor",
	    .cls = target_plugin_component_class(),
	    .registered = plugin_registered,
	    .describe = target_plugin_describe_settings,
	    .note_pickup = note_settings_pickup,
	    .posix_machine_hint = "the manifest's \"settings\" object (NNN-<id>.json in the DisplayProcessors dir)",
	};
	return cli_component_options_dispatch(&k, argc, argv);
}

static int
cmd_enable(const char *id, bool enable, bool machine)
{
	if (!cli_dp_id_valid(id)) {
		P("error: '%s' is not a plug-in id.\n", id != NULL ? id : "");
		return 1;
	}
	if (!plugin_registered(id)) {
		// A per-user Enabled for an unregistered id would be an orphan row
		// that `dp list` can never show: refuse, write nothing.
		P("FAIL: '%s' is not a registered display processor (see 'dp list') — nothing written.\n", id);
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
	if (!enable) {
		P("note: no scan loads '%s' while it is disabled — the per-monitor DP registry stops offering it "
		  "too.\n",
		  id);
	}
	poke_service();
	return 0;
}

static int
cmd_use(const char *id, bool machine)
{
	if (!cli_dp_id_valid(id)) {
		P("error: '%s' is not a plug-in id.\n", id != NULL ? id : "");
		return 1;
	}
	if (!plugin_registered(id)) {
		P("warning: '%s' is not a registered display processor; the preference is ignored until it is.\n", id);
	}
	const int rc = write_result(store_set_preferred(id, machine), "writing the PreferredPlugin override", machine);
	if (rc != 0) {
		return rc;
	}
#ifdef XRT_OS_WINDOWS
	P("%s PreferredPlugin set to '%s' — tried first, ahead of ProbeOrder.\n", machine ? "Machine-wide" : "Per-user",
	  id);
	if (machine) {
		P("note: a per-user (HKCU) PreferredPlugin still wins for that user.\n");
	}
#else
	P("PreferredPlugin set to '%s' (per-user 'preferred' file) — tried first, ahead of ProbeOrder.\n", id);
#endif
	poke_service();
	return 0;
}

static int
cmd_reset(bool machine)
{
	const int rc = write_result(store_clear_preferred(machine), "clearing the PreferredPlugin override", machine);
	if (rc != 0) {
		return rc;
	}
	P("%s PreferredPlugin override cleared — normal ProbeOrder discovery restored.\n",
	  machine ? "Machine-wide" : "Per-user");
	poke_service();
	return 0;
}


/*
 *
 * Per-screen preference (display dashboard phase 7).
 *
 */

/*!
 * Ask the running service to re-probe now (session-free DIAG RPC
 * `system_request_display_reprobe`), so the change applies without waiting
 * for a world event. Says what happened.
 */
static void
poke_service(void)
{
#ifdef CLI_HAVE_IPC
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	struct ipc_connection ipc_c;
	memset(&ipc_c, 0, sizeof(ipc_c));
	if (cli_ipc_connect(&ipc_c, U_LOGGING_RAW, &ii) == XRT_SUCCESS) {
		const xrt_result_t xret = ipc_call_system_request_display_reprobe(&ipc_c);
		ipc_client_connection_fini(&ipc_c);
		if (xret == XRT_SUCCESS) {
			P("The running DisplayXR service re-probes now: a secondary screen's segment DP\n");
			P("follows within ~1 s; the primary screen (and in-process apps) at the next session.\n");
			return;
		}
		P("The running DisplayXR service refused the re-probe request (xrt_result=%d) — applies at the "
		  "next session.\n",
		  (int)xret);
		return;
	}
#endif
	P("No DisplayXR service reachable — applies at the next session (in-process apps: their next launch).\n");
}

//! Is @p key a connected screen's key? (Enumeration only — no plug-in is loaded.)
static bool
screen_key_connected(const char *key)
{
	struct os_display_edid_list edid;
	memset(&edid, 0, sizeof(edid));
	os_display_edid_enumerate(&edid);
	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	const uint32_t dn = target_plugin_build_descriptors(&edid, descs, XRT_DP_REGISTRY_MAX_ENTRIES);
	for (uint32_t i = 0; i < dn; i++) {
		char k[TARGET_SCREEN_KEY_MAX];
		if (target_plugin_get_monitor_key(descs[i].monitor_id, k, sizeof(k), NULL, 0) && strcmp(k, key) == 0) {
			return true;
		}
	}
	return false;
}

//! Registered (HKLM row / manifest), enabled or not — enumeration only.
static bool
plugin_registered(const char *id)
{
	struct target_plugin_desc list[MAX_DPS];
	const int n = target_plugin_enumerate(list, MAX_DPS);
	return cli_dp_find(list, n, id) >= 0;
}

static int
machine_result(enum u_setting_write_result r, const char *what)
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

static int
cmd_use_screen(const char *id, const char *key, bool machine)
{
	if (strcmp(key, "all") == 0) {
		P("error: 'dp use --screen' needs one screen key ('all' is for 'dp reset').\n");
		return 1;
	}
	if (!screen_key_connected(key)) {
		P("note: no connected screen has the key '%s' (see 'dp list'); the preference is stored anyway.\n",
		  key);
	}
	if (!plugin_registered(id)) {
		P("note: '%s' is not a registered display processor; the preference is ignored until it is.\n", id);
	}
	if (machine) {
		const int rc = machine_result(u_setting_machine_set_preferred_plugin_for_screen(key, id),
		                              "writing the machine-wide per-screen preference");
		if (rc != 0) {
			return rc;
		}
		P("Machine-wide per-screen preference set: screen '%s' -> '%s' (a per-user preference for it still "
		  "wins).\n",
		  key, id);
	} else {
		if (!u_setting_user_set_preferred_plugin_for_screen(key, id)) {
			P("FAIL: could not write the per-user settings file (invalid key, or I/O error).\n");
			return 1;
		}
		char path[512] = {0};
		(void)u_setting_user_path(path, sizeof(path));
		P("Per-screen preference set: screen '%s' -> '%s' (%s).\n", key, id, path);
	}
	poke_service();
	return 0;
}

static int
cmd_reset_screen(const char *key, bool machine)
{
	const bool all = strcmp(key, "all") == 0;
	if (machine) {
		const int rc = machine_result(u_setting_machine_set_preferred_plugin_for_screen(all ? NULL : key, NULL),
		                              "clearing the machine-wide per-screen preference");
		if (rc != 0) {
			return rc;
		}
	} else {
		const bool ok = all ? u_setting_user_clear_preferred_plugin_per_screen()
		                    : u_setting_user_set_preferred_plugin_for_screen(key, NULL);
		if (!ok) {
			P("FAIL: could not update the per-user settings file.\n");
			return 1;
		}
	}
	P("%s per-screen preference%s cleared%s%s%s.\n", machine ? "Machine-wide" : "Per-user", all ? "s" : "",
	  all ? "" : " for screen '", all ? "" : key, all ? "" : "'");
	poke_service();
	return 0;
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli dp <list|enable|disable|use|reset> [--machine] [--screen <key>]\n");
	P("  list [--json]                 Registered display processors: ProbeOrder, Enabled (+ source), the\n");
	P("                                PreferredPlugin override, provenance, the one the loader would select\n");
	P("                                ('*'), and every screen's effective / preferred plug-in.\n");
	P("  enable | disable <id> [--machine]\n");
	P("                                Enabled on the plug-in's row: per user (HKCU, no admin) by default,\n");
	P("                                machine-wide (HKLM, admin) with --machine; HKCU wins. A disabled\n");
	P("                                plug-in is never loaded by any scan.\n");
	P("  use <id> [--machine]          Global PreferredPlugin override (e.g. 'sim-display', 'leia-sr'): tried\n");
	P("                                first, ahead of ProbeOrder (per user; --machine = HKLM).\n");
	P("  reset [--machine]             Clear the global override (restore normal ProbeOrder discovery).\n");
	cli_component_options_usage(&(const struct cli_component_class){
	    .verb = "dp", .noun = "display processor", .cls = target_plugin_component_class()});
	P("  use <id> --screen <key>       Weave the screen <key> (from 'dp list') with <id>; per-user, no admin.\n");
	P("  reset --screen <key>|all      Clear one / every per-screen preference.\n");
	P("    --machine                   With --screen: the machine-wide store (HKLM, admin) instead.\n");
	P("A running service re-probes at once (secondary screens follow; the primary at the next session).\n");
	P("POSIX: use/reset write the per-user 'preferred' file; enable/disable = the manifest's plugin.enabled.\n");
	return 1;
}

int
cli_cmd_dp(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	const char *verb = argv[2];
	const char *screen = flag_value(argc, argv, "--screen");
	const bool machine = cli_has_flag(argc, argv, "--machine");
	if (cli_has_flag(argc, argv, "--screen") && screen == NULL) {
		P("error: '--screen' needs a screen key (see 'dp list').\n\n");
		return print_usage();
	}
	if (strcmp(verb, "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(verb, "enable") == 0 || strcmp(verb, "disable") == 0) {
		if (screen != NULL) {
			P("error: 'dp %s' is global — there is no per-screen Enabled (use 'dp use --screen').\n\n",
			  verb);
			return print_usage();
		}
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'dp %s' needs a plug-in id.\n\n", verb);
			return print_usage();
		}
		return cmd_enable(argv[3], strcmp(verb, "enable") == 0, machine);
	}
	if (strcmp(verb, "use") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'dp use' needs a plug-in id.\n\n");
			return print_usage();
		}
		return screen != NULL ? cmd_use_screen(argv[3], screen, machine) : cmd_use(argv[3], machine);
	}
	if (strcmp(verb, "reset") == 0) {
		return screen != NULL ? cmd_reset_screen(screen, machine) : cmd_reset(machine);
	}
	const int rc = cmd_options_verbs(argc, argv); // options | set | unset (N3c)
	if (rc >= 0) {
		return rc;
	}
	P("error: unknown 'dp' subcommand '%s'.\n\n", verb);
	return print_usage();
}
