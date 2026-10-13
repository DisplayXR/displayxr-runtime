// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift API entry points — 2D→3D conversion streams (ADR-042),
 *         spec 4 tasks / readiness / events (ADR-053; was oxr_lift.c).
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
 * Any other in-process session reports XR_ERROR_FEATURE_UNSUPPORTED from the
 * spec-3 calls; spec 4 enumerates the service's tasks to it with imageKinds /
 * exportKinds 0 (known, not reachable from that session yet). What depends on
 * the graphics API or Win32 lives in oxr_conv_gpu_d3d.c.
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
#include "oxr_lift_rules.h"
#include "oxr_conv_gpu.h"
#include "oxr_conv_map.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_lift.h"

// st_oxr carries the ipc include path (oxr_workspace.c); these two headers
// need nothing generated.
#include "client/ipc_client_lift.h"
#include "client/ipc_client_lift_link.h"
#include "xrt/xrt_conv_wire.h"

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

void
oxr_lift_session_hint_interest(struct oxr_session *sess)
{
	if (sess == NULL || sess->sys == NULL || sess->sys->inst == NULL || !sess->sys->inst->extensions.DXR_lift) {
		return;
	}
	if (lift_session_is_ipc(sess)) {
		struct ipc_connection *c = comp_ipc_client_compositor_lift_connection(&sess->xcn->base);
		if (c != NULL) {
			(void)ipc_client_conv_hint_interest(c, 0);
		}
		return;
	}
	if (!oxr_conv_inprocess_eligible(sess)) {
		return;
	}
	// Start the background connect now; it sends the hint once connected.
	struct ipc_client_lift_link *link = oxr_conv_link_get(sess);
	if (link == NULL) {
		return;
	}
	enum ipc_client_lift_link_state state = IPC_CLIENT_LIFT_LINK_CONNECTING;
	uint32_t gen = 0;
	struct ipc_connection *c = ipc_client_lift_link_acquire(link, &state, &gen);
	if (c != NULL) {
		ipc_client_lift_link_release(link, gen, false);
	}
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
	if (!oxr_conv_inprocess_eligible(sess)) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "%s: in-process 2D→3D conversion is available to D3D11 / D3D12 sessions on Windows "
		                 "only (other sessions: the out-of-process service path)",
		                 what);
	}
	struct ipc_client_lift_link *link = oxr_conv_link_get(sess);
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
lift_validate_options(
    struct oxr_logger *log, const XrLiftOptionsDXR *o, uint32_t mode, uint32_t module_max_views, bool weave_path)
{
	// #2005 G2: NVIEW is bounded by the module's maxViews, not only the
	// extension's constant ceiling.
	if (!oxr_lift_view_count_ok(mode, o->viewCount, module_max_views)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftOptionsDXR::viewCount (%u) > %u (XrLiftPropertiesDXR::maxViews for this %s stream)",
		                 o->viewCount,
		                 mode == XRT_DP_LIFT_MODE_NVIEW ? oxr_lift_view_bound(module_max_views)
		                                                : (uint32_t)XR_LIFT_MAX_VIEWS_DXR,
		                 mode == XRT_DP_LIFT_MODE_NVIEW ? "NVIEW" : "non-NVIEW");
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
	oxr_conv_cpu_unmap(st->cpu_view);
	st->cpu_view = NULL;
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
 * Spec 4 (ADR-053 P3, #2054): tasks, readiness events, task-typed streams,
 * neutral input images, result contracts.
 *
 */

//! Map a conv_* family result; INPUT_UNSUPPORTED is this family's validation failure.
static XrResult
conv_xret(struct oxr_logger *log,
          struct oxr_session *sess,
          const struct lift_route *route,
          xrt_result_t xret,
          const char *what)
{
	if (xret == XRT_ERROR_INPUT_UNSUPPORTED) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "%s: the request does not fit this task (view count, input kind, export kind or mode)",
		                 what);
	}
	return lift_xret(log, sess, route, xret, what);
}

/*!
 * The route of a spec-4 QUERY (enumerate, warm, events). Like lift_route_open,
 * except that an in-process session the spec-3 calls refuse (GL, Vulkan,
 * Metal) still reads the service's tasks over a lift-only link: they are
 * known, with imageKinds / exportKinds 0 (not reachable from this session
 * yet). Without a link (no service transport on this platform) route->c is
 * NULL: nothing is known.
 */
static XrResult
conv_query_route_open(struct oxr_logger *log, struct oxr_session *sess, struct lift_route *route, const char *what)
{
	if (lift_session_is_ipc(sess) || oxr_conv_inprocess_eligible(sess)) {
		return lift_route_open(log, sess, 0, route, what);
	}
	memset(route, 0, sizeof(*route));
	struct ipc_client_lift_link *link = oxr_conv_link_get(sess);
	if (link == NULL) {
		route->state = IPC_CLIENT_LIFT_LINK_FAILED;
		return XR_SUCCESS;
	}
	route->inprocess = true;
	route->c = ipc_client_lift_link_acquire_wait(link, 0, &route->state, &route->gen);
	return XR_SUCCESS;
}

//! The first spec-4 call opts the session into the task / provider events.
static void
conv_latch(struct oxr_session *sess)
{
	sess->lift_v4 = true;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateLiftTasksDXR(XrSession session,
                            uint32_t capacityInput,
                            uint32_t *countOutput,
                            XrLiftTaskPropertiesDXR *tasks)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrEnumerateLiftTasksDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_lift);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);
	if (capacityInput > 0 && tasks == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "xrEnumerateLiftTasksDXR: tasks is NULL");
	}
	for (uint32_t i = 0; i < capacityInput; i++) {
		if (tasks[i].type != XR_TYPE_LIFT_TASK_PROPERTIES_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrEnumerateLiftTasksDXR: tasks[%u].type is not XR_TYPE_LIFT_TASK_PROPERTIES_DXR", i);
		}
	}
	*countOutput = 0;

	struct lift_route route;
	XrResult r = conv_query_route_open(&log, sess, &route, "xrEnumerateLiftTasksDXR");
	if (r != XR_SUCCESS) {
		return r;
	}
	conv_latch(sess);
	if (route.c == NULL) {
		return XR_SUCCESS; // in-process, no connection yet: nothing known; poll again
	}

	uint64_t in_kinds = 0, out_kinds = 0;
	oxr_conv_session_image_kinds(sess, &in_kinds, &out_kinds);

	struct xrt_conv_wire_task_desc found[XRT_CONV_TASK_RESERVED_FIRST];
	uint64_t found_in[XRT_CONV_TASK_RESERVED_FIRST], found_out[XRT_CONV_TASK_RESERVED_FIRST];
	uint32_t n = 0;
	xrt_result_t xret = XRT_SUCCESS;
	for (uint32_t t = XRT_CONV_TASK_DEPTH_MONO; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
		if (!oxr_conv_task_is_spec4((XrLiftTaskDXR)t)) {
			continue;
		}
		struct xrt_conv_wire_task_desc d;
		uint64_t ik = 0, ek = 0;
		xret = ipc_client_conv_enum_task(route.c, t, &d, &ik, &ek);
		if (xret != XRT_SUCCESS) {
			break;
		}
		if (d.state == XRT_CONV_STATE_ABSENT || d.task != t) {
			continue;
		}
		found[n] = d;
		found_in[n] = ik & in_kinds;
		found_out[n] = ek & out_kinds;
		n++;
		if (t == XRT_CONV_TASK_VIEWS_MONO && d.max_views > 0) {
			sess->lift_max_views = d.max_views > XR_LIFT_MAX_VIEWS_DXR ? XR_LIFT_MAX_VIEWS_DXR : d.max_views;
		}
	}
	lift_route_close(sess, &route, xret);
	r = lift_xret(&log, sess, &route, xret, "xrEnumerateLiftTasksDXR");
	if (r != XR_SUCCESS) {
		return r;
	}

	*countOutput = n;
	if (capacityInput == 0) {
		return XR_SUCCESS;
	}
	if (capacityInput < n) {
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "xrEnumerateLiftTasksDXR: capacity %u < %u",
		                 capacityInput, n);
	}
	for (uint32_t i = 0; i < n; i++) {
		oxr_conv_task_properties_from_wire(&found[i], found_in[i], found_out[i], &tasks[i]);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestLiftTaskWarmDXR(XrSession session, XrLiftTaskDXR task)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRequestLiftTaskWarmDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_lift);
	if (!oxr_conv_task_is_spec4(task)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrRequestLiftTaskWarmDXR: task %d is reserved or not a spec-4 task", (int)task);
	}
	struct lift_route route;
	XrResult r = conv_query_route_open(&log, sess, &route, "xrRequestLiftTaskWarmDXR");
	if (r != XR_SUCCESS) {
		return r;
	}
	conv_latch(sess);
	if (route.c == NULL) {
		return XR_SUCCESS; // in-process, connecting: the connect's own hint warms the defaults
	}
	xrt_result_t xret = ipc_client_conv_warm(route.c, (uint32_t)task);
	lift_route_close(sess, &route, xret);
	if (xret == XRT_ERROR_FEATURE_NOT_SUPPORTED) {
		return XR_SUCCESS; // nothing to warm (no candidate): the enumeration says so
	}
	return conv_xret(&log, sess, &route, xret, "xrRequestLiftTaskWarmDXR");
}

/*!
 * xrCreateLiftStreamDXR with XrLiftTaskStreamCreateInfoDXR chained (spec 4).
 * The base struct's contentHint / inputScale keep their meaning; its mode is
 * 0 or the legacy mode of the task.
 */
static XrResult
lift_create_task_stream(struct oxr_logger *log,
                        struct oxr_session *sess,
                        const XrLiftStreamCreateInfoDXR *ci,
                        const XrLiftTaskStreamCreateInfoDXR *t,
                        XrLiftStreamDXR *stream)
{
	const char *what = "xrCreateLiftStreamDXR (task)";
	if (!oxr_conv_task_is_spec4(t->task)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: task %d is reserved or not a spec-4 task", what,
		                 (int)t->task);
	}
	if (ci->mode != 0 && !oxr_conv_mode_matches_task((uint32_t)ci->mode, t->task)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: mode %d does not match task %d", what,
		                 (int)ci->mode, (int)t->task);
	}
	if (t->inputKind != 0 && t->inputKind != XR_LIFT_INPUT_KIND_MONO_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: input kind %d is not offered (spec 4: MONO)",
		                 what, (int)t->inputKind);
	}
	if ((int)t->qualityHint < 0 || (int)t->qualityHint > (int)XR_LIFT_QUALITY_HINT_QUALITY_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: qualityHint %d invalid", what,
		                 (int)t->qualityHint);
	}
	if (t->viewCount > XR_LIFT_MAX_VIEWS_DXR || t->viewCount == 1) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: viewCount %u (0 or 2..%u)", what, t->viewCount,
		                 (uint32_t)XR_LIFT_MAX_VIEWS_DXR);
	}
	uint64_t in_kinds = 0, out_kinds = 0;
	oxr_conv_session_image_kinds(sess, &in_kinds, &out_kinds);
	uint32_t export_memory = 0;
	if (t->exportKind != XR_LIFT_IMAGE_KIND_PLATFORM_DEFAULT_DXR) {
		if ((int)t->exportKind < 0 || (int)t->exportKind >= 64 ||
		    (out_kinds & (1ull << (uint32_t)t->exportKind)) == 0) {
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: exportKind %d is not offered here", what,
			                 (int)t->exportKind);
		}
		export_memory = t->exportKind == XR_LIFT_IMAGE_KIND_CPU_DXR ? XRT_CONV_MEM_CPU : XRT_CONV_MEM_WIN32_NT_HANDLE;
	}
	if (ci->contentHint != XR_LIFT_CONTENT_HINT_VIDEO_DXR && ci->contentHint != XR_LIFT_CONTENT_HINT_PHOTO_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: contentHint (%d) invalid", what,
		                 (int)ci->contentHint);
	}
	if (ci->inputScale < 0.0f || ci->inputScale > 1.0f) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: inputScale (%f) must be in [0, 1]", what,
		                 (double)ci->inputScale);
	}
	const bool views = t->task == XR_LIFT_TASK_VIEWS_MONO_DXR;
	// XrLiftDepthRequestDXR beside it: aux depth, advisory (VIEWS_MONO only).
	const XrLiftDepthRequestDXR *dreq =
	    OXR_GET_INPUT_FROM_CHAIN(ci, XR_TYPE_LIFT_DEPTH_REQUEST_DXR, XrLiftDepthRequestDXR);
	if (dreq != NULL && !views) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: XrLiftDepthRequestDXR is for VIEWS_MONO only",
		                 what);
	}
	if ((t->requiredOutputs & XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR) != 0 && !views) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: AUX_DEPTH is for VIEWS_MONO only", what);
	}

	struct xrt_conv_wire_stream_desc sd;
	memset(&sd, 0, sizeof(sd));
	sd.struct_size = (uint32_t)sizeof(sd);
	sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	sd.task = (uint32_t)t->task;
	sd.quality_hint = (uint32_t)t->qualityHint;
	sd.export_memory_kind = export_memory;
	sd.input_kind = XRT_CONV_INPUT_MONO;
	sd.content_hint = (uint32_t)ci->contentHint;
	sd.view_count = t->viewCount;
	sd.views_layout = XRT_CONV_VIEWS_ROW;
	if (dreq != NULL) {
		sd.outputs |= XRT_CONV_OUT_DEPTH; // advisory: the service adds it only when declared
	}

	struct lift_route route;
	XrResult r = lift_route_open(log, sess, 6000, &route, what);
	if (r != XR_SUCCESS) {
		return r;
	}
	conv_latch(sess);
	if (route.c == NULL) {
		if (route.state == IPC_CLIENT_LIFT_LINK_CONNECTING) {
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
			                 "%s: the lift service connection is still being made - retry", what);
		}
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: no service to convert with", what);
	}
	struct xrt_conv_wire_stream_created created;
	memset(&created, 0, sizeof(created));
	xrt_result_t xret = ipc_client_conv_stream_create(route.c, &sd, (uint64_t)t->requiredOutputs,
	                                                (uint32_t)ci->mode,
	                                                ci->inputScale > 0.0f ? ci->inputScale : 1.0f, &created);
	const uint32_t gen = route.gen;
	lift_route_close(sess, &route, xret);
	r = conv_xret(log, sess, &route, xret, what);
	if (r != XR_SUCCESS) {
		return r;
	}

	struct oxr_lift_stream_dxr *st = NULL;
	OXR_ALLOCATE_HANDLE_OR_RETURN(log, st, OXR_XR_DEBUG_LIFTSTREAM, oxr_lift_stream_destroy_cb, &sess->handle);
	st->sess = sess;
	st->id = created.stream_id;
	st->mode = !views ? XRT_DP_LIFT_MODE_DEPTH
	                  : (created.view_count > 2 ? XRT_DP_LIFT_MODE_NVIEW : XRT_DP_LIFT_MODE_SBS);
	st->depth_requested = views && (dreq != NULL || (t->requiredOutputs & XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR) != 0);
	st->link_gen = gen;
	st->max_views = sess->lift_max_views;
	st->v4 = true;
	st->task = (uint32_t)t->task;
	st->export_cpu = export_memory == XRT_CONV_MEM_CPU;
	*stream = XRT_CAST_PTR_TO_OXR_HANDLE(XrLiftStreamDXR, st);
	return XR_SUCCESS;
}

//! One resolved spec-4 input: either the v3 submit (D3D11 NT / DXGI at the
//! origin, keyed mutex) or a conv_submit_frame descriptor.
struct lift_input
{
	bool v4;
	void *handle;
	bool is_dxgi;
	XrExtent2Di extent;
	struct xrt_conv_wire_image image; //!< v4: kind, sync, CPU pitch / format
	void *sync_handle;                //!< v4 D3D_FENCE: the caller's fence (borrowed)
	uint32_t region_x, region_y;      //!< v4
	bool owns_handle;                 //!< v4 CPU: a mapping this call made (closed after the submit)
};

static uint32_t
lift_conv_fmt(XrLiftPixelFormatDXR f)
{
	switch (f) {
	case XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR:
	case XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR:
	case XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR:
	case XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR: return (uint32_t)f; // values equal the ABI's
	default: return 0;
	}
}

/*!
 * XrLiftFrameImagesDXR (spec 4) -> the submit. One COLOR image of a kind the
 * session's imageKinds offer, any region inside it: D3D11 NT / legacy DXGI
 * (keyed mutex, or a D3D fence), D3D12 NT (a D3D fence, or writes complete
 * before the call), CPU bytes (copied into a mapping before return).
 */
static XrResult
lift_frame_images_resolve(struct oxr_logger *log,
                          struct oxr_session *sess,
                          const XrLiftFrameImagesDXR *fi,
                          struct lift_input *out)
{
	memset(out, 0, sizeof(*out));
	if (fi->imageCount != 1 || fi->images == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftFrameImagesDXR: imageCount %u (a spec-4 runtime takes one COLOR image)",
		                 fi->imageCount);
	}
	const XrLiftImageDXR *im = &fi->images[0];
	if (im->type != XR_TYPE_LIFT_IMAGE_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftFrameImagesDXR: images[0].type invalid");
	}
	if (im->role != XR_LIFT_IMAGE_ROLE_COLOR_DXR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftImageDXR: role %d (spec 4: COLOR)", (int)im->role);
	}
	uint64_t in_kinds = 0, out_kinds = 0;
	oxr_conv_session_image_kinds(sess, &in_kinds, &out_kinds);
	XrLiftImageKindDXR kind = im->kind;
	if (kind == XR_LIFT_IMAGE_KIND_PLATFORM_DEFAULT_DXR) {
		kind = XR_LIFT_IMAGE_KIND_D3D11_NT_DXR;
	}
	if ((int)kind < 0 || (int)kind >= 64 || (in_kinds & (1ull << (uint32_t)kind)) == 0) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftImageDXR: kind %d is not offered here",
		                 (int)im->kind);
	}
	if (im->region.offset.x < 0 || im->region.offset.y < 0 || im->region.extent.width <= 0 ||
	    im->region.extent.height <= 0) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftImageDXR::region (%d,%d %dx%d) invalid",
		                 im->region.offset.x, im->region.offset.y, im->region.extent.width,
		                 im->region.extent.height);
	}
	const uint32_t rx = (uint32_t)im->region.offset.x, ry = (uint32_t)im->region.offset.y;
	const uint32_t rw = (uint32_t)im->region.extent.width, rh = (uint32_t)im->region.extent.height;
	out->extent = im->region.extent;
	out->region_x = rx;
	out->region_y = ry;
	struct xrt_conv_wire_image *w = &out->image;
	w->role = XRT_CONV_IMG_COLOR;
	w->width = rw;
	w->height = rh;
	w->format = lift_conv_fmt(im->format);

	if (kind == XR_LIFT_IMAGE_KIND_CPU_DXR) {
		if (im->cpuBytes == NULL || w->format == 0 || im->cpuRowPitch < (rx + rw) * 4u ||
		    (im->syncKind != XR_LIFT_SYNC_KIND_NONE_DXR && im->syncKind != XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR)) {
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrLiftImageDXR: a CPU image needs cpuBytes, an RGBA8 / BGRA8 format, cpuRowPitch >= "
			                 "(region x + width) * 4 and syncKind NONE / CPU_COMPLETE");
		}
		// The bytes are copied before return (the region only, tightly packed).
		if (!oxr_conv_cpu_upload(im->cpuBytes, im->cpuRowPitch, rx, ry, rw, rh, &out->handle)) {
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "XrLiftImageDXR: could not stage the CPU image");
		}
		out->owns_handle = true;
		out->v4 = true;
		out->region_x = 0;
		out->region_y = 0;
		w->memory_kind = XRT_CONV_MEM_CPU;
		w->row_stride = rw * 4u;
		w->byte_size = (uint64_t)rw * rh * 4u;
		w->sync_kind = XRT_CONV_SYNC_NONE;
		return XR_SUCCESS;
	}

	if (im->handle == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "XrLiftImageDXR::handle is NULL");
	}
	const bool d3d12 = kind == XR_LIFT_IMAGE_KIND_D3D12_NT_DXR;
	const bool fence = im->syncKind == XR_LIFT_SYNC_KIND_D3D_FENCE_DXR;
	const bool sync_ok =
	    fence || im->syncKind == XR_LIFT_SYNC_KIND_NONE_DXR ||
	    (d3d12 ? im->syncKind == XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR : im->syncKind == XR_LIFT_SYNC_KIND_KEYED_MUTEX_DXR);
	if (!sync_ok || (fence && im->syncHandle == NULL)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftImageDXR: syncKind %d does not fit kind %d (D3D11: keyed mutex or D3D fence; D3D12: "
		                 "D3D fence or CPU_COMPLETE)",
		                 (int)im->syncKind, (int)kind);
	}
	out->handle = im->handle;
	out->is_dxgi = kind == XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR;
	// The v3 submit when it can say it: a D3D11 texture at the origin, keyed mutex.
	out->v4 = d3d12 || fence || rx != 0 || ry != 0;
	if (out->v4) {
		w->memory_kind = out->is_dxgi ? XRT_CONV_MEM_WIN32_KMT_HANDLE : XRT_CONV_MEM_WIN32_NT_HANDLE;
		w->sync_kind = fence ? XRT_CONV_SYNC_FENCE_NT : XRT_CONV_SYNC_NONE;
		w->sync_acquire_value = fence ? im->syncValue : 0;
		out->sync_handle = fence ? im->syncHandle : NULL;
	}
	return XR_SUCCESS;
}

/*
 *
 * Events (opt-in per session: only after a spec-4 call).
 *
 */

//! xrPollEvent: drain each opted-in session's conversion events (throttled).
XrResult
oxr_conv_poll_events(struct oxr_logger *log, struct oxr_instance *inst)
{
	if (!inst->extensions.DXR_lift) {
		return XR_SUCCESS;
	}
	const uint64_t now = os_monotonic_get_ns();
	os_mutex_lock(&inst->sessions_mutex);
	for (struct oxr_session *sess = inst->sessions; sess != NULL; sess = sess->next) {
		if (!sess->lift_v4 || sess->has_lost) {
			continue;
		}
		// Events are rate-limited at the source (2 Hz progress); 20 ms bounds
		// the round trips of an app that polls in a tight loop.
		if (now - sess->lift_ev_poll_ns < OXR_CONV_EVENT_POLL_NS) {
			continue;
		}
		sess->lift_ev_poll_ns = now;
		struct lift_route route;
		if (conv_query_route_open(log, sess, &route, "xrPollEvent (lift)") != XR_SUCCESS) {
			continue;
		}
		xrt_result_t xret = XRT_SUCCESS;
		for (int i = 0; route.c != NULL && i < 16; i++) {
			bool has = false;
			struct xrt_conv_wire_event ev;
			xret = ipc_client_conv_poll_event(route.c, &has, &ev);
			if (xret != XRT_SUCCESS || !has) {
				break;
			}
			if (ev.kind == XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED) {
				oxr_event_push_XrEventDataLiftProvidersChangedDXR(log, sess);
			} else if (ev.kind == XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED) {
				oxr_event_push_XrEventDataLiftTaskStateChangedDXR(
				    log, sess, (XrLiftTaskDXR)ev.task, (XrLiftTaskStateDXR)ev.state,
				    (XrLiftTaskStateDXR)ev.previous_state, (XrLiftFailureReasonDXR)ev.failure_reason, ev.progress);
			}
		}
		// A dead pipe surfaces on the session's next real call, not here.
		lift_route_close(sess, &route, xret);
	}
	os_mutex_unlock(&inst->sessions_mutex);
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
		// #2005 G2: remember the module's view bound for validation.
		if (caps.max_views > 0) {
			sess->lift_max_views = caps.max_views;
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

	// Spec 4: a task-typed stream (its mode is 0 or the task's legacy mode).
	const XrLiftTaskStreamCreateInfoDXR *task_ci = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR, XrLiftTaskStreamCreateInfoDXR);
	if (task_ci != NULL) {
		return lift_create_task_stream(&log, sess, createInfo, task_ci, stream);
	}

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
	st->max_views = sess->lift_max_views; // #2005 G2
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

	// Spec 4: XrLiftFrameImagesDXR replaces inputTexture / inputIsDxgi / extent.
	struct lift_input li;
	memset(&li, 0, sizeof(li));
	li.handle = submitInfo->inputTexture;
	li.is_dxgi = submitInfo->inputIsDxgi == XR_TRUE;
	li.extent = submitInfo->extent;
	const XrLiftFrameImagesDXR *frame_images =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_LIFT_FRAME_IMAGES_DXR, XrLiftFrameImagesDXR);
	if (frame_images != NULL) {
		XrResult ir = lift_frame_images_resolve(&log, sess, frame_images, &li);
		if (ir != XR_SUCCESS) {
			return ir;
		}
	}
	void *in_texture = li.handle;
	const XrExtent2Di in_extent = li.extent;
	const XrLiftFrameControlDXR *frame_control =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_LIFT_FRAME_CONTROL_DXR, XrLiftFrameControlDXR);
	const bool discontinuity =
	    frame_control != NULL && (frame_control->flags & XR_LIFT_FRAME_CONTROL_DISCONTINUITY_BIT_DXR) != 0;

	if (in_texture == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "XrLiftFrameSubmitInfoDXR::inputTexture is NULL");
	}
	// Every return below this point goes through the submit (which closes a
	// CPU staging mapping), except the validation of the options: do it here.
	if (in_extent.width <= 0 || in_extent.height <= 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrLiftFrameSubmitInfoDXR::extent (%dx%d) must be positive", in_extent.width,
		                 in_extent.height);
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
		// #2005 G2: a module that became READY after this stream was created
		// has told the session its real bound since.
		if (sess->lift_max_views > 0) {
			st->max_views = sess->lift_max_views;
		}
		XrResult vr = lift_validate_options(&log, opt, st->mode, st->max_views, /*weave_path*/ false);
		if (vr != XR_SUCCESS) {
			if (li.owns_handle) {
				oxr_conv_close_handle(li.handle);
			}
			return vr;
		}
		lift_params_from_xr(opt, st->mode, &params);
		pp = &params;
		const XrLiftViewControlDXR *v =
		    OXR_GET_INPUT_FROM_CHAIN(opt, XR_TYPE_LIFT_VIEW_CONTROL_DXR, XrLiftViewControlDXR);
		if (v != NULL) {
			vr = oxr_lift_view_control_from_xr(&log, v, &vc);
			if (vr != XR_SUCCESS) {
				if (li.owns_handle) {
					oxr_conv_close_handle(li.handle);
				}
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
		if (li.owns_handle) {
			oxr_conv_close_handle(li.handle);
		}
		return rr;
	}
	xrt_result_t xret = XRT_SUCCESS;
	if (discontinuity) {
		// Spec 4: drop temporal state before this frame (seek / cut / switch).
		xret = ipc_client_conv_stream_reset(route.c, st->id);
		if (xret == XRT_ERROR_IPC_FAILURE) {
			if (li.owns_handle) {
				oxr_conv_close_handle(li.handle);
			}
			lift_route_close(sess, &route, xret);
			return lift_xret(&log, sess, &route, xret, "xrSubmitLiftFrameDXR (discontinuity)");
		}
	}
	if (li.v4) {
		xret = ipc_client_conv_submit_frame(
		    route.c, st->id, &li.image, (xrt_graphics_buffer_handle_t)(intptr_t)in_texture,
		    (xrt_graphics_buffer_handle_t)(intptr_t)li.sync_handle, li.region_x, li.region_y,
		    (uint32_t)in_extent.width, (uint32_t)in_extent.height, (int64_t)submitInfo->sourceTime, pp,
		    vp_count > 0 ? vps : NULL, vp_count, vcp, rigp, frameId);
		if (li.owns_handle) {
			oxr_conv_close_handle(li.handle); // the transport duplicated it into the service
		}
	} else {
		xret = ipc_client_lift_submit(route.c, st->id, (xrt_graphics_buffer_handle_t)(intptr_t)in_texture,
		                              li.is_dxgi, (uint32_t)in_extent.width, (uint32_t)in_extent.height,
		                              (int64_t)submitInfo->sourceTime, pp, vp_count > 0 ? vps : NULL, vp_count,
		                              vcp, rigp, frameId);
	}
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

//! Fill the spec-4 result contracts that are chained (type / next kept).
static void
lift_fill_v4_info(const struct xrt_conv_wire_result *r4,
                  bool discontinuity,
                  XrLiftDepthInfoDXR *depth_info,
                  XrLiftViewsInfoDXR *views_info)
{
	if (depth_info != NULL) {
		XrStructureType type = depth_info->type;
		void *next = depth_info->next;
		memset(depth_info, 0, sizeof(*depth_info));
		depth_info->type = type;
		depth_info->next = next;
		if ((r4->written & XRT_CONV_OUT_DEPTH) != 0) {
			oxr_conv_depth_info_from_wire(&r4->depth_info, discontinuity, depth_info);
		} else {
			depth_info->discontinuity = discontinuity ? XR_TRUE : XR_FALSE;
		}
	}
	if (views_info != NULL) {
		XrStructureType type = views_info->type;
		void *next = views_info->next;
		memset(views_info, 0, sizeof(*views_info));
		views_info->type = type;
		views_info->next = next;
		if ((r4->written & XRT_CONV_OUT_VIEWS) != 0) {
			oxr_conv_views_info_from_wire(&r4->views_info, r4->views_width, r4->views_height,
			                              (r4->written & XRT_CONV_OUT_DEPTH) != 0 ? &r4->depth_info : NULL,
			                              views_info);
		}
	}
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

	XrLiftResultExportDXR *export_info =
	    OXR_GET_OUTPUT_FROM_CHAIN(result, XR_TYPE_LIFT_RESULT_EXPORT_DXR, XrLiftResultExportDXR);
	XrLiftDepthInfoDXR *depth_info =
	    OXR_GET_OUTPUT_FROM_CHAIN(result, XR_TYPE_LIFT_DEPTH_INFO_DXR, XrLiftDepthInfoDXR);
	XrLiftViewsInfoDXR *views_info =
	    OXR_GET_OUTPUT_FROM_CHAIN(result, XR_TYPE_LIFT_VIEWS_INFO_DXR, XrLiftViewsInfoDXR);
	const bool want_v4 = depth_info != NULL || views_info != NULL || st->export_cpu;

	bool ready = false;
	struct xrt_lift_result r;
	struct xrt_conv_wire_result r4;
	bool discontinuity = false;
	memset(&r4, 0, sizeof(r4));
	xrt_result_t xret = want_v4 ? ipc_client_conv_acquire(route.c, st->id, &ready, &discontinuity, &r, &r4)
	                            : ipc_client_lift_acquire(route.c, st->id, &ready, &r);
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

	if (st->export_cpu) {
		// CPU export: the bytes sit in a mapping the service refreshed for
		// this acquire; map it on the first acquire and on reallocation.
		if (r.output_realloc || st->cpu_view == NULL) {
			bool have = false;
			uint32_t cw = 0, ch = 0, cf = 0;
			uint64_t cb = 0;
			xrt_graphics_buffer_handle_t mh = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
			xret = ipc_client_conv_get_cpu_output(route.c, st->id, &have, &cw, &ch, &cf, &cb, &mh);
			if (xret != XRT_SUCCESS) {
				lift_route_close(sess, &route, xret);
				return lift_xret(&log, sess, &route, xret, "xrAcquireLiftResultDXR (CPU export)");
			}
			oxr_conv_cpu_unmap(st->cpu_view);
			st->cpu_view = have ? oxr_conv_cpu_map((void *)(intptr_t)mh, cb) : NULL;
			oxr_conv_close_handle((void *)(intptr_t)mh); // the view keeps the section alive
			st->cpu_bytes = st->cpu_view != NULL ? cb : 0;
		}
		result->outputTexture = st->cpu_view; // tightly packed rows, valid until the next acquire
		result->fence = NULL;
		const int64_t dxgi = result->format;
		result->format = 0; // XrLiftResultExportDXR is authoritative
		if (export_info != NULL) {
			export_info->kind = XR_LIFT_IMAGE_KIND_CPU_DXR;
			export_info->format = oxr_conv_pixel_format_from_dxgi(dxgi);
			export_info->colorSpace = st->mode == XRT_DP_LIFT_MODE_DEPTH ? XR_LIFT_COLOR_SPACE_UNKNOWN_DXR
			                                                             : XR_LIFT_COLOR_SPACE_SRGB_DXR;
			export_info->syncKind = XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR;
		}
		st->exported = true;
	}

	// Handles: first acquire and every reallocation (the weave output pattern).
	if (!st->export_cpu && (r.output_realloc || !st->exported)) {
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

	// Spec 4: XrLiftResultExportDXR is left untouched on a D3D11_NT stream
	// (filled above for a CPU export); the result means what
	// XrLiftDepthInfoDXR / XrLiftViewsInfoDXR say.
	if (want_v4) {
		lift_fill_v4_info(&r4, discontinuity, depth_info, views_info);
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
