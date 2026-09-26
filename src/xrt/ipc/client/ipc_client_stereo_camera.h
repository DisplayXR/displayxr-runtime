// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) client side: connection-level calls,
 *         shared by the IPC instance's @ref xrt_stereo_camera_client aspect
 *         and `displayxr-cli camera`.
 *
 * Streams belong to the IPC connection (not a session), so every call takes a
 * bare @ref ipc_connection.
 *
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_stereo_camera.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;

xrt_result_t
ipc_client_stereo_camera_count(struct ipc_connection *ipc_c, uint32_t *out_count);

xrt_result_t
ipc_client_stereo_camera_get_properties(struct ipc_connection *ipc_c,
                                        uint32_t index,
                                        struct xrt_stereo_camera_properties *out_props);

xrt_result_t
ipc_client_stereo_camera_get_calibration(struct ipc_connection *ipc_c,
                                         uint64_t camera_id,
                                         uint32_t output,
                                         struct xrt_stereo_camera_calibration *out_calib);

xrt_result_t
ipc_client_stereo_camera_stream_create(struct ipc_connection *ipc_c,
                                       const struct xrt_stereo_camera_stream_request *req,
                                       uint64_t *out_stream_id);

xrt_result_t
ipc_client_stereo_camera_stream_destroy(struct ipc_connection *ipc_c, uint64_t stream_id);

xrt_result_t
ipc_client_stereo_camera_stream_start(struct ipc_connection *ipc_c, uint64_t stream_id);

xrt_result_t
ipc_client_stereo_camera_stream_stop(struct ipc_connection *ipc_c, uint64_t stream_id);

//! The started stream's ring section + wake handle (NEW handles, caller-owned).
xrt_result_t
ipc_client_stereo_camera_stream_get_transport(struct ipc_connection *ipc_c,
                                              uint64_t stream_id,
                                              struct xrt_stereo_camera_stream_layout *out_layout,
                                              xrt_shmem_handle_t *out_section,
                                              xrt_graphics_sync_handle_t *out_wake);

xrt_result_t
ipc_client_stereo_camera_acquire(struct ipc_connection *ipc_c,
                                 uint64_t stream_id,
                                 bool *out_ready,
                                 struct xrt_stereo_camera_frame_info *out_frame);

xrt_result_t
ipc_client_stereo_camera_stats(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               struct xrt_stereo_camera_stream_stats *out_stats);

/*!
 * The @ref xrt_stereo_camera_client aspect over @p ipc_c (which must outlive
 * it). Caller frees with free().
 */
struct xrt_stereo_camera_client *
ipc_client_stereo_camera_client_create(struct ipc_connection *ipc_c);

#ifdef __cplusplus
}
#endif
