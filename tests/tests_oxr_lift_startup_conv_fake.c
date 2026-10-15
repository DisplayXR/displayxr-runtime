// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The spec-4 seams of oxr_conv.c for tests_oxr_lift_startup (#2054).
 *
 * tests_oxr_lift_startup pins the v3 startup sequence and stays unchanged: a
 * v3 caller never reaches any of these, so each one records nothing and
 * answers "not served". They exist only so oxr_conv.c links without st_oxr
 * and the generated IPC client.
 */

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_handles.h"

#include "oxr_objects.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct ipc_connection;
struct ipc_lift_result;
struct ipc_arg_lift_submit;

xrt_result_t
ipc_call_conv_enum_task(struct ipc_connection *ipc_c,
                        uint32_t task,
                        struct xrt_conv_wire_task_desc *out_desc,
                        uint64_t *out_image_kinds,
                        uint64_t *out_export_kinds)
{
	(void)ipc_c;
	memset(out_desc, 0, sizeof(*out_desc));
	out_desc->task = task;
	out_desc->state = XRT_CONV_STATE_ABSENT;
	*out_image_kinds = 0;
	*out_export_kinds = 0;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_conv_request_warm(struct ipc_connection *ipc_c, const struct xrt_conv_wire_task_request *request)
{
	(void)ipc_c;
	(void)request;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
ipc_call_conv_stream_create(struct ipc_connection *ipc_c,
                            const struct xrt_conv_wire_stream_desc *desc,
                            uint64_t required_outputs,
                            uint32_t legacy_mode,
                            float input_scale,
                            struct xrt_conv_wire_stream_created *out_created)
{
	(void)ipc_c;
	(void)desc;
	(void)required_outputs;
	(void)legacy_mode;
	(void)input_scale;
	memset(out_created, 0, sizeof(*out_created));
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
ipc_call_conv_stream_reset(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	(void)ipc_c;
	(void)stream_id;
	return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

xrt_result_t
ipc_call_conv_acquire_result(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             bool *out_ready,
                             bool *out_discontinuity,
                             struct ipc_lift_result *out_lift_result,
                             struct xrt_conv_wire_result *out_result)
{
	(void)ipc_c;
	(void)stream_id;
	(void)out_lift_result;
	*out_ready = false;
	*out_discontinuity = false;
	memset(out_result, 0, sizeof(*out_result));
	return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

xrt_result_t
ipc_call_conv_poll_event(struct ipc_connection *ipc_c, bool *out_has_event, struct xrt_conv_wire_event *out_event)
{
	(void)ipc_c;
	*out_has_event = false;
	memset(out_event, 0, sizeof(*out_event));
	return XRT_SUCCESS;
}

XrResult
oxr_event_push_XrEventDataLiftTaskStateChangedDXR(struct oxr_logger *log,
                                                  struct oxr_session *sess,
                                                  XrLiftTaskDXR task,
                                                  XrLiftTaskStateDXR state,
                                                  XrLiftTaskStateDXR previousState,
                                                  XrLiftFailureReasonDXR failureReason,
                                                  float progress)
{
	(void)log;
	(void)sess;
	(void)task;
	(void)state;
	(void)previousState;
	(void)failureReason;
	(void)progress;
	return XR_SUCCESS;
}

XrResult
oxr_event_push_XrEventDataLiftProvidersChangedDXR(struct oxr_logger *log, struct oxr_session *sess)
{
	(void)log;
	(void)sess;
	return XR_SUCCESS;
}

xrt_result_t
ipc_call_conv_submit_frame(struct ipc_connection *ipc_c,
                           const struct ipc_arg_lift_submit *args,
                           const struct xrt_conv_wire_image *image,
                           uint32_t region_x,
                           uint32_t region_y,
                           const xrt_graphics_buffer_handle_t *handles,
                           const uint32_t handle_count,
                           uint64_t *out_frame_id)
{
	(void)ipc_c;
	(void)args;
	(void)image;
	(void)region_x;
	(void)region_y;
	(void)handles;
	(void)handle_count;
	*out_frame_id = 0;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
ipc_call_conv_get_cpu_output(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             bool *out_have_output,
                             uint32_t *out_width,
                             uint32_t *out_height,
                             uint32_t *out_format,
                             uint64_t *out_bytes,
                             xrt_graphics_buffer_handle_t *out_handles,
                             uint32_t max_handle_count)
{
	(void)ipc_c;
	(void)stream_id;
	(void)out_handles;
	(void)max_handle_count;
	*out_have_output = false;
	*out_width = 0;
	*out_height = 0;
	*out_format = 0;
	*out_bytes = 0;
	return XRT_SUCCESS;
}
