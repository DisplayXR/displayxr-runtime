// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) client-side IPC calls (see ipc_client_lift.h).
 * @ingroup ipc_client
 */

#include "xrt/xrt_config_os.h"

#include "util/u_misc.h"

#include "shared/ipc_protocol.h"
#include "shared/ipc_lift_caps.h"
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift.h"
#include "ipc_client_generated.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
params_to_ipc(const struct xrt_dp_lift_params *p,
              const float *viewpoints,
              uint32_t viewpoint_count,
              struct ipc_lift_params *out)
{
	U_ZERO(out);
	out->convergence = p->convergence;
	out->strength = p->strength;
	out->inpaint = p->inpaint;
	out->view_count = p->view_count;
	out->focal_px =
	    p->struct_size >= offsetof(struct xrt_dp_lift_params, focal_px) + sizeof(float) ? p->focal_px : 0.0f;
	if (viewpoints != NULL && viewpoint_count > 0) {
		uint32_t n = viewpoint_count > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : viewpoint_count;
		out->viewpoint_count = n;
		memcpy(out->viewpoints, viewpoints, (size_t)n * 3 * sizeof(float));
	}
}

xrt_result_t
ipc_client_conv_hint_interest(struct ipc_connection *ipc_c, uint64_t task_mask)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct xrt_conv_wire_hint_interest hint;
	U_ZERO(&hint);
	hint.struct_size = (uint32_t)sizeof(hint);
	hint.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	hint.task_mask = task_mask;
	return ipc_call_conv_hint_interest(ipc_c, &hint);
}

xrt_result_t
ipc_client_lift_get_properties(struct ipc_connection *ipc_c, struct xrt_dp_lift_caps *out_caps)
{
	xrt_dp_lift_caps_init(out_caps);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_lift_properties props;
	U_ZERO(&props);
	xrt_result_t xret = ipc_call_lift_get_properties(ipc_c, &props);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	// One copy, shared with the service (#2005 G1: this used to drop the aux-depth caps).
	ipc_lift_caps_from_properties(&props, out_caps);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_lift_stream_create(struct ipc_connection *ipc_c,
                              uint32_t mode,
                              uint32_t content_hint,
                              float input_scale,
                              uint32_t aux_outputs,
                              uint32_t aux_depth_format,
                              uint64_t *out_stream_id)
{
	if (ipc_c == NULL || out_stream_id == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_lift_stream_create(ipc_c, mode, content_hint, input_scale, aux_outputs, aux_depth_format,
	                                   out_stream_id);
}

xrt_result_t
ipc_client_lift_stream_destroy(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_lift_stream_destroy(ipc_c, stream_id);
}

xrt_result_t
ipc_client_lift_submit(struct ipc_connection *ipc_c,
                       uint64_t stream_id,
                       xrt_graphics_buffer_handle_t handle,
                       bool is_dxgi,
                       uint32_t width,
                       uint32_t height,
                       int64_t source_time,
                       const struct xrt_dp_lift_params *params,
                       const float *viewpoints,
                       uint32_t viewpoint_count,
                       const struct xrt_lift_view_control *view_control,
                       const struct xrt_lift_rig *rig,
                       uint64_t *out_frame_id)
{
	if (ipc_c == NULL || out_frame_id == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_frame_id = 0;
	struct ipc_arg_lift_submit args;
	U_ZERO(&args);
	args.stream_id = stream_id;
	args.source_time = source_time;
	args.width = width;
	args.height = height;
	if (params != NULL) {
		args.has_params = 1;
		params_to_ipc(params, viewpoints, viewpoint_count, &args.params);
		if (view_control != NULL) {
			args.params.has_view_control = 1;
			args.params.view_control = *view_control;
		}
		if (rig != NULL) {
			args.params.rig = *rig; // XR_DXR_lift v3; type NONE = none
		}
	}
	xrt_graphics_buffer_handle_t handles[1] = {handle};
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
	// Legacy DXGI handles cross raw, low-bit tagged (the weave_submit convention).
	if (is_dxgi && handles[0] != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		handles[0] = (void *)((size_t)handles[0] | 1);
	}
#else
	(void)is_dxgi;
#endif
	return ipc_call_lift_submit_frame(ipc_c, &args, handles, 1, out_frame_id);
}

static void
lift_result_from_ipc(const struct ipc_lift_result *r, struct xrt_lift_result *out_result)
{
	out_result->frame_id = r->frame_id;
	out_result->source_time = r->source_time;
	out_result->fence_value = r->fence_value;
	out_result->latency_ns = r->latency_ns;
	out_result->width = r->width;
	out_result->height = r->height;
	out_result->format = r->format;
	out_result->view_count = r->view_count;
	out_result->output_realloc = r->output_realloc != 0;
	out_result->viewpoint_count =
	    r->viewpoint_count > XRT_LIFT_MAX_VIEWS ? XRT_LIFT_MAX_VIEWS : r->viewpoint_count;
	memcpy(out_result->viewpoints, r->viewpoints, sizeof(out_result->viewpoints));
	memcpy(out_result->rect_center, r->rect_center, sizeof(out_result->rect_center));
	memcpy(out_result->rect_size, r->rect_size, sizeof(out_result->rect_size));
	out_result->depth = r->depth;
}

xrt_result_t
ipc_client_lift_acquire(struct ipc_connection *ipc_c,
                        uint64_t stream_id,
                        bool *out_ready,
                        struct xrt_lift_result *out_result)
{
	if (ipc_c == NULL || out_ready == NULL || out_result == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_ready = false;
	U_ZERO(out_result);
	struct ipc_lift_result r;
	U_ZERO(&r);
	bool ready = false;
	xrt_result_t xret = ipc_call_lift_acquire_result(ipc_c, stream_id, &ready, &r);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	*out_ready = ready;
	lift_result_from_ipc(&r, out_result);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_lift_get_depth_output(struct ipc_connection *ipc_c,
                                 uint64_t stream_id,
                                 bool *out_have,
                                 uint32_t *out_width,
                                 uint32_t *out_height,
                                 uint32_t *out_format,
                                 xrt_graphics_buffer_handle_t *out_handle)
{
	if (ipc_c == NULL || out_have == NULL || out_handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_have = false;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	uint32_t w = 0, h = 0, f = 0;
	xrt_result_t xret = ipc_call_lift_get_depth_output(ipc_c, stream_id, out_have, &w, &h, &f, out_handle, 1);
	if (out_width != NULL) {
		*out_width = w;
	}
	if (out_height != NULL) {
		*out_height = h;
	}
	if (out_format != NULL) {
		*out_format = f;
	}
	return xret;
}

xrt_result_t
ipc_client_lift_get_output(struct ipc_connection *ipc_c,
                           uint64_t stream_id,
                           bool *out_have,
                           uint32_t *out_width,
                           uint32_t *out_height,
                           uint32_t *out_format,
                           xrt_graphics_buffer_handle_t *out_handle)
{
	if (ipc_c == NULL || out_have == NULL || out_handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_have = false;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	uint32_t w = 0, h = 0, f = 0;
	xrt_result_t xret = ipc_call_lift_get_output(ipc_c, stream_id, out_have, &w, &h, &f, out_handle, 1);
	if (out_width != NULL) {
		*out_width = w;
	}
	if (out_height != NULL) {
		*out_height = h;
	}
	if (out_format != NULL) {
		*out_format = f;
	}
	return xret;
}

xrt_result_t
ipc_client_lift_get_fence(struct ipc_connection *ipc_c,
                          uint64_t stream_id,
                          bool *out_have,
                          xrt_graphics_sync_handle_t *out_handle)
{
	if (ipc_c == NULL || out_have == NULL || out_handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_have = false;
	*out_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	return ipc_call_lift_get_fence(ipc_c, stream_id, out_have, out_handle, 1);
}

xrt_result_t
ipc_client_lift_acquire_blob(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             uint64_t capacity,
                             uint8_t *out_bytes,
                             bool *out_ready,
                             bool *out_delivered,
                             struct xrt_lift_blob_info *out_info)
{
	if (ipc_c == NULL || out_ready == NULL || out_delivered == NULL || out_info == NULL ||
	    (capacity > 0 && out_bytes == NULL)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_ready = false;
	*out_delivered = false;
	U_ZERO(out_info);

	ipc_client_connection_lock(ipc_c);
	xrt_result_t xret = ipc_send_lift_acquire_blob_locked(ipc_c, stream_id, capacity);
	if (xret != XRT_SUCCESS) {
		ipc_client_connection_unlock(ipc_c);
		return xret;
	}
	bool ready = false;
	uint64_t frame_id = 0, byte_count = 0;
	int64_t source_time = 0;
	uint32_t format = 0;
	xrt_result_t result =
	    ipc_receive_lift_acquire_blob_locked(ipc_c, &ready, &frame_id, &source_time, &format, &byte_count);
	if (result != XRT_SUCCESS) {
		// Receive failure (pipe) or a service-side rejection: no payload follows.
		ipc_client_connection_unlock(ipc_c);
		return result;
	}
	// The service sends the bytes exactly when it had them and capacity sufficed.
	if (ready && byte_count > 0 && capacity >= byte_count) {
		xret = ipc_receive(&ipc_c->imc, out_bytes, (size_t)byte_count);
		*out_delivered = xret == XRT_SUCCESS;
	}
	ipc_client_connection_unlock(ipc_c);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	*out_ready = ready;
	out_info->frame_id = frame_id;
	out_info->source_time = source_time;
	out_info->format = format;
	out_info->byte_count = byte_count;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_lift_weave_rects(struct ipc_connection *ipc_c, uint32_t count, const struct xrt_lift_weave_rect *rects)
{
	if (ipc_c == NULL || count > IPC_LIFT_WEAVE_RECTS_MAX || (count > 0 && rects == NULL)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	// XR_DXR_lift v3: EXPLICIT viewpoints of lifted rects do not fit the rects
	// message; stage them first, one call per such rect.
	for (uint32_t i = 0; i < count; i++) {
		if (rects[i].viewpoint_floats < 3) {
			continue;
		}
		struct ipc_arg_lift_rect_viewpoints va;
		U_ZERO(&va);
		va.stream_id = rects[i].stream_id;
		uint32_t n = rects[i].viewpoint_floats / 3;
		n = n > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : n;
		va.viewpoint_count = n;
		memcpy(va.viewpoints, rects[i].viewpoints, (size_t)n * 3 * sizeof(float));
		xrt_result_t vx = ipc_call_lift_weave_rect_viewpoints(ipc_c, &va);
		if (vx == XRT_ERROR_IPC_FAILURE) {
			return vx;
		}
	}
	struct ipc_arg_lift_weave_rects args;
	U_ZERO(&args);
	args.count = count;
	for (uint32_t i = 0; i < count; i++) {
		args.rects[i].stream_id = rects[i].stream_id;
		args.rects[i].rect_index = rects[i].rect_index;
		args.rects[i].has_params = rects[i].has_params ? 1u : 0u;
		args.rects[i].convergence = rects[i].params.convergence;
		args.rects[i].strength = rects[i].params.strength;
		args.rects[i].inpaint = rects[i].params.inpaint;
		args.rects[i].view_count = rects[i].params.view_count;
		args.rects[i].focal_px = rects[i].params.focal_px;
		args.rects[i].has_view_control = rects[i].has_view_control ? 1u : 0u;
		args.rects[i].view_control = rects[i].view_control;
		args.rects[i].viewpoint_count = rects[i].viewpoint_floats / 3;
		args.rects[i].rig = rects[i].rig;
		args.rects[i].depth_cursor = rects[i].depth_cursor ? 1u : 0u;
	}
	return ipc_call_lift_weave_rects(ipc_c, &args);
}

xrt_result_t
ipc_client_lift_set_priority(struct ipc_connection *ipc_c, uint64_t stream_id, uint32_t priority)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_lift_stream_set_priority(ipc_c, stream_id, priority);
}

xrt_result_t
ipc_client_lift_stats(struct ipc_connection *ipc_c, uint64_t stream_id, struct xrt_lift_stream_stats *out_stats)
{
	if (ipc_c == NULL || out_stats == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	U_ZERO(out_stats);
	return ipc_call_lift_stream_stats(ipc_c, stream_id, out_stats);
}


/*
 *
 * XR_DXR_lift spec 4: the conv_* family (#2054).
 *
 */

xrt_result_t
ipc_client_conv_enum_task(struct ipc_connection *ipc_c,
                          uint32_t task,
                          struct xrt_conv_wire_task_desc *out_desc,
                          uint64_t *out_image_kinds,
                          uint64_t *out_export_kinds)
{
	U_ZERO(out_desc);
	*out_image_kinds = 0;
	*out_export_kinds = 0;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_conv_enum_task(ipc_c, task, out_desc, out_image_kinds, out_export_kinds);
}

xrt_result_t
ipc_client_conv_warm(struct ipc_connection *ipc_c, uint32_t task)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct xrt_conv_wire_task_request req;
	U_ZERO(&req);
	req.struct_size = (uint32_t)sizeof(req);
	req.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	req.task = task;
	return ipc_call_conv_request_warm(ipc_c, &req);
}

xrt_result_t
ipc_client_conv_stream_create(struct ipc_connection *ipc_c,
                              const struct xrt_conv_wire_stream_desc *desc,
                              uint64_t required_outputs,
                              uint32_t legacy_mode,
                              float input_scale,
                              struct xrt_conv_wire_stream_created *out_created)
{
	U_ZERO(out_created);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_conv_stream_create(ipc_c, desc, required_outputs, legacy_mode, input_scale, out_created);
}

xrt_result_t
ipc_client_conv_stream_create_v5(struct ipc_connection *ipc_c,
                                 const struct xrt_conv_wire_stream_desc *desc,
                                 uint64_t required_outputs,
                                 uint32_t legacy_mode,
                                 float input_scale,
                                 const char *preferred_provider,
                                 struct xrt_conv_wire_stream_created *out_created)
{
	if (preferred_provider == NULL || preferred_provider[0] == '\0') {
		return ipc_client_conv_stream_create(ipc_c, desc, required_outputs, legacy_mode, input_scale,
		                                     out_created);
	}
	U_ZERO(out_created);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct xrt_conv_wire_provider_pref pref;
	xrt_conv_wire_begin(&pref, (uint32_t)sizeof(pref));
	(void)snprintf(pref.provider, sizeof(pref.provider), "%s", preferred_provider);
	return ipc_call_conv_stream_create_v5(ipc_c, desc, required_outputs, legacy_mode, input_scale, &pref,
	                                      out_created);
}

xrt_result_t
ipc_client_conv_enum_candidates(
    struct ipc_connection *ipc_c, uint32_t task, struct xrt_conv_wire_candidate *out, uint32_t max, uint32_t *out_count)
{
	*out_count = 0;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	// One candidate per reply. A walk that straddles a change restarts; after
	// a few tries the last walk stands (the event tells the app to re-read).
	xrt_result_t xret = XRT_SUCCESS;
	for (int attempt = 0; attempt < 4; attempt++) {
		struct xrt_conv_wire_generation first;
		struct xrt_conv_wire_candidate c;
		uint32_t count = 0;
		xret = ipc_call_conv_enum_candidates(ipc_c, task, 0, &count, &first, &c);
		if (xret != XRT_SUCCESS) {
			return xret;
		}
		bool moved = false;
		for (uint32_t i = 0; i < count; i++) {
			struct xrt_conv_wire_generation g = first;
			uint32_t n = count;
			if (i > 0) {
				xret = ipc_call_conv_enum_candidates(ipc_c, task, i, &n, &g, &c);
				if (xret != XRT_SUCCESS) {
					return xret;
				}
			}
			if (n != count || g.conv_gen != first.conv_gen || g.status_gen != first.status_gen) {
				moved = true;
				break;
			}
			if (out != NULL && i < max) {
				out[i] = c;
			}
		}
		*out_count = count;
		if (!moved) {
			break;
		}
	}
	return xret;
}

xrt_result_t
ipc_client_conv_stream_reset(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_conv_stream_reset(ipc_c, stream_id);
}

xrt_result_t
ipc_client_conv_acquire(struct ipc_connection *ipc_c,
                        uint64_t stream_id,
                        bool *out_ready,
                        bool *out_discontinuity,
                        struct xrt_lift_result *out_result,
                        struct xrt_conv_wire_result *out_meta)
{
	*out_ready = false;
	*out_discontinuity = false;
	U_ZERO(out_result);
	U_ZERO(out_meta);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_lift_result r;
	U_ZERO(&r);
	xrt_result_t xret = ipc_call_conv_acquire_result(ipc_c, stream_id, out_ready, out_discontinuity, &r, out_meta);
	if (xret != XRT_SUCCESS || !*out_ready) {
		return xret;
	}
	lift_result_from_ipc(&r, out_result);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_conv_poll_event(struct ipc_connection *ipc_c, bool *out_has, struct xrt_conv_wire_event *out_event)
{
	*out_has = false;
	U_ZERO(out_event);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_conv_poll_event(ipc_c, out_has, out_event);
}

xrt_result_t
ipc_client_conv_submit_frame(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             const struct xrt_conv_wire_image *image,
                             xrt_graphics_buffer_handle_t handle,
                             xrt_graphics_buffer_handle_t sync_handle,
                             uint32_t region_x,
                             uint32_t region_y,
                             uint32_t width,
                             uint32_t height,
                             int64_t source_time,
                             const struct xrt_dp_lift_params *params,
                             const float *viewpoints,
                             uint32_t viewpoint_count,
                             const struct xrt_lift_view_control *view_control,
                             const struct xrt_lift_rig *rig,
                             uint64_t *out_frame_id)
{
	if (ipc_c == NULL || out_frame_id == NULL || image == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_frame_id = 0;
	struct ipc_arg_lift_submit args;
	U_ZERO(&args);
	args.stream_id = stream_id;
	args.source_time = source_time;
	args.width = width;
	args.height = height;
	if (params != NULL) {
		args.has_params = 1;
		params_to_ipc(params, viewpoints, viewpoint_count, &args.params);
		if (view_control != NULL) {
			args.params.has_view_control = 1;
			args.params.view_control = *view_control;
		}
		if (rig != NULL) {
			args.params.rig = *rig;
		}
	}
	xrt_graphics_buffer_handle_t handles[2] = {handle, sync_handle};
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
	// Legacy DXGI handles cross raw, low-bit tagged (the weave_submit convention).
	if (image->memory_kind == XRT_CONV_MEM_WIN32_KMT_HANDLE && handles[0] != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		handles[0] = (HANDLE)((size_t)handles[0] | 1);
	}
#endif
	uint32_t count = image->sync_kind == XRT_CONV_SYNC_FENCE_NT ? 2u : 1u;
#if !defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
	// ADR-053 P5 step E: a sync_file / timeline semaphore fd rides second.
	if (image->sync_kind == XRT_CONV_SYNC_SYNC_FD || image->sync_kind == XRT_CONV_SYNC_VK_SEMAPHORE_FD) {
		count = 2u;
	}
#endif
	return ipc_call_conv_submit_frame(ipc_c, &args, image, region_x, region_y, handles, count, out_frame_id);
}

xrt_result_t
ipc_client_conv_get_cpu_output(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               bool *out_have,
                               uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_format,
                               uint64_t *out_bytes,
                               xrt_graphics_buffer_handle_t *out_handle)
{
	*out_have = false;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_conv_get_cpu_output(ipc_c, stream_id, out_have, out_width, out_height, out_format, out_bytes,
	                                    out_handle, 1);
}
