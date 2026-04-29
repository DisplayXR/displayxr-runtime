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

#endif // OXR_HAVE_EXT_spatial_workspace
