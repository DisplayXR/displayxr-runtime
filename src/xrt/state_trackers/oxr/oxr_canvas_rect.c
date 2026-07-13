// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_canvas_rect API entry point (issue #697).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * Lets a windowless / present-owning producer declare the on-panel rectangle
 * where its content appears — directly, in display-relative device pixels —
 * instead of the runtime deriving it from a bound OS window. This is the
 * position/phase anchor #697 decouples from the HWND: the same value the
 * compositor's get_window_metrics would otherwise read via GetClientRect /
 * ClientToScreen (see docs/getting-started/app-classes.md).
 *
 * The create-time seed (XrCanvasRectBindingDXR chained on XrSessionCreateInfo)
 * is parsed in oxr_session_create and rides xrt_session_info into the
 * compositor. THIS file implements the live setter, which forwards to the
 * compositor that owns the canvas state: the in-process D3D11 native compositor
 * directly, or the out-of-process server via the IPC-client bridge. The bridge
 * symbols are declared here rather than via ipc_client.h because st_oxr does not
 * pull the ipc_client include path — same pattern as oxr_weave.c / oxr_capture.c.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_results.h"

#include <openxr/XR_DXR_canvas_rect.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef OXR_HAVE_DXR_canvas_rect

struct xrt_compositor;

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
// In-process D3D11 native compositor (defined in comp_d3d11_compositor.cpp).
xrt_result_t
comp_d3d11_compositor_set_canvas_rect(
    struct xrt_compositor *xc, bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h);
#endif

// IPC-client bridge → set_canvas_rect on the server (defined in
// ipc_client_compositor.c).
xrt_result_t
comp_ipc_client_compositor_set_canvas_rect(
    struct xrt_compositor *xc, bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h);

//! IPC sessions hold a native-compositor handle (the IPC client compositor) but
//! none of the in-process native-compositor flags are set. Mirrors oxr_weave.c.
static bool
session_is_ipc(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL) {
		return false;
	}
	bool inprocess = sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                 sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                 sess->is_vk_native_compositor;
	return !inprocess;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetCanvasRectDXR(XrSession session, const XrRect2Di *rectPx)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetCanvasRectDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_canvas_rect);

	// NULL clears the override (revert to bound window / display-scoped).
	bool valid = rectPx != NULL;
	int32_t x = 0, y = 0;
	uint32_t w = 0, h = 0;
	if (valid) {
		if (rectPx->extent.width <= 0 || rectPx->extent.height <= 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrSetCanvasRectDXR: rect extent must be positive (got %dx%d)",
			                 rectPx->extent.width, rectPx->extent.height);
		}
		x = rectPx->offset.x;
		y = rectPx->offset.y;
		w = (uint32_t)rectPx->extent.width;
		h = (uint32_t)rectPx->extent.height;
	}

	xrt_result_t xret = XRT_ERROR_NOT_IMPLEMENTED;
	if (session_is_ipc(sess)) {
		xret = comp_ipc_client_compositor_set_canvas_rect(&sess->xcn->base, valid, x, y, w, h);
	}
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	else if (sess->is_d3d11_native_compositor) {
		xret = comp_d3d11_compositor_set_canvas_rect(&sess->xcn->base, valid, x, y, w, h);
	}
#endif

	if (xret == XRT_ERROR_NOT_IMPLEMENTED) {
		return oxr_error(&log, XR_ERROR_FUNCTION_UNSUPPORTED,
		                 "xrSetCanvasRectDXR: the canvas-rect override is not implemented on this "
		                 "compositor backend (D3D11 in-process + service only)");
	}
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrSetCanvasRectDXR: set failed (xrt_result=%d)", (int)xret);
	}
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_canvas_rect
