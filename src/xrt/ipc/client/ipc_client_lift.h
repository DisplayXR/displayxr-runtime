// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) client side: connection-level calls shared by
 *         the OpenXR state tracker's bridges and `displayxr-cli lift`.
 *
 * Lift streams belong to the IPC connection, not a session, so every call here
 * takes a bare @ref ipc_connection. Results use the xrt_lift.h value types.
 *
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_lift.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;

xrt_result_t
ipc_client_lift_get_properties(struct ipc_connection *ipc_c, struct xrt_dp_lift_caps *out_caps);

xrt_result_t
ipc_client_lift_stream_create(
    struct ipc_connection *ipc_c, uint32_t mode, uint32_t content_hint, float input_scale, uint64_t *out_stream_id);

xrt_result_t
ipc_client_lift_stream_destroy(struct ipc_connection *ipc_c, uint64_t stream_id);

/*!
 * Submit one frame. @p params NULL = the stream's last parameters.
 * @p viewpoints holds @p viewpoint_count xyz triplets (EXPLICIT source), or
 * NULL/0 (TRACKED). On Windows a legacy DXGI handle is low-bit tagged on the
 * wire (@p is_dxgi), as for weave_submit.
 */
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
                       uint64_t *out_frame_id);

//! @p out_ready false = nothing newer than the last acquire (NOT READY).
xrt_result_t
ipc_client_lift_acquire(struct ipc_connection *ipc_c,
                        uint64_t stream_id,
                        bool *out_ready,
                        struct xrt_lift_result *out_result);

//! The stream's export texture (a NEW handle, caller-owned) + its layout.
xrt_result_t
ipc_client_lift_get_output(struct ipc_connection *ipc_c,
                           uint64_t stream_id,
                           bool *out_have,
                           uint32_t *out_width,
                           uint32_t *out_height,
                           uint32_t *out_format,
                           xrt_graphics_buffer_handle_t *out_handle);

//! The stream's export fence (a NEW handle, caller-owned).
xrt_result_t
ipc_client_lift_get_fence(struct ipc_connection *ipc_c,
                          uint64_t stream_id,
                          bool *out_have,
                          xrt_graphics_sync_handle_t *out_handle);

/*!
 * Two-call blob acquire. @p capacity 0 = size query (the service latches the
 * blob); otherwise, when @p capacity >= byte_count, the bytes are written to
 * @p out_bytes. @p out_delivered says whether they were.
 */
xrt_result_t
ipc_client_lift_acquire_blob(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             uint64_t capacity,
                             uint8_t *out_bytes,
                             bool *out_ready,
                             bool *out_delivered,
                             struct xrt_lift_blob_info *out_info);

//! XrLiftPriorityDXR (0 paused .. 3 high).
xrt_result_t
ipc_client_lift_set_priority(struct ipc_connection *ipc_c, uint64_t stream_id, uint32_t priority);

xrt_result_t
ipc_client_lift_stats(struct ipc_connection *ipc_c, uint64_t stream_id, struct xrt_lift_stream_stats *out_stats);

//! Latch the lift-flagged rects of the NEXT weave_submit on this connection.
xrt_result_t
ipc_client_lift_weave_rects(struct ipc_connection *ipc_c, uint32_t count, const struct xrt_lift_weave_rect *rects);

#ifdef __cplusplus
}
#endif
