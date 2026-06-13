// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_display_zones entrypoints — N 3D zones + wish mask
 *         (ADR-027, docs/specs/extensions/XR_EXT_display_zones.md).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * Zones are stateless per-frame data (no handles): the zone-scoped locate is
 * parsed in oxr_session.c, the zones-frame gating + zone-layer submission in
 * oxr_session_frame_end.c. Only the two pure queries live here.
 */

#include "xrt/xrt_compiler.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#ifdef OXR_HAVE_EXT_display_zones

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneCapabilitiesEXT(XrSession session, XrDisplayZoneCapabilitiesEXT *capabilities)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDisplayZoneCapabilitiesEXT");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, capabilities, XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT);

	// Never errors on an unsupported session — reports supported = false.
	// Zones need a window-bound native compositor (the same session class
	// as local-2D layers); maxZones3D is plugin-independent (zone assembly
	// is compositor-side, ADR-027 Decision 5). Out-of-process sessions
	// (Android OOP, #568) own no in-process native compositor but still
	// drive a window-bound per-session compositor via the service — the
	// is_service_mode flag covers that class (sess->xcn is the IPC proxy).
	const bool window_bound = (sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                           sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                           sess->has_external_window ||
	                           (sess->sys->xsysc != NULL && sess->sys->xsysc->info.is_service_mode)) &&
	                          sess->xcn != NULL;

	capabilities->supported = window_bound ? XR_TRUE : XR_FALSE;
	capabilities->maxZones3D = window_bound ? OXR_DISPLAY_ZONES_MAX_ZONES_3D : 0;

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneRecommendedViewSizeEXT(XrSession session,
                                           const XrRect2Di *zoneRect,
                                           XrExtent2Di *recommendedViewSize)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDisplayZoneRecommendedViewSizeEXT");
	OXR_VERIFY_ARG_NOT_NULL(&log, zoneRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, recommendedViewSize);

	if (zoneRect->extent.width <= 0 || zoneRect->extent.height <= 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(zoneRect->extent == {%i, %i}) must be positive", zoneRect->extent.width,
		                 zoneRect->extent.height);
	}

	// The zone-rect analog of the shipped canvas model: recommended
	// per-view size == zone rect extent (view dims track canvas dims 1:1
	// in the current display modes). Zones share the session's view COUNT;
	// only per-view dimensions vary by rect. A future mode-aware refinement
	// can scale this without an API change (the query is advisory).
	recommendedViewSize->width = zoneRect->extent.width;
	recommendedViewSize->height = zoneRect->extent.height;

	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_display_zones
