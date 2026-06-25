// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic workspace-controller chrome registry (macOS shell Tier 1, #48).
 *
 * The Windows D3D11 service compositor stores controller-submitted chrome
 * swapchains + layouts per client slot inside its monolith
 * (`comp_d3d11_service.cpp`, `struct d3d11_multi_client_slot`) and composites
 * them in `multi_compositor_render()`. The macOS null+comp_multi service has no
 * such monolith — each content client renders out-of-process to its own target
 * in `render_session_to_own_target()`. This is the vendor-neutral analogue: a
 * small process-global, thread-safe table of controller-submitted chrome, keyed
 * by the *target* client's per-session compositor pointer (the `xrt_compositor *`
 * the IPC server stores as `ics->xc` — which is the very `multi_compositor` the
 * render path iterates). The chrome IPC handlers (writers) populate it; the
 * per-session render path (reader, same library) looks up chrome registered for
 * its own compositor and composites it over the client's content.
 *
 * Layering note: this stays free of `ipc_protocol.h` so the compositor never
 * depends on the IPC layer. The IPC handler translates its wire layout
 * (`ipc_workspace_chrome_layout`) into @ref comp_multi_chrome_layout when calling.
 *
 * @ingroup comp_multi
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xrt/xrt_defines.h" // struct xrt_pose

struct xrt_compositor;
struct xrt_swapchain;
struct xrt_system_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * #61 (macOS): mark the workspace controller as active/inactive on the shared
 * system compositor and wake its render thread. The macOS shared spatial surface
 * renders an empty backdrop + DXR splash + launcher band while a controller is
 * connected, even with no active content app session (a controller never
 * xrBeginSession's). Called from the IPC workspace activate / deactivate handlers.
 * No-op / unused on other platforms (defined under XRT_OS_MACOS).
 *
 * @ingroup comp_multi
 */
void
comp_multi_system_set_workspace_active(struct xrt_system_compositor *xsc, bool active);

/*!
 * Compositor-local mirror of the controller's chrome layout — the subset the
 * per-session composite path needs, decoupled from the IPC wire form.
 *
 * @ingroup comp_multi
 */
struct comp_multi_chrome_layout
{
	struct xrt_pose pose_in_client;     //!< Chrome quad pose in client-window-local space (meters).
	float size_w_m;                     //!< Chrome quad width in meters.
	float size_h_m;                     //!< Chrome quad height in meters.
	bool follows_window_orient;         //!< If true chrome rotates with window. (v1: ignored — axis-aligned.)
	float depth_bias_meters;            //!< Bias toward eye. (v1: ignored — flat post-weave overlay.)
	bool anchor_to_window_top_edge;     //!< pose.y is an offset from the window's top edge.
	float width_as_fraction_of_window;  //!< 0 = absolute size_w_m; > 0 = window_width * fraction.
};

/*!
 * Register (or replace) the chrome swapchain for @p target_xc. Takes a strong
 * reference to @p xsc (released on replace / unregister / clear). @p swapchain_id
 * is stored so @ref comp_multi_workspace_chrome_unregister_by_id can match it.
 * Thread-safe; called from a per-client IPC handler thread.
 */
void
comp_multi_workspace_chrome_register(struct xrt_compositor *target_xc,
                                     uint32_t swapchain_id,
                                     struct xrt_swapchain *xsc);

/*!
 * Drop the chrome entry whose stored swapchain id matches @p swapchain_id
 * (the controller destroyed the swapchain). Releases the strong reference.
 */
void
comp_multi_workspace_chrome_unregister_by_id(uint32_t swapchain_id);

/*!
 * Store/replace the layout for @p target_xc and mark its chrome valid. The
 * controller calls this right after registering the swapchain.
 */
void
comp_multi_workspace_chrome_set_layout(struct xrt_compositor *target_xc,
                                       const struct comp_multi_chrome_layout *layout);

/*!
 * Per-frame pose-only update of an existing entry's `pose_in_client`. Does not
 * toggle validity; no-op if @p target_xc has no entry yet.
 */
void
comp_multi_workspace_chrome_update_pose(struct xrt_compositor *target_xc, const struct xrt_pose *pose_in_client);

/*!
 * Render-side lookup. If @p target_xc has a valid chrome registration, sets
 * @p out_xsc to the (registry-owned strong-ref) swapchain and copies the layout
 * into @p out_layout, returning true. The returned swapchain stays alive for the
 * frame by virtue of the registry's reference. Returns false otherwise.
 */
bool
comp_multi_workspace_chrome_get(struct xrt_compositor *target_xc,
                                struct xrt_swapchain **out_xsc,
                                struct comp_multi_chrome_layout *out_layout);

/*!
 * Drop all chrome state for @p target_xc (call on client disconnect so a reused
 * compositor pointer never inherits stale chrome). Releases the strong reference.
 */
void
comp_multi_workspace_chrome_clear(struct xrt_compositor *target_xc);

/*!
 * Set the minimized/hidden state for a managed content client (#61). When hidden,
 * the macOS per-session render path renders a black "desktop canvas" plus the
 * workspace overlays/cursor instead of the client's content, so the taskbar stays
 * visible while the app is minimized. Uses find-or-add: may be called before chrome
 * is registered for @p target_xc.
 */
void
comp_multi_workspace_set_window_visibility(struct xrt_compositor *target_xc, bool visible);

/*!
 * Query whether @p target_xc is currently hidden/minimized (#61). Returns false if
 * no state is registered for the client.
 */
bool
comp_multi_workspace_is_window_hidden(struct xrt_compositor *target_xc);

/*!
 * Window-dims query for the macOS workspace get_window_pose IPC handler. In the
 * single-app OOP model the client fills the display, so this returns the display
 * dims as the client's window size and an identity pose. Returns false if @p xc
 * is not a multi_compositor or display dims are unavailable. Defined in
 * comp_multi_system.c (alongside the chrome composite that uses the same dims).
 */
bool
comp_multi_workspace_get_client_window_dims(struct xrt_compositor *xc,
                                            struct xrt_pose *out_pose,
                                            float *out_w_m,
                                            float *out_h_m);

/*!
 * Tier-2 window placement (#59). Set the per-client window pose: reposition and
 * resize the client's runtime-owned NSWindow into a display sub-rect so multiple
 * apps tile instead of stacking full-screen. Backs the macOS
 * xrSetWorkspaceClientWindowPoseEXT handler. Computes the pixel rect from @p pose
 * (display-center origin, meters, +y up) using the display's pixel density,
 * stores it (so get_window_dims echoes it back to the controller and the cursor
 * composite can map global cursor px into the window), repositions the NSWindow
 * on the main thread and flags the per-session swapchain for recreation at the
 * new size. Defined in comp_multi_system.c (needs session_render + comp_window_macos).
 * Returns false if @p xc is not a placeable per-session client.
 */
bool
comp_multi_workspace_set_client_window_pose(struct xrt_compositor *xc,
                                            const struct xrt_pose *pose,
                                            float width_m,
                                            float height_m);

/*!
 * Store the per-client window placement in the registry (pose + meters + the
 * derived top-left-origin display-pixel rect). Called by
 * @ref comp_multi_workspace_set_client_window_pose. Find-or-add: a client may be
 * posed before chrome is registered.
 */
void
comp_multi_workspace_store_window_pose(struct xrt_compositor *target_xc,
                                       const struct xrt_pose *pose,
                                       float width_m,
                                       float height_m,
                                       int32_t px_x,
                                       int32_t px_y,
                                       int32_t px_w,
                                       int32_t px_h);

/*!
 * Per-client style pushed by the workspace controller via xrSetWorkspaceClientStyleEXT
 * (#59 Task 10). The compositor applies the focus tint to the focused client's
 * content edge — the macOS analogue of the D3D11 service's per-slot style. The
 * controller toggles @ref focus_glow_intensity by focus state (0 = unfocused).
 */
struct comp_multi_client_style
{
	bool valid;
	float corner_radius;        //!< Fraction of window height; 0 = use compositor default.
	float edge_feather_meters;  //!< Soft alpha falloff width in meters; 0 = use compositor default.
	float focus_glow_color[4];  //!< RGBA tint (applied when intensity > 0).
	float focus_glow_intensity; //!< Multiplier on color.a; 0 = no tint (unfocused / disabled).
};

/*!
 * Store the controller-pushed per-client style for @p target_xc (find-or-add).
 * Read back by the content compositing loop via @ref comp_multi_workspace_get_client_style.
 */
void
comp_multi_workspace_set_client_style(struct xrt_compositor *target_xc,
                                      const struct comp_multi_client_style *style);

/*!
 * Track which client currently holds workspace focus (#59 Task 10). @p target_xc
 * == NULL clears focus. The compositor gates the focus tint on this, mirroring the
 * D3D11 service's focused_slot — the controller pushes a glow style for every
 * windowed client, but only the focused one is tinted.
 */
void
comp_multi_workspace_set_focused_client(struct xrt_compositor *target_xc);

//! True if @p target_xc is the currently focused client.
bool
comp_multi_workspace_is_focused(struct xrt_compositor *target_xc);

/*!
 * Load the stored per-client style. Returns false (and leaves @p out_style
 * untouched) if no style has been pushed for @p target_xc.
 */
bool
comp_multi_workspace_get_client_style(struct xrt_compositor *target_xc,
                                      struct comp_multi_client_style *out_style);

/*!
 * Ask a managed content client to exit (macOS chrome close button / DELETE key,
 * #59). Pushes XRT_SESSION_EVENT_EXIT_REQUEST to the client's per-session
 * compositor; its xrPollEvent surfaces XR_SESSION_STATE_EXITING and the app
 * leaves its frame loop cleanly. Returns false if @p target_xc is not a
 * placeable per-session client. Defined in comp_multi_system.c.
 */
bool
comp_multi_workspace_request_client_exit(struct xrt_compositor *target_xc);

/*!
 * Load a previously stored window pose/dims. Returns false if @p target_xc has
 * not been placed yet (no set_window_pose has arrived).
 */
bool
comp_multi_workspace_load_window_pose(struct xrt_compositor *target_xc,
                                      struct xrt_pose *out_pose,
                                      float *out_w_m,
                                      float *out_h_m);

/*!
 * Load the stored top-left-origin display-pixel rect for @p target_xc (the
 * NSWindow's position on the display). The cursor composite uses it to convert
 * the global cursor px into window-local px. Returns false if not placed.
 */
bool
comp_multi_workspace_load_window_px_rect(struct xrt_compositor *target_xc,
                                         int32_t *out_x,
                                         int32_t *out_y,
                                         int32_t *out_w,
                                         int32_t *out_h);

/*!
 * #61 input routing: hit-test a display-pixel cursor position (top-left origin)
 * against the placed window rects. Returns the target compositor of the topmost
 * (nearest-to-viewer, largest pose.z) placed, visible window whose pixel rect
 * contains @p x,@p y — or NULL if the cursor is over no window (chrome floats
 * above the top edge, so it falls outside every content rect → NULL → controller).
 * The macOS AppKit router uses this to forward content pointer/scroll/motion to
 * the right client while routing workspace input to the controller.
 */
struct xrt_compositor *
comp_multi_workspace_hit_test_window_px(int32_t x, int32_t y);

/*!
 * #61 input routing: the content client currently holding workspace focus (the
 * value last set via @ref comp_multi_workspace_set_focused_client), or NULL. The
 * AppKit router forwards content keystrokes to this client (keyboard follows
 * focus, not the cursor).
 */
struct xrt_compositor *
comp_multi_workspace_get_focused_client(void);


/*
 *
 * Session-global cursor (#48 Phase 2, spec_version 13).
 *
 */

/*!
 * Session-global cursor sprite, set by the controller via xrSetWorkspaceCursorEXT.
 * Composited topmost over the content. v1 is flat at the screen plane (the depth
 * fields are stored for the deferred per-eye-disparity Tier-2 work; only @ref
 * over_window dimming is applied in v1).
 *
 * @ingroup comp_multi
 */
struct comp_multi_cursor_state
{
	float hot_x;       //!< Sprite UV X [0,1] of the click point.
	float hot_y;       //!< Sprite UV Y [0,1].
	float size_meters; //!< Physical size (width = height).
	bool visible;      //!< False = hidden even if a swapchain is set.
	float hit_z_m;     //!< Hit depth for disparity (v1: stored, unused).
	bool over_window;  //!< Dim the cursor when over a window.
};

//! Set/replace the cursor sprite swapchain (strong ref; NULL or !visible = hide).
void
comp_multi_workspace_set_cursor(struct xrt_swapchain *xsc, float hot_x, float hot_y, float size_meters, bool visible);

//! Update the cursor's hit depth + over-window state (xrSetWorkspaceCursorDepthEXT).
void
comp_multi_workspace_set_cursor_depth(float hit_z_m, bool over_window);

//! Render-side lookup. Returns true + the (registry-owned) sprite swapchain + state if visible.
bool
comp_multi_workspace_get_cursor(struct xrt_swapchain **out_xsc, struct comp_multi_cursor_state *out);

//! Latest pointer position in target framebuffer pixels (top-left origin). The
//! macOS AppKit pump writes it on mouse move; the cursor composite reads it.
void
comp_multi_workspace_set_pointer_px(int32_t x, int32_t y);
void
comp_multi_workspace_get_pointer_px(int32_t *out_x, int32_t *out_y);


/*
 *
 * Session-global overlays (#48 Phase 2, spec_version 17/21).
 *
 */

//! Max concurrent keyed overlays (taskbar/toast/launcher); matches the D3D11 cap.
#define COMP_MULTI_WORKSPACE_MAX_OVERLAYS 16

/*!
 * One controller overlay (e.g. taskbar), docked at a normalized display anchor.
 * Composited at z = 0 (zero disparity) — flat by design, so v1 is fully correct.
 *
 * @ingroup comp_multi
 */
struct comp_multi_overlay_state
{
	uint32_t overlay_id;          //!< Keyed-map slot.
	float anchor_x, anchor_y;     //!< Normalized display dock point [0,1].
	float pivot_x, pivot_y;       //!< Normalized sprite UV mapped onto the anchor.
	float size_w_m, size_h_m;     //!< Physical overlay extent in meters.
	bool stereo_sbs;              //!< Side-by-side stereo overlay (v1: left half only).
};

//! Upsert (or, if !visible / NULL xsc, remove) the overlay keyed by @p overlay_id.
void
comp_multi_workspace_set_overlay(uint32_t overlay_id,
                                 struct xrt_swapchain *xsc,
                                 float anchor_x,
                                 float anchor_y,
                                 float pivot_x,
                                 float pivot_y,
                                 float size_w_m,
                                 float size_h_m,
                                 bool visible,
                                 bool stereo_sbs);

/*!
 * Copy the current overlays (id-ascending) into the caller's parallel arrays for
 * the render path. Returns the count written (<= @p max). The returned swapchains
 * stay alive for the frame via the registry's references.
 */
uint32_t
comp_multi_workspace_copy_overlays(struct comp_multi_overlay_state *out_states,
                                   struct xrt_swapchain **out_xscs,
                                   uint32_t max);

#ifdef __cplusplus
}
#endif
