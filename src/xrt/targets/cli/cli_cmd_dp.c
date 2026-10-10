// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `dp` subcommand — list registered display processors and set or
 *         clear the non-destructive `PreferredPlugin` override, globally or
 *         per screen.
 *
 *   dp list [--json]   Enumerate registered display-processor plug-ins with
 *                      their ProbeOrder + which one the loader would select,
 *                      and every screen's effective / preferred plug-in.
 *   dp use <id>        Write the global PreferredPlugin override (the loader
 *                      tries <id> before the ProbeOrder sort). Requires a
 *                      service restart / fresh process to take effect.
 *   dp reset           Clear the global override (restore normal ProbeOrder).
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
 * Vendor-neutral throughout (ADR-019): enumeration reads the discovery root
 * without loading any plug-in DLL, so no vendor symbols are touched. `dp list`
 * also builds the headless status snapshot for its `screens[]`, which loads the
 * registered plug-ins (the same exposure as `status` / `displays --claims`).
 *
 * @author David Fattal
 */

#include "cli_common.h"

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

#include <stdio.h>
#include <stdlib.h>
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
	int active = predict_active(list, n, have_pref ? preferred : NULL);

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
		for (int i = 0; i < n; i++) {
			P("\t%s%s id='%s' name='%s' ProbeOrder=%u\n", i == active ? "* " : "  ",
			  (have_pref && strcmp(list[i].id, preferred) == 0) ? "[preferred]" : "           ", list[i].id,
			  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order);
			P("\t       %s\n", list[i].binary_path);
		}
		P("\t('*' = the plug-in the loader would select.)\n");
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
	if (ipc_client_connection_init(&ipc_c, U_LOGGING_RAW, &ii) == XRT_SUCCESS) {
		const xrt_result_t xret = ipc_call_system_request_display_reprobe(&ipc_c);
		ipc_client_connection_fini(&ipc_c);
		if (xret == XRT_SUCCESS) {
			P("The running DisplayXR service re-probes now: a secondary screen's segment DP\n");
			P("follows within ~1 s; the primary screen (and in-process apps) at the next session.\n");
			return;
		}
		P("The running DisplayXR service refused the re-probe request (xrt_result=%d) — applies on next "
		  "launch.\n",
		  (int)xret);
		return;
	}
#endif
	P("No DisplayXR service reachable — applies on next launch.\n");
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

static bool
plugin_registered(const char *id)
{
	struct target_plugin_desc list[MAX_DPS];
	const int n = target_plugin_enumerate(list, MAX_DPS);
	for (int i = 0; i < n; i++) {
		if (strcmp(list[i].id, id) == 0) {
			return true;
		}
	}
	return false;
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
	P("Usage: displayxr-cli dp <list|use <id>|reset> [--screen <key> [--machine]]\n");
	P("  list [--json]                 Registered display processors + every screen's effective / preferred\n");
	P("                                plug-in.\n");
	P("  use <id>                      Set the global PreferredPlugin override (e.g. 'sim-display', 'leia-sr').\n");
	P("  reset                         Clear the global override (restore normal ProbeOrder discovery).\n");
	P("  use <id> --screen <key>       Weave the screen <key> (from 'dp list') with <id>; per-user, no admin.\n");
	P("  reset --screen <key>|all      Clear one / every per-screen preference.\n");
	P("    --machine                   With --screen: the machine-wide store (HKLM, admin) instead.\n");
	return 1;
}

int
cli_cmd_dp(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	const char *screen = flag_value(argc, argv, "--screen");
	const bool machine = cli_has_flag(argc, argv, "--machine");
	if (cli_has_flag(argc, argv, "--screen") && screen == NULL) {
		P("error: '--screen' needs a screen key (see 'dp list').\n\n");
		return print_usage();
	}
	if (machine && screen == NULL) {
		P("error: '--machine' applies to a per-screen preference ('--screen <key>').\n\n");
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(argv[2], "use") == 0) {
		if (argc < 4 || argv[3][0] == '-') {
			P("error: 'dp use' needs a plug-in id.\n\n");
			return print_usage();
		}
		return screen != NULL ? cmd_use_screen(argv[3], screen, machine) : cmd_use(argv[3]);
	}
	if (strcmp(argv[2], "reset") == 0) {
		return screen != NULL ? cmd_reset_screen(screen, machine) : cmd_reset();
	}
	P("error: unknown 'dp' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
