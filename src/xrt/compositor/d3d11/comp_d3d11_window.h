// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window management.
 *
 * This module provides window creation for the D3D11 native compositor
 * when XR_EXT_win32_window_binding is NOT used. This allows apps like Blender
 * that don't provide a window handle to still use the D3D11 native compositor.
 *
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct comp_d3d11_window;
struct xrt_system_devices;

/*!
 * Input event buffered from WndProc for capture client SendInput dispatch.
 * The WndProc pushes these into a ring buffer instead of PostMessage for
 * capture clients; the compositor thread drains them and calls SendInput.
 */
struct workspace_input_event
{
	uint32_t message; //!< WM_KEYDOWN, WM_CHAR, WM_LBUTTONDOWN, etc.
	uint64_t wParam;
	int64_t lParam;
	int32_t mapped_x; //!< Pre-remapped app coords (-1 if keyboard event)
	int32_t mapped_y;
};

#define WORKSPACE_INPUT_RING_SIZE 64

/*!
 * Phase 2.D: raw event captured by WndProc for the public-API event drain
 * (xrEnumerateWorkspaceInputEventsEXT). The service-side drain enriches each
 * raw POINTER event with the workspace hit-test (clientId, region, UV) before
 * exposing it on the public surface; KEY and SCROLL events pass through with
 * no extra geometry.
 *
 * Kept separate from `struct workspace_input_event` (the capture-client ring
 * for SendInput dispatch) — this ring is parallel and consumed by a different
 * code path. WndProc writes into both rings as appropriate.
 */
enum workspace_public_event_kind
{
	WORKSPACE_PUBLIC_EVENT_POINTER = 0,
	WORKSPACE_PUBLIC_EVENT_KEY     = 2,
	WORKSPACE_PUBLIC_EVENT_SCROLL  = 3,
	WORKSPACE_PUBLIC_EVENT_MOTION  = 4, //!< Phase 2.K: per-frame WM_MOUSEMOVE while capture is enabled.
};

struct workspace_public_event_raw
{
	uint32_t kind;            //!< enum workspace_public_event_kind
	uint32_t timestamp_ms;    //!< Low 32 bits of GetTickCount.
	int32_t  cursor_x;        //!< POINTER/MOTION/SCROLL: client-area px; else 0.
	int32_t  cursor_y;
	uint32_t button_or_vk;    //!< POINTER: 1=L, 2=R, 3=M. KEY: VK code. MOTION: held-button mask.
	uint32_t is_down;         //!< POINTER/KEY: 1 = down, 0 = up. MOTION: unused (0).
	uint32_t modifiers;       //!< bit0=SHIFT, bit1=CTRL, bit2=ALT.
	float    scroll_delta_y;  //!< SCROLL only: wheel ticks; positive = up.
};

#define WORKSPACE_PUBLIC_RING_SIZE 32

/*!
 * Create a self-owned window for the D3D11 compositor.
 *
 * This creates a window on a **dedicated thread**. The window thread
 * handles its own message pump via GetMessage/DispatchMessage. The
 * caller does NOT need to pump messages — the compositor thread
 * continues rendering independently during modal drag/resize.
 *
 * The window is automatically positioned on the Leia display if found,
 * or on a secondary monitor, or on the primary display as fallback.
 *
 * By default, the window starts in fullscreen mode unless the environment
 * variable XRT_COMPOSITOR_START_WINDOWED=1 is set.
 *
 * @param width  Requested window width
 * @param height Requested window height
 * @param out    Pointer to receive the created window handle
 *
 * @return XRT_SUCCESS on success, error code otherwise
 */
xrt_result_t
comp_d3d11_window_create(uint32_t width, uint32_t height, struct comp_d3d11_window **out);

/*!
 * Destroy the self-owned window.
 *
 * Posts WM_CLOSE to the window thread and waits for it to exit.
 * Can be called from any thread.
 *
 * @param window Pointer to window handle (set to NULL after destruction)
 */
void
comp_d3d11_window_destroy(struct comp_d3d11_window **window);

/*!
 * Get the Win32 HWND handle from the window.
 *
 * @param window The window object
 *
 * @return The HWND handle, or NULL if window creation failed
 */
void *
comp_d3d11_window_get_hwnd(struct comp_d3d11_window *window);

/*!
 * Check if the window is still valid and not closed by user.
 *
 * @param window The window object
 *
 * @return true if window is valid, false if closed or invalid
 */
bool
comp_d3d11_window_is_valid(struct comp_d3d11_window *window);

/*!
 * Get the current dimensions of the window.
 *
 * @param window     The window object
 * @param out_width  Pointer to receive current width
 * @param out_height Pointer to receive current height
 */
void
comp_d3d11_window_get_dimensions(struct comp_d3d11_window *window,
                                  uint32_t *out_width,
                                  uint32_t *out_height);

/*!
 * Check if the window is currently inside a modal move/size loop.
 *
 * Returns true between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE. Callers
 * should defer expensive operations (swapchain resize, texture
 * reallocation) until the drag finishes.
 *
 * @param window The window object
 *
 * @return true if the user is currently dragging/resizing the window
 */
bool
comp_d3d11_window_is_in_size_move(struct comp_d3d11_window *window);

/*!
 * Wait for a WM_PAINT request during drag. Returns true if compositor
 * should render (drag in progress), false if drag ended.
 * Only blocks during modal size-move. Returns false immediately otherwise.
 *
 * @param window The window object
 *
 * @return true if drag is in progress and compositor should render
 */
bool
comp_d3d11_window_wait_for_paint(struct comp_d3d11_window *window);

/*!
 * Signal that the compositor has finished rendering and presenting.
 * Unblocks the WM_PAINT handler so the modal drag loop can continue.
 *
 * @param window The window object
 */
void
comp_d3d11_window_signal_paint_done(struct comp_d3d11_window *window);

/*!
 * No-op. The dedicated window thread handles its own messages.
 *
 * Retained for API compatibility. Callers do not need to pump messages
 * for the self-owned window.
 *
 * @param window The window object
 */
void
comp_d3d11_window_pump_messages(struct comp_d3d11_window *window);

/*!
 * No-op. With the dedicated window thread, the compositor thread
 * continues rendering during drag/resize without interruption.
 *
 * Retained for API compatibility.
 *
 * @param window    The window object
 * @param callback  Ignored
 * @param userdata  Ignored
 */
void
comp_d3d11_window_set_repaint_callback(struct comp_d3d11_window *window,
                                        void (*callback)(void *userdata),
                                        void *userdata);

/*!
 * Set the system devices for qwerty input handling.
 *
 * When set, the window will forward keyboard/mouse input to the qwerty driver.
 * This allows direct input from the D3D11 window without requiring the SDL debug GUI.
 *
 * @param window The window object
 * @param xsysd  The system devices (can be NULL to disable input handling)
 */
void
comp_d3d11_window_set_system_devices(struct comp_d3d11_window *window,
                                      struct xrt_system_devices *xsysd);

/*!
 * Set the target HWND and window rect for input forwarding (workspace mode).
 *
 * When hwnd is non-NULL, the window enters workspace input-forwarding mode:
 * - Workspace-reserved keys (ESC, TAB, DELETE) are consumed by the workspace
 * - All other keyboard input is forwarded to the target HWND via PostMessage
 * - Mouse events are remapped from workspace-window coords to app-window coords
 *   using the provided rect, then forwarded. Mouse outside the rect is not forwarded.
 *
 * When hwnd is NULL, normal qwerty handling resumes.
 *
 * @param window The window object
 * @param hwnd   The focused app's HWND (NULL to disable forwarding)
 * @param rect_x Virtual window left edge in workspace-window client pixels
 * @param rect_y Virtual window top edge in workspace-window client pixels
 * @param rect_w Virtual window width in workspace-window client pixels
 * @param rect_h Virtual window height in workspace-window client pixels
 */
void
comp_d3d11_window_set_input_forward(struct comp_d3d11_window *window,
                                     void *hwnd,
                                     int32_t rect_x,
                                     int32_t rect_y,
                                     int32_t rect_w,
                                     int32_t rect_h,
                                     bool is_capture);

/*!
 * Mark workspace mode as active/inactive on the window. While active, ESC on the
 * compositor window is swallowed instead of posting WM_CLOSE — an empty workspace
 * (no focused app) would otherwise take the service down with its own window.
 */
void
comp_d3d11_window_set_workspace_mode_active(struct comp_d3d11_window *window, bool active);

/*!
 * Phase 2.C spec_version 8: hand the window the workspace wakeup event handle.
 * Window thread SetEvent's it after every public-event ring push so the
 * controller's event-driven wait wakes promptly. Pass NULL to clear.
 * Window does NOT take ownership — runtime owns the source HANDLE.
 */
void
comp_d3d11_window_set_workspace_wakeup_event(struct comp_d3d11_window *window, void *handle);

/*!
 * Tell the window layer whether any client is currently maximized/fullscreen.
 * When true, ESC is consumed by the workspace for fullscreen restore and not
 * forwarded to the focused app.
 */
void
comp_d3d11_window_set_any_maximized(struct comp_d3d11_window *window, bool maximized);

/*!
 * Suppress or resume input forwarding (for workspace drag/resize operations).
 * When suppressed, the WndProc does not forward mouse or keyboard events to
 * the app.
 */
void
comp_d3d11_window_set_input_suppress(struct comp_d3d11_window *window, bool suppress);

/*!
 * Phase 5.12: extend input suppression for @p ms milliseconds from now,
 * regardless of the immediate `input_suppress` flag. Used when closing the
 * launcher: any WM_KEYDOWN messages still sitting in the window thread's
 * queue (e.g. the Esc that triggered the close) get swallowed instead of
 * leaking through to the focused app.
 */
void
comp_d3d11_window_set_input_suppress_grace_ms(struct comp_d3d11_window *window, uint32_t ms);

/*!
 * Phase 5.13: show a Win32 popup menu for a launcher tile right-click.
 * Must be called from the workspace's render thread — internally dispatches to
 * the window thread via SendMessage since TrackPopupMenu only works from
 * the thread that owns the target window.
 *
 * @return one of LAUNCHER_CTX_MENU_RESULT_* (see below), or 0 if no
 *         selection (cancel).
 */
#define LAUNCHER_CTX_MENU_RESULT_LAUNCH  1
#define LAUNCHER_CTX_MENU_RESULT_REMOVE  2
#define LAUNCHER_CTX_MENU_RESULT_REFRESH 3

uint32_t
comp_d3d11_window_show_launcher_context_menu(struct comp_d3d11_window *window);

/*!
 * Read and reset accumulated scroll wheel delta (for workspace window resize).
 *
 * @param window The window object
 * @return Accumulated scroll delta (positive = scroll up = enlarge), or 0
 */
int32_t
comp_d3d11_window_consume_scroll(struct comp_d3d11_window *window);

/*!
 * Set the desired cursor shape (called from compositor thread, applied on window thread).
 * IDs: 0=arrow, 1=sizewe, 2=sizens, 3=sizenwse, 4=sizenesw, 5=sizeall
 */
void
comp_d3d11_window_set_cursor(struct comp_d3d11_window *window, int cursor_id);

/*!
 * Set the workspace display processor for ESC/close handling.
 *
 * When the workspace window is closed (ESC or WM_CLOSE), this DP is switched
 * to 2D mode (lens off). Required because multi_compositor_render may not
 * run again after the last client disconnects.
 *
 * @param window The window object
 * @param dp     The workspace's display processor (opaque pointer)
 */
void
comp_d3d11_window_set_workspace_dp(struct comp_d3d11_window *window, void *dp);

/*!
 * Consume pending input events from the WndProc ring buffer.
 *
 * Called from the compositor/render thread to drain buffered input events
 * that the WndProc queued for capture clients (instead of PostMessage).
 *
 * @param window     The window object
 * @param out_events Array to receive events
 * @param max_events Maximum number of events to return
 * @return Number of events written to out_events
 */
uint32_t
comp_d3d11_window_consume_input_events(struct comp_d3d11_window *window,
                                       struct workspace_input_event *out_events,
                                       uint32_t max_events);

/*!
 * Phase 2.D: drain the public-event ring populated by WndProc for the
 * xrEnumerateWorkspaceInputEventsEXT path. SPSC; the service-side drain is
 * the sole consumer.
 *
 * @param window     The window object.
 * @param out_events Array to receive raw events.
 * @param max_events Maximum number of events to return.
 * @return Number of events written.
 */
uint32_t
comp_d3d11_window_consume_workspace_public_events(struct comp_d3d11_window *window,
                                                  struct workspace_public_event_raw *out_events,
                                                  uint32_t max_events);

/*!
 * Phase 2.D: set the pointer-capture flag honored by WndProc. While enabled
 * for a button, button-up events outside any window are still emitted to the
 * public-event ring (rather than being filtered as out-of-content).
 *
 * @param window  The window object.
 * @param enabled true to enable capture, false to disable.
 * @param button  Button index (1=L, 2=R, 3=M) when @p enabled is true.
 */
void
comp_d3d11_window_set_workspace_pointer_capture(struct comp_d3d11_window *window, bool enabled, uint32_t button);

/*!
 * Phase 2.K: read the current pointer-capture flag. Used by the service
 * compositor to suspend its built-in title-bar drag / resize / RMB rotation
 * while a workspace controller has taken over interactive policy. Lock-free
 * read of the same atomic the WndProc honours.
 */
bool
comp_d3d11_window_is_workspace_pointer_capture_enabled(struct comp_d3d11_window *window);

/*!
 * Request SetForegroundWindow on the window thread.
 *
 * SetForegroundWindow must be called from the thread that owns the current
 * foreground window. This posts the request to the window thread and waits
 * for completion. Used to give keyboard focus to off-screen capture client
 * HWNDs for SendInput dispatch.
 *
 * @param window      The window object
 * @param target_hwnd The HWND to make foreground (NULL to restore workspace window)
 */
void
comp_d3d11_window_request_foreground(struct comp_d3d11_window *window,
                                     void *target_hwnd);

/*!
 * Request the window thread to open a file dialog and launch an app.
 *
 * The file dialog runs on the window thread (modal, blocks message pump).
 * On success, launches the selected .exe with DISPLAYXR_WORKSPACE_SESSION=1
 * and XR_RUNTIME_JSON set. The app connects via IPC automatically.
 *
 * @param window The window object
 */
void
comp_d3d11_window_request_app_launch(struct comp_d3d11_window *window);

#ifdef __cplusplus
}
#endif
