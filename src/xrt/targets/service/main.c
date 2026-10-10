// Copyright 2020, Collabora, Ltd.
// Copyright 2024-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main file for DisplayXR service.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc
 */

#include "xrt/xrt_config_os.h"

#include "util/u_metrics.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"
#include "util/u_file_logging.h"
#include "util/u_crash_guard.h"
#include <displayxr_mcp/mcp_server.h>

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#include "service_config.h"
#include "service_orchestrator.h"
#include "service_tray_win.h"
#include <stdlib.h> // __argc, __argv
#include <wchar.h>  // _snwprintf_s
#endif

#include "server/ipc_server_interface.h"
#include "server/ipc_server.h"
#include "service_client_class.h" // #960

#ifdef XRT_OS_WINDOWS
#include "shared/ipc_protocol.h" // enum ipc_workspace_launch_status

// Display dashboard phase 8: the orchestrator's launch result, mapped onto the
// system_workspace_launch wire status (registered as the provider below).
static uint32_t
workspace_launch_provider(const char *controller_id)
{
	switch (service_orchestrator_request_launch(controller_id)) {
	case SERVICE_LAUNCH_STARTED: return IPC_WORKSPACE_LAUNCH_STARTED;
	case SERVICE_LAUNCH_NOT_ACTIVE: return IPC_WORKSPACE_LAUNCH_NOT_ACTIVE;
	case SERVICE_LAUNCH_DISABLED: return IPC_WORKSPACE_LAUNCH_DISABLED;
	case SERVICE_LAUNCH_ALREADY_RUNNING: return IPC_WORKSPACE_LAUNCH_ALREADY_RUNNING;
	case SERVICE_LAUNCH_NO_CONTROLLER: return IPC_WORKSPACE_LAUNCH_NO_CONTROLLER;
	default: return IPC_WORKSPACE_LAUNCH_UNSUPPORTED;
	}
}
#endif

#include "target_lists.h"
#include "target_status_snapshot.h" // ADR-051 D3: the status provider

// #950: run ipc_server_main under the structured-exception guard so an
// exception escaping the main thread is recorded ([TERMINATE]) before it
// propagates exactly as before.
struct service_main_args
{
	int argc;
	char **argv;
	struct ipc_server_main_info *ismi;
	int ret;
};

static void *
service_main_body(void *p)
{
	struct service_main_args *a = (struct service_main_args *)p;
	a->ret = ipc_server_main(a->argc, a->argv, a->ismi);
	return NULL;
}

#include <string.h> // strcmp for --workspace flag

/*!
 * ADR-051 D3: the rows of the display status snapshot only this target can
 * fill (the loader + screen registry live in targets/common, above the IPC
 * server). Read lazily on DIAG status RPCs; never per frame.
 */
static void
register_status_provider(void)
{
	static const struct ipc_server_status_provider provider = {
	    .build = target_status_snapshot_build_service,
	    .change_key = target_status_snapshot_change_key,
	};
	ipc_server_set_status_provider(&provider);
}


// Insert the on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)


#ifdef XRT_OS_WINDOWS

// Optimus / PowerXpress hints: tell hybrid-GPU drivers to put this process on
// the discrete GPU. NVIDIA and AMD drivers look these up by name from the main
// executable. The service compositor also explicitly picks the high-performance
// DXGI adapter — these exports cover the rest of the driver-side selection.
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

// Shutdown flag defined in ipc_server_mainloop_windows.cpp.
// Set by the tray icon's "Exit" menu item, checked by ipc_server_mainloop_poll().
extern volatile bool g_service_shutdown_requested;

static void
tray_shutdown_callback(void)
{
	g_service_shutdown_requested = true;
}

static void
tray_config_change_callback(const struct service_config *new_cfg)
{
	service_orchestrator_apply_config(new_cfg);
}

static void
setup_dpi_awareness(void)
{
	// Per-monitor DPI awareness FIRST, before any window/display operation.
	// Without it Windows reports scaled LOGICAL resolution (2560x1440 on a 4K
	// panel at 150%) instead of physical, breaking the weaver, which needs true
	// pixel dimensions. Since #1201 the embedded manifest
	// (targets/common/dpi_aware.manifest) normally has this in force before a
	// single instruction of ours runs; this call is the backstop for builds
	// that could not embed one, and is a no-op when the manifest applied.
	u_win_make_process_dpi_aware(NULL);
}

/*
 * Restart Manager support. A third-party installer that must replace a DLL this
 * process holds (a vendor plug-in's dependency, loaded for the process lifetime)
 * finds us with RmGetList, closes us with RmShutdown (the session-end window in
 * service_tray_win.c) and brings us back with RmRestart, which relaunches the
 * command line registered here. See docs/architecture/service-architecture.md
 * § Restart Manager.
 */
#define SERVICE_RM_RESTART_ARG "--rm-restart"

//! Mandatory integrity RID of this process (0x2000 medium, 0x3000 high), 0 if unknown.
static DWORD
process_integrity_rid(void)
{
	DWORD rid = 0;
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return 0;
	}
	union {
		TOKEN_MANDATORY_LABEL tml;
		BYTE buf[64];
	} u;
	DWORD len = 0;
	if (GetTokenInformation(tok, TokenIntegrityLevel, &u, sizeof(u), &len)) {
		PSID sid = u.tml.Label.Sid;
		rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
	}
	CloseHandle(tok);
	return rid;
}

static bool
process_is_elevated(void)
{
	TOKEN_ELEVATION e = {0};
	DWORD len = 0;
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return false;
	}
	BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
	CloseHandle(tok);
	return ok && e.TokenIsElevated != 0;
}

/*!
 * A Restart-Manager restart is driven by an installer that is usually elevated.
 * The service must run at the user's normal integrity (clients at medium/low
 * integrity connect to it), so if the restarted instance came up elevated, hand
 * the launch to the desktop shell (`explorer.exe <exe>` starts it in the
 * shell's own, non-elevated context — the same recipe as the documented manual
 * restart) and bow out. Returns true when the hand-off was started.
 */
static bool
relaunch_unelevated_via_shell(void)
{
	wchar_t exe[MAX_PATH];
	wchar_t windir[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
	UINT w = GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
	if (n == 0 || n >= ARRAYSIZE(exe) || w == 0 || w >= ARRAYSIZE(windir)) {
		return false;
	}
	wchar_t cmd[2 * MAX_PATH + 32];
	if (_snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE, L"\"%ls\\explorer.exe\" \"%ls\"", windir, exe) < 0) {
		return false;
	}
	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {0};
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

//! Register the command line Restart Manager relaunches us with after an
//! installer closed us. Not for crash/hang (WER behaviour is unchanged) and not
//! across a reboot (the HKLM Run key already starts us at logon).
static void
register_for_restart(bool workspace_mode)
{
	// No --autostart: a restart restores a service that was running, so the
	// start-on-login preference (which gates only logon auto-starts) must not
	// turn it into an exit. The marker lets the restarted instance recognise
	// itself (integrity check above).
	const wchar_t *args = workspace_mode ? L"" SERVICE_RM_RESTART_ARG L" --workspace" : L"" SERVICE_RM_RESTART_ARG;
	HRESULT hr = RegisterApplicationRestart(args, RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);
	if (SUCCEEDED(hr)) {
		U_LOG_W("Registered for Restart Manager restart (args \"%ls\").", args);
	} else {
		U_LOG_W(
		    "RegisterApplicationRestart failed (hr=0x%08lx); installers can close but not restart the "
		    "service.",
		    (unsigned long)hr);
	}
}

/*
 * ADR-045 complete adoption. When a re-probe adopts a better display plug-in
 * under a head device the fallback created, the IPC server ends its main loop
 * once no client is connected (ipc_server_restart_requested) and this host
 * starts a successor that waits for this process to exit, then builds its
 * whole system on the new plug-in. A successor never restarts itself again
 * for the same reason (no restart loop if a plug-in flaps between probes).
 */
#define SERVICE_ADOPTION_RESTART_ARG "--adoption-restart-after-pid"

//! Start the successor instance (same exe, same integrity: this process's own
//! token, so a medium-integrity service stays medium). Returns true on success.
static bool
start_adoption_successor(bool workspace_mode)
{
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
	if (n == 0 || n >= ARRAYSIZE(exe)) {
		return false;
	}
	wchar_t cmd[MAX_PATH + 96];
	if (_snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE,
	                 L"\"%ls\" "
	                 L"" SERVICE_ADOPTION_RESTART_ARG L" %lu%ls",
	                 exe, (unsigned long)GetCurrentProcessId(), workspace_mode ? L" --workspace" : L"") < 0) {
		return false;
	}
	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {0};
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

//! Successor side: wait (bounded) for the predecessor to exit so the
//! singleton mutex and the IPC pipe are free.
static void
wait_for_predecessor(unsigned long pid)
{
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
	if (h == NULL) {
		return; // already gone
	}
	DWORD w = WaitForSingleObject(h, 30000);
	CloseHandle(h);
	U_LOG_W("Adoption restart: predecessor pid %lu %s.", pid,
	        w == WAIT_OBJECT_0 ? "exited" : "did not exit within 30 s; continuing");
}

// GUI subsystem entry point (no console window).
int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	setup_dpi_awareness();

	// Use CRT globals for argc/argv (available even in WinMain)
	int argc = __argc;
	char **argv = __argv;

	u_win_try_privilege_or_priority_from_args(U_LOGGING_INFO, argc, argv);

	// #950 exit/terminate tripwires. Bring the file logger up first so the
	// [EXIT] record lands in the log: atexit handlers run LIFO, so the tripwire
	// must register AFTER the logger's own shutdown handler.
	u_file_logging_init();
	u_crash_guard_install_exit_tripwire();

	// Load orchestrator config (workspace mode, start-on-login)
	struct service_config cfg;
	service_config_load(&cfg);

	// Parse flags. --workspace is the manual multi-terminal workflow (the
	// orchestrator will also enable workspace mode when it spawns the
	// workspace controller). --autostart is appended by the HKLM Run key
	// registration so we can tell a logon auto-start from a manual launch.
	bool workspace_mode = false;
	bool autostart = false;
	bool rm_restart = false;
	unsigned long adoption_predecessor_pid = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--workspace") == 0) {
			workspace_mode = true;
		} else if (strcmp(argv[i], "--autostart") == 0) {
			autostart = true;
		} else if (strcmp(argv[i], SERVICE_RM_RESTART_ARG) == 0) {
			rm_restart = true;
		} else if (strcmp(argv[i], SERVICE_ADOPTION_RESTART_ARG) == 0 && i + 1 < argc) {
			adoption_predecessor_pid = strtoul(argv[++i], NULL, 10);
		}
	}

	// ADR-045: started by our own predecessor to complete a plug-in adoption.
	if (adoption_predecessor_pid != 0) {
		U_LOG_W("Started to complete a display plug-in adoption (predecessor pid %lu).",
		        adoption_predecessor_pid);
		wait_for_predecessor(adoption_predecessor_pid);
	}

	// Restarted by Restart Manager after an installer closed us: record the
	// context it gave us, and never stay elevated (see relaunch_unelevated_via_shell).
	if (rm_restart) {
		bool elevated = process_is_elevated();
		U_LOG_W("Started by a Restart Manager restart (integrity RID 0x%lx, elevated=%d).",
		        (unsigned long)process_integrity_rid(), elevated ? 1 : 0);
		if (elevated) {
			bool handed_off = relaunch_unelevated_via_shell();
			U_LOG_W("Restart Manager restart came up elevated; %s.",
			        handed_off ? "relaunched non-elevated via the shell, exiting"
			                   : "could NOT relaunch non-elevated, exiting (start the service normally)");
			u_crash_guard_mark_orderly_exit();
			ExitProcess(0);
		}
	}

	// If user disabled "Start on login" via the tray menu, bow out of logon
	// auto-starts only. Manual launches and IPC auto-launch from an OpenXR
	// app (neither passes --autostart) always start (#806 — the old
	// unconditional gate silently killed every launch on any machine where
	// the toggle had ever been unchecked).
	if (autostart && !cfg.start_on_login) {
		U_LOG_W("Start-on-login is disabled in service.json; exiting (logon auto-start).");
		u_crash_guard_mark_orderly_exit();
		ExitProcess(0);
	}

	// #975: become the singleton BEFORE touching the tray, the Ctrl+Space hook,
	// or any child process. The pipe's FILE_FLAG_FIRST_PIPE_INSTANCE inside
	// ipc_server_main stays the authoritative gate; this earlier check keeps a
	// second instance (client auto-launch racing a dev restart, a logon race)
	// from briefly grabbing tray/hotkey ownership and then dying on its own
	// teardown. A dying owner abandons the mutex within the wait, so a fast
	// restart still comes up. The handle is intentionally held for life.
	HANDLE singleton = CreateMutexW(NULL, TRUE, L"Local\\DisplayXR.Service.Singleton");
	if (singleton != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
		DWORD w = WaitForSingleObject(singleton, 3000);
		if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
			U_LOG_W(
			    "Another displayxr-service instance owns the singleton mutex; this instance exits "
			    "without touching the tray or hotkey (#975).");
			u_crash_guard_mark_orderly_exit();
			return 0;
		}
	}

	// Only the singleton owner registers: Restart Manager relaunches the
	// instance it closed, never one that bowed out above.
	register_for_restart(workspace_mode);

	// Start the system tray icon with orchestrator menu (also creates the
	// hidden session-end window Restart Manager closes us through).
	service_tray_init(tray_shutdown_callback, tray_config_change_callback, &cfg);

	// Initialize orchestrator (registers hotkeys, spawns children per config)
	service_orchestrator_init(&cfg);

	// Tell the IPC server how to look up the orchestrator-spawned workspace
	// controller's PID, so workspace_activate can authenticate that only the
	// controller we spawned may transition the runtime into workspace mode.
	ipc_server_set_workspace_pid_provider(service_orchestrator_get_workspace_pid);
	// #960: verify CONTROLLER (registered controller binary) / DIAG (runtime dir)
	// class claims — facts only the service target can check.
	ipc_server_set_client_class_verify_provider(service_client_class_verify);
	register_status_provider();

	// Same plumbing for the file-dialog capability bit so the IPC server can
	// short-circuit `session_request_file_picker` when the active controller
	// did not opt in to Tier 1 — apps then fall back to a flat OS dialog.
	ipc_server_set_workspace_supports_file_dialog_provider(
	    service_orchestrator_get_workspace_supports_file_dialog);

	// Display dashboard phase 8: `displayxr-cli workspace set|launch` (DIAG)
	// re-apply service.json live and launch the controller through the
	// hotkey's own spawn path.
	ipc_server_set_service_config_reload_provider(service_orchestrator_request_config_reload);
	ipc_server_set_workspace_launch_provider(workspace_launch_provider);
	ipc_server_set_workspace_hotkey_suspend_provider(service_orchestrator_request_hotkey_suspend);

	u_trace_marker_init();
	u_metrics_init();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "DisplayXR Service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	    .workspace_mode = workspace_mode,
	    // ADR-045: restart once to complete an adoption — but never from an
	    // instance that is itself such a restart (bounded, no loop).
	    .allow_adoption_restart = adoption_predecessor_pid == 0,
	};

	// MCP server moved out of the runtime in 2026-05 — workspace
	// control surfaces now live in the shell process (or whichever
	// workspace controller is registered). The service no longer hosts
	// any MCP endpoint.

	struct service_main_args sma = {argc, argv, &ismi, 0};
	u_crash_guard_run("service-main", service_main_body, &sma);
	int ret = sma.ret;

	// ADR-045: the IPC server ended its loop to complete a plug-in adoption.
	// The successor waits for this process to exit before it starts.
	if (ipc_server_restart_requested()) {
		bool started = start_adoption_successor(workspace_mode);
		U_LOG_W("Adoption restart: %s.", started ? "successor started; this instance exits"
		                                         : "could NOT start a successor (start the service manually)");
	}

	u_metrics_close();

	// Shut down orchestrator (terminates managed children, unregisters hotkeys)
	service_orchestrator_shutdown();

	// Clean up the tray icon
	service_tray_cleanup();

	u_crash_guard_mark_orderly_exit();
	return ret;
}

#else // !XRT_OS_WINDOWS

#ifdef XRT_OS_MACOS
#include "service_config.h"
#include "service_orchestrator.h"
#endif

#ifdef XRT_OS_LINUX_DESKTOP
// Linux workspace orchestrator: the macOS posix_spawn orchestrator plus the
// launch hotkey (service_hotkey_linux.c). Kept apart from the macOS blocks.
#include "service_config.h"
#include "service_orchestrator.h"
#endif

int
main(int argc, char *argv[])
{
	// #950 exit tripwire (see the Windows entry point for the ordering rule).
	u_file_logging_init();
	u_crash_guard_install_exit_tripwire();

	u_trace_marker_init();
	u_metrics_init();

	// Parse --workspace for multi-compositor mode
	bool workspace_mode = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--workspace") == 0) {
			workspace_mode = true;
			break;
		}
	}

#ifdef XRT_OS_MACOS
	// macOS orchestrator (#61): discover + auto-spawn the workspace controller
	// (the shell) and respawn it on crash, mirroring the Windows orchestrator.
	// Spawn happens at init, before the blocking ipc_server_main loop.
	struct service_config cfg;
	service_config_load(&cfg);
	service_orchestrator_init(&cfg);

	// Let the IPC server authenticate workspace_activate against the
	// orchestrator-spawned controller's PID (0 → manual mode, first-claim wins)
	// and gate the file-picker on the controller's advertised capability.
	ipc_server_set_workspace_pid_provider(service_orchestrator_get_workspace_pid);
	ipc_server_set_workspace_supports_file_dialog_provider(
	    service_orchestrator_get_workspace_supports_file_dialog);

	// Let the macOS menu-bar status item / Ctrl+Space hotkey summon the
	// controller through the orchestrator's registry-discovery + respawn path.
	ipc_server_set_workspace_summon_provider(service_orchestrator_summon_workspace);

	// Display dashboard phase 8: live re-apply of service.json + launch-now.
	ipc_server_set_service_config_reload_provider(service_orchestrator_request_config_reload);
	ipc_server_set_workspace_launch_provider(service_orchestrator_request_launch);
	ipc_server_set_workspace_hotkey_suspend_provider(service_orchestrator_request_hotkey_suspend);
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	// Linux orchestrator: discover the registered workspace controller (POSIX
	// manifests), spawn it per its launch mode / on the launch hotkey, respawn
	// it in ENABLE mode. The PID provider makes workspace_activate accept only
	// the controller we spawned (0 → manual mode, first-claim wins, as before).
	struct service_config linux_cfg;
	service_config_load(&linux_cfg);
	service_orchestrator_init(&linux_cfg);

	ipc_server_set_workspace_pid_provider(service_orchestrator_get_workspace_pid);
	ipc_server_set_workspace_supports_file_dialog_provider(service_orchestrator_get_workspace_supports_file_dialog);
	ipc_server_set_workspace_summon_provider(service_orchestrator_summon_workspace);

	// Display dashboard phase 8: live re-apply of service.json, launch-now and
	// the hotkey-capture suspend.
	ipc_server_set_service_config_reload_provider(service_orchestrator_request_config_reload);
	ipc_server_set_workspace_launch_provider(service_orchestrator_request_launch);
	ipc_server_set_workspace_hotkey_suspend_provider(service_orchestrator_request_hotkey_suspend);
#endif

	// #960: class verification (controller manifests are cross-platform; the
	// orchestrator entry exists on macOS and desktop Linux).
	ipc_server_set_client_class_verify_provider(service_client_class_verify);
	register_status_provider();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "DisplayXR Service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	    .workspace_mode = workspace_mode,
	};

	// MCP server moved out of the runtime in 2026-05 — workspace
	// control surfaces now live in the shell process (or whichever
	// workspace controller is registered). The service no longer hosts
	// any MCP endpoint.

	struct service_main_args sma = {argc, argv, &ismi, 0};
	u_crash_guard_run("service-main", service_main_body, &sma);
	int ret = sma.ret;

	u_metrics_close();

#ifdef XRT_OS_MACOS
	// Terminate the managed workspace controller + join the watcher thread.
	service_orchestrator_shutdown();
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	// Release the hotkey, terminate the managed controller, join the watcher.
	service_orchestrator_shutdown();
#endif

	u_crash_guard_mark_orderly_exit();
	return ret;
}

#endif // XRT_OS_WINDOWS
