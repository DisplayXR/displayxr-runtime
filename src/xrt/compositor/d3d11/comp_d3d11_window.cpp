// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window implementation (dedicated thread).
 *
 * This module creates a window on a **dedicated thread** for the D3D11 native
 * compositor when no window handle is provided by the application. The window
 * thread owns the HWND and runs its own GetMessage loop. The compositor thread
 * renders independently — Present is never blocked by a modal drag/resize loop
 * because the window-owning thread is separate from the presenting thread.
 *
 * Thread-safe communication uses volatile LONG + InterlockedExchange (idiomatic
 * Windows pattern, compatible with U_TYPED_CALLOC zero-initialization).
 *
 * Based on comp_window_mswin.c but redesigned for drag-free D3D11 compositor use.
 *
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_window.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_config_build.h"

// Include qwerty interface for Win32 input handling (conditional on qwerty driver being built)
#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include "xrt/xrt_device.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

// Environment variable to start in windowed mode
DEBUG_GET_ONCE_BOOL_OPTION(start_windowed, "XRT_COMPOSITOR_START_WINDOWED", false)

// Qwerty input is always enabled for non-session-target apps (Monado-owned window)
// DEBUG_GET_ONCE_BOOL_OPTION(qwerty_enable, "QWERTY_ENABLE", false)

// Window class name
static WCHAR szWindowClass[] = L"MonadoD3D11";
static WCHAR szWindowData[] = L"MonadoD3D11Window";

// Monitor enumeration data
static bool g_use_secondary_monitor = true;
static int g_monitor_info_count = 0;
static MONITORINFOEX g_monitor_info[16] = {};

/*!
 * D3D11 compositor self-owned window structure.
 *
 * The window lives on a dedicated thread. The compositor thread reads
 * dimensions and state via Interlocked* atomic operations.
 */
struct comp_d3d11_window
{
	//! Module instance
	HINSTANCE instance;

	//! Window handle (created on window thread, read-only after creation)
	HWND hwnd;

	//! Registered window class atom (window thread only)
	ATOM window_class;

	//! Requested width (set before thread start, read by window thread)
	uint32_t requested_width;

	//! Requested height (set before thread start, read by window thread)
	uint32_t requested_height;

	//! Current width (window thread writes, compositor thread reads)
	volatile LONG current_width;

	//! Current height (window thread writes, compositor thread reads)
	volatile LONG current_height;

	//! Fullscreen state as LONG for Interlocked* (window thread writes, compositor reads)
	volatile LONG is_fullscreen;

	//! True if user closed the window (window thread writes, compositor reads)
	volatile LONG should_exit;

	//! True while inside a modal move/size loop (window thread writes, compositor reads)
	volatile LONG in_size_move;

	//! Window thread handle
	HANDLE thread_handle;

	//! Window thread ID
	DWORD thread_id;

	//! Manual-reset event signaled after HWND is created on the window thread
	HANDLE window_ready_event;

	//! Auto-reset event: WM_PAINT signals compositor to render during drag
	HANDLE paint_requested_event;

	//! Auto-reset event: compositor signals WM_PAINT that frame is done
	HANDLE paint_done_event;

	//! System devices for qwerty input (set via comp_d3d11_window_set_system_devices)
	//! Can be NULL if not set or qwerty disabled
	struct xrt_system_devices *xsysd;

	//! True if qwerty input is enabled (checked once at startup from QWERTY_ENABLE env var)
	bool qwerty_enabled;
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
 * Window procedure — runs on the window thread.
 *
 * Uses InterlockedExchange to communicate state changes to the compositor
 * thread. No D3D11 operations happen here.
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
		InterlockedExchange(&w->in_size_move, TRUE);
		InvalidateRect(hWnd, NULL, FALSE); // Kick off first WM_PAINT
		return 0;

	case WM_EXITSIZEMOVE:
		InterlockedExchange(&w->in_size_move, FALSE);
		SetEvent(w->paint_requested_event); // Unblock compositor if waiting
		return 0;

	case WM_PAINT:
		if (InterlockedCompareExchange(&w->in_size_move, 0, 0)) {
			// During drag: trigger compositor render, wait for completion.
			// The modal loop is paused while we wait, so the window position
			// is stable between weave() and Present().
			SetEvent(w->paint_requested_event);
			WaitForSingleObject(w->paint_done_event, 100);
			InvalidateRect(hWnd, NULL, FALSE); // Request next WM_PAINT
			return 0;                          // Don't ValidateRect — keep region invalid
		}
		ValidateRect(hWnd, NULL);
		break;

	case WM_CLOSE:
		U_LOG_W("D3D11 window: WM_CLOSE received");
		InterlockedExchange(&w->should_exit, TRUE);
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_KEYDOWN:
		// F11 toggle fullscreen (always handled)
		if (wParam == VK_F11) {
			// Toggle fullscreen state (pure Win32, runs on window thread — safe)
			LONG fs = InterlockedCompareExchange(&w->is_fullscreen, 0, 0);
			fs = !fs;
			InterlockedExchange(&w->is_fullscreen, fs);
			set_fullscreen(hWnd, fs != 0);
			U_LOG_W("D3D11 window: F11 toggled to %s mode", fs ? "fullscreen" : "windowed");
			return 0;
		}
		// 1/2/3: switch device output mode (generic property interface)
		// NOTE: May be replaced by XR_EXT_display_render_mode / xrSetDisplayRenderModeEXT()
		if (w->xsysd != NULL) {
			struct xrt_device *head = w->xsysd->static_roles.head;
			if (wParam == '1') {
				xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 0);
				return 0;
			} else if (wParam == '2') {
				xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1);
				return 0;
			} else if (wParam == '3') {
				xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 2);
				return 0;
			}
		}
		// Forward to qwerty driver if enabled (fall through to WM_KEYUP case)
		// FALLTHROUGH
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
#ifdef XRT_BUILD_DRIVER_QWERTY
		// Forward keyboard input to qwerty driver
		if (w->qwerty_enabled && w->xsysd != NULL) {
			bool handled = false;
			qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
			                     message, wParam, lParam, &handled);
			if (handled) {
				return 0;
			}
		}
#else
		U_LOG_W("D3D11 window: XRT_BUILD_DRIVER_QWERTY not defined!");
#endif
		break;

	// Mouse input for qwerty driver
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL:
#ifdef XRT_BUILD_DRIVER_QWERTY
		if (w->qwerty_enabled && w->xsysd != NULL) {
			bool handled = false;
			qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
			                     message, wParam, lParam, &handled);
			// Don't consume mouse events - let them propagate for other uses
		}
#endif
		break;

	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			LONG new_w = LOWORD(lParam);
			LONG new_h = HIWORD(lParam);
			if (new_w > 0 && new_h > 0) {
				InterlockedExchange(&w->current_width, new_w);
				InterlockedExchange(&w->current_height, new_h);
				U_LOG_D("D3D11 window: resized to %ldx%ld", new_w, new_h);
			}
		}
		break;

	default:
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	return 0;
}

/*!
 * Window thread function.
 *
 * Creates the window, runs the message loop, and cleans up the window class
 * on exit. The compositor thread signals shutdown by posting WM_CLOSE.
 */
static DWORD WINAPI
window_thread_func(LPVOID param)
{
	struct comp_d3d11_window *w = (struct comp_d3d11_window *)param;

	U_LOG_W("D3D11 window thread: started (tid=%lu)", GetCurrentThreadId());

	// Register window class on this thread
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
			U_LOG_W("D3D11 window thread: Window class already registered, reusing");
		} else {
			U_LOG_E("D3D11 window thread: RegisterClassExW failed with error %lu", err);
			SetEvent(w->window_ready_event);
			return 1;
		}
	}

	// Position on Leia/secondary monitor if available
	RECT rc = {0, 0, (LONG)w->requested_width, (LONG)w->requested_height};
	int monitor_x = 0;
	int monitor_y = 0;
	if (get_leia_display_top_left(&monitor_x, &monitor_y)) {
		rc.left = monitor_x;
		rc.top = monitor_y;
		rc.right = monitor_x + (LONG)w->requested_width;
		rc.bottom = monitor_y + (LONG)w->requested_height;
	}

	U_LOG_W("D3D11 window thread: Creating window at (%d, %d) size %ux%u",
	        (int)rc.left, (int)rc.top, w->requested_width, w->requested_height);

	HWND hwnd = CreateWindowExW(0, szWindowClass, L"Monado D3D11", WS_OVERLAPPEDWINDOW, rc.left, rc.top,
	                            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, w->instance, NULL);

	if (hwnd == NULL) {
		DWORD err = GetLastError();
		U_LOG_E("D3D11 window thread: CreateWindowExW failed with error %lu", err);
		if (w->window_class) {
			UnregisterClassW((LPCWSTR)(uintptr_t)w->window_class, w->instance);
			w->window_class = 0;
		}
		SetEvent(w->window_ready_event);
		return 1;
	}

	// Check if we should start fullscreen
	bool start_fullscreen = !debug_get_bool_option_start_windowed();
	if (start_fullscreen) {
		InterlockedExchange(&w->is_fullscreen, TRUE);
		set_fullscreen(hwnd, true);
	}

	// Associate window data before showing
	SetPropW(hwnd, szWindowData, w);
	SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)w);

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	// Store initial client dimensions
	RECT client_rect;
	if (GetClientRect(hwnd, &client_rect)) {
		InterlockedExchange(&w->current_width, client_rect.right - client_rect.left);
		InterlockedExchange(&w->current_height, client_rect.bottom - client_rect.top);
	}

	// Store HWND and signal the creating thread that the window is ready.
	// Use a write barrier to ensure hwnd is visible before the event is signaled.
	w->hwnd = hwnd;
	MemoryBarrier();
	SetEvent(w->window_ready_event);

	U_LOG_W("D3D11 window thread: Window created successfully, HWND=%p", (void *)hwnd);

	// Message loop — blocks on GetMessage, exits on WM_QUIT (posted by WM_DESTROY → PostQuitMessage)
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	U_LOG_W("D3D11 window thread: Message loop exited");

	// Cleanup: unregister window class (must be done on the thread that registered it)
	if (w->window_class) {
		UnregisterClassW((LPCWSTR)(uintptr_t)w->window_class, w->instance);
		w->window_class = 0;
	}

	U_LOG_W("D3D11 window thread: exiting");
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
	w->requested_width = width > 0 ? width : 1920;
	w->requested_height = height > 0 ? height : 1080;
	w->xsysd = NULL;
	w->qwerty_enabled = true;  // Always enabled for Monado-owned windows

	U_LOG_W("D3D11 window: QWERTY input ENABLED");

	U_LOG_W("D3D11 window: Creating window on dedicated thread (%ux%u)", w->requested_width, w->requested_height);

	// Create manual-reset event for window-ready synchronization
	w->window_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (w->window_ready_event == NULL) {
		U_LOG_E("D3D11 window: Failed to create window_ready_event");
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Create auto-reset events for WM_PAINT-synchronized rendering during drag
	w->paint_requested_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	w->paint_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (w->paint_requested_event == NULL || w->paint_done_event == NULL) {
		U_LOG_E("D3D11 window: Failed to create paint sync events");
		if (w->paint_requested_event != NULL) CloseHandle(w->paint_requested_event);
		if (w->paint_done_event != NULL) CloseHandle(w->paint_done_event);
		CloseHandle(w->window_ready_event);
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Start window thread
	w->thread_handle = CreateThread(NULL, 0, window_thread_func, w, 0, &w->thread_id);
	if (w->thread_handle == NULL) {
		U_LOG_E("D3D11 window: Failed to create window thread");
		CloseHandle(w->window_ready_event);
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Wait for the window thread to create the HWND (10 second timeout)
	DWORD wait_result = WaitForSingleObject(w->window_ready_event, 10000);
	if (wait_result != WAIT_OBJECT_0) {
		U_LOG_E("D3D11 window: Timeout waiting for window thread to create HWND");
		// Try to terminate the thread gracefully
		WaitForSingleObject(w->thread_handle, 1000);
		CloseHandle(w->thread_handle);
		CloseHandle(w->window_ready_event);
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Check if the window was actually created
	if (w->hwnd == NULL) {
		U_LOG_E("D3D11 window: Window thread failed to create HWND");
		WaitForSingleObject(w->thread_handle, 5000);
		CloseHandle(w->thread_handle);
		CloseHandle(w->window_ready_event);
		free(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("D3D11 window: Window created on thread %lu, HWND=%p", w->thread_id, (void *)w->hwnd);
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

	U_LOG_W("D3D11 window: Destroying window");

	// Tell the window thread to close (PostMessage is safe cross-thread)
	if (w->hwnd != NULL) {
		PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
	}

	// Wait for window thread to exit
	if (w->thread_handle != NULL) {
		DWORD wait_result = WaitForSingleObject(w->thread_handle, 5000);
		if (wait_result != WAIT_OBJECT_0) {
			U_LOG_W("D3D11 window: Window thread did not exit within timeout");
		}
		CloseHandle(w->thread_handle);
	}

	if (w->window_ready_event != NULL) {
		CloseHandle(w->window_ready_event);
	}

	if (w->paint_requested_event != NULL) {
		CloseHandle(w->paint_requested_event);
	}

	if (w->paint_done_event != NULL) {
		CloseHandle(w->paint_done_event);
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
	return window->hwnd != NULL && !InterlockedCompareExchange(&window->should_exit, 0, 0);
}

extern "C" void
comp_d3d11_window_get_dimensions(struct comp_d3d11_window *window, uint32_t *out_width, uint32_t *out_height)
{
	if (window == NULL) {
		*out_width = 0;
		*out_height = 0;
		return;
	}
	*out_width = (uint32_t)InterlockedCompareExchange(&window->current_width, 0, 0);
	*out_height = (uint32_t)InterlockedCompareExchange(&window->current_height, 0, 0);
}

extern "C" bool
comp_d3d11_window_is_in_size_move(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return false;
	}
	return InterlockedCompareExchange(&window->in_size_move, 0, 0) != 0;
}

extern "C" void
comp_d3d11_window_pump_messages(struct comp_d3d11_window *window)
{
	// No-op: the dedicated window thread runs its own message loop.
	(void)window;
}

extern "C" void
comp_d3d11_window_set_repaint_callback(struct comp_d3d11_window *window,
                                        void (*callback)(void *userdata),
                                        void *userdata)
{
	// No-op: with the dedicated window thread, the compositor thread continues
	// rendering during drag/resize. No repaint callback is needed.
	(void)window;
	(void)callback;
	(void)userdata;
}

extern "C" bool
comp_d3d11_window_wait_for_paint(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return false;
	}
	if (!InterlockedCompareExchange(&window->in_size_move, 0, 0)) {
		return false;
	}

	// Block until WM_PAINT fires (or drag ends via WM_EXITSIZEMOVE signal)
	WaitForSingleObject(window->paint_requested_event, 50);

	// Return true if we should render (still in drag), false if drag ended
	return InterlockedCompareExchange(&window->in_size_move, 0, 0) != 0;
}

extern "C" void
comp_d3d11_window_signal_paint_done(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return;
	}
	SetEvent(window->paint_done_event);
}

extern "C" void
comp_d3d11_window_set_system_devices(struct comp_d3d11_window *window,
                                      struct xrt_system_devices *xsysd)
{
	if (window == NULL) {
		return;
	}

	window->xsysd = xsysd;

	U_LOG_W("D3D11 window: set_system_devices called - xsysd=%p qwerty_enabled=%d",
	        (void *)xsysd, window->qwerty_enabled);

	if (xsysd != NULL) {
		U_LOG_W("D3D11 window: xsysd has %u devices", (unsigned)xsysd->xdev_count);
		U_LOG_W("D3D11 window: System devices set - QWERTY input active");
		U_LOG_W("D3D11 window: Controls: WASDQE=move, Arrows=rotate, RightClick+Drag=look, Shift=sprint");
	}
}
