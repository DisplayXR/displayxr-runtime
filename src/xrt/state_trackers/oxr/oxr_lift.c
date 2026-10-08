// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift API entry points — 2D→3D conversion streams (ADR-042).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * The conversion runs in the service (d3d11_lift.cpp, its own thread and
 * device); these entry points validate, call ipc_client_lift.c on a service
 * connection and translate results. Lift streams belong to a CONNECTION, not a
 * session, so there are two routes to the one conversion module:
 *
 *  - an IPC session uses its own connection (ipc_client_compositor.c hands it
 *    over — the symbol resolves at link time, the oxr_weave.c pattern);
 *  - an in-process D3D11 / D3D12 session on Windows (engine apps — the Unity
 *    display provider renders in-process on its own D3D12 device) uses a
 *    lift-only connection the session owns (ipc_client_lift_link.c), opened in
 *    the background on first use (ADR-049). Results come back as the same NT
 *    handles (texture + ID3D11Fence-created shared fence) a D3D12 device opens
 *    with OpenSharedHandle.
 *
 * Any other in-process session reports XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Error contract (mirrors XR_DXR_weave §4b):
 *  - IPC session: a dead pipe (XRT_ERROR_IPC_FAILURE) marks the session lost →
 *    XR_ERROR_INSTANCE_LOST, then XR_ERROR_SESSION_LOST;
 *  - in-process session: a dead lift connection never touches the session —
 *    the call is a transient XR_ERROR_RUNTIME_FAILURE, streams of that
 *    connection then report XR_ERROR_FEATURE_UNSUPPORTED (recreate them), and
 *    properties reconnect in the background;
 *  - a transient service refusal (XRT_ERROR_WEAVE_REFUSED — keyed-mutex miss)
 *    is a non-fatal XR_ERROR_RUNTIME_FAILURE, retry next frame;
 *  - no module / mode unsupported (XRT_ERROR_FEATURE_NOT_SUPPORTED) is
 *    XR_ERROR_FEATURE_UNSUPPORTED, permanent for that mode.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_xret.h"
#include "oxr_handle.h"
#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_chain.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"
#include "util/u_logging.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_lift.h"

// st_oxr carries the ipc include path (oxr_workspace.c); these two headers
// need nothing generated.
#include "client/ipc_client_lift.h"
#include "client/ipc_client_lift_link.h"

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef OXR_HAVE_DXR_lift

// IPC-session bridge (defined in ipc_client_compositor.c): the session's connection.
struct ipc_connection *
comp_ipc_client_compositor_lift_connection(struct xrt_compositor *xc);

// The OpenXR and DP encodings must agree (the wire carries the DP one).
_Static_assert(XR_LIFT_MODE_DEPTH_DXR == XRT_DP_LIFT_MODE_DEPTH, "lift mode mismatch");
_Static_assert(XR_LIFT_MODE_SBS_DXR == XRT_DP_LIFT_MODE_SBS, "lift mode mismatch");
_Static_assert(XR_LIFT_MODE_NVIEW_DXR == XRT_DP_LIFT_MODE_NVIEW, "lift mode mismatch");
_Static_assert(XR_LIFT_MODE_GAUSSIANS_DXR == XRT_DP_LIFT_MODE_GAUSSIANS, "lift mode mismatch");
_Static_assert(XR_LIFT_STATE_READY_DXR == XRT_DP_LIFT_STATE_READY, "lift state mismatch");
_Static_assert(XR_LIFT_STATE_ACTIVATING_DXR == XRT_DP_LIFT_STATE_ACTIVATING, "lift state mismatch");
_Static_assert(XR_LIFT_BLOB_FORMAT_PLY_3DGS_DXR == XRT_DP_LIFT_BLOB_PLY_3DGS, "blob format mismatch");
_Static_assert(XR_LIFT_BLOB_FORMAT_SOG_DXR == XRT_DP_LIFT_BLOB_SOG, "blob format mismatch");
_Static_assert(XR_LIFT_PRIORITY_PAUSED_DXR == 0 && XR_LIFT_PRIORITY_LOW_DXR == 1 && XR_LIFT_PRIORITY_NORMAL_DXR == 2 &&
                   XR_LIFT_PRIORITY_HIGH_DXR == 3,
               "lift priority values are the wire values (u_lift_priority)");
_Static_assert(XR_LIFT_MAX_VIEWS_DXR == XRT_LIFT_MAX_VIEWS, "lift view bound mismatch");
_Static_assert(XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR == XRT_LIFT_WEAVE_RECTS_MAX, "lift rect bound mismatch");
_Static_assert(XR_LIFT_BACKEND_NAME_MAX_SIZE_DXR == sizeof(((struct xrt_dp_lift_caps *)0)->backend),
               "backend name size mismatch");
_Static_assert(XR_LIFT_DEPTH_ENCODING_LINEAR_DXR == XRT_DP_LIFT_DEPTH_ENCODING_LINEAR &&
                   XR_LIFT_DEPTH_ENCODING_INVERSE_DXR == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE,
               "lift depth encoding mismatch");

//! Same rule as oxr_weave.c: IPC sessions carry no in-process native flag.
static bool
lift_session_is_ipc(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		return false;
	}
	bool inprocess = sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                 sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                 sess->is_vk_native_compositor;
	return !inprocess;
}

/*!
 * ADR-049: which in-process sessions reach the service's module over a
 * lift-only connection. The conversion service is the Windows D3D11 service
 * and its results are D3D NT handles, which a D3D11 or D3D12 device opens
 * directly; other in-process APIs keep XR_ERROR_FEATURE_UNSUPPORTED.
 */
static bool
lift_inprocess_eligible(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	return sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor;
#else
	(void)sess;
	return false;
#endif
}

//! The session's lift link, created on first use (no connect yet).
static struct ipc_client_lift_link *
lift_link_get(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	struct ipc_client_lift_link *link = (struct ipc_client_lift_link *)InterlockedCompareExchangePointer(
	    (PVOID volatile *)&sess->lift_link, NULL, NULL);
	if (link != NULL) {
		return link;
	}

	// Name the connection after the executable so `displayxr-cli clients` and
	// the service log say whose it is.
	char exe[MAX_PATH] = {0};
	const char *base = "app";
	if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe)) > 0) {
		const char *slash = strrchr(exe, '\\');
		base = slash != NULL ? slash + 1 : exe;
	}
	char label[XRT_MAX_APPLICATION_NAME_SIZE];
	snprintf(label, sizeof(label), "%s (in-process lift)", base);

	struct ipc_client_lift_link *made = NULL;
	if (ipc_client_lift_link_create(label, &made) != XRT_SUCCESS) {
		return NULL;
	}
	// Two threads may race the first lift call of a session: one link wins.
	link = (struct ipc_client_lift_link *)InterlockedCompareExchangePointer((PVOID volatile *)&sess->lift_link,
	                                                                        made, NULL);
	if (link != NULL) {
		ipc_client_lift_link_destroy(&made);
		return link;
	}
	return made;
#else
	(void)sess;
	return NULL;
#endif
}

void
oxr_lift_session_fini(struct oxr_session *sess)
{
	if (sess != NULL && sess->lift_link != NULL) {
		ipc_client_lift_link_destroy(&sess->lift_link);
	}
}

//! The connection one lift call rides, and how its failure is treated.
struct lift_route
{
	struct ipc_connection *c; //!< NULL = no connection right now (in-process only)
	bool inprocess;           //!< the session's lift-only link (ADR-049)
	uint32_t gen;             //!< link generation the connection belongs to
	enum ipc_client_lift_link_state state;
};

/*!
 * Pick the route. IPC session: its own connection, always. Eligible in-process
 * session: the lift link; @p wait_ms bounds a wait for an in-flight connect
 * (0 for everything but stream creation). On XR_SUCCESS the caller must
 * lift_route_close() it; route->c may still be NULL in-process (see state).
 */
static XrResult
lift_route_open(
    struct oxr_logger *log, struct oxr_session *sess, uint32_t wait_ms, struct lift_route *route, const char *what)
{
	memset(route, 0, sizeof(*route));
	if (lift_session_is_ipc(sess)) {
		route->c = comp_ipc_client_compositor_lift_connection(&sess->xcn->base);
		route->state = IPC_CLIENT_LIFT_LINK_CONNECTED;
		return XR_SUCCESS;
	}
	if (!lift_inprocess_eligible(sess)) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "%s: in-process 2D→3D conversion is available to D3D11 / D3D12 sessions on Windows "
		                 "only (other sessions: the out-of-process service path)",
		                 what);
	}
	struct ipc_client_lift_link *link = lift_link_get(sess);
	if (link == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: could not create the lift service link", what);
	}
	route->inprocess = true;
	route->c = ipc_client_lift_link_acquire_wait(link, wait_ms, &route->state, &route->gen);
	return XR_SUCCESS;
}

static void
lift_route_close(struct oxr_session *sess, struct lift_route *route, xrt_result_t last_xret)
{
	if (route->inprocess && route->c != NULL) {
		ipc_client_lift_link_release(sess->lift_link, route->gen, last_xret == XRT_ERROR_IPC_FAILURE);
	}
	route->c = NULL;
}

/*!
 * A stream call's route: the stream's own connection, or -- in-process, when
 * that connection died -- XR_ERROR_FEATURE_UNSUPPORTED (the stream is gone with
 * it; the app destroys it and creates a new one).
 */
static XrResult
lift_stream_route_open(struct oxr_logger *log,
                       struct oxr_lift_stream_dxr *st,
                       struct lift_route *route,
                       const char *what)
{
	XrResult r = lift_route_open(log, st->sess, 0, route, what);
	if (r != XR_SUCCESS) {
		return r;
	}
	if (route->inprocess && (route->c == NULL || route->gen != st->link_gen)) {
		lift_route_close(st->sess, route, XRT_SUCCESS);
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "%s: this stream belonged to a lift service connection that is gone - destroy it and "
		                 "create a new one once xrGetLiftPropertiesDXR reports READY",
		                 what);
	}
	return XR_SUCCESS;
}

//! Map a service result; returns XR_SUCCESS only for XRT_SUCCESS.
static XrResult
lift_xret(struct oxr_logger *log,
          struct oxr_session *sess,
          const struct lift_route *route,
          xrt_result_t xret,
          const char *what)
{
	switch (xret) {
	case XRT_SUCCESS: return XR_SUCCESS;
	case XRT_ERROR_IPC_FAILURE:
		if (route->inprocess) {
			// The session renders in-process: losing the lift-only connection
			// loses its streams, never the session (ADR-049).
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
			                 "%s: the lift service connection is gone (session unaffected; recreate lift "
			                 "streams once xrGetLiftPropertiesDXR reports READY)",
			                 what);
		}
		sess->has_lost = true;
		return oxr_error(log, XR_ERROR_INSTANCE_LOST, "%s: the runtime service connection is gone", what);
	case XRT_ERROR_FEATURE_NOT_SUPPORTED:
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: no conversion module for this request", what);
	case XRT_ERROR_CLIENT_LIMIT_REACHED:
		return oxr_error(log, XR_ERROR_LIMIT_REACHED, "%s: maxStreams reached", what);
	default:
		// Transient (XRT_ERROR_WEAVE_REFUSED) or a stream the service no longer
		// knows: non-fatal, the session stays usable.
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: refused by the service (xrt_result=%d)", what,
		                 (int)xret);
	}
}

static void
lift_params_from_xr(const XrLiftOptionsDXR *o, uint32_t mode, struct xrt_dp_lift_params *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->convergence = o->convergence < 0.0f ? -1.0f : (o->convergence > 1.0f ? 1.0f : o->convergence);
	out->strength = o->strength >= 0.0f ? o->strength : 1.0f;
	out->inpaint = o->inpaint == XR_TRUE ? 1u : 0u;
	out->view_count = o->viewCount;
	out->focal_px = o->focalPx > 0.0f ? o->focalPx : 0.0f;
	if (out->view_count == 0) {
		out->view_count = mode == XRT_DP_LIFT_MODE_NVIEW ? 4u : 2u;
	}
}

static XrResult
lift_validate_options(struct oxr_logger *log, const XrLiftOptionsDXR *o, bool weave_path)
{
	if (o->viewCount > XR_LIFT_MAX_VIEWS_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftOptionsDXR::viewCount (%u) > %u",
		                 o->viewCount, (uint32_t)XR_LIFT_MAX_VIEWS_DXR);
	}
	if (o->viewpointSource != XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR &&
	    o->viewpointSource != XR_LIFT_VIEWPOINT_SOURCE_EXPLICIT_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftOptionsDXR::viewpointSource (%d) invalid",
		                 (int)o->viewpointSource);
	}
	(void)weave_path; // XR_DXR_lift v3: EXPLICIT is valid on the weave path too
	if (o->viewpointSource == XR_LIFT_VIEWPOINT_SOURCE_EXPLICIT_DXR) {
		if (o->viewCount == 0 || o->viewpoints == NULL) {
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrLiftOptionsDXR: EXPLICIT viewpoints need viewCount >= 1 and viewpoints");
		}
	}
	return XR_SUCCESS;
}

/*!
 * XrLiftViewControlDXR (spec v2, ADR-048) -> the runtime's view control.
 * Validates the enums; the service clamps the floats. Shared with
 * oxr_weave.c (the weave-rect chain).
 */
XrResult
oxr_lift_view_control_from_xr(struct oxr_logger *log, const XrLiftViewControlDXR *v, struct xrt_lift_view_control *out)
{
	if (v->axisMode < XR_LIFT_AXIS_MODE_X_DXR || v->axisMode > XR_LIFT_AXIS_MODE_XYZ_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftViewControlDXR::axisMode (%d) invalid",
		                 (int)v->axisMode);
	}
	if (v->recenterMode < XR_LIFT_RECENTER_MODE_OFF_DXR || v->recenterMode > XR_LIFT_RECENTER_MODE_EASE_BACK_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftViewControlDXR::recenterMode (%d) invalid",
		                 (int)v->recenterMode);
	}
	memset(out, 0, sizeof(*out));
	out->ipd_factor = v->ipdFactor;
	out->parallax_factor = v->parallaxFactor;
	out->axis_mode = (uint32_t)v->axisMode;
	out->max_offset_m = v->maxOffsetMeters;
	out->recenter_mode = (uint32_t)v->recenterMode;
	out->hold_s = v->recenterHoldSeconds;
	out->tau_s = v->recenterTimeConstantSeconds;
	return XR_SUCCESS;
}

/*!
 * XR_DXR_lift v3 (ADR-048 Addendum A): the app rig on a lift, if any — an
 * XrCameraRigDXR or XrDisplayRigDXR chained on @p direct (wins) or on
 * @p options (either may be NULL); a camera rig wins over a display rig, as in
 * xrLocateViews. Boundary-converted exactly like xrLocateViews' rig
 * (verticalFov -> half-tangent, convergenceDiopters = inverse distance,
 * metersToVirtual 0 -> 1); the service clamps. Shared with oxr_weave.c.
 * Returns false (out->type NONE) when none is chained.
 */
bool
oxr_lift_rig_from_chain(const void *direct, const void *options, struct xrt_lift_rig *out)
{
	memset(out, 0, sizeof(*out));
	out->type = XRT_LIFT_RIG_NONE;
	const void *chains[2] = {direct, options};
	for (int c = 0; c < 2; c++) {
		const XrBaseInStructure *head = (const XrBaseInStructure *)chains[c];
		if (head == NULL) {
			continue;
		}
		const XrCameraRigDXR *crig = OXR_GET_INPUT_FROM_CHAIN(head, XR_TYPE_CAMERA_RIG_DXR, XrCameraRigDXR);
		if (crig != NULL) {
			out->type = XRT_LIFT_RIG_CAMERA;
			out->ipd_factor = crig->ipdFactor;
			out->parallax_factor = crig->parallaxFactor;
			out->inv_convergence_distance = crig->convergenceDiopters;
			out->half_tan_vfov = tanf(crig->verticalFov * 0.5f);
			out->m2v = crig->metersToVirtual > 0.0f ? crig->metersToVirtual : 1.0f;
			out->perspective_factor = 1.0f;
			return true;
		}
		const XrDisplayRigDXR *drig = OXR_GET_INPUT_FROM_CHAIN(head, XR_TYPE_DISPLAY_RIG_DXR, XrDisplayRigDXR);
		if (drig != NULL) {
			out->type = XRT_LIFT_RIG_DISPLAY;
			out->ipd_factor = drig->ipdFactor;
			out->parallax_factor = drig->parallaxFactor;
			out->perspective_factor = drig->perspectiveFactor;
			out->m2v = 1.0f;
			return true;
		}
	}
	return false;
}

static XrResult
oxr_lift_stream_destroy_cb(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_lift_stream_dxr *st = (struct oxr_lift_stream_dxr *)hb;
	// Best effort: a dead connection already took every stream with it.
	if (st->sess != NULL && st->sess->xcn != NULL && !st->sess->has_lost) {
		struct lift_route route;
		if (lift_route_open(log, st->sess, 0, &route, "xrDestroyLiftStreamDXR") == XR_SUCCESS) {
			xrt_result_t xret = XRT_SUCCESS;
			if (route.c != NULL && (!route.inprocess || route.gen == st->link_gen)) {
				xret = ipc_client_lift_stream_destroy(route.c, st->id);
			}
			lift_route_close(st->sess, &route, xret);
		}
	}
	free(st);
	return XR_SUCCESS;
}


/*
 *
 * Entry points.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetLiftPropertiesDXR(XrSession session, XrLiftPropertiesDXR *properties)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetLiftPropertiesDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_lift);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, properties, XR_TYPE_LIFT_PROPERTIES_DXR);

	struct lift_route route;
	XrResult r = lift_route_open(&log, sess, 0, &route, "xrGetLiftPropertiesDXR");
	if (r != XR_SUCCESS) {
		return r;
	}

	struct xrt_dp_lift_caps caps;
	xrt_dp_lift_caps_init(&caps);
	if (route.c == NULL) {
		// In-process, no connection right now (ADR-049): a connect in flight
		// reads ACTIVATING (poll), no service reads UNAVAILABLE (open default).
		caps.state = route.state == IPC_CLIENT_LIFT_LINK_CONNECTING ? XRT_DP_LIFT_STATE_ACTIVATING
		                                                            : XRT_DP_LIFT_STATE_UNAVAILABLE;
	} else {
		xrt_result_t xret = ipc_client_lift_get_properties(route.c, &caps);
		lift_route_close(sess, &route, xret);
		r = lift_xret(&log, sess, &route, xret, "xrGetLiftPropertiesDXR");
		if (r != XR_SUCCESS) {
			return r;
		}
	}
	properties->supportedModes = (XrLiftModeFlagsDXR)caps.modes;
	properties->maxStreams = caps.max_streams;
	properties->maxViews = caps.max_views;
	properties->depthSemantics = caps.depth_semantics == XRT_DP_LIFT_DEPTH_METRIC
	                                 ? XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR
	                                 : XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR;
	properties->state = caps.state == XRT_DP_LIFT_STATE_READY        ? XR_LIFT_STATE_READY_DXR
	                    : caps.state == XRT_DP_LIFT_STATE_ACTIVATING ? XR_LIFT_STATE_ACTIVATING_DXR
	                                                                 : XR_LIFT_STATE_UNAVAILABLE_DXR;
	memcpy(properties->backend, caps.backend, sizeof(properties->backend));
	properties->backend[sizeof(properties->backend) - 1] = '\0';
	properties->typicalLatency = (XrDuration)caps.typical_latency_ns;

	// Spec v3 (ADR-048 Addendum A): the auxiliary-depth capability.
	XrLiftDepthPropertiesDXR *dp =
	    OXR_GET_OUTPUT_FROM_CHAIN(properties, XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR, XrLiftDepthPropertiesDXR);
	if (dp != NULL) {
		dp->auxDepthSupported = (caps.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0 ? XR_TRUE : XR_FALSE;
		dp->auxDepthSemantics = caps.aux_depth_semantics == XRT_DP_LIFT_DEPTH_METRIC
		                            ? XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR
		                            : XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateLiftStreamDXR(XrSession session, const XrLiftStreamCreateInfoDXR *createInfo, XrLiftStreamDXR *stream)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCreateLiftStreamDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_lift);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, createInfo, XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, stream);

	const uint32_t mode = (uint32_t)createInfo->mode;
	if (mode != XRT_DP_LIFT_MODE_DEPTH && mode != XRT_DP_LIFT_MODE_SBS && mode != XRT_DP_LIFT_MODE_NVIEW &&
	    mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftStreamCreateInfoDXR::mode (%d) is not one mode", (int)createInfo->mode);
	}
	if (createInfo->contentHint != XR_LIFT_CONTENT_HINT_VIDEO_DXR &&
	    createInfo->contentHint != XR_LIFT_CONTENT_HINT_PHOTO_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftStreamCreateInfoDXR::contentHint (%d) invalid", (int)createInfo->contentHint);
	}
	if (mode == XRT_DP_LIFT_MODE_GAUSSIANS && createInfo->contentHint != XR_LIFT_CONTENT_HINT_PHOTO_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftStreamCreateInfoDXR: GAUSSIANS streams take PHOTO content only");
	}
	if (createInfo->inputScale < 0.0f || createInfo->inputScale > 1.0f) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftStreamCreateInfoDXR::inputScale (%f) must be in [0, 1]",
		                 (double)createInfo->inputScale);
	}
	// Spec v3 (ADR-048 Addendum A): auxiliary depth, SBS / NVIEW only.
	const XrLiftDepthRequestDXR *dreq =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo, XR_TYPE_LIFT_DEPTH_REQUEST_DXR, XrLiftDepthRequestDXR);
	uint32_t aux_outputs = 0, aux_depth_format = 0;
	if (dreq != NULL) {
		if (mode != XRT_DP_LIFT_MODE_SBS && mode != XRT_DP_LIFT_MODE_NVIEW) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrLiftDepthRequestDXR: auxiliary depth is for SBS / NVIEW streams only");
		}
		aux_outputs = XRT_DP_LIFT_AUX_DEPTH;
		aux_depth_format = dreq->preferredFormat > 0 && dreq->preferredFormat <= UINT32_MAX
		                       ? (uint32_t)dreq->preferredFormat
		                       : 0u;
	}
	// Creation is a setup call: in-process it may wait for the background
	// connect (bounded by the connect's own service-starting window).
	struct lift_route route;
	XrResult r = lift_route_open(&log, sess, 6000, &route, "xrCreateLiftStreamDXR");
	if (r != XR_SUCCESS) {
		return r;
	}
	if (route.c == NULL) {
		if (route.state == IPC_CLIENT_LIFT_LINK_CONNECTING) {
			return oxr_error(
			    &log, XR_ERROR_RUNTIME_FAILURE,
			    "xrCreateLiftStreamDXR: the lift service connection is still being made - retry");
		}
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCreateLiftStreamDXR: no service to convert with (xrGetLiftPropertiesDXR reports "
		                 "UNAVAILABLE)");
	}

	uint64_t id = 0;
	xrt_result_t xret = ipc_client_lift_stream_create(route.c, mode, (uint32_t)createInfo->contentHint,
	                                                  createInfo->inputScale > 0.0f ? createInfo->inputScale : 1.0f,
	                                                  aux_outputs, aux_depth_format, &id);
	const uint32_t gen = route.gen;
	lift_route_close(sess, &route, xret);
	r = lift_xret(&log, sess, &route, xret, "xrCreateLiftStreamDXR");
	if (r != XR_SUCCESS) {
		return r;
	}

	struct oxr_lift_stream_dxr *st = NULL;
	OXR_ALLOCATE_HANDLE_OR_RETURN(&log, st, OXR_XR_DEBUG_LIFTSTREAM, oxr_lift_stream_destroy_cb, &sess->handle);
	st->sess = sess;
	st->id = id;
	st->mode = mode;
	st->depth_requested = aux_outputs != 0;
	st->link_gen = gen;
	*stream = XRT_CAST_PTR_TO_OXR_HANDLE(XrLiftStreamDXR, st);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyLiftStreamDXR(XrLiftStreamDXR stream)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrDestroyLiftStreamDXR");
	return oxr_handle_destroy(&log, &st->handle);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitLiftFrameDXR(XrLiftStreamDXR stream, const XrLiftFrameSubmitInfoDXR *submitInfo, uint64_t *frameId)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrSubmitLiftFrameDXR");
	struct oxr_session *sess = st->sess;
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, submitInfo, XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, frameId);
	*frameId = 0;

	if (submitInfo->inputTexture == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "XrLiftFrameSubmitInfoDXR::inputTexture is NULL");
	}
	if (submitInfo->extent.width <= 0 || submitInfo->extent.height <= 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftFrameSubmitInfoDXR::extent (%dx%d) must be positive", submitInfo->extent.width,
		                 submitInfo->extent.height);
	}

	struct xrt_dp_lift_params params;
	const struct xrt_dp_lift_params *pp = NULL;
	float vps[3 * XR_LIFT_MAX_VIEWS_DXR];
	uint32_t vp_count = 0;
	struct xrt_lift_view_control vc;
	const struct xrt_lift_view_control *vcp = NULL;
	struct xrt_lift_rig rig;
	const struct xrt_lift_rig *rigp = NULL;
	const XrLiftOptionsDXR *opt = OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_LIFT_OPTIONS_DXR, XrLiftOptionsDXR);
	if (opt != NULL) {
		XrResult vr = lift_validate_options(&log, opt, /*weave_path*/ false);
		if (vr != XR_SUCCESS) {
			return vr;
		}
		lift_params_from_xr(opt, st->mode, &params);
		pp = &params;
		const XrLiftViewControlDXR *v =
		    OXR_GET_INPUT_FROM_CHAIN(opt, XR_TYPE_LIFT_VIEW_CONTROL_DXR, XrLiftViewControlDXR);
		if (v != NULL) {
			vr = oxr_lift_view_control_from_xr(&log, v, &vc);
			if (vr != XR_SUCCESS) {
				return vr;
			}
			vcp = &vc;
		}
		// Spec v3: an app rig rides with the options (absent = cleared).
		if (oxr_lift_rig_from_chain(NULL, opt, &rig)) {
			rigp = &rig;
		}
		if (opt->viewpointSource == XR_LIFT_VIEWPOINT_SOURCE_EXPLICIT_DXR) {
			vp_count = opt->viewCount;
			for (uint32_t i = 0; i < vp_count; i++) {
				vps[3 * i + 0] = opt->viewpoints[i].x;
				vps[3 * i + 1] = opt->viewpoints[i].y;
				vps[3 * i + 2] = opt->viewpoints[i].z;
			}
		}
	}

	struct lift_route route;
	XrResult rr = lift_stream_route_open(&log, st, &route, "xrSubmitLiftFrameDXR");
	if (rr != XR_SUCCESS) {
		return rr;
	}
	xrt_result_t xret = ipc_client_lift_submit(
	    route.c, st->id, (xrt_graphics_buffer_handle_t)(intptr_t)submitInfo->inputTexture,
	    submitInfo->inputIsDxgi == XR_TRUE, (uint32_t)submitInfo->extent.width, (uint32_t)submitInfo->extent.height,
	    (int64_t)submitInfo->sourceTime, pp, vp_count > 0 ? vps : NULL, vp_count, vcp, rigp, frameId);
	lift_route_close(sess, &route, xret);
	return lift_xret(&log, sess, &route, xret, "xrSubmitLiftFrameDXR");
}

//! Fill an XrLiftDepthResultDXR from an acquired result (+ handles on first
//! depth acquire / reallocation) over the result's @p route; on a failed
//! export *out_xret carries the service result for lift_route_close().
static XrResult
lift_fill_depth_result(struct oxr_logger *log,
                       struct oxr_session *sess,
                       struct oxr_lift_stream_dxr *st,
                       const struct lift_route *route,
                       const struct xrt_lift_result *r,
                       XrLiftDepthResultDXR *d,
                       xrt_result_t *out_xret)
{
	XrStructureType type = d->type;
	void *next = d->next;
	memset(d, 0, sizeof(*d));
	d->type = type;
	d->next = next;
	const struct xrt_lift_depth_info *di = &r->depth;
	if (!st->depth_requested || di->valid == 0) {
		return XR_SUCCESS; // depthValid XR_FALSE, everything else zero
	}
	d->depthValid = XR_TRUE;
	d->fenceValue = r->fence_value;
	d->extent.width = (int32_t)di->width;
	d->extent.height = (int32_t)di->height;
	d->format = (int64_t)di->format;
	d->units = di->units == XRT_DP_LIFT_DEPTH_METRIC ? XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR
	                                                 : XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR;
	d->encoding = di->encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE ? XR_LIFT_DEPTH_ENCODING_INVERSE_DXR
	                                                                 : XR_LIFT_DEPTH_ENCODING_LINEAR_DXR;
	d->valueScale = di->value_scale;
	d->valueOffset = di->value_offset;
	d->nearDepth = di->near_depth;
	d->farDepth = di->far_depth;
	d->sameInference = di->same_inference != 0 ? XR_TRUE : XR_FALSE;
	d->frameId = r->frame_id;
	d->intrinsicsValid = di->intrinsics_valid != 0 ? XR_TRUE : XR_FALSE;
	d->focalLengthPx.x = di->focal[0];
	d->focalLengthPx.y = di->focal[1];
	d->principalPointPx.x = di->principal[0];
	d->principalPointPx.y = di->principal[1];
	d->convergenceDepth = di->convergence_depth;
	d->viewpoint.x = di->viewpoint[0];
	d->viewpoint.y = di->viewpoint[1];
	d->viewpoint.z = di->viewpoint[2];
	d->transformValid = di->transform_valid != 0 ? XR_TRUE : XR_FALSE;
	memcpy(d->depthToDisplay, di->depth_to_display, sizeof(d->depthToDisplay));

	// Handles: first depth acquire and every reallocation (the views' rule).
	if (di->realloc != 0 || !st->depth_exported) {
		bool have_tex = false, have_fence = false;
		xrt_graphics_buffer_handle_t th = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		xrt_graphics_sync_handle_t fh = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		xrt_result_t xret =
		    ipc_client_lift_get_depth_output(route->c, st->id, &have_tex, NULL, NULL, NULL, &th);
		if (xret != XRT_SUCCESS) {
			*out_xret = xret;
			return lift_xret(log, sess, route, xret, "xrAcquireLiftResultDXR (depth export)");
		}
		xret = ipc_client_lift_get_fence(route->c, st->id, &have_fence, &fh);
		if (xret != XRT_SUCCESS) {
			*out_xret = xret;
			return lift_xret(log, sess, route, xret, "xrAcquireLiftResultDXR (depth fence export)");
		}
		const bool got_tex = have_tex && th != XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		const bool got_fence = have_fence && fh != XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		if (got_tex) {
			d->depthTexture = (void *)(intptr_t)th;
		}
		if (got_fence) {
			d->fence = (void *)(intptr_t)fh;
		}
		st->depth_exported = got_tex && got_fence;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireLiftResultDXR(XrLiftStreamDXR stream, XrLiftResultDXR *result)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrAcquireLiftResultDXR");
	struct oxr_session *sess = st->sess;
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, result, XR_TYPE_LIFT_RESULT_DXR);

	if (st->mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrAcquireLiftResultDXR: a GAUSSIANS stream returns a blob (xrAcquireLiftBlobDXR)");
	}

	result->outputTexture = NULL;
	result->fence = NULL;

	struct lift_route route;
	XrResult xr = lift_stream_route_open(&log, st, &route, "xrAcquireLiftResultDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}

	bool ready = false;
	struct xrt_lift_result r;
	xrt_result_t xret = ipc_client_lift_acquire(route.c, st->id, &ready, &r);
	if (xret != XRT_SUCCESS || !ready) {
		lift_route_close(sess, &route, xret);
		xr = lift_xret(&log, sess, &route, xret, "xrAcquireLiftResultDXR");
		return xr != XR_SUCCESS ? xr : XR_LIFT_NOT_READY_DXR;
	}

	result->frameId = r.frame_id;
	result->sourceTime = (XrTime)r.source_time;
	result->fenceValue = r.fence_value;
	result->extent.width = (int32_t)r.width;
	result->extent.height = (int32_t)r.height;
	result->format = (int64_t)r.format;
	result->viewCount = r.view_count;
	result->latency = (XrDuration)r.latency_ns;

	// Spec v2 (ADR-048): echo the viewpoints this result was synthesized for.
	XrLiftResultViewpointsDXR *echo =
	    OXR_GET_OUTPUT_FROM_CHAIN(result, XR_TYPE_LIFT_RESULT_VIEWPOINTS_DXR, XrLiftResultViewpointsDXR);
	if (echo != NULL) {
		uint32_t n = r.viewpoint_count > XR_LIFT_MAX_VIEWS_DXR ? XR_LIFT_MAX_VIEWS_DXR : r.viewpoint_count;
		echo->viewpointCountOutput = n;
		for (uint32_t i = 0; i < XR_LIFT_MAX_VIEWS_DXR; i++) {
			echo->viewpoints[i].x = i < n ? r.viewpoints[3 * i + 0] : 0.0f;
			echo->viewpoints[i].y = i < n ? r.viewpoints[3 * i + 1] : 0.0f;
			echo->viewpoints[i].z = i < n ? r.viewpoints[3 * i + 2] : 0.0f;
		}
		echo->rectCenter.x = r.rect_center[0];
		echo->rectCenter.y = r.rect_center[1];
		echo->rectCenter.z = r.rect_center[2];
		echo->rectSize.width = r.rect_size[0];
		echo->rectSize.height = r.rect_size[1];
	}

	// Handles: first acquire and every reallocation (the weave output pattern).
	if (r.output_realloc || !st->exported) {
		bool have_tex = false, have_fence = false;
		xrt_graphics_buffer_handle_t th = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		xrt_graphics_sync_handle_t fh = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		xret = ipc_client_lift_get_output(route.c, st->id, &have_tex, NULL, NULL, NULL, &th);
		if (xret != XRT_SUCCESS) {
			lift_route_close(sess, &route, xret);
			return lift_xret(&log, sess, &route, xret, "xrAcquireLiftResultDXR (output export)");
		}
		xret = ipc_client_lift_get_fence(route.c, st->id, &have_fence, &fh);
		if (xret != XRT_SUCCESS) {
			lift_route_close(sess, &route, xret);
			return lift_xret(&log, sess, &route, xret, "xrAcquireLiftResultDXR (fence export)");
		}
		const bool got_tex = have_tex && th != XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		const bool got_fence = have_fence && fh != XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		if (got_tex) {
			result->outputTexture = (void *)(intptr_t)th;
		}
		if (got_fence) {
			result->fence = (void *)(intptr_t)fh;
		}
		// Latch only on a complete export (the #1427 rule): a miss retries.
		st->exported = got_tex && got_fence;
	}

	// Spec v3 (ADR-048 Addendum A): the auxiliary depth of this result.
	XrLiftDepthResultDXR *dres =
	    OXR_GET_OUTPUT_FROM_CHAIN(result, XR_TYPE_LIFT_DEPTH_RESULT_DXR, XrLiftDepthResultDXR);
	if (dres != NULL) {
		xret = XRT_SUCCESS;
		xr = lift_fill_depth_result(&log, sess, st, &route, &r, dres, &xret);
		if (xr != XR_SUCCESS) {
			lift_route_close(sess, &route, xret);
			return xr;
		}
	}
	lift_route_close(sess, &route, XRT_SUCCESS);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireLiftBlobDXR(XrLiftStreamDXR stream, XrLiftBlobDXR *blob)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrAcquireLiftBlobDXR");
	struct oxr_session *sess = st->sess;
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, blob, XR_TYPE_LIFT_BLOB_DXR);

	if (st->mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrAcquireLiftBlobDXR: only a GAUSSIANS stream returns a blob");
	}
	if (blob->byteCapacityInput > 0 && blob->bytes == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "XrLiftBlobDXR::bytes is NULL with a capacity");
	}

	struct lift_route route;
	XrResult xr = lift_stream_route_open(&log, st, &route, "xrAcquireLiftBlobDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}
	bool ready = false, delivered = false;
	struct xrt_lift_blob_info info;
	xrt_result_t xret = ipc_client_lift_acquire_blob(route.c, st->id, blob->byteCapacityInput, blob->bytes, &ready,
	                                                 &delivered, &info);
	lift_route_close(sess, &route, xret);
	xr = lift_xret(&log, sess, &route, xret, "xrAcquireLiftBlobDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}
	if (!ready) {
		blob->byteCountOutput = 0;
		return XR_LIFT_NOT_READY_DXR;
	}
	if (info.byte_count > UINT32_MAX) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "xrAcquireLiftBlobDXR: blob exceeds 4 GiB");
	}
	blob->frameId = info.frame_id;
	blob->sourceTime = (XrTime)info.source_time;
	blob->format = (XrLiftBlobFormatDXR)info.format;
	blob->byteCountOutput = (uint32_t)info.byte_count;
	if (blob->byteCapacityInput == 0) {
		return XR_SUCCESS; // size query; the service holds the blob latched
	}
	if (!delivered) {
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "xrAcquireLiftBlobDXR: capacity %u < %u",
		                 blob->byteCapacityInput, blob->byteCountOutput);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLiftStreamPriorityDXR(XrLiftStreamDXR stream, XrLiftPriorityDXR priority)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrSetLiftStreamPriorityDXR");
	struct oxr_session *sess = st->sess;
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	if ((int)priority < (int)XR_LIFT_PRIORITY_PAUSED_DXR || (int)priority > (int)XR_LIFT_PRIORITY_HIGH_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "xrSetLiftStreamPriorityDXR: priority (%d) invalid",
		                 (int)priority);
	}
	struct lift_route route;
	XrResult xr = lift_stream_route_open(&log, st, &route, "xrSetLiftStreamPriorityDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}
	xrt_result_t xret = ipc_client_lift_set_priority(route.c, st->id, (uint32_t)priority);
	lift_route_close(sess, &route, xret);
	return lift_xret(&log, sess, &route, xret, "xrSetLiftStreamPriorityDXR");
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetLiftStreamStatsDXR(XrLiftStreamDXR stream, XrLiftStreamStatsDXR *stats)
{
	OXR_TRACE_MARKER();

	struct oxr_lift_stream_dxr *st = NULL;
	struct oxr_logger log;
	OXR_VERIFY_LIFT_STREAM_AND_INIT_LOG(&log, stream, st, "xrGetLiftStreamStatsDXR");
	struct oxr_session *sess = st->sess;
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, stats, XR_TYPE_LIFT_STREAM_STATS_DXR);

	struct lift_route route;
	XrResult xr = lift_stream_route_open(&log, st, &route, "xrGetLiftStreamStatsDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}
	struct xrt_lift_stream_stats s;
	xrt_result_t xret = ipc_client_lift_stats(route.c, st->id, &s);
	lift_route_close(sess, &route, xret);
	xr = lift_xret(&log, sess, &route, xret, "xrGetLiftStreamStatsDXR");
	if (xr != XR_SUCCESS) {
		return xr;
	}
	stats->priority = (XrLiftPriorityDXR)s.priority;
	stats->framesSubmitted = s.submitted;
	stats->framesConverted = s.converted;
	stats->framesDropped = s.dropped;
	stats->framesFailed = s.failed;
	stats->latencyLast = (XrDuration)s.latency_last_ns;
	stats->latencyAverage = (XrDuration)s.latency_avg_ns;
	stats->latencyMin = (XrDuration)s.latency_min_ns;
	stats->latencyMax = (XrDuration)s.latency_max_ns;
	stats->conversionRate = s.rate_hz;
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_lift
