// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common client side code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_config_os.h"

#include "util/u_threading.h"
#include "util/u_logging.h"

#include "shared/ipc_utils.h"
#include "shared/ipc_protocol.h"
#include "shared/ipc_message_channel.h"

#include <stdio.h>


/*
 *
 * Logging
 *
 */

#define IPC_TRACE(IPC_C, ...) U_LOG_IFL_T((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_DEBUG(IPC_C, ...) U_LOG_IFL_D((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_INFO(IPC_C, ...) U_LOG_IFL_I((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_WARN(IPC_C, ...) U_LOG_IFL_W((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_ERROR(IPC_C, ...) U_LOG_IFL_E((IPC_C)->imc.log_level, __VA_ARGS__)

#define IPC_CHK_AND_RET(IPC_C, ...) U_LOG_CHK_AND_RET((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_CHK_WITH_GOTO(IPC_C, ...) U_LOG_CHK_WITH_GOTO((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_CHK_WITH_RET(IPC_C, ...) U_LOG_CHK_WITH_RET((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_CHK_ONLY_PRINT(IPC_C, ...) U_LOG_CHK_ONLY_PRINT((IPC_C)->imc.log_level, __VA_ARGS__)
#define IPC_CHK_ALWAYS_RET(IPC_C, ...) U_LOG_CHK_ALWAYS_RET((IPC_C)->imc.log_level, __VA_ARGS__)


/*
 *
 * Structs
 *
 */

struct xrt_compositor_native;


/*!
 * Connection.
 */
struct ipc_connection
{
	struct ipc_message_channel imc;

	struct ipc_shared_memory *ism;
	xrt_shmem_handle_t ism_handle;

	struct os_mutex mutex;

#ifdef XRT_OS_ANDROID
	struct ipc_client_android *ica;
#endif // XRT_OS_ANDROID
};


/*
 *
 * Internal functions.
 *
 */

/*!
 * Create an IPC client system compositor.
 *
 * It owns a special implementation of the @ref xrt_system_compositor interface.
 *
 * This actually creates an IPC client "native" compositor with deferred
 * initialization. The @ref ipc_client_create_native_compositor function
 * actually completes the deferred initialization of the compositor, effectively
 * finishing creation of a compositor IPC proxy.
 *
 * @param ipc_c IPC connection
 * @param xina Optional native image allocator for client-side allocation. Takes
 * ownership if one is supplied.
 * @param xdev Taken in but not used currently @todo remove this param?
 * @param[out] out_xcs Pointer to receive the created xrt_system_compositor.
 */
xrt_result_t
ipc_client_create_system_compositor(struct ipc_connection *ipc_c,
                                    struct xrt_image_native_allocator *xina,
                                    struct xrt_device *xdev,
                                    struct xrt_system_compositor **out_xcs);

/*!
 * Create a native compositor from a system compositor, this is used instead
 * of the normal xrt_system_compositor::create_native_compositor function
 * because it doesn't support events being generated on the app side. This will
 * also create the session on the service side.
 *
 * @param xsysc        IPC created system compositor.
 * @param xsi          Session information struct.
 * @param[out] out_xcn Pointer to receive the created xrt_compositor_native.
 */
xrt_result_t
ipc_client_create_native_compositor(struct xrt_system_compositor *xsysc,
                                    const struct xrt_session_info *xsi,
                                    struct xrt_compositor_native **out_xcn);

struct xrt_device *
ipc_client_hmd_create(struct ipc_connection *ipc_c, struct xrt_tracking_origin *xtrack, uint32_t device_id);

struct xrt_device *
ipc_client_device_create(struct ipc_connection *ipc_c, struct xrt_tracking_origin *xtrack, uint32_t device_id);

struct xrt_system *
ipc_client_system_create(struct ipc_connection *ipc_c, struct xrt_system_compositor *xsysc);

struct xrt_space_overseer *
ipc_client_space_overseer_create(struct ipc_connection *ipc_c);

struct xrt_system_devices *
ipc_client_system_devices_create(struct ipc_connection *ipc_c);

struct xrt_session *
ipc_client_session_create(struct ipc_connection *ipc_c);

/*!
 * Pull per-client shell window metrics over IPC.
 *
 * Only safe to call when `xc` is known to be an ipc_client_compositor
 * (e.g. the OpenXR state tracker's IPC branch in
 * `oxr_session_get_window_metrics`). Returns false if the call fails
 * or the server reports no valid per-client slot — caller should fall
 * back to display-dimension metrics.
 */
struct xrt_compositor;
struct xrt_window_metrics;

bool
comp_ipc_client_compositor_get_window_metrics(struct xrt_compositor *xc, struct xrt_window_metrics *out_metrics);

/*!
 * Workspace controller bridges (XR_EXT_spatial_workspace).
 *
 * Thin accessors used by the OpenXR state tracker to dispatch workspace
 * extension calls over IPC. Each extracts the underlying ipc_connection from
 * the compositor and calls the matching generated RPC. Same gating contract
 * as comp_ipc_client_compositor_get_window_metrics — only valid when `xc` is
 * an ipc_client_compositor.
 */
xrt_result_t
comp_ipc_client_compositor_workspace_activate(struct xrt_compositor *xc);

xrt_result_t
comp_ipc_client_compositor_workspace_deactivate(struct xrt_compositor *xc);

xrt_result_t
comp_ipc_client_compositor_workspace_get_state(struct xrt_compositor *xc, bool *out_active);

xrt_result_t
comp_ipc_client_compositor_workspace_add_capture_client(struct xrt_compositor *xc,
                                                        uint64_t hwnd,
                                                        uint32_t *out_client_id);

xrt_result_t
comp_ipc_client_compositor_workspace_remove_capture_client(struct xrt_compositor *xc, uint32_t client_id);

struct xrt_pose;

xrt_result_t
comp_ipc_client_compositor_workspace_set_window_pose(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     const struct xrt_pose *pose,
                                                     float width_m,
                                                     float height_m);

xrt_result_t
comp_ipc_client_compositor_workspace_get_window_pose(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     struct xrt_pose *out_pose,
                                                     float *out_width_m,
                                                     float *out_height_m);

xrt_result_t
comp_ipc_client_compositor_workspace_set_window_visibility(struct xrt_compositor *xc,
                                                           uint32_t client_id,
                                                           bool visible);

xrt_result_t
comp_ipc_client_compositor_workspace_hit_test(struct xrt_compositor *xc,
                                              int32_t cursor_x,
                                              int32_t cursor_y,
                                              uint32_t *out_client_id,
                                              float *out_local_u,
                                              float *out_local_v,
                                              uint32_t *out_hit_region);

xrt_result_t
comp_ipc_client_compositor_workspace_set_focused_client(struct xrt_compositor *xc, uint32_t client_id);

xrt_result_t
comp_ipc_client_compositor_workspace_get_focused_client(struct xrt_compositor *xc, uint32_t *out_client_id);

/*!
 * Phase 2.D: drain the workspace public-event ring.
 *
 * The state tracker passes an opaque buffer + size so it does not need to
 * include shared/ipc_protocol.h. The wire-format event size is
 * @p event_stride bytes; capacity is @p event_capacity entries (clamped to
 * IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX server-side). On return, @p out_count
 * is the number of events actually written.
 */
xrt_result_t
comp_ipc_client_compositor_workspace_enumerate_input_events(struct xrt_compositor *xc,
                                                            uint32_t requested_capacity,
                                                            uint32_t *out_count,
                                                            void *out_events_buf,
                                                            size_t event_stride,
                                                            size_t event_buf_capacity);

xrt_result_t
comp_ipc_client_compositor_workspace_pointer_capture_set(struct xrt_compositor *xc,
                                                         bool enabled,
                                                         uint32_t button);

/*!
 * Phase 2.I-prequel: capture the workspace composite frame to disk.
 *
 * Bridge accepts raw fields (matching the wire-format struct ipc_capture_request
 * input + ipc_capture_result output) so st_oxr does not see IPC types. The state
 * tracker translates between public XrWorkspaceCaptureRequestEXT/ResultEXT and
 * these primitives.
 */
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
                                                   float out_eye_right_m[3]);

/*!
 * Phase 2.I-prequel: workspace client enumeration. Bridge accepts a primitive
 * id array so st_oxr does not include IPC types.
 */
xrt_result_t
comp_ipc_client_compositor_workspace_enumerate_clients(struct xrt_compositor *xc,
                                                       uint32_t capacity,
                                                       uint32_t *out_count,
                                                       uint32_t *out_ids);

/*!
 * Phase 2.I-prequel: per-client metadata. Bridge unpacks struct ipc_app_state
 * into raw out-params so st_oxr stays free of IPC types. The state tracker
 * projects these into XrWorkspaceClientInfoEXT.
 */
xrt_result_t
comp_ipc_client_compositor_workspace_get_client_info(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     char *out_name,
                                                     size_t name_capacity,
                                                     uint64_t *out_pid,
                                                     uint32_t *out_z_order,
                                                     bool *out_is_focused,
                                                     bool *out_is_visible);

/*!
 * Launcher bridges (XR_EXT_app_launcher).
 *
 * Same gating contract as the workspace_* family — only valid when `xc` is
 * an ipc_client_compositor. The state tracker forward-declares these and
 * the runtime DLL links ipc_client so symbols resolve at link time.
 */
xrt_result_t
comp_ipc_client_compositor_launcher_clear_apps(struct xrt_compositor *xc);

xrt_result_t
comp_ipc_client_compositor_launcher_add_app(struct xrt_compositor *xc, const char *name, const char *icon_path);

xrt_result_t
comp_ipc_client_compositor_launcher_set_visible(struct xrt_compositor *xc, bool visible);

xrt_result_t
comp_ipc_client_compositor_launcher_poll_click(struct xrt_compositor *xc, int64_t *out_tile_index);

xrt_result_t
comp_ipc_client_compositor_launcher_set_running_tile_mask(struct xrt_compositor *xc, uint64_t mask);
