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

//! @p aux_outputs / @p aux_depth_format: XR_DXR_lift v3 auxiliary depth request
//! (XRT_DP_LIFT_AUX_*, DXGI hint); 0 / 0 = none.
xrt_result_t
ipc_client_lift_stream_create(struct ipc_connection *ipc_c,
                              uint32_t mode,
                              uint32_t content_hint,
                              float input_scale,
                              uint32_t aux_outputs,
                              uint32_t aux_depth_format,
                              uint64_t *out_stream_id);

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
                       const struct xrt_lift_view_control *view_control,
                       const struct xrt_lift_rig *rig,
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

//! XR_DXR_lift v3: the stream's auxiliary depth export texture (a NEW
//! handle, caller-owned) + its layout.
xrt_result_t
ipc_client_lift_get_depth_output(struct ipc_connection *ipc_c,
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

/*!
 * conv_hint_interest (ADR-053, #2005): this connection belongs to an instance
 * that enabled XR_DXR_lift. The service's PrewarmOn policy decides whether to
 * warm the conversion tasks in @p task_mask (bit 1 << XRT_CONV_TASK_*; 0 = the
 * default set) now. Fire and forget: never blocks on a provider.
 */
xrt_result_t
ipc_client_conv_hint_interest(struct ipc_connection *ipc_c, uint64_t task_mask);

/*
 * XR_DXR_lift spec 4 (ADR-053 P3, #2054): the conv_* family. Thin wrappers
 * over the generated calls, so the state tracker needs no generated header.
 * Any of them latches this connection for the task / provider events.
 */

struct xrt_conv_wire_event;
struct xrt_conv_wire_result;
struct xrt_conv_wire_stream_created;
struct xrt_conv_wire_stream_desc;
struct xrt_conv_wire_task_desc;

//! One task's effective capability + readiness, and the image kinds the service takes / gives for it.
xrt_result_t
ipc_client_conv_enum_task(struct ipc_connection *ipc_c,
                          uint32_t task,
                          struct xrt_conv_wire_task_desc *out_desc,
                          uint64_t *out_image_kinds,
                          uint64_t *out_export_kinds);

xrt_result_t
ipc_client_conv_warm(struct ipc_connection *ipc_c, uint32_t task);

xrt_result_t
ipc_client_conv_stream_create(struct ipc_connection *ipc_c,
                              const struct xrt_conv_wire_stream_desc *desc,
                              uint64_t required_outputs,
                              uint32_t legacy_mode,
                              float input_scale,
                              struct xrt_conv_wire_stream_created *out_created);

xrt_result_t
ipc_client_conv_stream_reset(struct ipc_connection *ipc_c, uint64_t stream_id);

//! The v3 acquire plus the result's spec-4 metadata (one round trip).
xrt_result_t
ipc_client_conv_acquire(struct ipc_connection *ipc_c,
                        uint64_t stream_id,
                        bool *out_ready,
                        bool *out_discontinuity,
                        struct xrt_lift_result *out_result,
                        struct xrt_conv_wire_result *out_meta);

xrt_result_t
ipc_client_conv_poll_event(struct ipc_connection *ipc_c, bool *out_has, struct xrt_conv_wire_event *out_event);

struct xrt_conv_wire_image;

/*!
 * conv_submit_frame: the v3 submit (same params, viewpoints, view control,
 * rig) of a spec-4 image @p image (kind, sync, CPU pitch) over the region
 * (@p region_x, @p region_y, @p width x @p height). @p handle (+ @p
 * sync_handle for a fence) are the caller's; the transport duplicates them.
 */
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
                             uint64_t *out_frame_id);

//! A CPU-export stream's mapping (a NEW handle, caller-owned) and its tightly packed layout.
xrt_result_t
ipc_client_conv_get_cpu_output(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               bool *out_have,
                               uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_format,
                               uint64_t *out_bytes,
                               xrt_graphics_buffer_handle_t *out_handle);

#ifdef __cplusplus
}
#endif
