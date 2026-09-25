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
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift.h"
#include "ipc_client_generated.h"

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
	if (viewpoints != NULL && viewpoint_count > 0) {
		uint32_t n = viewpoint_count > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : viewpoint_count;
		out->viewpoint_count = n;
		memcpy(out->viewpoints, viewpoints, (size_t)n * 3 * sizeof(float));
	}
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
	out_caps->modes = props.modes;
	out_caps->max_streams = props.max_streams;
	out_caps->max_views = props.max_views;
	out_caps->depth_semantics = props.depth_semantics;
	out_caps->state = props.state;
	out_caps->typical_latency_ns = props.typical_latency_ns;
	memcpy(out_caps->backend, props.backend, sizeof(out_caps->backend));
	out_caps->backend[sizeof(out_caps->backend) - 1] = '\0';
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_lift_stream_create(struct ipc_connection *ipc_c,
                              uint32_t mode,
                              uint32_t content_hint,
                              float input_scale,
                              uint64_t *out_stream_id)
{
	if (ipc_c == NULL || out_stream_id == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_lift_stream_create(ipc_c, mode, content_hint, input_scale, out_stream_id);
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
	out_result->frame_id = r.frame_id;
	out_result->source_time = r.source_time;
	out_result->fence_value = r.fence_value;
	out_result->latency_ns = r.latency_ns;
	out_result->width = r.width;
	out_result->height = r.height;
	out_result->format = r.format;
	out_result->view_count = r.view_count;
	out_result->output_realloc = r.output_realloc != 0;
	return XRT_SUCCESS;
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
	}
	return ipc_call_lift_weave_rects(ipc_c, &args);
}
