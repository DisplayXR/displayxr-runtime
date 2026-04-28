// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator — manages workspace controller and bridge child processes.
 * @ingroup ipc
 */

#include "service_orchestrator.h"
#include "service_tray_win.h"

#include "util/u_debug.h"
#include "util/u_logging.h"

#ifdef XRT_OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#endif

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
 * Hotkey ID shared with the workspace controller (must match workspace controller HOTKEY_TOGGLE)
 *
 */

#define HOTKEY_TOGGLE 1

// WM_APP + N messages are user-defined; safe to use on our subclassed tray
// HWND for cross-thread signalling from the low-level keyboard hook and for
// install/uninstall of the hook (which must happen on the tray thread since
// WH_KEYBOARD_LL requires a message pump on the installing thread, and our
// main thread blocks in ipc_server_main without one).
//
// Must NOT collide with WM_TRAYICON (WM_APP + 1) defined in service_tray_win.c
// — every Shell_NotifyIcon notification (WM_RBUTTONUP, WM_MOUSEMOVE,
// NIN_POPUPOPEN, …) is delivered as that message, and a clash makes this
// orchestrator subclass eat them all before tray_wnd_proc ever sees them.
#define WM_ORCHESTRATOR_SPAWN_SHELL    (WM_APP + 100)
#define WM_ORCHESTRATOR_INSTALL_HOOK   (WM_APP + 101)
#define WM_ORCHESTRATOR_UNINSTALL_HOOK (WM_APP + 102)


/*
 *
 * Static state
 *
 */

static struct service_config s_cfg;
static PROCESS_INFORMATION s_workspace_pi;
static bool s_workspace_running = false;
static HANDLE s_workspace_watch_thread = NULL;
static bool s_hotkey_registered = false; // true when WH_KEYBOARD_LL hook is active
static HHOOK s_kbd_hook = NULL;

static PROCESS_INFORMATION s_bridge_pi;
static bool s_bridge_running = false;
static HANDLE s_bridge_watch_thread = NULL;
static CRITICAL_SECTION s_bridge_lock;
static bool s_bridge_lock_inited = false;

#define BRIDGE_TRAMPOLINE_PORT 9014

static SOCKET s_trampoline_sock = INVALID_SOCKET;
static HANDLE s_trampoline_thread = NULL;
static HANDLE s_trampoline_quit_event = NULL;
static bool s_trampoline_running = false;
static bool s_wsa_started = false;


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
//! If log_path is non-NULL, child's stdout and stderr are redirected to it.
static bool
launch_child_with_log(const char *exe_path, const char *args,
                      const char *log_path, PROCESS_INFORMATION *pi)
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

	HANDLE log_h = INVALID_HANDLE_VALUE;
	BOOL inherit = FALSE;
	if (log_path && log_path[0]) {
		SECURITY_ATTRIBUTES sa;
		ZeroMemory(&sa, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		log_h = CreateFileA(log_path,
		                    FILE_APPEND_DATA | SYNCHRONIZE,
		                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                    &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (log_h != INVALID_HANDLE_VALUE) {
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			si.hStdOutput = log_h;
			si.hStdError = log_h;
			inherit = TRUE;
		} else {
			OW("Failed to open log file %s (error %lu)", log_path,
			   (unsigned long)GetLastError());
		}
	}

	BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, inherit, CREATE_NO_WINDOW, NULL, NULL, &si, pi);
	if (log_h != INVALID_HANDLE_VALUE) {
		CloseHandle(log_h); // our copy; child keeps its own
	}
	if (!ok) {
		OW("Failed to launch: %s (error %lu)", cmd, (unsigned long)GetLastError());
		return false;
	}

	// Close the thread handle immediately — we only need the process handle
	CloseHandle(pi->hThread);
	pi->hThread = NULL;

	return true;
}

static bool
launch_child(const char *exe_path, const char *args, PROCESS_INFORMATION *pi)
{
	return launch_child_with_log(exe_path, args, NULL, pi);
}

//! Build %LOCALAPPDATA%\DisplayXR\<basename>.log.
static bool
appdata_log_path(const char *basename, char *buf, size_t buf_size)
{
	char appdata[MAX_PATH];
	DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return false;
	}
	snprintf(buf, buf_size, "%s\\DisplayXR\\%s.log", appdata, basename);
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
 * Workspace watchdog thread — monitors workspace process, re-registers hotkey on exit
 *
 */

static DWORD WINAPI
workspace_watch_thread_func(LPVOID param)
{
	(void)param;

	// Wait for the workspace process to exit
	WaitForSingleObject(s_workspace_pi.hProcess, INFINITE);

	CloseHandle(s_workspace_pi.hProcess);
	s_workspace_pi.hProcess = NULL;
	s_workspace_running = false;

	OW("Shell process exited");

	// In Auto mode, nothing to re-register: the low-level keyboard hook
	// stays installed across workspace sessions, it's gated by s_workspace_running
	// in the hook proc.

	// In Enable mode, restart the workspace controller
	if (s_cfg.workspace == SERVICE_CHILD_ENABLE) {
		char workspace_path[MAX_PATH];
		if (sibling_exe_path(s_cfg.workspace_binary, workspace_path, sizeof(workspace_path))) {
			if (launch_child(workspace_path, "--service-managed", &s_workspace_pi)) {
				s_workspace_running = true;
				OW("Restarted workspace controller (Enable mode)");
				// Recurse — start watching the new process
				s_workspace_watch_thread = CreateThread(NULL, 0, workspace_watch_thread_func, NULL, 0, NULL);
			}
		}
	}

	return 0;
}

//! Spawn the workspace controller and start watching it.
static void
spawn_workspace(void)
{
	if (s_workspace_running) {
		return;
	}

	char workspace_path[MAX_PATH];
	if (!sibling_exe_path(s_cfg.workspace_binary, workspace_path, sizeof(workspace_path))) {
		OW("Cannot find workspace controller binary '%s' next to service", s_cfg.workspace_binary);
		return;
	}

	if (!launch_child(workspace_path, "--service-managed", &s_workspace_pi)) {
		return;
	}

	s_workspace_running = true;
	OW("Launched workspace controller (PID %lu)", (unsigned long)s_workspace_pi.dwProcessId);

	// Start watchdog thread
	if (s_workspace_watch_thread) {
		CloseHandle(s_workspace_watch_thread);
	}
	s_workspace_watch_thread = CreateThread(NULL, 0, workspace_watch_thread_func, NULL, 0, NULL);
}


/*
 *
 * Bridge watchdog thread — monitors webxr bridge, restarts in Enable mode,
 * restarts the trampoline in Auto mode.
 *
 */

// Forward decls — trampoline helpers defined below.
static void start_bridge_trampoline(void);

static DWORD WINAPI
bridge_watch_thread_func(LPVOID param)
{
	(void)param;

	WaitForSingleObject(s_bridge_pi.hProcess, INFINITE);

	EnterCriticalSection(&s_bridge_lock);
	CloseHandle(s_bridge_pi.hProcess);
	s_bridge_pi.hProcess = NULL;
	s_bridge_running = false;
	enum service_child_mode mode = s_cfg.bridge;
	LeaveCriticalSection(&s_bridge_lock);

	OW("WebXR bridge process exited");

	// In Enable mode, restart the bridge
	if (mode == SERVICE_CHILD_ENABLE) {
		char bridge_path[MAX_PATH];
		char log_path[MAX_PATH];
		const char *log = NULL;
		if (appdata_log_path("webxr-bridge", log_path, sizeof(log_path))) {
			log = log_path;
		}
		if (sibling_exe_path("displayxr-webxr-bridge.exe", bridge_path, sizeof(bridge_path))) {
			EnterCriticalSection(&s_bridge_lock);
			if (launch_child_with_log(bridge_path, NULL, log, &s_bridge_pi)) {
				s_bridge_running = true;
				OW("Restarted WebXR bridge (Enable mode)");
				s_bridge_watch_thread =
				    CreateThread(NULL, 0, bridge_watch_thread_func, NULL, 0, NULL);
			}
			LeaveCriticalSection(&s_bridge_lock);
		}
	}

	// In Auto mode, restart the trampoline so the next webXR app trigger
	// can respawn the bridge.
	if (mode == SERVICE_CHILD_AUTO) {
		start_bridge_trampoline();
	}

	return 0;
}

//! Spawn the webxr bridge and start watching it. Safe to call concurrently.
static void
spawn_bridge(void)
{
	EnterCriticalSection(&s_bridge_lock);
	if (s_bridge_running) {
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	char bridge_path[MAX_PATH];
	if (!sibling_exe_path("displayxr-webxr-bridge.exe", bridge_path, sizeof(bridge_path))) {
		OW("Cannot find displayxr-webxr-bridge.exe next to service");
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	char log_path[MAX_PATH];
	const char *log = NULL;
	if (appdata_log_path("webxr-bridge", log_path, sizeof(log_path))) {
		log = log_path;
	}
	if (!launch_child_with_log(bridge_path, NULL, log, &s_bridge_pi)) {
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	s_bridge_running = true;
	OW("Launched WebXR bridge (PID %lu)", (unsigned long)s_bridge_pi.dwProcessId);

	if (s_bridge_watch_thread) {
		CloseHandle(s_bridge_watch_thread);
	}
	s_bridge_watch_thread = CreateThread(NULL, 0, bridge_watch_thread_func, NULL, 0, NULL);
	LeaveCriticalSection(&s_bridge_lock);
}


/*
 *
 * Bridge trampoline — listens on 127.0.0.1:9014 in Auto mode. A TCP
 * connection attempt means the Chrome extension (driven by a webXR app
 * that reads session.displayXR) wants the bridge. On first accept we
 * close our listener + the accepted socket and spawn the bridge; the
 * extension's exponential-backoff reconnect lands on the bridge once
 * it binds the port.
 *
 */

//! Worker thread — select() loop, spawns bridge on first connection.
static DWORD WINAPI
trampoline_thread_func(LPVOID param)
{
	(void)param;

	for (;;) {
		if (WaitForSingleObject(s_trampoline_quit_event, 0) == WAIT_OBJECT_0) {
			break;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(s_trampoline_sock, &readfds);
		struct timeval tv = {0, 500000}; // 500 ms

		int r = select(0, &readfds, NULL, NULL, &tv);
		if (r == SOCKET_ERROR) {
			// Listener was closed out from under us (stop path) — exit.
			break;
		}
		if (r == 0) {
			continue; // timeout, re-check quit event
		}

		SOCKET accepted = accept(s_trampoline_sock, NULL, NULL);
		if (accepted == INVALID_SOCKET) {
			break;
		}

		// Release the port immediately so the bridge can bind it.
		closesocket(accepted);

		EnterCriticalSection(&s_bridge_lock);
		if (s_trampoline_sock != INVALID_SOCKET) {
			closesocket(s_trampoline_sock);
			s_trampoline_sock = INVALID_SOCKET;
		}
		s_trampoline_running = false;
		LeaveCriticalSection(&s_bridge_lock);

		OW("Bridge trampoline accepted — launching WebXR bridge");
		spawn_bridge();
		return 0;
	}

	return 0;
}

static void
start_bridge_trampoline(void)
{
	EnterCriticalSection(&s_bridge_lock);
	if (s_trampoline_running || s_bridge_running) {
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	if (!s_wsa_started) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			OW("Bridge trampoline: WSAStartup failed (%d)", WSAGetLastError());
			LeaveCriticalSection(&s_bridge_lock);
			return;
		}
		s_wsa_started = true;
	}

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		OW("Bridge trampoline: socket() failed (%d)", WSAGetLastError());
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr;
	ZeroMemory(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(BRIDGE_TRAMPOLINE_PORT);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		OW("Bridge trampoline: bind(127.0.0.1:%d) failed (%d). Bridge may already "
		   "be running externally.",
		   BRIDGE_TRAMPOLINE_PORT, WSAGetLastError());
		closesocket(sock);
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	if (listen(sock, 1) == SOCKET_ERROR) {
		OW("Bridge trampoline: listen() failed (%d)", WSAGetLastError());
		closesocket(sock);
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}

	if (!s_trampoline_quit_event) {
		s_trampoline_quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	} else {
		ResetEvent(s_trampoline_quit_event);
	}

	s_trampoline_sock = sock;
	s_trampoline_running = true;
	s_trampoline_thread = CreateThread(NULL, 0, trampoline_thread_func, NULL, 0, NULL);

	OW("Bridge trampoline listening on 127.0.0.1:%d (Auto mode)", BRIDGE_TRAMPOLINE_PORT);
	LeaveCriticalSection(&s_bridge_lock);
}

static void
stop_bridge_trampoline(void)
{
	HANDLE thread = NULL;

	EnterCriticalSection(&s_bridge_lock);
	if (!s_trampoline_running && s_trampoline_thread == NULL) {
		LeaveCriticalSection(&s_bridge_lock);
		return;
	}
	if (s_trampoline_quit_event) {
		SetEvent(s_trampoline_quit_event);
	}
	if (s_trampoline_sock != INVALID_SOCKET) {
		closesocket(s_trampoline_sock);
		s_trampoline_sock = INVALID_SOCKET;
	}
	thread = s_trampoline_thread;
	s_trampoline_thread = NULL;
	s_trampoline_running = false;
	LeaveCriticalSection(&s_bridge_lock);

	if (thread) {
		WaitForSingleObject(thread, 3000);
		CloseHandle(thread);
	}
}


/*
 *
 * Hotkey handling — low-level keyboard hook (WH_KEYBOARD_LL).
 *
 * RegisterHotKey turned out unreliable on systems where Windows IME or
 * third-party launchers already claim Ctrl+Space (error 1408 on every
 * plausible combo). A low-level keyboard hook runs before global-hotkey
 * dispatch, so our chord always wins.
 *
 * The hook proc runs on the thread that installed it (the tray thread,
 * which has a GetMessage pump). From there we PostMessage a custom
 * message to the subclassed tray HWND and let spawn_workspace run on a
 * normal stack — hook procs should do minimal work.
 *
 */

static LRESULT CALLBACK
orchestrator_wnd_proc_hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

//! Original tray window proc — we subclass it to intercept our custom msg
static WNDPROC s_original_wnd_proc = NULL;

static LRESULT CALLBACK
orchestrator_kbd_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
		KBDLLHOOKSTRUCT *kbd = (KBDLLHOOKSTRUCT *)lParam;
		if (kbd->vkCode == VK_SPACE) {
			bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
			bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
			           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
			OW("kbd hook: Space ctrl=%d shift=%d alt=%d win=%d workspace_running=%d",
			   ctrl, shift, alt, win, (int)s_workspace_running);
			// Plain Ctrl+Space only, no other modifiers.
			if (ctrl && !shift && !alt && !win && !s_workspace_running) {
				HWND hwnd = (HWND)service_tray_get_hwnd();
				if (hwnd) {
					PostMessageW(hwnd, WM_ORCHESTRATOR_SPAWN_SHELL, 0, 0);
				}
				return 1; // swallow — don't let other handlers see Ctrl+Space
			}
		}
	}
	return CallNextHookEx(s_kbd_hook, nCode, wParam, lParam);
}

static LRESULT CALLBACK
orchestrator_wnd_proc_hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_ORCHESTRATOR_SPAWN_SHELL) {
		OW("Ctrl+Space pressed — launching workspace controller");
		spawn_workspace();
		return 0;
	}
	if (msg == WM_ORCHESTRATOR_INSTALL_HOOK) {
		if (s_kbd_hook == NULL) {
			s_kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL,
			                               orchestrator_kbd_hook_proc,
			                               GetModuleHandleW(NULL), 0);
			if (s_kbd_hook == NULL) {
				OW("SetWindowsHookEx(WH_KEYBOARD_LL) failed: error %lu",
				   (unsigned long)GetLastError());
			} else {
				OW("Installed Ctrl+Space keyboard hook (Auto mode)");
			}
		}
		return 0;
	}
	if (msg == WM_ORCHESTRATOR_UNINSTALL_HOOK) {
		if (s_kbd_hook != NULL) {
			UnhookWindowsHookEx(s_kbd_hook);
			s_kbd_hook = NULL;
		}
		return 0;
	}
	return CallWindowProcW(s_original_wnd_proc, hwnd, msg, wParam, lParam);
}

//! Ask the tray thread to install the low-level keyboard hook. WH_KEYBOARD_LL
//! callbacks execute on the installing thread, which must have a message
//! pump — the tray thread does, our main thread (blocked in ipc_server_main)
//! doesn't. Returns true if the install request was posted successfully.
static bool
install_workspace_hotkey(void)
{
	HWND hwnd = (HWND)service_tray_get_hwnd();
	if (hwnd == NULL) return false;
	return PostMessageW(hwnd, WM_ORCHESTRATOR_INSTALL_HOOK, 0, 0) != 0;
}

static void
uninstall_workspace_hotkey(void)
{
	HWND hwnd = (HWND)service_tray_get_hwnd();
	if (hwnd != NULL) {
		PostMessageW(hwnd, WM_ORCHESTRATOR_UNINSTALL_HOOK, 0, 0);
	}
}


/*
 *
 * Mode apply helpers
 *
 */

static void
apply_workspace_mode(enum service_child_mode mode)
{
	switch (mode) {
	case SERVICE_CHILD_ENABLE:
		// Uninstall keyboard hook if active
		if (s_hotkey_registered) {
			uninstall_workspace_hotkey();
			s_hotkey_registered = false;
		}
		// Launch workspace controller if not running
		spawn_workspace();
		break;

	case SERVICE_CHILD_DISABLE:
		// Uninstall keyboard hook if active
		if (s_hotkey_registered) {
			uninstall_workspace_hotkey();
			s_hotkey_registered = false;
		}
		// Terminate workspace controller if running
		if (s_workspace_running) {
			terminate_child(&s_workspace_pi, &s_workspace_running);
			OW("Terminated workspace controller (Disable mode)");
		}
		break;

	case SERVICE_CHILD_AUTO:
		// Install low-level keyboard hook so Ctrl+Space summons the workspace controller.
		// The hook's proc checks s_workspace_running per-keypress and passes
		// through if the workspace is already up. The actual install happens
		// on the tray thread via PostMessage; log only on failure here.
		if (!s_hotkey_registered) {
			if (install_workspace_hotkey()) {
				s_hotkey_registered = true;
			} else {
				OW("install_workspace_hotkey: could not post to tray HWND");
			}
		}
		break;
	}
}

static void
apply_bridge_mode(enum service_child_mode mode)
{
	switch (mode) {
	case SERVICE_CHILD_ENABLE:
		stop_bridge_trampoline();
		spawn_bridge();
		break;

	case SERVICE_CHILD_DISABLE:
		stop_bridge_trampoline();
		EnterCriticalSection(&s_bridge_lock);
		if (s_bridge_running) {
			terminate_child(&s_bridge_pi, &s_bridge_running);
			OW("Terminated WebXR bridge (Disable mode)");
		}
		LeaveCriticalSection(&s_bridge_lock);
		break;

	case SERVICE_CHILD_AUTO:
		// Listen on 9014 until a webXR app (via the extension) requests the
		// bridge. No-op if bridge is already running.
		start_bridge_trampoline();
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
	ZeroMemory(&s_workspace_pi, sizeof(s_workspace_pi));
	ZeroMemory(&s_bridge_pi, sizeof(s_bridge_pi));

	if (!s_bridge_lock_inited) {
		InitializeCriticalSection(&s_bridge_lock);
		s_bridge_lock_inited = true;
	}

	// Subclass the tray HWND to intercept WM_HOTKEY
	HWND hwnd = (HWND)service_tray_get_hwnd();
	if (hwnd) {
		s_original_wnd_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
		                                                  (LONG_PTR)orchestrator_wnd_proc_hook);
	}

	apply_workspace_mode(cfg->workspace);
	apply_bridge_mode(cfg->bridge);

	return true;
}

void
service_orchestrator_apply_config(const struct service_config *cfg)
{
	enum service_child_mode old_workspace = s_cfg.workspace;
	enum service_child_mode old_bridge = s_cfg.bridge;
	s_cfg = *cfg;

	if (cfg->workspace != old_workspace) {
		apply_workspace_mode(cfg->workspace);
	}

	if (cfg->bridge != old_bridge) {
		apply_bridge_mode(cfg->bridge);
	}
}

void
service_orchestrator_shutdown(void)
{
	HWND hwnd = (HWND)service_tray_get_hwnd();

	// Uninstall keyboard hook
	if (s_hotkey_registered) {
		uninstall_workspace_hotkey();
		s_hotkey_registered = false;
	}

	// Restore original window proc
	if (s_original_wnd_proc && hwnd) {
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)s_original_wnd_proc);
		s_original_wnd_proc = NULL;
	}

	// Terminate shell
	if (s_workspace_running) {
		terminate_child(&s_workspace_pi, &s_workspace_running);
	}

	// Wait for watchdog thread
	if (s_workspace_watch_thread) {
		WaitForSingleObject(s_workspace_watch_thread, 3000);
		CloseHandle(s_workspace_watch_thread);
		s_workspace_watch_thread = NULL;
	}

	// Stop the trampoline and terminate the bridge. Flip mode to DISABLE
	// first so the watchdog, when it sees the process exit, does not try
	// to respawn or relaunch the trampoline.
	if (s_bridge_lock_inited) {
		EnterCriticalSection(&s_bridge_lock);
		s_cfg.bridge = SERVICE_CHILD_DISABLE;
		LeaveCriticalSection(&s_bridge_lock);
	}

	stop_bridge_trampoline();

	if (s_bridge_lock_inited) {
		EnterCriticalSection(&s_bridge_lock);
		if (s_bridge_running) {
			terminate_child(&s_bridge_pi, &s_bridge_running);
		}
		LeaveCriticalSection(&s_bridge_lock);
	}

	if (s_bridge_watch_thread) {
		WaitForSingleObject(s_bridge_watch_thread, 3000);
		CloseHandle(s_bridge_watch_thread);
		s_bridge_watch_thread = NULL;
	}

	if (s_trampoline_quit_event) {
		CloseHandle(s_trampoline_quit_event);
		s_trampoline_quit_event = NULL;
	}

	if (s_wsa_started) {
		WSACleanup();
		s_wsa_started = false;
	}

	if (s_bridge_lock_inited) {
		DeleteCriticalSection(&s_bridge_lock);
		s_bridge_lock_inited = false;
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
service_orchestrator_shutdown(void)
{
}

#endif // XRT_OS_WINDOWS
