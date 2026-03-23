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

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#include "service_tray_win.h"
#include <stdlib.h> // __argc, __argv
#endif

#include "server/ipc_server_interface.h"

#include "target_lists.h"


// Insert the on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)


#ifdef XRT_OS_WINDOWS

// Global shutdown flag set by the tray icon's "Exit" menu item.
// Checked by ipc_server_mainloop_poll() in ipc_server_mainloop_windows.cpp.
volatile bool g_service_shutdown_requested = false;

static void
tray_shutdown_callback(void)
{
	g_service_shutdown_requested = true;
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

	// Start the system tray icon
	service_tray_init(tray_shutdown_callback);

	u_trace_marker_init();
	u_metrics_init();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "DisplayXR Service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	};

	int ret = ipc_server_main(argc, argv, &ismi);

	u_metrics_close();

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

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "DisplayXR Service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	};

	int ret = ipc_server_main(argc, argv, &ismi);

	u_metrics_close();

	return ret;
}

#endif // XRT_OS_WINDOWS
