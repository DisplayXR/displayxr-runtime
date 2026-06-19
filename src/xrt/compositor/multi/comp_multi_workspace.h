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

#ifdef __cplusplus
extern "C" {
#endif

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
