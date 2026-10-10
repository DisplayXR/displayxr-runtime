// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `workspace` subcommand — the registered workspace controllers and
 *         their user-customizable launch settings (display dashboard phase 8).
 *
 *   workspace list [--json]
 *       Every controller registered under
 *       `HKLM\Software\DisplayXR\WorkspaceControllers` (POSIX: the JSON
 *       manifests), which one the service would spawn (`active_id`), whether it
 *       is connected to the running service right now, and its launch settings
 *       (mode + hotkey, with `source` "default" or "user").
 *   workspace set <id> [--hotkey <combo> | --no-hotkey] [--mode auto|disabled]
 *       Write the launch settings into service.json, then ask the running
 *       service to re-apply them (`system_reload_service_config`).
 *   workspace reset <id>
 *       Forget every customisation of <id> (Ctrl+Space, auto).
 *   workspace launch <id>
 *       Ask the running service to spawn <id> now, through the hotkey's own
 *       spawn path (`system_workspace_launch`).
 *   workspace hotkey-suspend on|off
 *       Take the service's launch-hotkey hook out of the input pipeline (on)
 *       or put it back (off) — for a hotkey-capture box that must see the
 *       current combo (`system_workspace_hotkey_suspend`). Not persisted; the
 *       service resumes by itself after 60 s (each `on` re-arms that).
 *
 * The runtime knows controllers only by role and registration — never by
 * product name (docs/specs/runtime/workspace-controller-registration.md).
 * service.json is per-user (%LOCALAPPDATA%\DisplayXR\service.json), so `set`
 * needs no admin.
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

// Platform headers first: the registry header falls back to its own MAX_PATH.
#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#elif defined(XRT_OS_LINUX)
#include <unistd.h>
#endif

#include "service_config.h"
#include "service_hotkey.h"
#include "service_workspace_registry.h"

#include "xrt/xrt_results.h"
#include "util/u_logging.h"

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


#define P(...) printf(__VA_ARGS__)
#define E(...) fprintf(stderr, __VA_ARGS__)


/*
 *
 * Service connection (DIAG)
 *
 */

#ifdef CLI_HAVE_IPC

//! Is a service listening? Checked BEFORE connecting, because the IPC client
//! auto-launches displayxr-service when the pipe is missing — a settings read
//! (the dashboard polls `workspace list`) must never start the service.
static bool
service_pipe_present(void)
{
#ifdef XRT_OS_WINDOWS
	if (WaitNamedPipeA("\\\\.\\pipe\\displayxr\\displayxr_comp_ipc", 1)) {
		return true;
	}
	return GetLastError() != ERROR_FILE_NOT_FOUND; // busy / timeout = it exists
#else
	return true; // POSIX dial never spawns (socket activation is the OS's job)
#endif
}

//! Connect as a DIAG client. False (and @p ipc_c untouched) when no service is
//! reachable: none running, an elevated prompt on Windows, a version skew.
static bool
diag_connect(struct ipc_connection *ipc_c)
{
	if (!service_pipe_present()) {
		return false;
	}
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	memset(ipc_c, 0, sizeof(*ipc_c));
	return ipc_client_connection_init(ipc_c, U_LOGGING_RAW, &ii) == XRT_SUCCESS;
}

//! Executable path of process @p pid, or false.
static bool
pid_exe_path(long pid, char *buf, size_t cap)
{
	buf[0] = '\0';
#ifdef XRT_OS_WINDOWS
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h == NULL) {
		return false;
	}
	DWORD len = (DWORD)cap;
	BOOL ok = QueryFullProcessImageNameA(h, 0, buf, &len);
	CloseHandle(h);
	return ok && len > 0;
#elif defined(__APPLE__)
	return proc_pidpath((int)pid, buf, (uint32_t)cap) > 0;
#elif defined(XRT_OS_LINUX)
	char link[64];
	snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
	ssize_t n = readlink(link, buf, cap - 1);
	if (n <= 0) {
		buf[0] = '\0';
		return false;
	}
	buf[n] = '\0';
	return true;
#else
	(void)pid;
	(void)cap;
	return false;
#endif
}

static bool
path_eq(const char *a, const char *b)
{
#ifdef XRT_OS_WINDOWS
	return _stricmp(a, b) == 0;
#else
	return strcmp(a, b) == 0;
#endif
}

/*!
 * Fill @p live (array of @p n) from the running service: a controller is
 * connected when a CONTROLLER-class client runs its binary. A CONTROLLER whose
 * exe cannot be read is attributed to the active controller (the service
 * verified its class against the registered binary, #960). Returns false when
 * no service is reachable (@p live left "unknown").
 */
static bool
fetch_live(const struct workspace_controller_entry *entries, int n, int active, struct service_controller_live *live)
{
	struct ipc_connection ipc_c;
	if (!diag_connect(&ipc_c)) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		live[i].known = true;
		live[i].connected = false;
		live[i].pid = 0;
	}

	struct ipc_client_list list;
	memset(&list, 0, sizeof(list));
	if (ipc_call_system_get_clients(&ipc_c, &list) == XRT_SUCCESS) {
		for (uint32_t c = 0; c < list.id_count && c < IPC_MAX_CLIENTS; c++) {
			struct ipc_app_state ias;
			memset(&ias, 0, sizeof(ias));
			if (ipc_call_system_get_client_info(&ipc_c, list.ids[c], &ias) != XRT_SUCCESS ||
			    ias.client_class != XRT_CLIENT_CLASS_CONTROLLER) {
				continue;
			}
			char exe[1024];
			int match = -1;
			if (pid_exe_path((long)ias.pid, exe, sizeof(exe))) {
				for (int i = 0; i < n; i++) {
					if (path_eq(exe, entries[i].binary)) {
						match = i;
						break;
					}
				}
			} else if (active >= 0) {
				match = active;
			}
			if (match >= 0) {
				live[match].connected = true;
				live[match].pid = (long)ias.pid;
			}
		}
	}
	ipc_client_connection_fini(&ipc_c);
	return true;
}

#endif // CLI_HAVE_IPC


/*
 *
 * Helpers
 *
 */

struct registry_view
{
	struct workspace_controller_entry entries[WORKSPACE_REGISTRY_MAX_ENTRIES];
	int n;
	int active; //!< index, -1 none, SERVICE_PICK_DEV_OVERRIDE
	struct service_config cfg;
};

static void
load_view(struct registry_view *v)
{
	memset(v, 0, sizeof(*v));
	service_config_load(&v->cfg);
	v->n = service_workspace_registry_enumerate(v->entries, WORKSPACE_REGISTRY_MAX_ENTRIES);
	v->active = service_config_pick_controller(&v->cfg, v->entries, v->n);
}

static int
find_controller(const struct registry_view *v, const char *id)
{
	for (int i = 0; i < v->n; i++) {
#ifdef XRT_OS_WINDOWS
		if (_stricmp(v->entries[i].id, id) == 0) {
#else
		if (strcmp(v->entries[i].id, id) == 0) {
#endif
			return i;
		}
	}
	return -1;
}

static void
print_cjson(cJSON *root)
{
	char *out = cJSON_Print(root);
	if (out != NULL) {
		printf("%s\n", out);
		cJSON_free(out);
	}
	fflush(stdout);
}

/*!
 * Ask the running service to re-apply service.json. Says what happened.
 */
static void
reload_service(void)
{
#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (diag_connect(&ipc_c)) {
		const xrt_result_t xret = ipc_call_system_reload_service_config(&ipc_c);
		ipc_client_connection_fini(&ipc_c);
		if (xret == XRT_SUCCESS) {
			P("The running DisplayXR service applied it (no restart of a running controller).\n");
			return;
		}
		P("The running DisplayXR service did not apply it (xrt_result=%d) — applies on next service start.\n",
		  (int)xret);
		return;
	}
#endif
	P("No DisplayXR service reachable — applies on next service start.\n");
}


/*
 *
 * Verbs
 *
 */

static int
cmd_list(bool json)
{
	struct registry_view v;
	load_view(&v);

	struct service_controller_live live[WORKSPACE_REGISTRY_MAX_ENTRIES];
	memset(live, 0, sizeof(live));
	bool have_live = false;
#ifdef CLI_HAVE_IPC
	have_live = fetch_live(v.entries, v.n, v.active, live);
#endif

	cJSON *root = (cJSON *)service_workspace_controllers_to_cjson(&v.cfg, v.entries, v.n, have_live ? live : NULL);
	if (root == NULL) {
		E("FAIL: out of memory.\n");
		return 1;
	}
	if (json) {
		print_cjson(root);
		cJSON_Delete(root);
		return 0;
	}

	if (v.n == 0) {
		P("No workspace controllers registered.\n");
	}
	if (v.active == SERVICE_PICK_DEV_OVERRIDE) {
		P("Dev override (service.json workspace_binary): %s\n", v.cfg.workspace_binary);
	}
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "controllers");
	const cJSON *o = NULL;
	int i = 0;
	cJSON_ArrayForEach(o, arr)
	{
		const cJSON *launch = cJSON_GetObjectItemCaseSensitive(o, "launch");
		const cJSON *hk = cJSON_GetObjectItemCaseSensitive(launch, "hotkey");
		P("%s %-16s %s (%s %s)\n", i == v.active ? "*" : " ", v.entries[i].id, v.entries[i].display_name,
		  v.entries[i].vendor[0] ? v.entries[i].vendor : "-",
		  v.entries[i].version[0] ? v.entries[i].version : "-");
		P("    binary:  %s\n", v.entries[i].binary);
		P("    launch:  mode=%s hotkey=%s (%s)\n",
		  cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(launch, "mode")),
		  cJSON_IsString(hk) ? hk->valuestring : "none",
		  cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(launch, "source")));
		if (have_live) {
			if (live[i].connected) {
				P("    service: connected (pid %ld)\n", live[i].pid);
			} else {
				P("    service: not connected\n");
			}
		} else {
			P("    service: not reachable\n");
		}
		i++;
	}
	if (v.n > 0) {
		P("(* = the controller the service spawns)\n");
	}
	cJSON_Delete(root);
	return 0;
}

static int
cmd_set(int argc, const char **argv)
{
	if (argc < 4 || argv[3][0] == '-') {
		E("error: 'workspace set' needs a controller id (see 'workspace list').\n");
		return 1;
	}
	const char *id = argv[3];

	const char *hotkey = NULL;
	bool no_hotkey = false;
	const char *mode = NULL;
	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--hotkey") == 0) {
			if (i + 1 >= argc) {
				E("error: '--hotkey' needs a combo, e.g. \"Ctrl+Space\".\n");
				return 1;
			}
			hotkey = argv[++i];
		} else if (strcmp(argv[i], "--no-hotkey") == 0) {
			no_hotkey = true;
		} else if (strcmp(argv[i], "--mode") == 0) {
			if (i + 1 >= argc) {
				E("error: '--mode' needs auto|disabled.\n");
				return 1;
			}
			mode = argv[++i];
		} else {
			E("error: unknown option '%s'.\n", argv[i]);
			return 1;
		}
	}
	if (hotkey == NULL && !no_hotkey && mode == NULL) {
		E("error: nothing to set — give --hotkey <combo>, --no-hotkey and/or --mode auto|disabled.\n");
		return 1;
	}
	if (hotkey != NULL && no_hotkey) {
		E("error: --hotkey and --no-hotkey are exclusive.\n");
		return 1;
	}
	enum service_child_mode m = SERVICE_CHILD_AUTO;
	if (mode != NULL) {
		if (strcmp(mode, "auto") == 0) {
			m = SERVICE_CHILD_AUTO;
		} else if (strcmp(mode, "disabled") == 0) {
			m = SERVICE_CHILD_DISABLE;
		} else {
			E("error: --mode must be 'auto' or 'disabled' (got '%s').\n", mode);
			return 1;
		}
	}
	if (hotkey != NULL) {
		struct service_hotkey hk;
		if (!service_hotkey_parse(hotkey, &hk)) {
			E("error: '%s' is not a valid hotkey. Grammar: [Ctrl+][Shift+][Alt+][Win+]<Key>, at least one\n"
			  "       modifier; <Key> = Space, A-Z, 0-9, F1-F24, Tab, Enter, Backquote, Minus, Equals,\n"
			  "       BracketLeft, BracketRight, Semicolon, Quote, Comma, Period, Slash, Backslash, "
			  "Insert,\n"
			  "       Delete, Home, End, PageUp, PageDown, Left, Up, Right, Down.\n",
			  hotkey);
			return 1;
		}
	}

	struct registry_view v;
	load_view(&v);
	const int idx = find_controller(&v, id);
	if (idx < 0) {
		E("error: no workspace controller '%s' is registered (see 'workspace list').\n", id);
		return 1;
	}
	const char *canon_id = v.entries[idx].id; // registry spelling
	const bool is_active = idx == v.active;

	if ((hotkey != NULL || no_hotkey) &&
	    !service_config_set_controller_hotkey(&v.cfg, canon_id, no_hotkey ? NULL : hotkey)) {
		E("FAIL: could not record the hotkey (too many customised controllers?).\n");
		return 1;
	}
	if (mode != NULL && !service_config_set_controller_mode(&v.cfg, canon_id, m, is_active)) {
		E("FAIL: could not record the mode.\n");
		return 1;
	}
	if (!service_config_save(&v.cfg)) {
		E("FAIL: could not write service.json.\n");
		return 1;
	}

	struct service_launch_resolved r;
	service_config_resolve_launch(&v.cfg, canon_id, is_active, &r);
	char path[512] = "";
	(void)service_config_path(path, sizeof(path));
	P("Workspace controller '%s': mode=%s hotkey=%s (%s).\n", canon_id, service_config_launch_mode_str(r.mode),
	  r.has_hotkey ? r.hotkey_text : "none", path);
	if (!is_active) {
		P("note: '%s' is not the controller the service spawns (active: %s); its settings apply once it is.\n",
		  canon_id, v.active >= 0 ? v.entries[v.active].id : "none");
	}
	reload_service();
	return 0;
}

static int
cmd_reset(int argc, const char **argv)
{
	if (argc < 4 || argv[3][0] == '-') {
		E("error: 'workspace reset' needs a controller id (see 'workspace list').\n");
		return 1;
	}
	struct registry_view v;
	load_view(&v);
	const int idx = find_controller(&v, argv[3]);
	if (idx < 0) {
		E("error: no workspace controller '%s' is registered (see 'workspace list').\n", argv[3]);
		return 1;
	}
	service_config_reset_controller(&v.cfg, v.entries[idx].id, idx == v.active);
	if (!service_config_save(&v.cfg)) {
		E("FAIL: could not write service.json.\n");
		return 1;
	}
	P("Workspace controller '%s': launch settings back to the defaults (mode=auto hotkey=%s).\n", v.entries[idx].id,
	  SERVICE_HOTKEY_DEFAULT);
	reload_service();
	return 0;
}

static int
cmd_launch(int argc, const char **argv)
{
	if (argc < 4 || argv[3][0] == '-') {
		E("error: 'workspace launch' needs a controller id (see 'workspace list').\n");
		return 1;
	}
	struct registry_view v;
	load_view(&v);
	const int idx = find_controller(&v, argv[3]);
	if (idx < 0) {
		E("error: no workspace controller '%s' is registered (see 'workspace list').\n", argv[3]);
		return 1;
	}
	const char *id = v.entries[idx].id;

	// Local pre-checks give the clearest message; the service re-checks.
	if (idx != v.active) {
		E("Refused: '%s' is not the controller the service spawns (active: %s).\n", id,
		  v.active >= 0 ? v.entries[v.active].id : "none");
		return 1;
	}
	struct service_launch_resolved r;
	service_config_resolve_launch(&v.cfg, id, true, &r);
	if (r.mode == SERVICE_CHILD_DISABLE) {
		E("Refused: '%s' is disabled (displayxr-cli workspace set %s --mode auto).\n", id, id);
		return 1;
	}

#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (!diag_connect(&ipc_c)) {
		E("Refused: no DisplayXR service reachable (start it first; on Windows run from a non-elevated "
		  "prompt).\n");
		return 2;
	}
	struct ipc_workspace_controller_id cid;
	memset(&cid, 0, sizeof(cid));
	snprintf(cid.id, sizeof(cid.id), "%s", id);
	uint32_t status = IPC_WORKSPACE_LAUNCH_UNSUPPORTED;
	const xrt_result_t xret = ipc_call_system_workspace_launch(&ipc_c, &cid, &status);
	ipc_client_connection_fini(&ipc_c);
	if (xret != XRT_SUCCESS) {
		E("FAIL: the service refused the launch request (xrt_result=%d).\n", (int)xret);
		return 1;
	}
	switch (status) {
	case IPC_WORKSPACE_LAUNCH_STARTED: P("Launching workspace controller '%s'.\n", id); return 0;
	case IPC_WORKSPACE_LAUNCH_ALREADY_RUNNING: E("Refused: '%s' is already running.\n", id); return 1;
	case IPC_WORKSPACE_LAUNCH_DISABLED:
		E("Refused: '%s' is disabled in the running service (displayxr-cli workspace set %s --mode auto).\n",
		  id, id);
		return 1;
	case IPC_WORKSPACE_LAUNCH_NOT_ACTIVE:
		E("Refused: the running service does not spawn '%s' (its active controller differs).\n", id);
		return 1;
	case IPC_WORKSPACE_LAUNCH_NO_CONTROLLER:
		E("Refused: the running service sees no workspace controller.\n");
		return 1;
	default: E("Refused: the running service cannot launch workspace controllers.\n"); return 1;
	}
#else
	E("Refused: this displayxr-cli was built without IPC — no service to ask.\n");
	return 2;
#endif
}

static int
cmd_hotkey_suspend(int argc, const char **argv)
{
	if (argc < 4 || (strcmp(argv[3], "on") != 0 && strcmp(argv[3], "off") != 0)) {
		E("error: 'workspace hotkey-suspend' needs on|off.\n");
		return 1;
	}
	const bool suspend = strcmp(argv[3], "on") == 0;
#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (!diag_connect(&ipc_c)) {
		E("No DisplayXR service reachable — there is no hook to %s.\n", suspend ? "suspend" : "resume");
		return 2;
	}
	const xrt_result_t xret = ipc_call_system_workspace_hotkey_suspend(&ipc_c, suspend);
	ipc_client_connection_fini(&ipc_c);
	if (xret != XRT_SUCCESS) {
		E("FAIL: the service did not apply it (xrt_result=%d).\n", (int)xret);
		return 1;
	}
	if (suspend) {
		P("Launch hotkey suspended: the service's hook is out until 'workspace hotkey-suspend off' or 60 s.\n");
	} else {
		P("Launch hotkey resumed (re-installed from the current settings).\n");
	}
	return 0;
#else
	E("This displayxr-cli was built without IPC — no service to ask.\n");
	return 2;
#endif
}

static int
print_usage(void)
{
	E("Usage: displayxr-cli workspace <list|set|reset|launch|hotkey-suspend> ...\n");
	E("  list [--json]                       Registered workspace controllers, the active one, live state and\n");
	E("                                      launch settings.\n");
	E("  set <id> --hotkey <combo>           Launch hotkey, e.g. \"Ctrl+Space\" (the default), "
	  "\"Ctrl+Shift+F9\".\n");
	E("  set <id> --no-hotkey                No hotkey (launch from the tray or 'workspace launch').\n");
	E("  set <id> --mode auto|disabled       auto = on demand (default); disabled = never launched.\n");
	E("  reset <id>                          Back to the defaults (Ctrl+Space, auto).\n");
	E("  launch <id>                         Ask the running service to spawn <id> now.\n");
	E("  hotkey-suspend on|off               Take the service's launch-hotkey hook out / put it back\n");
	E("                                      (for a hotkey-capture box; auto-resumes after 60 s).\n");
	E("Combo grammar: [Ctrl+][Shift+][Alt+][Win+]<Key>, at least one modifier.\n");
	return 1;
}

int
cli_cmd_workspace(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(cli_has_flag(argc, argv, "--json"));
	}
	if (strcmp(argv[2], "set") == 0) {
		return cmd_set(argc, argv);
	}
	if (strcmp(argv[2], "reset") == 0) {
		return cmd_reset(argc, argv);
	}
	if (strcmp(argv[2], "launch") == 0) {
		return cmd_launch(argc, argv);
	}
	if (strcmp(argv[2], "hotkey-suspend") == 0) {
		return cmd_hotkey_suspend(argc, argv);
	}
	E("error: unknown 'workspace' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
