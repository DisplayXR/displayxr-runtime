// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_desktop_control entry points (ADR-056): an app's agent observes
 *         and operates the desktop through the OS's own consent, inside the
 *         app's process.
 *
 * Thin by design: every rule (consent order, kill switch, idle wait, target
 * verification, audit) lives in aux/desktop/u_desktop_control.c, identically
 * on every OS. This file owns the handle plumbing:
 *
 *  - one u_desktop_control per XrSession, created lazily on the first call
 *    and destroyed in session destroy BEFORE the session's events are drained
 *    (oxr_desktop_control_session_fini), so no event outlives the session;
 *  - the object's callbacks push XrEventDataDesktopControlStateChangedDXR /
 *    XrEventDataDesktopActionCompletedDXR (the instance event queue is locked
 *    internally, so the worker thread may push);
 *  - struct conversion and the OpenXR two-call idioms.
 *
 * Never exposed outside the process: no IPC, no MCP (ADR-056 D2/D4). An app in
 * service mode still runs this in its own process — the client library is in
 * the app.
 *
 * Logging: refusals that are normal flow (NOT_GRANTED while the app polls,
 * FEATURE_UNSUPPORTED) are returned without oxr_error() so a frame loop does
 * not spam the log; malformed calls go through oxr_error() as usual.
 *
 * @ingroup oxr_api
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "desktop/u_desktop_control.h"
#include "desktop/u_desktop_policy.h"

#include "util/u_logging.h"
#include "util/u_time.h"

#include <stdlib.h>
#include <string.h>

#ifdef OXR_HAVE_DXR_desktop_control

// The aux layer is OpenXR-free; pin its mirrors to the real values.
_Static_assert(U_DESKTOP_XR_SUCCESS == XR_SUCCESS, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_VALIDATION_FAILURE == XR_ERROR_VALIDATION_FAILURE, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_RUNTIME_FAILURE == XR_ERROR_RUNTIME_FAILURE, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED == XR_ERROR_FEATURE_UNSUPPORTED, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_LIMIT_REACHED == XR_ERROR_LIMIT_REACHED, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_REFUSED == XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_NOT_GRANTED == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR, "result mirror");
_Static_assert(U_DESKTOP_XR_ERROR_WINDOW_LOST == XR_ERROR_DESKTOP_WINDOW_LOST_DXR, "result mirror");
_Static_assert((int)U_DESKTOP_STATE_SUSPENDED == (int)XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR, "state mirror");
_Static_assert((int)U_DESKTOP_STATE_GRANTED == (int)XR_DESKTOP_CONTROL_STATE_GRANTED_DXR, "state mirror");
_Static_assert((int)U_DESKTOP_ACTION_SCROLL == (int)XR_DESKTOP_ACTION_TYPE_SCROLL_DXR, "action mirror");
_Static_assert((int)U_DESKTOP_ACTION_CLOSE == (int)XR_DESKTOP_ACTION_TYPE_CLOSE_DXR, "action mirror");
_Static_assert((int)U_DESKTOP_PICK_OUTSIDE == (int)XR_DESKTOP_PICK_KIND_OUTSIDE_DXR, "pick mirror");
_Static_assert(U_DESKTOP_APP_ID_SIZE == XR_MAX_DESKTOP_APP_ID_SIZE_DXR, "size mirror");
_Static_assert(U_DESKTOP_TITLE_SIZE == XR_MAX_DESKTOP_TITLE_SIZE_DXR, "size mirror");
_Static_assert(U_DESKTOP_XR_ERROR_SIZE_INSUFFICIENT == XR_ERROR_SIZE_INSUFFICIENT, "result mirror");
_Static_assert(U_DESKTOP_CAP_CAPTURE == 0x40 /* XR_DESKTOP_CONTROL_CAPABILITY_CAPTURE_BIT_DXR (a static const) */, "caps mirror");
_Static_assert(U_DESKTOP_SCOPE_CONTROL == 0x1 /* XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR */, "scope mirror");
_Static_assert(U_DESKTOP_SCOPE_CAPTURE == 0x2 /* XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR */, "scope mirror");
_Static_assert(U_DESKTOP_SCOPE_REASK == 0x100 /* XR_DESKTOP_CONTROL_SCOPE_ASK_AGAIN_BIT_DXR */, "scope mirror");
_Static_assert((int)U_DESKTOP_CAPTURE_WINDOW == (int)XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR, "target mirror");
_Static_assert((int)U_DESKTOP_CAPTURE_REGION == (int)XR_DESKTOP_CAPTURE_TARGET_REGION_DXR, "target mirror");

//! Largest text xrGetDesktopTextAtDXR returns (bytes incl. NUL); longer text is cut on a code point.
#define TEXT_AT_MAX 16384


/*
 *
 * Callbacks (any thread).
 *
 */

static void
on_state_changed(void *ctx, enum u_desktop_control_state state)
{
	struct oxr_session *sess = (struct oxr_session *)ctx;
	struct oxr_logger log;
	oxr_log_init(&log, "xrDesktopControl(state)");
	U_LOG_I("desktop_control: state -> %s", u_desktop_control_state_str(state));
	oxr_event_push_XrEventDataDesktopControlStateChanged(&log, sess, (XrDesktopControlStateDXR)state);
}

static void
on_action_completed(void *ctx, uint64_t action_id, int32_t result, const char *message)
{
	struct oxr_session *sess = (struct oxr_session *)ctx;
	struct oxr_logger log;
	oxr_log_init(&log, "xrDesktopControl(action)");
	oxr_event_push_XrEventDataDesktopActionCompleted(&log, sess, action_id, (XrResult)result, message);
}

static void
on_capture_state_changed(void *ctx, enum u_desktop_control_state state)
{
	struct oxr_session *sess = (struct oxr_session *)ctx;
	struct oxr_logger log;
	oxr_log_init(&log, "xrDesktopControl(capture state)");
	U_LOG_I("desktop_control: capture state -> %s", u_desktop_control_state_str(state));
	oxr_event_push_XrEventDataDesktopCaptureStateChanged(&log, sess, (XrDesktopControlStateDXR)state);
}

static void
on_capture_completed(void *ctx, const struct u_desktop_capture_done *d)
{
	struct oxr_session *sess = (struct oxr_session *)ctx;
	struct oxr_logger log;
	oxr_log_init(&log, "xrDesktopControl(capture)");
	XrRect2Di src = {{d->source.x, d->source.y}, {d->source.w, d->source.h}};
	XrTime t = d->frame_ns != 0 ? time_state_monotonic_to_ts_ns(sess->sys->inst->timekeeping, d->frame_ns) : 0;
	oxr_event_push_XrEventDataDesktopCaptureCompleted(&log, sess, d->capture_id, (XrResult)d->result, d->w, d->h,
	                                                  src, t, d->message);
}


/*
 *
 * Per-session object.
 *
 */

//! The session's object, created on first use. NULL only on allocation failure.
static struct u_desktop_control *
get_dc(struct oxr_session *sess)
{
	os_mutex_lock(&sess->desktop_control_mutex);
	if (sess->desktop_control == NULL) {
		char exe[1024] = {0};
		char name[256] = {0};
		if (!u_desktop_self_identity(exe, sizeof(exe), name, sizeof(name))) {
			snprintf(name, sizeof(name), "this app");
		}
		struct u_desktop_app_info app = {exe, name, NULL};
		struct u_desktop_control_callbacks cb = {on_state_changed, on_action_completed, sess,
		                                         on_capture_state_changed, on_capture_completed};
		u_desktop_control_create(u_desktop_backend_create(), &app, &cb, NULL, &sess->desktop_control);
	}
	struct u_desktop_control *dc = sess->desktop_control;
	os_mutex_unlock(&sess->desktop_control_mutex);
	return dc;
}

void
oxr_desktop_control_session_fini(struct oxr_session *sess)
{
	os_mutex_lock(&sess->desktop_control_mutex);
	struct u_desktop_control *dc = sess->desktop_control;
	sess->desktop_control = NULL;
	os_mutex_unlock(&sess->desktop_control_mutex);
	u_desktop_control_destroy(&dc);
}


/*
 *
 * Conversions.
 *
 */

static void
to_xr_window(const struct u_desktop_window *in, XrDesktopWindowDXR *out)
{
	out->type = XR_TYPE_DESKTOP_WINDOW_DXR;
	out->next = NULL;
	out->windowId = in->id;
	out->parentWindowId = in->parent_id;
	out->pid = in->pid;
	memcpy(out->appId, in->app_id, sizeof(out->appId));
	out->appId[sizeof(out->appId) - 1] = '\0';
	memcpy(out->title, in->title, sizeof(out->title));
	out->title[sizeof(out->title) - 1] = '\0';
	out->frame.offset.x = in->frame.x;
	out->frame.offset.y = in->frame.y;
	out->frame.extent.width = in->frame.w;
	out->frame.extent.height = in->frame.h;
	out->state = in->state;
	out->zOrder = in->z;
}

//! Fill @p out (keeps its type/next) from @p in; window zeroed unless kind == WINDOW.
static void
to_xr_pick(const struct u_desktop_pick *in, XrDesktopPickDXR *out)
{
	out->point.x = in->x;
	out->point.y = in->y;
	out->kind = (XrDesktopPickKindDXR)in->kind;
	memset(&out->window, 0, sizeof(out->window));
	if (in->kind == U_DESKTOP_PICK_WINDOW) {
		to_xr_window(&in->window, &out->window);
	}
}

static XrResult
gate_result(struct oxr_logger *log, int32_t r)
{
	switch (r) {
	case U_DESKTOP_XR_SUCCESS:
	case U_DESKTOP_XR_ERROR_NOT_GRANTED:
	case U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED:
	case U_DESKTOP_XR_ERROR_REFUSED:
	case U_DESKTOP_XR_ERROR_WINDOW_LOST: return (XrResult)r;
	default: return oxr_error(log, (XrResult)r, "desktop backend call failed");
	}
}


/*
 *
 * Entry points.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDesktopControlStateDXR(XrSession session,
                                XrDesktopControlStateDXR *state,
                                XrDesktopControlCapabilityFlagsDXR *capabilities)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDesktopControlStateDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_NOT_NULL(&log, state);

	// Never gated (it is how an app learns it is not granted).
	struct u_desktop_control *dc = get_dc(sess);
	uint32_t caps = 0;
	*state = dc != NULL ? (XrDesktopControlStateDXR)u_desktop_control_get_state(dc, &caps)
	                    : XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	if (capabilities != NULL) {
		*capabilities = caps;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestDesktopControlDXR(XrSession session, const XrDesktopControlRequestInfoDXR *info)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRequestDesktopControlDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_TYPE_CAN_BE_NULL(&log, info, XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR);
	// Spec v2: the scopes, chained on info->next (absent = CONTROL only, as in v1).
	// Other structures in the chain are ignored, as OpenXR does.
	uint32_t scopes = U_DESKTOP_SCOPE_CONTROL;
	for (const XrBaseInStructure *n = info != NULL ? (const XrBaseInStructure *)info->next : NULL; n != NULL;
	     n = n->next) {
		if (n->type != XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR) {
			continue;
		}
		XrDesktopControlScopeFlagsDXR s = ((const XrDesktopControlRequestScopeDXR *)n)->scopes;
		const XrDesktopControlScopeFlagsDXR scope_bits =
		    XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR | XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR;
		const XrDesktopControlScopeFlagsDXR known = scope_bits | XR_DESKTOP_CONTROL_SCOPE_ASK_AGAIN_BIT_DXR;
		if ((s & scope_bits) == 0 || (s & ~known) != 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "(XrDesktopControlRequestScopeDXR::scopes) must be non-zero, known bits only");
		}
		scopes = (uint32_t)s;
		break;
	}

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "could not create the desktop-control session");
	}
	char reason[512] = {0};
	if (info != NULL && info->reason != NULL) {
		snprintf(reason, sizeof(reason), "%s", info->reason);
	}
	return gate_result(&log, u_desktop_control_request_scopes(dc, reason[0] != '\0' ? reason : NULL, scopes));
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateDesktopWindowsDXR(XrSession session,
                                 uint32_t windowCapacityInput,
                                 uint32_t *windowCountOutput,
                                 XrDesktopWindowDXR *windows)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrEnumerateDesktopWindowsDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_NOT_NULL(&log, windowCountOutput);
	if (windowCapacityInput > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, windows);
		for (uint32_t i = 0; i < windowCapacityInput; i++) {
			if (windows[i].type != XR_TYPE_DESKTOP_WINDOW_DXR) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "(windows[%u].type) is not XR_TYPE_DESKTOP_WINDOW_DXR", i);
			}
		}
	}

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}

	struct u_desktop_window *tmp = NULL;
	if (windowCapacityInput > 0) {
		tmp = (struct u_desktop_window *)calloc(windowCapacityInput, sizeof(*tmp));
		if (tmp == NULL) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "out of memory");
		}
	}
	uint32_t count = 0;
	int32_t r = u_desktop_control_list_windows(dc, tmp, windowCapacityInput, &count);
	if (r != U_DESKTOP_XR_SUCCESS) {
		free(tmp);
		return gate_result(&log, r);
	}
	*windowCountOutput = count;
	if (windowCapacityInput == 0) {
		free(tmp);
		return XR_SUCCESS;
	}
	if (windowCapacityInput < count) {
		free(tmp);
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	for (uint32_t i = 0; i < count; i++) {
		to_xr_window(&tmp[i], &windows[i]);
	}
	free(tmp);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDesktopPointerDXR(XrSession session, XrDesktopPickDXR *pick)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDesktopPointerDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, pick, XR_TYPE_DESKTOP_PICK_DXR);

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}
	struct u_desktop_pick p;
	int32_t r = u_desktop_control_get_pointer(dc, &p);
	if (r != U_DESKTOP_XR_SUCCESS) {
		return gate_result(&log, r);
	}
	to_xr_pick(&p, pick);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPickDesktopDXR(XrSession session, XrOffset2Di point, XrDesktopPickDXR *pick)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrPickDesktopDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, pick, XR_TYPE_DESKTOP_PICK_DXR);

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}
	struct u_desktop_pick p;
	int32_t r = u_desktop_control_pick(dc, point.x, point.y, &p);
	if (r != U_DESKTOP_XR_SUCCESS) {
		return gate_result(&log, r);
	}
	to_xr_pick(&p, pick);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDesktopTextAtDXR(
    XrSession session, XrOffset2Di point, uint32_t capacityInput, uint32_t *countOutput, char *buffer)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDesktopTextAtDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);
	if (capacityInput > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, buffer);
	}

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}
	char *tmp = (char *)malloc(TEXT_AT_MAX);
	if (tmp == NULL) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "out of memory");
	}
	// Live UI on every call: the fill call may see more text than the size call
	// did, and then answers SIZE_INSUFFICIENT with the new count (retry).
	int32_t r = u_desktop_control_text_at(dc, point.x, point.y, tmp, TEXT_AT_MAX);
	if (r != U_DESKTOP_XR_SUCCESS) {
		free(tmp);
		return gate_result(&log, r);
	}
	// u_desktop_control_text_at already dropped a code point a full buffer cut in
	// half (u_desktop_utf8_trim_partial); the old in-place loop here tested the NUL
	// terminator instead of the last byte and never trimmed anything.
	size_t len = strlen(tmp);
	uint32_t needed = (uint32_t)len + 1;
	*countOutput = needed;
	XrResult ret = XR_SUCCESS;
	if (capacityInput > 0) {
		if (capacityInput < needed) {
			ret = XR_ERROR_SIZE_INSUFFICIENT;
		} else {
			memcpy(buffer, tmp, needed);
		}
	}
	free(tmp);
	return ret;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitDesktopActionDXR(XrSession session, const XrDesktopActionDXR *action, uint64_t *actionId)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSubmitDesktopActionDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, action, XR_TYPE_DESKTOP_ACTION_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, actionId);
	*actionId = 0;

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}

	struct u_desktop_action a;
	memset(&a, 0, sizeof(a));
	a.type = (uint32_t)action->actionType;
	a.target_window_id = action->targetWindowId;
	a.rect.x = action->rect.offset.x;
	a.rect.y = action->rect.offset.y;
	a.rect.w = action->rect.extent.width;
	a.rect.h = action->rect.extent.height;
	a.x = action->point.x;
	a.y = action->point.y;
	a.x2 = action->point2.x;
	a.y2 = action->point2.y;
	a.button = (uint32_t)action->button;
	a.click_count = action->clickCount;
	a.scroll_x = action->scrollX;
	a.scroll_y = action->scrollY;
	a.text = action->text;
	a.keys = action->keys;

	int32_t r = u_desktop_control_submit(dc, &a, actionId);
	if (r == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE) {
		char why[160];
		u_desktop_control_validate(&a, why, sizeof(why));
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "%s", why);
	}
	if (r == U_DESKTOP_XR_ERROR_LIMIT_REACHED) {
		return oxr_error(&log, XR_ERROR_LIMIT_REACHED, "%u actions already queued for this session",
		                 U_DESKTOP_CONTROL_QUEUE_SIZE);
	}
	return gate_result(&log, r);
}


/*
 *
 * Spec v2: screen capture (§10).
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDesktopCaptureStateDXR(XrSession session, XrDesktopControlStateDXR *state)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDesktopCaptureStateDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_NOT_NULL(&log, state);

	// Never gated, like xrGetDesktopControlStateDXR.
	struct u_desktop_control *dc = get_dc(sess);
	*state = dc != NULL ? (XrDesktopControlStateDXR)u_desktop_control_get_capture_state(dc)
	                    : XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCaptureDesktopDXR(XrSession session, const XrDesktopCaptureInfoDXR *info, uint64_t *captureId)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCaptureDesktopDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_DESKTOP_CAPTURE_INFO_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, captureId);
	*captureId = 0;

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	}
	struct u_desktop_capture_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.target = (uint32_t)info->target;
	ci.window_id = info->windowId;
	ci.region.x = info->region.offset.x;
	ci.region.y = info->region.offset.y;
	ci.region.w = info->region.extent.width;
	ci.region.h = info->region.extent.height;
	ci.max_w = info->maxWidth;
	ci.max_h = info->maxHeight;

	int32_t r = u_desktop_control_capture(dc, &ci, captureId);
	if (r == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE) {
		char why[160];
		u_desktop_control_capture_validate(&ci, why, sizeof(why));
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "%s", why);
	}
	if (r == U_DESKTOP_XR_ERROR_LIMIT_REACHED) {
		// Normal flow for an agent polling faster than its captures complete: no log.
		return XR_ERROR_LIMIT_REACHED;
	}
	return gate_result(&log, r);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDesktopCaptureImageDXR(
    XrSession session, uint64_t captureId, uint32_t capacityInput, uint32_t *countOutput, uint8_t *rgba)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetDesktopCaptureImageDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_desktop_control);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);
	if (capacityInput > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, rgba);
	}

	struct u_desktop_control *dc = get_dc(sess);
	if (dc == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "no capture with that id");
	}
	int32_t r = u_desktop_control_capture_image(dc, captureId, capacityInput, countOutput, rgba);
	switch (r) {
	case U_DESKTOP_XR_SUCCESS: return XR_SUCCESS;
	case U_DESKTOP_XR_ERROR_SIZE_INSUFFICIENT: return XR_ERROR_SIZE_INSUFFICIENT;
	case U_DESKTOP_XR_ERROR_VALIDATION_FAILURE:
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(captureId) is not the most recent successful capture, or its image was "
		                 "already fetched or expired (10 s)");
	default: return oxr_error(&log, (XrResult)r, "desktop capture image failed");
	}
}

#endif // OXR_HAVE_DXR_desktop_control
