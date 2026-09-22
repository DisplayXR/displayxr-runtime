// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The views the IPC server handed a client, kept for its own commit.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 *
 * #1674 — THE TWO AUTHORITIES.
 *
 * Under the D3D11 service a client's frame is composed from two different
 * sources of "where the camera is":
 *
 *  - its PROJECTION layers are an identity-MVP blit of pixels the client
 *    rendered with the views the SERVICE handed it at `xrLocateViews` —
 *    `ipc_try_get_sr_view_poses()` in ipc_server_handler.c, whose frustum is
 *    whatever rig that client is running (camera-centric, display-centric, a
 *    chained `XR_DXR_view_rig`, the workspace override);
 *  - its UI layers (quad / cylinder / equirect2) are PROJECTED by the service,
 *    which needs a camera of its own.
 *
 * `comp_layer_view_camera_select_eyes()` answers the second question from the
 * frame when it can — the app's own projection layer (branch a) or the frame
 * cameras the state tracker carries (branch b'). When it cannot, it synthesises
 * a DISPLAY-centric Kooima frustum from the DP eye and the client's window
 * metrics (branch b), which is right only when the client's rig IS that same
 * Kooima-from-window frustum. For any other rig the two authorities disagree
 * and the quad is composed through a frustum the client never rendered with.
 *
 * So the server records what it handed out, here, and the service's UI pass
 * consults it before it falls back to the synthesis. Deliberately a RECORD, not
 * a re-derivation: re-running the rig math in the compositor would be a second
 * implementation of the thing that must agree.
 *
 * WHAT IS STORED, AND IN WHICH FRAME. Exactly what the IPC reply carries: the
 * per-view FOV, the per-view pose HEAD-LOCAL (`T_head_view`, which is what
 * `ipc_try_get_sr_view_poses()` writes into `out_poses`), and the head pose the
 * server paired them with (`out_head_relation`, i.e. `T_root_head`). The lift
 * into the layer frame is the consumer's, and it is the SAME lift branch (b)
 * performs with `xrt_layer_frame_data::head_pose` (#1594) — storing an
 * already-lifted pose here would invite the #1613 double-apply the moment a
 * caller lifted it again.
 *
 * Header-only and device-free on purpose: the ring and its lookup are pure, so
 * `tests_comp_layer_view_camera` exercises them with no D3D11 device.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stddef.h> // NULL
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How many frames of handed-out views to keep.
 *
 * A locate and its `xrEndFrame` are one turn of the same client's IPC thread,
 * so one slot would nearly always do; the ring exists so an app that locates
 * several display times ahead (or locates twice for one frame) still finds its
 * own frame rather than a neighbour's.
 *
 * @ingroup comp_d3d11_service
 */
#define COMP_D3D11_SERVICE_LOCATED_VIEW_SLOTS 4

/*!
 * One frame's worth of views, exactly as the IPC server handed them out.
 *
 * @ingroup comp_d3d11_service
 */
struct comp_d3d11_service_located_views
{
	/*!
	 * The locate's display time, in the runtime's monotonic ns — the same
	 * clock `xrt_layer_frame_data::display_time_ns` is in, so a commit can
	 * ask for ITS frame rather than the newest one.
	 */
	int64_t display_time_ns;

	//! How many of @ref fovs / @ref poses are filled.
	uint32_t count;

	//! `T_root_head` the server paired these views with.
	struct xrt_pose head_pose;

	//! Per-view FOV, verbatim.
	struct xrt_fov fovs[XRT_MAX_VIEWS];

	//! Per-view pose, HEAD-LOCAL (`T_head_view`) — see the file comment.
	struct xrt_pose poses[XRT_MAX_VIEWS];
};

/*!
 * A client's last few handed-out view sets.
 *
 * Zero-initialised is empty and valid.
 *
 * @ingroup comp_d3d11_service
 */
struct comp_d3d11_service_located_view_ring
{
	struct comp_d3d11_service_located_views slots[COMP_D3D11_SERVICE_LOCATED_VIEW_SLOTS];

	//! Where the next record goes.
	uint32_t next;

	//! The slot the last record went into, valid only when @ref any is true.
	uint32_t latest;

	//! Has anything ever been recorded?
	bool any;
};

/*!
 * Record one frame's handed-out views.
 *
 * A record with no views, or with no ring, is dropped: an empty set would shadow
 * a usable earlier one for the rest of the session.
 *
 * @ingroup comp_d3d11_service
 */
static inline void
comp_d3d11_service_located_views_record(struct comp_d3d11_service_located_view_ring *ring,
                                        int64_t display_time_ns,
                                        uint32_t count,
                                        const struct xrt_pose *head_pose,
                                        const struct xrt_fov *fovs,
                                        const struct xrt_pose *poses)
{
	if (ring == NULL || fovs == NULL || poses == NULL || count == 0) {
		return;
	}
	if (count > XRT_MAX_VIEWS) {
		count = XRT_MAX_VIEWS;
	}

	const uint32_t slot = ring->next % COMP_D3D11_SERVICE_LOCATED_VIEW_SLOTS;
	struct comp_d3d11_service_located_views *out = &ring->slots[slot];

	out->display_time_ns = display_time_ns;
	out->count = count;
	if (head_pose != NULL) {
		out->head_pose = *head_pose;
	} else {
		// Spelt out rather than XRT_POSE_IDENTITY: this header is compiled
		// as C++ too, where the compound-literal cast is non-standard.
		out->head_pose.orientation.x = 0.0f;
		out->head_pose.orientation.y = 0.0f;
		out->head_pose.orientation.z = 0.0f;
		out->head_pose.orientation.w = 1.0f;
		out->head_pose.position.x = 0.0f;
		out->head_pose.position.y = 0.0f;
		out->head_pose.position.z = 0.0f;
	}
	for (uint32_t i = 0; i < count; i++) {
		out->fovs[i] = fovs[i];
		out->poses[i] = poses[i];
	}

	ring->latest = slot;
	ring->next = (slot + 1) % COMP_D3D11_SERVICE_LOCATED_VIEW_SLOTS;
	ring->any = true;
}

/*!
 * The views handed out for @p display_time_ns, or the newest set otherwise.
 *
 * The exact match is what makes this "the views THIS frame was rendered with".
 * The newest-set fallback covers a client whose `xrEndFrame` display time is not
 * bit-identical to the one it located with (nothing in OpenXR requires it to be)
 * — still the right rig, possibly one frame's worth of head motion stale, which
 * is categorically closer than a different frustum.
 *
 * @return NULL only when nothing has ever been recorded.
 *
 * @ingroup comp_d3d11_service
 */
static inline const struct comp_d3d11_service_located_views *
comp_d3d11_service_located_views_for_frame(const struct comp_d3d11_service_located_view_ring *ring,
                                           int64_t display_time_ns)
{
	if (ring == NULL || !ring->any) {
		return NULL;
	}

	for (uint32_t i = 0; i < COMP_D3D11_SERVICE_LOCATED_VIEW_SLOTS; i++) {
		const struct comp_d3d11_service_located_views *s = &ring->slots[i];
		if (s->count != 0 && s->display_time_ns == display_time_ns) {
			return s;
		}
	}

	const struct comp_d3d11_service_located_views *newest = &ring->slots[ring->latest];
	return newest->count != 0 ? newest : NULL;
}

#ifdef __cplusplus
}
#endif
