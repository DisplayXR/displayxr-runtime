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
#endif

#include "server/ipc_server_interface.h"
#include "server/ipc_server.h"

#include "target_lists.h"

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
	// Enable per-monitor DPI awareness FIRST, before any window/display operations.
	// This is critical for high-DPI displays like Leia (7680x4320 at 300% scaling).
	// Without this, Windows reports scaled logical resolution (2560x1440) instead of
	// physical resolution, breaking SR weaver which needs true pixel dimensions.
	//
	// SetProcessDpiAwarenessContext is Win10 1607+ (SDK 10.0.14393+)
	typedef BOOL(WINAPI * PFN_SetProcessDpiAwarenessContext)(HANDLE);
	HMODULE user32 = GetModuleHandleA("user32.dll");
	if (user32) {
		PFN_SetProcessDpiAwarenessContext fn =
		    (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		if (fn) {
			// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
			fn((HANDLE)(intptr_t)-4);
		}
	}
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

	// Load orchestrator config (workspace/bridge modes, start-on-login)
	struct service_config cfg;
	service_config_load(&cfg);

	// Parse flags. --workspace is the manual multi-terminal workflow (the
	// orchestrator will also enable workspace mode when it spawns the
	// workspace controller). --autostart is appended by the HKLM Run key
	// registration so we can tell a logon auto-start from a manual launch.
	bool workspace_mode = false;
	bool autostart = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--workspace") == 0) {
			workspace_mode = true;
		} else if (strcmp(argv[i], "--autostart") == 0) {
			autostart = true;
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

	// Start the system tray icon with orchestrator menu
	service_tray_init(tray_shutdown_callback, tray_config_change_callback, &cfg);

	// Initialize orchestrator (registers hotkeys, spawns children per config)
	service_orchestrator_init(&cfg);

	// Tell the IPC server how to look up the orchestrator-spawned workspace
	// controller's PID, so workspace_activate can authenticate that only the
	// controller we spawned may transition the runtime into workspace mode.
	ipc_server_set_workspace_pid_provider(service_orchestrator_get_workspace_pid);

	// Same plumbing for the file-dialog capability bit so the IPC server can
	// short-circuit `session_request_file_picker` when the active controller
	// did not opt in to Tier 1 — apps then fall back to a flat OS dialog.
	ipc_server_set_workspace_supports_file_dialog_provider(
	    service_orchestrator_get_workspace_supports_file_dialog);

	u_trace_marker_init();
	u_metrics_init();

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
#endif

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

	u_crash_guard_mark_orderly_exit();
	return ret;
}

#endif // XRT_OS_WINDOWS
