// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_local_3d_zone entrypoints — authored 2D/3D mask (#439,
 *         unified 2D/3D compositing).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * The oxr layer owns the mask handle lifecycle and forwards authoring calls to
 * the native compositor's zone-mask entry points. The D3D11 consumer shipped
 * with Phases 1–2 (docs/roadmap/unified-2d-3d-phase1-impl.md, -phase2-impl.md);
 * the D3D12 consumer with the cross-API leg
 * (docs/roadmap/unified-2d-3d-crossapi-impl.md §3). VK forwards to stubs until
 * its consumer leg lands — the caps query reports supported = false for those
 * sessions, and a stubbed compositor's XRT_ERROR_NOT_IMPLEMENTED maps to
 * XR_ERROR_FEATURE_UNSUPPORTED. Every other compositor falls through to
 * XR_ERROR_FEATURE_UNSUPPORTED.
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

#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
#include "d3d12/comp_d3d12_compositor.h"
#endif

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
#include "vk_native/comp_vk_native_compositor.h"
#endif

#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
#include "metal/comp_metal_compositor.h"
#endif

#ifdef OXR_HAVE_EXT_local_3d_zone

//! D3D11/D3D12/VK max texture dimension — bounds the authored mask size.
#define OXR_LOCAL_3D_ZONE_MAX_MASK_DIM 16384

#if defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR) || defined(XRT_HAVE_D3D12_NATIVE_COMPOSITOR) ||                          \
    defined(XRT_HAVE_VK_NATIVE_COMPOSITOR) || defined(XRT_HAVE_METAL_NATIVE_COMPOSITOR)
#define OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
#endif

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
/*!
 * Map a compositor zone-mask result: stubs (consumer leg not landed) report
 * XRT_ERROR_NOT_IMPLEMENTED → XR_ERROR_FEATURE_UNSUPPORTED; anything else
 * non-success is a runtime failure.
 */
static XrResult
zone_mask_xret_to_xr(struct oxr_logger *log, xrt_result_t xret, const char *what)
{
	if (xret == XRT_SUCCESS) {
		return XR_SUCCESS;
	}
	if (xret == XRT_ERROR_NOT_IMPLEMENTED) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "%s: local 3D zone consumer not implemented for this compositor yet", what);
	}
	return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s failed (%d)", what, xret);
}
#endif


/*
 *
 * Handle lifecycle.
 *
 */

static XrResult
oxr_local_3d_zone_destroy_cb(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_local_3d_zone_ext *zone = (struct oxr_local_3d_zone_ext *)hb;

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (zone->comp_mask != NULL && sess != NULL && sess->xcn != NULL) {
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		if (sess->is_d3d11_native_compositor) {
			comp_d3d11_compositor_zone_mask_destroy(&sess->xcn->base, zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		if (sess->is_d3d12_native_compositor) {
			comp_d3d12_compositor_zone_mask_destroy(&sess->xcn->base, zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		if (sess->is_vk_native_compositor) {
			comp_vk_native_compositor_zone_mask_destroy(&sess->xcn->base, zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		if (sess->is_metal_native_compositor) {
			comp_metal_compositor_zone_mask_destroy(&sess->xcn->base, zone->comp_mask);
		}
#endif
	}
#endif // OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
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
	// VK sessions also report false until that consumer leg lands
	// (docs/roadmap/unified-2d-3d-crossapi-impl.md — flip here per leg).
	capabilities->supported = XR_FALSE;
	capabilities->hardwareZoneGridWidth = 0;
	capabilities->hardwareZoneGridHeight = 0;
	capabilities->maxMaskWidth = 0;
	capabilities->maxMaskHeight = 0;

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		capabilities->supported = XR_TRUE;
		// #224 hardware-DP leg: surface the DP's switchable-lens zone grid
		// (1×1 = global on/off panel; 0×0 = legacy DP, compositor consumer
		// only — the mask still composites, it just can't drive the panel).
		uint32_t grid_w = 0;
		uint32_t grid_h = 0;
		comp_d3d11_compositor_zone_get_hw_caps(&sess->xcn->base, &grid_w, &grid_h);
		capabilities->hardwareZoneGridWidth = grid_w;
		capabilities->hardwareZoneGridHeight = grid_h;
		capabilities->maxMaskWidth = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
		capabilities->maxMaskHeight = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
	}
#endif

#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
	if (sess->is_d3d12_native_compositor && sess->xcn != NULL) {
		// #439 D3D12 consumer leg — parity with the D3D11 consumer.
		capabilities->supported = XR_TRUE;
		capabilities->hardwareZoneGridWidth = 0;
		capabilities->hardwareZoneGridHeight = 0;
		capabilities->maxMaskWidth = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
		capabilities->maxMaskHeight = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
	}
#endif

#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
	if (sess->is_metal_native_compositor && sess->xcn != NULL) {
		// #439 Phase 3 Metal consumer — Tier 1/2 + Local2D layers
		// (no Tier-3 render-target binding on Metal yet).
		capabilities->supported = XR_TRUE;
		uint32_t grid_w = 0;
		uint32_t grid_h = 0;
		comp_metal_compositor_zone_get_hw_caps(&sess->xcn->base, &grid_w, &grid_h);
		capabilities->hardwareZoneGridWidth = grid_w;
		capabilities->hardwareZoneGridHeight = grid_h;
		capabilities->maxMaskWidth = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
		capabilities->maxMaskHeight = OXR_LOCAL_3D_ZONE_MAX_MASK_DIM;
	}
#endif

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
	if (sess->is_vk_native_compositor && sess->xcn != NULL) {
		// #439 Phase 3 VK consumer — Tier 1/2/3 authored masks + Local2D
		// layers (compositor-side composite; no DP-driven hardware zone grid).
		capabilities->supported = XR_TRUE;
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

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	bool api_matched = false;
	if (sess->xcn != NULL) {
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		api_matched = api_matched || sess->is_d3d11_native_compositor;
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		api_matched = api_matched || sess->is_d3d12_native_compositor;
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		api_matched = api_matched || sess->is_vk_native_compositor;
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		api_matched = api_matched || sess->is_metal_native_compositor;
#endif
	}
	if (api_matched) {
		struct oxr_local_3d_zone_ext *zone = NULL;
		OXR_ALLOCATE_HANDLE_OR_RETURN(&log, zone, OXR_XR_DEBUG_LOCAL3DZONE, oxr_local_3d_zone_destroy_cb,
		                              &sess->handle);

		zone->sess = sess;
		// 0 lets the compositor choose (clamped to the client window).
		zone->width = createInfo->maskWidth;
		zone->height = createInfo->maskHeight;

		xrt_result_t xret = XRT_ERROR_NOT_IMPLEMENTED;
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		if (sess->is_d3d11_native_compositor) {
			xret = comp_d3d11_compositor_zone_mask_create(&sess->xcn->base, zone->width, zone->height,
			                                              &zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		if (sess->is_d3d12_native_compositor) {
			xret = comp_d3d12_compositor_zone_mask_create(&sess->xcn->base, zone->width, zone->height,
			                                              &zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		if (sess->is_vk_native_compositor) {
			xret = comp_vk_native_compositor_zone_mask_create(&sess->xcn->base, zone->width, zone->height,
			                                                  &zone->comp_mask);
		}
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		if (sess->is_metal_native_compositor) {
			xret = comp_metal_compositor_zone_mask_create(&sess->xcn->base, zone->width, zone->height,
			                                              &zone->comp_mask);
		}
#endif
		if (xret != XRT_SUCCESS) {
			XrResult ret = zone_mask_xret_to_xr(&log, xret, "xrCreateLocal3DZoneMaskEXT");
			oxr_handle_destroy(&log, &zone->handle);
			return ret;
		}

		*mask = oxr_local_3d_zone_to_openxr(zone);
		return XR_SUCCESS;
	}
#endif // OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR

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

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->xcn != NULL) {
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		if (sess->is_d3d11_native_compositor) {
			return zone_mask_xret_to_xr(&log,
			                            comp_d3d11_compositor_zone_mask_set_whole(
			                                &sess->xcn->base, zone->comp_mask, enable3D == XR_TRUE),
			                            "xrSetLocal3DZoneWholeWindowEXT");
		}
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		if (sess->is_d3d12_native_compositor) {
			return zone_mask_xret_to_xr(&log,
			                            comp_d3d12_compositor_zone_mask_set_whole(
			                                &sess->xcn->base, zone->comp_mask, enable3D == XR_TRUE),
			                            "xrSetLocal3DZoneWholeWindowEXT");
		}
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		if (sess->is_vk_native_compositor) {
			return zone_mask_xret_to_xr(&log,
			                            comp_vk_native_compositor_zone_mask_set_whole(
			                                &sess->xcn->base, zone->comp_mask, enable3D == XR_TRUE),
			                            "xrSetLocal3DZoneWholeWindowEXT");
		}
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		if (sess->is_metal_native_compositor) {
			return zone_mask_xret_to_xr(&log,
			                            comp_metal_compositor_zone_mask_set_whole(
			                                &sess->xcn->base, zone->comp_mask, enable3D == XR_TRUE),
			                            "xrSetLocal3DZoneWholeWindowEXT");
		}
#endif
	}
#else
	(void)enable3D;
#endif // OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR

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

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->xcn != NULL) {
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

		xrt_result_t xret = XRT_ERROR_NOT_IMPLEMENTED;
		bool api_matched = false;
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		if (sess->is_d3d11_native_compositor) {
			xret = comp_d3d11_compositor_zone_mask_set_rects(&sess->xcn->base, zone->comp_mask, rectCount,
			                                                 xrects);
			api_matched = true;
		}
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		if (!api_matched && sess->is_d3d12_native_compositor) {
			xret = comp_d3d12_compositor_zone_mask_set_rects(&sess->xcn->base, zone->comp_mask, rectCount,
			                                                 xrects);
			api_matched = true;
		}
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		if (!api_matched && sess->is_vk_native_compositor) {
			xret = comp_vk_native_compositor_zone_mask_set_rects(&sess->xcn->base, zone->comp_mask,
			                                                     rectCount, xrects);
			api_matched = true;
		}
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		if (!api_matched && sess->is_metal_native_compositor) {
			xret = comp_metal_compositor_zone_mask_set_rects(&sess->xcn->base, zone->comp_mask, rectCount,
			                                                 xrects);
			api_matched = true;
		}
#endif
		free(xrects);
		if (api_matched) {
			return zone_mask_xret_to_xr(&log, xret, "xrSetLocal3DZoneFromRectsEXT");
		}
	}
#endif // OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR

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

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->xcn == NULL) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "Local 3D zone masks are not supported for this compositor");
	}
#endif

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor) {
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
			return zone_mask_xret_to_xr(&log, xret, "xrAcquireLocal3DZoneRenderTargetEXT");
		}

		d3d11_binding->renderTargetView = rtv;
		d3d11_binding->width = w;
		d3d11_binding->height = h;
		return XR_SUCCESS;
	}
#endif

#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
	if (sess->is_d3d12_native_compositor) {
		XrLocal3DZoneRenderTargetD3D12EXT *d3d12_binding = (XrLocal3DZoneRenderTargetD3D12EXT *)binding;
		if (d3d12_binding->type != XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "(binding->type) expected XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT");
		}

		void *resource = NULL;
		uint32_t w = 0, h = 0;
		xrt_result_t xret =
		    comp_d3d12_compositor_zone_mask_acquire_rt(&sess->xcn->base, zone->comp_mask, &resource, &w, &h);
		if (xret != XRT_SUCCESS) {
			return zone_mask_xret_to_xr(&log, xret, "xrAcquireLocal3DZoneRenderTargetEXT");
		}

		d3d12_binding->resource = resource;
		d3d12_binding->width = w;
		d3d12_binding->height = h;
		return XR_SUCCESS;
	}
#endif

#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
	if (sess->is_metal_native_compositor) {
		// No Metal Tier-3 binding type in header v3 — author via Tier 1/2.
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "Tier-3 freeform masks are not available on Metal (use Tier 1/2)");
	}
#endif

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
	if (sess->is_vk_native_compositor) {
		XrLocal3DZoneRenderTargetVulkanEXT *vk_binding = (XrLocal3DZoneRenderTargetVulkanEXT *)binding;
		if (vk_binding->type != XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_EXT) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "(binding->type) expected XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_EXT");
		}

		void *image = NULL;
		void *image_view = NULL;
		uint32_t w = 0, h = 0;
		xrt_result_t xret = comp_vk_native_compositor_zone_mask_acquire_rt(&sess->xcn->base, zone->comp_mask,
		                                                                   &image, &image_view, &w, &h);
		if (xret != XRT_SUCCESS) {
			return zone_mask_xret_to_xr(&log, xret, "xrAcquireLocal3DZoneRenderTargetEXT");
		}

		vk_binding->image = image;
		vk_binding->imageView = image_view;
		vk_binding->width = w;
		vk_binding->height = h;
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

#ifdef OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR
	struct oxr_session *sess = zone->sess;
	if (sess->xcn != NULL) {
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		if (sess->is_d3d11_native_compositor) {
			return zone_mask_xret_to_xr(
			    &log, comp_d3d11_compositor_zone_mask_submit(&sess->xcn->base, zone->comp_mask),
			    "xrSubmitLocal3DZoneEXT");
		}
#endif
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
		if (sess->is_d3d12_native_compositor) {
			return zone_mask_xret_to_xr(
			    &log, comp_d3d12_compositor_zone_mask_submit(&sess->xcn->base, zone->comp_mask),
			    "xrSubmitLocal3DZoneEXT");
		}
#endif
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
		if (sess->is_vk_native_compositor) {
			return zone_mask_xret_to_xr(
			    &log, comp_vk_native_compositor_zone_mask_submit(&sess->xcn->base, zone->comp_mask),
			    "xrSubmitLocal3DZoneEXT");
		}
#endif
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
		if (sess->is_metal_native_compositor) {
			return zone_mask_xret_to_xr(
			    &log, comp_metal_compositor_zone_mask_submit(&sess->xcn->base, zone->comp_mask),
			    "xrSubmitLocal3DZoneEXT");
		}
#endif
	}
#endif // OXR_LOCAL_3D_ZONE_HAVE_ANY_COMPOSITOR

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
