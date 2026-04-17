// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator — manages shell and bridge child processes.
 * @ingroup ipc
 */

#include "service_orchestrator.h"
#include "service_tray_win.h"

#include "util/u_debug.h"
#include "util/u_logging.h"

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#include <string.h>
#include <stdio.h>
#endif

// Callback pointer defined in ipc_server_handler.c — we set it to our handler.
extern void (*g_ipc_appcontainer_client_cb)(void);

DEBUG_GET_ONCE_LOG_OPTION(orchestrator_log, "DISPLAYXR_ORCHESTRATOR_LOG", U_LOGGING_WARN)
#define OL(log_level, ...) \
	do { \
		if (debug_get_log_option_orchestrator_log() >= log_level) \
			U_LOG(log_level, __VA_ARGS__); \
	} while (0)
#define OW(...) OL(U_LOGGING_WARN, __VA_ARGS__)


#ifdef XRT_OS_WINDOWS

/*
 *
 * Hotkey ID shared with the shell (must match shell/main.c HOTKEY_TOGGLE)
 *
 */

#define HOTKEY_TOGGLE 1


/*
 *
 * Static state
 *
 */

static struct service_config s_cfg;
static PROCESS_INFORMATION s_shell_pi;
static bool s_shell_running = false;
static HANDLE s_shell_watch_thread = NULL;
static bool s_hotkey_registered = false;


/*
 *
 * Helpers
 *
 */

//! Build the path to a sibling executable next to displayxr-service.exe.
static bool
sibling_exe_path(const char *exe_name, char *buf, size_t buf_size)
{
	char self_path[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, self_path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return false;
	}

	// Strip filename to get directory
	char *last_sep = strrchr(self_path, '\\');
	if (!last_sep) {
		last_sep = strrchr(self_path, '/');
	}
	if (!last_sep) {
		return false;
	}
	*(last_sep + 1) = '\0';

	snprintf(buf, buf_size, "%s%s", self_path, exe_name);
	return true;
}

//! Launch a child process and return its PROCESS_INFORMATION.
static bool
launch_child(const char *exe_path, const char *args, PROCESS_INFORMATION *pi)
{
	char cmd[1024];
	if (args && args[0]) {
		snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe_path, args);
	} else {
		snprintf(cmd, sizeof(cmd), "\"%s\"", exe_path);
	}

	STARTUPINFOA si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(pi, sizeof(*pi));

	BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
	if (!ok) {
		OW("Failed to launch: %s (error %lu)", cmd, (unsigned long)GetLastError());
		return false;
	}

	// Close the thread handle immediately — we only need the process handle
	CloseHandle(pi->hThread);
	pi->hThread = NULL;

	return true;
}

//! Terminate a child process if running.
static void
terminate_child(PROCESS_INFORMATION *pi, bool *running_flag)
{
	if (pi->hProcess) {
		TerminateProcess(pi->hProcess, 0);
		WaitForSingleObject(pi->hProcess, 3000);
		CloseHandle(pi->hProcess);
		pi->hProcess = NULL;
	}
	if (running_flag) {
		*running_flag = false;
	}
}


/*
 *
 * Shell watchdog thread — monitors shell process, re-registers hotkey on exit
 *
 */

static DWORD WINAPI
shell_watch_thread_func(LPVOID param)
{
	(void)param;

	// Wait for the shell process to exit
	WaitForSingleObject(s_shell_pi.hProcess, INFINITE);

	CloseHandle(s_shell_pi.hProcess);
	s_shell_pi.hProcess = NULL;
	s_shell_running = false;

	OW("Shell process exited");

	// In Auto mode, re-register the hotkey so the user can re-summon the shell
	if (s_cfg.shell == SERVICE_CHILD_AUTO) {
		HWND hwnd = (HWND)service_tray_get_hwnd();
		if (hwnd) {
			RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL, VK_SPACE);
			s_hotkey_registered = true;
			OW("Re-registered Ctrl+Space hotkey (shell exited)");
		}
	}

	// In Enable mode, restart the shell
	if (s_cfg.shell == SERVICE_CHILD_ENABLE) {
		char shell_path[MAX_PATH];
		if (sibling_exe_path("displayxr-shell.exe", shell_path, sizeof(shell_path))) {
			if (launch_child(shell_path, "--service-managed", &s_shell_pi)) {
				s_shell_running = true;
				OW("Restarted shell (Enable mode)");
				// Recurse — start watching the new process
				s_shell_watch_thread = CreateThread(NULL, 0, shell_watch_thread_func, NULL, 0, NULL);
			}
		}
	}

	return 0;
}

//! Spawn the shell and start watching it.
static void
spawn_shell(void)
{
	if (s_shell_running) {
		return;
	}

	char shell_path[MAX_PATH];
	if (!sibling_exe_path("displayxr-shell.exe", shell_path, sizeof(shell_path))) {
		OW("Cannot find displayxr-shell.exe next to service");
		return;
	}

	if (!launch_child(shell_path, "--service-managed", &s_shell_pi)) {
		return;
	}

	s_shell_running = true;
	OW("Launched shell (PID %lu)", (unsigned long)s_shell_pi.dwProcessId);

	// Start watchdog thread
	if (s_shell_watch_thread) {
		CloseHandle(s_shell_watch_thread);
	}
	s_shell_watch_thread = CreateThread(NULL, 0, shell_watch_thread_func, NULL, 0, NULL);
}


/*
 *
 * Hotkey handling — called from tray window proc via WM_HOTKEY
 *
 */

static LRESULT CALLBACK
orchestrator_wnd_proc_hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

//! Original tray window proc — we subclass it to intercept WM_HOTKEY
static WNDPROC s_original_wnd_proc = NULL;

static LRESULT CALLBACK
orchestrator_wnd_proc_hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_HOTKEY && wParam == HOTKEY_TOGGLE) {
		// Ctrl+Space pressed — unregister hotkey and spawn shell
		UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
		s_hotkey_registered = false;
		OW("Ctrl+Space pressed — launching shell");
		spawn_shell();
		return 0;
	}

	return CallWindowProcW(s_original_wnd_proc, hwnd, msg, wParam, lParam);
}


/*
 *
 * Mode apply helpers
 *
 */

static void
apply_shell_mode(enum service_child_mode mode)
{
	HWND hwnd = (HWND)service_tray_get_hwnd();

	switch (mode) {
	case SERVICE_CHILD_ENABLE:
		// Unregister hotkey if we own it
		if (s_hotkey_registered && hwnd) {
			UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
			s_hotkey_registered = false;
		}
		// Launch shell if not running
		spawn_shell();
		break;

	case SERVICE_CHILD_DISABLE:
		// Unregister hotkey if we own it
		if (s_hotkey_registered && hwnd) {
			UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
			s_hotkey_registered = false;
		}
		// Terminate shell if running
		if (s_shell_running) {
			terminate_child(&s_shell_pi, &s_shell_running);
			OW("Terminated shell (Disable mode)");
		}
		break;

	case SERVICE_CHILD_AUTO:
		// If shell is already running, let it be
		if (!s_shell_running && hwnd) {
			if (!s_hotkey_registered) {
				RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL, VK_SPACE);
				s_hotkey_registered = true;
				OW("Registered Ctrl+Space hotkey (Auto mode)");
			}
		}
		break;
	}
}


/*
 *
 * Public API
 *
 */

bool
service_orchestrator_init(const struct service_config *cfg)
{
	s_cfg = *cfg;
	ZeroMemory(&s_shell_pi, sizeof(s_shell_pi));

	// Set the AppContainer client callback so the IPC handler can notify us
	g_ipc_appcontainer_client_cb = service_orchestrator_on_appcontainer_client;

	// Subclass the tray HWND to intercept WM_HOTKEY
	HWND hwnd = (HWND)service_tray_get_hwnd();
	if (hwnd) {
		s_original_wnd_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
		                                                  (LONG_PTR)orchestrator_wnd_proc_hook);
	}

	apply_shell_mode(cfg->shell);

	return true;
}

void
service_orchestrator_apply_config(const struct service_config *cfg)
{
	enum service_child_mode old_shell = s_cfg.shell;
	s_cfg = *cfg;

	if (cfg->shell != old_shell) {
		apply_shell_mode(cfg->shell);
	}

	// Bridge mode changes will be handled here when bridge merges
}

void
service_orchestrator_on_appcontainer_client(void)
{
	OW("AppContainer client detected (Chrome WebXR)");

	// Bridge auto-start will be implemented when feature/webxr-bridge-v2 merges.
	// When ready: if (s_cfg.bridge == SERVICE_CHILD_AUTO && !s_bridge_running) spawn_bridge();
}

void
service_orchestrator_shutdown(void)
{
	// Clear the AppContainer callback before tearing down
	g_ipc_appcontainer_client_cb = NULL;

	HWND hwnd = (HWND)service_tray_get_hwnd();

	// Unregister hotkey
	if (s_hotkey_registered && hwnd) {
		UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
		s_hotkey_registered = false;
	}

	// Restore original window proc
	if (s_original_wnd_proc && hwnd) {
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)s_original_wnd_proc);
		s_original_wnd_proc = NULL;
	}

	// Terminate shell
	if (s_shell_running) {
		terminate_child(&s_shell_pi, &s_shell_running);
	}

	// Wait for watchdog thread
	if (s_shell_watch_thread) {
		WaitForSingleObject(s_shell_watch_thread, 3000);
		CloseHandle(s_shell_watch_thread);
		s_shell_watch_thread = NULL;
	}
}

#else // !XRT_OS_WINDOWS

// Stubs for non-Windows platforms

bool
service_orchestrator_init(const struct service_config *cfg)
{
	(void)cfg;
	return true;
}

void
service_orchestrator_apply_config(const struct service_config *cfg)
{
	(void)cfg;
}

void
service_orchestrator_on_appcontainer_client(void)
{
}

void
service_orchestrator_shutdown(void)
{
}

#endif // XRT_OS_WINDOWS
