// Copyright 2018-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds session related functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup oxr_main
 */

#include "oxr_frame_sync.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_config_build.h" // IWYU pragma: keep
#include "xrt/xrt_config_have.h"  // IWYU pragma: keep
#include "xrt/xrt_config_os.h"    // IWYU pragma: keep

#ifdef XR_USE_PLATFORM_XLIB
#include "xrt/xrt_gfx_xlib.h" // IWYU pragma: keep

#endif // XR_USE_PLATFORM_XLIB

#ifdef XRT_HAVE_VULKAN
#include "xrt/xrt_gfx_vk.h" // IWYU pragma: keep

#endif // XRT_HAVE_VULKAN

#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_visibility_mask.h"
#include "util/u_verify.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_space.h"

#include "display3d_view.h"
#include "camera3d_view.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_two_call.h"
#include "oxr_handle.h"
#include "oxr_chain.h"
#include "oxr_api_verify.h"
#include "oxr_pretty_print.h"
#include "oxr_conversions.h"
#include "oxr_xret.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

// Vendor-neutral display metric types (eye positions, window metrics, Kooima FOV)
#include "xrt/xrt_display_metrics.h"

// Eye tracking backends (vendor-specific headers gated by their own ifdefs)

#include "sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include "multi/comp_multi_private.h"

#ifdef XRT_HAVE_LEIA_SR_VULKAN
#include "leia/leia_sr.h"
#endif

#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
#include "d3d11/comp_d3d11_compositor.h"
#endif


DEBUG_GET_ONCE_NUM_OPTION(ipd, "OXR_DEBUG_IPD_MM", 63)
DEBUG_GET_ONCE_NUM_OPTION(wait_frame_sleep, "OXR_DEBUG_WAIT_FRAME_EXTRA_SLEEP_MS", 0)
DEBUG_GET_ONCE_BOOL_OPTION(frame_timing_spew, "OXR_FRAME_TIMING_SPEW", false)
DEBUG_GET_ONCE_BOOL_OPTION(hand_tracking_prioritize_conforming, "OXR_HAND_TRACKING_PRIORITIZE_CONFORMING", false)


/*
 *
 * Helpers.
 *
 */

static bool
should_render(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_VISIBLE:
	case XR_SESSION_STATE_FOCUSED:
	case XR_SESSION_STATE_STOPPING: return true;
	default: return false;
	}
}

XRT_MAYBE_UNUSED static const char *
to_string(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_UNKNOWN: return "XR_SESSION_STATE_UNKNOWN";
	case XR_SESSION_STATE_IDLE: return "XR_SESSION_STATE_IDLE";
	case XR_SESSION_STATE_READY: return "XR_SESSION_STATE_READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "XR_SESSION_STATE_SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "XR_SESSION_STATE_VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "XR_SESSION_STATE_FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "XR_SESSION_STATE_STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "XR_SESSION_STATE_LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "XR_SESSION_STATE_EXITING";
	case XR_SESSION_STATE_MAX_ENUM: return "XR_SESSION_STATE_MAX_ENUM";
	default: return "";
	}
}

/*!
 * Get predicted eye positions from the session's compositor.
 * Returns false if eye tracking is not available.
 */
static bool
oxr_session_get_predicted_eye_positions(struct oxr_session *sess, struct xrt_eye_pair *out_eye_pair)
{
	if (sess == NULL || sess->xcn == NULL || out_eye_pair == NULL) {
		return false;
	}

#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
	// D3D11 native compositor path
	if (sess->is_d3d11_native_compositor) {
		struct xrt_vec3 left_eye, right_eye;
		bool got_positions = comp_d3d11_compositor_get_predicted_eye_positions(&sess->xcn->base, &left_eye, &right_eye);

		if (got_positions) {
			out_eye_pair->left = (struct xrt_eye_position){left_eye.x, left_eye.y, left_eye.z};
			out_eye_pair->right = (struct xrt_eye_position){right_eye.x, right_eye.y, right_eye.z};
			out_eye_pair->timestamp_ns = os_monotonic_get_ns();
			out_eye_pair->valid = true;
			// Heuristic: collapsed eye positions = tracking lost
			float dx = right_eye.x - left_eye.x;
			float dy = right_eye.y - left_eye.y;
			float dz = right_eye.z - left_eye.z;
			float dist_sq = dx * dx + dy * dy + dz * dz;
			out_eye_pair->is_tracking = (dist_sq > 1e-6f);
			return true;
		}
		return false;
	}
#endif

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	// Multi-compositor path (Vulkan)
	struct multi_compositor *mc = multi_compositor(&sess->xcn->base);
	return multi_compositor_get_predicted_eye_positions(mc, (struct leiasr_eye_pair *)out_eye_pair);
#else
	return false;
#endif
}

/*!
 * Get display dimensions for Kooima asymmetric FOV calculation.
 *
 * Vendor-neutral: reads from xrt_system_compositor_info which is populated
 * at init time by either SR SDK or sim_display. The D3D11 native compositor
 * path is kept for backward compatibility.
 */
static bool
oxr_session_get_display_dimensions(struct oxr_session *sess, float *out_width_m, float *out_height_m)
{
	if (sess == NULL || out_width_m == NULL || out_height_m == NULL) {
		return false;
	}

#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
	// D3D11 native compositor path (has its own display dimension query)
	if (sess->xcn != NULL && sess->is_d3d11_native_compositor) {
		return comp_d3d11_compositor_get_display_dimensions(&sess->xcn->base, out_width_m, out_height_m);
	}
#endif

	// Generic path: read from system compositor info (populated at init by SR or sim_display)
	const struct xrt_system_compositor_info *info = &sess->sys->xsysc->info;
	if (info->display_width_m <= 0.0f || info->display_height_m <= 0.0f) {
		return false;
	}
	*out_width_m = info->display_width_m;
	*out_height_m = info->display_height_m;
	return true;
}

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 *
 * Vendor-neutral: works with both SR SDK (via per-session weaver) and
 * sim_display (via generic Win32 APIs in multi compositor).
 */
static bool
oxr_session_get_window_metrics(struct oxr_session *sess,
                                struct xrt_window_metrics *out_metrics)
{
	if (sess == NULL || sess->xcn == NULL || out_metrics == NULL) {
		return false;
	}

#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
	if (sess->is_d3d11_native_compositor) {
		return comp_d3d11_compositor_get_window_metrics(&sess->xcn->base,
		    (struct leiasr_window_metrics *)out_metrics);
	}
#endif

	// Vendor-neutral path — works for both SR and sim_display (in-process only).
	// IPC clients have an ipc_client_compositor, not a multi_compositor.
	if (sess->sys->xsysc->xmcc != NULL) {
		struct multi_compositor *mc = multi_compositor(&sess->xcn->base);
		return multi_compositor_get_window_metrics(mc, out_metrics);
	}

	return false;
}

// Helper: copy XrFovf → xrt_fov (same layout, different field names)
static inline void
xrfov_to_xrt_fov(const XrFovf *src, struct xrt_fov *dst)
{
	dst->angle_left = src->angleLeft;
	dst->angle_right = src->angleRight;
	dst->angle_up = src->angleUp;
	dst->angle_down = src->angleDown;
}

#ifdef OXR_HAVE_EXT_display_info
XrResult
oxr_session_request_display_mode(struct oxr_logger *log, struct oxr_session *sess, bool enable_3d)
{
	if (sess == NULL || sess->xcn == NULL) {
		return oxr_error(log, XR_ERROR_HANDLE_INVALID, "Invalid session");
	}

	bool success = false;

#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
	if (sess->is_d3d11_native_compositor) {
		success = comp_d3d11_compositor_request_display_mode(&sess->xcn->base, enable_3d);
		if (success) {
			sess->display_mode_3d = enable_3d;
		}
		return XR_SUCCESS;
	}
#endif

	// In-process multi compositor path (not used for IPC clients).
	// IPC clients have an ipc_client_compositor, not a multi_compositor.
	if (sess->sys->xsysc->xmcc != NULL) {
		struct multi_compositor *mc = multi_compositor(&sess->xcn->base);
		success = multi_compositor_request_display_mode(mc, enable_3d);
		if (success) {
			sess->display_mode_3d = enable_3d;
		}
		return XR_SUCCESS;
	}

	(void)success;
	return XR_SUCCESS;
}
#endif // OXR_HAVE_EXT_display_info

static XrResult
emit_reference_space_change_pending(struct oxr_logger *log,
                                    struct oxr_session *sess,
                                    struct xrt_session_event_reference_space_change_pending *ref_change,
                                    XrReferenceSpaceType type)
{
	struct oxr_instance *inst = sess->sys->inst;
	XrTime changeTime = time_state_monotonic_to_ts_ns(inst->timekeeping, ref_change->timestamp_ns);
	const XrPosef *poseInPreviousSpace = (XrPosef *)&ref_change->pose_in_previous_space;
	bool poseValid = ref_change->pose_valid;

	//! @todo properly handle return (not done yet because requires larger rewrite),
	oxr_event_push_XrEventDataReferenceSpaceChangePending( //
	    log,                                               // log
	    sess,                                              // sess
	    type,                                              // referenceSpaceType
	    changeTime,                                        // changeTime
	    poseValid,                                         // poseValid
	    poseInPreviousSpace);                              // poseInPreviousSpace

	return XR_SUCCESS;
}

static XrResult
handle_reference_space_change_pending(struct oxr_logger *log,
                                      struct oxr_session *sess,
                                      struct xrt_session_event_reference_space_change_pending *ref_change)
{
	struct oxr_instance *inst = sess->sys->inst;
	XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;

	if (inst->quirks.map_stage_to_local_floor) {
		/* When stage is mapped to local_floor:
		 * ignore stage changes
		 * for local_floor changes, send a duplicate envent for stage
		 * */
		switch (ref_change->ref_type) {
		case XRT_SPACE_REFERENCE_TYPE_STAGE: return XR_SUCCESS;
		case XRT_SPACE_REFERENCE_TYPE_LOCAL_FLOOR:
			emit_reference_space_change_pending(log, sess, ref_change, XR_REFERENCE_SPACE_TYPE_STAGE);
			break;
		default: break;
		}
	}

	switch (ref_change->ref_type) {
	case XRT_SPACE_REFERENCE_TYPE_VIEW: type = XR_REFERENCE_SPACE_TYPE_VIEW; break;
	case XRT_SPACE_REFERENCE_TYPE_LOCAL: type = XR_REFERENCE_SPACE_TYPE_LOCAL; break;
	case XRT_SPACE_REFERENCE_TYPE_STAGE: type = XR_REFERENCE_SPACE_TYPE_STAGE; break;
	case XRT_SPACE_REFERENCE_TYPE_LOCAL_FLOOR:
#ifdef OXR_HAVE_EXT_local_floor
		if (inst->extensions.EXT_local_floor) {
			type = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
			break;
		} else {
			// Silently ignored, extension not enabled.
			return XR_SUCCESS;
		}
#else
		// Silently ignored, not compiled with this extension supported.
		return XR_SUCCESS;
#endif
	case XRT_SPACE_REFERENCE_TYPE_UNBOUNDED:
#ifdef OXR_HAVE_MSFT_unbounded_reference_space
		if (inst->extensions.MSFT_unbounded_reference_space) {
			type = XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT;
			break;
		} else {
			// Silently ignored, extension not enabled.
			return XR_SUCCESS;
		}
#else
		// Silently ignored, not compiled with this extension supported.
		return XR_SUCCESS;
#endif
	case XRT_SPACE_REFERENCE_TYPE_DISPLAY:
		// Display space is physically anchored, not affected by recentering.
		return XR_SUCCESS;
	}

	if (type == XR_REFERENCE_SPACE_TYPE_MAX_ENUM) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "invalid reference space type");
	}

	emit_reference_space_change_pending(log, sess, ref_change, type);

	return XR_SUCCESS;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
oxr_session_change_state(struct oxr_logger *log, struct oxr_session *sess, XrSessionState state, XrTime time)
{
	if (sess->state == state) {
		oxr_warn(log,
		         "Session state changed to the same state (%s), not sending XrEventDataSessionStateChanged",
		         to_string(state));
		return;
	}

	oxr_event_push_XrEventDataSessionStateChanged(log, sess, state, time);
	sess->state = state;
}

XrResult
oxr_session_enumerate_formats(struct oxr_logger *log,
                              struct oxr_session *sess,
                              uint32_t formatCapacityInput,
                              uint32_t *formatCountOutput,
                              int64_t *formats)
{
	struct oxr_instance *inst = sess->sys->inst;
	struct xrt_compositor *xc = sess->compositor;
	if (formatCountOutput == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "(formatCountOutput == NULL) cannot be null");
	}
	if (xc == NULL) {
		if (formatCountOutput != NULL) {
			*formatCountOutput = 0;
		}
		return oxr_session_success_result(sess);
	}

	uint32_t filtered_count = 0;
	int64_t filtered_formats[XRT_MAX_SWAPCHAIN_FORMATS];
	for (uint32_t i = 0; i < xc->info.format_count; i++) {
		int64_t format = xc->info.formats[i];

		if (inst->quirks.disable_vulkan_format_depth_stencil &&
		    format == 130 /* VK_FORMAT_D32_SFLOAT_S8_UINT */) {
			continue;
		}

		filtered_formats[filtered_count++] = format;
	}

	OXR_TWO_CALL_HELPER(log, formatCapacityInput, formatCountOutput, formats, filtered_count, filtered_formats,
	                    oxr_session_success_result(sess));
}

XrResult
oxr_session_begin(struct oxr_logger *log, struct oxr_session *sess, const XrSessionBeginInfo *beginInfo)
{
	/*
	 * If the session is not running when the application calls xrBeginSession, but the session is not yet in the
	 * XR_SESSION_STATE_READY state, the runtime must return error XR_ERROR_SESSION_NOT_READY.
	 */
	if (sess->state != XR_SESSION_STATE_READY) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_READY, "Session is not ready to begin");
	}

	struct xrt_compositor *xc = sess->compositor;
	if (xc != NULL) {
		XrViewConfigurationType view_type = beginInfo->primaryViewConfigurationType;

		// in a headless session there is no compositor and primaryViewConfigurationType must be ignored
		if (sess->compositor != NULL && view_type != sess->sys->view_config_type) {
			/*! @todo we only support a single view config type per
			 * system right now */
			return oxr_error(log, XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED,
			                 "(beginInfo->primaryViewConfigurationType == "
			                 "0x%08x) view configuration type not supported",
			                 view_type);
		}

		const struct oxr_extension_status *extensions = &sess->sys->inst->extensions;

		const struct xrt_begin_session_info begin_session_info = {
		    .view_type = (enum xrt_view_type)beginInfo->primaryViewConfigurationType,
#ifdef OXR_HAVE_EXT_hand_tracking
		    .ext_hand_tracking_enabled = extensions->EXT_hand_tracking,
#endif
#ifdef OXR_HAVE_EXT_hand_tracking_data_source
		    .ext_hand_tracking_data_source_enabled = extensions->EXT_hand_tracking_data_source,
#endif
#ifdef OXR_HAVE_EXT_eye_gaze_interaction
		    .ext_eye_gaze_interaction_enabled = extensions->EXT_eye_gaze_interaction,
#endif
#ifdef OXR_HAVE_EXT_hand_interaction
		    .ext_hand_interaction_enabled = extensions->EXT_hand_interaction,
#endif
#ifdef OXR_HAVE_HTC_facial_tracking
		    .htc_facial_tracking_enabled = extensions->HTC_facial_tracking,
#endif
#ifdef OXR_HAVE_FB_body_tracking
		    .fb_body_tracking_enabled = extensions->FB_body_tracking,
#endif
#ifdef OXR_HAVE_FB_face_tracking2
		    .fb_face_tracking2_enabled = extensions->FB_face_tracking2,
#endif
#ifdef OXR_HAVE_META_body_tracking_full_body
		    .meta_body_tracking_full_body_enabled = extensions->META_body_tracking_full_body,
#endif
#ifdef OXR_HAVE_META_body_tracking_calibration
		    .meta_body_tracking_calibration_enabled = extensions->META_body_tracking_calibration,
#endif
		};

		xrt_result_t xret = xrt_comp_begin_session(xc, &begin_session_info);
		OXR_CHECK_XRET(log, sess, xret, xrt_comp_begin_session);

#ifdef OXR_HAVE_EXT_user_presence
		struct xrt_device *xdev = GET_XDEV_BY_ROLE(sess->sys, head);
		if (extensions->EXT_user_presence && xdev->supported.presence) {
			bool presence = false;
			xret = xrt_device_get_presence(xdev, &presence);
			OXR_CHECK_XRET(log, sess, xret, xrt_device_get_presence);
			oxr_event_push_XrEventDataUserPresenceChangedEXT(log, sess, presence);
		}
#endif

		// For AppContainer apps (Chrome WebXR), force visibility/focus flags to true
		// to avoid race condition where client calls xrBeginSession before polling events.
		if (sess->is_appcontainer && (!sess->compositor_visible || !sess->compositor_focused)) {
			sess->compositor_visible = true;
			sess->compositor_focused = true;
		}

		// Trigger state transitions if visibility/focus flags are set.
		// AppContainer apps: delay VISIBLE/FOCUSED until first xrWaitFrame.
		if (sess->compositor_visible && sess->compositor_focused) {
			oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
			if (!sess->is_appcontainer) {
				oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
				oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED, 0);
			}
		}
	} else {
		// Headless, pretend we got event from the compositor.
		sess->compositor_visible = true;
		sess->compositor_focused = true;

		// Transition into focused.
		oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
		oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED, 0);
	}
	XrResult ret = oxr_frame_sync_begin_session(&sess->frame_sync);
	if (ret != XR_SUCCESS) {
		return oxr_error(log, ret,
		                 "Frame sync object refused to let us begin session, probably already running");
	}

	// Auto-switch to 3D mode on session begin
#ifdef OXR_HAVE_EXT_display_info
	oxr_session_request_display_mode(log, sess, true);
#endif

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_end(struct oxr_logger *log, struct oxr_session *sess)
{
	// there is a bug in Unreal 4 where calling this function will result in a crash, so skip it.
	if (sess->sys->inst->quirks.skip_end_session) {
		return XR_SUCCESS;
	}

	/*
	 * If the session is not running when the application calls xrEndSession, the runtime must return
	 * error XR_ERROR_SESSION_NOT_RUNNING
	 */
	if (!oxr_frame_sync_is_session_running(&sess->frame_sync)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING, "Session is not running");
	}

	/*
	 * If the session is still running when the application calls xrEndSession, but the session is not yet in
	 * the XR_SESSION_STATE_STOPPING state, the runtime must return error XR_ERROR_SESSION_NOT_STOPPING.
	 */
	if (sess->state != XR_SESSION_STATE_STOPPING) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_STOPPING, "Session is not stopping");
	}

	// Auto-switch to 2D mode on session end
#ifdef OXR_HAVE_EXT_display_info
	oxr_session_request_display_mode(log, sess, false);
#endif

	struct xrt_compositor *xc = sess->compositor;
	if (xc != NULL) {
		if (sess->frame_id.waited > 0) {
			xrt_comp_discard_frame(xc, sess->frame_id.waited);
			sess->frame_id.waited = -1;
		}
		if (sess->frame_id.begun > 0) {
			xrt_comp_discard_frame(xc, sess->frame_id.begun);
			sess->frame_id.begun = -1;
		}
		sess->frame_started = false;

		xrt_result_t xret = xrt_comp_end_session(xc);
		OXR_CHECK_XRET(log, sess, xret, xrt_comp_end_session);
	} else {
		// Headless, pretend we got event from the compositor.
		sess->compositor_visible = false;
		sess->compositor_focused = false;
	}

	oxr_session_change_state(log, sess, XR_SESSION_STATE_IDLE, 0);
	if (sess->exiting) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_EXITING, 0);
	} else {
#ifndef XRT_OS_ANDROID
		// @todo In multi-clients scenario with a session being reused, changing session
		//       state to XR_SESSION_STATE_READY would cause application to call xrBeginSession
		//       immediately and this is not desired. On Android platform, runtime would
		//       change session state to XR_SESSION_STATE_READY once application goes to
		//       foreground again. But on other platform it's not handled yet.
		oxr_session_change_state(log, sess, XR_SESSION_STATE_READY, 0);
#endif // !XRT_OS_ANDROID
	}
	XrResult ret = oxr_frame_sync_end_session(&sess->frame_sync);
	if (ret != XR_SUCCESS) {
		return oxr_error(log, ret, "Frame sync object refused to let us end session, probably not running");
	}
	sess->has_ended_once = false;

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_request_exit(struct oxr_logger *log, struct oxr_session *sess)
{
	if (sess->state == XR_SESSION_STATE_FOCUSED) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
	}
	if (sess->state == XR_SESSION_STATE_VISIBLE) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
	}
	if (!sess->has_ended_once && sess->state != XR_SESSION_STATE_SYNCHRONIZED) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
		// Fake the synchronization.
		sess->has_ended_once = true;
	}

	//! @todo start fading out the app.
	oxr_session_change_state(log, sess, XR_SESSION_STATE_STOPPING, 0);
	sess->exiting = true;
	return oxr_session_success_result(sess);
}

#ifdef OXR_HAVE_FB_passthrough
static inline XrPassthroughStateChangedFlagsFB
xrt_to_passthrough_state_flags(enum xrt_passthrough_state state)
{
	XrPassthroughStateChangedFlagsFB res = 0;
	if (state & XRT_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT) {
		res |= XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB;
	}
	if (state & XRT_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT) {
		res |= XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB;
	}
	if (state & XRT_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT) {
		res |= XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB;
	}
	if (state & XRT_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT) {
		res |= XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB;
	}
	return res;
}
#endif

XrResult
oxr_session_poll(struct oxr_logger *log, struct oxr_session *sess)
{
	struct xrt_session *xs = sess->xs;
	xrt_result_t xret;

	if (xs == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "xrt_session is null");
	}

#ifdef XRT_OS_MACOS
	// Pump macOS events on the main thread. NSWindow and CAMetalLayer
	// require periodic run loop processing for the Window Server to
	// commit rendered content to the screen. This is the only place
	// where the app's main thread calls into the runtime each frame.
	extern void oxr_macos_pump_events(struct xrt_device **xdevs, uint32_t xdev_count);
	oxr_macos_pump_events(sess->sys->xsysd->xdevs, sess->sys->xsysd->xdev_count);
#endif

#ifdef XRT_OS_ANDROID
	// Most recent Android activity lifecycle event was OnPause: move toward stopping
	if (sess->sys->inst->activity_state == XRT_ANDROID_LIVECYCLE_EVENT_ON_PAUSE) {
		if (sess->state == XR_SESSION_STATE_FOCUSED) {
			U_LOG_I("Activity paused: changing session state FOCUSED->VISIBLE");
			oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
		}

		if (sess->state == XR_SESSION_STATE_VISIBLE) {
			U_LOG_I("Activity paused: changing session state VISIBLE->SYNCHRONIZED");
			oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
		}

		if (sess->state == XR_SESSION_STATE_SYNCHRONIZED) {
			U_LOG_I("Activity paused: changing session state SYNCHRONIZED->STOPPING");
			oxr_session_change_state(log, sess, XR_SESSION_STATE_STOPPING, 0);
		}
		// TODO return here to avoid polling other events?
		// see https://gitlab.freedesktop.org/monado/monado/-/issues/419
	}

	// Most recent Android activity lifecycle event was OnResume: move toward ready
	if (sess->sys->inst->activity_state == XRT_ANDROID_LIVECYCLE_EVENT_ON_RESUME) {
		if (sess->state == XR_SESSION_STATE_IDLE) {
			U_LOG_I("Activity resumed: changing session state IDLE->READY");
			oxr_session_change_state(log, sess, XR_SESSION_STATE_READY, 0);
		}
	}
#endif // XRT_OS_ANDROID


	bool read_more_events = true;
	while (read_more_events) {
		union xrt_session_event xse = {0};
		xret = xrt_session_poll_events(xs, &xse);
		OXR_CHECK_XRET(log, sess, xret, "xrt_session_poll_events");

		// dispatch based on event type
		switch (xse.type) {
		case XRT_SESSION_EVENT_NONE:
			// No more events.
			read_more_events = false;
			break;
		case XRT_SESSION_EVENT_STATE_CHANGE:
			sess->compositor_visible = xse.state.visible;
			sess->compositor_focused = xse.state.focused;
			break;
		case XRT_SESSION_EVENT_OVERLAY_CHANGE:
#ifdef OXR_HAVE_EXTX_overlay
			oxr_event_push_XrEventDataMainSessionVisibilityChangedEXTX(log, sess, xse.overlay.visible);
#endif
			break;
		case XRT_SESSION_EVENT_LOSS_PENDING:
			oxr_session_change_state(
			    log, sess, XR_SESSION_STATE_LOSS_PENDING,
			    time_state_monotonic_to_ts_ns(sess->sys->inst->timekeeping, xse.loss_pending.loss_time_ns));
			break;
		case XRT_SESSION_EVENT_LOST: sess->has_lost = true; break;
		case XRT_SESSION_EVENT_DISPLAY_REFRESH_RATE_CHANGE:
#ifdef OXR_HAVE_FB_display_refresh_rate
			oxr_event_push_XrEventDataDisplayRefreshRateChangedFB( //
			    log,                                               //
			    sess,                                              //
			    xse.display.from_display_refresh_rate_hz,          //
			    xse.display.to_display_refresh_rate_hz);           //
#endif
			break;
		case XRT_SESSION_EVENT_REFERENCE_SPACE_CHANGE_PENDING:
			handle_reference_space_change_pending(log, sess, &xse.ref_change);
			break;
		case XRT_SESSION_EVENT_PERFORMANCE_CHANGE:
#ifdef OXR_HAVE_EXT_performance_settings
			oxr_event_push_XrEventDataPerfSettingsEXTX(
			    log, sess, xse.performance.domain, xse.performance.sub_domain, xse.performance.from_level,
			    xse.performance.to_level);
#endif // OXR_HAVE_EXT_performance_settings
			break;
		case XRT_SESSION_EVENT_PASSTHRU_STATE_CHANGE:
#ifdef OXR_HAVE_FB_passthrough
			oxr_event_push_XrEventDataPassthroughStateChangedFB(
			    log, sess, xrt_to_passthrough_state_flags(xse.passthru.state));
#endif // OXR_HAVE_FB_passthrough
			break;
		case XRT_SESSION_EVENT_VISIBILITY_MASK_CHANGE:
#ifdef OXR_HAVE_KHR_visibility_mask
			oxr_event_push_XrEventDataVisibilityMaskChangedKHR(log, sess, sess->sys->view_config_type,
			                                                   xse.mask_change.view_index);
			break;
#endif // OXR_HAVE_KHR_visibility_mask
		case XRT_SESSION_EVENT_USER_PRESENCE_CHANGE:
#ifdef OXR_HAVE_EXT_user_presence
			oxr_event_push_XrEventDataUserPresenceChangedEXT(log, sess,
			                                                 xse.presence_change.is_user_present);
#endif // OXR_HAVE_EXT_user_presence
			break;
		case XRT_SESSION_EVENT_EXIT_REQUEST:
			// Runtime-initiated session exit (e.g. own window was closed).
			// Drive the state machine to STOPPING so the app calls xrEndSession.
			// Set sess->exiting so xrEndSession transitions IDLE → EXITING
			// (not IDLE → READY). Without this, apps like Blender see READY
			// and immediately restart VR, creating a new window in a loop.
			// With EXITING, apps destroy the session and stay alive — the user
			// can start a new VR session manually.
			sess->exiting = true;
			if (sess->state == XR_SESSION_STATE_FOCUSED) {
				oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
			}
			if (sess->state == XR_SESSION_STATE_VISIBLE) {
				oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
			}
			if (!sess->has_ended_once && sess->state != XR_SESSION_STATE_SYNCHRONIZED) {
				oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
				sess->has_ended_once = true;
			}
			oxr_session_change_state(log, sess, XR_SESSION_STATE_STOPPING, 0);
			break;
		default: U_LOG_W("unhandled event type! %d", xse.type); break;
		}
	}

	// For AppContainer apps, delay VISIBLE/FOCUSED until the poll AFTER SYNCHRONIZED.
	if (sess->is_appcontainer && sess->state == XR_SESSION_STATE_SYNCHRONIZED) {
		if (sess->appcontainer_synchronized_polled) {
			if (sess->compositor_visible) {
				oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
			}
		} else {
			sess->appcontainer_synchronized_polled = true;
		}
	} else if (!sess->is_appcontainer && sess->state == XR_SESSION_STATE_SYNCHRONIZED && sess->compositor_visible) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
	}

	if (sess->state == XR_SESSION_STATE_VISIBLE && sess->compositor_focused && !sess->is_appcontainer) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED, 0);
	} else if (sess->is_appcontainer && sess->state == XR_SESSION_STATE_VISIBLE && sess->compositor_focused) {
		// AppContainer: deliver FOCUSED right after VISIBLE
		oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED, 0);
	}

	if (sess->state == XR_SESSION_STATE_FOCUSED && !sess->compositor_focused) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
	}

	if (sess->state == XR_SESSION_STATE_VISIBLE && !sess->compositor_visible) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED, 0);
	}

	return XR_SUCCESS;
}

static inline XrViewStateFlags
xrt_to_view_state_flags(enum xrt_space_relation_flags flags)
{
	XrViewStateFlags res = 0;
	if (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) {
		res |= XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	}
	if (flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) {
		res |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
	}
	if (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) {
		res |= XR_VIEW_STATE_POSITION_VALID_BIT;
	}
	if (flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) {
		res |= XR_VIEW_STATE_POSITION_TRACKED_BIT;
	}
	return res;
}

static void
adjust_fov(const struct xrt_fov *original_fov, const struct xrt_quat *original_rotation, struct xrt_fov *adjusted_fov)
{
	struct xrt_quat identity = XRT_QUAT_IDENTITY;

	struct xrt_quat original_rotation_inv;
	math_quat_invert(original_rotation, &original_rotation_inv);

	struct xrt_quat rotation_diff;
	math_quat_rotate(&original_rotation_inv, &identity, &rotation_diff);

	struct xrt_vec3 euler_angles;
	math_quat_to_euler_angles(&rotation_diff, &euler_angles);

	*adjusted_fov = (struct xrt_fov){
	    .angle_left = original_fov->angle_left + euler_angles.y,
	    .angle_right = original_fov->angle_right + euler_angles.y,
	    .angle_up = original_fov->angle_up + euler_angles.x,
	    .angle_down = original_fov->angle_down + euler_angles.x,
	};
}

XrResult
oxr_session_locate_views(struct oxr_logger *log,
                         struct oxr_session *sess,
                         const XrViewLocateInfo *viewLocateInfo,
                         XrViewState *viewState,
                         uint32_t viewCapacityInput,
                         uint32_t *viewCountOutput,
                         XrView *views)
{
	struct oxr_sink_logger slog = {0};
	bool print = sess->sys->inst->debug_views;
	struct xrt_device *xdev = GET_XDEV_BY_ROLE(sess->sys, head);
	struct oxr_space *baseSpc = XRT_CAST_OXR_HANDLE_TO_PTR(struct oxr_space *, viewLocateInfo->space);
	uint32_t view_count = xdev->hmd->view_count;

	// Start two call handling.
	if (viewCountOutput != NULL) {
		*viewCountOutput = view_count;
	}
	if (viewCapacityInput == 0) {
		return oxr_session_success_result(sess);
	}
	if (viewCapacityInput < view_count) {
		return oxr_error(log, XR_ERROR_SIZE_INSUFFICIENT, "(viewCapacityInput == %u) need %u",
		                 viewCapacityInput, view_count);
	}
	// End two call handling.

	if (print) {
		oxr_slog(&slog, "\n\tviewLocateInfo->displayTime: %" PRIu64, viewLocateInfo->displayTime);
		oxr_pp_space_indented(&slog, baseSpc, "viewLocateInfo->baseSpace");
	}

	/*
	 * Get head relation, fovs and view poses.
	 */

	// To be passed down to the devices, some can override this.
	// Start with static IPD-based calculation.
	struct xrt_vec3 default_eye_relation = {
	    sess->ipd_meters,
	    0.0f,
	    0.0f,
	};

	const uint64_t xdisplay_time =
	    time_state_ts_to_monotonic_ns(sess->sys->inst->timekeeping, viewLocateInfo->displayTime);

	// The head pose as in the xdev's space, aka XRT_INPUT_GENERIC_HEAD_POSE.
	struct xrt_space_relation T_xdev_head = XRT_SPACE_RELATION_ZERO;
	struct xrt_fov fovs[XRT_MAX_VIEWS] = {0};
	struct xrt_pose poses[XRT_MAX_VIEWS] = {0};

	// Eye pair and tracking state (vendor-neutral types)
	struct xrt_eye_pair eye_pair = {0};
	bool have_kooima_fov = false;

	// Throttled logging for display/Kooima diagnostics
	static int log_counter = 0;
	bool should_log = (++log_counter % 120) == 1; // Log every ~2 seconds at 60fps

	// World head pose - declared here so it's accessible in the view loop later
	struct xrt_vec3 world_head_pos = {0.0f, 1.6f, 0.0f};  // Default: standing height
	struct xrt_quat world_head_ori = XRT_QUAT_IDENTITY;

	// Stereo view override: when set, view poses use these world-space eye positions
	bool have_stereo_eye_override = false;
	struct xrt_vec3 stereo_eye_world[2] = {{0}};
#ifdef XRT_BUILD_DRIVER_QWERTY
	struct qwerty_stereo_state stereo_state = {0};
	bool have_stereo_state = qwerty_get_stereo_state(
	    sess->sys->xsysd->xdevs, sess->sys->xsysd->xdev_count, &stereo_state);
	if (should_log) {
		U_LOG_I("STEREO STATE: have=%d cam=%d cam_ipd=%.3f cam_conv=%.2f disp_ipd=%.3f disp_vH=%.2f",
		        have_stereo_state, stereo_state.camera_mode,
		        stereo_state.cam_ipd_factor, stereo_state.cam_convergence,
		        stereo_state.disp_ipd_factor, stereo_state.disp_vHeight);
	}
#else
	struct { bool camera_mode; float cam_ipd_factor, cam_parallax_factor, cam_convergence,
	         cam_half_tan_vfov, disp_ipd_factor, disp_parallax_factor, disp_vHeight,
	         nominal_viewer_z, screen_height_m; } stereo_state = {0};
	bool have_stereo_state = false;
#endif

	// Query eye tracking (vendor-neutral — returns false if no backend available)
	bool got_eye_positions = oxr_session_get_predicted_eye_positions(sess, &eye_pair);

	if (should_log) {
		U_LOG_I("Eye tracking: got_positions=%d, valid=%d, is_d3d11=%d",
		        got_eye_positions, eye_pair.valid, sess->is_d3d11_native_compositor);
		if (got_eye_positions) {
			U_LOG_I("  left=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f)",
			        eye_pair.left.x, eye_pair.left.y, eye_pair.left.z,
			        eye_pair.right.x, eye_pair.right.y, eye_pair.right.z);
		}
	}

	// Get device pose for stereo world-space computation (qwerty = virtual display)
	if (!sess->has_external_window) {
		struct xrt_space_relation display_relation = XRT_SPACE_RELATION_ZERO;
		xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, xdisplay_time, &display_relation);
		world_head_pos = display_relation.pose.position;
		world_head_ori = display_relation.pose.orientation;

		if (should_log) {
			U_LOG_I("Display pose: pos=(%.3f,%.3f,%.3f) ori=(%.3f,%.3f,%.3f,%.3f)",
			        world_head_pos.x, world_head_pos.y, world_head_pos.z,
			        world_head_ori.x, world_head_ori.y, world_head_ori.z, world_head_ori.w);
		}
	}

	bool have_eyes = got_eye_positions && eye_pair.valid;

	// Kooima FOV computation (vendor-neutral)
	// Works with either tracked eye positions or nominal viewer position
	{
		struct xrt_eye_position adj_left = {0};
		struct xrt_eye_position adj_right = {0};
		bool have_eye_positions = false;

		if (have_eyes) {
			// Tracked eyes (from eye tracking SDK)
			adj_left = eye_pair.left;
			adj_right = eye_pair.right;
			have_eye_positions = true;
		} else {
			// Nominal viewer from system compositor info (sim_display, etc.)
			const struct xrt_system_compositor_info *sinfo = &sess->sys->xsysc->info;
			if (sinfo->nominal_viewer_z_m > 0.0f) {
				float ipd_m = sess->ipd_meters;
				adj_left = (struct xrt_eye_position){
				    -ipd_m / 2.0f, sinfo->nominal_viewer_y_m, sinfo->nominal_viewer_z_m};
				adj_right = (struct xrt_eye_position){
				    ipd_m / 2.0f, sinfo->nominal_viewer_y_m, sinfo->nominal_viewer_z_m};
				have_eye_positions = true;
				if (should_log) {
					U_LOG_I("Nominal eyes: L=(%.4f,%.4f,%.4f) R=(%.4f,%.4f,%.4f), IPD=%.1fmm",
					        adj_left.x, adj_left.y, adj_left.z,
					        adj_right.x, adj_right.y, adj_right.z,
					        ipd_m * 1000.0f);
				}
			}
		}

		if (have_eye_positions) {
			float screen_width_m = 0.0f;
			float screen_height_m = 0.0f;

			struct xrt_window_metrics wm = {0};
			bool have_wm = oxr_session_get_window_metrics(sess, &wm);

			if (have_wm && wm.valid && wm.window_width_m > 0.0f && wm.window_height_m > 0.0f) {
				// SRHydra viewport scale formula
				float min_disp = fminf(wm.display_width_m, wm.display_height_m);
				float min_win  = fminf(wm.window_width_m, wm.window_height_m);
				float vs = min_disp / min_win;

				screen_width_m  = wm.window_width_m * vs;
				screen_height_m = wm.window_height_m * vs;

				if (should_log) {
					U_LOG_I("Window-adaptive FOV: vs=%.3f, screen=%.4fx%.4fm, "
					        "eye_offset=(%.4f,%.4f)m",
					        vs, screen_width_m, screen_height_m,
					        wm.window_center_offset_x_m, wm.window_center_offset_y_m);
				}
			} else if (oxr_session_get_display_dimensions(sess, &screen_width_m, &screen_height_m) &&
			           screen_width_m > 0.0f && screen_height_m > 0.0f) {
				// Fallback: full display dimensions (fullscreen or no window metrics)
			}

			if (should_log) {
					U_LOG_I("KOOIMA GATE: have_eyes=%d screen=%.4fx%.4f have_stereo=%d cam=%d ext_win=%d",
					        have_eye_positions, screen_width_m, screen_height_m,
					        have_stereo_state, stereo_state.camera_mode,
					        sess->has_external_window);
				}

				if (screen_width_m > 0.0f && screen_height_m > 0.0f) {
				// Nominal viewer for stereo math (parallax lerp target)
				const struct xrt_system_compositor_info *si = &sess->sys->xsysc->info;
				XrVector3f nominal = {0, si->nominal_viewer_y_m, si->nominal_viewer_z_m};
				XrVector3f raw_l = {adj_left.x, adj_left.y, adj_left.z};
				XrVector3f raw_r = {adj_right.x, adj_right.y, adj_right.z};
				Display3DScreen scr = {screen_width_m, screen_height_m};
				XrPosef display_pose = {
				    {world_head_ori.x, world_head_ori.y, world_head_ori.z, world_head_ori.w},
				    {world_head_pos.x, world_head_pos.y, world_head_pos.z}};

				// Camera-centric path: canonical camera3d_compute_stereo_views
				if (have_stereo_state && stereo_state.camera_mode &&
				    !sess->has_external_window) {
					Camera3DTunables ct = {
					    .ipd_factor = stereo_state.cam_ipd_factor,
					    .parallax_factor = stereo_state.cam_parallax_factor,
					    .inv_convergence_distance = stereo_state.cam_convergence,
					    .half_tan_vfov = stereo_state.cam_half_tan_vfov,
					};
					Camera3DStereoView cam_views[2];
					camera3d_compute_stereo_views(&raw_l, &raw_r, &nominal, &scr, &ct,
					                              &display_pose, 0.01f, 100.0f,
					                              &cam_views[0], &cam_views[1]);

					// Extract {fov, eye_world} — runtime doesn't need matrices
					for (int ei = 0; ei < 2; ei++) {
						xrfov_to_xrt_fov(&cam_views[ei].fov, &fovs[ei]);
						stereo_eye_world[ei] = (struct xrt_vec3){
						    cam_views[ei].eye_world.x,
						    cam_views[ei].eye_world.y,
						    cam_views[ei].eye_world.z};
					}
					have_kooima_fov = true;
					have_stereo_eye_override = true;
				} else {
					// Display-centric (Kooima) path: canonical display3d_compute_stereo_views
					Display3DTunables dt = display3d_default_tunables();
					if (have_stereo_state && !sess->has_external_window) {
						dt.ipd_factor = stereo_state.disp_ipd_factor;
						dt.parallax_factor = stereo_state.disp_parallax_factor;
						dt.perspective_factor = 1.0f;
						dt.virtual_display_height = stereo_state.disp_vHeight;
					} else {
						dt.virtual_display_height = screen_height_m; // identity m2v
					}

					Display3DStereoView disp_views[2];
					display3d_compute_stereo_views(&raw_l, &raw_r, &nominal, &scr, &dt,
					                               &display_pose, 0.01f, 100.0f,
					                               &disp_views[0], &disp_views[1]);

					// Extract {fov, eye_world} — runtime doesn't need matrices
					for (int ei = 0; ei < 2; ei++) {
						xrfov_to_xrt_fov(&disp_views[ei].fov, &fovs[ei]);
					}
					have_kooima_fov = true;

					if (!sess->has_external_window) {
						for (int ei = 0; ei < 2; ei++) {
							stereo_eye_world[ei] = (struct xrt_vec3){
							    disp_views[ei].eye_world.x,
							    disp_views[ei].eye_world.y,
							    disp_views[ei].eye_world.z};
						}
						have_stereo_eye_override = true;
					}
				}

				// Compare FOVs between eyes (throttled logging)
				if (should_log) {
					float left_h_fov = (fovs[0].angle_right - fovs[0].angle_left) * 57.2958f;
					float right_h_fov = (fovs[1].angle_right - fovs[1].angle_left) * 57.2958f;
					float left_v_fov = (fovs[0].angle_up - fovs[0].angle_down) * 57.2958f;
					float right_v_fov = (fovs[1].angle_up - fovs[1].angle_down) * 57.2958f;
					U_LOG_I("FOV: Left H=%.2f° V=%.2f°, Right H=%.2f° V=%.2f°",
					        left_h_fov, left_v_fov, right_h_fov, right_v_fov);
				}
			} else if (have_eyes) {
				// Eye tracking active but no display dims — fallback to device FOV
				fovs[0] = xdev->hmd->distortion.fov[0];
				fovs[1] = xdev->hmd->distortion.fov[1];
			}
		}
	}

	// Always get view poses from device (provides T_xdev_head and poses[])
	// Save Kooima fovs before xrt_device_get_view_poses overwrites them
	{
		struct xrt_fov kooima_fovs[2];
		if (have_kooima_fov && have_stereo_state) {
			kooima_fovs[0] = fovs[0];
			kooima_fovs[1] = fovs[1];
		}

		xrt_result_t xret = xrt_device_get_view_poses( //
		    xdev,                                      //
		    &default_eye_relation,                     //
		    xdisplay_time,                             //
		    view_count,                                //
		    &T_xdev_head,                              //
		    fovs,                                      //
		    poses);
		OXR_CHECK_XRET(log, sess, xret, xrt_device_get_view_poses);

		// Restore client-side Kooima FOVs only when the client has local
		// stereo state (non-IPC mode). In IPC mode, the server already
		// computes stereo-adjusted Kooima FOVs and returns them via
		// get_view_poses — don't override those.
		if (have_kooima_fov && have_stereo_state) {
			fovs[0] = kooima_fovs[0];
			fovs[1] = kooima_fovs[1];
		}

		if (should_log) {
			float h0 = (fovs[0].angle_right - fovs[0].angle_left) * 57.2958f;
			float v0 = (fovs[0].angle_up - fovs[0].angle_down) * 57.2958f;
			U_LOG_I("FINAL FOV: kooima=%d override=%d H=%.2f° V=%.2f° "
			        "L=%.4f R=%.4f U=%.4f D=%.4f",
			        have_kooima_fov, have_stereo_eye_override, h0, v0,
			        fovs[0].angle_left, fovs[0].angle_right,
			        fovs[0].angle_up, fovs[0].angle_down);
		}
	}

	// The xdev pose in the base space.
	struct xrt_space_relation T_base_xdev = XRT_SPACE_RELATION_ZERO;
	XrResult ret = oxr_space_locate_device( //
	    log,                                //
	    xdev,                               //
	    baseSpc,                            //
	    viewLocateInfo->displayTime,        //
	    &T_base_xdev);                      //
	if (ret != XR_SUCCESS || T_base_xdev.relation_flags == 0) {
		if (print) {
			oxr_slog(&slog, "\n\tReturning invalid poses");
			oxr_log_slog(log, &slog);
		} else {
			oxr_slog_cancel(&slog);
		}
		return ret;
	}

	struct xrt_space_relation T_base_head;
	struct xrt_relation_chain xrc = {0};
	m_relation_chain_push_relation(&xrc, &T_xdev_head);
	m_relation_chain_push_relation(&xrc, &T_base_xdev);
	m_relation_chain_resolve(&xrc, &T_base_head);


	if (print) {
		for (uint32_t i = 0; i < view_count; i++) {
			char tmp[32];
			snprintf(tmp, 32, "xdev.view[%i]", i);
			oxr_pp_fov_indented_as_object(&slog, &fovs[i], tmp);
			oxr_pp_pose_indented_as_object(&slog, &poses[i], tmp);
		}
		oxr_pp_relation_indented(&slog, &T_xdev_head, "T_xdev_head");
		oxr_pp_relation_indented(&slog, &T_base_xdev, "T_base_xdev");
	}

	for (uint32_t i = 0; i < view_count; i++) {
		/*
		 * Pose
		 */

		struct xrt_pose view_pose = poses[i];

		if (sess->sys->inst->quirks.parallel_views) {
			view_pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
		}

		// Do the magical space relation dance here.
		struct xrt_space_relation result = {0};
		struct xrt_relation_chain xrc = {0};
		m_relation_chain_push_pose_if_not_identity(&xrc, &view_pose);
		m_relation_chain_push_relation(&xrc, &T_base_head);
		m_relation_chain_resolve(&xrc, &result);
		OXR_XRT_POSE_TO_XRPOSEF(result.pose, views[i].pose);

		// Override view poses for stereo controls or eye tracking.
		// stereo_eye_world[] is set by either camera-centric or display-centric path
		// to ensure FOV and view position are consistent.
		if ((have_eyes || have_stereo_eye_override) && view_count == 2) {
			if (have_stereo_eye_override && !sess->has_external_window) {
				// STEREO OVERRIDE: use pre-computed world-space eye positions
				views[i].pose.position.x = stereo_eye_world[i].x;
				views[i].pose.position.y = stereo_eye_world[i].y;
				views[i].pose.position.z = stereo_eye_world[i].z;
				views[i].pose.orientation = (XrQuaternionf){
				    world_head_ori.x, world_head_ori.y, world_head_ori.z, world_head_ori.w};
			} else if (have_eyes) {
				// Get tracked eye position for this view (in display-local coords)
				struct xrt_vec3 tracked_eye = (i == 0)
				    ? (struct xrt_vec3){eye_pair.left.x, eye_pair.left.y, eye_pair.left.z}
				    : (struct xrt_vec3){eye_pair.right.x, eye_pair.right.y, eye_pair.right.z};

				if (!sess->has_external_window) {
					// DISPLAY MODE (Monado window): Transform tracked eye to world
					struct xrt_vec3 rotated_eye;
					math_quat_rotate_vec3(&world_head_ori, &tracked_eye, &rotated_eye);

					views[i].pose.position.x = world_head_pos.x + rotated_eye.x;
					views[i].pose.position.y = world_head_pos.y + rotated_eye.y;
					views[i].pose.position.z = world_head_pos.z + rotated_eye.z;
					views[i].pose.orientation = (XrQuaternionf){
					    world_head_ori.x, world_head_ori.y, world_head_ori.z, world_head_ori.w};
				} else {
					// SESSION TARGET: Use tracked eye positions directly
					views[i].pose.position.x = tracked_eye.x;
					views[i].pose.position.y = tracked_eye.y;
					views[i].pose.position.z = tracked_eye.z;
					views[i].pose.orientation = (XrQuaternionf){0.0f, 0.0f, 0.0f, 1.0f};
				}
			}

			if (should_log) {
				U_LOG_I("Eye %d: view=(%.3f,%.3f,%.3f) mode=%s",
				        i, views[i].pose.position.x, views[i].pose.position.y,
				        views[i].pose.position.z,
				        have_stereo_eye_override ? "camera" : "display");
			}
		}

		/*
		 * Fov
		 */

		struct xrt_fov fov = fovs[i];

		if (sess->sys->inst->quirks.parallel_views) {
			adjust_fov(&fovs[i], &poses[i].orientation, &fov);
		}

		OXR_XRT_FOV_TO_XRFOVF(fov, views[i].fov);

		if (should_log && i == 0) {
			U_LOG_I("VIEW[0] FINAL: pos=(%.3f,%.3f,%.3f) fov_L=%.4f fov_R=%.4f fov_U=%.4f fov_D=%.4f",
			        views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
			        views[0].fov.angleLeft, views[0].fov.angleRight,
			        views[0].fov.angleUp, views[0].fov.angleDown);
		}


		/*
		 * Printing.
		 */

		if (print) {
			char tmp[16];
			snprintf(tmp, 16, "view[%i]", i);
			oxr_pp_pose_indented_as_object(&slog, &result.pose, tmp);
		}

		/*
		 * Checking, debug and flag handling.
		 */

		struct xrt_pose *pose = (struct xrt_pose *)&views[i].pose;
		if ((result.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0 &&
		    !math_quat_ensure_normalized(&pose->orientation)) {
			struct xrt_quat *q = &pose->orientation;
			struct xrt_quat norm = *q;
			math_quat_normalize(&norm);
			oxr_slog_cancel(&slog);
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
			                 "Quaternion %a %a %a %a (normalized %a %a %a %a) "
			                 "in xrLocateViews was invalid",
			                 q->x, q->y, q->z, q->w, norm.x, norm.y, norm.z, norm.w);
		}

		if (i == 0) {
			viewState->viewStateFlags = xrt_to_view_state_flags(result.relation_flags);
		} else {
			viewState->viewStateFlags &= xrt_to_view_state_flags(result.relation_flags);
		}
	}

#ifdef OXR_HAVE_EXT_display_info
	if (sess->sys->inst->extensions.EXT_display_info) {
		XrViewEyeTrackingStateEXT *ets = OXR_GET_OUTPUT_FROM_CHAIN(
		    viewState, XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT, XrViewEyeTrackingStateEXT);
		if (ets) {
			ets->activeMode = (XrEyeTrackingModeEXT)sess->eye_tracking_mode;
			if (got_eye_positions && eye_pair.valid) {
				ets->isTracking = eye_pair.is_tracking ? XR_TRUE : XR_FALSE;
			} else {
				// No eye tracking backend or invalid data.
				// For sim_display (RAW_BIT, always "tracking"), eye positions
				// come from nominal viewer, not the eye tracking path.
				// Report isTracking based on whether the system has tracking
				// and the nominal path was used successfully.
				const struct xrt_system_compositor_info *et_info =
				    sess->sys->xsysc ? &sess->sys->xsysc->info : NULL;
				uint32_t supported = et_info ? et_info->supported_eye_tracking_modes : 0;
				if (supported != 0 && !got_eye_positions) {
					// System claims tracking support but no eye tracking
					// backend is active (e.g., sim_display with nominal viewer).
					// Sim display is always "tracking" its simulated viewer.
					ets->isTracking = XR_TRUE;
				} else {
					ets->isTracking = XR_FALSE;
				}
			}
		}
	}
#endif // OXR_HAVE_EXT_display_info

	if (print) {
		oxr_log_slog(log, &slog);
	} else {
		oxr_slog_cancel(&slog);
	}

	return oxr_session_success_result(sess);
}

static double
ns_to_ms(int64_t ns)
{
	double ms = ((double)ns) * 1. / 1000. * 1. / 1000.;
	return ms;
}

static double
ts_ms(struct oxr_session *sess)
{
	timepoint_ns now = time_state_get_now(sess->sys->inst->timekeeping);
	int64_t monotonic = time_state_ts_to_monotonic_ns(sess->sys->inst->timekeeping, now);
	return ns_to_ms(monotonic);
}

static XrResult
do_wait_frame_and_checks(struct oxr_logger *log,
                         struct oxr_session *sess,
                         int64_t *out_frame_id,
                         int64_t *out_predicted_display_time,
                         int64_t *out_predicted_display_period,
                         XrTime *out_converted_time)
{
	assert(sess->compositor != NULL);

	int64_t frame_id = -1;
	int64_t predicted_display_time = 0;
	int64_t predicted_display_period = 0;

	xrt_result_t xret = xrt_comp_wait_frame( //
	    sess->compositor,                    // compositor
	    &frame_id,                           // out_frame_id
	    &predicted_display_time,             // out_predicted_display_time
	    &predicted_display_period);          // out_predicted_display_period
	OXR_CHECK_XRET(log, sess, xret, xrt_comp_wait_frame);

	if (frame_id < 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Got a negative frame id '%" PRIi64 "'", frame_id);
	}

	if ((int64_t)predicted_display_time <= 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Got a negative display time '%" PRIi64 "'",
		                 (int64_t)predicted_display_time);
	}

	XrTime converted_time = time_state_monotonic_to_ts_ns(sess->sys->inst->timekeeping, predicted_display_time);
	if (converted_time <= 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Got '%" PRIi64 "' from time_state_monotonic_to_ts_ns",
		                 converted_time);
	}

	*out_frame_id = frame_id;
	*out_predicted_display_time = predicted_display_time;
	*out_predicted_display_period = predicted_display_period;
	*out_converted_time = converted_time;

	return XR_SUCCESS;
}

XrResult
oxr_session_frame_wait(struct oxr_logger *log, struct oxr_session *sess, XrFrameState *frameState)
{
	//! @todo this should be carefully synchronized, because there may be
	//! more than one session per instance.
	XRT_MAYBE_UNUSED timepoint_ns now = time_state_get_now_and_update(sess->sys->inst->timekeeping);

	struct xrt_compositor *xc = sess->compositor;
	if (xc == NULL) {
		frameState->shouldRender = XR_FALSE;
		return oxr_session_success_result(sess);
	}

	// For AppContainer apps (Chrome WebXR), deliver the deferred VISIBLE/FOCUSED
	// state transitions at the start of xrWaitFrame. This is earlier than xrEndFrame
	// and allows Chrome to proceed with its frame loop.
	if (sess->is_appcontainer &&
	    sess->state == XR_SESSION_STATE_SYNCHRONIZED &&
	    sess->compositor_visible && sess->compositor_focused) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE, 0);
		oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED, 0);
	}

	if (sess->frame_timing_spew) {
		oxr_log(log, "Called at %8.3fms", ts_ms(sess));
	}

	/*
	 * A subsequent xrWaitFrame call must: block until the previous frame
	 * has been begun. It's extremely forbidden to call xrWaitFrame from
	 * multiple threads. We do this before so we call predicted after any
	 * waiting for xrBeginFrame has happened, for better timing information.
	 */
	XrResult ret = oxr_frame_sync_wait_frame(&sess->frame_sync);
	if (XR_SUCCESS != ret) {
		// session not running
		return ret;
	}

	if (sess->frame_timing_spew) {
		oxr_log(log, "Finished waiting for previous frame begin at %8.3fms", ts_ms(sess));
	}

	int64_t frame_id = -1;
	int64_t predicted_display_time = 0;
	int64_t predicted_display_period = 0;
	XrTime converted_time = 0;

	ret = do_wait_frame_and_checks( //
	    log,                        // log
	    sess,                       // sess
	    &frame_id,                  // out_frame_id
	    &predicted_display_time,    // out_predicted_display_time
	    &predicted_display_period,  // out_predicted_display_period
	    &converted_time);           // out_converted_time
	if (ret != XR_SUCCESS) {
		// On error we need to release the semaphore ourselves as xrBeginFrame won't do it.
		// Should not get an error.
		XrResult release_ret = oxr_frame_sync_release(&sess->frame_sync);
		assert(release_ret == XR_SUCCESS);
		(void)release_ret;
		// Error already logged.
		return ret;
	}
	assert(predicted_display_time != 0);
	assert(predicted_display_period != 0);
	assert(converted_time != 0);

	/*
	 * We set the frame_id along with the number of active waited frames to
	 * avoid races with xrBeginFrame. The function xrBeginFrame will only
	 * allow xrWaitFrame to continue from the semaphore above once it has
	 * cleared the `sess->frame_id.waited`.
	 */
	os_mutex_lock(&sess->active_wait_frames_lock);
	sess->active_wait_frames++;
	sess->frame_id.waited = frame_id;
	os_mutex_unlock(&sess->active_wait_frames_lock);

	frameState->shouldRender = should_render(sess->state);
	frameState->predictedDisplayPeriod = predicted_display_period;
	frameState->predictedDisplayTime = converted_time;

	if (sess->frame_timing_spew) {
		oxr_log(log,
		        "Waiting finished at %8.3fms. Predicted display time "
		        "%8.3fms, "
		        "period %8.3fms",
		        ts_ms(sess), ns_to_ms(predicted_display_time), ns_to_ms(predicted_display_period));
	}

	if (sess->frame_timing_wait_sleep_ms > 0) {
		int64_t sleep_ns = U_TIME_1MS_IN_NS * sess->frame_timing_wait_sleep_ms;
		os_precise_sleeper_nanosleep(&sess->sleeper, sleep_ns);
	}

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_frame_begin(struct oxr_logger *log, struct oxr_session *sess)
{
	struct xrt_compositor *xc = sess->compositor;

	os_mutex_lock(&sess->active_wait_frames_lock);
	int active_wait_frames = sess->active_wait_frames;
	os_mutex_unlock(&sess->active_wait_frames_lock);

	XrResult ret;
	if (active_wait_frames == 0) {
		return oxr_error(log, XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without xrWaitFrame");
	}

	if (sess->frame_started) {
		// max 2 xrWaitFrame can be in flight so a second xrBeginFrame
		// is only valid if we have a second xrWaitFrame in flight
		if (active_wait_frames != 2) {
			return oxr_error(log, XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without xrWaitFrame");
		}


		ret = XR_FRAME_DISCARDED;
		if (xc != NULL) {
			xrt_result_t xret = xrt_comp_discard_frame(xc, sess->frame_id.begun);
			OXR_CHECK_XRET(log, sess, xret, xrt_comp_discard_frame);
			sess->frame_id.begun = -1;

			os_mutex_lock(&sess->active_wait_frames_lock);
			sess->active_wait_frames--;
			os_mutex_unlock(&sess->active_wait_frames_lock);
		}
	} else {
		ret = oxr_session_success_result(sess);
		sess->frame_started = true;
	}
	if (xc != NULL) {
		xrt_result_t xret = xrt_comp_begin_frame(xc, sess->frame_id.waited);
		OXR_CHECK_XRET(log, sess, xret, xrt_comp_begin_frame);
		sess->frame_id.begun = sess->frame_id.waited;
		sess->frame_id.waited = -1;
	}

	// beginFrame is about to succeed, we can release an xrWaitFrame, if available.
	XrResult osh_ret = oxr_frame_sync_release(&sess->frame_sync);
	if (XR_SUCCESS != osh_ret) {
		// session not running
		return osh_ret;
	}

	return ret;
}

static XrResult
oxr_session_destroy(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_session *sess = (struct oxr_session *)hb;

	XrResult ret = oxr_event_remove_session_events(log, sess);

	oxr_session_binding_destroy_all(log, sess);

	for (size_t i = 0; i < sess->action_set_attachment_count; ++i) {
		oxr_action_set_attachment_teardown(&sess->act_set_attachments[i]);
	}
	free(sess->act_set_attachments);
	sess->act_set_attachments = NULL;
	sess->action_set_attachment_count = 0;

	// If we tore everything down correctly, these are empty now.
	assert(sess->act_sets_attachments_by_key == NULL || u_hashmap_int_empty(sess->act_sets_attachments_by_key));
	assert(sess->act_attachments_by_key == NULL || u_hashmap_int_empty(sess->act_attachments_by_key));

	u_hashmap_int_destroy(&sess->act_sets_attachments_by_key);
	u_hashmap_int_destroy(&sess->act_attachments_by_key);

	xrt_comp_destroy(&sess->compositor);
	xrt_comp_native_destroy(&sess->xcn);
	xrt_session_destroy(&sess->xs);

	os_precise_sleeper_deinit(&sess->sleeper);
	oxr_frame_sync_fini(&sess->frame_sync);
	os_mutex_destroy(&sess->active_wait_frames_lock);

	free(sess);

	return ret;
}

static XrResult
oxr_session_allocate_and_init(struct oxr_logger *log,
                              struct oxr_system *sys,
                              enum oxr_session_graphics_ext gfx_ext,
                              struct oxr_session **out_session)
{
	struct oxr_session *sess = NULL;
	OXR_ALLOCATE_HANDLE_OR_RETURN(log, sess, OXR_XR_DEBUG_SESSION, oxr_session_destroy, &sys->inst->handle);

	// What graphics API type was this created with.
	sess->gfx_ext = gfx_ext;

	// What system is this session based on.
	sess->sys = sys;

	// Init the begin/wait frame handler.
	oxr_frame_sync_init(&sess->frame_sync);

	// Init the wait frame precise sleeper.
	os_precise_sleeper_init(&sess->sleeper);

	sess->active_wait_frames = 0;
	os_mutex_init(&sess->active_wait_frames_lock);

	// Debug and user options.
	sess->ipd_meters = debug_get_num_option_ipd() / 1000.0f;
	sess->frame_timing_spew = debug_get_bool_option_frame_timing_spew();
	sess->frame_timing_wait_sleep_ms = debug_get_num_option_wait_frame_sleep();

#ifdef OXR_HAVE_EXT_display_info
	// Initialize eye tracking mode from driver's default
	if (sys->xsysc) {
		sess->eye_tracking_mode = sys->xsysc->info.default_eye_tracking_mode;
	}
#endif

	// Action system hashmaps.
	u_hashmap_int_create(&sess->act_sets_attachments_by_key);
	u_hashmap_int_create(&sess->act_attachments_by_key);

	// Done with basic init, set out variable.
	*out_session = sess;

	return XR_SUCCESS;
}

#define OXR_CHECK_XSYSC(LOG, SYS)                                                                                      \
	do {                                                                                                           \
		if (sys->xsysc == NULL) {                                                                              \
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,                                             \
			                 " Can not use graphics bindings when have asked to not create graphics");     \
		}                                                                                                      \
	} while (false)

#define OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(LOG, XSI, SESS)                                                   \
	do {                                                                                                           \
		if ((SESS)->sys->xsysc == NULL) {                                                                      \
			return oxr_error((LOG), XR_ERROR_RUNTIME_FAILURE,                                              \
			                 "The system compositor wasn't created, can't create native compositor!");     \
		}                                                                                                      \
		xrt_result_t xret = xrt_system_create_session((SESS)->sys->xsys, (XSI), &(SESS)->xs, &(SESS)->xcn);    \
		if (xret == XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED) {                                                 \
			return oxr_error((LOG), XR_ERROR_LIMIT_REACHED, "Per instance multi-session not supported.");  \
		}                                                                                                      \
		if (xret != XRT_SUCCESS) {                                                                             \
			return oxr_error((LOG), XR_ERROR_RUNTIME_FAILURE,                                              \
			                 "Failed to create xrt_session and xrt_compositor_native! '%i'", xret);        \
		}                                                                                                      \
		if ((SESS)->sys->xsysc->xmcc != NULL) {                                                                \
			xrt_syscomp_set_state((SESS)->sys->xsysc, &(SESS)->xcn->base, true, true);                     \
			xrt_syscomp_set_z_order((SESS)->sys->xsysc, &(SESS)->xcn->base, 0);                            \
			/* Pass system devices to multi_compositor for qwerty input */                                  \
			struct multi_compositor *_mc = multi_compositor(&(SESS)->xcn->base);                            \
			_mc->xsysd = (SESS)->sys->xsysd;                                                              \
		}                                                                                                      \
	} while (false)

#define OXR_SESSION_ALLOCATE_AND_INIT(LOG, SYS, GFX_TYPE, OUT)                                                         \
	do {                                                                                                           \
		XrResult ret = oxr_session_allocate_and_init(LOG, SYS, GFX_TYPE, &OUT);                                \
		if (ret != XR_SUCCESS) {                                                                               \
			return ret;                                                                                    \
		}                                                                                                      \
	} while (0)


/*
 * Does allocation, population and basic init, so we can use early-returns to
 * simplify code flow and avoid weird if/else.
 */
static XrResult
oxr_session_create_impl(struct oxr_logger *log,
                        struct oxr_system *sys,
                        const XrSessionCreateInfo *createInfo,
                        const struct xrt_session_info *xsi,
                        struct oxr_session **out_session)
{
#if defined(XR_USE_PLATFORM_XLIB) && defined(XR_USE_GRAPHICS_API_OPENGL)
	XrGraphicsBindingOpenGLXlibKHR const *opengl_xlib = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR, XrGraphicsBindingOpenGLXlibKHR);
	if (opengl_xlib != NULL) {
		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called "
			                 "xrGetOpenGL[ES]GraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_XLIB_GL, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_gl_xlib(log, sys, opengl_xlib, *out_session);
	}
#endif


#if defined(XR_USE_PLATFORM_ANDROID) && defined(XR_USE_GRAPHICS_API_OPENGL_ES)
	XrGraphicsBindingOpenGLESAndroidKHR const *opengles_android = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, XrGraphicsBindingOpenGLESAndroidKHR);
	if (opengles_android != NULL) {
		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called "
			                 "xrGetOpenGLESGraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_ANDROID_GLES, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_gles_android(log, sys, opengles_android, *out_session);
	}
#endif

#if defined(XR_USE_PLATFORM_WIN32) && defined(XR_USE_GRAPHICS_API_OPENGL)
	XrGraphicsBindingOpenGLWin32KHR const *opengl_win32 = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR, XrGraphicsBindingOpenGLWin32KHR);
	if (opengl_win32 != NULL) {
		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called xrGetOpenGLGraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_WIN32_GL, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_gl_win32(log, sys, opengl_win32, *out_session);
	}
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
	XrGraphicsBindingVulkanKHR const *vulkan =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, XrGraphicsBindingVulkanKHR);
	if (vulkan != NULL) {
		OXR_CHECK_XSYSC(log, sys);

		OXR_VERIFY_ARG_NOT_ZERO(log, vulkan->instance);
		OXR_VERIFY_ARG_NOT_ZERO(log, vulkan->physicalDevice);
		if (vulkan->device == VK_NULL_HANDLE) {
			return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID, "VkDevice must not be VK_NULL_HANDLE");
		}

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called "
			                 "xrGetVulkanGraphicsRequirementsKHR");
		}

		if (sys->suggested_vulkan_physical_device == VK_NULL_HANDLE) {
			char *fn = sys->inst->extensions.KHR_vulkan_enable ? "xrGetVulkanGraphicsDeviceKHR"
			                                                   : "xrGetVulkanGraphicsDevice2KHR";
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Has not called %s", fn);
		}

		if (sys->suggested_vulkan_physical_device != vulkan->physicalDevice) {
			char *fn = sys->inst->extensions.KHR_vulkan_enable ? "xrGetVulkanGraphicsDeviceKHR"
			                                                   : "xrGetVulkanGraphicsDevice2KHR";
			return oxr_error(
			    log, XR_ERROR_VALIDATION_FAILURE,
			    "XrGraphicsBindingVulkanKHR::physicalDevice %p must match device %p specified by %s",
			    (void *)vulkan->physicalDevice, (void *)sys->suggested_vulkan_physical_device, fn);
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_VULKAN, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_vk(log, sys, vulkan, *out_session);
	}
#endif

#ifdef XR_USE_PLATFORM_EGL
	XrGraphicsBindingEGLMNDX const *egl =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo, XR_TYPE_GRAPHICS_BINDING_EGL_MNDX, XrGraphicsBindingEGLMNDX);
	if (egl != NULL) {
		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called "
			                 "xrGetOpenGL[ES]GraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_EGL, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_egl(log, sys, egl, *out_session);
	}
#endif

#ifdef XR_USE_GRAPHICS_API_D3D11
	XrGraphicsBindingD3D11KHR const *d3d11 =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, XrGraphicsBindingD3D11KHR);
	if (d3d11 != NULL) {
		// we know the fields of this struct are OK by now since they were checked with XrSessionCreateInfo

		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called xrGetD3D11GraphicsRequirementsKHR");
		}
		XrResult result = oxr_d3d11_check_device(log, sys, d3d11->device);

		if (!XR_SUCCEEDED(result)) {
			return result;
		}

		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_D3D11, *out_session);

		// Log which compositor path we're taking (use U_LOG_IFL_I to always log)
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D11 session creation - checking compositor options...");

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
		// Check if D3D11 native compositor should be used (bypasses Vulkan)
		if (oxr_d3d11_native_compositor_supported(sys, xsi->external_window_handle)) {
			// Create session without Vulkan compositor
			xrt_result_t xret = xrt_system_create_session(sys->xsys, xsi, &(*out_session)->xs, NULL);
			if (xret == XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED) {
				return oxr_error(log, XR_ERROR_LIMIT_REACHED, "Per instance multi-session not supported.");
			}
			if (xret != XRT_SUCCESS) {
				return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Failed to create xrt_session! '%i'", xret);
			}
			// Use D3D11 native compositor - no Vulkan involvement
			return oxr_session_populate_d3d11_native(log, sys, d3d11, xsi->external_window_handle, *out_session);
		}
#else
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D11 native compositor NOT compiled in (XRT_HAVE_D3D11_NATIVE_COMPOSITOR not defined)");
#endif
		// Fall back to Vulkan-backed D3D11 compositor
		U_LOG_IFL_I(U_LOGGING_INFO, "Using IPC D3D11 client compositor (server-side rendering)");
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_d3d11(log, sys, d3d11, *out_session);
	}
#endif

#ifdef XR_USE_GRAPHICS_API_D3D12
	XrGraphicsBindingD3D12KHR const *d3d12 =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR, XrGraphicsBindingD3D12KHR);
	if (d3d12 != NULL) {
		// we know the fields of this struct are OK by now since they were checked with XrSessionCreateInfo

		OXR_CHECK_XSYSC(log, sys);

		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING,
			                 "Has not called xrGetD3D12GraphicsRequirementsKHR");
		}
		XrResult result = oxr_d3d12_check_device(log, sys, d3d12->device);

		if (!XR_SUCCEEDED(result)) {
			return result;
		}


		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_D3D12, *out_session);
		OXR_CREATE_XRT_SESSION_AND_NATIVE_COMPOSITOR(log, xsi, *out_session);
		return oxr_session_populate_d3d12(log, sys, d3d12, *out_session);
	}
#endif
	/*
	 * Add any new graphics binding structs here - before the headless
	 * check. (order for non-headless checks not specified in standard.)
	 * Any new addition will also need to be added to
	 * oxr_verify_XrSessionCreateInfo and have its own associated verify
	 * function added.
	 */

#ifdef OXR_HAVE_MND_headless
	if (sys->inst->extensions.MND_headless) {
		OXR_SESSION_ALLOCATE_AND_INIT(log, sys, OXR_SESSION_GRAPHICS_EXT_HEADLESS, *out_session);
		(*out_session)->compositor = NULL;
		(*out_session)->create_swapchain = NULL;

		xrt_result_t xret = xrt_system_create_session(sys->xsys, xsi, &(*out_session)->xs, NULL);
		if (xret == XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED) {
			return oxr_error(log, XR_ERROR_LIMIT_REACHED, "Per instance multi-session not supported.");
		}
		if (xret != XRT_SUCCESS) {
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Failed to create xrt_session! '%i'", xret);
		}

		return XR_SUCCESS;
	}
#endif
	return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
	                 "(createInfo->next->type) doesn't contain a valid "
	                 "graphics binding structs");
}

XrResult
oxr_session_create(struct oxr_logger *log,
                   struct oxr_system *sys,
                   const XrSessionCreateInfo *createInfo,
                   struct oxr_session **out_session)
{
	struct oxr_session *sess = NULL;

	struct xrt_session_info xsi = {0};
	const XrSessionCreateInfoOverlayEXTX *overlay_info = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX, XrSessionCreateInfoOverlayEXTX);
	if (overlay_info) {
		xsi.is_overlay = true;
		xsi.flags = overlay_info->createFlags;
		xsi.z_order = overlay_info->sessionLayersPlacement;
	}

#ifdef XRT_OS_WINDOWS
	// Parse XR_EXT_win32_window_binding extension - allows app to provide its own window
	const XrWin32WindowBindingCreateInfoEXT *target_info = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT, XrWin32WindowBindingCreateInfoEXT);
	if (target_info && target_info->windowHandle) {
		xsi.external_window_handle = (void *)target_info->windowHandle;
	}
#endif

#ifdef XRT_OS_MACOS
	// Parse XR_EXT_macos_window_binding extension - allows app to provide its own NSView
	// or offscreen readback (viewHandle=NULL + readbackCallback)
	const XrMacOSWindowBindingCreateInfoEXT *macos_target_info = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT, XrMacOSWindowBindingCreateInfoEXT);
	if (macos_target_info) {
		if (macos_target_info->viewHandle) {
			xsi.external_window_handle = (void *)macos_target_info->viewHandle;
		}
		if (macos_target_info->readbackCallback) {
			xsi.readback_callback = macos_target_info->readbackCallback;
			xsi.readback_userdata = macos_target_info->readbackUserdata;
		}
	}
#endif

	/* Try allocating and populating. */
	XrResult ret = oxr_session_create_impl(log, sys, createInfo, &xsi, &sess);
	if (ret != XR_SUCCESS) {
		if (sess != NULL) {
			/* clean up allocation first */
			XrResult cleanup_result = oxr_handle_destroy(log, &sess->handle);
			assert(cleanup_result == XR_SUCCESS);
			(void)cleanup_result;
		}
		return ret;
	}

	// Track whether this session has an external window handle or offscreen readback
	sess->has_external_window = (xsi.external_window_handle != NULL || xsi.readback_callback != NULL);

	// Tell sim_display driver to return raw eye positions (no qwerty compose)
	if (sess->has_external_window) {
		struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
		if (head != NULL && strcmp(head->str, "Sim 3D Display") == 0) {
			sim_display_hmd_set_ext_app_mode(head, true);
		}
	}

	// Track whether this is an AppContainer app (Chrome WebXR)
	// Used to delay session state transitions for sandbox compatibility
#ifdef OXR_HAVE_EXT_win32_appcontainer_compatible
	sess->is_appcontainer = sys->inst->extensions.EXT_win32_appcontainer_compatible;
#else
	sess->is_appcontainer = false;
#endif

	// Initialize compositor visibility/focus from compositor info (IPC long-term fix)
	// This eliminates the race condition where events must be polled before flags are set
	if (sess->compositor != NULL) {
		sess->compositor_visible = sess->compositor->info.initial_visible;
		sess->compositor_focused = sess->compositor->info.initial_focused;
	}

	// Everything is in order, start the state changes.
	oxr_session_change_state(log, sess, XR_SESSION_STATE_IDLE, 0);
	oxr_session_change_state(log, sess, XR_SESSION_STATE_READY, 0);

	*out_session = sess;

	return ret;
}

void
xrt_to_xr_pose(struct xrt_pose *xrt_pose, XrPosef *xr_pose)
{
	xr_pose->orientation.x = xrt_pose->orientation.x;
	xr_pose->orientation.y = xrt_pose->orientation.y;
	xr_pose->orientation.z = xrt_pose->orientation.z;
	xr_pose->orientation.w = xrt_pose->orientation.w;

	xr_pose->position.x = xrt_pose->position.x;
	xr_pose->position.y = xrt_pose->position.y;
	xr_pose->position.z = xrt_pose->position.z;
}

XrResult
oxr_session_hand_joints(struct oxr_logger *log,
                        struct oxr_hand_tracker *hand_tracker,
                        const XrHandJointsLocateInfoEXT *locateInfo,
                        XrHandJointLocationsEXT *locations)
{
	struct oxr_space *baseSpc = XRT_CAST_OXR_HANDLE_TO_PTR(struct oxr_space *, locateInfo->baseSpace);

	struct oxr_session *sess = hand_tracker->sess;
	struct oxr_instance *inst = sess->sys->inst;

	XrHandJointVelocitiesEXT *vel =
	    OXR_GET_OUTPUT_FROM_CHAIN(locations, XR_TYPE_HAND_JOINT_VELOCITIES_EXT, XrHandJointVelocitiesEXT);

	const XrTime at_time = locateInfo->time;

	//! Convert at_time to monotonic and give to device.
	const int64_t at_timestamp_ns = time_state_ts_to_monotonic_ns(inst->timekeeping, at_time);

	const struct oxr_hand_tracking_data_source *data_sources[ARRAY_SIZE(hand_tracker->requested_sources)] = {0};
	memcpy(data_sources, hand_tracker->requested_sources, sizeof(data_sources));

	if (debug_get_bool_option_hand_tracking_prioritize_conforming() && //
	    hand_tracker->requested_sources_count > 1) {
		const struct oxr_hand_tracking_data_source *tmp = data_sources[0];
		data_sources[0] = data_sources[1];
		data_sources[1] = tmp;
	}

	struct xrt_hand_joint_set value;
	const struct oxr_hand_tracking_data_source *data_source = NULL;
	for (uint32_t i = 0; i < hand_tracker->requested_sources_count; ++i) {
		data_source = data_sources[i];
		if (data_source == NULL || data_source->xdev == NULL)
			continue;
		int64_t ignored;
		value = (struct xrt_hand_joint_set){0};
		xrt_result_t xret = xrt_device_get_hand_tracking(data_source->xdev, data_source->input_name,
		                                                 at_timestamp_ns, &value, &ignored);
		OXR_CHECK_XRET(log, sess, xret, xrt_device_get_hand_tracking);
		if (value.is_active) {
			break;
		}
	}

	if (data_source == NULL || data_source->xdev == NULL) {
		locations->isActive = false;
		return XR_SUCCESS;
	}

#ifdef OXR_HAVE_EXT_hand_tracking_data_source
	XrHandTrackingDataSourceStateEXT *data_source_state = NULL;
	if (hand_tracker->sess->sys->inst->extensions.EXT_hand_tracking_data_source) {
		data_source_state = OXR_GET_OUTPUT_FROM_CHAIN(locations, XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT,
		                                              XrHandTrackingDataSourceStateEXT);
	}

	if (data_source_state != NULL) {
		data_source_state->isActive = XR_TRUE;
		data_source_state->dataSource = xrt_hand_tracking_data_source_to_xr(data_source->input_name);
	}
#endif

	// The hand pose is returned in the xdev's space.
	struct xrt_space_relation T_xdev_hand = value.hand_pose;

	// Get the xdev's pose in the base space.
	struct xrt_space_relation T_base_xdev = XRT_SPACE_RELATION_ZERO;

	XrResult ret = oxr_space_locate_device(log, data_source->xdev, baseSpc, at_time, &T_base_xdev);
	if (ret != XR_SUCCESS) {
		// Error printed logged oxr_space_locate_device
		return ret;
	}
	if (T_base_xdev.relation_flags == 0) {
		locations->isActive = false;
		return XR_SUCCESS;
	}

	// Get the hands pose in the base space.
	struct xrt_space_relation T_base_hand;
	struct xrt_relation_chain xrc = {0};
	m_relation_chain_push_relation(&xrc, &T_xdev_hand);
	m_relation_chain_push_relation(&xrc, &T_base_xdev);
	m_relation_chain_resolve(&xrc, &T_base_hand);

	// Can we not relate to this space or did we not get values?
	if (T_base_hand.relation_flags == 0 || !value.is_active) {
		locations->isActive = false;

		// Loop over all joints and zero flags.
		for (uint32_t i = 0; i < locations->jointCount; i++) {
			locations->jointLocations[i].locationFlags = XRT_SPACE_RELATION_BITMASK_NONE;
			if (vel) {
				XrHandJointVelocityEXT *v = &vel->jointVelocities[i];
				v->velocityFlags = XRT_SPACE_RELATION_BITMASK_NONE;
			}
		}

		return XR_SUCCESS;
	}

	// We know we are active.
	locations->isActive = true;

	for (uint32_t i = 0; i < locations->jointCount; i++) {
		locations->jointLocations[i].locationFlags =
		    xrt_to_xr_space_location_flags(value.values.hand_joint_set_default[i].relation.relation_flags);
		locations->jointLocations[i].radius = value.values.hand_joint_set_default[i].radius;

		struct xrt_space_relation r = value.values.hand_joint_set_default[i].relation;

		struct xrt_space_relation result;
		struct xrt_relation_chain chain = {0};
		m_relation_chain_push_relation(&chain, &r);
		m_relation_chain_push_relation(&chain, &T_base_hand);
		m_relation_chain_resolve(&chain, &result);

		xrt_to_xr_pose(&result.pose, &locations->jointLocations[i].pose);

		if (vel) {
			XrHandJointVelocityEXT *v = &vel->jointVelocities[i];

			v->velocityFlags = 0;
			if ((result.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT)) {
				v->velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
			}
			if ((result.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)) {
				v->velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
			}

			v->linearVelocity.x = result.linear_velocity.x;
			v->linearVelocity.y = result.linear_velocity.y;
			v->linearVelocity.z = result.linear_velocity.z;

			v->angularVelocity.x = result.angular_velocity.x;
			v->angularVelocity.y = result.angular_velocity.y;
			v->angularVelocity.z = result.angular_velocity.z;
		}
	}

	return XR_SUCCESS;
}

/*
 * Gets the body pose in the base space.
 */
XrResult
oxr_get_base_body_pose(struct oxr_logger *log,
                       const struct xrt_body_joint_set *body_joint_set,
                       struct oxr_space *base_spc,
                       struct xrt_device *body_xdev,
                       XrTime at_time,
                       struct xrt_space_relation *out_base_body)
{
	const struct xrt_space_relation space_relation_zero = XRT_SPACE_RELATION_ZERO;
	*out_base_body = space_relation_zero;

	// The body pose is returned in the xdev's space.
	const struct xrt_space_relation *T_xdev_body = &body_joint_set->body_pose;

	// Get the xdev's pose in the base space.
	struct xrt_space_relation T_base_xdev = XRT_SPACE_RELATION_ZERO;

	XrResult ret = oxr_space_locate_device(log, body_xdev, base_spc, at_time, &T_base_xdev);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	if (T_base_xdev.relation_flags == 0) {
		return XR_SUCCESS;
	}

	struct xrt_relation_chain xrc = {0};
	m_relation_chain_push_relation(&xrc, T_xdev_body);
	m_relation_chain_push_relation(&xrc, &T_base_xdev);
	m_relation_chain_resolve(&xrc, out_base_body);

	return XR_SUCCESS;
}

static enum xrt_output_name
xr_hand_to_force_feedback_output(XrHandEXT hand)
{
	switch (hand) {
	case XR_HAND_LEFT_EXT: return XRT_OUTPUT_NAME_FORCE_FEEDBACK_LEFT;
	case XR_HAND_RIGHT_EXT: return XRT_OUTPUT_NAME_FORCE_FEEDBACK_RIGHT;
	default: assert(false); return 0;
	}
}

XrResult
oxr_session_apply_force_feedback(struct oxr_logger *log,
                                 struct oxr_hand_tracker *hand_tracker,
                                 const XrForceFeedbackCurlApplyLocationsMNDX *locations)
{
	struct xrt_output_value result = {0};
	result.type = XRT_OUTPUT_VALUE_TYPE_FORCE_FEEDBACK;
	result.force_feedback.force_feedback_location_count = locations->locationCount;
	for (uint32_t i = 0; i < locations->locationCount; i++) {
		result.force_feedback.force_feedback[i].location =
		    (enum xrt_force_feedback_location)locations->locations[i].location;
		result.force_feedback.force_feedback[i].value = locations->locations[i].value;
	}

	const struct oxr_hand_tracking_data_source *data_sources[2] = {
	    &hand_tracker->unobstructed,
	    &hand_tracker->conforming,
	};
	for (uint32_t i = 0; i < ARRAY_SIZE(data_sources); ++i) {
		struct xrt_device *xdev = data_sources[i]->xdev;
		if (xdev) {
			xrt_result_t xret =
			    xrt_device_set_output(xdev, xr_hand_to_force_feedback_output(hand_tracker->hand), &result);
			if (xret != XRT_SUCCESS) {
				return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "xr_device_set_output failed");
			}
		}
	}

	return XR_SUCCESS;
}

#ifdef OXR_HAVE_KHR_android_thread_settings
static enum xrt_thread_hint
xr_thread_type_to_thread_hint(XrAndroidThreadTypeKHR type)
{
	switch (type) {
	case XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR: return XRT_THREAD_HINT_APPLICATION_MAIN;
	case XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR: return XRT_THREAD_HINT_APPLICATION_WORKER;
	case XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR: return XRT_THREAD_HINT_RENDERER_MAIN;
	case XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR: return XRT_THREAD_HINT_RENDERER_WORKER;
	default: assert(false); return 0;
	}
}

XrResult
oxr_session_android_thread_settings(struct oxr_logger *log,
                                    struct oxr_session *sess,
                                    XrAndroidThreadTypeKHR threadType,
                                    uint32_t threadId)
{
	struct xrt_compositor *xc = &sess->xcn->base;

	if (xc == NULL) {
		return oxr_error(log, XR_ERROR_FUNCTION_UNSUPPORTED,
		                 "Extension XR_KHR_android_thread_settings not be implemented");
	}

	// Convert.
	enum xrt_thread_hint xhint = xr_thread_type_to_thread_hint(threadType);

	// Do the call!
	xrt_result_t xret = xrt_comp_set_thread_hint(xc, xhint, threadId);
	OXR_CHECK_XRET(log, sess, xret, oxr_session_android_thread_settings);

	return XR_SUCCESS;
}
#endif // OXR_HAVE_KHR_android_thread_settings

#ifdef OXR_HAVE_KHR_visibility_mask

static enum xrt_visibility_mask_type
convert_mask_type(XrVisibilityMaskTypeKHR type)
{
	switch (type) {
	case XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR: return XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH;
	case XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR: return XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH;
	case XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR: return XRT_VISIBILITY_MASK_TYPE_LINE_LOOP;
	default: return (enum xrt_visibility_mask_type)0;
	}
}

XrResult
oxr_session_get_visibility_mask(struct oxr_logger *log,
                                struct oxr_session *sess,
                                XrVisibilityMaskTypeKHR visibilityMaskType,
                                uint32_t viewIndex,
                                XrVisibilityMaskKHR *visibilityMask)
{
	struct oxr_system *sys = sess->sys;
	struct xrt_device *xdev = GET_XDEV_BY_ROLE(sess->sys, head);
	enum xrt_visibility_mask_type type = convert_mask_type(visibilityMaskType);
	xrt_result_t xret;

	assert(viewIndex < ARRAY_SIZE(sys->visibility_mask));

	struct xrt_visibility_mask *mask = sys->visibility_mask[viewIndex];

	// Do we need to free the mask.
	if (mask != NULL && mask->type != type) {
		free(mask);
		mask = NULL;
		sys->visibility_mask[viewIndex] = NULL;
	}

	// If we didn't have any cached mask get it.
	if (mask == NULL) {
		xret = xrt_device_get_visibility_mask(xdev, type, viewIndex, &mask);
		if (xret == XRT_ERROR_NOT_IMPLEMENTED && xdev->hmd != NULL) {
			const struct xrt_fov fov = xdev->hmd->distortion.fov[viewIndex];
			u_visibility_mask_get_default(type, &fov, &mask);
			xret = XRT_SUCCESS;
		}
		OXR_CHECK_XRET(log, sess, xret, get_visibility_mask);
		sys->visibility_mask[viewIndex] = mask;
	}

	visibilityMask->vertexCountOutput = mask->vertex_count;
	visibilityMask->indexCountOutput = mask->index_count;

	if (visibilityMask->vertexCapacityInput == 0 || visibilityMask->indexCapacityInput == 0) {
		return XR_SUCCESS;
	}

	if (visibilityMask->vertexCapacityInput < mask->vertex_count) {
		return oxr_error(log, XR_ERROR_SIZE_INSUFFICIENT, "vertexCapacityInput is %u, need %u",
		                 visibilityMask->vertexCapacityInput, mask->vertex_count);
	} else if (visibilityMask->indexCapacityInput < mask->index_count) {
		return oxr_error(log, XR_ERROR_SIZE_INSUFFICIENT, "indexCapacityInput is %u, need %u",
		                 visibilityMask->indexCapacityInput, mask->index_count);
	}

	memcpy(visibilityMask->vertices, xrt_visibility_mask_get_vertices(mask),
	       sizeof(struct xrt_vec2) * mask->vertex_count);
	memcpy(visibilityMask->indices, xrt_visibility_mask_get_indices(mask), sizeof(uint32_t) * mask->index_count);

	return XR_SUCCESS;
}

#endif // OXR_HAVE_KHR_visibility_mask

#ifdef OXR_HAVE_FB_display_refresh_rate
XrResult
oxr_session_get_display_refresh_rate(struct oxr_logger *log, struct oxr_session *sess, float *displayRefreshRate)
{
	struct xrt_compositor *xc = &sess->xcn->base;

	if (xc == NULL) {
		return oxr_session_success_result(sess);
	}

	xrt_result_t xret = xrt_comp_get_display_refresh_rate(xc, displayRefreshRate);
	OXR_CHECK_XRET(log, sess, xret, xrt_comp_get_display_refresh_rate);

	return XR_SUCCESS;
}

XrResult
oxr_session_request_display_refresh_rate(struct oxr_logger *log, struct oxr_session *sess, float displayRefreshRate)
{
	struct xrt_compositor *xc = &sess->xcn->base;

	if (xc == NULL) {
		return oxr_session_success_result(sess);
	}

	xrt_result_t xret = xrt_comp_request_display_refresh_rate(xc, displayRefreshRate);
	OXR_CHECK_XRET(log, sess, xret, xrt_comp_request_display_refresh_rate);

	return XR_SUCCESS;
}
#endif // OXR_HAVE_FB_display_refresh_rate

#ifdef OXR_HAVE_EXT_performance_settings
XrResult
oxr_session_set_perf_level(struct oxr_logger *log,
                           struct oxr_session *sess,
                           XrPerfSettingsDomainEXT domain,
                           XrPerfSettingsLevelEXT level)
{
	struct xrt_compositor *xc = &sess->xcn->base;

	if (xc->set_performance_level == NULL) {
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	}
	enum xrt_perf_domain oxr_domain = xr_perf_domain_to_xrt(domain);
	enum xrt_perf_set_level oxr_level = xr_perf_level_to_xrt(level);
	xrt_comp_set_performance_level(xc, oxr_domain, oxr_level);

	return XR_SUCCESS;
}
#endif // OXR_HAVE_EXT_performance_settings
