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
#include "util/u_mcp_server.h"

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

#include <string.h> // strcmp for --workspace / --shell flag


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

	// Load orchestrator config (workspace/bridge modes, start-on-login)
	struct service_config cfg;
	service_config_load(&cfg);

	// If user disabled "Start on login" via the tray menu, exit immediately.
	// The HKLM Run key still launches us, but we honor the config and bow out
	// before creating any windows or tray icons. The service can still be
	// started manually or via IPC auto-launch from an OpenXR app.
	if (!cfg.start_on_login) {
		ExitProcess(0);
	}

	// Parse --workspace (or legacy --shell alias) for the multi-terminal
	// workflow. The orchestrator will also enable workspace mode when it
	// spawns the workspace controller.
	bool workspace_mode = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--workspace") == 0 || strcmp(argv[i], "--shell") == 0) {
			workspace_mode = true;
			break;
		}
	}

	// Start the system tray icon with orchestrator menu
	service_tray_init(tray_shutdown_callback, tray_config_change_callback, &cfg);

	// Initialize orchestrator (registers hotkeys, spawns children per config)
	service_orchestrator_init(&cfg);

	// Tell the IPC server how to look up the orchestrator-spawned workspace
	// controller's PID, so workspace_activate can authenticate that only the
	// controller we spawned may transition the runtime into workspace mode.
	ipc_server_set_workspace_pid_provider(service_orchestrator_get_workspace_pid);

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

	// Opt-in MCP server for agent-driven shell control / debugging.
	// Gated on DISPLAYXR_MCP env var; binds a well-known "service"
	// socket so the adapter can discover it by role, not PID.
	u_mcp_server_maybe_start_named("service");

	int ret = ipc_server_main(argc, argv, &ismi);

	u_mcp_server_stop();

	u_metrics_close();

	// Shut down orchestrator (terminates managed children, unregisters hotkeys)
	service_orchestrator_shutdown();

	// Clean up the tray icon
	service_tray_cleanup();

	return ret;
}

#else // !XRT_OS_WINDOWS

int
main(int argc, char *argv[])
{
	u_trace_marker_init();
	u_metrics_init();

	// Parse --workspace (or legacy --shell alias) for multi-compositor mode
	bool workspace_mode = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--workspace") == 0 || strcmp(argv[i], "--shell") == 0) {
			workspace_mode = true;
			break;
		}
	}

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "DisplayXR Service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	    .workspace_mode = workspace_mode,
	};

	// Opt-in MCP server for agent-driven shell control / debugging.
	// Gated on DISPLAYXR_MCP env var; binds a well-known "service"
	// socket so the adapter can discover it by role, not PID.
	u_mcp_server_maybe_start_named("service");

	int ret = ipc_server_main(argc, argv, &ismi);

	u_mcp_server_stop();

	u_metrics_close();

	return ret;
}

#endif // XRT_OS_WINDOWS
