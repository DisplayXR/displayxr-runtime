// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  LeiaXR Runtime Switcher - Main entry point
 *
 * A simple utility to switch between SRHydra and SRMonado OpenXR runtimes.
 *
 * @author Leia Inc.
 * @ingroup targets_switcher
 */

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>

#include "switcher_registry.h"

// Enable visual styles
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Dialog dimensions
static const int DIALOG_WIDTH = 400;
static const int DIALOG_HEIGHT = 300;

// Control IDs
static const int ID_LISTBOX = 1001;
static const int ID_APPLY = 1002;
static const int ID_CANCEL = 1003;
static const int ID_REFRESH = 1004;
static const int ID_STATUS = 1005;

// Global state
static RuntimeRegistry *g_registry = nullptr;
static HWND g_hwnd_list = NULL;
static HWND g_hwnd_status = NULL;
static HFONT g_font = NULL;

/*!
 * Update the listbox with current runtime information.
 */
static void
update_runtime_list(HWND hwnd_list)
{
	SendMessageW(hwnd_list, LB_RESETCONTENT, 0, 0);

	const auto &runtimes = g_registry->get_runtimes();
	for (size_t i = 0; i < runtimes.size(); ++i) {
		std::wstring item = runtimes[i].name;
		if (runtimes[i].is_active) {
			item += L"  \x2190 Active"; // Left arrow
		}
		SendMessageW(hwnd_list, LB_ADDSTRING, 0, (LPARAM)item.c_str());
	}

	// Select the active runtime
	int active = g_registry->get_active_index();
	if (active >= 0) {
		SendMessageW(hwnd_list, LB_SETCURSEL, active, 0);
	}
}

/*!
 * Update the status text.
 */
static void
update_status(const wchar_t *text)
{
	if (g_hwnd_status) {
		SetWindowTextW(g_hwnd_status, text);
	}
}

/*!
 * Handle Apply button click.
 */
static void
on_apply(HWND hwnd)
{
	int sel = (int)SendMessageW(g_hwnd_list, LB_GETCURSEL, 0, 0);
	if (sel == LB_ERR) {
		update_status(L"Please select a runtime first.");
		return;
	}

	const auto &runtimes = g_registry->get_runtimes();
	if ((size_t)sel >= runtimes.size()) {
		return;
	}

	if (runtimes[sel].is_active) {
		update_status(L"Selected runtime is already active.");
		return;
	}

	if (g_registry->set_active_runtime(sel)) {
		update_runtime_list(g_hwnd_list);
		std::wstring msg = runtimes[sel].name + L" is now the active OpenXR runtime.";
		update_status(msg.c_str());
	} else {
		update_status(L"Failed to set runtime. Try running as Administrator.");
		MessageBoxW(hwnd, L"Failed to update the registry.\n\nPlease run this application as Administrator.",
		            L"Permission Required", MB_ICONWARNING | MB_OK);
	}
}

/*!
 * Window procedure.
 */
static LRESULT CALLBACK
window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CREATE: {
		// Create title label
		HWND label = CreateWindowExW(0, L"STATIC", L"Available OpenXR Runtimes:", WS_CHILD | WS_VISIBLE, 20, 15,
		                             DIALOG_WIDTH - 40, 20, hwnd, NULL, NULL, NULL);
		SendMessageW(label, WM_SETFONT, (WPARAM)g_font, TRUE);

		// Create listbox
		g_hwnd_list =
		    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
		                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 20, 40,
		                    DIALOG_WIDTH - 40, 140, hwnd, (HMENU)(INT_PTR)ID_LISTBOX, NULL, NULL);
		SendMessageW(g_hwnd_list, WM_SETFONT, (WPARAM)g_font, TRUE);

		// Create status label
		g_hwnd_status = CreateWindowExW(0, L"STATIC", L"Select a runtime and click Apply to switch.",
		                                WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 190, DIALOG_WIDTH - 40, 40, hwnd,
		                                (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
		SendMessageW(g_hwnd_status, WM_SETFONT, (WPARAM)g_font, TRUE);

		// Create buttons
		int btn_y = DIALOG_HEIGHT - 60;
		int btn_width = 80;
		int btn_spacing = 10;
		int total_width = btn_width * 3 + btn_spacing * 2;
		int btn_x = (DIALOG_WIDTH - total_width) / 2;

		HWND btn_apply = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
		                                 btn_x, btn_y, btn_width, 30, hwnd, (HMENU)(INT_PTR)ID_APPLY, NULL, NULL);
		SendMessageW(btn_apply, WM_SETFONT, (WPARAM)g_font, TRUE);

		btn_x += btn_width + btn_spacing;
		HWND btn_refresh =
		    CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, btn_x, btn_y, btn_width, 30, hwnd,
		                    (HMENU)(INT_PTR)ID_REFRESH, NULL, NULL);
		SendMessageW(btn_refresh, WM_SETFONT, (WPARAM)g_font, TRUE);

		btn_x += btn_width + btn_spacing;
		HWND btn_cancel =
		    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE, btn_x, btn_y, btn_width, 30, hwnd,
		                    (HMENU)(INT_PTR)ID_CANCEL, NULL, NULL);
		SendMessageW(btn_cancel, WM_SETFONT, (WPARAM)g_font, TRUE);

		// Populate the list
		update_runtime_list(g_hwnd_list);

		if (g_registry->get_runtimes().empty()) {
			update_status(L"No LeiaXR runtimes found. Install SRMonado or SRHydra.");
		}

		return 0;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case ID_APPLY: on_apply(hwnd); break;
		case ID_REFRESH:
			g_registry->refresh();
			update_runtime_list(g_hwnd_list);
			update_status(L"Runtime list refreshed.");
			break;
		case ID_CANCEL: DestroyWindow(hwnd); break;
		case ID_LISTBOX:
			if (HIWORD(wParam) == LBN_DBLCLK) {
				on_apply(hwnd);
			}
			break;
		}
		return 0;

	case WM_DESTROY: PostQuitMessage(0); return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/*!
 * Application entry point.
 */
int WINAPI
wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	(void)hPrevInstance;
	(void)lpCmdLine;

	// Initialize common controls
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);

	// Create font
	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	g_font = CreateFontIndirectW(&ncm.lfMessageFont);

	// Initialize registry scanner
	g_registry = new RuntimeRegistry();

	// Register window class
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = window_proc;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"LeiaXRSwitcherClass";

	if (!RegisterClassExW(&wc)) {
		MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_ICONERROR);
		return 1;
	}

	// Calculate window size (including non-client area)
	RECT rc = {0, 0, DIALOG_WIDTH, DIALOG_HEIGHT};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE);
	int width = rc.right - rc.left;
	int height = rc.bottom - rc.top;

	// Center on screen
	int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
	int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

	// Create window
	HWND hwnd =
	    CreateWindowExW(0, L"LeiaXRSwitcherClass", L"LeiaXR Runtime Switcher",
	                    WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), x, y, width, height, NULL, NULL,
	                    hInstance, NULL);

	if (!hwnd) {
		MessageBoxW(NULL, L"Failed to create window.", L"Error", MB_ICONERROR);
		return 1;
	}

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// Message loop
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Cleanup
	delete g_registry;
	if (g_font) {
		DeleteObject(g_font);
	}

	return (int)msg.wParam;
}
