// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_local_3d_zone entrypoints — authored 2D/3D mask (Phase 1 of
 *         unified 2D/3D compositing, docs/roadmap/unified-2d-3d-phase1-impl.md).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * The oxr layer owns the mask handle lifecycle and forwards authoring calls to
 * the native compositor's zone-mask entry points. Phase 1 wires the D3D11
 * consumer only; every other compositor falls through to
 * XR_ERROR_FEATURE_UNSUPPORTED (the caps query reports supported = false
 * instead of erroring).
 */

#include <stdlib.h>

#include "xrt/xrt_compiler.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_handle.h"

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
#include "d3d11/comp_d3d11_compositor.h"
#endif

#ifdef OXR_HAVE_EXT_local_3d_zone

//! D3D11 max texture dimension — bounds the authored mask size.
#define OXR_LOCAL_3D_ZONE_MAX_MASK_DIM 16384


/*
 *
 * Handle lifecycle.
 *
 */

static XrResult
oxr_local_3d_zone_destroy_cb(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_local_3d_zone_ext *zone = (struct oxr_local_3d_zone_ext *)hb;

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (zone->comp_mask != NULL && sess != NULL && sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		comp_d3d11_compositor_zone_mask_destroy(&sess->xcn->base, zone->comp_mask);
	}
#endif
	zone->comp_mask = NULL;

	free(zone);

	return XR_SUCCESS;
}


/*
 *
 * API entrypoints.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetLocal3DZoneCapabilitiesEXT(XrSession session, XrLocal3DZoneCapabilitiesEXT *capabilities)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetLocal3DZoneCapabilitiesEXT");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, capabilities, XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_EXT);

	// Never errors on an unsupported compositor — reports supported = false.
	capabilities->supported = XR_FALSE;
	capabilities->hardwareZoneGridWidth = 0;
	capabilities->hardwareZoneGridHeight = 0;
	capabilities->maxMaskWidth = 0;
	capabilities->maxMaskHeight = 0;

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		capabilities->supported = XR_TRUE;
		// Compositor consumer only — no hardware-zone DP leg yet.
		capabilities->hardwareZoneGridWidth = 0;
		capabilities->hardwareZoneGridHeight = 0;
		capabilities->maxMaskWidth = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
		capabilities->maxMaskHeight = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
	}
#endif

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateLocal3DZoneMaskEXT(XrSession session,
                               const XrLocal3DZoneMaskCreateInfoEXT *createInfo,
                               XrLocal3DZoneMaskEXT *mask)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCreateLocal3DZoneMaskEXT");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, createInfo, XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT);
	OXR_VERIFY_ARG_NOT_NULL(&log, mask);

	if (createInfo->maskWidth > OXR_LOCAL_3D_ZONE_MAX_MASK_DIM ||
	    createInfo->maskHeight > OXR_LOCAL_3D_ZONE_MAX_MASK_DIM) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(createInfo->maskWidth/Height == %ux%u) exceeds the max mask dimension (%u)",
		                 createInfo->maskWidth, createInfo->maskHeight, OXR_LOCAL_3D_ZONE_MAX_MASK_DIM);
	}

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		struct oxr_local_3d_zone_ext *zone = NULL;
		OXR_ALLOCATE_HANDLE_OR_RETURN(&log, zone, OXR_XR_DEBUG_LOCAL3DZONE, oxr_local_3d_zone_destroy_cb,
		                              &sess->handle);

		zone->sess = sess;
		// 0 lets the compositor choose (clamped to the client window).
		zone->width = createInfo->maskWidth;
		zone->height = createInfo->maskHeight;

		xrt_result_t xret = comp_d3d11_compositor_zone_mask_create(&sess->xcn->base, zone->width,
		                                                           zone->height, &zone->comp_mask);
		if (xret != XRT_SUCCESS) {
			XrResult ret = oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
			                         "Failed to create compositor zone mask (%d)", xret);
			oxr_handle_destroy(&log, &zone->handle);
			return ret;
		}

		*mask = oxr_local_3d_zone_to_openxr(zone);
		return XR_SUCCESS;
	}
#endif

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "Local 3D zone masks are not supported for this compositor");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLocal3DZoneWholeWindowEXT(XrLocal3DZoneMaskEXT mask, XrBool32 enable3D)
{
	OXR_TRACE_MARKER();

	struct oxr_local_3d_zone_ext *zone;
	struct oxr_logger log;
	OXR_VERIFY_LOCAL_3D_ZONE_AND_INIT_LOG(&log, mask, zone, "xrSetLocal3DZoneWholeWindowEXT");

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		xrt_result_t xret =
		    comp_d3d11_compositor_zone_mask_set_whole(&sess->xcn->base, zone->comp_mask, enable3D == XR_TRUE);
		if (xret != XRT_SUCCESS) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "Failed to set whole-window zone mask (%d)",
			                 xret);
		}
		return XR_SUCCESS;
	}
#else
	(void)enable3D;
#endif

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "Local 3D zone masks are not supported for this compositor");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLocal3DZoneFromRectsEXT(XrLocal3DZoneMaskEXT mask, uint32_t rectCount, const XrRect2Di *rects)
{
	OXR_TRACE_MARKER();

	struct oxr_local_3d_zone_ext *zone;
	struct oxr_logger log;
	OXR_VERIFY_LOCAL_3D_ZONE_AND_INIT_LOG(&log, mask, zone, "xrSetLocal3DZoneFromRectsEXT");
	OXR_VERIFY_ARG_NOT_NULL(&log, rects);

	if (rectCount == 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(rectCount == 0) must be at least one rect");
	}

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		struct xrt_rect *xrects = U_TYPED_ARRAY_CALLOC(struct xrt_rect, rectCount);
		if (xrects == NULL) {
			return oxr_error(&log, XR_ERROR_OUT_OF_MEMORY, "Failed to allocate rect array");
		}
		for (uint32_t i = 0; i < rectCount; i++) {
			xrects[i].offset.w = rects[i].offset.x;
			xrects[i].offset.h = rects[i].offset.y;
			xrects[i].extent.w = rects[i].extent.width;
			xrects[i].extent.h = rects[i].extent.height;
		}

		xrt_result_t xret =
		    comp_d3d11_compositor_zone_mask_set_rects(&sess->xcn->base, zone->comp_mask, rectCount, xrects);
		free(xrects);
		if (xret != XRT_SUCCESS) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "Failed to set zone mask rects (%d)", xret);
		}
		return XR_SUCCESS;
	}
#endif

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "Local 3D zone masks are not supported for this compositor");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireLocal3DZoneRenderTargetEXT(XrLocal3DZoneMaskEXT mask, void *binding)
{
	OXR_TRACE_MARKER();

	struct oxr_local_3d_zone_ext *zone;
	struct oxr_logger log;
	OXR_VERIFY_LOCAL_3D_ZONE_AND_INIT_LOG(&log, mask, zone, "xrAcquireLocal3DZoneRenderTargetEXT");
	OXR_VERIFY_ARG_NOT_NULL(&log, binding);

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		XrLocal3DZoneRenderTargetD3D11EXT *d3d11_binding = (XrLocal3DZoneRenderTargetD3D11EXT *)binding;
		if (d3d11_binding->type != XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_EXT) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "(binding->type) expected XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_EXT");
		}

		void *rtv = NULL;
		uint32_t w = 0, h = 0;
		xrt_result_t xret =
		    comp_d3d11_compositor_zone_mask_acquire_rt(&sess->xcn->base, zone->comp_mask, &rtv, &w, &h);
		if (xret != XRT_SUCCESS) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
			                 "Failed to acquire zone mask render target (%d)", xret);
		}

		d3d11_binding->renderTargetView = rtv;
		d3d11_binding->width = w;
		d3d11_binding->height = h;
		return XR_SUCCESS;
	}
#endif

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "Local 3D zone masks are not supported for this compositor");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitLocal3DZoneEXT(XrLocal3DZoneMaskEXT mask)
{
	OXR_TRACE_MARKER();

	struct oxr_local_3d_zone_ext *zone;
	struct oxr_logger log;
	OXR_VERIFY_LOCAL_3D_ZONE_AND_INIT_LOG(&log, mask, zone, "xrSubmitLocal3DZoneEXT");

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		xrt_result_t xret = comp_d3d11_compositor_zone_mask_submit(&sess->xcn->base, zone->comp_mask);
		if (xret != XRT_SUCCESS) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "Failed to submit zone mask (%d)", xret);
		}
		return XR_SUCCESS;
	}
#endif

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "Local 3D zone masks are not supported for this compositor");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyLocal3DZoneMaskEXT(XrLocal3DZoneMaskEXT mask)
{
	OXR_TRACE_MARKER();

	struct oxr_local_3d_zone_ext *zone;
	struct oxr_logger log;
	OXR_VERIFY_LOCAL_3D_ZONE_AND_INIT_LOG(&log, mask, zone, "xrDestroyLocal3DZoneMaskEXT");

	return oxr_handle_destroy(&log, &zone->handle);
}

#endif // OXR_HAVE_EXT_local_3d_zone
