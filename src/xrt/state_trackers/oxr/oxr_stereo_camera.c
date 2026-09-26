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
 *  - refused by consent (XRT_ERROR_NOT_AUTHORIZED) -> XR_ERROR_PERMISSION_INSUFFICIENT
 *    (retryable: the stream stays created)
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

// The wire carries the xrt encodings; they equal the XR ones.
_Static_assert(XR_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING,
               "flag mismatch");
_Static_assert(XR_STEREO_CAMERA_USER_FACING_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_USER_FACING, "flag mismatch");
_Static_assert(XR_STEREO_CAMERA_CALIBRATED_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_CALIBRATED, "flag mismatch");
_Static_assert(XR_STEREO_CAMERA_NATIVELY_RECTIFIED_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED,
               "flag mismatch");
_Static_assert(XR_STEREO_CAMERA_MONOCHROME_BIT_DXR == XRT_PLUGIN_STEREO_CAMERA_MONOCHROME, "flag mismatch");
_Static_assert((int)XR_STEREO_CAMERA_STATE_AVAILABLE_DXR == (int)XRT_STEREO_CAMERA_STATE_AVAILABLE, "state");
_Static_assert((int)XR_STEREO_CAMERA_STATE_WAITING_DXR == (int)XRT_STEREO_CAMERA_STATE_WAITING, "state");
_Static_assert((int)XR_STEREO_CAMERA_STATE_SUSPENDED_DXR == (int)XRT_STEREO_CAMERA_STATE_SUSPENDED, "state");
_Static_assert((int)XR_STEREO_CAMERA_STATE_UNAVAILABLE_DXR == (int)XRT_STEREO_CAMERA_STATE_UNAVAILABLE, "state");
_Static_assert((int)XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR == (int)XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, "output");
_Static_assert((int)XR_STEREO_CAMERA_OUTPUT_RAW_DXR == (int)XRT_STEREO_CAMERA_OUTPUT_RAW, "output");
_Static_assert((int)XR_STEREO_CAMERA_FORMAT_GRAY8_DXR == (int)XRT_STEREO_CAMERA_FORMAT_GRAY8, "format");
_Static_assert((int)XR_STEREO_CAMERA_FORMAT_NV12_DXR == (int)XRT_STEREO_CAMERA_FORMAT_NV12, "format");
_Static_assert((int)XR_STEREO_CAMERA_FORMAT_BGRA8_DXR == (int)XRT_STEREO_CAMERA_FORMAT_BGRA8, "format");
_Static_assert(XR_STEREO_CAMERA_FORMAT_NV12_BIT_DXR == XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12),
               "format bit");
_Static_assert(XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_BIT_DXR ==
                   XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY),
               "transport bit");
_Static_assert((int)XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR == (int)XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY,
               "transport");
_Static_assert(XR_STEREO_CAMERA_RING_SLOTS_DXR == XRT_STEREO_CAMERA_RING_SLOTS, "ring slots");
_Static_assert(XR_STEREO_CAMERA_PERSISTENT_ID_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->persistent_id),
               "id size");
_Static_assert(XR_STEREO_CAMERA_DISPLAY_NAME_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->display_name),
               "name size");
_Static_assert(XR_STEREO_CAMERA_PLATFORM_HINT_MAX_SIZE_DXR ==
                   sizeof(((struct xrt_stereo_camera_properties *)0)->platform_device_hint),
               "hint size");

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
		                 "%s: the runtime refused camera access (consent; see XR_DXR_stereo_camera §7)", what);
	case XRT_ERROR_INPUT_UNSUPPORTED:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "%s: unknown camera/stream or unsupported request",
		                 what);
	case XRT_ERROR_FEATURE_NOT_SUPPORTED:
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "%s: not supported by this runtime / camera", what);
	case XRT_ERROR_CLIENT_LIMIT_REACHED:
		return oxr_error(log, XR_ERROR_LIMIT_REACHED, "%s: too many streams on this camera", what);
	default: return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "%s: service refused (xrt_result=%d)", what, (int)xret);
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
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "(capacityInput == %u) < %u cameras", capacityInput, n);
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
	struct xrt_stereo_camera_calibration k;
	xrt_result_t xret = c->get_calibration(c, cameraId, (uint32_t)calibration->output, &k);
	if (xret != XRT_SUCCESS) {
		return scam_xret(&log, xret, "xrGetStereoCameraCalibrationDXR");
	}
	for (int e = 0; e < 2; e++) {
		XrStereoCameraIntrinsicsDXR *o = &calibration->eye[e];
		o->imageExtent.width = (int32_t)k.eye[e].width;
		o->imageExtent.height = (int32_t)k.eye[e].height;
		o->fx = k.eye[e].fx;
		o->fy = k.eye[e].fy;
		o->cx = k.eye[e].cx;
		o->cy = k.eye[e].cy;
		o->model = (XrStereoCameraDistortionModelDXR)k.eye[e].model;
		memcpy(o->coefficients, k.eye[e].coefficients, sizeof(o->coefficients));
	}
	calibration->rightFromLeft.orientation.x = k.orientation[0];
	calibration->rightFromLeft.orientation.y = k.orientation[1];
	calibration->rightFromLeft.orientation.z = k.orientation[2];
	calibration->rightFromLeft.orientation.w = k.orientation[3];
	calibration->rightFromLeft.position.x = k.position[0];
	calibration->rightFromLeft.position.y = k.position[1];
	calibration->rightFromLeft.position.z = k.position[2];
	calibration->baselineMm = k.baseline_mm;
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

	XrStereoCameraStreamTransportDXR *t = OXR_GET_OUTPUT_FROM_CHAIN(info, XR_TYPE_STEREO_CAMERA_STREAM_TRANSPORT_DXR,
	                                                                XrStereoCameraStreamTransportDXR);
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
	xrt_result_t xret = c->acquire(c, s->id, &ready, &fi);
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

#endif // OXR_HAVE_DXR_stereo_camera
