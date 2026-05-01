// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client side wrapper of compositor.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_config_os.h"


#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_wait.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"
#include "util/u_limited_unique_id.h"

#include "shared/ipc_protocol.h"
#include "client/ipc_client.h"
#include "ipc_client_generated.h"

// Phase 2.D: the workspace input-event bridge translates wire events into
// the public extension struct so the state tracker can pass its public-API
// array through without seeing IPC types. Pulling the public header here
// is fine — it lives outside the IPC layer's coupling concerns.
#include <openxr/XR_EXT_spatial_workspace.h>

#include <string.h>
#include <stdio.h>
#if !defined(XRT_OS_WINDOWS)
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <errno.h>
#include <assert.h>

#ifdef XRT_GRAPHICS_SYNC_HANDLE_IS_FD
#include <unistd.h>
#endif


/*
 *
 * Internal structs and helpers.
 *
 */

//! Define to test the loopback allocator.
#undef IPC_USE_LOOPBACK_IMAGE_ALLOCATOR

/*!
 * Client proxy for an xrt_compositor_native implementation over IPC.
 * @implements xrt_compositor_native
 */
struct ipc_client_compositor
{
	struct xrt_compositor_native base;

	//! Should be turned into its own object.
	struct xrt_system_compositor system;

	struct ipc_connection *ipc_c;

	//! Optional image allocator.
	struct xrt_image_native_allocator *xina;

	struct
	{
		//! Id that we are currently using for submitting layers.
		uint32_t slot_id;

		uint32_t layer_count;
	} layers;

	//! Has the native compositor been created, only supports one for now.
	bool compositor_created;

	//! Initial visibility state returned from server (avoids race condition)
	bool initial_visible;
	//! Initial focus state returned from server (avoids race condition)
	bool initial_focused;

	//! To get better wake up in wait frame.
	struct os_precise_sleeper sleeper;

#ifdef IPC_USE_LOOPBACK_IMAGE_ALLOCATOR
	//! To test image allocator.
	struct xrt_image_native_allocator loopback_xina;
#endif
};

/*!
 * Client proxy for an xrt_swapchain_native implementation over IPC.
 * @implements xrt_swapchain_native
 */
struct ipc_client_swapchain
{
	struct xrt_swapchain_native base;

	struct ipc_client_compositor *icc;

	uint32_t id;
};

/*!
 * Client proxy for an xrt_compositor_semaphore implementation over IPC.
 * @implements xrt_compositor_semaphore
 */
struct ipc_client_compositor_semaphore
{
	struct xrt_compositor_semaphore base;

	struct ipc_client_compositor *icc;

	uint32_t id;
};


/*
 *
 * Helper functions.
 *
 */

static inline struct ipc_client_compositor *
ipc_client_compositor(struct xrt_compositor *xc)
{
	return (struct ipc_client_compositor *)xc;
}

static inline struct ipc_client_swapchain *
ipc_client_swapchain(struct xrt_swapchain *xs)
{
	return (struct ipc_client_swapchain *)xs;
}

static inline struct ipc_client_compositor_semaphore *
ipc_client_compositor_semaphore(struct xrt_compositor_semaphore *xcsem)
{
	return (struct ipc_client_compositor_semaphore *)xcsem;
}


/*
 *
 * Misc functions
 *
 */

static xrt_result_t
get_info(struct xrt_compositor *xc, struct xrt_compositor_info *out_info)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	xrt_result_t xret = ipc_call_compositor_get_info(icc->ipc_c, out_info);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_get_info");
}

static xrt_result_t
get_system_info(struct ipc_client_compositor *icc, struct xrt_system_compositor_info *out_info)
{
	xrt_result_t xret = ipc_call_system_compositor_get_info(icc->ipc_c, out_info);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_system_compositor_get_info");
}

/*!
 * Pull per-client window metrics over IPC.
 *
 * Used by the OpenXR state tracker for IPC-client sessions (Chrome WebXR,
 * any sandboxed app) to drive window-scoped Kooima FOV in workspace mode.
 * Returns false if the round trip fails or the server reports no valid
 * per-client slot (headless, workspace mode off, pre-render race) — caller
 * then falls back to display-dimension metrics.
 *
 * Caller MUST only invoke this on a compositor that is actually an
 * `ipc_client_compositor`. The OpenXR state tracker gates on
 * !is_*_native_compositor && xmcc==NULL, which is the only case where
 * `xc` is an IPC client compositor.
 */
bool
comp_ipc_client_compositor_get_window_metrics(struct xrt_compositor *xc, struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		return false;
	}

	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return false;
	}

	xrt_result_t xret = ipc_call_compositor_get_window_metrics(icc->ipc_c, out_metrics);
	if (xret != XRT_SUCCESS) {
		return false;
	}

	return out_metrics->valid;
}

/*
 * Workspace controller bridges — thin accessors used by the OpenXR state
 * tracker's XR_EXT_spatial_workspace implementation. The state tracker does
 * not pull ipc_client headers; it forward-declares these wrappers and the
 * runtime DLL resolves the symbols at link time. Each one extracts the
 * underlying ipc_connection and dispatches the matching generated RPC.
 *
 * Caller is responsible for only invoking these on an ipc_client_compositor
 * (same gate as comp_ipc_client_compositor_get_window_metrics).
 */
xrt_result_t
comp_ipc_client_compositor_workspace_activate(struct xrt_compositor *xc)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_activate(icc->ipc_c);
}

xrt_result_t
comp_ipc_client_compositor_workspace_deactivate(struct xrt_compositor *xc)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_deactivate(icc->ipc_c);
}

xrt_result_t
comp_ipc_client_compositor_workspace_get_state(struct xrt_compositor *xc, bool *out_active)
{
	if (xc == NULL || out_active == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_get_state(icc->ipc_c, out_active);
}

xrt_result_t
comp_ipc_client_compositor_workspace_add_capture_client(struct xrt_compositor *xc,
                                                        uint64_t hwnd,
                                                        uint32_t *out_client_id)
{
	if (xc == NULL || out_client_id == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_add_capture_client(icc->ipc_c, hwnd, out_client_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_remove_capture_client(struct xrt_compositor *xc, uint32_t client_id)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_remove_capture_client(icc->ipc_c, client_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_set_window_pose(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     const struct xrt_pose *pose,
                                                     float width_m,
                                                     float height_m)
{
	if (xc == NULL || pose == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_set_window_pose(icc->ipc_c, client_id, pose, width_m, height_m);
}

xrt_result_t
comp_ipc_client_compositor_workspace_get_window_pose(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     struct xrt_pose *out_pose,
                                                     float *out_width_m,
                                                     float *out_height_m)
{
	if (xc == NULL || out_pose == NULL || out_width_m == NULL || out_height_m == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_get_window_pose(icc->ipc_c, client_id, out_pose, out_width_m, out_height_m);
}

xrt_result_t
comp_ipc_client_compositor_workspace_set_window_visibility(struct xrt_compositor *xc,
                                                           uint32_t client_id,
                                                           bool visible)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_set_window_visibility(icc->ipc_c, client_id, visible);
}

xrt_result_t
comp_ipc_client_compositor_workspace_hit_test(struct xrt_compositor *xc,
                                              int32_t cursor_x,
                                              int32_t cursor_y,
                                              uint32_t *out_client_id,
                                              float *out_local_u,
                                              float *out_local_v,
                                              uint32_t *out_hit_region)
{
	if (xc == NULL || out_client_id == NULL || out_local_u == NULL || out_local_v == NULL ||
	    out_hit_region == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_hit_test(icc->ipc_c, cursor_x, cursor_y, out_client_id, out_local_u,
	                                   out_local_v, out_hit_region);
}

xrt_result_t
comp_ipc_client_compositor_workspace_set_focused_client(struct xrt_compositor *xc, uint32_t client_id)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_set_focused_client(icc->ipc_c, client_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_get_focused_client(struct xrt_compositor *xc, uint32_t *out_client_id)
{
	if (xc == NULL || out_client_id == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_get_focused_client(icc->ipc_c, out_client_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_enumerate_input_events(struct xrt_compositor *xc,
                                                            uint32_t requested_capacity,
                                                            uint32_t *out_count,
                                                            void *out_events_buf,
                                                            size_t event_stride,
                                                            size_t event_buf_capacity)
{
	if (xc == NULL || out_count == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_count = 0;
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// When the caller has no buffer space we ask the server for zero
	// events so the (destructive) drain doesn't lose anything. capacity=0
	// is the only valid count-query because drained events are not
	// returned by a subsequent call.
	uint32_t cap = requested_capacity;
	if (out_events_buf == NULL || event_buf_capacity == 0 || event_stride == 0) {
		cap = 0;
	}
	if (cap > event_buf_capacity) {
		cap = (uint32_t)event_buf_capacity;
	}
	if (cap > IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX) {
		cap = IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX;
	}

	struct ipc_workspace_input_event_batch batch = {0};
	xrt_result_t xret = ipc_call_workspace_enumerate_input_events(icc->ipc_c, cap, &batch);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (cap == 0 || out_events_buf == NULL) {
		*out_count = 0;
		return XRT_SUCCESS;
	}

	// Translate wire events → public XrWorkspaceInputEventEXT records.
	// The caller's stride lets the bridge stay agnostic to the public
	// struct's exact layout — but we know what fields it carries because
	// the public header is part of the runtime ABI we own.
	uint32_t copy = batch.count;
	if (copy > cap) {
		copy = cap;
	}
	uint8_t *dst = (uint8_t *)out_events_buf;
	for (uint32_t i = 0; i < copy; i++) {
		const struct ipc_workspace_input_event *src = &batch.events[i];
		XrWorkspaceInputEventEXT *out =
		    (XrWorkspaceInputEventEXT *)(dst + (size_t)i * event_stride);
		memset(out, 0, sizeof(*out));
		out->eventType = (XrWorkspaceInputEventTypeEXT)src->event_type;
		out->timestampMs = src->timestamp_ms;
		switch (src->event_type) {
		case IPC_WORKSPACE_INPUT_EVENT_POINTER:
			out->pointer.hitClientId = (XrWorkspaceClientId)src->u.pointer.hit_client_id;
			out->pointer.hitRegion = (XrWorkspaceHitRegionEXT)src->u.pointer.hit_region;
			out->pointer.localUV.x = src->u.pointer.local_u;
			out->pointer.localUV.y = src->u.pointer.local_v;
			out->pointer.cursorX = (int32_t)src->u.pointer.cursor_x;
			out->pointer.cursorY = (int32_t)src->u.pointer.cursor_y;
			out->pointer.button = src->u.pointer.button;
			out->pointer.isDown = src->u.pointer.is_down ? XR_TRUE : XR_FALSE;
			out->pointer.modifiers = src->u.pointer.modifiers;
			out->pointer.chromeRegionId = (XrWorkspaceChromeRegionIdEXT)src->u.pointer.chrome_region_id;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_POINTER_HOVER:
			out->pointerHover.prevClientId =
			    (XrWorkspaceClientId)src->u.pointer_hover.prev_client_id;
			out->pointerHover.prevRegion =
			    (XrWorkspaceHitRegionEXT)src->u.pointer_hover.prev_region;
			out->pointerHover.currentClientId =
			    (XrWorkspaceClientId)src->u.pointer_hover.curr_client_id;
			out->pointerHover.currentRegion =
			    (XrWorkspaceHitRegionEXT)src->u.pointer_hover.curr_region;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_KEY:
			out->key.vkCode = src->u.key.vk_code;
			out->key.isDown = src->u.key.is_down ? XR_TRUE : XR_FALSE;
			out->key.modifiers = src->u.key.modifiers;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_SCROLL:
			out->scroll.deltaY = src->u.scroll.delta_y;
			out->scroll.cursorX = (int32_t)src->u.scroll.cursor_x;
			out->scroll.cursorY = (int32_t)src->u.scroll.cursor_y;
			out->scroll.modifiers = src->u.scroll.modifiers;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION:
			out->pointerMotion.hitClientId =
			    (XrWorkspaceClientId)src->u.pointer_motion.hit_client_id;
			out->pointerMotion.hitRegion =
			    (XrWorkspaceHitRegionEXT)src->u.pointer_motion.hit_region;
			out->pointerMotion.localUV.x = src->u.pointer_motion.local_u;
			out->pointerMotion.localUV.y = src->u.pointer_motion.local_v;
			out->pointerMotion.cursorX = (int32_t)src->u.pointer_motion.cursor_x;
			out->pointerMotion.cursorY = (int32_t)src->u.pointer_motion.cursor_y;
			out->pointerMotion.buttonMask = src->u.pointer_motion.button_mask;
			out->pointerMotion.modifiers = src->u.pointer_motion.modifiers;
			out->pointerMotion.chromeRegionId = (XrWorkspaceChromeRegionIdEXT)src->u.pointer_motion.chrome_region_id;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK:
			out->frameTick.timestampNs = src->u.frame_tick.timestamp_ns;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED:
			out->focusChanged.prevClientId =
			    (XrWorkspaceClientId)src->u.focus_changed.prev_client_id;
			out->focusChanged.currentClientId =
			    (XrWorkspaceClientId)src->u.focus_changed.curr_client_id;
			break;
		case IPC_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED:
			out->windowPoseChanged.clientId = (XrWorkspaceClientId)src->u.window_pose_changed.client_id;
			out->windowPoseChanged.pose.orientation.x = src->u.window_pose_changed.pose_orient_x;
			out->windowPoseChanged.pose.orientation.y = src->u.window_pose_changed.pose_orient_y;
			out->windowPoseChanged.pose.orientation.z = src->u.window_pose_changed.pose_orient_z;
			out->windowPoseChanged.pose.orientation.w = src->u.window_pose_changed.pose_orient_w;
			out->windowPoseChanged.pose.position.x = src->u.window_pose_changed.pose_pos_x;
			out->windowPoseChanged.pose.position.y = src->u.window_pose_changed.pose_pos_y;
			out->windowPoseChanged.pose.position.z = src->u.window_pose_changed.pose_pos_z;
			out->windowPoseChanged.widthMeters = src->u.window_pose_changed.width_m;
			out->windowPoseChanged.heightMeters = src->u.window_pose_changed.height_m;
			break;
		default:
			break;
		}
	}
	*out_count = copy;
	return XRT_SUCCESS;
}

xrt_result_t
comp_ipc_client_compositor_workspace_pointer_capture_set(struct xrt_compositor *xc,
                                                         bool enabled,
                                                         uint32_t button)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_pointer_capture_set(icc->ipc_c, enabled, button);
}

xrt_result_t
comp_ipc_client_compositor_workspace_capture_frame(struct xrt_compositor *xc,
                                                   const char *path_prefix,
                                                   uint32_t flags,
                                                   uint64_t *out_timestamp_ns,
                                                   uint32_t *out_atlas_w,
                                                   uint32_t *out_atlas_h,
                                                   uint32_t *out_eye_w,
                                                   uint32_t *out_eye_h,
                                                   uint32_t *out_views_written,
                                                   uint32_t *out_tile_columns,
                                                   uint32_t *out_tile_rows,
                                                   float *out_display_w_m,
                                                   float *out_display_h_m,
                                                   float out_eye_left_m[3],
                                                   float out_eye_right_m[3])
{
	if (xc == NULL || path_prefix == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	struct ipc_capture_request req = {0};
	size_t plen = strlen(path_prefix);
	if (plen >= sizeof(req.path_prefix)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	memcpy(req.path_prefix, path_prefix, plen);
	req.path_prefix[plen] = '\0';
	req.flags = flags;

	struct ipc_capture_result result = {0};
	xrt_result_t xret = ipc_call_workspace_capture_frame(icc->ipc_c, &req, &result);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (out_timestamp_ns)   *out_timestamp_ns   = result.timestamp_ns;
	if (out_atlas_w)        *out_atlas_w        = result.atlas_width;
	if (out_atlas_h)        *out_atlas_h        = result.atlas_height;
	if (out_eye_w)          *out_eye_w          = result.eye_width;
	if (out_eye_h)          *out_eye_h          = result.eye_height;
	if (out_views_written)  *out_views_written  = result.views_written;
	if (out_tile_columns)   *out_tile_columns   = result.tile_columns;
	if (out_tile_rows)      *out_tile_rows      = result.tile_rows;
	if (out_display_w_m)    *out_display_w_m    = result.display_width_m;
	if (out_display_h_m)    *out_display_h_m    = result.display_height_m;
	if (out_eye_left_m) {
		out_eye_left_m[0] = result.eye_left_m[0];
		out_eye_left_m[1] = result.eye_left_m[1];
		out_eye_left_m[2] = result.eye_left_m[2];
	}
	if (out_eye_right_m) {
		out_eye_right_m[0] = result.eye_right_m[0];
		out_eye_right_m[1] = result.eye_right_m[1];
		out_eye_right_m[2] = result.eye_right_m[2];
	}
	return XRT_SUCCESS;
}

xrt_result_t
comp_ipc_client_compositor_workspace_enumerate_clients(struct xrt_compositor *xc,
                                                       uint32_t capacity,
                                                       uint32_t *out_count,
                                                       uint32_t *out_ids)
{
	if (xc == NULL || out_count == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_count = 0;
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	struct ipc_client_list list = {0};
	xrt_result_t xret = ipc_call_workspace_enumerate_clients(icc->ipc_c, &list);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	uint32_t copy = list.id_count;
	if (out_ids != NULL && capacity > 0) {
		if (copy > capacity) {
			copy = capacity;
		}
		for (uint32_t i = 0; i < copy; i++) {
			out_ids[i] = list.ids[i];
		}
	} else {
		copy = 0;
	}
	*out_count = (out_ids == NULL || capacity == 0) ? list.id_count : copy;
	return XRT_SUCCESS;
}

xrt_result_t
comp_ipc_client_compositor_workspace_get_client_info(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     char *out_name,
                                                     size_t name_capacity,
                                                     uint64_t *out_pid,
                                                     uint32_t *out_z_order,
                                                     bool *out_is_focused,
                                                     bool *out_is_visible)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	struct ipc_app_state state = {0};
	xrt_result_t xret = ipc_call_workspace_get_client_info(icc->ipc_c, client_id, &state);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (out_name != NULL && name_capacity > 0) {
		size_t copy = name_capacity - 1;
		if (copy > sizeof(state.info.application_name) - 1) {
			copy = sizeof(state.info.application_name) - 1;
		}
		memcpy(out_name, state.info.application_name, copy);
		out_name[copy] = '\0';
	}
	if (out_pid)        *out_pid        = (uint64_t)state.pid;
	if (out_z_order)    *out_z_order    = state.z_order;
	if (out_is_focused) *out_is_focused = state.session_focused;
	if (out_is_visible) *out_is_visible = state.session_visible;
	return XRT_SUCCESS;
}

xrt_result_t
comp_ipc_client_compositor_workspace_request_client_exit(struct xrt_compositor *xc, uint32_t client_id)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_request_client_exit(icc->ipc_c, client_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_request_client_fullscreen(struct xrt_compositor *xc,
                                                               uint32_t client_id,
                                                               bool fullscreen)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_request_client_fullscreen(icc->ipc_c, client_id, fullscreen);
}

/*
 * Phase 2.C: chrome swapchain register/unregister + layout setter.
 */
xrt_result_t
comp_ipc_client_compositor_workspace_register_chrome_swapchain(struct xrt_compositor *xc,
                                                               uint32_t client_id,
                                                               uint32_t swapchain_id)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_register_chrome_swapchain(icc->ipc_c, client_id, swapchain_id);
}

xrt_result_t
comp_ipc_client_compositor_workspace_unregister_chrome_swapchain(struct xrt_compositor *xc,
                                                                 uint32_t swapchain_id)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_unregister_chrome_swapchain(icc->ipc_c, swapchain_id);
}

/*!
 * Phase 2.C: helper to extract the IPC swapchain id from an xrt_swapchain.
 * Used by the state-tracker dispatch wrappers to register/unregister chrome
 * swapchains without pulling IPC headers into oxr_workspace.c.
 */
uint32_t
comp_ipc_client_compositor_get_swapchain_id(struct xrt_swapchain *xsc)
{
	if (xsc == NULL) {
		return 0;
	}
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);
	return ics->id;
}

xrt_result_t
comp_ipc_client_compositor_workspace_set_chrome_layout(struct xrt_compositor *xc,
                                                       uint32_t client_id,
                                                       const struct ipc_workspace_chrome_layout *layout)
{
	if (xc == NULL || layout == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_workspace_set_chrome_layout(icc->ipc_c, client_id, layout);
}

xrt_result_t
comp_ipc_client_compositor_workspace_acquire_wakeup_event(struct xrt_compositor *xc,
                                                          xrt_graphics_sync_handle_t *out_handle)
{
	if (xc == NULL || out_handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;

	xrt_graphics_sync_handle_t handles[1] = {XRT_GRAPHICS_SYNC_HANDLE_INVALID};
	xrt_result_t xret = ipc_call_workspace_acquire_wakeup_event(icc->ipc_c, handles, 1);
	if (xret == XRT_SUCCESS) {
		*out_handle = handles[0];
	}
	return xret;
}

/*
 * Launcher bridges (XR_EXT_app_launcher).
 *
 * Same gating contract as the workspace_* family: only valid when `xc` is
 * an ipc_client_compositor. The OpenXR state tracker forward-declares
 * these and the runtime DLL resolves them at link time.
 */
xrt_result_t
comp_ipc_client_compositor_launcher_clear_apps(struct xrt_compositor *xc)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_launcher_clear_apps(icc->ipc_c);
}

xrt_result_t
comp_ipc_client_compositor_launcher_add_app(struct xrt_compositor *xc,
                                            const char *name,
                                            const char *icon_path,
                                            const char *app_type,
                                            const char *icon_3d_path,
                                            const char *icon_3d_layout)
{
	if (xc == NULL || name == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	// State tracker stays free of ipc_launcher_app — bridge fills the wire
	// struct from the primitive parameters. exe_path is intentionally not
	// exposed at the public surface; the runtime never launches binaries.
	struct ipc_launcher_app app = {0};
	snprintf(app.name, sizeof(app.name), "%s", name);
	if (icon_path != NULL) {
		snprintf(app.icon_path, sizeof(app.icon_path), "%s", icon_path);
	}
	if (app_type != NULL) {
		snprintf(app.type, sizeof(app.type), "%s", app_type);
	}
	if (icon_3d_path != NULL) {
		snprintf(app.icon_3d_path, sizeof(app.icon_3d_path), "%s", icon_3d_path);
	}
	if (icon_3d_layout != NULL) {
		snprintf(app.icon_3d_layout, sizeof(app.icon_3d_layout), "%s", icon_3d_layout);
	}
	return ipc_call_launcher_add_app(icc->ipc_c, &app);
}

xrt_result_t
comp_ipc_client_compositor_launcher_set_visible(struct xrt_compositor *xc, bool visible)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_launcher_set_visible(icc->ipc_c, visible);
}

xrt_result_t
comp_ipc_client_compositor_launcher_poll_click(struct xrt_compositor *xc, int64_t *out_tile_index)
{
	if (xc == NULL || out_tile_index == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_launcher_poll_click(icc->ipc_c, out_tile_index);
}

xrt_result_t
comp_ipc_client_compositor_launcher_set_running_tile_mask(struct xrt_compositor *xc, uint64_t mask)
{
	if (xc == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	if (icc == NULL || icc->ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_launcher_set_running_tile_mask(icc->ipc_c, mask);
}


/*
 *
 * Swapchain.
 *
 */

static void
ipc_compositor_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);
	struct ipc_client_compositor *icc = ics->icc;
	xrt_result_t xret;

	xret = ipc_call_swapchain_destroy(icc->ipc_c, ics->id);

	// Can't return anything here, just continue.
	IPC_CHK_ONLY_PRINT(icc->ipc_c, xret, "ipc_call_compositor_semaphore_destroy");

	free(xsc);
}

static xrt_result_t
ipc_compositor_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);
	struct ipc_client_compositor *icc = ics->icc;
	xrt_result_t xret;

	xret = ipc_call_swapchain_wait_image(icc->ipc_c, ics->id, timeout_ns, index);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_swapchain_wait_image");
}

static xrt_result_t
ipc_compositor_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);
	struct ipc_client_compositor *icc = ics->icc;
	xrt_result_t xret;

	xret = ipc_call_swapchain_acquire_image(icc->ipc_c, ics->id, out_index);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_swapchain_acquire_image");
}

static xrt_result_t
ipc_compositor_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);
	struct ipc_client_compositor *icc = ics->icc;
	xrt_result_t xret;

	xret = ipc_call_swapchain_release_image(icc->ipc_c, ics->id, index);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_swapchain_release_image");
}


/*
 *
 * Compositor semaphore functions.
 *
 */

static xrt_result_t
ipc_client_compositor_semaphore_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns)
{
	struct ipc_client_compositor_semaphore *iccs = ipc_client_compositor_semaphore(xcsem);
	struct ipc_client_compositor *icc = iccs->icc;

	IPC_ERROR(icc->ipc_c, "Cannot call wait on client side!");

	return XRT_ERROR_IPC_FAILURE;
}

static void
ipc_client_compositor_semaphore_destroy(struct xrt_compositor_semaphore *xcsem)
{
	struct ipc_client_compositor_semaphore *iccs = ipc_client_compositor_semaphore(xcsem);
	struct ipc_client_compositor *icc = iccs->icc;
	xrt_result_t xret;

	xret = ipc_call_compositor_semaphore_destroy(icc->ipc_c, iccs->id);

	// Can't return anything here, just continue.
	IPC_CHK_ONLY_PRINT(icc->ipc_c, xret, "ipc_call_compositor_semaphore_destroy");

	free(iccs);
}


/*
 *
 * Compositor functions.
 *
 */

static xrt_result_t
ipc_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_swapchain_get_properties(icc->ipc_c, info, xsccp);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_swapchain_get_properties");
}

static xrt_result_t
swapchain_server_create(struct ipc_client_compositor *icc,
                        const struct xrt_swapchain_create_info *info,
                        struct xrt_swapchain **out_xsc)
{
	xrt_graphics_buffer_handle_t remote_handles[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	xrt_result_t xret;
	uint32_t handle;
	uint32_t image_count;
	uint64_t size;
	bool use_dedicated_allocation;

	xret = ipc_call_swapchain_create( //
	    icc->ipc_c,                   // connection
	    info,                         // in
	    &handle,                      // out
	    &image_count,                 // out
	    &size,                        // out
	    &use_dedicated_allocation,    // out
	    remote_handles,               // handles
	    XRT_MAX_SWAPCHAIN_IMAGES);    // handles
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_swapchain_create");

	struct ipc_client_swapchain *ics = U_TYPED_CALLOC(struct ipc_client_swapchain);
	ics->base.base.image_count = image_count;
	ics->base.base.wait_image = ipc_compositor_swapchain_wait_image;
	ics->base.base.acquire_image = ipc_compositor_swapchain_acquire_image;
	ics->base.base.release_image = ipc_compositor_swapchain_release_image;
	ics->base.base.destroy = ipc_compositor_swapchain_destroy;
	ics->base.base.reference.count = 1;
	ics->base.limited_unique_id = u_limited_unique_id_get();
	ics->icc = icc;
	ics->id = handle;

	for (uint32_t i = 0; i < image_count; i++) {
		ics->base.images[i].handle = remote_handles[i];
		ics->base.images[i].size = size;
		ics->base.images[i].use_dedicated_allocation = use_dedicated_allocation;
	}

	*out_xsc = &ics->base.base;

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_server_import(struct ipc_client_compositor *icc,
                        const struct xrt_swapchain_create_info *info,
                        struct xrt_image_native *native_images,
                        uint32_t image_count,
                        struct xrt_swapchain **out_xsc)
{
	struct ipc_arg_swapchain_from_native args = {0};
	xrt_graphics_buffer_handle_t handles[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	xrt_result_t xret;
	uint32_t id = 0;

	for (uint32_t i = 0; i < image_count; i++) {
		handles[i] = native_images[i].handle;
		args.sizes[i] = native_images[i].size;

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
		// DXGI handles need to be dealt with differently, they are identified
		// by having their lower bit set to 1 during transfer
		if (native_images[i].is_dxgi_handle) {
			handles[i] = (void *)((size_t)handles[i] | 1);
		}
#endif
	}

	// This does not consume the handles, it copies them.
	xret = ipc_call_swapchain_import( //
	    icc->ipc_c,                   // connection
	    info,                         // in
	    &args,                        // in
	    handles,                      // handles
	    image_count,                  // handles
	    &id);                         // out
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_swapchain_create");

	struct ipc_client_swapchain *ics = U_TYPED_CALLOC(struct ipc_client_swapchain);
	ics->base.base.image_count = image_count;
	ics->base.base.wait_image = ipc_compositor_swapchain_wait_image;
	ics->base.base.acquire_image = ipc_compositor_swapchain_acquire_image;
	ics->base.base.release_image = ipc_compositor_swapchain_release_image;
	ics->base.base.destroy = ipc_compositor_swapchain_destroy;
	ics->base.base.reference.count = 1;
	ics->base.limited_unique_id = u_limited_unique_id_get();
	ics->icc = icc;
	ics->id = id;

	// The handles were copied in the IPC call so we can reuse them here.
	for (uint32_t i = 0; i < image_count; i++) {
		ics->base.images[i] = native_images[i];
	}

	*out_xsc = &ics->base.base;

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_allocator_create(struct ipc_client_compositor *icc,
                           struct xrt_image_native_allocator *xina,
                           const struct xrt_swapchain_create_info *info,
                           struct xrt_swapchain **out_xsc)
{
	struct xrt_swapchain_create_properties xsccp = {0};
	struct xrt_image_native *images = NULL;
	xrt_result_t xret;

	// Get any needed properties.
	xret = ipc_compositor_get_swapchain_create_properties(&icc->base.base, info, &xsccp);
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_compositor_get_swapchain_create_properties");

	// Alloc the array of structs for the images.
	images = U_TYPED_ARRAY_CALLOC(struct xrt_image_native, xsccp.image_count);

	// Now allocate the images themselves
	xret = xrt_images_allocate(xina, info, xsccp.image_count, images);
	IPC_CHK_WITH_GOTO(icc->ipc_c, xret, "xrt_images_allocate", out_free);

	/*
	 * The import function takes ownership of the handles,
	 * we do not need free them if the call succeeds.
	 */
	xret = swapchain_server_import(icc, info, images, xsccp.image_count, out_xsc);
	IPC_CHK_ONLY_PRINT(icc->ipc_c, xret, "swapchain_server_import");
	if (xret != XRT_SUCCESS) {
		xrt_images_free(xina, xsccp.image_count, images);
	}

out_free:
	free(images);

	return xret;
}

static xrt_result_t
ipc_compositor_swapchain_create(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	struct xrt_image_native_allocator *xina = icc->xina;
	xrt_result_t xret;

	if (xina == NULL) {
		xret = swapchain_server_create(icc, info, out_xsc);
	} else {
		xret = swapchain_allocator_create(icc, xina, info, out_xsc);
	}

	// Errors already printed.
	return xret;
}

static xrt_result_t
ipc_compositor_create_passthrough(struct xrt_compositor *xc, const struct xrt_passthrough_create_info *info)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_create_passthrough(icc->ipc_c, info);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_create_passthrough");
}

static xrt_result_t
ipc_compositor_create_passthrough_layer(struct xrt_compositor *xc, const struct xrt_passthrough_layer_create_info *info)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_create_passthrough_layer(icc->ipc_c, info);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_create_passthrough_layer");
}

static xrt_result_t
ipc_compositor_destroy_passthrough(struct xrt_compositor *xc)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_destroy_passthrough(icc->ipc_c);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_destroy_passthrough");
}

static xrt_result_t
ipc_compositor_swapchain_import(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_image_native *native_images,
                                uint32_t image_count,
                                struct xrt_swapchain **out_xsc)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	// Errors already printed.
	return swapchain_server_import(icc, info, native_images, image_count, out_xsc);
}

static xrt_result_t
ipc_compositor_semaphore_create(struct xrt_compositor *xc,
                                xrt_graphics_sync_handle_t *out_handle,
                                struct xrt_compositor_semaphore **out_xcsem)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xrt_result_t xret;
	uint32_t id = 0;

	xret = ipc_call_compositor_semaphore_create(icc->ipc_c, &id, &handle, 1);
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_compositor_semaphore_create");

	struct ipc_client_compositor_semaphore *iccs = U_TYPED_CALLOC(struct ipc_client_compositor_semaphore);
	iccs->base.reference.count = 1;
	iccs->base.wait = ipc_client_compositor_semaphore_wait;
	iccs->base.destroy = ipc_client_compositor_semaphore_destroy;
	iccs->id = id;
	iccs->icc = icc;

	*out_handle = handle;
	*out_xcsem = &iccs->base;

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	IPC_TRACE(icc->ipc_c, "Compositor begin session.");

	xret = ipc_call_session_begin(icc->ipc_c);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_session_begin");
}

static xrt_result_t
ipc_compositor_end_session(struct xrt_compositor *xc)
{
	IPC_TRACE_MARKER();

	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	IPC_TRACE(icc->ipc_c, "Compositor end session.");

	xret = ipc_call_session_end(icc->ipc_c);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_session_end");
}

static xrt_result_t
ipc_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time,
                          int64_t *out_predicted_display_period)
{
	IPC_TRACE_MARKER();
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t predicted_display_time = 0;
	int64_t predicted_display_period = 0;

	xret = ipc_call_compositor_predict_frame( //
	    icc->ipc_c,                           // Connection
	    &frame_id,                            // Frame id
	    &wake_up_time_ns,                     // When we should wake up
	    &predicted_display_time,              // Display time
	    &predicted_display_period);           // Current period
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_compositor_predict_frame");

	// Wait until the given wake up time.
	u_wait_until(&icc->sleeper, wake_up_time_ns);

	// Signal that we woke up.
	xret = ipc_call_compositor_wait_woke(icc->ipc_c, frame_id);
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_compositor_wait_woke");

	// Only write arguments once we have fully waited.
	*out_frame_id = frame_id;
	*out_predicted_display_time = predicted_display_time;
	*out_predicted_display_period = predicted_display_period;

	return xret;
}

static xrt_result_t
ipc_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_begin_frame(icc->ipc_c, frame_id);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_begin_frame");
}

static xrt_result_t
ipc_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];

	slot->data = *data;

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	assert(data->type == XRT_LAYER_PROJECTION);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];
	struct ipc_layer_entry *layer = &slot->layers[icc->layers.layer_count];
	layer->xdev_id = 0; //! @todo Real id.
	layer->data = *data;
	for (uint32_t i = 0; i < data->view_count; ++i) {
		struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc[i]);
		layer->swapchain_ids[i] = ics->id;
	}
	// Increment the number of layers.
	icc->layers.layer_count++;

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	assert(data->type == XRT_LAYER_PROJECTION_DEPTH);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];
	struct ipc_layer_entry *layer = &slot->layers[icc->layers.layer_count];
	struct ipc_client_swapchain *xscn[XRT_MAX_VIEWS];
	struct ipc_client_swapchain *d_xscn[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = ipc_client_swapchain(xsc[i]);
		d_xscn[i] = ipc_client_swapchain(d_xsc[i]);

		layer->swapchain_ids[i] = xscn[i]->id;
		layer->swapchain_ids[i + data->view_count] = d_xscn[i]->id;
	}

	layer->xdev_id = 0; //! @todo Real id.

	layer->data = *data;

	// Increment the number of layers.
	icc->layers.layer_count++;

	return XRT_SUCCESS;
}

static xrt_result_t
handle_layer(struct xrt_compositor *xc,
             struct xrt_device *xdev,
             struct xrt_swapchain *xsc,
             const struct xrt_layer_data *data,
             enum xrt_layer_type type)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	assert(data->type == type);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];
	struct ipc_layer_entry *layer = &slot->layers[icc->layers.layer_count];
	struct ipc_client_swapchain *ics = ipc_client_swapchain(xsc);

	layer->xdev_id = 0; //! @todo Real id.
	layer->swapchain_ids[0] = ics->id;
	layer->swapchain_ids[1] = -1;
	layer->swapchain_ids[2] = -1;
	layer->swapchain_ids[3] = -1;
	layer->data = *data;

	// Increment the number of layers.
	icc->layers.layer_count++;

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_QUAD);
}

static xrt_result_t
ipc_compositor_layer_cube(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_CUBE);
}

static xrt_result_t
ipc_compositor_layer_cylinder(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              struct xrt_swapchain *xsc,
                              const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_CYLINDER);
}

static xrt_result_t
ipc_compositor_layer_equirect1(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_EQUIRECT1);
}

static xrt_result_t
ipc_compositor_layer_equirect2(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_EQUIRECT2);
}

static xrt_result_t
ipc_compositor_layer_passthrough(struct xrt_compositor *xc, struct xrt_device *xdev, const struct xrt_layer_data *data)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	assert(data->type == XRT_LAYER_PASSTHROUGH);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];
	struct ipc_layer_entry *layer = &slot->layers[icc->layers.layer_count];

	layer->xdev_id = 0; //! @todo Real id.
	layer->data = *data;

	// Increment the number of layers.
	icc->layers.layer_count++;

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	return handle_layer(xc, xdev, xsc, data, XRT_LAYER_WINDOW_SPACE);
}

static xrt_result_t
ipc_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	bool valid_sync = xrt_graphics_sync_handle_is_valid(sync_handle);

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];

	// Last bit of data to put in the shared memory area.
	slot->layer_count = icc->layers.layer_count;

	xret = ipc_call_compositor_layer_sync( //
	    icc->ipc_c,                        //
	    icc->layers.slot_id,               //
	    &sync_handle,                      //
	    valid_sync ? 1 : 0,                //
	    &icc->layers.slot_id);             //

	/*
	 * We are probably in a really bad state if we fail, at
	 * least print out the error and continue as best we can.
	 */
	IPC_CHK_ONLY_PRINT(icc->ipc_c, xret, "ipc_call_compositor_layer_sync_with_semaphore");

	// Reset.
	icc->layers.layer_count = 0;

	// Need to consume this handle.
	if (valid_sync) {
		u_graphics_sync_unref(&sync_handle);
	}

	return xret;
}

static xrt_result_t
ipc_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                           struct xrt_compositor_semaphore *xcsem,
                                           uint64_t value)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	struct ipc_client_compositor_semaphore *iccs = ipc_client_compositor_semaphore(xcsem);
	xrt_result_t xret;

	struct ipc_shared_memory *ism = icc->ipc_c->ism;
	struct ipc_layer_slot *slot = &ism->slots[icc->layers.slot_id];

	// Last bit of data to put in the shared memory area.
	slot->layer_count = icc->layers.layer_count;

	xret = ipc_call_compositor_layer_sync_with_semaphore( //
	    icc->ipc_c,                                       //
	    icc->layers.slot_id,                              //
	    iccs->id,                                         //
	    value,                                            //
	    &icc->layers.slot_id);                            //

	/*
	 * We are probably in a really bad state if we fail, at
	 * least print out the error and continue as best we can.
	 */
	IPC_CHK_ONLY_PRINT(icc->ipc_c, xret, "ipc_call_compositor_layer_sync_with_semaphore");

	// Reset.
	icc->layers.layer_count = 0;

	return xret;
}

static xrt_result_t
ipc_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_discard_frame(icc->ipc_c, frame_id);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_discard_frame");
}

static xrt_result_t
ipc_compositor_set_performance_level(struct xrt_compositor *xc,
                                     enum xrt_perf_domain domain,
                                     enum xrt_perf_set_level level)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;
	xret = ipc_call_compositor_set_performance_level(icc->ipc_c, domain, level);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_set_performance_level");
}

static xrt_result_t
ipc_compositor_set_thread_hint(struct xrt_compositor *xc, enum xrt_thread_hint hint, uint32_t thread_id)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_set_thread_hint(icc->ipc_c, hint, thread_id);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_set_thread_hint");
}

static xrt_result_t
ipc_compositor_get_display_refresh_rate(struct xrt_compositor *xc, float *out_display_refresh_rate_hz)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_get_display_refresh_rate(icc->ipc_c, out_display_refresh_rate_hz);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_get_display_refresh_rate");
}

static xrt_result_t
ipc_compositor_request_display_refresh_rate(struct xrt_compositor *xc, float display_refresh_rate_hz)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_request_display_refresh_rate(icc->ipc_c, display_refresh_rate_hz);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_request_display_refresh_rate");
}

static xrt_result_t
ipc_compositor_get_reference_bounds_rect(struct xrt_compositor *xc,
                                         enum xrt_reference_space_type reference_space_type,
                                         struct xrt_vec2 *bounds)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);
	xrt_result_t xret;

	xret = ipc_call_compositor_get_reference_bounds_rect(icc->ipc_c, reference_space_type, bounds);
	IPC_CHK_ALWAYS_RET(icc->ipc_c, xret, "ipc_call_compositor_get_reference_bounds_rect");
}

static void
ipc_compositor_destroy(struct xrt_compositor *xc)
{
	struct ipc_client_compositor *icc = ipc_client_compositor(xc);

	assert(icc->compositor_created);

	os_precise_sleeper_deinit(&icc->sleeper);

	icc->compositor_created = false;
}

static void
ipc_compositor_init(struct ipc_client_compositor *icc, struct xrt_compositor_native **out_xcn)
{
	icc->base.base.get_swapchain_create_properties = ipc_compositor_get_swapchain_create_properties;
	icc->base.base.create_swapchain = ipc_compositor_swapchain_create;
	icc->base.base.import_swapchain = ipc_compositor_swapchain_import;
	icc->base.base.create_semaphore = ipc_compositor_semaphore_create;
	icc->base.base.create_passthrough = ipc_compositor_create_passthrough;
	icc->base.base.create_passthrough_layer = ipc_compositor_create_passthrough_layer;
	icc->base.base.destroy_passthrough = ipc_compositor_destroy_passthrough;
	icc->base.base.begin_session = ipc_compositor_begin_session;
	icc->base.base.end_session = ipc_compositor_end_session;
	icc->base.base.wait_frame = ipc_compositor_wait_frame;
	icc->base.base.begin_frame = ipc_compositor_begin_frame;
	icc->base.base.discard_frame = ipc_compositor_discard_frame;
	icc->base.base.layer_begin = ipc_compositor_layer_begin;
	icc->base.base.layer_projection = ipc_compositor_layer_projection;
	icc->base.base.layer_projection_depth = ipc_compositor_layer_projection_depth;
	icc->base.base.layer_quad = ipc_compositor_layer_quad;
	icc->base.base.layer_cube = ipc_compositor_layer_cube;
	icc->base.base.layer_cylinder = ipc_compositor_layer_cylinder;
	icc->base.base.layer_equirect1 = ipc_compositor_layer_equirect1;
	icc->base.base.layer_equirect2 = ipc_compositor_layer_equirect2;
	icc->base.base.layer_passthrough = ipc_compositor_layer_passthrough;
	icc->base.base.layer_window_space = ipc_compositor_layer_window_space;
	icc->base.base.layer_commit = ipc_compositor_layer_commit;
	icc->base.base.layer_commit_with_semaphore = ipc_compositor_layer_commit_with_semaphore;
	icc->base.base.destroy = ipc_compositor_destroy;
	icc->base.base.set_thread_hint = ipc_compositor_set_thread_hint;
	icc->base.base.get_display_refresh_rate = ipc_compositor_get_display_refresh_rate;
	icc->base.base.request_display_refresh_rate = ipc_compositor_request_display_refresh_rate;
	icc->base.base.set_performance_level = ipc_compositor_set_performance_level;
	icc->base.base.get_reference_bounds_rect = ipc_compositor_get_reference_bounds_rect;

	// Using in wait frame.
	os_precise_sleeper_init(&icc->sleeper);

	// Fetch info from the compositor, among it the format format list.
	get_info(&(icc->base.base), &icc->base.base.info);

	*out_xcn = &icc->base;
}


/*
 *
 * Loopback image allocator.
 *
 */

#ifdef IPC_USE_LOOPBACK_IMAGE_ALLOCATOR
static inline xrt_result_t
ipc_compositor_images_allocate(struct xrt_image_native_allocator *xina,
                               const struct xrt_swapchain_create_info *xsci,
                               size_t in_image_count,
                               struct xrt_image_native *out_images)
{
	struct ipc_client_compositor *icc = container_of(xina, struct ipc_client_compositor, loopback_xina);

	int remote_fds[IPC_MAX_SWAPCHAIN_FDS] = {0};
	xrt_result_t xret;
	uint32_t image_count;
	uint32_t handle;
	uint64_t size;

	for (size_t i = 0; i < ARRAY_SIZE(remote_fds); i++) {
		remote_fds[i] = -1;
	}

	for (size_t i = 0; i < in_image_count; i++) {
		out_images[i].fd = -1;
		out_images[i].size = 0;
	}

	xret = ipc_call_swapchain_create( //
	    icc->ipc_c,                   // connection
	    xsci,                         // in
	    &handle,                      // out
	    &image_count,                 // out
	    &size,                        // out
	    remote_fds,                   // fds
	    IPC_MAX_SWAPCHAIN_FDS);       // fds
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_swapchain_create");

	/*
	 * It's okay to destroy it immediately, the native handles are
	 * now owned by us and we keep the buffers alive that way.
	 */
	xret = ipc_call_swapchain_destroy(icc->ipc_c, handle);
	assert(xret == XRT_SUCCESS);

	// Clumsy way of handling this.
	if (image_count < in_image_count) {
		for (uint32_t k = 0; k < image_count && k < in_image_count; k++) {
			/*
			 * Overly-broad condition: we know that any fd not touched by
			 * ipc_call_swapchain_create will be -1.
			 */
			if (remote_fds[k] >= 0) {
				close(remote_fds[k]);
				remote_fds[k] = -1;
			}
		}

		return XRT_ERROR_IPC_FAILURE;
	}

	// Copy up to in_image_count, or image_count what ever is lowest.
	uint32_t i = 0;
	for (; i < image_count && i < in_image_count; i++) {
		out_images[i].fd = remote_fds[i];
		out_images[i].size = size;
	}

	// Close any fds we are not interested in.
	for (; i < image_count; i++) {
		/*
		 * Overly-broad condition: we know that any fd not touched by
		 * ipc_call_swapchain_create will be -1.
		 */
		if (remote_fds[i] >= 0) {
			close(remote_fds[i]);
			remote_fds[i] = -1;
		}
	}

	return XRT_SUCCESS;
}

static inline xrt_result_t
ipc_compositor_images_free(struct xrt_image_native_allocator *xina,
                           size_t image_count,
                           struct xrt_image_native *out_images)
{
	for (uint32_t i = 0; i < image_count; i++) {
		close(out_images[i].fd);
		out_images[i].fd = -1;
		out_images[i].size = 0;
	}

	return XRT_SUCCESS;
}

static inline void
ipc_compositor_images_destroy(struct xrt_image_native_allocator *xina)
{
	// Noop
}
#endif


/*
 *
 * System compositor.
 *
 */

xrt_result_t
ipc_syscomp_create_native_compositor(struct xrt_system_compositor *xsc,
                                     const struct xrt_session_info *xsi,
                                     struct xrt_session_event_sink *xses,
                                     struct xrt_compositor_native **out_xcn)
{
	struct ipc_client_compositor *icc = container_of(xsc, struct ipc_client_compositor, system);

	IPC_ERROR(icc->ipc_c, "This function shouldn't be called!");

	return XRT_ERROR_IPC_FAILURE;
}

void
ipc_syscomp_destroy(struct xrt_system_compositor *xsc)
{
	struct ipc_client_compositor *icc = container_of(xsc, struct ipc_client_compositor, system);

	// Does null checking.
	xrt_images_destroy(&icc->xina);

	//! @todo Implement
	IPC_TRACE(icc->ipc_c, "NOT IMPLEMENTED compositor destroy.");

	free(icc);
}


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
ipc_client_create_native_compositor(struct xrt_system_compositor *xsysc,
                                    const struct xrt_session_info *xsi,
                                    struct xrt_compositor_native **out_xcn)
{
	struct ipc_client_compositor *icc = container_of(xsysc, struct ipc_client_compositor, system);
	xrt_result_t xret;

	if (icc->compositor_created) {
		return XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED;
	}

	/*
	 * Needs to be done before init, we don't own the service side session
	 * the session does. But we create it here in case any extra arguments
	 * that only the compositor knows about needs to be sent.
	 */
	bool initial_visible = false;
	bool initial_focused = false;
	xret = ipc_call_session_create( //
	    icc->ipc_c,                 // ipc_c
	    xsi,                        // xsi
	    true,                       // create_native_compositor
	    &initial_visible,           // out_initial_visible
	    &initial_focused);          // out_initial_focused
	IPC_CHK_AND_RET(icc->ipc_c, xret, "ipc_call_session_create");

	// Store initial state for the OpenXR layer to query (avoids race condition)
	icc->initial_visible = initial_visible;
	icc->initial_focused = initial_focused;

	// Needs to be done after session create call.
	ipc_compositor_init(icc, out_xcn);

	// Set initial visibility/focus in compositor info (for OpenXR layer to read)
	// This avoids the race condition where events must be polled before these are set
	icc->base.base.info.initial_visible = initial_visible;
	icc->base.base.info.initial_focused = initial_focused;

	icc->compositor_created = true;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_create_system_compositor(struct ipc_connection *ipc_c,
                                    struct xrt_image_native_allocator *xina,
                                    struct xrt_device *xdev,
                                    struct xrt_system_compositor **out_xcs)
{
	struct ipc_client_compositor *c = U_TYPED_CALLOC(struct ipc_client_compositor);

	c->system.create_native_compositor = ipc_syscomp_create_native_compositor;
	c->system.destroy = ipc_syscomp_destroy;
	c->ipc_c = ipc_c;
	c->xina = xina;


#ifdef IPC_USE_LOOPBACK_IMAGE_ALLOCATOR
	c->loopback_xina.images_allocate = ipc_compositor_images_allocate;
	c->loopback_xina.images_free = ipc_compositor_images_free;
	c->loopback_xina.destroy = ipc_compositor_images_destroy;

	if (c->xina == NULL) {
		c->xina = &c->loopback_xina;
	}
#endif

	// Fetch info from the system compositor.
	get_system_info(c, &c->system.info);

	// Mark that we're running in service mode (IPC client)
	// This flag is used to prevent in-process compositors (like D3D11 native) from being used
	c->system.info.is_service_mode = true;

	*out_xcs = &c->system;

	return XRT_SUCCESS;
}
