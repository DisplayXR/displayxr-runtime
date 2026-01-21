// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - System tray icon implementation
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#include "watchdog_tray.h"

#include <cstdio>

// Window class name
static const wchar_t *WINDOW_CLASS_NAME = L"SRMonadoWatchdogTrayClass";

// Global pointer for window proc callback
static TrayIcon *g_tray_instance = nullptr;

TrayIcon::TrayIcon(HINSTANCE hInstance, TrayActionCallback callback) : m_hInstance(hInstance), m_callback(callback)
{
	g_tray_instance = this;
}

TrayIcon::~TrayIcon()
{
	destroy();
	g_tray_instance = nullptr;
}

bool
TrayIcon::create()
{
	// Register window class
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = window_proc;
	wc.hInstance = m_hInstance;
	wc.lpszClassName = WINDOW_CLASS_NAME;

	if (!RegisterClassExW(&wc)) {
		if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			return false;
		}
	}

	// Create hidden window for tray messages
	m_hwnd = CreateWindowExW(0, WINDOW_CLASS_NAME, L"SRMonado Watchdog", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
	                         m_hInstance, NULL);

	if (m_hwnd == NULL) {
		return false;
	}

	// Set up tray icon data
	m_nid.cbSize = sizeof(m_nid);
	m_nid.hWnd = m_hwnd;
	m_nid.uID = 1;
	m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	m_nid.uCallbackMessage = WM_TRAYICON;

	// Load icon (use application icon or default)
	m_nid.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(101));
	if (m_nid.hIcon == NULL) {
		m_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	}

	update_tooltip();

	// Add the icon to the tray
	if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
		DestroyWindow(m_hwnd);
		m_hwnd = NULL;
		return false;
	}

	return true;
}

void
TrayIcon::destroy()
{
	if (m_hwnd != NULL) {
		Shell_NotifyIconW(NIM_DELETE, &m_nid);
		DestroyWindow(m_hwnd);
		m_hwnd = NULL;
	}
}

void
TrayIcon::set_service_running(bool running)
{
	if (m_service_running != running) {
		m_service_running = running;
		update_tooltip();
	}
}

void
TrayIcon::set_client_count(int count)
{
	if (m_client_count != count) {
		m_client_count = count;
		update_tooltip();
	}
}

LRESULT CALLBACK
TrayIcon::window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_TRAYICON && g_tray_instance != nullptr) {
		if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
			g_tray_instance->show_context_menu();
		}
		return 0;
	}

	if (msg == WM_COMMAND && g_tray_instance != nullptr) {
		switch (LOWORD(wParam)) {
		case IDM_STATUS:
			if (g_tray_instance->m_callback) {
				g_tray_instance->m_callback(TrayAction::ShowStatus);
			}
			break;
		case IDM_RESTART:
			if (g_tray_instance->m_callback) {
				g_tray_instance->m_callback(TrayAction::RestartService);
			}
			break;
		case IDM_STOP:
			if (g_tray_instance->m_callback) {
				g_tray_instance->m_callback(TrayAction::StopService);
			}
			break;
		case IDM_EXIT:
			if (g_tray_instance->m_callback) {
				g_tray_instance->m_callback(TrayAction::Exit);
			}
			PostQuitMessage(0);
			break;
		}
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void
TrayIcon::show_context_menu()
{
	HMENU menu = CreatePopupMenu();
	if (menu == NULL) {
		return;
	}

	// Build status string
	wchar_t status_text[128];
	swprintf_s(status_text, L"Service: %s, Clients: %d", m_service_running ? L"Running" : L"Stopped",
	           m_client_count);

	AppendMenuW(menu, MF_STRING | MF_DISABLED, IDM_STATUS, status_text);
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, IDM_RESTART, L"Restart Service");
	AppendMenuW(menu, MF_STRING | (m_service_running ? 0 : MF_GRAYED), IDM_STOP, L"Stop Service");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

	// Get cursor position
	POINT pt;
	GetCursorPos(&pt);

	// Required to make the menu dismiss properly
	SetForegroundWindow(m_hwnd);

	// Show menu
	TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, NULL);

	// Required cleanup
	PostMessage(m_hwnd, WM_NULL, 0, 0);

	DestroyMenu(menu);
}

void
TrayIcon::update_tooltip()
{
	swprintf_s(m_nid.szTip, L"SRMonado - Service: %s, Clients: %d", m_service_running ? L"Running" : L"Stopped",
	           m_client_count);

	if (m_hwnd != NULL) {
		Shell_NotifyIconW(NIM_MODIFY, &m_nid);
	}
}
