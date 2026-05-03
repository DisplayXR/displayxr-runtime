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
#include <shellapi.h>
#include <string.h>

#define IDI_DISPLAYXR_ICON_WHITE 101
#define IDI_DISPLAYXR_ICON_BLACK 102
#define WM_TRAYICON              (WM_APP + 1)

// Menu command IDs
#define IDM_WORKSPACE_ENABLE    1010
#define IDM_WORKSPACE_DISABLE   1011
#define IDM_WORKSPACE_AUTO      1012
#define IDM_BRIDGE_ENABLE   1020
#define IDM_BRIDGE_DISABLE  1021
#define IDM_BRIDGE_AUTO     1022
#define IDM_START_ON_LOGIN  1030
#define IDM_EXIT            1001

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

//! Map service_child_mode to the corresponding menu ID for the bridge submenu.
static UINT
bridge_mode_to_id(enum service_child_mode m)
{
	switch (m) {
	case SERVICE_CHILD_ENABLE: return IDM_BRIDGE_ENABLE;
	case SERVICE_CHILD_DISABLE: return IDM_BRIDGE_DISABLE;
	default: return IDM_BRIDGE_AUTO;
	}
}

//! Build and show the tray context menu at the cursor position.
static void
show_context_menu(HWND hwnd)
{
	POINT pt;
	GetCursorPos(&pt);

	// Bridge submenu (radio group: Enable, Auto, Disable)
	HMENU bridge_sub = CreatePopupMenu();
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_ENABLE, L"Enable");
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_AUTO, L"Auto");
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_DISABLE, L"Disable");
	CheckMenuRadioItem(bridge_sub, IDM_BRIDGE_ENABLE, IDM_BRIDGE_AUTO,
	                   bridge_mode_to_id(s_config.bridge), MF_BYCOMMAND);

	// Main menu
	HMENU menu = CreatePopupMenu();

	// Workspace submenu — only built when a controller binary is detected.
	// Bare runtime (no controller installed) gets just the Bridge entry,
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

	AppendMenuW(menu, MF_POPUP, (UINT_PTR)bridge_sub, L"WebXR Bridge");
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

		// Bridge mode radio group
		case IDM_BRIDGE_ENABLE:
			s_config.bridge = SERVICE_CHILD_ENABLE;
			config_changed();
			break;
		case IDM_BRIDGE_AUTO:
			s_config.bridge = SERVICE_CHILD_AUTO;
			config_changed();
			break;
		case IDM_BRIDGE_DISABLE:
			s_config.bridge = SERVICE_CHILD_DISABLE;
			config_changed();
			break;

		// Start on login toggle
		case IDM_START_ON_LOGIN:
			s_config.start_on_login = !s_config.start_on_login;
			config_changed();
			break;

		// Exit
		case IDM_EXIT:
			if (s_shutdown_cb) {
				s_shutdown_cb();
			}
			PostQuitMessage(0);
			break;
		}
		return 0;
	}

	case WM_SETTINGCHANGE:
		// Windows theme changed — swap tray icon to match
		s_nid.hIcon = load_theme_icon();
		Shell_NotifyIconW(NIM_MODIFY, &s_nid);
		return 0;

	case WM_DESTROY:
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
tray_thread_func(LPVOID param)
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
		s_config.bridge = SERVICE_CHILD_AUTO;
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

	return s_tray_hwnd != NULL;
}

void
service_tray_cleanup(void)
{
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
