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
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_config_build.h"

// Include qwerty interface for Win32 input handling (conditional on qwerty driver being built)
#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include "xrt/xrt_device.h"

// EDID-based display identification for window positioning
#ifdef XRT_HAVE_LEIA_SR
#include "leia/leia_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM

#include <stdlib.h>
#include <string.h>

// Environment variable to start in windowed mode
DEBUG_GET_ONCE_BOOL_OPTION(start_windowed, "XRT_COMPOSITOR_START_WINDOWED", false)

// Marker for SendInput-injected mouse events — WndProc skips re-forwarding these.
// Used to break the loop: WndProc→SendInput→WM_LBUTTONDOWN→WndProc.
#define SHELL_SENDINPUT_MARKER 0xD15B1A7E

// Qwerty input is always enabled for non-session-target apps (DisplayXR-owned window)
// DEBUG_GET_ONCE_BOOL_OPTION(qwerty_enable, "QWERTY_ENABLE", false)

// Coarse "a bridge relay session exists" flag. True whenever the bridge
// exe has created its headless OpenXR session; NOT a signal that the
// compositor should divert input away from qwerty. Kept as the outer fast
// exit so there's zero overhead when no bridge process is running.
extern "C" bool g_bridge_relay_active;

// True iff a bridge relay session exists AND a page has attached to the
// bridge (i.e. it actually read session.displayXR). Gated on the
// DXR_BridgeClientActive HWND prop set by the bridge exe on the compositor
// window when it receives a 'bridge-attach' WS message from the extension.
// Legacy non-bridge WebXR pages never send that message, so the prop stays
// absent and this returns false — letting qwerty keep processing keys /
// mouse in the normal way even with the extension loaded and a bridge
// session existing.
static inline bool
bridge_page_attached(HWND hwnd)
{
	if (!g_bridge_relay_active) return false;
	if (hwnd == nullptr) return false;
	return GetPropW(hwnd, L"DXR_BridgeClientActive") != nullptr;
}

// Window class name
static WCHAR szWindowClass[] = L"DisplayXRD3D11";
static WCHAR szWindowData[] = L"DisplayXRD3D11Window";

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

	//! True if window should stay hidden (for SR weaver HWND in shared-texture mode)
	bool hidden;

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

	//! Target HWND for input forwarding in shell mode (NULL = disabled).
	//! When set, non-shell keyboard and mouse input is forwarded to this HWND.
	volatile HWND input_forward_hwnd;

	//! True when the forward target is a captured 2D window.
	volatile LONG input_forward_is_capture;

	//! True while shell_mode is active on the service side. Gates ESC-closes-window
	//! so an empty shell (no focused app → input_forward_hwnd == NULL) doesn't
	//! PostMessage(WM_CLOSE) and take the service down with it.
	volatile LONG shell_mode_active;

	//! True while any shell window is maximized (fullscreen). Gates ESC suppression
	//! so ESC only restores fullscreen and doesn't reach apps when not maximized.
	volatile LONG any_window_maximized;

	//! Focused window rect in shell-window client pixels (for mouse coord remapping).
	//! When forwarding mouse events, shell coords are remapped to app-local coords.
	volatile LONG input_forward_rect_x;
	volatile LONG input_forward_rect_y;
	volatile LONG input_forward_rect_w;
	volatile LONG input_forward_rect_h;

	//! Desired cursor ID (compositor thread writes, window thread reads in WM_SETCURSOR).
	//! 0=arrow, 1=sizewe, 2=sizens, 3=sizenwse, 4=sizenesw, 5=sizeall
	volatile LONG desired_cursor;

	//! Accumulated scroll wheel delta for shell window resize (positive = enlarge).
	//! Written by WndProc, read+reset by render loop.
	volatile LONG scroll_accum;

	//! When true, mouse input forwarding is suppressed (shell drag/resize active).
	//! Set by compositor thread, read by WndProc thread.
	volatile LONG input_suppress;

	//! Phase 5.12: grace period for input_suppress after the launcher closes.
	//! Holds a GetTickCount() value; while GetTickCount() < input_suppress_until_tick
	//! the WndProc treats input as suppressed even when input_suppress=0. Prevents
	//! the Esc-that-closes-the-launcher from leaking into the focused app on the
	//! next message-queue drain (render thread vs window thread race).
	volatile LONG input_suppress_until_tick;

	//! True when a mouse button press originated inside the app content rect.
	//! Used to prevent title bar clicks from being forwarded as app drags.
	//! Set on button-down inside rect, cleared on button-up.
	bool mouse_press_in_content;

	//! Shell display processor for ESC/close 2D mode switch (opaque, can be NULL).
	volatile void *shell_dp;

	//! Ring buffer for capture client input events (WndProc writes, render thread reads).
	//! Lock-free SPSC: WndProc is the single producer, render loop is the single consumer.
	struct shell_input_event input_ring[SHELL_INPUT_RING_SIZE];
	volatile LONG input_ring_write; //!< Next write index (WndProc thread)
	volatile LONG input_ring_read;  //!< Next read index (compositor thread)

	//! Target HWND for SetForegroundWindow request (compositor writes, window thread reads).
	//! NULL means no pending request. Window thread clears after calling SetForegroundWindow.
	volatile HWND pending_foreground_hwnd;

	//! Signaled by window thread after SetForegroundWindow completes.
	volatile LONG foreground_done;
};

// Forward declarations
static void set_fullscreen(HWND hWnd, bool fullscreen);

// Custom message IDs (posted to window thread from compositor thread)
#define WM_SHELL_SET_FOREGROUND (WM_USER + 100)
#define WM_SHELL_LAUNCH_APP    (WM_USER + 101)

#include <commdlg.h> // GetOpenFileNameA

/*!
 * Push an input event into the ring buffer (WndProc thread only).
 * Drops the event if the buffer is full (bounded loss, not a hang).
 */
static void
input_ring_push(struct comp_d3d11_window *w,
                uint32_t message,
                uint64_t wParam,
                int64_t lParam,
                int32_t mapped_x,
                int32_t mapped_y)
{
	LONG wr = InterlockedCompareExchange(&w->input_ring_write, 0, 0);
	LONG rd = InterlockedCompareExchange(&w->input_ring_read, 0, 0);
	LONG next = (wr + 1) % SHELL_INPUT_RING_SIZE;
	if (next == rd) {
		// Buffer full — drop event
		return;
	}
	w->input_ring[wr].message = message;
	w->input_ring[wr].wParam = wParam;
	w->input_ring[wr].lParam = lParam;
	w->input_ring[wr].mapped_x = mapped_x;
	w->input_ring[wr].mapped_y = mapped_y;
	MemoryBarrier();
	InterlockedExchange(&w->input_ring_write, next);
}

/*!
 * Check if a virtual key code is reserved for shell controls.
 * These keys are NOT forwarded to the focused app in shell mode.
 */
static bool
is_shell_reserved_key(WPARAM vk)
{
	// Only true shell-management keys are reserved.
	// V, P, 0-9 are forwarded to the app (it may use them for its own purposes).
	// The qwerty handler processes them server-side regardless; if the app also
	// tries to change rendering mode via xrRequestDisplayRenderingModeEXT,
	// that call is blocked in shell/IPC mode.
	switch (vk) {
	case VK_TAB:    // Cycle focus
	case VK_DELETE: // Close focused app
		return true;
	default:
		return false;
	}
}

/*!
 * Resolve the top-left pixel coordinate of the 3D display on this machine.
 *
 * Does a fresh EDID probe every call (no stale cache) so monitor rearrangements
 * take effect without restarting the service. Falls back to primary (0, 0) when
 * no 3D display is identified — we do NOT guess "first non-primary monitor",
 * because on a setup where the 3D display IS primary, that guess would land
 * the compositor on a 2D monitor.
 */
static bool
get_3d_display_top_left(int *x, int *y)
{
#ifdef XRT_HAVE_LEIA_SR
	int32_t left = 0;
	int32_t top = 0;
	if (leia_edid_find_3d_display_rect(&left, &top, NULL, NULL)) {
		*x = (int)left;
		*y = (int)top;
		U_LOG_W("3D display resolved via EDID to (%d, %d)", *x, *y);
		return true;
	}
	U_LOG_W("3D display EDID probe found no known panel — defaulting to primary monitor");
#endif

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

// Forward declaration — defined later in this file alongside the suppress
// setters so the logic lives in one place. Used by wnd_proc to gate key
// and mouse forwarding.
static bool input_is_suppressed(struct comp_d3d11_window *w);

// Phase 5.13: private message ID for "show launcher context menu". Sent by
// the render thread via comp_d3d11_window_show_launcher_context_menu so the
// window thread actually runs TrackPopupMenu — that API only works on the
// thread that owns the target window. Defined up here so both wnd_proc and
// the exported helper can reference it.
#define WM_LAUNCHER_CTX_MENU (WM_USER + 110)

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
	case WM_SETCURSOR:
		// The compositor thread sets desired_cursor via InterlockedExchange.
		// We read it here on the window thread and apply the correct cursor.
		if (LOWORD(lParam) == HTCLIENT) {
			LONG cid = InterlockedCompareExchange(&w->desired_cursor, 0, 0);
			HCURSOR cur;
			switch (cid) {
			case 1:  cur = LoadCursor(NULL, IDC_SIZEWE); break;
			case 2:  cur = LoadCursor(NULL, IDC_SIZENS); break;
			case 3:  cur = LoadCursor(NULL, IDC_SIZENWSE); break;
			case 4:  cur = LoadCursor(NULL, IDC_SIZENESW); break;
			case 5:  cur = LoadCursor(NULL, IDC_SIZEALL); break;
			default: cur = LoadCursor(NULL, IDC_ARROW); break;
			}
			SetCursor(cur);
			return TRUE;
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);

	case WM_ENTERSIZEMOVE:
		InterlockedExchange(&w->in_size_move, TRUE);
		// Publish to HWND so cross-process consumers (WebXR bridge mouse
		// hook) can suppress mouse forwarding during the modal drag loop.
		SetPropW(hWnd, L"DXR_InSizeMove", (HANDLE)(uintptr_t)1);
		InvalidateRect(hWnd, NULL, FALSE); // Kick off first WM_PAINT
		return 0;

	case WM_EXITSIZEMOVE:
		InterlockedExchange(&w->in_size_move, FALSE);
		RemovePropW(hWnd, L"DXR_InSizeMove");
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
		// Switch shell DP to 2D mode (lens off) before closing.
		// This runs on the window thread and works even with no active clients.
		{
			void *dp = (void *)InterlockedCompareExchangePointer(
			    (volatile PVOID *)&w->shell_dp, NULL, NULL);
			if (dp != NULL) {
				struct xrt_display_processor_d3d11 *xdp =
				    (struct xrt_display_processor_d3d11 *)dp;
				xrt_display_processor_d3d11_request_display_mode(xdp, false);
				U_LOG_W("D3D11 window: switched shell DP to 2D on close");
			}
		}
		InterlockedExchange(&w->should_exit, TRUE);
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_LAUNCHER_CTX_MENU: {
		// Phase 5.13: synchronously show the launcher tile context menu
		// on THIS thread (the window thread). TrackPopupMenu only works
		// from the thread that owns the target window — calling it from
		// the render thread silently returns 0. Caller (render thread)
		// uses SendMessage so we block until the user picks / cancels.
		HMENU menu = CreatePopupMenu();
		if (menu == NULL) {
			return 0;
		}
		AppendMenuW(menu, MF_STRING, 1, L"Launch");
		AppendMenuW(menu, MF_STRING, 2, L"Remove from launcher");
		AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
		AppendMenuW(menu, MF_STRING, 3, L"Cancel");

		POINT pt;
		GetCursorPos(&pt);
		SetForegroundWindow(hWnd);
		UINT cmd = TrackPopupMenu(menu,
		                          TPM_RETURNCMD | TPM_NONOTIFY,
		                          pt.x, pt.y, 0, hWnd, NULL);
		DestroyMenu(menu);
		PostMessageW(hWnd, WM_NULL, 0, 0);
		// Map to public result codes (1=launch, 2=remove, 0=cancel).
		if (cmd == 1) return LAUNCHER_CTX_MENU_RESULT_LAUNCH;
		if (cmd == 2) return LAUNCHER_CTX_MENU_RESULT_REMOVE;
		return 0;
	}

	case WM_KEYDOWN:
		// F11: toggle fullscreen for non-shell (single app) windows.
		// In shell mode, F11 is handled in the multi-compositor render loop instead.
		if (wParam == VK_F11) {
			HWND fwd_check = (HWND)InterlockedCompareExchangePointer(
			    (volatile PVOID *)&w->input_forward_hwnd, NULL, NULL);
			if (fwd_check == NULL) {
				// Non-shell mode: toggle fullscreen directly
				LONG fs = InterlockedCompareExchange(&w->is_fullscreen, 0, 0);
				fs = !fs;
				InterlockedExchange(&w->is_fullscreen, fs);
				set_fullscreen(hWnd, fs != 0);
				U_LOG_W("D3D11 window: F11 toggled to %s mode", fs ? "fullscreen" : "windowed");
				return 0;
			}
			// Shell mode: fall through to forwarding (handled server-side)
		}
		// FALLTHROUGH to WM_KEYUP/SYSKEYDOWN/SYSKEYUP/CHAR
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
	case WM_CHAR:
	case WM_SYSCHAR: {
		// Phase 5.12: when the shell is suppressing input (launcher visible,
		// resize drag, or within the post-launcher grace period) we eat the
		// key entirely — don't forward, don't run qwerty. Otherwise the
		// launcher's Esc / arrows / Enter leak through and close the app.
		if (input_is_suppressed(w)) {
			return 0;
		}

		// Shell input forwarding: all keys go to BOTH qwerty and the app.
		// Qwerty processes first (mode toggles, camera controls), then
		// the key is forwarded to the focused app's HWND.
		HWND fwd = (HWND)InterlockedCompareExchangePointer((volatile PVOID *)&w->input_forward_hwnd, NULL, NULL);
		LONG is_capture = InterlockedCompareExchange(&w->input_forward_is_capture, 0, 0);
		if (fwd != NULL) {
			// WM_CHAR/WM_SYSCHAR: forward to the target app.
			if (message == WM_CHAR || message == WM_SYSCHAR) {
				if (is_capture) {
					// Capture client: buffer for SendInput dispatch
					input_ring_push(w, message, (uint64_t)wParam, (int64_t)lParam, -1, -1);
				} else {
					PostMessage(fwd, message, wParam, lParam);
				}
				return 0;
			}

			// Process qwerty first
#ifdef XRT_BUILD_DRIVER_QWERTY
			if (w->qwerty_enabled && w->xsysd != NULL && !bridge_page_attached(hWnd)) {
				bool handled = false;
				qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
				                     message, wParam, lParam, &handled);
			}
#endif
			if (is_shell_reserved_key(wParam)) {
				// Shell-only keys (TAB, DELETE) → don't forward to app
				return 0;
			}
			// ESC: only suppress when a window is maximized (fullscreen restore).
			// When not maximized, let ESC through so the app can handle it.
			if (wParam == VK_ESCAPE &&
			    InterlockedCompareExchange(&w->any_window_maximized, 0, 0)) {
				return 0;
			}
			// Ctrl+1-4: layout presets (handled server-side) → don't forward
			if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
			    wParam >= '1' && wParam <= '4') {
				return 0;
			}
			if (is_capture) {
				// Capture client: buffer for SendInput dispatch
				input_ring_push(w, message, (uint64_t)wParam, (int64_t)lParam, -1, -1);
			} else {
				// IPC app: forward via PostMessage (works fine)
				PostMessage(fwd, message, wParam, lParam);
			}
			return 0;
		}

		// Normal mode (no forwarding): pass all keys to qwerty.
		// ESC → close window: Phase 4C made ESC a no-op in qwerty to
		// prevent it from killing the shell, but that also broke non-shell
		// ESC (WebXR, standalone IPC clients). Handle ESC here before
		// qwerty sees it. Shell mode never reaches this block because
		// input_forward_hwnd != NULL takes the forwarding path above.
		if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
			// Shell mode with no focused app: input_forward_hwnd is NULL so we
			// fall into this block, but closing the window kills the service.
			// Swallow the key instead; user can press Ctrl+Space to dismiss.
			if (InterlockedCompareExchange(&w->shell_mode_active, 0, 0)) {
				return 0;
			}
			U_LOG_W("D3D11 window: ESC pressed — closing (non-shell mode)");
			PostMessageW(hWnd, WM_CLOSE, 0, 0);
			return 0;
		}
#ifdef XRT_BUILD_DRIVER_QWERTY
		// When a bridge-attached page owns this window, don't forward keys
		// to qwerty — mode changes go through the app-initiated HWND
		// property relay. Consume all keys (return 0) so DefWindowProc
		// doesn't process them. The bridge's LL hook captures them for
		// the sample. Legacy WebXR pages (no session.displayXR use) fall
		// through to the normal qwerty path below, even with the bridge
		// exe running in the background.
		if (bridge_page_attached(hWnd)) {
			return 0;
		}
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
	} break;

	// Focus change: let qwerty clear any latched modifier/button state.
	// Alt+Tab can swallow the matching KEYUP once focus leaves, leaving
	// e.g. the right controller stuck-active on re-entry. Fall through
	// to DefWindowProc so Windows still handles default focus behavior
	// (repainting, activation visuals).
	case WM_KILLFOCUS:
	case WM_SETFOCUS:
	case WM_ACTIVATE:
#ifdef XRT_BUILD_DRIVER_QWERTY
		if (w->qwerty_enabled && w->xsysd != NULL) {
			qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
			                     message, wParam, lParam, NULL);
		}
#endif
		return DefWindowProcW(hWnd, message, wParam, lParam);

	// Mouse input: forward to app in shell mode, or to qwerty in normal mode
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL: {
		// Skip forwarding when shell drag/resize is active or within the
		// launcher grace period.
		if (input_is_suppressed(w)) {
			break; // fall through to qwerty/default handling
		}

		// Skip re-forwarding of SendInput-injected mouse events (prevents
		// WndProc→SendInput→WM_LBUTTONDOWN→WndProc infinite loop).
		if (GetMessageExtraInfo() == (LPARAM)SHELL_SENDINPUT_MARKER) {
			return DefWindowProcW(hWnd, message, wParam, lParam);
		}

		HWND fwd = (HWND)InterlockedCompareExchangePointer((volatile PVOID *)&w->input_forward_hwnd, NULL, NULL);
		LONG is_capture = InterlockedCompareExchange(&w->input_forward_is_capture, 0, 0);
		if (fwd != NULL) {
			// Shell mode wheel routing:
			//   cursor over app content + no modifier → forward to app (e.g. 3DGS zoom)
			//   cursor outside app content            → shell resize (no modifier needed)
			//   Ctrl + scroll                         → shell resize (force, even over app)
			//   Shift + scroll                        → window Z-depth
			// Capture clients don't take wheel input, so always consume their delta.
			if (message == WM_MOUSEWHEEL) {
				bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

				// WM_MOUSEWHEEL puts SCREEN coords in lParam (unlike other
				// mouse messages). Convert to shell-client coords for the
				// in-rect test.
				POINT pt;
				pt.x = GET_X_LPARAM(lParam);
				pt.y = GET_Y_LPARAM(lParam);
				ScreenToClient(hWnd, &pt);

				LONG rx = InterlockedCompareExchange(&w->input_forward_rect_x, 0, 0);
				LONG ry = InterlockedCompareExchange(&w->input_forward_rect_y, 0, 0);
				LONG rw = InterlockedCompareExchange(&w->input_forward_rect_w, 0, 0);
				LONG rh = InterlockedCompareExchange(&w->input_forward_rect_h, 0, 0);
				bool in_rect = (rw > 0 && rh > 0 &&
				                pt.x >= rx && pt.x < rx + rw &&
				                pt.y >= ry && pt.y < ry + rh);

				if (in_rect && !is_capture && !ctrl && !shift) {
					PostMessage(fwd, message, wParam, lParam);
				} else {
					short delta = GET_WHEEL_DELTA_WPARAM(wParam);
					InterlockedExchangeAdd(&w->scroll_accum, (LONG)delta);
				}
				return 0;
			}

			// Capture clients are preview-only — don't forward mouse events.
			// PostMessage(WM_MOUSEMOVE) to the capture HWND causes the OS
			// cursor to teleport. Input forwarding tracked in #124.
			if (is_capture) {
				return 0;
			}

			// Is this a mouse button event (not movement)?
			bool is_button = (message != WM_MOUSEMOVE);
			bool is_button_down = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
			                       message == WM_MBUTTONDOWN);
			bool is_button_up = (message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
			                     message == WM_MBUTTONUP);

			// Remap shell-window coords to app-window coords
			LONG rx = InterlockedCompareExchange(&w->input_forward_rect_x, 0, 0);
			LONG ry = InterlockedCompareExchange(&w->input_forward_rect_y, 0, 0);
			LONG rw = InterlockedCompareExchange(&w->input_forward_rect_w, 0, 0);
			LONG rh = InterlockedCompareExchange(&w->input_forward_rect_h, 0, 0);

			if (rw > 0 && rh > 0) {
				// Extract shell-window client coords
				int shell_x = GET_X_LPARAM(lParam);
				int shell_y = GET_Y_LPARAM(lParam);

				bool in_rect = (shell_x >= rx && shell_x < rx + rw &&
				                shell_y >= ry && shell_y < ry + rh);

				// Track whether the press originated inside the content rect.
				// Only forward drag (button-held movement) if the press started
				// inside content — prevents title bar clicks from being forwarded.
				if (is_button_down) {
					w->mouse_press_in_content = in_rect;
				}
				if (is_button_up) {
					w->mouse_press_in_content = false;
				}

				// Forward if inside rect, or if dragging from an in-content press.
				bool buttons_held = (wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
				bool dragging = buttons_held && w->mouse_press_in_content;
				if (in_rect || dragging) {
					// Remap to app-window client coords.
					// Scale if target HWND is a different size than the
					// virtual rect (e.g., captured 2D windows).
					RECT target_cr;
					GetClientRect(fwd, &target_cr);
					int target_w = target_cr.right - target_cr.left;
					int target_h = target_cr.bottom - target_cr.top;

					int rel_x = shell_x - rx;
					int rel_y = shell_y - ry;
					int app_x, app_y;
					if (target_w > 0 && target_h > 0 &&
					    (target_w != rw || target_h != rh)) {
						// Scale: virtual rect → actual HWND client area
						app_x = (int)((float)rel_x * (float)target_w / (float)rw);
						app_y = (int)((float)rel_y * (float)target_h / (float)rh);
					} else {
						// Same size — offset only (IPC apps)
						app_x = rel_x;
						app_y = rel_y;
					}
					// PostMessage: works for classic Win32 apps (test apps)
					PostMessage(fwd, message, wParam, MAKELPARAM(app_x, app_y));

					// For mouse buttons: also inject via SendInput so apps
					// using Raw Input (RIDEV_INPUTSINK) see the event in
					// their WM_INPUT stream. This is needed for Unity's New
					// Input System which reads buttons from Raw Input, not
					// from posted WM_LBUTTONDOWN messages.
					// Mouse movement is NOT injected — RIDEV_INPUTSINK
					// already delivers hardware mouse moves to all sinks.
					if (is_button) {
						DWORD flags = 0;
						switch (message) {
						case WM_LBUTTONDOWN: flags = MOUSEEVENTF_LEFTDOWN; break;
						case WM_LBUTTONUP:   flags = MOUSEEVENTF_LEFTUP; break;
						case WM_RBUTTONDOWN: flags = MOUSEEVENTF_RIGHTDOWN; break;
						case WM_RBUTTONUP:   flags = MOUSEEVENTF_RIGHTUP; break;
						case WM_MBUTTONDOWN: flags = MOUSEEVENTF_MIDDLEDOWN; break;
						case WM_MBUTTONUP:   flags = MOUSEEVENTF_MIDDLEUP; break;
						default: break;
						}
						if (flags != 0) {
							INPUT inp = {};
							inp.type = INPUT_MOUSE;
							inp.mi.dwFlags = flags;
							inp.mi.dwExtraInfo = SHELL_SENDINPUT_MARKER;
							SendInput(1, &inp, sizeof(INPUT));
						}
					}
				}
			} else {
				// No rect set — fallback to 1:1 forwarding
				PostMessage(fwd, message, wParam, lParam);
			}
			return 0;
		}
		// Normal mode: pass to qwerty driver
#ifdef XRT_BUILD_DRIVER_QWERTY
		if (w->qwerty_enabled && w->xsysd != NULL && !bridge_page_attached(hWnd)) {
			bool handled = false;
			qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
			                     message, wParam, lParam, &handled);
			// Don't consume mouse events - let them propagate for other uses
		}
#endif
	} break;

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

	case WM_SHELL_SET_FOREGROUND: {
		// Cross-thread foreground request from compositor.
		// wParam = target HWND. NULL means restore shell window.
		HWND target = (HWND)wParam;
		if (target != NULL) {
			SetForegroundWindow(target);
		} else {
			SetForegroundWindow(hWnd);
		}
		InterlockedExchange(&w->foreground_done, 1);
		return 0;
	}

	case WM_SHELL_LAUNCH_APP: {
		// Open file dialog and launch selected app.
		// Runs on window thread (modal dialog, blocks message pump).
		// Compositor thread continues rendering independently.
		char path[MAX_PATH] = {0};
		OPENFILENAMEA ofn = {0};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = hWnd;
		ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
		ofn.lpstrFile = path;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = "Launch App in Shell";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		if (GetOpenFileNameA(&ofn)) {
			// Set shell session env var and ensure XR_RUNTIME_JSON points to
			// the dev build runtime (matching the running service).
			SetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", "1");
			if (getenv("XR_RUNTIME_JSON") == NULL) {
				// Use dev build manifest (has absolute path to DLL).
				// Service is at _package/bin/, dev manifest is at build/Release/.
				char module_dir[MAX_PATH] = {0};
				GetModuleFileNameA(NULL, module_dir, MAX_PATH);
				char *sep = strrchr(module_dir, '\\');
				if (sep) *sep = '\0';
				char json_path[MAX_PATH];
				// _package/bin/../../build/Release/openxr_displayxr-dev.json
				snprintf(json_path, sizeof(json_path),
				         "%s\\..\\..\\build\\Release\\openxr_displayxr-dev.json", module_dir);
				char abs_json[MAX_PATH];
				if (_fullpath(abs_json, json_path, MAX_PATH) &&
				    GetFileAttributesA(abs_json) != INVALID_FILE_ATTRIBUTES) {
					SetEnvironmentVariableA("XR_RUNTIME_JSON", abs_json);
					U_LOG_W("D3D11 window: set XR_RUNTIME_JSON=%s", abs_json);
				}
			}

			// Extract app directory for working directory (so it finds co-located DLLs)
			char app_dir[MAX_PATH] = {0};
			strncpy(app_dir, path, MAX_PATH - 1);
			char *last_slash = strrchr(app_dir, '\\');
			if (!last_slash) last_slash = strrchr(app_dir, '/');
			if (last_slash) *last_slash = '\0';

			char cmd[MAX_PATH + 16];
			snprintf(cmd, sizeof(cmd), "\"%s\"", path);
			STARTUPINFOA si = {0};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi = {0};
			if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
			                   CREATE_NEW_CONSOLE, NULL, app_dir, &si, &pi)) {
				U_LOG_W("D3D11 window: launched app '%s' (PID %lu)", path, pi.dwProcessId);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			} else {
				U_LOG_E("D3D11 window: failed to launch '%s' (error %lu)", path, GetLastError());
			}
		}
		return 0;
	}

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
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
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
	if (get_3d_display_top_left(&monitor_x, &monitor_y)) {
		rc.left = monitor_x;
		rc.top = monitor_y;
		rc.right = monitor_x + (LONG)w->requested_width;
		rc.bottom = monitor_y + (LONG)w->requested_height;
	}

	// Hidden windows use WS_POPUP (borderless) so client rect = window rect = exact texture size.
	// Visible windows use WS_OVERLAPPEDWINDOW for normal window chrome.
	DWORD style = w->hidden ? WS_POPUP : WS_OVERLAPPEDWINDOW;

	U_LOG_W("D3D11 window thread: Creating %s window at (%d, %d) size %ux%u",
	        w->hidden ? "hidden" : "visible",
	        (int)rc.left, (int)rc.top, w->requested_width, w->requested_height);

	HWND hwnd = CreateWindowExW(0, szWindowClass, L"DisplayXR \u2014 D3D11 Native Compositor", style, rc.left, rc.top,
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

	// Associate window data before showing
	SetPropW(hwnd, szWindowData, w);
	SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)w);

	// Verify the window actually landed where we asked. Win32 occasionally
	// nudges overlapped windows at negative coordinates to the primary monitor.
	// If that happened, force-move it back onto the 3D display.
	{
		RECT actual = {0};
		if (GetWindowRect(hwnd, &actual) && (actual.left != rc.left || actual.top != rc.top)) {
			U_LOG_W("D3D11 window: CreateWindow landed at (%d, %d), expected (%d, %d) — correcting",
			        (int)actual.left, (int)actual.top, (int)rc.left, (int)rc.top);
			SetWindowPos(hwnd, NULL, rc.left, rc.top,
			             rc.right - rc.left, rc.bottom - rc.top,
			             SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	if (!w->hidden) {
		// Check if we should start fullscreen
		bool start_fullscreen = !debug_get_bool_option_start_windowed();
		if (start_fullscreen) {
			InterlockedExchange(&w->is_fullscreen, TRUE);
			set_fullscreen(hwnd, true);
		}

		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
	}

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
	w->qwerty_enabled = true;  // Always enabled for DisplayXR-owned windows

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

extern "C" void
comp_d3d11_window_set_input_forward(struct comp_d3d11_window *window,
                                     void *hwnd,
                                     int32_t rect_x,
                                     int32_t rect_y,
                                     int32_t rect_w,
                                     int32_t rect_h,
                                     bool is_capture)
{
	if (window == NULL) {
		return;
	}

	// Write rect first (before HWND) so the WndProc sees consistent values
	InterlockedExchange(&window->input_forward_rect_x, (LONG)rect_x);
	InterlockedExchange(&window->input_forward_rect_y, (LONG)rect_y);
	InterlockedExchange(&window->input_forward_rect_w, (LONG)rect_w);
	InterlockedExchange(&window->input_forward_rect_h, (LONG)rect_h);
	InterlockedExchange(&window->input_forward_is_capture, is_capture ? 1 : 0);
	InterlockedExchangePointer((volatile PVOID *)&window->input_forward_hwnd, (PVOID)hwnd);

	if (hwnd != NULL) {
		U_LOG_W("D3D11 window: input forwarding enabled → HWND=%p rect=(%d,%d,%d,%d) capture=%d",
		        hwnd, rect_x, rect_y, rect_w, rect_h, is_capture);
	} else {
		U_LOG_W("D3D11 window: input forwarding disabled");
	}
}

void
comp_d3d11_window_set_shell_mode_active(struct comp_d3d11_window *window, bool active)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchange(&window->shell_mode_active, active ? 1 : 0);
}

void
comp_d3d11_window_set_any_maximized(struct comp_d3d11_window *window, bool maximized)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchange(&window->any_window_maximized, maximized ? 1 : 0);
}

// Returns true if input forwarding should currently be suppressed, considering
// both the immediate flag and the tick-count grace period (Phase 5.12).
static bool
input_is_suppressed(struct comp_d3d11_window *w)
{
	if (InterlockedCompareExchange(&w->input_suppress, 0, 0)) return true;
	LONG until = InterlockedCompareExchange(&w->input_suppress_until_tick, 0, 0);
	if (until == 0) return false;
	// GetTickCount() wraps every ~49 days; use signed diff for wrap-safety.
	LONG now = (LONG)GetTickCount();
	return (LONG)(until - now) > 0;
}

extern "C" void
comp_d3d11_window_set_input_suppress_grace_ms(struct comp_d3d11_window *window, uint32_t ms)
{
	if (window == NULL) return;
	LONG until = (LONG)(GetTickCount() + ms);
	InterlockedExchange(&window->input_suppress_until_tick, until);
}

extern "C" uint32_t
comp_d3d11_window_show_launcher_context_menu(struct comp_d3d11_window *window)
{
	if (window == NULL || window->hwnd == NULL) return 0;
	LRESULT r = SendMessageW(window->hwnd, WM_LAUNCHER_CTX_MENU, 0, 0);
	return (uint32_t)r;
}

extern "C" void
comp_d3d11_window_set_input_suppress(struct comp_d3d11_window *window, bool suppress)
{
	if (window == NULL) return;
	InterlockedExchange(&window->input_suppress, suppress ? 1 : 0);

	// Publish the suppress state to the compositor HWND so cross-process
	// consumers can mirror it. The WebXR bridge's low-level mouse hook
	// reads DXR_InSizeMove to skip forwarding — without this extension,
	// title-bar drags, edge resizes, and launcher-open suppress input for
	// in-process handle apps (via input_suppress) but still forward mouse
	// events to bridge-aware pages (Chrome interprets them as in-scene
	// drag/rotate), so moving a WebXR window drags its content instead
	// of the window itself. The native WM_ENTERSIZEMOVE path also sets
	// this prop directly; reusing the prop keeps one gate on the bridge
	// side. Semantically DXR_InSizeMove now means "compositor is in a
	// modal-like interaction (native OR shell-virtual)" — document in
	// webxr-bridge/DEVELOPER.md alongside the Phase-5 description.
	if (window->hwnd != NULL) {
		if (suppress) {
			SetPropW(window->hwnd, L"DXR_InSizeMove", (HANDLE)(uintptr_t)1);
		} else {
			RemovePropW(window->hwnd, L"DXR_InSizeMove");
		}
	}

	// When suppressing, cancel any in-progress app interaction by sending
	// a synthetic button-up. This prevents stuck button state in the app
	// (e.g., rotation continuing after resize because button-down was
	// forwarded before suppress activated).
	if (suppress) {
		window->mouse_press_in_content = false;
		HWND fwd = (HWND)InterlockedCompareExchangePointer(
		    (volatile PVOID *)&window->input_forward_hwnd, NULL, NULL);
		if (fwd != NULL) {
			PostMessage(fwd, WM_LBUTTONUP, 0, 0);
		}
	}
}

extern "C" int32_t
comp_d3d11_window_consume_scroll(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return 0;
	}
	return (int32_t)InterlockedExchange(&window->scroll_accum, 0);
}

extern "C" void
comp_d3d11_window_set_cursor(struct comp_d3d11_window *window, int cursor_id)
{
	if (window == NULL) return;
	InterlockedExchange(&window->desired_cursor, (LONG)cursor_id);
}

extern "C" void
comp_d3d11_window_set_shell_dp(struct comp_d3d11_window *window, void *dp)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchangePointer((volatile PVOID *)&window->shell_dp, (PVOID)dp);
}

extern "C" uint32_t
comp_d3d11_window_consume_input_events(struct comp_d3d11_window *window,
                                       struct shell_input_event *out_events,
                                       uint32_t max_events)
{
	if (window == NULL || out_events == NULL || max_events == 0) {
		return 0;
	}

	uint32_t count = 0;
	while (count < max_events) {
		LONG rd = InterlockedCompareExchange(&window->input_ring_read, 0, 0);
		LONG wr = InterlockedCompareExchange(&window->input_ring_write, 0, 0);
		if (rd == wr) {
			break; // Empty
		}
		MemoryBarrier();
		out_events[count] = window->input_ring[rd];
		InterlockedExchange(&window->input_ring_read, (rd + 1) % SHELL_INPUT_RING_SIZE);
		count++;
	}
	return count;
}

extern "C" void
comp_d3d11_window_request_foreground(struct comp_d3d11_window *window,
                                     void *target_hwnd)
{
	if (window == NULL || window->hwnd == NULL) {
		return;
	}

	InterlockedExchange(&window->foreground_done, 0);
	// Post to window thread — it owns the current foreground window
	PostMessageW(window->hwnd, WM_SHELL_SET_FOREGROUND, (WPARAM)target_hwnd, 0);

	// Wait for completion (with timeout to avoid deadlock)
	for (int i = 0; i < 100; i++) {
		if (InterlockedCompareExchange(&window->foreground_done, 0, 0)) {
			break;
		}
		Sleep(1);
	}
}

extern "C" void
comp_d3d11_window_request_app_launch(struct comp_d3d11_window *window)
{
	if (window == NULL || window->hwnd == NULL) {
		return;
	}
	PostMessageW(window->hwnd, WM_SHELL_LAUNCH_APP, 0, 0);
}
