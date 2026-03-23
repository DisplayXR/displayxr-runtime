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

#define IDI_DISPLAYXR_ICON 101
#define WM_TRAYICON        (WM_APP + 1)
#define IDM_EXIT           1001

static HWND s_tray_hwnd = NULL;
static HANDLE s_tray_thread = NULL;
static HANDLE s_ready_event = NULL;
static NOTIFYICONDATAW s_nid;
static service_tray_shutdown_cb s_shutdown_cb = NULL;

static LRESULT CALLBACK
tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_TRAYICON:
		if (LOWORD(lParam) == WM_RBUTTONUP) {
			POINT pt;
			GetCursorPos(&pt);

			HMENU menu = CreatePopupMenu();
			AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit DisplayXR Service");

			// Required for TrackPopupMenu to work from a background window
			SetForegroundWindow(hwnd);
			TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
			PostMessageW(hwnd, WM_NULL, 0, 0);

			DestroyMenu(menu);
		}
		return 0;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDM_EXIT) {
			if (s_shutdown_cb) {
				s_shutdown_cb();
			}
			PostQuitMessage(0);
		}
		return 0;

	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &s_nid);
		PostQuitMessage(0);
		return 0;

	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

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

	// Load icon from embedded resource, fall back to system default
	s_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_DISPLAYXR_ICON));
	if (!s_nid.hIcon) {
		s_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
	}

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

bool
service_tray_init(service_tray_shutdown_cb shutdown_cb)
{
	s_shutdown_cb = shutdown_cb;

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
