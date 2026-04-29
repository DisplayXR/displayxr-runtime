// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_app_launcher API entry points (Phase 2.B).
 * @author DisplayXR
 * @ingroup oxr_api
 *
 * Wraps the launcher IPC RPCs as OpenXR extension functions. Each entry
 * point validates session/extension state, gates on IPC mode (the
 * workspace controller is always external), and dispatches via the
 * forward-declared comp_ipc_client_compositor_launcher_* bridges. The
 * IPC-bridge / forward-declaration pattern matches oxr_workspace.c —
 * see that file for the same architectural rationale.
 *
 * XrLauncherAppInfoEXT → struct ipc_launcher_app translation lives here
 * because the IPC layer should not know about OpenXR types. Phase 2.B
 * narrows the public surface to (name, iconPath, appIndex); the
 * additional IPC fields (exe_path, type, icon_3d_path, icon_3d_layout)
 * are zeroed and a follow-up sub-phase reconciles the boundary.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_results.h"

#include <openxr/XR_EXT_app_launcher.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_app_launcher

// Forward declarations of the IPC-bridge wrappers (defined in
// src/xrt/ipc/client/ipc_client_compositor.c). The bridge takes primitive
// fields rather than the IPC wire struct so the state tracker can stay
// out of the ipc_client include path.
struct xrt_compositor;
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
	case XRT_ERROR_NOT_AUTHORIZED:
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: not authorized as workspace controller",
		                 label);
	case XRT_ERROR_IPC_FAILURE:
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: IPC failure", label);
	default: return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: xrt_result=%d", label, (int)xret);
	}
}


/*
 * Tile registry
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrClearLauncherAppsEXT(XrSession session)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrClearLauncherAppsEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrClearLauncherAppsEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_launcher_clear_apps(&sess->xcn->base);
	return xret_to_xr_result(&log, xret, "launcher_clear_apps");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAddLauncherAppEXT(XrSession session, const XrLauncherAppInfoEXT *info)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrAddLauncherAppEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_LAUNCHER_APP_INFO_EXT);

	if (info->appIndex < 0 || info->appIndex >= XR_LAUNCHER_MAX_APPS_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrAddLauncherAppEXT: appIndex %d out of range [0, %d)", (int)info->appIndex,
		                 (int)XR_LAUNCHER_MAX_APPS_EXT);
	}

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrAddLauncherAppEXT requires an IPC-mode session");
	}

	// The Phase 2.B public surface exposes only (name, iconPath). The
	// IPC bridge fills the wire struct (which carries additional fields
	// not yet promoted to the public API) and zeroes the rest.
	xrt_result_t xret = comp_ipc_client_compositor_launcher_add_app(&sess->xcn->base, info->name,
	                                                                info->iconPath);
	return xret_to_xr_result(&log, xret, "launcher_add_app");
}


/*
 * Visibility
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLauncherVisibleEXT(XrSession session, XrBool32 visible)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetLauncherVisibleEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetLauncherVisibleEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_launcher_set_visible(&sess->xcn->base,
	                                                                    visible == XR_TRUE);
	return xret_to_xr_result(&log, xret, "launcher_set_visible");
}


/*
 * Click polling
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPollLauncherClickEXT(XrSession session, int32_t *outAppIndex)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrPollLauncherClickEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);
	OXR_VERIFY_ARG_NOT_NULL(&log, outAppIndex);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrPollLauncherClickEXT requires an IPC-mode session");
	}

	int64_t tile_index = -1;
	xrt_result_t xret = comp_ipc_client_compositor_launcher_poll_click(&sess->xcn->base, &tile_index);
	if (xret != XRT_SUCCESS) {
		return xret_to_xr_result(&log, xret, "launcher_poll_click");
	}

	// Wire format is int64; public API is int32 (appIndex + special-action
	// sentinels all fit). Clamp out-of-range values to "no click" rather
	// than truncating into a misleading sentinel.
	if (tile_index > INT32_MAX || tile_index < INT32_MIN) {
		U_LOG_W("xrPollLauncherClickEXT: out-of-range tile_index=%lld; reporting INVALID",
		        (long long)tile_index);
		*outAppIndex = XR_LAUNCHER_INVALID_APPINDEX_EXT;
	} else {
		*outAppIndex = (int32_t)tile_index;
	}
	return XR_SUCCESS;
}


/*
 * Running indicator
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLauncherRunningTileMaskEXT(XrSession session, uint64_t mask)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetLauncherRunningTileMaskEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrSetLauncherRunningTileMaskEXT requires an IPC-mode session");
	}

	xrt_result_t xret = comp_ipc_client_compositor_launcher_set_running_tile_mask(&sess->xcn->base, mask);
	return xret_to_xr_result(&log, xret, "launcher_set_running_tile_mask");
}

#endif // OXR_HAVE_EXT_app_launcher
