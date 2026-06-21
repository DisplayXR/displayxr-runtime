// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_weave API entry points (issue #625).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * A window-bound, synchronous weave service for present-owners (see
 * XR_EXT_weave.h and docs/roadmap/webxr-support.md §2.4 "Step 0"). The caller
 * owns its OS window and presents itself; the runtime's display processor
 * weaves a window sub-rect from a caller-supplied pre-weave SBS texture and
 * hands back a weaved shared texture + fence. The caller NEVER weaves
 * (ADR-007 / ADR-019).
 *
 * Availability: implemented only on the out-of-process (service / IPC) path —
 * the weave runs in the D3D11 service compositor. An in-process session reports
 * XR_ERROR_FEATURE_UNSUPPORTED. The entry points forward to thin IPC-client
 * bridges (defined in ipc_client_compositor.c); st_oxr does not pull the
 * ipc_client include path, so the symbols resolve at link time — same pattern
 * as oxr_capture.c / oxr_workspace.c.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_results.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_display_metrics.h"

#include <openxr/XR_EXT_weave.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_weave

// Forward decls of the IPC-bridge wrappers (defined in ipc_client_compositor.c).
struct xrt_compositor;

xrt_result_t
comp_ipc_client_compositor_weave_bind_window(struct xrt_compositor *xc, uint64_t hwnd);

xrt_result_t
comp_ipc_client_compositor_weave_submit(struct xrt_compositor *xc,
                                        xrt_graphics_buffer_handle_t in_handle,
                                        bool in_is_dxgi,
                                        int32_t rect_x,
                                        int32_t rect_y,
                                        uint32_t rect_w,
                                        uint32_t rect_h,
                                        bool *out_have_output,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint64_t *out_fence_value,
                                        struct xrt_eye_positions *out_eyes);

xrt_result_t
comp_ipc_client_compositor_weave_get_output(struct xrt_compositor *xc,
                                            bool *out_have_output,
                                            uint32_t *out_width,
                                            uint32_t *out_height,
                                            xrt_graphics_buffer_handle_t *out_handle);

xrt_result_t
comp_ipc_client_compositor_weave_get_fence(struct xrt_compositor *xc,
                                           bool *out_have_fence,
                                           xrt_graphics_sync_handle_t *out_handle);

xrt_result_t
comp_ipc_client_compositor_weave_snap_window_rect(struct xrt_compositor *xc,
                                                  int32_t origin_x,
                                                  int32_t origin_y,
                                                  int32_t target_x,
                                                  int32_t target_y,
                                                  bool *out_snapped,
                                                  int32_t *out_snapped_x,
                                                  int32_t *out_snapped_y);

//! IPC sessions hold a native-compositor handle (the IPC client compositor) but
//! none of the in-process native-compositor flags are set. Mirrors oxr_capture.c.
static bool
session_is_ipc(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		return false;
	}
	bool inprocess = sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                 sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                 sess->is_vk_native_compositor;
	return !inprocess;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveBindWindowEXT(XrSession session, void *windowHandle)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveBindWindowEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_weave);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveBindWindowEXT: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_bind_window(
	    &sess->xcn->base, (uint64_t)(uintptr_t)windowHandle);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrWeaveBindWindowEXT: bind failed (xrt_result=%d)", (int)xret);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSubmitEXT(XrSession session, const XrWeaveSubmitInfoEXT *submitInfo, XrWeaveOutputEXT *output)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSubmitEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, submitInfo, XR_TYPE_WEAVE_SUBMIT_INFO_EXT);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, output, XR_TYPE_WEAVE_OUTPUT_EXT);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSubmitEXT: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	bool have_out = false;
	uint32_t w = 0, h = 0;
	uint64_t fence_value = 0;
	struct xrt_eye_positions eyes = {0};
	xrt_result_t xret = comp_ipc_client_compositor_weave_submit(
	    &sess->xcn->base, (xrt_graphics_buffer_handle_t)submitInfo->inputTexture,
	    submitInfo->inputIsDxgi == XR_TRUE, submitInfo->rect.offset.x, submitInfo->rect.offset.y,
	    (uint32_t)submitInfo->rect.extent.width, (uint32_t)submitInfo->rect.extent.height, &have_out, &w, &h,
	    &fence_value, &eyes);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrWeaveSubmitEXT: weave failed (xrt_result=%d)", (int)xret);
	}

	// Per-frame scalars are always valid; the shared HANDLEs are handed back
	// only on the first submit and on re-allocation (resize → dims change).
	output->weavedTexture = NULL;
	output->width = w;
	output->height = h;
	output->fence = NULL;
	output->fenceValue = fence_value;

	// Eyes flow OUT: the caller renders its NEXT pre-weave frame's off-axis
	// projection from these tracked positions (look-around). The interlace
	// itself is DP-internal.
	uint32_t ec = eyes.count;
	if (ec > XR_WEAVE_MAX_EYES_EXT) {
		ec = XR_WEAVE_MAX_EYES_EXT;
	}
	output->eyeCount = ec;
	for (uint32_t i = 0; i < ec; i++) {
		output->eyes[i].x = eyes.eyes[i].x;
		output->eyes[i].y = eyes.eyes[i].y;
		output->eyes[i].z = eyes.eyes[i].z;
	}
	output->eyesValid = eyes.valid ? XR_TRUE : XR_FALSE;
	output->eyesTracking = eyes.is_tracking ? XR_TRUE : XR_FALSE;

	bool need_export = !sess->weave.exported || w != sess->weave.last_w || h != sess->weave.last_h;
	if (have_out && w != 0 && h != 0 && need_export) {
		bool have_tex = false;
		uint32_t gw = 0, gh = 0;
		xrt_graphics_buffer_handle_t tex_h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		if (comp_ipc_client_compositor_weave_get_output(&sess->xcn->base, &have_tex, &gw, &gh, &tex_h) ==
		        XRT_SUCCESS &&
		    have_tex && tex_h != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
			output->weavedTexture = (void *)tex_h;
		}

		bool have_fence = false;
		xrt_graphics_sync_handle_t fence_h = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		if (comp_ipc_client_compositor_weave_get_fence(&sess->xcn->base, &have_fence, &fence_h) ==
		        XRT_SUCCESS &&
		    have_fence && fence_h != XRT_GRAPHICS_SYNC_HANDLE_INVALID) {
			output->fence = (void *)fence_h;
		}

		sess->weave.exported = true;
		sess->weave.last_w = w;
		sess->weave.last_h = h;
	}

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSnapWindowRectEXT(XrSession session,
                             const XrRect2Di *originRect,
                             const XrRect2Di *targetRect,
                             XrRect2Di *snappedRect)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSnapWindowRectEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_weave);
	OXR_VERIFY_ARG_NOT_NULL(&log, originRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, targetRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, snappedRect);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSnapWindowRectEXT: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	// Only the top-left is phase-snapped; the size passes through unchanged.
	// On no DP snap support the bridge returns the target unchanged, so the
	// result is always a valid rect.
	bool snapped = false;
	int32_t sx = targetRect->offset.x, sy = targetRect->offset.y;
	xrt_result_t xret = comp_ipc_client_compositor_weave_snap_window_rect(
	    &sess->xcn->base, originRect->offset.x, originRect->offset.y, targetRect->offset.x, targetRect->offset.y,
	    &snapped, &sx, &sy);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrWeaveSnapWindowRectEXT: snap failed (xrt_result=%d)", (int)xret);
	}

	snappedRect->offset.x = sx;
	snappedRect->offset.y = sy;
	snappedRect->extent = targetRect->extent;
	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_weave
