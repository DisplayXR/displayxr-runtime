// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window management.
 *
 * This module provides window creation for the D3D11 native compositor
 * when XR_DXR_win32_window_binding is NOT used. This allows apps like Blender
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

//! Max controller-supplied reserved-key chords (spec_version 24). Must match
//! XR_WORKSPACE_MAX_RESERVED_KEYS_DXR in the OpenXR extension header — kept as a
//! local define so this compositor header doesn't pull in the OpenXR headers.
#define WORKSPACE_RESERVED_KEYS_MAX 32

/*!
 * Phase 2.D: raw event captured by WndProc for the public-API event drain
 * (xrEnumerateWorkspaceInputEventsDXR). The service-side drain enriches each
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
 * The window is positioned at (@p screen_left, @p screen_top) in OS screen
 * coordinates — the display top-left that the vendor plug-in iface published
 * through `xrt_system_compositor_info.display_screen_left/top`. (0, 0) means
 * the primary monitor (sim_display default, or unknown panel).
 *
 * By default, the window starts in fullscreen mode unless the environment
 * variable XRT_COMPOSITOR_START_WINDOWED=1 is set.
 *
 * @param width        Requested window width
 * @param height       Requested window height
 * @param screen_left  Display top-left X in OS screen coords
 * @param screen_top   Display top-left Y in OS screen coords
 * @param out          Pointer to receive the created window handle
 *
 * @return XRT_SUCCESS on success, error code otherwise
 */
xrt_result_t
comp_d3d11_window_create(uint32_t width,
                         uint32_t height,
                         int32_t screen_left,
                         int32_t screen_top,
                         struct comp_d3d11_window **out);

/*!
 * As @ref comp_d3d11_window_create, but with control over the window's initial
 * visibility.
 *
 * @p start_hidden = true creates the window fully (thread, class, HWND,
 * fullscreen geometry, so the client rect is already native-res) but leaves it
 * HIDDEN. Used by the always-on service pipeline (#964): the service window
 * exists from the first client so the panel display processor and the render
 * thread have a home, but it is only SHOWN while it is the active presenter —
 * an app presenting into its own HWND must not have the service window on top
 * of it. Show/hide it later with @ref comp_d3d11_window_set_visible.
 *
 * @p start_hidden = false is identical to @ref comp_d3d11_window_create.
 */
xrt_result_t
comp_d3d11_window_create_ex(uint32_t width,
                            uint32_t height,
                            int32_t screen_left,
                            int32_t screen_top,
                            bool start_hidden,
                            struct comp_d3d11_window **out);

/*!
 * Show or hide the window asynchronously (#964).
 *
 * Uses ShowWindowAsync so the caller — typically the compositor render thread
 * holding render_mutex — never blocks on the window thread's message loop.
 * Idempotent from the OS's point of view; the caller tracks transitions.
 */
void
comp_d3d11_window_set_visible(struct comp_d3d11_window *window, bool visible);

/*!
 * Minimize the window (async) so it stays in the taskbar / Alt-Tab list.
 * Used instead of hiding while a hosted client still lives behind it.
 */
void
comp_d3d11_window_minimize(struct comp_d3d11_window *window);

/*!
 * Set the window's title bar / taskbar text (#1014).
 *
 * Used to name a hosted client's runtime-owned window after its application,
 * so its taskbar and Alt-Tab entry identify it. Safe to call from any thread:
 * the WM_SETTEXT is sent with a timeout, so a wedged window thread cannot
 * stall the caller.
 */
void
comp_d3d11_window_set_title(struct comp_d3d11_window *window, const char *title);

/*!
 * #1016: may this window feed the process-global qwerty state?
 *
 * `qwerty_process_win32` keeps ONE file-static state machine, so with a runtime
 * window per hosted client (#1014) every window's key and activation messages
 * landed in the same integrator — an unfocused window's WM_KILLFOCUS ran the
 * focus-loss reset while the user held W in the focused one. The service grants
 * this to exactly one window, the ACTIVE PRESENTER's, so input follows the
 * panel. Defaults to true; the legacy in-process paths never call it.
 */
void
comp_d3d11_window_set_qwerty_active(struct comp_d3d11_window *window, bool active);

/*!
 * Destroy the self-owned window.
 *
 * Posts the private WM_DXR_DESTROY_WINDOW message to the window thread and waits
 * for it to exit. MUST be called only after the compositor's display processor
 * has been destroyed (the SR weaver subclasses this HWND; destroying the window
 * while it is still attached re-enters the weaver and crashes). A user-initiated
 * WM_CLOSE (ESC / window X) does NOT destroy the window — it only sets
 * should_exit so the app tears down through this orderly path.
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
 * Phase-snap provider for user drags of the runtime-owned window (#625).
 *
 * All coordinates are absolute screen pixels (window outer rect top-left).
 * Returns true when a snapped position was produced in out_x/out_y; false
 * means "no snap available" and the caller keeps the proposed position
 * unchanged. A display processor with no interlace lattice (or none at all)
 * simply never produces a snap.
 */
typedef bool (*comp_d3d11_window_snap_fn)(void *userdata,
                                          int32_t origin_x,
                                          int32_t origin_y,
                                          int32_t target_x,
                                          int32_t target_y,
                                          int32_t *out_x,
                                          int32_t *out_y);

/*!
 * Install (or remove, fn == NULL) the drag phase-snap provider.
 *
 * The provider is called from the WINDOW THREAD inside WM_WINDOWPOSCHANGING
 * during a modal move/size loop, so it must be quick and must not block on the
 * compositor. The setter takes an exclusive lock that waits out any call in
 * flight — detach the provider BEFORE destroying whatever it calls into.
 *
 * @param window   The window object
 * @param fn       Snap callback, or NULL to detach
 * @param userdata Opaque pointer handed back to fn
 */
void
comp_d3d11_window_set_snap_provider(struct comp_d3d11_window *window,
                                    comp_d3d11_window_snap_fn fn,
                                    void *userdata);

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
 * Install the controller's reserved-key table (XR_DXR_spatial_workspace
 * spec_version 24). Each (vks[i], mods[i]) is a chord the controller owns:
 * matching key events are still emitted on the public ring but NOT forwarded
 * to the focused app. mods uses the 3-bit KEY-event mask (bit0=SHIFT,
 * bit1=CTRL, bit2=ALT); matching is exact on (vk, mods).
 *
 * Pass count == 0 (or NULL arrays) to restore the runtime's built-in default
 * reserved set. count is clamped to WORKSPACE_RESERVED_KEYS_MAX.
 *
 * Thread-safe: the service thread calls this; the WndProc thread reads the
 * table. The count is published last as the visibility barrier.
 */
void
comp_d3d11_window_set_reserved_keys(struct comp_d3d11_window *window,
                                    const uint32_t *vks,
                                    const uint32_t *mods,
                                    uint32_t count);

/*!
 * Return the HWND the most recent button-DOWN was forwarded to, or NULL if it
 * was not forwarded (a click on an unfocused window, where the cursor is
 * outside the focused window's rect). Captured at WndProc time, before the
 * workspace controller's async focus change can move the forward target — the
 * render-loop click handler uses it to decide whether to synthesize a DOWN to
 * the hit window for drag-in-one-click.
 */
void *
comp_d3d11_window_get_last_pointer_down_target(struct comp_d3d11_window *window);

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
 * Suppress or resume input forwarding. When suppressed, the WndProc does not
 * forward mouse or keyboard events to the focused app — the events still reach
 * the workspace controller via the public event ring. Driven by controller-
 * owned drag/resize gestures and by the modal input grab
 * (xrSetWorkspaceInputGrabDXR, spec_version 18).
 */
void
comp_d3d11_window_set_input_suppress(struct comp_d3d11_window *window, bool suppress);

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
 * #966: take the pending "panel should go flat" request left by WM_CLOSE.
 *
 * The window thread must never call the vendor display processor — with one DP
 * per panel (#964) the compositor's render thread is driving that same object
 * every frame, and the WndProc took no lock. So WM_CLOSE only sets a flag and
 * the render thread consumes it here, then applies the 2D transition through
 * its own queue.
 *
 * One-shot: returns true at most once per request.
 */
bool
comp_d3d11_window_take_close_request(struct comp_d3d11_window *window);

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
 * xrEnumerateWorkspaceInputEventsDXR path. SPSC; the service-side drain is
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


#ifdef __cplusplus
}
#endif
