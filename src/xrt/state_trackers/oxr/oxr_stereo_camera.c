// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera API entry points (ADR-043).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * Instance-level: a capture component has no session. Every call goes through
 * the instance's @ref xrt_stereo_camera_client aspect, which only the IPC
 * client instance carries — an in-process instance enumerates zero cameras
 * (the service is each camera's single owner; maintainer decision for R1,
 * roadmap §G.3).
 *
 * Error contract:
 *  - dead pipe (XRT_ERROR_IPC_FAILURE)        -> XR_ERROR_INSTANCE_LOST
 *  - OS camera switch / class (XRT_ERROR_NOT_AUTHORIZED) -> XR_ERROR_PERMISSION_INSUFFICIENT
 *  - user / stored consent refused            -> XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR
 *    (retryable: the stream stays created; a browser maps it to NotAllowedError)
 *  - sharing off (toggle / kill switch)       -> XR_ERROR_STEREO_CAMERA_DISABLED_DXR
 *  - source cannot be opened right now        -> XR_ERROR_STEREO_CAMERA_BUSY_DXR
 *  - the service ended this stream            -> XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR
 *  - no such camera / bad format / output      -> XR_ERROR_VALIDATION_FAILURE
 *  - transport or output the runtime lacks     -> XR_ERROR_FEATURE_UNSUPPORTED
 *  - 8 streams on the camera already           -> XR_ERROR_LIMIT_REACHED
 *  - anything else on a healthy pipe           -> XR_ERROR_RUNTIME_FAILURE
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"
#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_chain.h"

#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_trace_marker.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_stereo_camera.h"

#include "xrt/xrt_config_os.h"

#include <string.h>
#if defined(XRT_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef OXR_HAVE_DXR_stereo_camera

// The wire carries the xrt encodings; they equal the XR ones. The XR flag bits
// are `static const XrFlags64` — not constant expressions in C (MSVC and GCC
// reject them in _Static_assert; clang only accepts them as an extension) — so
// the flag / format-bit mirrors are pinned against the header's literal values
// here and cross-checked against the real constants in verify_encodings().
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING == 0x1, "flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_USER_FACING == 0x2, "flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_CALIBRATED == 0x4, "flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED == 0x8, "flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_MONOCHROME == 0x10, "flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE == 0x20, "flag mirror");
// ADR-061: XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR (XR_DXR_app_share.h), reserved by the stereo camera header.
_Static_assert(XRT_STEREO_CAMERA_APP_CONTENT == 0x40, "flag mirror");
_Static_assert(XRT_APP_SHARE_EXECUTABLE_NAME_SIZE == XR_APP_SHARE_EXECUTABLE_NAME_MAX_SIZE_DXR, "exe name size");
_Static_assert(XRT_APP_SHARE_APPLICATION_NAME_SIZE == XR_MAX_APPLICATION_NAME_SIZE, "app name size");
_Static_assert((XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE & XRT_STEREO_CAMERA_CLIENT_PUBLIC_MASK) == 0,
               "the internal app-share client flag must never alias a public XrStereoCameraClientFlagsDXR bit");
// The enum mirrors are pinned the same way, each side against the literal on
// its own: a direct XR-vs-xrt comparison is "operands are different enum types"
// (MSVC C5287 at /W4), and MSVC's C front end raises it even through (int)
// casts. Two enum-vs-literal checks carry the same proof with no cross-enum op.
#define SCAM_PIN(xr, xrt, v) _Static_assert((xr) == (v) && (xrt) == (v), #xr " != " #xrt)
SCAM_PIN(XR_STEREO_CAMERA_STATE_AVAILABLE_DXR, XRT_STEREO_CAMERA_STATE_AVAILABLE, 1);
SCAM_PIN(XR_STEREO_CAMERA_STATE_WAITING_DXR, XRT_STEREO_CAMERA_STATE_WAITING, 2);
SCAM_PIN(XR_STEREO_CAMERA_STATE_SUSPENDED_DXR, XRT_STEREO_CAMERA_STATE_SUSPENDED, 3);
SCAM_PIN(XR_STEREO_CAMERA_STATE_UNAVAILABLE_DXR, XRT_STEREO_CAMERA_STATE_UNAVAILABLE, 4);
SCAM_PIN(XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, 1);
SCAM_PIN(XR_STEREO_CAMERA_OUTPUT_RAW_DXR, XRT_STEREO_CAMERA_OUTPUT_RAW, 2);
SCAM_PIN(XR_STEREO_CAMERA_FORMAT_GRAY8_DXR, XRT_STEREO_CAMERA_FORMAT_GRAY8, 1);
SCAM_PIN(XR_STEREO_CAMERA_FORMAT_NV12_DXR, XRT_STEREO_CAMERA_FORMAT_NV12, 2);
SCAM_PIN(XR_STEREO_CAMERA_FORMAT_BGRA8_DXR, XRT_STEREO_CAMERA_FORMAT_BGRA8, 3);
SCAM_PIN(XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR, XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 1);
#undef SCAM_PIN
_Static_assert(XR_STEREO_CAMERA_RING_SLOTS_DXR == XRT_STEREO_CAMERA_RING_SLOTS, "ring slots");
_Static_assert(XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR == XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN,
               "frame orientation unknown");
_Static_assert(XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR == 0xFFFFFFFFu, "frame orientation unknown literal");
_Static_assert(XR_TYPE_STEREO_CAMERA_ORIENTATION_DXR == 1004999318, "orientation struct type");
// Spec v8 — the wire carries the xrt encodings; they equal the XR ones.
_Static_assert(XR_TYPE_STEREO_CAMERA_MODE_DXR == 1004999319, "v8 type");
_Static_assert(XR_TYPE_STEREO_CAMERA_STREAM_MODE_INFO_DXR == 1004999460, "v8 type");
_Static_assert(XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR == -1004999468, "v8 result");
_Static_assert(XRT_STEREO_CAMERA_MODE_DEFAULT == 0x1 && XRT_STEREO_CAMERA_MODE_STREAM == 0x2 &&
                   XRT_STEREO_CAMERA_MODE_STILL == 0x4,
               "mode flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE == 0x1 &&
                   XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE == 0x2 &&
                   XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT == 0x4 &&
                   XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION == 0x8,
               "control flag mirror");
_Static_assert(XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_DISTANCE == 0x1 &&
                   XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_STATE == 0x2 &&
                   XRT_PLUGIN_STEREO_CAMERA_RESULT_EXPOSURE_TIME == 0x4 &&
                   XRT_PLUGIN_STEREO_CAMERA_RESULT_SENSITIVITY == 0x8 &&
                   XRT_PLUGIN_STEREO_CAMERA_RESULT_EYE_TIMES == 0x10,
               "capture result flag mirror");
#define SCAM_PIN8(xr, xrt, v) _Static_assert((xr) == (v) && (xrt) == (v), #xr " != " #xrt)
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_CONTINUOUS_AUTO, 1);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_MODE_AUTO_ONCE_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE, 2);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_MODE_MANUAL_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL, 3);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_MODE_LOCKED_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_LOCKED, 4);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_STATE_SEARCHING_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_SEARCHING, 1);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_STATE_LOCKED_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_LOCKED, 2);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_STATE_FIXED_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_FIXED, 3);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_STATE_FAILED_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_FAILED, 4);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_CALIBRATION_APPROXIMATE_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_APPROXIMATE, 1);
SCAM_PIN8(XR_STEREO_CAMERA_FOCUS_CALIBRATION_CALIBRATED_DXR, XRT_PLUGIN_STEREO_CAMERA_FOCUS_CALIBRATED, 2);
#undef SCAM_PIN8
_Static_assert(XR_STEREO_CAMERA_PERSISTENT_ID_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->persistent_id),
               "id size");
_Static_assert(XR_STEREO_CAMERA_DISPLAY_NAME_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->display_name),
               "name size");
_Static_assert(XR_STEREO_CAMERA_PLATFORM_HINT_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->platform_device_hint),
               "hint size");

//! Runtime half of the encoding check (see the _Static_assert block above).
static bool
verify_encodings(void)
{
	return XR_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING &&
	       XR_STEREO_CAMERA_USER_FACING_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_USER_FACING &&
	       XR_STEREO_CAMERA_CALIBRATED_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_CALIBRATED &&
	       XR_STEREO_CAMERA_NATIVELY_RECTIFIED_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED &&
	       XR_STEREO_CAMERA_MONOCHROME_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_MONOCHROME &&
	       XR_STEREO_CAMERA_RECTIFIABLE_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE &&
	       XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR == XRT_STEREO_CAMERA_APP_CONTENT &&
	       XR_STEREO_CAMERA_FORMAT_GRAY8_BIT_DXR == XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_GRAY8) &&
	       XR_STEREO_CAMERA_FORMAT_NV12_BIT_DXR == XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12) &&
	       XR_STEREO_CAMERA_FORMAT_BGRA8_BIT_DXR == XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_BGRA8) &&
	       XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_BIT_DXR ==
	           XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY) &&
	       // Spec v8.
	       XR_STEREO_CAMERA_MODE_DEFAULT_BIT_DXR == XRT_STEREO_CAMERA_MODE_DEFAULT &&
	       XR_STEREO_CAMERA_MODE_STREAM_BIT_DXR == XRT_STEREO_CAMERA_MODE_STREAM &&
	       XR_STEREO_CAMERA_MODE_STILL_BIT_DXR == XRT_STEREO_CAMERA_MODE_STILL &&
	       XR_STEREO_CAMERA_CONTROL_FOCUS_MODE_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE &&
	       XR_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE &&
	       XR_STEREO_CAMERA_CONTROL_METERING_POINT_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT &&
	       XR_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION_BIT_DXR ==
	           XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION &&
	       XR_STEREO_CAMERA_FOCUS_MODE_AUTO_ONCE_BIT_DXR == (1u << XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE) &&
	       XR_STEREO_CAMERA_CAPTURE_RESULT_FOCUS_DISTANCE_BIT_DXR ==
	           XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_DISTANCE &&
	       XR_STEREO_CAMERA_CAPTURE_RESULT_EYE_CAPTURE_TIMES_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_RESULT_EYE_TIMES;
}

static struct xrt_stereo_camera_client *
client_of(struct oxr_instance *inst)
{
	return inst->xinst != NULL ? inst->xinst->stereo_camera : NULL;
}

static XrResult
scam_xret(struct oxr_logger *log, xrt_result_t xret, const char *what)
{
	switch (xret) {
	case XRT_SUCCESS: return XR_SUCCESS;
	case XRT_ERROR_IPC_FAILURE:
		return oxr_error(log, XR_ERROR_INSTANCE_LOST, "%s: the runtime service connection is gone", what);
	case XRT_ERROR_NOT_AUTHORIZED:
		return oxr_error(log, XR_ERROR_PERMISSION_INSUFFICIENT,
		                 "%s: the runtime refused camera access (OS camera privacy switch, or this client class "
		                 "may not use the camera; see XR_DXR_stereo_camera §7)",
		                 what);
	case XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED:
		return oxr_error(log, XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR,
		                 "%s: the user (or the stored decision) refused this app the 3D camera; retry after "
		                 "consent (XR_DXR_stereo_camera §7.1)",
		                 what);
	case XRT_ERROR_STEREO_CAMERA_DISABLED:
		return oxr_error(log, XR_ERROR_STEREO_CAMERA_DISABLED_DXR,
		                 "%s: 3D camera sharing is switched off (XR_DXR_stereo_camera §7.4)", what);
	case XRT_ERROR_STEREO_CAMERA_BUSY:
		return oxr_error(log, XR_ERROR_STEREO_CAMERA_BUSY_DXR,
		                 "%s: the camera source cannot be opened right now; retry later", what);
	case XRT_ERROR_STEREO_CAMERA_STREAM_ENDED:
		return oxr_error(log, XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR,
		                 "%s: the service ended this stream (user stop / sharing off / source lost); destroy it "
		                 "and create a new one",
		                 what);
	case XRT_ERROR_INPUT_UNSUPPORTED:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: unknown camera/stream or unsupported request",
		                 what);
	case XRT_ERROR_FEATURE_NOT_SUPPORTED:
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: not supported by this runtime / camera", what);
	case XRT_ERROR_CLIENT_LIMIT_REACHED:
		return oxr_error(log, XR_ERROR_LIMIT_REACHED,
		                 "%s: limit reached (8 streams per camera; one still per stream at a time)", what);
	case XRT_ERROR_STEREO_CAMERA_CONTROL_HELD:
		return oxr_error(log, XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR,
		                 "%s: another stream holds this camera's controls; retry after it releases them", what);
	case XRT_ERROR_CALL_ORDER_INVALID:
		return oxr_error(log, XR_ERROR_CALL_ORDER_INVALID,
		                 "%s: the stream is not started, or the still is not ready yet", what);
	default:
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: service refused (xrt_result=%d)", what, (int)xret);
	}
}

//! The wire calibration into the app's struct (its `next` chain untouched).
static void
fill_xr_calibration(const struct xrt_stereo_camera_calibration *k, XrStereoCameraCalibrationDXR *calibration)
{
	for (int e = 0; e < 2; e++) {
		XrStereoCameraIntrinsicsDXR *o = &calibration->eye[e];
		o->imageExtent.width = (int32_t)k->eye[e].width;
		o->imageExtent.height = (int32_t)k->eye[e].height;
		o->fx = k->eye[e].fx;
		o->fy = k->eye[e].fy;
		o->cx = k->eye[e].cx;
		o->cy = k->eye[e].cy;
		o->model = (XrStereoCameraDistortionModelDXR)k->eye[e].model;
		memcpy(o->coefficients, k->eye[e].coefficients, sizeof(o->coefficients));
	}
	calibration->rightFromLeft.orientation.x = k->orientation[0];
	calibration->rightFromLeft.orientation.y = k->orientation[1];
	calibration->rightFromLeft.orientation.z = k->orientation[2];
	calibration->rightFromLeft.orientation.w = k->orientation[3];
	calibration->rightFromLeft.position.x = k->position[0];
	calibration->rightFromLeft.position.y = k->position[1];
	calibration->rightFromLeft.position.z = k->position[2];
	calibration->baselineMm = k->baseline_mm;
}

//! Spec v8: the wire capture state into the app's struct (times converted to XrTime).
static void
fill_xr_capture_result(struct oxr_instance *inst,
                       const struct xrt_stereo_camera_capture_result *r,
                       XrStereoCameraCaptureResultDXR *out)
{
	out->validFields = r->valid_fields;
	out->focusDistanceDiopters = r->focus_distance_diopters;
	out->focusState = (XrStereoCameraFocusStateDXR)r->focus_state;
	out->exposureTime = (XrDuration)r->exposure_time_ns;
	out->sensitivityIso = r->sensitivity_iso;
	for (int e = 0; e < 2; e++) {
		out->eyeCaptureTime[e] =
		    r->eye_time_ns[e] > 0 ? time_state_monotonic_to_ts_ns(inst->timekeeping, r->eye_time_ns[e]) : 0;
	}
}

static XrResult
stream_destroy_cb(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_stereo_camera_stream_dxr *s = (struct oxr_stereo_camera_stream_dxr *)hb;
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c != NULL && s->id != 0) {
		// Best effort: a dead connection already released every stream.
		(void)c->stream_destroy(c, s->id);
	}
	free(s);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateStereoCamerasDXR(XrInstance instance,
                                XrSystemId systemId,
                                uint32_t capacityInput,
                                uint32_t *countOutput,
                                XrStereoCameraPropertiesDXR *cameras)
{
	OXR_TRACE_MARKER();
	struct oxr_instance *inst = NULL;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrEnumerateStereoCamerasDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_stereo_camera);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);
	XrResult sr = oxr_system_verify_id(&log, inst, systemId);
	if (sr != XR_SUCCESS) {
		return sr;
	}
	if (capacityInput > 0 && cameras == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(cameras == NULL) with capacityInput > 0");
	}

	*countOutput = 0;
	struct xrt_stereo_camera_client *c = client_of(inst);
	if (c == NULL) {
		return XR_SUCCESS; // in-process instance: zero cameras, on purpose
	}
	if (!verify_encodings()) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "XR_DXR_stereo_camera encoding mismatch (build bug)");
	}
	uint32_t n = 0;
	xrt_result_t xret = c->count(c, &n);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrEnumerateStereoCamerasDXR");
	}
	*countOutput = n;
	if (capacityInput == 0) {
		return XR_SUCCESS;
	}
	if (capacityInput < n) {
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "(capacityInput == %u) < %u cameras", capacityInput,
		                 n);
	}
	for (uint32_t i = 0; i < n; i++) {
		if (cameras[i].type != XR_TYPE_STEREO_CAMERA_PROPERTIES_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "cameras[%u].type", i);
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		struct xrt_stereo_camera_properties p;
		xret = c->get_properties(c, i, &p);
		if (xret != XRT_SUCCESS) {
			return scam_xret(&log, xret, "xrEnumerateStereoCamerasDXR");
		}
		XrStereoCameraPropertiesDXR *o = &cameras[i];
		o->cameraId = p.camera_id;
		memcpy(o->persistentId, p.persistent_id, sizeof(o->persistentId));
		memcpy(o->displayName, p.display_name, sizeof(o->displayName));
		memcpy(o->platformDeviceHint, p.platform_device_hint, sizeof(o->platformDeviceHint));
		o->flags = p.flags;
		o->state = (XrStereoCameraStateDXR)p.state;
		o->viewCount = p.view_count;
		o->eyeExtent.width = (int32_t)p.eye_width;
		o->eyeExtent.height = (int32_t)p.eye_height;
		o->maxFrameRate = p.max_frame_rate;
		o->baselineMm = p.baseline_mm;
		o->horizontalFovDeg = p.horizontal_fov_deg;
		o->supportedFormats = p.supported_formats;
		o->supportedTransports = p.supported_transports;
		// Spec v5: the camera's display, when the app chained the struct.
		XrStereoCameraDisplayDXR *disp =
		    OXR_GET_OUTPUT_FROM_CHAIN(o, XR_TYPE_STEREO_CAMERA_DISPLAY_DXR, XrStereoCameraDisplayDXR);
		if (disp != NULL) {
			disp->displayId = p.display_id;
		}
		// Spec v7: how the published frame is turned, when the app chained it.
		XrStereoCameraOrientationDXR *orient =
		    OXR_GET_OUTPUT_FROM_CHAIN(o, XR_TYPE_STEREO_CAMERA_ORIENTATION_DXR, XrStereoCameraOrientationDXR);
		if (orient != NULL) {
			orient->frameOrientationDegrees = p.frame_orientation_deg;
		}
		// Spec v8: the camera's controls, when the app chained the struct.
		XrStereoCameraControlCapabilitiesDXR *caps = OXR_GET_OUTPUT_FROM_CHAIN(
		    o, XR_TYPE_STEREO_CAMERA_CONTROL_CAPABILITIES_DXR, XrStereoCameraControlCapabilitiesDXR);
		if (caps != NULL && c->get_control_caps != NULL) {
			struct xrt_stereo_camera_control_caps k;
			xret = c->get_control_caps(c, p.camera_id, &k);
			if (xret == XRT_ERROR_IPC_FAILURE) {
				return scam_xret(&log, xret, "xrEnumerateStereoCamerasDXR");
			}
			if (xret != XRT_SUCCESS) {
				memset(&k, 0, sizeof(k)); // gone meanwhile: no controls
			}
			caps->supportedControls = k.supported_controls;
			caps->supportedFocusModes = k.supported_focus_modes;
			caps->minFocusDistanceDiopters = k.min_focus_distance_diopters;
			caps->focusDistanceCalibration =
			    (XrStereoCameraFocusCalibrationDXR)k.focus_distance_calibration;
			caps->exposureCompensationMinEv = k.exposure_compensation_min_ev;
			caps->exposureCompensationMaxEv = k.exposure_compensation_max_ev;
			caps->exposureCompensationStepEv = k.exposure_compensation_step_ev;
		}
#ifdef OXR_HAVE_DXR_app_share
		// ADR-061: an app entry's description, when the app chained it (spec §2:
		// filled only for an APP_CONTENT entry; test the flag, not the struct).
		XrAppShareSourcePropertiesDXR *ap = OXR_GET_OUTPUT_FROM_CHAIN(
		    o, XR_TYPE_APP_SHARE_SOURCE_PROPERTIES_DXR, XrAppShareSourcePropertiesDXR);
		if (ap != NULL && inst->extensions.DXR_app_share && (p.flags & XRT_STEREO_CAMERA_APP_CONTENT) != 0 &&
		    c->get_app_source != NULL) {
			struct xrt_app_share_source_info ai;
			xret = c->get_app_source(c, p.camera_id, &ai);
			if (xret == XRT_ERROR_IPC_FAILURE) {
				return scam_xret(&log, xret, "xrEnumerateStereoCamerasDXR");
			}
			if (xret == XRT_SUCCESS) {
				ap->processId = ai.pid;
				memcpy(ap->executableName, ai.executable_name, sizeof(ap->executableName));
				ap->executableName[sizeof(ap->executableName) - 1] = '\0';
				memcpy(ap->applicationName, ai.application_name, sizeof(ap->applicationName));
				ap->applicationName[sizeof(ap->applicationName) - 1] = '\0';
				ap->windowRect.offset.x = ai.window_x;
				ap->windowRect.offset.y = ai.window_y;
				ap->windowRect.extent.width = (int32_t)ai.window_width;
				ap->windowRect.extent.height = (int32_t)ai.window_height;
				ap->contentEyeExtent.width = (int32_t)ai.content_eye_width;
				ap->contentEyeExtent.height = (int32_t)ai.content_eye_height;
				ap->viewCount = ai.view_count;
				ap->activeViewCount = ai.active_view_count;
				ap->zeroDisparityShiftFrac = ai.zero_disparity_shift_frac;
				ap->encodingIsLinear = ai.encoding_is_linear ? XR_TRUE : XR_FALSE;
			}
			// Gone between count and here: the struct stays as the app passed it.
		}
#endif
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetStereoCameraCalibrationDXR(XrInstance instance, uint64_t cameraId, XrStereoCameraCalibrationDXR *calibration)
{
	OXR_TRACE_MARKER();
	struct oxr_instance *inst = NULL;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrGetStereoCameraCalibrationDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_stereo_camera);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, calibration, XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR);
	if (calibration->output != XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR &&
	    calibration->output != XR_STEREO_CAMERA_OUTPUT_RAW_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "calibration->output");
	}
	struct xrt_stereo_camera_client *c = client_of(inst);
	if (c == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "no stereo camera %llu (in-process instance)",
		                 (unsigned long long)cameraId);
	}
	// Spec v8 §5.5: the numbers of frames of a mode, when the app chained one.
	const XrStereoCameraStreamModeInfoDXR *mode = OXR_GET_INPUT_FROM_CHAIN(
	    calibration, XR_TYPE_STEREO_CAMERA_STREAM_MODE_INFO_DXR, XrStereoCameraStreamModeInfoDXR);
	uint32_t ew = 0, eh = 0;
	if (mode != NULL) {
		if (mode->eyeExtent.width <= 0 || mode->eyeExtent.height <= 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrStereoCameraStreamModeInfoDXR::eyeExtent");
		}
		ew = (uint32_t)mode->eyeExtent.width;
		eh = (uint32_t)mode->eyeExtent.height;
	}
	struct xrt_stereo_camera_calibration k;
	xrt_result_t xret = c->get_calibration(c, cameraId, (uint32_t)calibration->output, ew, eh, &k);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrGetStereoCameraCalibrationDXR");
	}
	fill_xr_calibration(&k, calibration);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateStereoCameraStreamDXR(XrInstance instance,
                                  const XrStereoCameraStreamCreateInfoDXR *createInfo,
                                  XrStereoCameraStreamDXR *stream)
{
	OXR_TRACE_MARKER();
	struct oxr_instance *inst = NULL;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrCreateStereoCameraStreamDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_stereo_camera);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, createInfo, XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, stream);

	if (createInfo->format < XR_STEREO_CAMERA_FORMAT_GRAY8_DXR ||
	    createInfo->format > XR_STEREO_CAMERA_FORMAT_BGRA8_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "createInfo->format");
	}
	if (createInfo->output != XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR &&
	    createInfo->output != XR_STEREO_CAMERA_OUTPUT_RAW_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "createInfo->output");
	}
	if (createInfo->transport != XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "createInfo->transport: only SHARED_MEMORY is implemented (spec v1)");
	}
	struct xrt_stereo_camera_client *c = client_of(inst);
	if (c == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "no stereo camera %llu (in-process instance)",
		                 (unsigned long long)createInfo->cameraId);
	}

	struct xrt_stereo_camera_stream_request req = {
	    .camera_id = createInfo->cameraId,
	    .output = (uint32_t)createInfo->output,
	    .format = (uint32_t)createInfo->format,
	    .transport = (uint32_t)createInfo->transport,
	    .max_frame_rate = createInfo->maxFrameRate,
	};
	// Spec v8 §5.5: the stream's mode; absent = the default (v7 unchanged).
	const XrStereoCameraStreamModeInfoDXR *mode = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_STEREO_CAMERA_STREAM_MODE_INFO_DXR, XrStereoCameraStreamModeInfoDXR);
	if (mode != NULL) {
		if (mode->eyeExtent.width <= 0 || mode->eyeExtent.height <= 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrStereoCameraStreamModeInfoDXR::eyeExtent");
		}
		req.eye_width = (uint32_t)mode->eyeExtent.width;
		req.eye_height = (uint32_t)mode->eyeExtent.height;
	}
	uint64_t id = 0;
	xrt_result_t xret = c->stream_create(c, &req, &id);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrCreateStereoCameraStreamDXR");
	}

	struct oxr_stereo_camera_stream_dxr *s = NULL;
	XrResult ret = OXR_ALLOCATE_HANDLE(&log, s, OXR_XR_DEBUG_STEREOCAMSTREAM, stream_destroy_cb, &inst->handle);
	if (ret != XR_SUCCESS) {
		(void)c->stream_destroy(c, id);
		return ret;
	}
	s->inst = inst;
	s->id = id;
	s->camera_id = createInfo->cameraId;
	*stream = XRT_CAST_PTR_TO_OXR_HANDLE(XrStereoCameraStreamDXR, s);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyStereoCameraStreamDXR(XrStereoCameraStreamDXR stream)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrDestroyStereoCameraStreamDXR");
	return oxr_handle_destroy(&log, &s->handle);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStartStereoCameraStreamDXR(XrStereoCameraStreamDXR stream)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrStartStereoCameraStreamDXR");
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	xrt_result_t xret = c->stream_start(c, s->id);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrStartStereoCameraStreamDXR");
	}
	s->started = true;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStopStereoCameraStreamDXR(XrStereoCameraStreamDXR stream)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrStopStereoCameraStreamDXR");
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	xrt_result_t xret = c->stream_stop(c, s->id);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrStopStereoCameraStreamDXR");
	}
	s->started = false;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetStereoCameraStreamInfoDXR(XrStereoCameraStreamDXR stream, XrStereoCameraStreamInfoDXR *info)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrGetStereoCameraStreamInfoDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_STEREO_CAMERA_STREAM_INFO_DXR);
	if (!s->started) {
		return oxr_error(&log, XR_ERROR_CALL_ORDER_INVALID, "the stream has not been started");
	}
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	struct xrt_stereo_camera_stream_layout lay;
	xrt_shmem_handle_t section = XRT_SHMEM_HANDLE_INVALID;
	xrt_graphics_sync_handle_t wake = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xrt_result_t xret = c->stream_get_transport(c, s->id, &lay, &section, &wake);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrGetStereoCameraStreamInfoDXR");
	}
	info->extent.width = (int32_t)lay.width;
	info->extent.height = (int32_t)lay.height;
	info->format = (XrStereoCameraFormatDXR)lay.format;
	info->output = (XrStereoCameraOutputDXR)lay.output;
	info->transport = (XrStereoCameraTransportDXR)lay.transport;
	info->maxFrameRate = lay.max_frame_rate;

	XrStereoCameraStreamTransportDXR *t = OXR_GET_OUTPUT_FROM_CHAIN(
	    info, XR_TYPE_STEREO_CAMERA_STREAM_TRANSPORT_DXR, XrStereoCameraStreamTransportDXR);
	if (t != NULL) {
		// NEW handles, now the caller's to close.
		t->sectionHandle = (uint64_t)(uintptr_t)section;
		t->sectionSize = lay.section_size;
		t->slotCount = lay.slot_count;
		t->slotStride = lay.slot_stride;
		t->wakeHandle = (uint64_t)(uintptr_t)wake;
	} else {
		// Nobody to hand them to.
#if defined(XRT_OS_WINDOWS)
		if (section != NULL) {
			CloseHandle(section);
		}
		if (wake != NULL) {
			CloseHandle(wake);
		}
#else
		if (section >= 0) {
			close(section);
		}
		if (wake >= 0) {
			close(wake);
		}
#endif
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireStereoCameraFrameDXR(XrStereoCameraStreamDXR stream, XrStereoCameraFrameDXR *frame)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrAcquireStereoCameraFrameDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, frame, XR_TYPE_STEREO_CAMERA_FRAME_DXR);
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	bool ready = false;
	struct xrt_stereo_camera_frame_info fi;
	struct xrt_app_share_frame_info app;
	struct xrt_stereo_camera_capture_result res;
	xrt_result_t xret = c->acquire(c, s->id, &ready, &fi, &app, &res);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrAcquireStereoCameraFrameDXR");
	}
	if (!ready) {
		return XR_STEREO_CAMERA_FRAME_NOT_READY_DXR;
	}
	frame->frameIndex = fi.frame_index;
	frame->slot = fi.slot;
	frame->extent.width = (int32_t)fi.width;
	frame->extent.height = (int32_t)fi.height;
	frame->format = (XrStereoCameraFormatDXR)fi.format;
	frame->rowPitch[0] = fi.row_pitch[0];
	frame->rowPitch[1] = fi.row_pitch[1];
	frame->planeOffset[0] = fi.plane_offset[0];
	frame->planeOffset[1] = fi.plane_offset[1];
	frame->captureTime = time_state_monotonic_to_ts_ns(s->inst->timekeeping, fi.capture_time_ns);
	frame->captureTimeIsExposure = fi.time_is_exposure ? XR_TRUE : XR_FALSE;
	frame->output = (XrStereoCameraOutputDXR)fi.output;
	frame->calibrationGeneration = fi.calibration_generation;
	// Spec v8 §5.6: the frame's capture state, when the app chained it.
	XrStereoCameraCaptureResultDXR *cr =
	    OXR_GET_OUTPUT_FROM_CHAIN(frame, XR_TYPE_STEREO_CAMERA_CAPTURE_RESULT_DXR, XrStereoCameraCaptureResultDXR);
	if (cr != NULL) {
		fill_xr_capture_result(s->inst, &res, cr);
	}
#ifdef OXR_HAVE_DXR_app_share
	// ADR-061 spec §4: per-frame app metadata on an APP_CONTENT stream.
	XrAppShareFrameInfoDXR *afi =
	    OXR_GET_OUTPUT_FROM_CHAIN(frame, XR_TYPE_APP_SHARE_FRAME_INFO_DXR, XrAppShareFrameInfoDXR);
	if (afi != NULL && s->inst->extensions.DXR_app_share && app.valid != 0) {
		afi->contentRect.offset.x = app.content_x;
		afi->contentRect.offset.y = app.content_y;
		afi->contentRect.extent.width = (int32_t)app.content_width;
		afi->contentRect.extent.height = (int32_t)app.content_height;
		afi->mono = app.mono ? XR_TRUE : XR_FALSE;
		afi->contentGeneration = app.content_generation;
		afi->zeroDisparityShiftFrac = app.zero_disparity_shift_frac;
		afi->sourceCommitTime = app.commit_time_ns > 0
		                            ? time_state_monotonic_to_ts_ns(s->inst->timekeeping, app.commit_time_ns)
		                            : 0;
	}
#endif
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetStereoCameraStreamStatsDXR(XrStereoCameraStreamDXR stream, XrStereoCameraStreamStatsDXR *stats)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrGetStereoCameraStreamStatsDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, stats, XR_TYPE_STEREO_CAMERA_STREAM_STATS_DXR);
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	struct xrt_stereo_camera_stream_stats st;
	xrt_result_t xret = c->stats(c, s->id, &st);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrGetStereoCameraStreamStatsDXR");
	}
	stats->sourceFrameRate = st.source_frame_rate;
	stats->deliveredFrameRate = st.delivered_frame_rate;
	stats->framesPublished = st.frames_published;
	stats->framesSkipped = st.frames_skipped;
	stats->framesAcquired = st.frames_acquired;
	stats->meanLatencyNs = st.mean_latency_ns;
	return XR_SUCCESS;
}


/*
 *
 * Spec v8: modes, controls, stills (§5.5 - §5.7).
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateStereoCameraModesDXR(
    XrInstance instance, uint64_t cameraId, uint32_t capacityInput, uint32_t *countOutput, XrStereoCameraModeDXR *modes)
{
	OXR_TRACE_MARKER();
	struct oxr_instance *inst = NULL;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrEnumerateStereoCameraModesDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_stereo_camera);
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);
	if (capacityInput > 0 && modes == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(modes == NULL) with capacityInput > 0");
	}
	*countOutput = 0;
	struct xrt_stereo_camera_client *c = client_of(inst);
	if (c == NULL || c->get_modes == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "no stereo camera %llu (in-process instance)",
		                 (unsigned long long)cameraId);
	}
	struct xrt_stereo_camera_mode_list list;
	xrt_result_t xret = c->get_modes(c, cameraId, &list);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrEnumerateStereoCameraModesDXR");
	}
	*countOutput = list.count;
	if (capacityInput == 0) {
		return XR_SUCCESS;
	}
	if (capacityInput < list.count) {
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "(capacityInput == %u) < %u modes", capacityInput,
		                 list.count);
	}
	for (uint32_t i = 0; i < list.count; i++) {
		if (modes[i].type != XR_TYPE_STEREO_CAMERA_MODE_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "modes[%u].type", i);
		}
	}
	for (uint32_t i = 0; i < list.count; i++) {
		modes[i].eyeExtent.width = (int32_t)list.modes[i].eye_width;
		modes[i].eyeExtent.height = (int32_t)list.modes[i].eye_height;
		modes[i].maxFrameRate = list.modes[i].max_frame_rate;
		modes[i].flags = list.modes[i].flags;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetStereoCameraControlsDXR(XrStereoCameraStreamDXR stream, const XrStereoCameraControlsDXR *controls)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrSetStereoCameraControlsDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, controls, XR_TYPE_STEREO_CAMERA_CONTROLS_DXR);
	if ((controls->validControls & ~(XrStereoCameraControlFlagsDXR)0xF) != 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "controls->validControls has unknown bits");
	}
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c->set_controls == NULL) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED, "controls are not supported by this runtime");
	}
	struct xrt_stereo_camera_controls ctl = {
	    .valid_controls = (uint32_t)controls->validControls,
	    .focus_mode = (uint32_t)controls->focusMode,
	    .focus_distance_diopters = controls->focusDistanceDiopters,
	    .metering_x = controls->meteringPoint.x,
	    .metering_y = controls->meteringPoint.y,
	    .exposure_compensation_ev = controls->exposureCompensationEv,
	};
	xrt_result_t xret = c->set_controls(c, s->id, &ctl);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrSetStereoCameraControlsDXR");
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrReleaseStereoCameraControlsDXR(XrStereoCameraStreamDXR stream)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrReleaseStereoCameraControlsDXR");
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c->release_controls == NULL) {
		return XR_SUCCESS;
	}
	xrt_result_t xret = c->release_controls(c, s->id);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrReleaseStereoCameraControlsDXR");
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestStereoCameraStillDXR(XrStereoCameraStreamDXR stream,
                                  const XrStereoCameraStillRequestDXR *request,
                                  uint64_t *requestId)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrRequestStereoCameraStillDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, request, XR_TYPE_STEREO_CAMERA_STILL_REQUEST_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, requestId);
	*requestId = 0;
	if (request->format < XR_STEREO_CAMERA_FORMAT_GRAY8_DXR ||
	    request->format > XR_STEREO_CAMERA_FORMAT_BGRA8_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "request->format");
	}
	if (request->output != XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR &&
	    request->output != XR_STEREO_CAMERA_OUTPUT_RAW_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "request->output");
	}
	if (request->eyeExtent.width < 0 || request->eyeExtent.height < 0 ||
	    ((request->eyeExtent.width == 0) != (request->eyeExtent.height == 0))) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "request->eyeExtent (a STILL size, or {0, 0})");
	}
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c->request_still == NULL) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED, "stills are not supported by this runtime");
	}
	struct xrt_stereo_camera_still_request req = {
	    .eye_width = (uint32_t)request->eyeExtent.width,
	    .eye_height = (uint32_t)request->eyeExtent.height,
	    .output = (uint32_t)request->output,
	    .format = (uint32_t)request->format,
	};
	xrt_result_t xret = c->request_still(c, s->id, &req, requestId);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrRequestStereoCameraStillDXR");
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireStereoCameraStillDXR(XrStereoCameraStreamDXR stream, uint64_t requestId, XrStereoCameraStillDXR *still)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrAcquireStereoCameraStillDXR");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, still, XR_TYPE_STEREO_CAMERA_STILL_DXR);
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c->acquire_still == NULL) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED, "stills are not supported by this runtime");
	}
	struct xrt_stereo_camera_still_info info;
	xrt_shmem_handle_t section = XRT_SHMEM_HANDLE_INVALID;
	xrt_result_t xret = c->acquire_still(c, s->id, requestId, &info, &section);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrAcquireStereoCameraStillDXR");
	}
	still->requestId = info.request_id;
	still->extent.width = (int32_t)info.width;
	still->extent.height = (int32_t)info.height;
	still->format = (XrStereoCameraFormatDXR)info.format;
	still->output = (XrStereoCameraOutputDXR)info.output;
	still->rowPitch[0] = info.row_pitch[0];
	still->rowPitch[1] = info.row_pitch[1];
	still->planeOffset[0] = info.plane_offset[0];
	still->planeOffset[1] = info.plane_offset[1];
	still->sectionHandle = (uint64_t)(uintptr_t)section; // NEW handle, now the caller's to close
	still->sectionSize = info.section_size;
	still->captureTime = time_state_monotonic_to_ts_ns(s->inst->timekeeping, info.capture_time_ns);
	still->captureTimeIsExposure = info.time_is_exposure ? XR_TRUE : XR_FALSE;
	XrStereoCameraCaptureResultDXR *cr =
	    OXR_GET_OUTPUT_FROM_CHAIN(still, XR_TYPE_STEREO_CAMERA_CAPTURE_RESULT_DXR, XrStereoCameraCaptureResultDXR);
	if (cr != NULL) {
		fill_xr_capture_result(s->inst, &info.result, cr);
	}
	XrStereoCameraCalibrationDXR *cal =
	    OXR_GET_OUTPUT_FROM_CHAIN(still, XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR, XrStereoCameraCalibrationDXR);
	if (cal != NULL) {
		cal->output = (XrStereoCameraOutputDXR)info.output; // what these pixels are
		fill_xr_calibration(&info.calibration, cal);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrReleaseStereoCameraStillDXR(XrStereoCameraStreamDXR stream, uint64_t requestId)
{
	OXR_TRACE_MARKER();
	struct oxr_stereo_camera_stream_dxr *s = NULL;
	struct oxr_logger log;
	OXR_VERIFY_STEREO_CAMERA_STREAM_AND_INIT_LOG(&log, stream, s, "xrReleaseStereoCameraStillDXR");
	struct xrt_stereo_camera_client *c = client_of(s->inst);
	if (c->release_still == NULL) {
		return XR_SUCCESS;
	}
	xrt_result_t xret = c->release_still(c, s->id, requestId);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrReleaseStereoCameraStillDXR");
	}
	return XR_SUCCESS;
}


/*
 *
 * R3: events. The service queues them per connection; xrPollEvent drains
 * them here into the instance's event queue, mapping a service stream id
 * back to the handle the app holds.
 *
 */

static struct oxr_stereo_camera_stream_dxr *
stream_by_service_id(struct oxr_instance *inst, uint64_t id)
{
	struct oxr_stereo_camera_stream_dxr *found = NULL;
	os_mutex_lock(&inst->handle.children_mutex);
	for (uint32_t i = 0; i < XRT_MAX_HANDLE_CHILDREN; i++) {
		struct oxr_handle_base *hb = inst->handle.children[i];
		if (hb != NULL && hb->debug == OXR_XR_DEBUG_STEREOCAMSTREAM) {
			struct oxr_stereo_camera_stream_dxr *s = (struct oxr_stereo_camera_stream_dxr *)hb;
			if (s->id == id) {
				found = s;
				break;
			}
		}
	}
	os_mutex_unlock(&inst->handle.children_mutex);
	return found;
}

//! Spec v8 §5.7: why a still failed, as the event's XrResult (no log: it is data).
static XrResult
still_failure_result(int32_t xret)
{
	switch ((xrt_result_t)xret) {
	case XRT_ERROR_NOT_AUTHORIZED: return XR_ERROR_PERMISSION_INSUFFICIENT;
	case XRT_ERROR_STEREO_CAMERA_STREAM_ENDED: return XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR;
	default: return XR_ERROR_STEREO_CAMERA_BUSY_DXR;
	}
}

XrResult
oxr_stereo_camera_poll_events(struct oxr_logger *log, struct oxr_instance *inst)
{
	struct xrt_stereo_camera_client *c = client_of(inst);
	if (c == NULL || c->poll_event == NULL) {
		return XR_SUCCESS;
	}
	for (int i = 0; i < 32; i++) {
		bool has = false;
		struct xrt_stereo_camera_event ev;
		if (c->poll_event(c, &has, &ev) != XRT_SUCCESS || !has) {
			return XR_SUCCESS; // a dead pipe surfaces on the next real call
		}
		switch (ev.kind) {
		case XRT_STEREO_CAMERA_EVENT_CAMERA_STATE:
			oxr_event_push_XrEventDataStereoCameraStateChanged(log, inst, ev.camera_id,
			                                                   (XrStereoCameraStateDXR)ev.value);
			break;
		case XRT_STEREO_CAMERA_EVENT_STREAM_ENDED: {
			struct oxr_stereo_camera_stream_dxr *s = stream_by_service_id(inst, ev.stream_id);
			if (s != NULL) {
				s->started = false;
				oxr_event_push_XrEventDataStereoCameraStreamEnded(
				    log, inst, XRT_CAST_PTR_TO_OXR_HANDLE(XrStereoCameraStreamDXR, s),
				    (XrStereoCameraStreamEndReasonDXR)ev.value);
			}
			break;
		}
		case XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED: oxr_event_push_XrEventDataStereoCamerasChanged(log, inst); break;
		case XRT_STEREO_CAMERA_EVENT_STILL_READY:
		case XRT_STEREO_CAMERA_EVENT_STILL_FAILED: {
			// Spec v8 §5.7.
			struct oxr_stereo_camera_stream_dxr *s = stream_by_service_id(inst, ev.stream_id);
			if (s == NULL) {
				break; // destroyed meanwhile
			}
			XrStereoCameraStreamDXR h = XRT_CAST_PTR_TO_OXR_HANDLE(XrStereoCameraStreamDXR, s);
			if (ev.kind == XRT_STEREO_CAMERA_EVENT_STILL_READY) {
				oxr_event_push_XrEventDataStereoCameraStillReady(log, inst, h, ev.request_id);
			} else {
				oxr_event_push_XrEventDataStereoCameraStillFailed(log, inst, h, ev.request_id,
				                                                  still_failure_result(ev.result));
			}
			break;
		}
		default: break;
		}
	}
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_stereo_camera
