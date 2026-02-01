// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window implementation.
 *
 * This module creates a window on the calling thread for the D3D11 native
 * compositor when no window handle is provided by the application.
 * The caller is responsible for pumping Win32 messages (either via
 * @ref comp_d3d11_window_pump_messages or its own PeekMessage loop).
 *
 * Based on comp_window_mswin.c but simplified for D3D11 compositor use.
 *
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_window.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

// Environment variable to start in windowed mode
DEBUG_GET_ONCE_BOOL_OPTION(start_windowed, "XRT_COMPOSITOR_START_WINDOWED", false)

// Window class name
static WCHAR szWindowClass[] = L"MonadoD3D11";
static WCHAR szWindowData[] = L"MonadoD3D11Window";

// Monitor enumeration data
static bool g_use_secondary_monitor = true;
static int g_monitor_info_count = 0;
static MONITORINFOEX g_monitor_info[16] = {};

/*!
 * D3D11 compositor self-owned window structure.
 */
struct comp_d3d11_window
{
	//! Module instance
	HINSTANCE instance;

	//! Window handle
	HWND hwnd;

	//! Registered window class atom
	ATOM window_class;

	//! Requested width
	uint32_t width;

	//! Requested height
	uint32_t height;

	//! Current fullscreen state (for F11 toggle)
	bool is_fullscreen;

	//! True if user closed the window
	bool should_exit;

	//! True while inside a modal move/size loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE)
	bool in_size_move;

	//! Callback invoked from WM_PAINT during drag/resize
	void (*repaint_callback)(void *userdata);

	//! Opaque pointer forwarded to @ref repaint_callback
	void *repaint_userdata;
};

// Forward declarations
static void set_fullscreen(HWND hWnd, bool fullscreen);

/*!
 * Monitor enumeration callback.
 */
static BOOL CALLBACK
monitor_enum_proc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	MONITORINFOEX info;
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	GetMonitorInfo(hMonitor, (LPMONITORINFO)&info);
	if (g_monitor_info_count < 16) {
		g_monitor_info[g_monitor_info_count++] = info;
	}
	return TRUE;
}

/*!
 * Get the top-left coordinate of the Leia display, or secondary monitor.
 */
static bool
get_leia_display_top_left(int *x, int *y)
{
	// Get connected monitor info
	g_monitor_info_count = 0;
	EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, 0);

	// Look for Leia display by device ID
	const char kLeiaDisplayIDPrefix[] = "\\\\?\\DISPLAY#AUO2E9A";
	for (int iMonitor = 0; iMonitor < g_monitor_info_count; ++iMonitor) {
		int iDevice = 0;
		while (true) {
			DISPLAY_DEVICE device;
			device.cb = sizeof(device);
			if (!EnumDisplayDevices(g_monitor_info[iMonitor].szDevice, iDevice, &device,
			                        EDD_GET_DEVICE_INTERFACE_NAME)) {
				break;
			}

			if (strncmp(device.DeviceID, kLeiaDisplayIDPrefix, sizeof(kLeiaDisplayIDPrefix) - 1) == 0) {
				RECT *rect = &g_monitor_info[iMonitor].rcMonitor;
				*x = rect->left;
				*y = rect->top;
				return true;
			}
			++iDevice;
		}
	}

	// Fall back to secondary monitor if available
	if (g_use_secondary_monitor) {
		for (int i = 0; i < g_monitor_info_count; i++) {
			if (0 == (g_monitor_info[i].dwFlags & MONITORINFOF_PRIMARY)) {
				*x = g_monitor_info[i].rcMonitor.left;
				*y = g_monitor_info[i].rcMonitor.top;
				return true;
			}
		}
	}

	// Only one display - use primary
	*x = 0;
	*y = 0;
	return false;
}

/*!
 * Set window fullscreen state.
 */
static void
set_fullscreen(HWND hWnd, bool fullscreen)
{
	static int windowPrevX = 0;
	static int windowPrevY = 0;
	static int windowPrevWidth = 0;
	static int windowPrevHeight = 0;

	DWORD style = GetWindowLong(hWnd, GWL_STYLE);
	if (fullscreen) {
		RECT rect;
		MONITORINFO mi = {sizeof(mi)};
		GetWindowRect(hWnd, &rect);

		windowPrevX = rect.left;
		windowPrevY = rect.top;
		windowPrevWidth = rect.right - rect.left;
		windowPrevHeight = rect.bottom - rect.top;

		GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLong(hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
		             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
		             SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	} else {
		MONITORINFO mi = {sizeof(mi)};
		UINT flags = SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
		GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLong(hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPos(hWnd, HWND_NOTOPMOST, windowPrevX, windowPrevY, windowPrevWidth, windowPrevHeight, flags);
	}
}

/*!
 * Window procedure.
 */
static LRESULT CALLBACK
wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	struct comp_d3d11_window *w = (struct comp_d3d11_window *)GetPropW(hWnd, szWindowData);

	if (!w) {
		// Window not fully set up yet
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	switch (message) {
	case WM_ENTERSIZEMOVE:
		w->in_size_move = true;
		U_LOG_E("D3D11 window: WM_ENTERSIZEMOVE — entering modal drag/resize loop");
		InvalidateRect(hWnd, NULL, FALSE); // Kick off the first WM_PAINT (essential for MOVE drags where WM_SIZE never fires)
		return 0;

	case WM_EXITSIZEMOVE:
		w->in_size_move = false;
		U_LOG_E("D3D11 window: WM_EXITSIZEMOVE — left modal drag/resize loop");
		return 0;

	case WM_PAINT:
		if (w->in_size_move && w->repaint_callback != NULL) {
			static int paint_log_counter = 0;
			if (++paint_log_counter % 60 == 1) {
				U_LOG_I("D3D11 window: WM_PAINT during drag — invoking repaint callback");
			}
			w->repaint_callback(w->repaint_userdata);
			
			// Validate the rect to satisfy Windows (prevents "ghost" paints or system throttling)
			ValidateRect(hWnd, NULL);
			
			// Immediately invalidate again to force the NEXT frame (keep the loop going)
			InvalidateRect(hWnd, NULL, FALSE);
			return 0;
		}
		ValidateRect(hWnd, NULL);
		break;

	case WM_CLOSE:
		U_LOG_I("D3D11 window: WM_CLOSE received");
		w->should_exit = true;
		DestroyWindow(hWnd);
		w->hwnd = NULL;
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_KEYDOWN:
		if (wParam == VK_F11) {
			// Toggle fullscreen state
			w->is_fullscreen = !w->is_fullscreen;
			set_fullscreen(hWnd, w->is_fullscreen);
			U_LOG_I("D3D11 window: F11 toggled to %s mode", w->is_fullscreen ? "fullscreen" : "windowed");
		}
		break;

	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			w->width = LOWORD(lParam);
			w->height = HIWORD(lParam);
			if (w->width > 0 && w->height > 0) {
				U_LOG_D("D3D11 window: resized to %ux%u", w->width, w->height);
			}
			// During drag, trigger a repaint so the swapchain resizes
			// and the weaver re-presents with correct phase alignment.
			if (w->in_size_move) {
				InvalidateRect(hWnd, NULL, FALSE);
			}
		}
		break;

	default:
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	return 0;
}

/*
 *
 * Public API
 *
 */

extern "C" xrt_result_t
comp_d3d11_window_create(uint32_t width, uint32_t height, struct comp_d3d11_window **out)
{
	struct comp_d3d11_window *w = U_TYPED_CALLOC(struct comp_d3d11_window);
	if (w == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	w->instance = GetModuleHandle(NULL);
	w->width = width > 0 ? width : 1920;
	w->height = height > 0 ? height : 1080;

	U_LOG_I("D3D11 window: Creating window on calling thread (%ux%u)", w->width, w->height);

	// Register window class
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEXW);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = wnd_proc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = w->instance;
	wcex.lpszClassName = szWindowClass;
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

	w->window_class = RegisterClassExW(&wcex);
	if (!w->window_class) {
		DWORD err = GetLastError();
		if (err == ERROR_CLASS_ALREADY_EXISTS) {
			// Another session already registered the class - reuse it
			U_LOG_I("D3D11 window: Window class already registered, reusing");
		} else {
			U_LOG_E("D3D11 window: RegisterClassExW failed with error %lu", err);
			free(w);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
	}

	// Position on Leia/secondary monitor if available
	RECT rc = {0, 0, (LONG)w->width, (LONG)w->height};
	int monitor_x = 0;
	int monitor_y = 0;
	if (get_leia_display_top_left(&monitor_x, &monitor_y)) {
		rc.left = monitor_x;
		rc.top = monitor_y;
		rc.right = monitor_x + w->width;
		rc.bottom = monitor_y + w->height;
	}

	U_LOG_I("D3D11 window: Creating window at (%d, %d) size %ux%u", (int)rc.left, (int)rc.top, w->width, w->height);

	w->hwnd = CreateWindowExW(0, szWindowClass, L"Monado D3D11", WS_OVERLAPPEDWINDOW, rc.left, rc.top,
	                          rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, w->instance, NULL);

	if (w->hwnd == NULL) {
		DWORD err = GetLastError();
		U_LOG_E("D3D11 window: CreateWindowExW failed with error %lu", err);
		if (w->window_class) {
			UnregisterClassW((LPCWSTR)(uintptr_t)w->window_class, w->instance);
		}
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Set fullscreen by default unless XRT_COMPOSITOR_START_WINDOWED=1
	w->is_fullscreen = !debug_get_bool_option_start_windowed();
	if (w->is_fullscreen) {
		set_fullscreen(w->hwnd, true);
	}

	// Associate window data
	SetPropW(w->hwnd, szWindowData, w);
	SetWindowLongPtr(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
	ShowWindow(w->hwnd, SW_SHOWDEFAULT);
	UpdateWindow(w->hwnd);

	// Pump initial messages so the window is fully visible before we return
	MSG msg;
	while (PeekMessageW(&msg, w->hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	U_LOG_I("D3D11 window: Window created successfully, HWND=%p", (void *)w->hwnd);
	*out = w;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_window_destroy(struct comp_d3d11_window **window)
{
	if (window == NULL || *window == NULL) {
		return;
	}

	struct comp_d3d11_window *w = *window;

	U_LOG_I("D3D11 window: Destroying window");

	if (w->hwnd != NULL) {
		DestroyWindow(w->hwnd);
		w->hwnd = NULL;
	}

	if (w->window_class) {
		UnregisterClassW((LPCWSTR)(uintptr_t)w->window_class, w->instance);
	}

	free(w);
	*window = NULL;
}

extern "C" void *
comp_d3d11_window_get_hwnd(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return NULL;
	}
	return (void *)window->hwnd;
}

extern "C" bool
comp_d3d11_window_is_valid(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return false;
	}
	return window->hwnd != NULL && !window->should_exit;
}

extern "C" void
comp_d3d11_window_get_dimensions(struct comp_d3d11_window *window, uint32_t *out_width, uint32_t *out_height)
{
	if (window == NULL) {
		*out_width = 0;
		*out_height = 0;
		return;
	}
	*out_width = window->width;
	*out_height = window->height;
}

extern "C" bool
comp_d3d11_window_is_in_size_move(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return false;
	}
	return window->in_size_move;
}

extern "C" void
comp_d3d11_window_pump_messages(struct comp_d3d11_window *window)
{
	if (window == NULL || window->hwnd == NULL) {
		return;
	}

	MSG msg;
	while (PeekMessageW(&msg, window->hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

extern "C" void
comp_d3d11_window_set_repaint_callback(struct comp_d3d11_window *window,
                                        void (*callback)(void *userdata),
                                        void *userdata)
{
	if (window == NULL) {
		return;
	}
	window->repaint_callback = callback;
	window->repaint_userdata = userdata;
}
