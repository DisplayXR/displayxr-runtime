// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows system tray icon for DisplayXR service.
 * @ingroup ipc
 */

#include "service_tray_win.h"
#include "service_orchestrator.h"
#include "service_workspace_registry.h"

#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "util/u_crash_guard.h"
#include "util/u_logging.h"

#include "server/ipc_server_interface.h" // ADR-045: ipc_server_request_display_reprobe
#include "target_plugin_loader.h"        // ADR-045: per-plug-in platform state for the tooltip

#define IDI_DISPLAYXR_ICON_WHITE 101
#define IDI_DISPLAYXR_ICON_BLACK 102
#define WM_TRAYICON              (WM_APP + 1)

// Menu command IDs
#define IDM_WORKSPACE_ENABLE    1010
#define IDM_WORKSPACE_DISABLE   1011
#define IDM_WORKSPACE_AUTO      1012
#define IDM_START_ON_LOGIN  1030
#define IDM_CONTROL_PANEL   1002
#define IDM_EXIT            1001

// ADR-045: tooltip refresh of the display-processor status line.
#define TRAY_STATUS_TIMER_ID 1
#define TRAY_STATUS_PERIOD_MS 5000

// Workspace published-action IDs. Range matches
// WORKSPACE_REGISTRY_MAX_ACTIONS (16) with a small margin for growth.
#define IDM_WORKSPACE_ACTION_BASE 1040
#define IDM_WORKSPACE_ACTION_END  1059


/*
 *
 * Static state
 *
 */

static HWND s_tray_hwnd = NULL;
static HANDLE s_tray_thread = NULL;
static HANDLE s_ready_event = NULL;
static NOTIFYICONDATAW s_nid;
static service_tray_shutdown_cb s_shutdown_cb = NULL;
static service_tray_config_change_cb s_config_cb = NULL;
static struct service_config s_config;

// ADR-045 R-c: registry waiter on the plug-in registration root.
static HANDLE s_reg_watch_thread = NULL;
static HANDLE s_reg_watch_stop = NULL;


/*
 *
 * Theme detection
 *
 */

// Detect whether the Windows taskbar is using a dark theme.
// Returns true if dark (white icon needed), false if light (black icon needed).
static bool
is_taskbar_dark_theme(void)
{
	HKEY hKey;
	DWORD value = 1; // default to light theme (0 = dark, 1 = light for AppsUseLightTheme)
	DWORD size = sizeof(value);

	if (RegOpenKeyExW(HKEY_CURRENT_USER,
	                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
	                  0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, NULL, (LPBYTE)&value, &size);
		RegCloseKey(hKey);
	}

	return value == 0; // 0 = dark theme → need white icon
}

static HICON
load_theme_icon(void)
{
	HINSTANCE hInst = GetModuleHandleW(NULL);
	int resId = is_taskbar_dark_theme() ? IDI_DISPLAYXR_ICON_WHITE : IDI_DISPLAYXR_ICON_BLACK;
	HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(resId));
	if (!icon) {
		icon = LoadIconW(NULL, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
	}
	return icon;
}


/*
 *
 * Menu helpers
 *
 */

//! Map service_child_mode to the corresponding menu ID for the workspace submenu.
static UINT
workspace_mode_to_id(enum service_child_mode m)
{
	switch (m) {
	case SERVICE_CHILD_ENABLE: return IDM_WORKSPACE_ENABLE;
	case SERVICE_CHILD_DISABLE: return IDM_WORKSPACE_DISABLE;
	default: return IDM_WORKSPACE_AUTO;
	}
}

//! Map a published "lifecycle:*" action type to its service_child_mode.
//! Returns true on match.
static bool
lifecycle_type_to_mode(const char *type, enum service_child_mode *out)
{
	if (strcmp(type, "lifecycle:enable") == 0) {
		*out = SERVICE_CHILD_ENABLE;
		return true;
	}
	if (strcmp(type, "lifecycle:auto") == 0) {
		*out = SERVICE_CHILD_AUTO;
		return true;
	}
	if (strcmp(type, "lifecycle:disable") == 0) {
		*out = SERVICE_CHILD_DISABLE;
		return true;
	}
	return false;
}

//! Append a workspace-controller-published Actions list to @p sub. Returns
//! true if at least one menu item was appended (caller can decide whether
//! to fall back to hardcoded defaults). Marks the active lifecycle mode
//! with MF_CHECKED.
static bool
append_published_actions(HMENU sub, const struct workspace_controller_entry *entry)
{
	if (entry == NULL || entry->n_actions <= 0) {
		return false;
	}

	int appended = 0;
	for (int i = 0; i < entry->n_actions; i++) {
		const struct workspace_controller_action *a = &entry->actions[i];

		if (strcmp(a->type, "separator") == 0) {
			AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
			appended++;
			continue;
		}

		// Convert UTF-8 label to wide for the menu API.
		wchar_t label_wide[256];
		if (MultiByteToWideChar(CP_UTF8, 0, a->label, -1,
		                        label_wide, ARRAYSIZE(label_wide)) == 0) {
			// Skip unrenderable labels rather than crashing.
			continue;
		}

		UINT id = IDM_WORKSPACE_ACTION_BASE + i;
		UINT flags = MF_STRING;

		// Mark the active lifecycle mode with a checkmark.
		enum service_child_mode mode;
		if (lifecycle_type_to_mode(a->type, &mode) && mode == s_config.workspace) {
			flags |= MF_CHECKED;
		}

		AppendMenuW(sub, flags, id, label_wide);
		appended++;
	}

	return appended > 0;
}

//! Resolve a binary that ships next to displayxr-service.exe (same Runtime
//! dir) into @p out. Returns true and a NUL-terminated path on success.
static bool
resolve_sibling_exe(const wchar_t *name, wchar_t *out, size_t cap)
{
	wchar_t dir[MAX_PATH];
	DWORD len = GetModuleFileNameW(NULL, dir, ARRAYSIZE(dir));
	if (len == 0 || len >= ARRAYSIZE(dir)) {
		return false;
	}
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (slash == NULL) {
		return false;
	}
	*(slash + 1) = L'\0';
	// dir now ends in a backslash; append the binary name.
	if (wcscpy_s(out, cap, dir) != 0 || wcscat_s(out, cap, name) != 0) {
		return false;
	}
	return true;
}

//! True if the Control Panel GUI is installed alongside the service.
static bool
control_panel_available(void)
{
	wchar_t path[MAX_PATH];
	return resolve_sibling_exe(L"displayxr-control-panel.exe", path, ARRAYSIZE(path)) &&
	       GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

//! Launch the sibling Control Panel GUI (best-effort; runs in the user's
//! session since the service tray itself does).
static void
launch_control_panel(void)
{
	wchar_t path[MAX_PATH];
	if (!resolve_sibling_exe(L"displayxr-control-panel.exe", path, ARRAYSIZE(path))) {
		return;
	}
	// ShellExecute "open" is enough — the panel is a normal GUI exe and we
	// don't need its handle. If it's already open this just brings up a
	// second instance; the panel is cheap and stateless, so that's fine.
	ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

//! Build and show the tray context menu at the cursor position.
static void
show_context_menu(HWND hwnd)
{
	POINT pt;
	GetCursorPos(&pt);

	// Re-enumerate the workspace controller registry so a shell installed
	// (or uninstalled) after the service started shows up immediately on
	// the next right-click — no service restart required. One registry
	// walk per menu open is cheap.
	service_orchestrator_refresh_workspace_controller();

	// Main menu
	HMENU menu = CreatePopupMenu();

	// Control Panel (runtime diagnostics GUI) — top-level entry, shown only
	// when the panel exe is installed alongside the service. It's a runtime-
	// level diagnostic, not a workspace-app action, so it lives on the
	// always-on service tray rather than a workspace controller's submenu.
	if (control_panel_available()) {
		AppendMenuW(menu, MF_STRING, IDM_CONTROL_PANEL, L"DisplayXR Control Panel");
		AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	}

	// Workspace submenu — only built when a controller binary is detected.
	// Bare runtime (no controller installed) gets no workspace entry at all,
	// which is the honest reflection of what the runtime can do on its own.
	//
	// The submenu items themselves come from the controller's published
	// `Actions\*` registry subkeys when present (V2.J Part 2). If the
	// controller has not adopted that contract yet, fall back to the
	// hardcoded Enable / Auto / Disable defaults — preserves UX for any
	// controller not shipping `Actions`.
	if (service_orchestrator_is_workspace_available()) {
		const struct workspace_controller_entry *entry =
		    service_orchestrator_get_workspace_entry();

		HMENU workspace_sub = CreatePopupMenu();
		if (!append_published_actions(workspace_sub, entry)) {
			AppendMenuW(workspace_sub, MF_STRING, IDM_WORKSPACE_ENABLE, L"Enable");
			AppendMenuW(workspace_sub, MF_STRING, IDM_WORKSPACE_AUTO, L"Auto");
			AppendMenuW(workspace_sub, MF_STRING, IDM_WORKSPACE_DISABLE, L"Disable");
			CheckMenuRadioItem(workspace_sub, IDM_WORKSPACE_ENABLE, IDM_WORKSPACE_AUTO,
			                   workspace_mode_to_id(s_config.workspace), MF_BYCOMMAND);
		}

		// Convert UTF-8 display name to wide for the parent menu item.
		const char *name_utf8 = service_orchestrator_get_workspace_display_name();
		wchar_t name_wide[256];
		if (name_utf8 == NULL || name_utf8[0] == '\0' ||
		    MultiByteToWideChar(CP_UTF8, 0, name_utf8, -1,
		                        name_wide, ARRAYSIZE(name_wide)) == 0) {
			wcscpy_s(name_wide, ARRAYSIZE(name_wide), L"Workspace Controller");
		}
		AppendMenuW(menu, MF_POPUP, (UINT_PTR)workspace_sub, name_wide);
	}

	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING | (s_config.start_on_login ? MF_CHECKED : MF_UNCHECKED),
	            IDM_START_ON_LOGIN, L"Start on Windows login");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit DisplayXR Service");

	// Required for TrackPopupMenu to work from a background window
	SetForegroundWindow(hwnd);
	TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
	PostMessageW(hwnd, WM_NULL, 0, 0);

	DestroyMenu(menu);
}

//! Save config and notify the orchestrator.
static void
config_changed(void)
{
	service_config_save(&s_config);
	if (s_config_cb) {
		s_config_cb(&s_config);
	}
}


//! The service's one clean-shutdown request, from the tray thread: raise the
//! flag the IPC mainloop polls (it returns within one 50 ms tick and WinMain
//! tears down) and end the tray thread's message loop. Shared by the tray
//! "Exit" item and the session-end window below.
static void
request_service_exit(void)
{
	if (s_shutdown_cb) {
		s_shutdown_cb();
	}
	PostQuitMessage(0);
}


/*
 *
 * Display-processor status (ADR-045 R-e)
 *
 */

/*!
 * One line for the tray tooltip: which display processor is active, plus a
 * degraded reason when it is the fallback (why the better-ranked plug-in is
 * not active, in that plug-in's own words) or when the active plug-in reports
 * NO_DISPLAY. Names come from the plug-in registration (DisplayName); nothing
 * vendor-specific is known here. Never triggers discovery: it reads the
 * loader's records, which the IPC server's instance filled in.
 */
static void
build_status_line(char *out, size_t cap)
{
	struct target_plugin_status st[16];
	int n = target_plugin_get_status(st, 16);
	const struct target_plugin_status *active = NULL;
	const struct target_plugin_status *best_other = NULL;
	for (int i = 0; i < n; i++) {
		if (st[i].result == TARGET_PLUGIN_RESULT_ACTIVE) {
			active = &st[i];
		} else if (!st[i].fallback && st[i].result != TARGET_PLUGIN_RESULT_NOT_ATTEMPTED &&
		           (best_other == NULL || st[i].probe_order < best_other->probe_order)) {
			best_other = &st[i];
		}
	}

	if (active == NULL) {
		snprintf(out, cap, "%s", n == 0 ? "Display: starting" : "Display: no display processor");
		return;
	}
	const char *name = active->display_name[0] != '\0' ? active->display_name : active->id;

	if (active->fallback) {
		if (best_other == NULL) {
			snprintf(out, cap, "Display: %s (no 3D display plug-in)", name);
			return;
		}
		const char *other = best_other->display_name[0] != '\0' ? best_other->display_name : best_other->id;
		if (best_other->platform_state != XRT_PLUGIN_PLATFORM_STATE_UNKNOWN) {
			snprintf(out, cap, "Display: %s. %s: %s%s%s", name, other,
			         target_plugin_platform_state_str(best_other->platform_state),
			         best_other->hint[0] != '\0' ? " - " : "", best_other->hint);
		} else {
			snprintf(out, cap, "Display: %s. %s: %s", name, other,
			         target_plugin_load_result_str(best_other->result));
		}
		return;
	}

	if (active->platform_state == XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY) {
		snprintf(out, cap, "Display: %s: NO_DISPLAY%s%s", name, active->hint[0] != '\0' ? " - " : "",
		         active->hint);
		return;
	}
	snprintf(out, cap, "Display: %s", name);
}

static void
update_status_tooltip(void)
{
	char line[192];
	build_status_line(line, sizeof(line));

	wchar_t wline[192];
	if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, ARRAYSIZE(wline)) == 0) {
		wline[0] = L'\0';
	}
	wchar_t tip[ARRAYSIZE(s_nid.szTip)];
	_snwprintf_s(tip, ARRAYSIZE(tip), _TRUNCATE, L"DisplayXR Service\n%ls", wline);
	if (wcscmp(tip, s_nid.szTip) == 0) {
		return;
	}
	wcscpy_s(s_nid.szTip, ARRAYSIZE(s_nid.szTip), tip);
	s_nid.uFlags = NIF_TIP;
	Shell_NotifyIconW(NIM_MODIFY, &s_nid);
	s_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
}


/*
 *
 * World-event sources for the display re-probe (ADR-045 R-c)
 *
 */

/*!
 * Watch HKLM\Software\DisplayXR\DisplayProcessors (subtree) so a plug-in
 * registered, removed or re-registered while the service runs is re-probed at
 * once instead of on the next client connect. The root may not exist yet (no
 * plug-in ever installed): wait on HKLM\Software\DisplayXR instead until it
 * does. Requests go through ipc_server_request_display_reprobe, which only
 * flags the IPC server's debounced worker, so nothing here blocks on a probe.
 */
static DWORD WINAPI
reg_watch_thread_body(LPVOID param)
{
	(void)param;
	HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (ev == NULL) {
		return 1;
	}
	for (;;) {
		bool watching_root = true;
		HKEY key = NULL;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\DisplayProcessors", 0,
		                  KEY_NOTIFY | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
			watching_root = false;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR", 0, KEY_NOTIFY | KEY_WOW64_64KEY,
			                  &key) != ERROR_SUCCESS) {
				key = NULL;
			}
		}

		DWORD w;
		if (key != NULL && RegNotifyChangeKeyValue(key, TRUE,
		                                           REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET |
		                                               REG_NOTIFY_THREAD_AGNOSTIC,
		                                           ev, TRUE) == ERROR_SUCCESS) {
			HANDLE hs[2] = {s_reg_watch_stop, ev};
			w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
		} else {
			// Nothing to watch yet (or the API failed): poll slowly.
			w = WaitForSingleObject(s_reg_watch_stop, 10000);
		}
		if (key != NULL) {
			RegCloseKey(key);
		}
		if (w == WAIT_OBJECT_0) {
			break; // stop
		}
		if (w == WAIT_OBJECT_0 + 1 && watching_root) {
			ipc_server_request_display_reprobe("plug-in registration changed");
		}
	}
	CloseHandle(ev);
	return 0;
}

static void
reg_watch_start(void)
{
	s_reg_watch_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (s_reg_watch_stop == NULL) {
		return;
	}
	s_reg_watch_thread = CreateThread(NULL, 0, reg_watch_thread_body, NULL, 0, NULL);
	if (s_reg_watch_thread == NULL) {
		U_LOG_W("Could not start the plug-in registration watcher (error %lu).", GetLastError());
	}
}

static void
reg_watch_stop(void)
{
	if (s_reg_watch_stop != NULL) {
		SetEvent(s_reg_watch_stop);
	}
	if (s_reg_watch_thread != NULL) {
		WaitForSingleObject(s_reg_watch_thread, 2000);
		CloseHandle(s_reg_watch_thread);
		s_reg_watch_thread = NULL;
	}
	if (s_reg_watch_stop != NULL) {
		CloseHandle(s_reg_watch_stop);
		s_reg_watch_stop = NULL;
	}
}


/*
 *
 * Session-end window (Restart Manager, logoff, shutdown)
 *
 */

/*!
 * Exit budget after a session-end / Restart Manager close request. Teardown
 * normally finishes in well under a second, but it does not join the per-client
 * IPC threads and calls into the vendor DP, so it is not provably bounded.
 * Restart Manager force-terminates an application that has not exited 30 s
 * after the request (RmShutdown docs), and an installer that did NOT pass
 * RmForceShutdown just fails instead; stay well inside that window so the
 * files an installer is waiting on are always released by process exit.
 */
#define SESSION_END_EXIT_BUDGET_MS 20000

static DWORD WINAPI
session_end_backstop_thread(LPVOID param)
{
	(void)param;
	Sleep(SESSION_END_EXIT_BUDGET_MS);
	U_LOG_W("Session-end: clean shutdown did not finish within %d ms; terminating the process.",
	        SESSION_END_EXIT_BUDGET_MS);
	TerminateProcess(GetCurrentProcess(), 0);
	return 0;
}

//! A session-end request (WM_ENDSESSION or WM_CLOSE) arrived: take the normal
//! exit path once, and arm the bounded backstop.
static void
session_end_request_exit(const char *what, LPARAM lParam)
{
	static bool requested = false;
	if (requested) {
		return;
	}
	requested = true;

	U_LOG_W("%s received (lParam=0x%llx%s); shutting down cleanly (Restart Manager / session end).", what,
	        (unsigned long long)lParam, (lParam & ENDSESSION_CLOSEAPP) ? " ENDSESSION_CLOSEAPP" : "");

	HANDLE t = CreateThread(NULL, 0, session_end_backstop_thread, NULL, 0, NULL);
	if (t != NULL) {
		CloseHandle(t);
	}

	request_service_exit();
}

/*!
 * Hidden top-level window. It exists only so the service is a closeable
 * Restart Manager application: RM (and logoff/shutdown) deliver
 * WM_QUERYENDSESSION / WM_ENDSESSION / WM_CLOSE to top-level windows, and the
 * tray window is message-only (HWND_MESSAGE), which receives none of them.
 * Without this window the only top-level windows are incidental ones (a GPU
 * driver's dummy window, the IME window) whose default handling never ends the
 * process, so only a forced shutdown (TerminateProcess) could close it. Never
 * shown.
 */
static LRESULT CALLBACK
session_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_QUERYENDSESSION:
		// Always agree. Must answer within RM's 5 s per-message timeout: no
		// UI, no waits.
		//
		// A Restart Manager close request (ENDSESSION_CLOSEAPP: an installer
		// needs a file we hold) is acted on HERE, not on WM_ENDSESSION. RM
		// walks every top-level window of the process, and a vendor library
		// loaded into it can own one on a thread that never pumps messages
		// (hardware-verified: a 2D-to-3D conversion module's hidden GL
		// window). RM then never gets past the query round, WM_ENDSESSION is
		// never sent, and a forced shutdown ends in TerminateProcess with no
		// restart. The query is the one signal that always arrives; the
		// service has no unsaved state, and if another application vetoes
		// the shutdown the next client simply starts the service again.
		// A logoff/shutdown query (no CLOSEAPP) still waits for WM_ENDSESSION.
		if (lParam & ENDSESSION_CLOSEAPP) {
			session_end_request_exit("WM_QUERYENDSESSION", lParam);
		}
		return TRUE;

	case WM_ENDSESSION:
		// wParam FALSE = the session is not ending after all (someone vetoed).
		if (wParam) {
			session_end_request_exit("WM_ENDSESSION", lParam);
		}
		return 0;

	case WM_CLOSE:
		// RM sends WM_CLOSE to an app that did not exit on WM_ENDSESSION, and
		// `taskkill` without /F sends it too: same clean exit.
		session_end_request_exit("WM_CLOSE", lParam);
		return 0;

	// ADR-045 R-c: world events re-evaluate display-processor selection. Both
	// are broadcast only to TOP-LEVEL windows, which is why they land here and
	// not on the message-only tray window. The request only flags the IPC
	// server's debounced worker; the refresh never runs on this thread.
	case WM_DISPLAYCHANGE: ipc_server_request_display_reprobe("display change"); return 0;

	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVNODES_CHANGED) {
			ipc_server_request_display_reprobe("device change");
		}
		return TRUE;

	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}


/*
 *
 * Window procedure
 *
 */

static LRESULT CALLBACK
tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_TRAYICON:
		if (LOWORD(lParam) == WM_RBUTTONUP) {
			show_context_menu(hwnd);
		}
		return 0;

	case WM_COMMAND: {
		UINT cmd_id = LOWORD(wParam);

		// Published-action range — controller defined the menu via
		// HKLM\Software\DisplayXR\WorkspaceControllers\<id>\Actions\*.
		if (cmd_id >= IDM_WORKSPACE_ACTION_BASE && cmd_id <= IDM_WORKSPACE_ACTION_END) {
			const struct workspace_controller_entry *entry =
			    service_orchestrator_get_workspace_entry();
			int idx = (int)(cmd_id - IDM_WORKSPACE_ACTION_BASE);
			if (entry == NULL || idx < 0 || idx >= entry->n_actions) {
				return 0;
			}
			const struct workspace_controller_action *a = &entry->actions[idx];

			enum service_child_mode mode;
			if (lifecycle_type_to_mode(a->type, &mode)) {
				s_config.workspace = mode;
				config_changed();
			} else if (strncmp(a->type, "controller:", 11) == 0) {
				service_orchestrator_dispatch_controller_action(a->type + 11);
			}
			// Unknown / "separator" types: silent no-op (separators are
			// not click targets; unknowns are forward-compat for V2.x).
			return 0;
		}

		switch (cmd_id) {
		// Workspace mode radio group — fallback path when the active
		// controller did not publish an Actions list.
		case IDM_WORKSPACE_ENABLE:
			s_config.workspace = SERVICE_CHILD_ENABLE;
			config_changed();
			break;
		case IDM_WORKSPACE_AUTO:
			s_config.workspace = SERVICE_CHILD_AUTO;
			config_changed();
			break;
		case IDM_WORKSPACE_DISABLE:
			s_config.workspace = SERVICE_CHILD_DISABLE;
			config_changed();
			break;

		// Open the runtime diagnostics Control Panel
		case IDM_CONTROL_PANEL:
			launch_control_panel();
			break;

		// Start on login toggle
		case IDM_START_ON_LOGIN:
			s_config.start_on_login = !s_config.start_on_login;
			config_changed();
			break;

		// Exit
		case IDM_EXIT: request_service_exit(); break;
		}
		return 0;
	}

	case WM_TIMER:
		if (wParam == TRAY_STATUS_TIMER_ID) {
			update_status_tooltip();
		}
		return 0;

	case WM_SETTINGCHANGE:
		// Windows theme changed — swap tray icon to match
		s_nid.hIcon = load_theme_icon();
		Shell_NotifyIconW(NIM_MODIFY, &s_nid);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, TRAY_STATUS_TIMER_ID);
		Shell_NotifyIconW(NIM_DELETE, &s_nid);
		PostQuitMessage(0);
		return 0;

	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}


/*
 *
 * Tray thread
 *
 */

static DWORD WINAPI
tray_thread_body(LPVOID param)
{
	(void)param;

	const wchar_t *cls_name = L"DisplayXRServiceTray";

	WNDCLASSEXW wcex = {0};
	wcex.cbSize = sizeof(WNDCLASSEXW);
	wcex.lpfnWndProc = tray_wnd_proc;
	wcex.hInstance = GetModuleHandleW(NULL);
	wcex.lpszClassName = cls_name;
	RegisterClassExW(&wcex);

	s_tray_hwnd = CreateWindowExW(0, cls_name, L"DisplayXR Service Tray", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
	                              GetModuleHandleW(NULL), NULL);

	if (!s_tray_hwnd) {
		SetEvent(s_ready_event);
		return 1;
	}

	// Hidden top-level session-end window on this same thread (its messages
	// are pumped by the loop below). Best-effort: without it the service still
	// runs, it just cannot be closed by Restart Manager except by force.
	WNDCLASSEXW session_wcex = {0};
	session_wcex.cbSize = sizeof(WNDCLASSEXW);
	session_wcex.lpfnWndProc = session_wnd_proc;
	session_wcex.hInstance = GetModuleHandleW(NULL);
	session_wcex.lpszClassName = L"DisplayXRServiceSession";
	RegisterClassExW(&session_wcex);
	HWND session_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, session_wcex.lpszClassName, L"DisplayXR Service",
	                                    WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
	if (!session_hwnd) {
		U_LOG_W(
		    "Could not create the session-end window (error %lu); Restart Manager can only force-close "
		    "the service.",
		    GetLastError());
	}

	// Set up the tray icon
	ZeroMemory(&s_nid, sizeof(s_nid));
	s_nid.cbSize = sizeof(NOTIFYICONDATAW);
	s_nid.hWnd = s_tray_hwnd;
	s_nid.uID = 1;
	s_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	s_nid.uCallbackMessage = WM_TRAYICON;
	wcscpy_s(s_nid.szTip, ARRAYSIZE(s_nid.szTip), L"DisplayXR Service");

	// Load theme-appropriate icon (black for light taskbar, white for dark)
	s_nid.hIcon = load_theme_icon();

	Shell_NotifyIconW(NIM_ADD, &s_nid);

	// ADR-045 R-e: keep the tooltip's display-processor line current (cheap:
	// reads the loader's records + one platform-state query).
	SetTimer(s_tray_hwnd, TRAY_STATUS_TIMER_ID, TRAY_STATUS_PERIOD_MS, NULL);

	// Signal that we're ready
	SetEvent(s_ready_event);

	// Message loop
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return 0;
}


// #950: the tray thread hosts the message pump, the Ctrl+Space hook and the
// orchestrator spawn path; an exception escaping it is recorded ([TERMINATE])
// before it propagates exactly as before.
static void *
tray_thread_body_adapter(void *p)
{
	return (void *)(uintptr_t)tray_thread_body((LPVOID)p);
}

static DWORD WINAPI
tray_thread_func(LPVOID param)
{
	return (DWORD)(uintptr_t)u_crash_guard_run("tray", tray_thread_body_adapter, (void *)param);
}

/*
 *
 * Public API
 *
 */

bool
service_tray_init(service_tray_shutdown_cb shutdown_cb,
                  service_tray_config_change_cb config_cb,
                  const struct service_config *initial_cfg)
{
	s_shutdown_cb = shutdown_cb;
	s_config_cb = config_cb;
	if (initial_cfg) {
		s_config = *initial_cfg;
	} else {
		s_config.workspace = SERVICE_CHILD_AUTO;
		s_config.start_on_login = true;
	}

	s_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!s_ready_event) {
		return false;
	}

	s_tray_thread = CreateThread(NULL, 0, tray_thread_func, NULL, 0, NULL);
	if (!s_tray_thread) {
		CloseHandle(s_ready_event);
		s_ready_event = NULL;
		return false;
	}

	// Wait for the tray thread to finish initialization (up to 5 seconds)
	WaitForSingleObject(s_ready_event, 5000);
	CloseHandle(s_ready_event);
	s_ready_event = NULL;

	// ADR-045 R-c: plug-in registration changes re-probe selection.
	reg_watch_start();

	return s_tray_hwnd != NULL;
}

void
service_tray_cleanup(void)
{
	reg_watch_stop();

	if (s_tray_hwnd) {
		// Tell the tray thread to exit
		PostMessageW(s_tray_hwnd, WM_DESTROY, 0, 0);
	}

	if (s_tray_thread) {
		WaitForSingleObject(s_tray_thread, 5000);
		CloseHandle(s_tray_thread);
		s_tray_thread = NULL;
	}

	s_tray_hwnd = NULL;
}

void *
service_tray_get_hwnd(void)
{
	return (void *)s_tray_hwnd;
}
