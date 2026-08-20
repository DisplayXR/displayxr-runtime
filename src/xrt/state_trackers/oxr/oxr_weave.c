// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave API entry points (issue #625).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * A window-bound, synchronous weave service for present-owners (see
 * XR_DXR_weave.h and docs/roadmap/webxr-support.md §2.4 "Step 0"). The caller
 * owns its OS window and presents itself; the runtime's display processor
 * weaves a window sub-rect from a caller-supplied pre-weave SBS texture and
 * hands back a weaved shared texture + fence. The caller NEVER weaves
 * (ADR-007 / ADR-019).
 *
 * Availability: implemented only on the out-of-process (service / IPC) path —
 * the weave runs in the D3D11 service compositor on Windows and in the
 * comp_multi Vulkan weave engine on macOS (#759: IOSurface in/out, synchronous
 * completion, no fence handle). An in-process session reports
 * XR_ERROR_FEATURE_UNSUPPORTED. The entry points forward to thin IPC-client
 * bridges (defined in ipc_client_compositor.c); st_oxr does not pull the
 * ipc_client include path, so the symbols resolve at link time — same pattern
 * as oxr_capture.c / oxr_workspace.c.
 *
 * Connection loss (browser#103): every bridge result goes through
 * OXR_CHECK_XRET_MSG, so a dead pipe (XRT_ERROR_IPC_FAILURE) marks the session
 * lost and reports XR_ERROR_INSTANCE_LOST — after which
 * OXR_VERIFY_SESSION_NOT_LOST short-circuits every later call to
 * XR_ERROR_SESSION_LOST, exactly like xrEndFrame / xrLocateViews. A weave-only
 * present-owner is otherwise the one client in the runtime that never learns its
 * connection died.
 *
 * A service-side weave REFUSAL is deliberately NOT that: the service answers
 * XRT_ERROR_WEAVE_REFUSED over a healthy pipe (canonically the 4 ms input
 * AcquireSync timeout in comp_d3d11_service_weave_submit, retried next frame),
 * which stays a non-fatal XR_ERROR_RUNTIME_FAILURE and leaves the session usable.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_xret.h"

#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_chain.h"

#include "xrt/xrt_results.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_display_metrics.h"

#include <openxr/XR_DXR_weave.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef OXR_HAVE_DXR_weave

// Forward decls of the IPC-bridge wrappers (defined in ipc_client_compositor.c).
struct xrt_compositor;

xrt_result_t
comp_ipc_client_compositor_weave_bind_window(struct xrt_compositor *xc, uint64_t hwnd);

xrt_result_t
comp_ipc_client_compositor_weave_set_window_geometry(struct xrt_compositor *xc,
                                                     int32_t origin_x,
                                                     int32_t origin_y,
                                                     uint32_t client_w,
                                                     uint32_t client_h,
                                                     int32_t display_id);

xrt_result_t
comp_ipc_client_compositor_weave_submit(struct xrt_compositor *xc,
                                        xrt_graphics_buffer_handle_t in_handle,
                                        bool in_is_dxgi,
                                        int32_t rect_x,
                                        int32_t rect_y,
                                        uint32_t rect_w,
                                        uint32_t rect_h,
                                        uint32_t rect_count,
                                        const struct xrt_rect *rects,
                                        xrt_graphics_buffer_handle_t overlay_handle,
                                        bool overlay_is_dxgi,
                                        uint32_t overlay_rect_count,
                                        const struct xrt_rect *overlay_rects,
                                        bool weave_frame_first,
                                        const struct xrt_weave_atlas_layout *layout,
                                        uint32_t flat_rect_count,
                                        const struct xrt_rect *flat_rects,
                                        bool *out_have_output,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint64_t *out_fence_value,
                                        struct xrt_eye_positions *out_eyes);

xrt_result_t
comp_ipc_client_compositor_weave_set_screen_flat_regions(struct xrt_compositor *xc,
                                                         uint32_t rect_count,
                                                         const struct xrt_rect *screen_rects);

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

/*!
 * Is @p kind a handle kind THIS platform can accept (spec v7, #1036)?
 * PLATFORM_DEFAULT always is — it means "resolve from the platform", which is
 * what every pre-v7 caller does implicitly.
 */
static bool
weave_handle_kind_ok(XrWeaveHandleKindDXR kind)
{
	switch (kind) {
	case XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR: return true;
#if defined(XR_USE_PLATFORM_WIN32)
	case XR_WEAVE_HANDLE_KIND_D3D11_NT_DXR:
	case XR_WEAVE_HANDLE_KIND_D3D11_DXGI_DXR: return true;
#elif defined(XR_USE_PLATFORM_ANDROID)
	case XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR: return true;
#elif defined(XR_USE_PLATFORM_MACOS)
	case XR_WEAVE_HANDLE_KIND_IOSURFACE_DXR: return true;
#endif
	default: return false;
	}
}

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
oxr_xrWeaveBindWindowDXR(XrSession session, void *windowHandle)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveBindWindowDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveBindWindowDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_bind_window(
	    &sess->xcn->base, (uint64_t)(uintptr_t)windowHandle);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindowDXR: bind failed (xrt_result=%d)", (int)xret);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveBindWindow2DXR(XrSession session, const XrWeaveBindWindowInfoDXR *bindInfo)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveBindWindow2DXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, bindInfo, XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveBindWindow2DXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	// The handle half is exactly xrWeaveBindWindowDXR (NULL is legal — Android
	// has no window handle at all and binds by geometry alone).
	xrt_result_t xret = comp_ipc_client_compositor_weave_bind_window(
	    &sess->xcn->base, (uint64_t)(uintptr_t)bindInfo->windowHandle);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindow2DXR: bind failed (xrt_result=%d)", (int)xret);

	// Spec v7 (#1036): explicit client-area geometry on the panel. Optional on
	// Windows / macOS (the runtime can read it off the window handle there),
	// required on Android. Forwarded to the DP's per-window phase slot.
	const XrWeaveWindowGeometryDXR *geom =
	    OXR_GET_INPUT_FROM_CHAIN(bindInfo, XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR, XrWeaveWindowGeometryDXR);
	if (geom != NULL) {
		if (geom->clientSize.width <= 0 || geom->clientSize.height <= 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveBindWindow2DXR: XrWeaveWindowGeometryDXR::clientSize "
			                 "(%dx%d) must be positive",
			                 geom->clientSize.width, geom->clientSize.height);
		}
		xret = comp_ipc_client_compositor_weave_set_window_geometry(
		    &sess->xcn->base, geom->windowOriginOnScreen.x, geom->windowOriginOnScreen.y,
		    (uint32_t)geom->clientSize.width, (uint32_t)geom->clientSize.height, geom->displayId);
		OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindow2DXR: geometry update failed (xrt_result=%d)",
		                   (int)xret);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSubmitDXR(XrSession session, const XrWeaveSubmitInfoDXR *submitInfo, XrWeaveOutputDXR *output)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSubmitDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, submitInfo, XR_TYPE_WEAVE_SUBMIT_INFO_DXR);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, output, XR_TYPE_WEAVE_OUTPUT_DXR);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSubmitDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	// Spec v3 batched submit: a chained XrWeaveSubmitRectsDXR switches the
	// input-layout contract (window-sized input, content at each rect's own
	// window position; base rect ignored). Absent chain = legacy single-rect,
	// byte-equivalent to spec v2.
	uint32_t rect_count = 0;
	struct xrt_rect rects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	const XrWeaveSubmitRectsDXR *batch =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_RECTS_DXR, XrWeaveSubmitRectsDXR);
	if (batch != NULL) {
		if (batch->rectCount < 1 || batch->rectCount > XR_WEAVE_SUBMIT_MAX_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitRectsDXR::rectCount (%u) must be "
			                 "1..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (%u)",
			                 batch->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
		}
		OXR_VERIFY_ARG_NOT_NULL(&log, batch->rects);
		rect_count = batch->rectCount;
		for (uint32_t i = 0; i < rect_count; i++) {
			// xrt_offset names its fields w/h (see the @todo in xrt_defines.h);
			// they are x/y here.
			rects[i].offset.w = batch->rects[i].offset.x;
			rects[i].offset.h = batch->rects[i].offset.y;
			rects[i].extent.w = (int)batch->rects[i].extent.width;
			rects[i].extent.h = (int)batch->rects[i].extent.height;
		}
	}

	// Spec v4 (browser#18): a chained XrWeaveSubmitOverlaysDXR carries a
	// window-sized premul-RGBA overlay atlas (a second shared texture) the DP
	// composites over the woven output; overlayCount 0 = whole atlas.
	xrt_graphics_buffer_handle_t overlay_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	bool overlay_is_dxgi = false;
	uint32_t overlay_rect_count = 0;
	struct xrt_rect overlay_rects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	const XrWeaveSubmitOverlaysDXR *ov =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR, XrWeaveSubmitOverlaysDXR);
	if (ov != NULL && ov->overlayTexture != NULL) {
		if (ov->rectCount > XR_WEAVE_SUBMIT_MAX_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitOverlaysDXR::rectCount (%u) must be "
			                 "0..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (%u)",
			                 ov->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
		}
		if (ov->rectCount > 0) {
			OXR_VERIFY_ARG_NOT_NULL(&log, ov->rects);
		}
		overlay_handle = (xrt_graphics_buffer_handle_t)ov->overlayTexture;
		overlay_is_dxgi = ov->overlayIsDxgi == XR_TRUE;
		overlay_rect_count = ov->rectCount;
		for (uint32_t i = 0; i < overlay_rect_count; i++) {
			overlay_rects[i].offset.w = ov->rects[i].offset.x;
			overlay_rects[i].offset.h = ov->rects[i].offset.y;
			overlay_rects[i].extent.w = (int)ov->rects[i].extent.width;
			overlay_rects[i].extent.h = (int)ov->rects[i].extent.height;
		}
	}

	// Spec v7 (#1036): a chained XrWeaveSubmitHandlesDXR declares what the
	// handles ARE. It changes no layout contract — it only lets us reject a
	// cross-platform mistake here instead of dereferencing an alien pointer in
	// the service. PLATFORM_DEFAULT (and an absent chain) = pre-v7 behaviour.
	const XrWeaveSubmitHandlesDXR *kinds =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR, XrWeaveSubmitHandlesDXR);
	if (kinds != NULL) {
		if (!weave_handle_kind_ok(kinds->inputKind)) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitHandlesDXR::inputKind (%d) is not a "
			                 "handle kind this platform accepts",
			                 (int)kinds->inputKind);
		}
		if (overlay_handle != XRT_GRAPHICS_BUFFER_HANDLE_INVALID && !weave_handle_kind_ok(kinds->overlayKind)) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitHandlesDXR::overlayKind (%d) is not a "
			                 "handle kind this platform accepts",
			                 (int)kinds->overlayKind);
		}
	}

	// Spec v8 (browser#88): a chained XrWeaveSubmitFlatRegionsDXR names the
	// regions of this submit that must be PHYSICALLY FLAT. The service subtracts
	// them from the weave rects to derive the per-region hardware wish it
	// publishes to the DP. Purely advisory and hardware-only (ADR-027 D6 /
	// ADR-030) — it gates no content, so validation here is bounds only and an
	// absent chain is byte-for-byte pre-v8.
	uint32_t flat_rect_count = 0;
	struct xrt_rect flat_rects[XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR];
	const XrWeaveSubmitFlatRegionsDXR *flat = OXR_GET_INPUT_FROM_CHAIN(
	    submitInfo, XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR, XrWeaveSubmitFlatRegionsDXR);
	if (flat != NULL && flat->rectCount > 0) {
		if (flat->rectCount > XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitFlatRegionsDXR::rectCount (%u) must be "
			                 "0..XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR (%u)",
			                 flat->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR);
		}
		OXR_VERIFY_ARG_NOT_NULL(&log, flat->rects);
		flat_rect_count = flat->rectCount;
		for (uint32_t i = 0; i < flat_rect_count; i++) {
			// xrt_offset names its fields w/h; they are x/y here.
			flat_rects[i].offset.w = flat->rects[i].offset.x;
			flat_rects[i].offset.h = flat->rects[i].offset.y;
			flat_rects[i].extent.w = (int)flat->rects[i].extent.width;
			flat_rects[i].extent.h = (int)flat->rects[i].extent.height;
		}
	}

	// Spec v6 (#774): a chained XrWeaveSubmitLayoutDXR declares that the input
	// is a worst-case-sized N-view atlas (tiles packed contiguously from the
	// top-left at contentViewWidth/Height) instead of per-rect squeezed SBS.
	// Absent chain = legacy layout, byte-for-byte unchanged.
	struct xrt_weave_atlas_layout layout = {0};
	const XrWeaveSubmitLayoutDXR *lay =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR, XrWeaveSubmitLayoutDXR);
	if (lay != NULL) {
		if (lay->tileColumns == 0 || lay->tileRows == 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::tileColumns (%u) and "
			                 "tileRows (%u) must both be non-zero",
			                 lay->tileColumns, lay->tileRows);
		}
		if (lay->viewCount == 0 || lay->viewCount > XR_WEAVE_MAX_EYES_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::viewCount (%u) must be "
			                 "1..%u",
			                 lay->viewCount, (uint32_t)XR_WEAVE_MAX_EYES_DXR);
		}
		if (lay->viewCount != lay->tileColumns * lay->tileRows) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::viewCount (%u) must equal "
			                 "tileColumns * tileRows (%u * %u)",
			                 lay->viewCount, lay->tileColumns, lay->tileRows);
		}
		if (lay->contentViewWidth == 0 || lay->contentViewHeight == 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR content view dims "
			                 "(%ux%u) must both be non-zero",
			                 lay->contentViewWidth, lay->contentViewHeight);
		}
		layout.view_count = lay->viewCount;
		layout.tile_columns = lay->tileColumns;
		layout.tile_rows = lay->tileRows;
		layout.content_view_w = lay->contentViewWidth;
		layout.content_view_h = lay->contentViewHeight;
	}

	bool have_out = false;
	uint32_t w = 0, h = 0;
	uint64_t fence_value = 0;
	struct xrt_eye_positions eyes = {0};
	xrt_result_t xret = comp_ipc_client_compositor_weave_submit(
	    &sess->xcn->base, (xrt_graphics_buffer_handle_t)submitInfo->inputTexture,
	    submitInfo->inputIsDxgi == XR_TRUE, submitInfo->rect.offset.x, submitInfo->rect.offset.y,
	    (uint32_t)submitInfo->rect.extent.width, (uint32_t)submitInfo->rect.extent.height, rect_count,
	    rect_count > 0 ? rects : NULL, overlay_handle, overlay_is_dxgi, overlay_rect_count,
	    overlay_rect_count > 0 ? overlay_rects : NULL, submitInfo->firstChunk == XR_TRUE,
	    layout.view_count > 0 ? &layout : NULL, flat_rect_count, flat_rect_count > 0 ? flat_rects : NULL,
	    &have_out, &w, &h, &fence_value, &eyes);
	// XRT_ERROR_WEAVE_REFUSED (the service said "not this frame") lands on the
	// non-fatal branch; only a dead pipe loses the session. See the file header.
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSubmitDXR: weave failed (xrt_result=%d)", (int)xret);

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
	if (ec > XR_WEAVE_MAX_EYES_DXR) {
		ec = XR_WEAVE_MAX_EYES_DXR;
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
			// (intptr_t hop: the handle is an fd int on POSIX — macOS never
			// exports a fence (#759, completion is synchronous), so this
			// branch fires on Windows HANDLEs only.)
			output->fence = (void *)(intptr_t)fence_h;
		}

		sess->weave.exported = true;
		sess->weave.last_w = w;
		sess->weave.last_h = h;
	}

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSnapWindowRectDXR(XrSession session,
                             const XrRect2Di *originRect,
                             const XrRect2Di *targetRect,
                             XrRect2Di *snappedRect)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSnapWindowRectDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_NOT_NULL(&log, originRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, targetRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, snappedRect);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSnapWindowRectDXR: the weave service is only available on the "
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
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSnapWindowRectDXR: snap failed (xrt_result=%d)", (int)xret);

	snappedRect->offset.x = sx;
	snappedRect->offset.y = sy;
	snappedRect->extent = targetRect->extent;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSetScreenFlatRegionsDXR(XrSession session, uint32_t rectCount, const XrRect2Di *screenRects)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSetScreenFlatRegionsDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);

	if (rectCount > XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrWeaveSetScreenFlatRegionsDXR: rectCount (%u) must be "
		                 "0..XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR (%u)",
		                 rectCount, (uint32_t)XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR);
	}
	// rectCount 0 CLEARS the latch, so screenRects is only required when there is
	// something to read.
	if (rectCount > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, screenRects);
	}

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSetScreenFlatRegionsDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	struct xrt_rect rects[XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR];
	for (uint32_t i = 0; i < rectCount; i++) {
		// xrt_offset names its fields w/h; they are x/y here. Absolute PHYSICAL
		// screen pixels — unlike the per-submit list these are not
		// window-relative, so no origin is applied on this side.
		rects[i].offset.w = screenRects[i].offset.x;
		rects[i].offset.h = screenRects[i].offset.y;
		rects[i].extent.w = (int)screenRects[i].extent.width;
		rects[i].extent.h = (int)screenRects[i].extent.height;
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_set_screen_flat_regions(&sess->xcn->base, rectCount,
	                                                                            rectCount > 0 ? rects : NULL);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSetScreenFlatRegionsDXR: latch failed (xrt_result=%d)", (int)xret);
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_weave
