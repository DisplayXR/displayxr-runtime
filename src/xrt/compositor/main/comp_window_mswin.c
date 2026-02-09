// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Microsoft Windows window code.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 */

#include <stdlib.h>
#include <string.h>
#include "xrt/xrt_compiler.h"
#include "main/comp_window.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "os/os_threading.h"


#undef ALLOW_CLOSING_WINDOW

DEBUG_GET_ONCE_BOOL_OPTION(start_windowed, "XRT_COMPOSITOR_START_WINDOWED", false)

static bool          g_use_secondary_monitor = true;
static int           g_monitor_info_count    = 0;
static MONITORINFOEX g_monitor_info[16]      = { 0 };

/*
 *
 * Private structs.
 *
 */

/*!
 * A Microsoft Windows window.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_mswin
{
	struct comp_target_swapchain base;
	struct os_thread_helper oth;

	ATOM window_class;
	HINSTANCE instance;
	HWND window;

	//! True if we created the window (default), false if external window provided
	bool owns_window;

	bool fullscreen_requested;
	bool should_exit;
	bool thread_started;
	bool thread_exited;

	//! Current fullscreen state (for F11 toggle)
	bool is_fullscreen;

	//! TRUE between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (drag/resize modal loop)
	volatile LONG in_size_move;
	//! Auto-reset event: window thread signals when WM_PAINT fires during drag
	HANDLE paint_requested_event;
	//! Auto-reset event: compositor signals when rendering is done during drag
	HANDLE paint_done_event;
};

static WCHAR szWindowClass[] = L"Monado";
static WCHAR szWindowData[] = L"MonadoWindow";

#define COMP_ERROR_GETLASTERROR(C, MSG_WITH_PLACEHOLDER, MSG_WITHOUT_PLACEHOLDER)                                      \
	do {                                                                                                           \
		DWORD err = GetLastError();                                                                            \
		char *buf = NULL;                                                                                      \
		if (0 != FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,        \
		                        LANG_SYSTEM_DEFAULT, (LPSTR)&buf, 256, NULL)) {                                \
			COMP_ERROR(C, MSG_WITH_PLACEHOLDER, buf);                                                      \
			LocalFree(buf);                                                                                \
		} else {                                                                                               \
			COMP_ERROR(C, MSG_WITHOUT_PLACEHOLDER);                                                        \
		}                                                                                                      \
	} while (0)
/*
 *
 * Functions.
 *
 */

static void
draw_window(HWND hWnd, struct comp_window_mswin *cwm)
{
	ValidateRect(hWnd, NULL);
}

// Forward declaration for F11 toggle support
static void set_fullscreen(HWND hWnd, bool fullscreen);

static LRESULT CALLBACK
WndProc(HWND hWnd, unsigned int message, WPARAM wParam, LPARAM lParam)
{
	struct comp_window_mswin *cwm = GetPropW(hWnd, szWindowData);

	if (!cwm) {
		// This is before we've set up our window, or for some other helper window...
		// We might want to handle messages differently in here.
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	struct comp_compositor *c = cwm->base.base.c;
	switch (message) {
	case WM_ENTERSIZEMOVE:
		InterlockedExchange(&cwm->in_size_move, TRUE);
		InvalidateRect(hWnd, NULL, FALSE);  // Kick off first WM_PAINT
		return 0;

	case WM_EXITSIZEMOVE:
		InterlockedExchange(&cwm->in_size_move, FALSE);
		if (cwm->paint_requested_event)
			SetEvent(cwm->paint_requested_event);  // Unblock compositor if waiting
		return 0;

	case WM_PAINT:
		if (cwm->owns_window && InterlockedCompareExchange(&cwm->in_size_move, 0, 0)) {
			// During drag: signal compositor to render, wait for completion.
			// The modal loop is paused while we wait, so window position is stable.
			SetEvent(cwm->paint_requested_event);
			WaitForSingleObject(cwm->paint_done_event, 100);  // 100ms timeout prevents deadlock
			InvalidateRect(hWnd, NULL, FALSE);  // Request next WM_PAINT
			return 0;  // Don't validate — keeps window "dirty" for continuous paint
		}
		draw_window(hWnd, cwm);
		break;

	case WM_QUIT:
		// COMP_INFO(c, "WM_QUIT");
		PostQuitMessage(0);
		break;
	case WM_CLOSE:
		// COMP_INFO(c, "WM_CLOSE");
		cwm->should_exit = true;
		DestroyWindow(hWnd);
		cwm->window = NULL;
		break;
	case WM_DESTROY:
		// COMP_INFO(c, "WM_DESTROY");
		// Post a quit message and return.
		PostQuitMessage(0);
		break;
	case WM_KEYDOWN:
		if (wParam == VK_F11) {
			// Toggle fullscreen state
			cwm->is_fullscreen = !cwm->is_fullscreen;
			set_fullscreen(hWnd, cwm->is_fullscreen);
			COMP_INFO(c, "F11: Toggled to %s mode", cwm->is_fullscreen ? "fullscreen" : "windowed");
		}
		break;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			uint32_t width = LOWORD(lParam);
			uint32_t height = HIWORD(lParam);
			if (width > 0 && height > 0) {
				COMP_DEBUG(c, "Window resized to %ux%u", width, height);
			}
		}
		break;
	default: return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}


static inline struct vk_bundle *
get_vk(struct comp_window_mswin *cwm)
{
	return &cwm->base.base.c->base.vk;
}

static void
comp_window_mswin_destroy(struct comp_target *ct)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;

	// Only cleanup window thread if we created the window ourselves
	if (cwm->owns_window) {
		// Stop the Windows thread first, destroy also stops the thread.
		os_thread_helper_destroy(&cwm->oth);
	}

	// Cleanup drag/resize sync events
	if (cwm->paint_requested_event)
		CloseHandle(cwm->paint_requested_event);
	if (cwm->paint_done_event)
		CloseHandle(cwm->paint_done_event);

	comp_target_swapchain_cleanup(&cwm->base);

	//! @todo

	free(ct);
}

static void
comp_window_mswin_update_window_title(struct comp_target *ct, const char *title)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	//! @todo
}

static void
comp_window_mswin_fullscreen(struct comp_window_mswin *w)
{
	//! @todo
}

static VkResult
comp_window_mswin_create_surface(struct comp_window_mswin *w, VkSurfaceKHR *out_surface)
{
	struct vk_bundle *vk = get_vk(w);
	VkResult ret;

	VkWin32SurfaceCreateInfoKHR surface_info = {
	    .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
	    .hinstance = w->instance,
	    .hwnd = w->window,
	};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ret = vk->vkCreateWin32SurfaceKHR( //
	    vk->instance,                  //
	    &surface_info,                 //
	    NULL,                          //
	    &surface);                     //
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateWin32SurfaceKHR: %s", vk_result_string(ret));
		return ret;
	}

	VK_NAME_SURFACE(vk, surface, "comp_window_mswin surface");
	*out_surface = surface;

	return VK_SUCCESS;
}

static bool
comp_window_mswin_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	VkResult ret;

	ret = comp_window_mswin_create_surface(cwm, &cwm->base.surface.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Failed to create surface '%s'!", vk_result_string(ret));
		return false;
	}

	//! @todo

	return true;
}


static void
comp_window_mswin_flush(struct comp_target *ct)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
}

static BOOL CALLBACK GetDefaultWindowStartPos_MonitorEnumProc(__in HMONITOR hMonitor, __in HDC hdcMonitor, __in LPRECT lprcMonitor, __in LPARAM dwData)
{
	MONITORINFOEX info;
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	GetMonitorInfo(hMonitor, (LPMONITORINFO)&info);
	g_monitor_info[g_monitor_info_count++] = info;
	return TRUE;
}

static bool get_leia_display_top_left_coordinate(int* x, int* y)
{
	// Get connected monitor info.
	g_monitor_info_count = 0;
	EnumDisplayMonitors(NULL, NULL, GetDefaultWindowStartPos_MonitorEnumProc, (LPARAM)NULL);

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

	if (!g_use_secondary_monitor) {
		return false;
	}

	// If we have multiple monitors, select the first non-primary one.
	for (int i = 0; i < g_monitor_info_count; i++)
	{
		if (0 == (g_monitor_info[i].dwFlags & MONITORINFOF_PRIMARY)) {
			*x = g_monitor_info[i].rcMonitor.left;
			*y = g_monitor_info[i].rcMonitor.top;
			return true;
		}
	}

	// Didn't find a non-primary, there is only one display connected.
	x = 0;
	y = 0;
	return false;
}

static void set_fullscreen(HWND hWnd, bool fullscreen)
{
	static int windowPrevX = 0;
	static int windowPrevY = 0;
	static int windowPrevWidth = 0;
	static int windowPrevHeight = 0;

	DWORD style = GetWindowLong(hWnd, GWL_STYLE);
	if (fullscreen) {
		RECT rect;
		MONITORINFO mi = { sizeof(mi) };
		GetWindowRect(hWnd, &rect);

		windowPrevX = rect.left;
		windowPrevY = rect.top;
		windowPrevWidth = rect.right - rect.left;
		windowPrevHeight = rect.bottom - rect.top;

		GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLong(hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	else
	{
		MONITORINFO mi = { sizeof(mi) };
		UINT flags = SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
		GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLong(hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPos(hWnd, HWND_NOTOPMOST, windowPrevX, windowPrevY, windowPrevWidth, windowPrevHeight, flags);
	}
}

static void
comp_window_mswin_window_loop(struct comp_window_mswin *cwm)
{
	struct comp_target *ct = &cwm->base.base;
	RECT rc = {0, 0, (LONG)(ct->width), (LONG)ct->height};

	int secondary_monitor_X = 0;
	int secondary_monitor_y = 0;
	if (get_leia_display_top_left_coordinate(&secondary_monitor_X, &secondary_monitor_y)) {
		rc.left   = secondary_monitor_X;
		rc.top    = secondary_monitor_y;
		rc.right  = secondary_monitor_X + ct->width;
		rc.bottom = secondary_monitor_y + ct->height;
	}

	COMP_INFO(ct->c, "Creating window");
	cwm->window =
	    CreateWindowExW(0, szWindowClass, L"Monado (Windowed)", WS_OVERLAPPEDWINDOW, rc.left, rc.top,
	                    rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, cwm->instance, NULL);
	if (cwm->window == NULL) {
		COMP_ERROR_GETLASTERROR(ct->c, "Failed to create window: %s", "Failed to create window");
		// parent thread will be notified (by caller) that we have exited.
		return;
	}

	// Set fullscreen if requested, and track the state for F11 toggle.
	// Use XRT_COMPOSITOR_START_WINDOWED=1 to start in windowed mode.
	cwm->is_fullscreen = !debug_get_bool_option_start_windowed();
	if (cwm->is_fullscreen) {
		set_fullscreen(cwm->window, true);
	}

	COMP_INFO(ct->c, "Setting window properties and showing window");
	SetPropW(cwm->window, szWindowData, cwm);
	SetWindowLongPtr(cwm->window, GWLP_USERDATA, (LONG_PTR)(cwm));
	ShowWindow(cwm->window, SW_SHOWDEFAULT);
	UpdateWindow(cwm->window);

	COMP_INFO(ct->c, "Unblocking parent thread");
	// Unblock the parent thread now that we're successfully running.
	{
		os_thread_helper_lock(&cwm->oth);
		cwm->thread_started = true;
		os_thread_helper_signal_locked(&cwm->oth);
		os_thread_helper_unlock(&cwm->oth);
	}
	COMP_INFO(ct->c, "Starting the Windows window message loop");

	bool done = false;
	while (os_thread_helper_is_running(&cwm->oth)) {
		// force handling messages.
		MSG msg;
		while (PeekMessageW(&msg, cwm->window, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
#ifdef ALLOW_CLOSING_WINDOW
			/// @todo We need to bubble this up to multi-compositor
			/// and the state tracker (as "instance lost")
			if (msg.message == WM_QUIT) {
				COMP_INFO(cwm->base.base.c, "Got WM_QUIT message");
				return;
			}
			if (msg.message == WM_DESTROY) {
				COMP_INFO(cwm->base.base.c, "Got WM_DESTROY message");
				return;
			}
			if (cwm->should_exit) {
				COMP_INFO(cwm->base.base.c, "Got 'should_exit' flag.");
				return;
			}
#endif
		}
	}
	if (cwm->window != NULL) {
		// Got shut down by app code, not by a window message, so we still need to clean up our window.
		if (0 == DestroyWindow(cwm->window)) {
			COMP_ERROR_GETLASTERROR(ct->c, "DestroyWindow failed: %s", "DestroyWindow failed");
		}
		cwm->window = NULL;
	}
}

static void
comp_window_mswin_mark_exited(struct comp_window_mswin *cwm)
{
	// Unblock the parent thread to advise of our exit
	os_thread_helper_lock(&cwm->oth);
	cwm->thread_exited = true;
	os_thread_helper_signal_locked(&cwm->oth);
	os_thread_helper_unlock(&cwm->oth);
}

static void
comp_window_mswin_thread(struct comp_window_mswin *cwm)
{
	struct comp_target *ct = &cwm->base.base;

	RECT rc = {0, 0, (LONG)(ct->width), (LONG)ct->height};

	WNDCLASSEXW wcex;
	U_ZERO(&wcex);
	wcex.cbSize = sizeof(WNDCLASSEXW);
	wcex.style = CS_HREDRAW | CS_VREDRAW;

	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = cwm->instance;
	wcex.lpszClassName = szWindowClass;
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
//! @todo icon
#if 0
	wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SAMPLEGUI));
	wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_SAMPLEGUI);
	wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
#endif
	COMP_INFO(ct->c, "Registering window class");
	ATOM window_class = RegisterClassExW(&wcex);
	if (!window_class) {
		COMP_ERROR_GETLASTERROR(ct->c, "Failed to register window class: %s",
		                        "Failed to register window class");
		comp_window_mswin_mark_exited(cwm);
		return;
	}

	comp_window_mswin_window_loop(cwm);

	COMP_INFO(ct->c, "Unregistering window class");
	if (0 == UnregisterClassW((LPCWSTR)window_class, NULL)) {
		COMP_ERROR_GETLASTERROR(ct->c, "Failed to unregister window class: %s",
		                        "Failed to unregister window class");
	}

	comp_window_mswin_mark_exited(cwm);
}

static void *
comp_window_mswin_thread_func(void *ptr)
{

	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ptr;
	os_thread_helper_name(&(cwm->oth), "Compositor Window Message Thread");

	comp_window_mswin_thread(cwm);
	os_thread_helper_signal_stop(&cwm->oth);
	COMP_INFO(cwm->base.base.c, "Windows window message thread now exiting.");
	return NULL;
}

static bool
comp_window_mswin_init(struct comp_target *ct)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	cwm->instance = GetModuleHandle(NULL);

	ct->width = 1280;
	ct->height = 720;

	if (os_thread_helper_start(&cwm->oth, comp_window_mswin_thread_func, cwm) != 0) {
		COMP_ERROR(ct->c, "Failed to start Windows window message thread");
		return false;
	}

	// Wait for thread to start, create window, etc.
	os_thread_helper_lock(&cwm->oth);
	while (!cwm->thread_started && !cwm->thread_exited) {
		os_thread_helper_wait_locked(&cwm->oth);
	}
	bool ret = cwm->thread_started && !cwm->thread_exited;
	os_thread_helper_unlock(&cwm->oth);
	return ret;
}

static void
comp_window_mswin_configure(struct comp_window_mswin *w, int32_t width, int32_t height)
{
	if (w->base.base.c->settings.fullscreen && !w->fullscreen_requested) {
		COMP_DEBUG(w->base.base.c, "Setting full screen");
		comp_window_mswin_fullscreen(w);
		w->fullscreen_requested = true;
	}
}

#ifdef ALLOW_CLOSING_WINDOW
/// @todo This is somehow triggering crashes in the multi-compositor, which is trying to run without things it needs,
/// even though it didn't do this when we called the parent impl instead of inlining it.
static bool
comp_window_mswin_configure_check_ready(struct comp_target *ct)
{
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	return os_thread_helper_is_running(&cwm->oth) && cwm->base.swapchain.handle != VK_NULL_HANDLE;
}
#endif

struct comp_target *
comp_window_mswin_create(struct comp_compositor *c)
{
	struct comp_window_mswin *w = U_TYPED_CALLOC(struct comp_window_mswin);
	if (os_thread_helper_init(&w->oth) != 0) {
		COMP_ERROR(c, "Failed to init Windows window message thread");
		free(w);
		return NULL;
	}

	// The display timing code hasn't been tested on Windows and may be broken.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "MS Windows";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.destroy = comp_window_mswin_destroy;
	w->base.base.flush = comp_window_mswin_flush;
	w->base.base.init_pre_vulkan = comp_window_mswin_init;
	w->base.base.init_post_vulkan = comp_window_mswin_init_swapchain;
	w->base.base.set_title = comp_window_mswin_update_window_title;
#ifdef ALLOW_CLOSING_WINDOW
	w->base.base.check_ready = comp_window_mswin_configure_check_ready;
#endif
	w->base.base.c = c;
	w->owns_window = true;  // We create and own the window

	// Create synchronization events for drag/resize rendering
	w->paint_requested_event = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset
	w->paint_done_event = CreateEvent(NULL, FALSE, FALSE, NULL);       // Auto-reset

	return &w->base.base;
}

bool
comp_window_mswin_create_from_external(struct comp_compositor *c,
                                       void *external_hwnd,
                                       struct comp_target **out_ct)
{
	if (external_hwnd == NULL) {
		U_LOG_E("External HWND is NULL");
		return false;
	}

	struct comp_window_mswin *cwm = U_TYPED_CALLOC(struct comp_window_mswin);
	if (cwm == NULL) {
		return false;
	}

	// Store the external window handle
	cwm->window = (HWND)external_hwnd;
	cwm->instance = GetModuleHandle(NULL);
	cwm->owns_window = false;  // Don't destroy on cleanup - app owns the window

	// Initialize base target (skip window thread creation since window already exists)
	// The display timing code hasn't been tested on Windows and may be broken.
	comp_target_swapchain_init_and_set_fnptrs(&cwm->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	cwm->base.base.name = "MS Windows (External)";
	cwm->base.display = VK_NULL_HANDLE;
	cwm->base.base.c = c;

	// Set up function pointers
	cwm->base.base.destroy = comp_window_mswin_destroy;
	cwm->base.base.flush = comp_window_mswin_flush;
	cwm->base.base.init_pre_vulkan = NULL;  // Skip - window already exists
	cwm->base.base.init_post_vulkan = comp_window_mswin_init_swapchain;
	cwm->base.base.set_title = comp_window_mswin_update_window_title;

	// Get window dimensions
	RECT rect;
	if (GetClientRect(cwm->window, &rect)) {
		cwm->base.base.width = rect.right - rect.left;
		cwm->base.base.height = rect.bottom - rect.top;
	} else {
		// Fallback to default size
		cwm->base.base.width = 1280;
		cwm->base.base.height = 720;
	}

	U_LOG_I("Created comp_target from external HWND %p (%ux%u)",
	        external_hwnd, cwm->base.base.width, cwm->base.base.height);

	*out_ct = &cwm->base.base;
	return true;
}


/*
 *
 * Drag/resize synchronization functions.
 *
 */

bool
comp_window_mswin_is_in_size_move(struct comp_target *ct)
{
	// Safety: check name prefix to verify this is a mswin target
	if (ct == NULL || ct->name == NULL || strncmp(ct->name, "MS Windows", 10) != 0)
		return false;
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	if (!cwm->owns_window)
		return false;  // App-owned windows don't use runtime sync
	return InterlockedCompareExchange(&cwm->in_size_move, 0, 0) != 0;
}

bool
comp_window_mswin_wait_for_paint(struct comp_target *ct)
{
	if (ct == NULL || ct->name == NULL || strncmp(ct->name, "MS Windows", 10) != 0)
		return false;
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	if (!cwm->owns_window || !cwm->paint_requested_event)
		return false;
	if (!InterlockedCompareExchange(&cwm->in_size_move, 0, 0))
		return false;  // Not in drag
	WaitForSingleObject(cwm->paint_requested_event, 50);  // 50ms timeout
	return InterlockedCompareExchange(&cwm->in_size_move, 0, 0) != 0;
}

void
comp_window_mswin_signal_paint_done(struct comp_target *ct)
{
	if (ct == NULL || ct->name == NULL || strncmp(ct->name, "MS Windows", 10) != 0)
		return;
	struct comp_window_mswin *cwm = (struct comp_window_mswin *)ct;
	if (cwm->paint_done_event)
		SetEvent(cwm->paint_done_event);
}


/*
 *
 * Factory
 *
 */

static const char *instance_extensions[] = {
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
};

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	// Return true so the service creates its own window at startup.
	// This is needed for WebXR/AppContainer apps that don't provide a window handle.
	return true;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = comp_window_mswin_create(c);
	if (ct == NULL) {
		return false;
	}

	*out_ct = ct;

	return true;
}

const struct comp_target_factory comp_target_factory_mswin = {
    .name = "Microsoft Windows(TM)",
    .identifier = "mswin",
    .requires_vulkan_for_create = false,
    .is_deferred = false,  // Create window at startup for WebXR/AppContainer support
    .required_instance_version = 0,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .optional_device_extensions = NULL,
    .optional_device_extension_count = 0,
    .detect = detect,
    .create_target = create_target,
};
