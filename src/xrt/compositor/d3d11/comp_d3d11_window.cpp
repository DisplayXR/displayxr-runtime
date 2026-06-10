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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM

#include <stdlib.h>
#include <string.h>

// Environment variable to start in windowed mode
DEBUG_GET_ONCE_BOOL_OPTION(start_windowed, "XRT_COMPOSITOR_START_WINDOWED", false)

// Marker for SendInput-injected mouse events — WndProc skips re-forwarding these.
// Used to break the loop: WndProc→SendInput→WM_LBUTTONDOWN→WndProc.
#define WORKSPACE_SENDINPUT_MARKER 0xD15B1A7E

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

	//! Display top-left in OS screen coords (from xsysc->info, plug-in iface).
	//! (0, 0) = primary monitor (sim_display default or unknown panel).
	int32_t display_screen_left;
	int32_t display_screen_top;

	//! Current width (window thread writes, compositor thread reads)
	volatile LONG current_width;

	//! Current height (window thread writes, compositor thread reads)
	volatile LONG current_height;

	//! Fullscreen state as LONG for Interlocked* (window thread writes, compositor reads)
	volatile LONG is_fullscreen;

	//! True if user closed the window (window thread writes, compositor reads)
	volatile LONG should_exit;

	//! Two-party cleanup vote. The window thread (on exit) and
	//! comp_d3d11_window_destroy each vote exactly once via window_release();
	//! the second voter frees the struct. This guarantees w outlives both users
	//! with no use-after-free and no leak, regardless of teardown timing.
	volatile LONG cleanup_votes;

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

	//! Target HWND for input forwarding in workspace mode (NULL = disabled).
	//! When set, non-workspace keyboard and mouse input is forwarded to this HWND.
	volatile HWND input_forward_hwnd;

	//! True when the forward target is a captured 2D window.
	volatile LONG input_forward_is_capture;

	//! True while workspace_mode is active on the service side. Gates ESC-closes-window
	//! so an empty workspace (no focused app → input_forward_hwnd == NULL) doesn't
	//! PostMessage(WM_CLOSE) and take the service down with it.
	volatile LONG workspace_mode_active;

	//! Focused window rect in workspace-window client pixels (for mouse coord remapping).
	//! When forwarding mouse events, workspace coords are remapped to app-local coords.
	volatile LONG input_forward_rect_x;
	volatile LONG input_forward_rect_y;
	volatile LONG input_forward_rect_w;
	volatile LONG input_forward_rect_h;

	//! Modal input grab (XR_EXT_spatial_workspace spec_version 18): when set, the
	//! WndProc stops forwarding keyboard / mouse-button / scroll input to the
	//! focused app and routes everything to the controller via the public event
	//! ring. Set by the compositor thread (driven by xrSetWorkspaceInputGrabEXT)
	//! and by the controller-owned drag/resize gestures; read by the WndProc
	//! thread.
	volatile LONG input_suppress;

	//! True when a mouse button press originated inside the app content rect.
	//! Used to prevent title bar clicks from being forwarded as app drags.
	//! Set on button-down inside rect, cleared on button-up.
	bool mouse_press_in_content;

	//! The HWND the most recent button-DOWN was forwarded to, or NULL if it
	//! was not forwarded (cursor outside the focused window's rect — i.e. a
	//! click on an unfocused window). Captured at WndProc time, BEFORE the
	//! workspace controller's async xrSetWorkspaceFocusedClientEXT can update
	//! the forward target. The render-loop click handler reads this to decide
	//! whether to synthesize a DOWN to the hit window for drag-in-one-click:
	//! it must NOT rely on focused_slot, which the controller mutates async.
	volatile HWND last_pointer_down_target;

	//! Workspace display processor for ESC/close 2D mode switch (opaque, can be NULL).
	volatile void *workspace_dp;

	//! Ring buffer for capture client input events (WndProc writes, render thread reads).
	//! Lock-free SPSC: WndProc is the single producer, render loop is the single consumer.
	struct workspace_input_event input_ring[WORKSPACE_INPUT_RING_SIZE];
	volatile LONG input_ring_write; //!< Next write index (WndProc thread)
	volatile LONG input_ring_read;  //!< Next read index (compositor thread)

	//! Phase 2.D: parallel ring for the public xrEnumerateWorkspaceInputEventsEXT
	//! path. SPSC; producer is WndProc, consumer is the service-side drain that
	//! enriches each pointer event with hit-test info before exposing on IPC.
	struct workspace_public_event_raw workspace_public_ring[WORKSPACE_PUBLIC_RING_SIZE];
	volatile LONG workspace_public_ring_write;
	volatile LONG workspace_public_ring_read;

	//! Phase 2.D: pointer-capture flag honored by WndProc click filtering.
	volatile LONG workspace_pointer_capture_enabled;
	volatile LONG workspace_pointer_capture_button;

	//! Phase 2.C spec_version 8: wakeup event the controller waits on.
	//! Window thread SetEvent's it after every workspace_public_ring_push so
	//! the controller's MsgWaitForMultipleObjects returns promptly. NULL
	//! until the IPC handler calls comp_d3d11_window_set_workspace_wakeup_event
	//! with the runtime's source-of-truth handle (lazy-created on the first
	//! xrAcquireWorkspaceWakeupEventEXT call). Read with InterlockedCompare
	//! ExchangePointer because the handle is set from the IPC thread and
	//! read from the WndProc thread.
	volatile HANDLE workspace_wakeup_event;

	//! Target HWND for SetForegroundWindow request (compositor writes, window thread reads).
	//! NULL means no pending request. Window thread clears after calling SetForegroundWindow.
	volatile HWND pending_foreground_hwnd;

	//! Signaled by window thread after SetForegroundWindow completes.
	volatile LONG foreground_done;

	//! Controller-supplied reserved-key table (XR_EXT_spatial_workspace spec_version
	//! 24). The controller declares which (vkCode, modifiers) chords it owns via
	//! xrSetWorkspaceReservedKeysEXT; reserved chords are still emitted on the public
	//! ring but never forwarded to the focused app. reserved_key_count is the publish
	//! barrier: the service thread fills reserved_keys[] then InterlockedExchanges the
	//! count LAST, so the WndProc thread never reads a half-written table. -1 means the
	//! controller has not registered a set — fall back to is_workspace_reserved_key's
	//! built-in default. >= 0 is the controller's table size.
	struct { uint32_t vk; uint32_t mods; } reserved_keys[WORKSPACE_RESERVED_KEYS_MAX];
	volatile LONG reserved_key_count;
};

// Forward declarations
static void set_fullscreen(HWND hWnd, bool fullscreen);

// Custom message IDs (posted to window thread from compositor thread)
#define WM_WORKSPACE_SET_FOREGROUND (WM_USER + 100)
// (WM_USER + 101) was WM_WORKSPACE_LAUNCH_APP — removed in #376; the
// browse + launch affordance is controller-owned now.
#define WM_WORKSPACE_SET_CAPTURE   (WM_USER + 102) //!< Phase 2.K: wParam=enabled (0/1).

// spec_version 24: when a key/char message is PostMessage'd to a composited IPC
// app, the runtime stamps the current modifier mask into spare lParam bits so
// the client-side shim (oxr_workspace_modal_win32.c) can restore the app
// thread's keyboard state before TranslateMessage / GetKeyState — PostMessage
// alone does not carry modifier state across processes. Bit 28 marks a forwarded
// message; bits 25-27 hold SHIFT/CTRL/ALT (same 3-bit mask as
// workspace_compute_modifiers()). These are Windows-reserved lParam bits (25-28)
// that TranslateMessage ignores; the shim clears them before the app sees them.
// KEEP IN SYNC with the decoder in oxr_workspace_modal_win32.c.
#define DXR_FWD_KEY_MARKER_BIT  (1u << 28)
#define DXR_FWD_KEY_MODS_SHIFT  25

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
	LONG next = (wr + 1) % WORKSPACE_INPUT_RING_SIZE;
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
 * Phase 2.D: push a raw event into the public-event ring (WndProc thread only).
 * Drops events when the ring is full; the workspace controller's drain rate
 * sets the bound, but losing input here is preferable to blocking WndProc.
 */
static void
workspace_public_ring_push(struct comp_d3d11_window *w,
                           uint32_t kind,
                           int32_t cursor_x,
                           int32_t cursor_y,
                           uint32_t button_or_vk,
                           uint32_t is_down,
                           uint32_t modifiers,
                           float scroll_delta_y)
{
	LONG wr = InterlockedCompareExchange(&w->workspace_public_ring_write, 0, 0);
	LONG rd = InterlockedCompareExchange(&w->workspace_public_ring_read, 0, 0);
	LONG next = (wr + 1) % WORKSPACE_PUBLIC_RING_SIZE;
	if (next == rd) {
		return;
	}
	struct workspace_public_event_raw *ev = &w->workspace_public_ring[wr];
	ev->kind = kind;
	ev->timestamp_ms = (uint32_t)GetTickCount();
	ev->cursor_x = cursor_x;
	ev->cursor_y = cursor_y;
	ev->button_or_vk = button_or_vk;
	ev->is_down = is_down;
	ev->modifiers = modifiers;
	ev->scroll_delta_y = scroll_delta_y;
	MemoryBarrier();
	InterlockedExchange(&w->workspace_public_ring_write, next);

	// spec_version 8: wake the controller's event-driven wait so it drains
	// promptly. SetEvent is a no-op when no waiter is pending; cheap.
	HANDLE wake = (HANDLE)InterlockedCompareExchangePointer(
	    (volatile PVOID *)&w->workspace_wakeup_event, NULL, NULL);
	if (wake != NULL) {
		SetEvent(wake);
	}
}

static uint32_t
workspace_compute_modifiers(void)
{
	uint32_t m = 0;
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   m |= 1u << 0;
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= 1u << 1;
	if (GetAsyncKeyState(VK_MENU) & 0x8000)    m |= 1u << 2;
	return m;
}

/*!
 * The built-in default reserved-key policy, used until a controller registers
 * its own table via xrSetWorkspaceReservedKeysEXT. These keys are NOT forwarded
 * to the focused app in workspace mode.
 *
 * SHIFT+TAB is forwarded (apps use it as a HUD toggle); only bare TAB is
 * reserved for workspace focus cycling.
 */
static bool
is_default_reserved_key(WPARAM vk, bool shift)
{
	// Only true workspace-management keys are reserved.
	// V, P, 0-9 are forwarded to the app (it may use them for its own purposes).
	// The qwerty handler processes them server-side regardless; if the app also
	// tries to change rendering mode via xrRequestDisplayRenderingModeEXT,
	// that call is blocked in workspace/IPC mode.
	switch (vk) {
	case VK_TAB:    return !shift;  // bare TAB cycles focus; Shift+TAB → app
	case VK_DELETE: return true;    // Close focused app
	// #307: ESC is a workspace-management key — the controller restores a
	// maximized window on ESC. Reserve it so it never reaches the focused app
	// (which would otherwise treat ESC as quit). When nothing is maximized the
	// controller ignores ESC, so it's a harmless no-op rather than killing the
	// app. (The runtime itself holds no maximize state — ADR-018.)
	case VK_ESCAPE: return true;
	// #305: [ and ] step the focused window's Z-depth — a workspace control
	// owned by the controller. Reserve them so they don't also reach the app.
	case VK_OEM_4:  return true;    // [ = window Z back
	case VK_OEM_6:  return true;    // ] = window Z forward
	default:        return false;
	}
}

/*!
 * Decide whether a key event is reserved (consumed by the workspace, never
 * forwarded to the focused app). spec_version 24: if the controller has
 * registered a reserved-key table, match exactly on (vkCode, modifiers) against
 * it — so {TAB,0} reserves bare Tab while Shift+Tab forwards. Until then, fall
 * back to the built-in default policy above.
 */
static bool
is_workspace_reserved_key(struct comp_d3d11_window *w, WPARAM vk)
{
	LONG n = InterlockedCompareExchange(&w->reserved_key_count, 0, 0);
	if (n < 0) {
		// No controller table yet — use the built-in default.
		bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		return is_default_reserved_key(vk, shift);
	}
	uint32_t mods = workspace_compute_modifiers();
	for (LONG i = 0; i < n; i++) {
		if (w->reserved_keys[i].vk == (uint32_t)vk &&
		    w->reserved_keys[i].mods == mods) {
			return true;
		}
	}
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
		// Hide the OS cursor over the workspace window's client area.
		// The runtime renders its own 3D cursor sprite into the atlas at
		// the per-frame raycast hit's z-depth with per-eye disparity
		// (spec_version 13: sprite content is controller-pushed via
		// xrSetWorkspaceCursorEXT). Win32 keeps tracking the cursor
		// position for hit-test, drag operations, and event delivery —
		// only the visual is suppressed.
		if (LOWORD(lParam) == HTCLIENT) {
			SetCursor(NULL);
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
		// Switch workspace DP to 2D mode (lens off) before closing.
		// This runs on the window thread and works even with no active clients.
		{
			void *dp = (void *)InterlockedCompareExchangePointer(
			    (volatile PVOID *)&w->workspace_dp, NULL, NULL);
			if (dp != NULL) {
				struct xrt_display_processor_d3d11 *xdp =
				    (struct xrt_display_processor_d3d11 *)dp;
				xrt_display_processor_d3d11_request_display_mode(xdp, false);
				U_LOG_W("D3D11 window: switched workspace DP to 2D on close");
			}
		}
		InterlockedExchange(&w->should_exit, TRUE);
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_KEYDOWN:
		// F11: toggle fullscreen for non-workspace (single app) windows.
		// In workspace mode, F11 flows to the controller as a workspace KEY
		// event (#307) — the controller owns the maximize policy.
		if (wParam == VK_F11) {
			HWND fwd_check = (HWND)InterlockedCompareExchangePointer(
			    (volatile PVOID *)&w->input_forward_hwnd, NULL, NULL);
			if (fwd_check == NULL) {
				// Non-workspace mode: toggle fullscreen directly
				LONG fs = InterlockedCompareExchange(&w->is_fullscreen, 0, 0);
				fs = !fs;
				InterlockedExchange(&w->is_fullscreen, fs);
				set_fullscreen(hWnd, fs != 0);
				U_LOG_W("D3D11 window: F11 toggled to %s mode", fs ? "fullscreen" : "windowed");
				return 0;
			}
			// Workspace mode: fall through to forwarding (handled server-side)
		}
		// FALLTHROUGH to WM_KEYUP/SYSKEYDOWN/SYSKEYUP/CHAR
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
	case WM_CHAR:
	case WM_SYSCHAR: {
		// Phase 2.D: emit KEY events to the public-event ring before any
		// suppression / forwarding logic so a workspace controller draining
		// xrEnumerateWorkspaceInputEventsEXT sees the key regardless of
		// whether the runtime forwards it. WM_CHAR/WM_SYSCHAR are post-
		// translation duplicates of the original VK_* — skip them so the
		// public surface reports physical keys only.
		if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
			workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_KEY, 0, 0,
			                           (uint32_t)wParam, 1, workspace_compute_modifiers(), 0.0f);
		} else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
			workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_KEY, 0, 0,
			                           (uint32_t)wParam, 0, workspace_compute_modifiers(), 0.0f);
		}

		// When input is grabbed by the controller (modal UI like the launcher
		// band) or a workspace drag/resize is active, eat the key entirely —
		// don't forward, don't run qwerty. The KEY event was already pushed to
		// the public ring above, so the controller still sees arrows / Enter /
		// Esc; they just never reach the focused app.
		if (input_is_suppressed(w)) {
			return 0;
		}

		// Workspace input forwarding: all keys go to BOTH qwerty and the app.
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
					LPARAM lp = (LPARAM)lParam | DXR_FWD_KEY_MARKER_BIT |
					            ((LPARAM)(workspace_compute_modifiers() & 0x7u)
					             << DXR_FWD_KEY_MODS_SHIFT);
					PostMessage(fwd, message, wParam, lp);
				}
				return 0;
			}

			// Process qwerty first
#ifdef XRT_BUILD_DRIVER_QWERTY
			// Suppress qwerty entirely under workspace mode: the shell + apps
			// handle their own input, and qwerty intercepting V here would
			// fire qwerty_toggle_display_mode behind the workspace's back
			// (bypassing the acked-flip pipeline + cooldown). Bare V should
			// fall through to the focused app's WindowProc / qwerty stays
			// active only in non-workspace paths.
			if (w->qwerty_enabled && w->xsysd != NULL && !bridge_page_attached(hWnd) &&
			    !InterlockedCompareExchange(&w->workspace_mode_active, 0, 0)) {
				bool handled = false;
				qwerty_process_win32(w->xsysd->xdevs, w->xsysd->xdev_count,
				                     message, wParam, lParam, &handled);
			}
#endif
			if (is_workspace_reserved_key(w, wParam)) {
				// Controller-reserved chord (or built-in default: bare
				// TAB/DELETE/ESC/[/]) → emitted on the public ring above but
				// NOT forwarded to the app. Non-reserved chords (e.g. Shift+Tab,
				// Ctrl+C) fall through and get forwarded below.
				return 0;
			}
			// #307: ESC is no longer suppressed here. The maximize state
			// machine moved to the controller (ADR-018); ESC flows to the
			// controller as a workspace KEY event (pushed above, before this
			// forwarding) AND on to the focused app. The controller restores a
			// maximized window on ESC; when nothing is maximized it ignores ESC
			// and the app handles it as before.
			// Phase 2.G: Ctrl+1..4 are no longer reserved by the runtime.
			// They flow through the public input-event drain so a workspace
			// controller can bind them to its own layout presets.
			if (is_capture) {
				// Capture client: buffer for SendInput dispatch. SendInput
				// injects into the real OS input queue (modifier VK keydowns
				// included), so chords are already preserved — no mask needed.
				input_ring_push(w, message, (uint64_t)wParam, (int64_t)lParam, -1, -1);
			} else {
				// IPC app: PostMessage can't carry keyboard state across
				// processes, so stamp the modifier mask into spare lParam bits
				// for the client-side shim to restore (spec_version 24).
				LPARAM lp = (LPARAM)lParam | DXR_FWD_KEY_MARKER_BIT |
				            ((LPARAM)(workspace_compute_modifiers() & 0x7u)
				             << DXR_FWD_KEY_MODS_SHIFT);
				PostMessage(fwd, message, wParam, lp);
			}
			return 0;
		}

		// Normal mode (no forwarding): pass all keys to qwerty.
		// ESC → close window: Phase 4C made ESC a no-op in qwerty to
		// prevent it from killing the workspace, but that also broke non-workspace
		// ESC (WebXR, standalone IPC clients). Handle ESC here before
		// qwerty sees it. Workspace mode never reaches this block because
		// input_forward_hwnd != NULL takes the forwarding path above.
		if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
			// Workspace mode with no focused app: input_forward_hwnd is NULL so
			// we fall into this block, but closing the window kills the service.
			// Swallow the key instead; user can press Ctrl+Space to dismiss.
			//
			// Phase 2.G considered removing this carve-out (the workspace
			// controller now owns deactivate semantics via xrDeactivateSpatial-
			// WorkspaceEXT and Ctrl+Space, so the runtime in principle could
			// fall through to WM_CLOSE on bare ESC). We kept it because an
			// accidental ESC press while in empty-workspace state would still
			// take the service down with the window, which is a real regression
			// risk. The controller can revisit if it grows a more granular
			// deactivate UX.
			if (InterlockedCompareExchange(&w->workspace_mode_active, 0, 0)) {
				return 0;
			}
			U_LOG_W("D3D11 window: ESC pressed — closing (non-workspace mode)");
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
		// Suppress qwerty under workspace mode — see L596 comment.
		if (w->qwerty_enabled && w->xsysd != NULL &&
		    !InterlockedCompareExchange(&w->workspace_mode_active, 0, 0)) {
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

	// Mouse input: forward to app in workspace mode, or to qwerty in normal mode
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL: {
		// Phase 2.D: emit POINTER and SCROLL events to the public-event ring
		// before any filtering. SendInput-injected mouse events are still
		// skipped here — they're the runtime's own forwarded clicks bouncing
		// back, not new user input.
		// Phase 2.K: WM_MOUSEMOVE now emits MOTION events while pointer
		// capture is enabled (controllers want per-frame hover for chrome
		// and drag-to-rotate carousels). Idle motion still bypasses the ring
		// when capture is off.
		if (GetMessageExtraInfo() != (LPARAM)WORKSPACE_SENDINPUT_MARKER) {
			int32_t cx = GET_X_LPARAM(lParam);
			int32_t cy = GET_Y_LPARAM(lParam);
			uint32_t mods = workspace_compute_modifiers();
			// #305: SCROLL is emitted later in the wheel-routing block so that
			// only workspace (non-app-forwarded) scrolls reach the controller —
			// a plain scroll over app content is forwarded to the app and must
			// NOT also drive a controller resize.
			if (message == WM_MOUSEMOVE) {
				// Gate motion on pointer capture so idle hover doesn't
				// flood the public ring. wParam carries the held-button
				// state (MK_LBUTTON / MK_RBUTTON / MK_MBUTTON) — pack
				// directly into button_or_vk as a button mask.
				LONG cap_enabled =
				    InterlockedCompareExchange(&w->workspace_pointer_capture_enabled, 0, 0);
				if (cap_enabled) {
					uint32_t mask = 0;
					if (wParam & MK_LBUTTON) mask |= 0x1u;
					if (wParam & MK_RBUTTON) mask |= 0x2u;
					if (wParam & MK_MBUTTON) mask |= 0x4u;
					workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_MOTION,
					                           cx, cy, mask, 0, mods, 0.0f);
				}
			} else if (message != WM_MOUSEWHEEL) {
				uint32_t button = 0;
				uint32_t is_down = 0;
				switch (message) {
				case WM_LBUTTONDOWN: button = 1; is_down = 1; break;
				case WM_LBUTTONUP:   button = 1; is_down = 0; break;
				case WM_RBUTTONDOWN: button = 2; is_down = 1; break;
				case WM_RBUTTONUP:   button = 2; is_down = 0; break;
				case WM_MBUTTONDOWN: button = 3; is_down = 1; break;
				case WM_MBUTTONUP:   button = 3; is_down = 0; break;
				}
				workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_POINTER,
				                           cx, cy, button, is_down, mods, 0.0f);
				// Default: this DOWN was not forwarded. The forward site
				// below overwrites with the actual target HWND if it is.
				if (is_down) {
					w->last_pointer_down_target = NULL;
				}
			}
		}

		// Skip forwarding when input is grabbed by the controller (modal UI like
		// the launcher band) or a workspace drag/resize is active. Wheel events
		// still reach the controller: the POINTER/MOTION push above runs before
		// this gate, but SCROLL is emitted lower in the fwd!=NULL block — so under
		// a grab (where fwd may be a focused app) we emit SCROLL_EXT here, then
		// swallow the wheel so it never also forwards to the app.
		if (input_is_suppressed(w)) {
			if (message == WM_MOUSEWHEEL &&
			    GetMessageExtraInfo() != (LPARAM)WORKSPACE_SENDINPUT_MARKER) {
				POINT pt;
				pt.x = GET_X_LPARAM(lParam);
				pt.y = GET_Y_LPARAM(lParam);
				ScreenToClient(hWnd, &pt);
				short delta = GET_WHEEL_DELTA_WPARAM(wParam);
				workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_SCROLL, pt.x, pt.y, 0, 0,
				                           workspace_compute_modifiers(),
				                           (float)delta / (float)WHEEL_DELTA);
			}
			break; // fall through to qwerty/default handling
		}

		// Skip re-forwarding of SendInput-injected mouse events (prevents
		// WndProc→SendInput→WM_LBUTTONDOWN→WndProc infinite loop).
		if (GetMessageExtraInfo() == (LPARAM)WORKSPACE_SENDINPUT_MARKER) {
			return DefWindowProcW(hWnd, message, wParam, lParam);
		}

		HWND fwd = (HWND)InterlockedCompareExchangePointer((volatile PVOID *)&w->input_forward_hwnd, NULL, NULL);
		LONG is_capture = InterlockedCompareExchange(&w->input_forward_is_capture, 0, 0);
		if (fwd != NULL) {
			// Workspace mode wheel routing:
			//   cursor over app content + no modifier → forward to app (e.g. 3DGS zoom)
			//   cursor outside app content            → workspace scroll (emitted to controller)
			//   Ctrl / Shift + scroll                 → workspace scroll (emitted to controller)
			// Capture clients don't take wheel input, so always treat as workspace.
			// #305: the runtime no longer interprets workspace scrolls — it emits
			// SCROLL_EXT and the controller owns resize/Z policy. Emission happens
			// only on this (non-forwarded) path so a plain scroll over app content
			// goes to the app alone, never also resizing.
			if (message == WM_MOUSEWHEEL) {
				bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

				// WM_MOUSEWHEEL puts SCREEN coords in lParam (unlike other
				// mouse messages). Convert to workspace-client coords for the
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
					// Controller-bound scroll: resize / Z-depth policy lives
					// in the workspace controller (ADR-018, #305).
					workspace_public_ring_push(w, WORKSPACE_PUBLIC_EVENT_SCROLL,
					                           pt.x, pt.y, 0, 0,
					                           workspace_compute_modifiers(),
					                           (float)delta / (float)WHEEL_DELTA);
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

			// Remap workspace-window coords to app-window coords
			LONG rx = InterlockedCompareExchange(&w->input_forward_rect_x, 0, 0);
			LONG ry = InterlockedCompareExchange(&w->input_forward_rect_y, 0, 0);
			LONG rw = InterlockedCompareExchange(&w->input_forward_rect_w, 0, 0);
			LONG rh = InterlockedCompareExchange(&w->input_forward_rect_h, 0, 0);

			if (rw > 0 && rh > 0) {
				// Extract workspace-window client coords
				int workspace_x = GET_X_LPARAM(lParam);
				int workspace_y = GET_Y_LPARAM(lParam);

				bool in_rect = (workspace_x >= rx && workspace_x < rx + rw &&
				                workspace_y >= ry && workspace_y < ry + rh);

				// Track whether the press originated inside the content rect.
				// Only forward drag (button-held movement) if the press started
				// inside content — prevents title bar clicks from being forwarded.
				if (is_button_down) {
					w->mouse_press_in_content = in_rect;
				}
				if (is_button_up) {
					w->mouse_press_in_content = false;
				}

				LONG cap_en = InterlockedCompareExchange(
				    &w->workspace_pointer_capture_enabled, 0, 0);
				LONG cap_btn = InterlockedCompareExchange(
				    &w->workspace_pointer_capture_button, 0, 0);
				uint32_t evt_button = 0;
				switch (message) {
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:   evt_button = 1; break;
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:   evt_button = 2; break;
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:   evt_button = 3; break;
				default: break;
				}

				// The controller enables pointer capture ONLY for its own
				// window-management drags (title-bar move, edge resize — see
				// displayxr-shell's enable_pointer_capture call sites) and
				// disables it on release. The captured button's DOWN/UP still
				// forward (capture_active): the grip press lands inside the
				// window's forward rect and is eagerly forwarded before the
				// controller engages capture, so the app must still receive the
				// matching UP or it sits with a stuck button (which then turns
				// later hover-moves into a phantom drag and snaps the content at
				// drag end). What must be swallowed are the MOVES in between —
				// they carry no evt_button, so the old capture_active gate missed
				// them and they leaked into the app via in_rect the moment the
				// cursor crossed into content, mirroring the controller's drag.
				bool capture_active =
				    (cap_en != 0) && evt_button != 0 &&
				    cap_btn == (LONG)evt_button;
				uint32_t cap_mk = 0;
				switch (cap_btn) {
				case 1: cap_mk = MK_LBUTTON; break;
				case 2: cap_mk = MK_RBUTTON; break;
				case 3: cap_mk = MK_MBUTTON; break;
				default: break;
				}
				bool capture_move_suppress =
				    (cap_en != 0) && cap_mk != 0 && message == WM_MOUSEMOVE &&
				    (((uint32_t)wParam & cap_mk) != 0);

				// Forward if inside rect, dragging from an in-content press, or
				// the captured button's up/down — but never a captured drag's
				// moves.
				bool buttons_held = (wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
				bool dragging = buttons_held && w->mouse_press_in_content;
				if (!capture_move_suppress && (in_rect || dragging || capture_active)) {
					// Remap to app-window client coords.
					// Scale if target HWND is a different size than the
					// virtual rect (e.g., captured 2D windows).
					RECT target_cr;
					GetClientRect(fwd, &target_cr);
					int target_w = target_cr.right - target_cr.left;
					int target_h = target_cr.bottom - target_cr.top;

					int rel_x = workspace_x - rx;
					int rel_y = workspace_y - ry;
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

					// Record where a button-DOWN was actually forwarded so
					// the render-loop click handler can tell whether the hit
					// window already received this DOWN (no synth needed) or
					// not (synth a DOWN for drag-in-one-click). Independent of
					// the controller's async focus update.
					if (is_button_down) {
						w->last_pointer_down_target = fwd;
					}

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
							inp.mi.dwExtraInfo = WORKSPACE_SENDINPUT_MARKER;
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
		// Normal mode: pass to qwerty driver — but not under workspace
		// (workspace apps own input; qwerty intercepting here is a regression).
#ifdef XRT_BUILD_DRIVER_QWERTY
		if (w->qwerty_enabled && w->xsysd != NULL && !bridge_page_attached(hWnd) &&
		    !InterlockedCompareExchange(&w->workspace_mode_active, 0, 0)) {
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

	case WM_WORKSPACE_SET_FOREGROUND: {
		// Cross-thread foreground request from compositor.
		// wParam = target HWND. NULL means restore workspace window.
		HWND target = (HWND)wParam;
		if (target != NULL) {
			SetForegroundWindow(target);
		} else {
			SetForegroundWindow(hWnd);
		}
		InterlockedExchange(&w->foreground_done, 1);
		return 0;
	}

	case WM_WORKSPACE_SET_CAPTURE: {
		// Phase 2.K: cross-thread SetCapture / ReleaseCapture request. SetCapture
		// must run on the window thread, so the IPC-server thread posts here.
		// wParam = 1 → SetCapture(hWnd) so motion outside the workspace window
		// keeps reaching this WndProc; wParam = 0 → ReleaseCapture.
		if (wParam) {
			SetCapture(hWnd);
		} else {
			// Only release if we actually hold capture, to avoid stomping on
			// some other window that legitimately captured the cursor.
			if (GetCapture() == hWnd) {
				ReleaseCapture();
			}
		}
		return 0;
	}

	// #376: WM_WORKSPACE_LAUNCH_APP (the Ctrl+O browse + launch handler) was
	// removed. The browse + launch-arbitrary-exe affordance is now owned by the
	// workspace controller, which has its own file picker and launch path.

	default:
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	return 0;
}

/*!
 * Two-party cleanup for @ref comp_d3d11_window. Both the window thread (when it
 * exits) and @ref comp_d3d11_window_destroy call this exactly once. The second
 * caller (vote reaches 2) frees the struct and closes its handles; the first
 * caller returns, leaving the struct alive for the other party still using it.
 *
 * This is the fix for the CTS full-run crash: previously destroy freed @p w on a
 * wait timeout while the window thread was still inside DestroyWindow (its window
 * proc reads @p w back from GWLP_USERDATA), causing a use-after-free
 * ACCESS_VIOLATION. Now whoever finishes last frees, so @p w always outlives both.
 */
static void
window_release(struct comp_d3d11_window *w)
{
	if (InterlockedIncrement(&w->cleanup_votes) < 2) {
		return; // other party is still using w; it will free.
	}
	if (w->thread_handle != NULL) {
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
			window_release(w);
			return 1;
		}
	}

	// Position on the 3D display. The vendor plug-in iface publishes the
	// display top-left through xsysc->info.display_screen_left/top; the
	// state tracker forwards those into comp_d3d11_window_create. (0, 0)
	// means primary monitor (sim_display default or unknown panel).
	RECT rc = {
	    (LONG)w->display_screen_left,
	    (LONG)w->display_screen_top,
	    (LONG)w->display_screen_left + (LONG)w->requested_width,
	    (LONG)w->display_screen_top + (LONG)w->requested_height,
	};

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
		window_release(w);
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
	window_release(w);
	return 0;
}

/*
 *
 * Public API
 *
 */

extern "C" xrt_result_t
comp_d3d11_window_create(uint32_t width,
                         uint32_t height,
                         int32_t screen_left,
                         int32_t screen_top,
                         struct comp_d3d11_window **out)
{
	struct comp_d3d11_window *w = U_TYPED_CALLOC(struct comp_d3d11_window);
	if (w == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	w->instance = GetModuleHandle(NULL);
	w->requested_width = width > 0 ? width : 1920;
	w->requested_height = height > 0 ? height : 1080;
	w->display_screen_left = screen_left;
	w->display_screen_top = screen_top;
	w->xsysd = NULL;
	w->qwerty_enabled = true;  // Always enabled for DisplayXR-owned windows
	// -1 = controller has not registered a reserved-key set yet; the WndProc
	// gate falls back to the built-in default until xrSetWorkspaceReservedKeysEXT
	// arrives (U_TYPED_CALLOC would otherwise leave this 0 = "empty set").
	w->reserved_key_count = -1;

	U_LOG_W("D3D11 window: QWERTY input ENABLED");

	U_LOG_W("D3D11 window: Creating window on dedicated thread (%ux%u) at (%d, %d)",
	        w->requested_width, w->requested_height,
	        (int)w->display_screen_left, (int)w->display_screen_top);

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
		// The thread is still running and still references w. Vote and let the
		// thread free w when it eventually exits (window_release: last voter
		// frees). Freeing here would be a use-after-free.
		window_release(w);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Check if the window was actually created
	if (w->hwnd == NULL) {
		U_LOG_E("D3D11 window: Window thread failed to create HWND");
		// The thread took an error exit and already cast its vote; wait for it
		// to finish, then this second vote frees w.
		WaitForSingleObject(w->thread_handle, 5000);
		window_release(w);
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
	*window = NULL;

	U_LOG_W("D3D11 window: Destroying window");

	// Ask the window thread to close: WM_CLOSE -> DestroyWindow -> WM_DESTROY ->
	// PostQuitMessage breaks its message loop, after which it frees its own
	// resources and votes via window_release().
	InterlockedExchange(&w->should_exit, TRUE);
	if (w->hwnd != NULL) {
		PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
	}

	// Wait so the caller (compositor teardown) knows the window is gone before it
	// releases the D3D device the thread renders with. The WM_CLOSE handler
	// switches the DP to 2D and runs DestroyWindow, which synchronously re-enters
	// the window proc (it reads w back from GWLP_USERDATA), so under the CTS's
	// heavy create/destroy churn this can take a while — allow a generous timeout.
	// If it still hasn't exited we proceed anyway: window_release() guarantees we
	// never free w while the thread is alive (whoever finishes last frees), which
	// eliminates the use-after-free ACCESS_VIOLATION that crashed the CTS run.
	if (w->thread_handle != NULL) {
		if (WaitForSingleObject(w->thread_handle, 30000) != WAIT_OBJECT_0) {
			U_LOG_E("D3D11 window: thread still running after 30s; deferring "
			        "cleanup to it (no use-after-free)");
		}
	}

	window_release(w);
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

extern "C" void
comp_d3d11_window_set_reserved_keys(struct comp_d3d11_window *window,
                                    const uint32_t *vks,
                                    const uint32_t *mods,
                                    uint32_t count)
{
	if (window == NULL) {
		return;
	}
	if (count == 0 || vks == NULL || mods == NULL) {
		// Restore the built-in default policy.
		InterlockedExchange(&window->reserved_key_count, -1);
		U_LOG_W("D3D11 window: reserved keys reset to built-in default");
		return;
	}
	if (count > WORKSPACE_RESERVED_KEYS_MAX) {
		count = WORKSPACE_RESERVED_KEYS_MAX;
	}
	// Fill the table BEFORE publishing the count: the WndProc reader keys off
	// reserved_key_count, so the array must be fully written when the count
	// becomes visible. InterlockedExchange of the count is the publish barrier.
	for (uint32_t i = 0; i < count; i++) {
		window->reserved_keys[i].vk = vks[i];
		window->reserved_keys[i].mods = mods[i];
	}
	MemoryBarrier();
	InterlockedExchange(&window->reserved_key_count, (LONG)count);
	U_LOG_W("D3D11 window: controller registered %u reserved key chord(s)", count);
}

void *
comp_d3d11_window_get_last_pointer_down_target(struct comp_d3d11_window *window)
{
	if (window == NULL) {
		return NULL;
	}
	return (void *)InterlockedCompareExchangePointer(
	    (volatile PVOID *)&window->last_pointer_down_target, NULL, NULL);
}

void
comp_d3d11_window_set_workspace_mode_active(struct comp_d3d11_window *window, bool active)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchange(&window->workspace_mode_active, active ? 1 : 0);
}

void
comp_d3d11_window_set_workspace_wakeup_event(struct comp_d3d11_window *window, void *handle)
{
	if (window == NULL) {
		return;
	}
	// Window doesn't take ownership — runtime keeps the source HANDLE and
	// closes it on workspace teardown. Set once at workspace activation.
	InterlockedExchangePointer((volatile PVOID *)&window->workspace_wakeup_event, handle);
}

// Returns true if input forwarding should currently be suppressed — i.e. the
// controller has grabbed input (modal UI like the launcher band, via
// xrSetWorkspaceInputGrabEXT) or a workspace drag/resize gesture is active.
static bool
input_is_suppressed(struct comp_d3d11_window *w)
{
	return InterlockedCompareExchange(&w->input_suppress, 0, 0) != 0;
}

extern "C" void
comp_d3d11_window_set_input_suppress(struct comp_d3d11_window *window, bool suppress)
{
	if (window == NULL) return;
	InterlockedExchange(&window->input_suppress, suppress ? 1 : 0);

	// Publish the suppress state to the compositor HWND so cross-process
	// consumers can mirror it. The WebXR bridge's low-level mouse hook
	// reads DXR_InSizeMove to skip forwarding — without this extension,
	// title-bar drags, edge resizes, and controller input-grab suppress input
	// for in-process handle apps (via input_suppress) but still forward mouse
	// events to bridge-aware pages (Chrome interprets them as in-scene
	// drag/rotate), so moving a WebXR window drags its content instead
	// of the window itself. The native WM_ENTERSIZEMOVE path also sets
	// this prop directly; reusing the prop keeps one gate on the bridge
	// side. Semantically DXR_InSizeMove now means "compositor is in a
	// modal-like interaction (native OR workspace-virtual)" — document in
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

extern "C" void
comp_d3d11_window_set_workspace_dp(struct comp_d3d11_window *window, void *dp)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchangePointer((volatile PVOID *)&window->workspace_dp, (PVOID)dp);
}

extern "C" uint32_t
comp_d3d11_window_consume_input_events(struct comp_d3d11_window *window,
                                       struct workspace_input_event *out_events,
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
		InterlockedExchange(&window->input_ring_read, (rd + 1) % WORKSPACE_INPUT_RING_SIZE);
		count++;
	}
	return count;
}

extern "C" uint32_t
comp_d3d11_window_consume_workspace_public_events(struct comp_d3d11_window *window,
                                                  struct workspace_public_event_raw *out_events,
                                                  uint32_t max_events)
{
	if (window == NULL || out_events == NULL || max_events == 0) {
		return 0;
	}
	uint32_t count = 0;
	while (count < max_events) {
		LONG rd = InterlockedCompareExchange(&window->workspace_public_ring_read, 0, 0);
		LONG wr = InterlockedCompareExchange(&window->workspace_public_ring_write, 0, 0);
		if (rd == wr) {
			break;
		}
		MemoryBarrier();
		out_events[count] = window->workspace_public_ring[rd];
		InterlockedExchange(&window->workspace_public_ring_read, (rd + 1) % WORKSPACE_PUBLIC_RING_SIZE);
		count++;
	}
	return count;
}

extern "C" bool
comp_d3d11_window_is_workspace_pointer_capture_enabled(struct comp_d3d11_window *window)
{
	if (window == NULL) return false;
	return InterlockedCompareExchange(&window->workspace_pointer_capture_enabled, 0, 0) != 0;
}

extern "C" void
comp_d3d11_window_set_workspace_pointer_capture(struct comp_d3d11_window *window, bool enabled, uint32_t button)
{
	if (window == NULL) {
		return;
	}
	InterlockedExchange(&window->workspace_pointer_capture_enabled, enabled ? 1 : 0);
	InterlockedExchange(&window->workspace_pointer_capture_button, (LONG)button);
	// Phase 2.K: also drive Win32 SetCapture / ReleaseCapture so the WndProc
	// keeps receiving WM_MOUSEMOVE while the cursor leaves the workspace
	// window during a drag. SetCapture must run on the thread that owns the
	// HWND, so post to the window thread.
	if (window->hwnd != NULL) {
		PostMessageW(window->hwnd, WM_WORKSPACE_SET_CAPTURE, enabled ? 1 : 0, 0);
	}
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
	PostMessageW(window->hwnd, WM_WORKSPACE_SET_FOREGROUND, (WPARAM)target_hwnd, 0);

	// Wait for completion (with timeout to avoid deadlock)
	for (int i = 0; i < 100; i++) {
		if (InterlockedCompareExchange(&window->foreground_done, 0, 0)) {
			break;
		}
		Sleep(1);
	}
}

// #376: comp_d3d11_window_request_app_launch was removed — the Ctrl+O browse +
// launch affordance is controller-owned now.
