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

#include <openxr/XR_EXT_spatial_workspace.h>

#include <stdbool.h>
#include <stdint.h>

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
                          XrVector2f *outLocalUV)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWorkspaceHitTestEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_spatial_workspace);
	OXR_VERIFY_ARG_NOT_NULL(&log, outClientId);
	OXR_VERIFY_ARG_NOT_NULL(&log, outLocalUV);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWorkspaceHitTestEXT requires an IPC-mode session");
	}

	uint32_t client_id = 0;
	float u = 0.0f, v = 0.0f;
	uint32_t region = 0; // discarded — Phase 2.D Commit 3 surfaces this through a
	                     // public-API parameter; this commit only plumbs the wire.
	xrt_result_t xret = comp_ipc_client_compositor_workspace_hit_test(&sess->xcn->base, cursorX, cursorY,
	                                                                  &client_id, &u, &v, &region);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "workspace_hit_test");
	}
	*outClientId = (XrWorkspaceClientId)client_id;
	outLocalUV->x = u;
	outLocalUV->y = v;
	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_spatial_workspace
