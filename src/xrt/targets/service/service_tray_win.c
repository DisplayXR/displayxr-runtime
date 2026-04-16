// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows system tray icon for DisplayXR service.
 * @ingroup ipc
 */

#include "service_tray_win.h"

#include <windows.h>
#include <shellapi.h>

#define IDI_DISPLAYXR_ICON_WHITE 101
#define IDI_DISPLAYXR_ICON_BLACK 102
#define WM_TRAYICON              (WM_APP + 1)

// Menu command IDs
#define IDM_SHELL_ENABLE    1010
#define IDM_SHELL_DISABLE   1011
#define IDM_SHELL_AUTO      1012
#define IDM_BRIDGE_ENABLE   1020
#define IDM_BRIDGE_DISABLE  1021
#define IDM_BRIDGE_AUTO     1022
#define IDM_START_ON_LOGIN  1030
#define IDM_EXIT            1001


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

//! Map service_child_mode to the corresponding menu ID for the shell submenu.
static UINT
shell_mode_to_id(enum service_child_mode m)
{
	switch (m) {
	case SERVICE_CHILD_ENABLE: return IDM_SHELL_ENABLE;
	case SERVICE_CHILD_DISABLE: return IDM_SHELL_DISABLE;
	default: return IDM_SHELL_AUTO;
	}
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

	// Shell submenu (radio group: Enable, Auto, Disable)
	HMENU shell_sub = CreatePopupMenu();
	AppendMenuW(shell_sub, MF_STRING, IDM_SHELL_ENABLE, L"Enable");
	AppendMenuW(shell_sub, MF_STRING, IDM_SHELL_AUTO, L"Auto");
	AppendMenuW(shell_sub, MF_STRING, IDM_SHELL_DISABLE, L"Disable");
	CheckMenuRadioItem(shell_sub, IDM_SHELL_ENABLE, IDM_SHELL_AUTO,
	                   shell_mode_to_id(s_config.shell), MF_BYCOMMAND);

	// Bridge submenu (radio group: Enable, Auto, Disable)
	HMENU bridge_sub = CreatePopupMenu();
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_ENABLE, L"Enable");
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_AUTO, L"Auto");
	AppendMenuW(bridge_sub, MF_STRING, IDM_BRIDGE_DISABLE, L"Disable");
	CheckMenuRadioItem(bridge_sub, IDM_BRIDGE_ENABLE, IDM_BRIDGE_AUTO,
	                   bridge_mode_to_id(s_config.bridge), MF_BYCOMMAND);

	// Main menu
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_POPUP, (UINT_PTR)shell_sub, L"Spatial Shell");
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

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		// Shell mode radio group
		case IDM_SHELL_ENABLE:
			s_config.shell = SERVICE_CHILD_ENABLE;
			config_changed();
			break;
		case IDM_SHELL_AUTO:
			s_config.shell = SERVICE_CHILD_AUTO;
			config_changed();
			break;
		case IDM_SHELL_DISABLE:
			s_config.shell = SERVICE_CHILD_DISABLE;
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
		s_config.shell = SERVICE_CHILD_AUTO;
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
