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

#ifdef __cplusplus
}
#endif
