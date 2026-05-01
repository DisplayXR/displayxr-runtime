// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_spatial_workspace API entry points.
 * @author DisplayXR
 * @ingroup oxr_api
 *
 * Phase 2.A wrapping of the workspace IPC RPCs as OpenXR extension functions.
 * Each entry point validates session/extension state, gates on IPC mode (the
 * workspace controller is always an external process talking to the runtime
 * over IPC), then dispatches via a thin compositor-side bridge that lives in
 * ipc_client_compositor.c. The bridge functions are forward-declared here
 * because st_oxr does not pull the ipc_client include path; the runtime DLL
 * links ipc_client so the symbols resolve at link time. Same pattern as
 * comp_ipc_client_compositor_get_window_metrics in oxr_session.c.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_results.h"

// Phase 2.C: chrome layout marshalling. Pull the IPC POD typedef so the
// dispatch wrapper can fill it directly. The CMake include path
// (state_trackers/oxr/../../ipc) lets us write this as "shared/...".
#include "shared/ipc_protocol.h"

#include <openxr/XR_EXT_spatial_workspace.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_spatial_workspace

// Forward declarations of the IPC-bridge wrappers. Defined in
// src/xrt/ipc/client/ipc_client_compositor.c. See header comment above.
struct xrt_compositor;
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
xrt_result_t
comp_ipc_client_compositor_workspace_enumerate_clients(struct xrt_compositor *xc,
                                                       uint32_t capacity,
                                                       uint32_t *out_count,
                                                       uint32_t *out_ids);
xrt_result_t
comp_ipc_client_compositor_workspace_get_client_info(struct xrt_compositor *xc,
                                                     uint32_t client_id,
                                                     char *out_name,
                                                     size_t name_capacity,
                                                     uint64_t *out_pid,
                                                     uint32_t *out_z_order,
                                                     bool *out_is_focused,
                                                     bool *out_is_visible);
xrt_result_t
comp_ipc_client_compositor_workspace_request_client_exit(struct xrt_compositor *xc, uint32_t client_id);
xrt_result_t
comp_ipc_client_compositor_workspace_request_client_fullscreen(struct xrt_compositor *xc,
                                                               uint32_t client_id,
                                                               bool fullscreen);
// Phase 2.C: chrome swapchain bridges. Forward-declared with the IPC POD layout
// type from ipc_protocol.h.
struct ipc_workspace_chrome_layout;
struct xrt_swapchain;
xrt_result_t
comp_ipc_client_compositor_workspace_register_chrome_swapchain(struct xrt_compositor *xc,
                                                               uint32_t client_id,
                                                               uint32_t swapchain_id);
xrt_result_t
comp_ipc_client_compositor_workspace_unregister_chrome_swapchain(struct xrt_compositor *xc,
                                                                 uint32_t swapchain_id);
xrt_result_t
comp_ipc_client_compositor_workspace_set_chrome_layout(struct xrt_compositor *xc,
                                                       uint32_t client_id,
                                                       const struct ipc_workspace_chrome_layout *layout);
uint32_t
comp_ipc_client_compositor_get_swapchain_id(struct xrt_swapchain *xsc);


/*
 * Helpers
 */

static bool
session_is_ipc_client(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		return false;
	}
	if (sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	    sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	    sess->is_vk_native_compositor) {
		return false;
	}
	// In-process multi-compositor path; not IPC.
	if (sess->sys->xsysc->xmcc != NULL) {
		return false;
	}
	return true;
}

static XrResult
xret_to_xr_result(struct oxr_logger *log, xrt_result_t xret, const char *label)
{
	switch (xret) {
	case XRT_SUCCESS: return XR_SUCCESS;
	// Phase 2.0 PID-mismatch path — caller is not the registered workspace controller.
	case XRT_ERROR_NOT_AUTHORIZED:
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: not authorized as workspace controller",
		                 label);
	case XRT_ERROR_IPC_FAILURE:
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: IPC failure", label);
	default: return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: xrt_result=%d", label, (int)xret);
	}
}


/*
 * Lifecycle
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrActivateSpatialWorkspaceEXT(XrSession session)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrActivateSpatialWorkspaceEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrActivateSpatialWorkspaceEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_activate(&sess->xcn->base);
	return xret_to_xr_result(&log, xret, "workspace_activate");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDeactivateSpatialWorkspaceEXT(XrSession session)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrDeactivateSpatialWorkspaceEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrDeactivateSpatialWorkspaceEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_deactivate(&sess->xcn->base);
	return xret_to_xr_result(&log, xret, "workspace_deactivate");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetSpatialWorkspaceStateEXT(XrSession session, XrBool32 *out_active)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetSpatialWorkspaceStateEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, out_active);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetSpatialWorkspaceStateEXT requires an IPC-mode session");
	}

	bool active = false;
	xrt_result_t xret = comp_ipc_client_compositor_workspace_get_state(&sess->xcn->base, &active);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_get_state");
	}
	*out_active = active ? XR_TRUE : XR_FALSE;
	return XR_SUCCESS;
}


/*
 * Capture clients
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAddWorkspaceCaptureClientEXT(XrSession session,
                                   uint64_t nativeWindow,
                                   const char *nameOptional,
                                   XrWorkspaceClientId *outClientId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrAddWorkspaceCaptureClientEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, outClientId);

	if (nativeWindow == 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrAddWorkspaceCaptureClientEXT: nativeWindow must be a valid HWND");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrAddWorkspaceCaptureClientEXT requires an IPC-mode session");
	}

	// nameOptional is part of the public API but proto.json does not yet carry
	// the field. Phase 2.A logs the label and drops it; a follow-up sub-phase
	// extends the IPC wire format and threads it through the handler.
	if (nameOptional != NULL) {
		U_LOG_I("xrAddWorkspaceCaptureClientEXT: name=\"%s\" (advisory; not yet propagated through IPC)",
		        nameOptional);
	}

	uint32_t client_id = 0;
	xrt_result_t xret = comp_ipc_client_compositor_workspace_add_capture_client(&sess->xcn->base, nativeWindow,
	                                                                            &client_id);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_add_capture_client");
	}
	*outClientId = (XrWorkspaceClientId)client_id;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRemoveWorkspaceCaptureClientEXT(XrSession session, XrWorkspaceClientId clientId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRemoveWorkspaceCaptureClientEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrRemoveWorkspaceCaptureClientEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrRemoveWorkspaceCaptureClientEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_remove_capture_client(&sess->xcn->base,
	                                                                               (uint32_t)clientId);
	return xret_to_xr_result(&log, xret, "workspace_remove_capture_client");
}


/*
 * Window pose + visibility (spec_version 2)
 *
 * XrPosef and struct xrt_pose are layout-compatible (quaternion + vec3,
 * x/y/z/w ordering on the quat, x/y/z on the vec3). The translation here
 * is field-by-field rather than memcpy to keep the boundary explicit and
 * survive any future divergence in either type.
 */

static void
xr_pose_to_xrt_pose(const XrPosef *in, struct xrt_pose *out)
{
	out->orientation.x = in->orientation.x;
	out->orientation.y = in->orientation.y;
	out->orientation.z = in->orientation.z;
	out->orientation.w = in->orientation.w;
	out->position.x = in->position.x;
	out->position.y = in->position.y;
	out->position.z = in->position.z;
}

static void
xrt_pose_to_xr_pose(const struct xrt_pose *in, XrPosef *out)
{
	out->orientation.x = in->orientation.x;
	out->orientation.y = in->orientation.y;
	out->orientation.z = in->orientation.z;
	out->orientation.w = in->orientation.w;
	out->position.x = in->position.x;
	out->position.y = in->position.y;
	out->position.z = in->position.z;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientWindowPoseEXT(XrSession session,
                                      XrWorkspaceClientId clientId,
                                      const XrPosef *pose,
                                      float widthMeters,
                                      float heightMeters)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetWorkspaceClientWindowPoseEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, pose);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientWindowPoseEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}
	if (!(widthMeters > 0.0f) || !(heightMeters > 0.0f)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientWindowPoseEXT: widthMeters and heightMeters must be > 0");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetWorkspaceClientWindowPoseEXT requires an IPC-mode session");
	}

	struct xrt_pose xpose;
	xr_pose_to_xrt_pose(pose, &xpose);

	xrt_result_t xret = comp_ipc_client_compositor_workspace_set_window_pose(
	    &sess->xcn->base, (uint32_t)clientId, &xpose, widthMeters, heightMeters);
	return xret_to_xr_result(&log, xret, "workspace_set_window_pose");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceClientWindowPoseEXT(XrSession session,
                                      XrWorkspaceClientId clientId,
                                      XrPosef *outPose,
                                      float *outWidthMeters,
                                      float *outHeightMeters)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetWorkspaceClientWindowPoseEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, outPose);
	OXR_VERIFY_ARG_NOT_NULL(&log, outWidthMeters);
	OXR_VERIFY_ARG_NOT_NULL(&log, outHeightMeters);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrGetWorkspaceClientWindowPoseEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetWorkspaceClientWindowPoseEXT requires an IPC-mode session");
	}

	struct xrt_pose xpose = {0};
	float w = 0.0f, h = 0.0f;
	xrt_result_t xret = comp_ipc_client_compositor_workspace_get_window_pose(
	    &sess->xcn->base, (uint32_t)clientId, &xpose, &w, &h);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_get_window_pose");
	}
	xrt_pose_to_xr_pose(&xpose, outPose);
	*outWidthMeters = w;
	*outHeightMeters = h;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientVisibilityEXT(XrSession session, XrWorkspaceClientId clientId, XrBool32 visible)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetWorkspaceClientVisibilityEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientVisibilityEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetWorkspaceClientVisibilityEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_set_window_visibility(
	    &sess->xcn->base, (uint32_t)clientId, visible == XR_TRUE);
	return xret_to_xr_result(&log, xret, "workspace_set_window_visibility");
}


/*
 * Hit-test (spec_version 3)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWorkspaceHitTestEXT(XrSession session,
                          int32_t cursorX,
                          int32_t cursorY,
                          XrWorkspaceClientId *outClientId,
                          XrVector2f *outLocalUV,
                          XrWorkspaceHitRegionEXT *outHitRegion)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWorkspaceHitTestEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, outClientId);
	OXR_VERIFY_ARG_NOT_NULL(&log, outLocalUV);
	OXR_VERIFY_ARG_NOT_NULL(&log, outHitRegion);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWorkspaceHitTestEXT requires an IPC-mode session");
	}

	uint32_t client_id = 0;
	float u = 0.0f, v = 0.0f;
	uint32_t region = 0;
	xrt_result_t xret = comp_ipc_client_compositor_workspace_hit_test(&sess->xcn->base, cursorX, cursorY,
	                                                                  &client_id, &u, &v, &region);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_hit_test");
	}
	*outClientId = (XrWorkspaceClientId)client_id;
	outLocalUV->x = u;
	outLocalUV->y = v;
	*outHitRegion = (XrWorkspaceHitRegionEXT)region;
	return XR_SUCCESS;
}


/*
 * Focus control (spec_version 4)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceFocusedClientEXT(XrSession session, XrWorkspaceClientId clientId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetWorkspaceFocusedClientEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	// XR_NULL_WORKSPACE_CLIENT_ID is valid here — clears focus.

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetWorkspaceFocusedClientEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_set_focused_client(&sess->xcn->base,
	                                                                            (uint32_t)clientId);
	return xret_to_xr_result(&log, xret, "workspace_set_focused_client");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceFocusedClientEXT(XrSession session, XrWorkspaceClientId *outClientId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetWorkspaceFocusedClientEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, outClientId);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetWorkspaceFocusedClientEXT requires an IPC-mode session");
	}

	uint32_t client_id = 0;
	xrt_result_t xret = comp_ipc_client_compositor_workspace_get_focused_client(&sess->xcn->base, &client_id);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_get_focused_client");
	}
	*outClientId = (XrWorkspaceClientId)client_id;
	return XR_SUCCESS;
}


/*
 * Input event drain + pointer capture (spec_version 4)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateWorkspaceInputEventsEXT(XrSession session,
                                       uint32_t capacityInput,
                                       uint32_t *countOutput,
                                       XrWorkspaceInputEventEXT *events)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrEnumerateWorkspaceInputEventsEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);

	if (capacityInput > 0 && events == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrEnumerateWorkspaceInputEventsEXT: events must not be NULL when capacityInput > 0");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrEnumerateWorkspaceInputEventsEXT requires an IPC-mode session");
	}

	uint32_t count = 0;
	// Bridge translates wire events → XrWorkspaceInputEventEXT records and
	// writes them into the caller-supplied array. Pass the array through
	// as an opaque buffer so st_oxr does not need to peek inside.
	xrt_result_t xret = comp_ipc_client_compositor_workspace_enumerate_input_events(
	    &sess->xcn->base, capacityInput, &count, events, sizeof(XrWorkspaceInputEventEXT),
	    (size_t)capacityInput);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_enumerate_input_events");
	}
	*countOutput = count;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnableWorkspacePointerCaptureEXT(XrSession session, uint32_t button)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrEnableWorkspacePointerCaptureEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrEnableWorkspacePointerCaptureEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_pointer_capture_set(&sess->xcn->base, true, button);
	return xret_to_xr_result(&log, xret, "workspace_pointer_capture_set(enabled)");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDisableWorkspacePointerCaptureEXT(XrSession session)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrDisableWorkspacePointerCaptureEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrDisableWorkspacePointerCaptureEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_pointer_capture_set(&sess->xcn->base, false, 0);
	return xret_to_xr_result(&log, xret, "workspace_pointer_capture_set(disabled)");
}


/*
 * Frame capture (spec_version 5)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCaptureWorkspaceFrameEXT(XrSession session,
                               const XrWorkspaceCaptureRequestEXT *request,
                               XrWorkspaceCaptureResultEXT *result)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCaptureWorkspaceFrameEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, request);
	OXR_VERIFY_ARG_NOT_NULL(&log, result);

	if (request->type != XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCaptureWorkspaceFrameEXT: request->type must be "
		                 "XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCaptureWorkspaceFrameEXT requires an IPC-mode session");
	}

	uint64_t ts_ns = 0;
	uint32_t aw = 0, ah = 0, ew = 0, eh = 0, vw = 0, tc = 0, tr = 0;
	float dw_m = 0.0f, dh_m = 0.0f;
	float eye_l[3] = {0}, eye_r[3] = {0};
	xrt_result_t xret = comp_ipc_client_compositor_workspace_capture_frame(
	    &sess->xcn->base, request->pathPrefix, (uint32_t)request->flags, &ts_ns, &aw, &ah, &ew, &eh, &vw,
	    &tc, &tr, &dw_m, &dh_m, eye_l, eye_r);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_capture_frame");
	}

	result->type = XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT;
	result->next = NULL;
	result->timestampNs = ts_ns;
	result->atlasWidth = aw;
	result->atlasHeight = ah;
	result->eyeWidth = ew;
	result->eyeHeight = eh;
	result->viewsWritten = (XrWorkspaceCaptureFlagsEXT)vw;
	result->tileColumns = tc;
	result->tileRows = tr;
	result->displayWidthM = dw_m;
	result->displayHeightM = dh_m;
	result->eyeLeftM[0] = eye_l[0];
	result->eyeLeftM[1] = eye_l[1];
	result->eyeLeftM[2] = eye_l[2];
	result->eyeRightM[0] = eye_r[0];
	result->eyeRightM[1] = eye_r[1];
	result->eyeRightM[2] = eye_r[2];
	return XR_SUCCESS;
}


/*
 * Client enumeration (spec_version 5)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateWorkspaceClientsEXT(XrSession session,
                                   uint32_t capacityInput,
                                   uint32_t *countOutput,
                                   XrWorkspaceClientId *clientIds)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrEnumerateWorkspaceClientsEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);

	if (capacityInput > 0 && clientIds == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrEnumerateWorkspaceClientsEXT: clientIds must not be NULL when capacityInput > 0");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrEnumerateWorkspaceClientsEXT requires an IPC-mode session");
	}

	uint32_t count = 0;
	// XrWorkspaceClientId == uint32_t — pass clientIds buffer through directly.
	xrt_result_t xret = comp_ipc_client_compositor_workspace_enumerate_clients(
	    &sess->xcn->base, capacityInput, &count, (uint32_t *)clientIds);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_enumerate_clients");
	}
	*countOutput = count;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceClientInfoEXT(XrSession session,
                                XrWorkspaceClientId clientId,
                                XrWorkspaceClientInfoEXT *info)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetWorkspaceClientInfoEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, info);

	if (info->type != XR_TYPE_WORKSPACE_CLIENT_INFO_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrGetWorkspaceClientInfoEXT: info->type must be "
		                 "XR_TYPE_WORKSPACE_CLIENT_INFO_EXT");
	}

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrGetWorkspaceClientInfoEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetWorkspaceClientInfoEXT requires an IPC-mode session");
	}

	uint64_t pid = 0;
	uint32_t z_order = 0;
	bool is_focused = false;
	bool is_visible = false;
	char name[XR_MAX_APPLICATION_NAME_SIZE] = {0};
	xrt_result_t xret = comp_ipc_client_compositor_workspace_get_client_info(
	    &sess->xcn->base, (uint32_t)clientId, name, sizeof(name), &pid, &z_order, &is_focused, &is_visible);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_get_client_info");
	}

	info->clientId = clientId;
	// Capture-client ids (slot+1000) and OpenXR-client ids share the same
	// numbering space but are issued by different code paths. Enumerate
	// only returns OpenXR clients today, so we report OPENXR_3D here. A
	// future workspace_get_client_info that also resolves capture-client
	// ids would distinguish via the slot+1000 range.
	info->clientType = XR_WORKSPACE_CLIENT_TYPE_OPENXR_3D_EXT;
	memcpy(info->name, name, sizeof(info->name));
	info->name[sizeof(info->name) - 1] = '\0';
	info->pid = pid;
	info->isFocused = is_focused ? XR_TRUE : XR_FALSE;
	info->isVisible = is_visible ? XR_TRUE : XR_FALSE;
	info->zOrder = z_order;
	return XR_SUCCESS;
}


/*
 * Lifecycle requests (spec_version 6)
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestWorkspaceClientExitEXT(XrSession session, XrWorkspaceClientId clientId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRequestWorkspaceClientExitEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrRequestWorkspaceClientExitEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrRequestWorkspaceClientExitEXT requires an IPC-mode session");
	}

	xrt_result_t xret =
	    comp_ipc_client_compositor_workspace_request_client_exit(&sess->xcn->base, (uint32_t)clientId);
	return xret_to_xr_result(&log, xret, "workspace_request_client_exit");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestWorkspaceClientFullscreenEXT(XrSession session, XrWorkspaceClientId clientId, XrBool32 fullscreen)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRequestWorkspaceClientFullscreenEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);

	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(
		    &log, XR_ERROR_VALIDATION_FAILURE,
		    "xrRequestWorkspaceClientFullscreenEXT: clientId must not be XR_NULL_WORKSPACE_CLIENT_ID");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrRequestWorkspaceClientFullscreenEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_request_client_fullscreen(
	    &sess->xcn->base, (uint32_t)clientId, fullscreen == XR_TRUE);
	return xret_to_xr_result(&log, xret, "workspace_request_client_fullscreen");
}


/*
 * Controller-owned chrome (spec_version 7)
 *
 * C1 stubs: validate input + return XR_SUCCESS without producing real state.
 * C2 wires real D3D11 swapchain creation, side-table registration, and
 * per-render compositing.
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateWorkspaceClientChromeSwapchainEXT(XrSession session,
                                              XrWorkspaceClientId clientId,
                                              const XrWorkspaceChromeSwapchainCreateInfoEXT *createInfo,
                                              XrSwapchain *swapchain)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCreateWorkspaceClientChromeSwapchainEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, createInfo);
	OXR_VERIFY_ARG_NOT_NULL(&log, swapchain);

	if (createInfo->type != XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT: createInfo->type must be "
		                 "XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT");
	}
	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT: clientId must not be "
		                 "XR_NULL_WORKSPACE_CLIENT_ID");
	}
	if (createInfo->width == 0 || createInfo->height == 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT: width and height must be > 0");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT requires an IPC-mode session");
	}

	if (sess->compositor == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT: session has no compositor");
	}

	// Mint a regular OpenXR swapchain. Chrome is just a single-image color
	// swapchain — controllers Acquire/Wait/Release through standard image-loop
	// entry points. The runtime side-table tags this swapchain id as chrome
	// for `clientId` so the per-render path knows to composite it at the
	// layout-specified pose.
	XrSwapchainCreateInfo sci = {0};
	sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                 XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                 XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	sci.format = createInfo->format;
	sci.sampleCount = createInfo->sampleCount > 0 ? createInfo->sampleCount : 1;
	sci.width = createInfo->width;
	sci.height = createInfo->height;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = createInfo->mipCount > 0 ? createInfo->mipCount : 1;

	struct oxr_swapchain *sc = NULL;
	XrResult ret = sess->create_swapchain(&log, sess, &sci, &sc);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	uint32_t swapchain_id = comp_ipc_client_compositor_get_swapchain_id(sc->swapchain);
	if (swapchain_id == 0) {
		// Defensive: id 0 is reserved. Unwind by destroying the swapchain.
		oxr_handle_destroy(&log, &sc->handle);
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrCreateWorkspaceClientChromeSwapchainEXT: failed to resolve IPC swapchain id");
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_register_chrome_swapchain(
	    &sess->xcn->base, (uint32_t)clientId, swapchain_id);
	if (xret != XRT_SUCCESS) {
		oxr_handle_destroy(&log, &sc->handle);
		return xret_to_xr_result(&log, xret, "workspace_register_chrome_swapchain");
	}

	*swapchain = oxr_swapchain_to_openxr(sc);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyWorkspaceClientChromeSwapchainEXT(XrSwapchain swapchain)
{
	OXR_TRACE_MARKER();

	struct oxr_swapchain *sc = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SWAPCHAIN_AND_INIT_LOG(&log, swapchain, sc, "xrDestroyWorkspaceClientChromeSwapchainEXT");
	OXR_VERIFY_EXTENSION(&log, sc->sess->sys->inst, EXT_spatial_workspace);

	// Drop the runtime-side chrome registration before destroying the
	// swapchain. The runtime is tolerant of a missing entry — if the swapchain
	// was never registered (e.g. created via xrCreateSwapchain directly), the
	// unregister is a no-op there.
	uint32_t swapchain_id = comp_ipc_client_compositor_get_swapchain_id(sc->swapchain);
	if (swapchain_id != 0 && session_is_ipc_client(sc->sess)) {
		(void)comp_ipc_client_compositor_workspace_unregister_chrome_swapchain(
		    &sc->sess->xcn->base, swapchain_id);
	}

	return oxr_handle_destroy(&log, &sc->handle);
}

static void
chrome_layout_xr_to_ipc(const XrWorkspaceChromeLayoutEXT *in,
                        struct ipc_workspace_chrome_layout *out)
{
	memset(out, 0, sizeof(*out));
	xr_pose_to_xrt_pose(&in->poseInClient, &out->pose_in_client);
	out->size_w_m = in->sizeMeters.width;
	out->size_h_m = in->sizeMeters.height;
	out->follows_window_orient = in->followsWindowOrient ? 1u : 0u;
	out->depth_bias_meters = in->depthBiasMeters;
	out->hit_region_count = in->hitRegionCount;
	for (uint32_t i = 0; i < in->hitRegionCount && i < IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS; ++i) {
		out->hit_regions[i].id = (uint32_t)in->hitRegions[i].id;
		out->hit_regions[i].bounds_x = in->hitRegions[i].bounds.offset.x;
		out->hit_regions[i].bounds_y = in->hitRegions[i].bounds.offset.y;
		out->hit_regions[i].bounds_w = in->hitRegions[i].bounds.extent.width;
		out->hit_regions[i].bounds_h = in->hitRegions[i].bounds.extent.height;
	}
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientChromeLayoutEXT(XrSession session,
                                        XrWorkspaceClientId clientId,
                                        const XrWorkspaceChromeLayoutEXT *layout)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetWorkspaceClientChromeLayoutEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, layout);

	if (layout->type != XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientChromeLayoutEXT: layout->type must be "
		                 "XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT");
	}
	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientChromeLayoutEXT: clientId must not be "
		                 "XR_NULL_WORKSPACE_CLIENT_ID");
	}
	if (layout->hitRegionCount > XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientChromeLayoutEXT: hitRegionCount %u exceeds max %u",
		                 layout->hitRegionCount, XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_EXT);
	}
	if (layout->hitRegionCount > 0 && layout->hitRegions == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientChromeLayoutEXT: hitRegions must not be NULL when "
		                 "hitRegionCount > 0");
	}
	if (!(layout->sizeMeters.width > 0.0f) || !(layout->sizeMeters.height > 0.0f)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetWorkspaceClientChromeLayoutEXT: sizeMeters width/height must be > 0");
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetWorkspaceClientChromeLayoutEXT requires an IPC-mode session");
	}

	struct ipc_workspace_chrome_layout ipc_layout;
	chrome_layout_xr_to_ipc(layout, &ipc_layout);

	xrt_result_t xret = comp_ipc_client_compositor_workspace_set_chrome_layout(
	    &sess->xcn->base, (uint32_t)clientId, &ipc_layout);
	return xret_to_xr_result(&log, xret, "workspace_set_chrome_layout");
}

#endif // OXR_HAVE_EXT_spatial_workspace
