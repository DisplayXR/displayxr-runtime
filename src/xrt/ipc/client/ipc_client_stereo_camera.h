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

//! @p out_app (may be NULL): the frame's XR_DXR_app_share metadata (valid = 0 for a camera).
xrt_result_t
ipc_client_stereo_camera_acquire(struct ipc_connection *ipc_c,
                                 uint64_t stream_id,
                                 bool *out_ready,
                                 struct xrt_stereo_camera_frame_info *out_frame,
                                 struct xrt_app_share_frame_info *out_app);

/*!
 * ADR-061: the XR_DXR_app_share description of APP_CONTENT entry @p camera_id.
 *
 * An in-process app is offered by its `ipc_client_service_link` (the ADR-049
 * lift link generalised into ONE sessionless service link per in-process
 * session with capabilities {lift, share}, ADR-049 Amendment 1); the service
 * lists it as XRT_STEREO_CAMERA_SOURCE_APP_LINK.
 */
xrt_result_t
ipc_client_stereo_camera_get_app_source(struct ipc_connection *ipc_c,
                                        uint64_t camera_id,
                                        struct xrt_app_share_source_info *out_info);

xrt_result_t
ipc_client_stereo_camera_stats(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               struct xrt_stereo_camera_stream_stats *out_stats);

/*!
 * The @ref xrt_stereo_camera_client aspect over @p ipc_c (which must outlive
 * it). Caller frees with free().
 */
xrt_result_t
ipc_client_stereo_camera_poll_event(struct ipc_connection *ipc_c,
                                    bool *out_has,
                                    struct xrt_stereo_camera_event *out_event);

xrt_result_t
ipc_client_stereo_camera_control(struct ipc_connection *ipc_c, uint32_t op, uint32_t arg, uint32_t *out_value);

/*
 * Spec v8 (modes, controls, stills).
 */

//! get_calibration for the mode of per-eye size @p eye_width x @p eye_height (0 x 0 = default).
xrt_result_t
ipc_client_stereo_camera_get_calibration_mode(struct ipc_connection *ipc_c,
                                              uint64_t camera_id,
                                              uint32_t output,
                                              uint32_t eye_width,
                                              uint32_t eye_height,
                                              struct xrt_stereo_camera_calibration *out_calib);

//! Spec v8 §5.9: the online vertical-alignment correction of camera @p camera_id.
xrt_result_t
ipc_client_stereo_camera_get_refinement(struct ipc_connection *ipc_c,
                                        uint64_t camera_id,
                                        struct xrt_stereo_camera_refinement *out_refinement);

//! acquire, plus the frame's capture state (@p out_result may be NULL).
xrt_result_t
ipc_client_stereo_camera_acquire_ex(struct ipc_connection *ipc_c,
                                    uint64_t stream_id,
                                    bool *out_ready,
                                    struct xrt_stereo_camera_frame_info *out_frame,
                                    struct xrt_app_share_frame_info *out_app,
                                    struct xrt_stereo_camera_capture_result *out_result);

xrt_result_t
ipc_client_stereo_camera_get_modes(struct ipc_connection *ipc_c,
                                   uint64_t camera_id,
                                   struct xrt_stereo_camera_mode_list *out_modes);

xrt_result_t
ipc_client_stereo_camera_get_control_caps(struct ipc_connection *ipc_c,
                                          uint64_t camera_id,
                                          struct xrt_stereo_camera_control_caps *out_caps);

xrt_result_t
ipc_client_stereo_camera_set_controls(struct ipc_connection *ipc_c,
                                      uint64_t stream_id,
                                      const struct xrt_stereo_camera_controls *controls);

xrt_result_t
ipc_client_stereo_camera_release_controls(struct ipc_connection *ipc_c, uint64_t stream_id);

xrt_result_t
ipc_client_stereo_camera_request_still(struct ipc_connection *ipc_c,
                                       uint64_t stream_id,
                                       const struct xrt_stereo_camera_still_request *req,
                                       uint64_t *out_request_id);

//! @p out_section: a NEW read-only handle the caller closes.
xrt_result_t
ipc_client_stereo_camera_acquire_still(struct ipc_connection *ipc_c,
                                       uint64_t stream_id,
                                       uint64_t request_id,
                                       struct xrt_stereo_camera_still_info *out_info,
                                       xrt_shmem_handle_t *out_section);

xrt_result_t
ipc_client_stereo_camera_release_still(struct ipc_connection *ipc_c, uint64_t stream_id, uint64_t request_id);

struct xrt_stereo_camera_client *
ipc_client_stereo_camera_client_create(struct ipc_connection *ipc_c);

#ifdef __cplusplus
}
#endif
