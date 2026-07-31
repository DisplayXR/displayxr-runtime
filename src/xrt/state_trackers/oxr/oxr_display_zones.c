// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_display_zones entrypoints — N 3D zones + wish mask
 *         (ADR-027, docs/specs/extensions/XR_DXR_display_zones.md).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * Zones are stateless per-frame data (no handles): the zone-scoped locate is
 * parsed in oxr_session.c, the zones-frame gating + zone-layer submission in
 * oxr_session_frame_end.c. Only the two pure queries live here.
 */

#include <stdlib.h>

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h" // struct xrt_window_metrics (tile size, #225)

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#ifdef OXR_HAVE_DXR_display_zones

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneCapabilitiesDXR(XrSession session, XrDisplayZoneCapabilitiesDXR *capabilities)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDisplayZoneCapabilitiesDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, capabilities, XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR);

	// Never errors on an unsupported session — reports supported = false.
	// Zones need a window-bound native compositor (the same session class
	// as local-2D layers); maxZones3D is plugin-independent (zone assembly
	// is compositor-side, ADR-027 Decision 5). Out-of-process sessions
	// (Android OOP, #568) own no in-process native compositor but still
	// drive a window-bound per-session compositor via the service — the
	// is_service_mode flag covers that class (sess->xcn is the IPC proxy).
	const bool window_bound = (sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                           sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                           sess->is_vk_native_compositor || sess->has_external_window ||
	                           (sess->sys->xsysc != NULL && sess->sys->xsysc->info.is_service_mode)) &&
	                          sess->xcn != NULL;

	capabilities->supported = window_bound ? XR_TRUE : XR_FALSE;
	capabilities->maxZones3D = window_bound ? OXR_DISPLAY_ZONES_MAX_ZONES_3D : 0;

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneRecommendedViewSizeDXR(XrSession session,
                                           const XrRect2Di *zoneRect,
                                           XrExtent2Di *recommendedViewSize)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDisplayZoneRecommendedViewSizeDXR");
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
	// only per-view dimensions vary by rect.
	recommendedViewSize->width = zoneRect->extent.width;
	recommendedViewSize->height = zoneRect->extent.height;

	// Optional supersample trim (DXR_ZONE_VIEW_SCALE, unset = 1:1 as before).
	//
	// 1:1 means every zones app renders the full rect per view while the active
	// mode's view scale then resamples it DOWN — Leia's 3D modes are 0.5x0.5, so
	// each view carries ~4x the fragments the panel can resolve. Trimming here
	// cuts fill for every zones app at once (native and Unity alike), because
	// both consume this advisory value.
	//
	// This trades quality, so it is opt-in and clamped: the floor is the active
	// mode's own view scale, i.e. we never advertise FEWER pixels than the weave
	// actually resolves, and never more than the rect.
	//
	// Measure before enabling, and expect a SMALL win. On the Meteor Lake iGPU +
	// Leia DS1_156 reference box, a 0.5 trim halves the demo-avatar renderer's own
	// GPU cost (1.9 -> 1.0 % of the device) but moves total app GPU by only ~3 %
	// (~0.5 points of ~15) — the app's fill is a small share of a `_handle` app's
	// cost, most of which is present + weave at panel resolution and untouched by
	// this. An earlier -15.7 % figure did not replicate; it came from spot-sampled
	// GPU% with too few samples. So this is a fill knob for fill-bound apps, not a
	// general throughput win.
	{
		static float s_scale = -1.0f; // <0 = not yet read
		if (s_scale < 0.0f) {
			const char *e = getenv("DXR_ZONE_VIEW_SCALE");
			s_scale = (e != NULL) ? (float)atof(e) : 0.0f;
			if (s_scale > 0.0f) {
				U_LOG_W("display zones: DXR_ZONE_VIEW_SCALE=%.3f "
				        "(recommended view size trimmed toward the "
				        "mode's resolved resolution)",
				        (double)s_scale);
			}
		}

		if (s_scale > 0.0f && s_scale < 1.0f && sess->sys != NULL && sess->sys->xsysc != NULL) {
			// Per axis, because modes may scale x and y differently.
			float fx = sess->sys->xsysc->info.recommended_view_scale_x;
			float fy = sess->sys->xsysc->info.recommended_view_scale_y;
			if (!(fx > 0.0f) || fx > 1.0f) {
				fx = 1.0f; // no/absurd advertisement: no trim on this axis
			}
			if (!(fy > 0.0f) || fy > 1.0f) {
				fy = 1.0f;
			}

			const float sx = (s_scale < fx) ? fx : s_scale;
			const float sy = (s_scale < fy) ? fy : s_scale;

			int32_t w = (int32_t)((float)zoneRect->extent.width * sx + 0.5f);
			int32_t h = (int32_t)((float)zoneRect->extent.height * sy + 0.5f);
			if (w < 1) {
				w = 1;
			}
			if (h < 1) {
				h = 1;
			}
			recommendedViewSize->width = w;
			recommendedViewSize->height = h;
		}
	}

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceTileSizeDXR(XrSession session, XrExtent2Di *tileSize)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetWorkspaceTileSizeDXR");
	OXR_VERIFY_ARG_NOT_NULL(&log, tileSize);

	// The client's live target canvas = the shell-driven per-client window rect
	// (#225). window_pixel_* is recomputed from the tile pose on every resize
	// (slot_pose_to_pixel_rect), so this tracks the 3D-window size — which a
	// minimized engine tile's own backbuffer cannot. 0x0 before the slot binds
	// → the app keeps its current authoring size and re-queries next frame.
	struct xrt_window_metrics wm = {0};
	if (oxr_session_get_window_metrics(sess, &wm) && wm.valid &&
	    wm.window_pixel_width > 0 && wm.window_pixel_height > 0) {
		tileSize->width = (int32_t)wm.window_pixel_width;
		tileSize->height = (int32_t)wm.window_pixel_height;
		return XR_SUCCESS;
	}

	tileSize->width = 0;
	tileSize->height = 0;
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_display_zones
