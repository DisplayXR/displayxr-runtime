// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_conformance_automation entrypoint functions.
 *
 * Thin verification layer over @ref oxr_conformance.c. The Khronos CTS calls
 * these to inject synthetic controller state; we validate the arguments and
 * forward to the per-session override store.
 *
 * @ingroup oxr_api
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include <string.h>

#ifdef OXR_HAVE_EXT_conformance_automation

/*
 *
 * Argument verification helpers.
 *
 */

//! Validate a top-level user path (e.g. /user/hand/left). Returns
//! XR_ERROR_PATH_UNSUPPORTED for a path the runtime has no role for.
static XrResult
verify_top_level_path(struct oxr_logger *log, struct oxr_session *sess, XrPath top_level_path)
{
	struct oxr_subaction_paths paths;
	if (top_level_path == XR_NULL_PATH) {
		return oxr_error(log, XR_ERROR_PATH_INVALID, "(topLevelPath == XR_NULL_PATH)");
	}
	if (!oxr_classify_subaction_paths(log, sess->sys->inst, 1, &top_level_path, &paths)) {
		return oxr_error(log, XR_ERROR_PATH_UNSUPPORTED, "(topLevelPath) is not a supported top-level user path");
	}
	return XR_SUCCESS;
}

//! Validate an input source path resolves to a string in the instance path
//! cache. We do not require it to match a current binding — an unbound source
//! simply never gets applied — but a malformed XrPath is rejected.
static XrResult
verify_source_path(struct oxr_logger *log, struct oxr_session *sess, XrPath source_path)
{
	const char *str = NULL;
	size_t length = 0;
	if (source_path == XR_NULL_PATH) {
		return oxr_error(log, XR_ERROR_PATH_INVALID, "(inputSourcePath == XR_NULL_PATH)");
	}
	if (oxr_path_get_string(log, sess->sys->inst, source_path, &str, &length) != XR_SUCCESS) {
		return oxr_error(log, XR_ERROR_PATH_INVALID, "(inputSourcePath) is not a valid path");
	}
	return XR_SUCCESS;
}


/*
 *
 * Entrypoints.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceActiveEXT(XrSession session, XrPath interactionProfile, XrPath topLevelPath, XrBool32 isActive)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputDeviceActiveEXT");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_conformance_automation);

	XrResult ret = verify_top_level_path(&log, sess, topLevelPath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	// interactionProfile may be XR_NULL_PATH (means "any"); if set, validate it.
	if (interactionProfile != XR_NULL_PATH) {
		const char *str = NULL;
		size_t length = 0;
		if (oxr_path_get_string(&log, sess->sys->inst, interactionProfile, &str, &length) != XR_SUCCESS) {
			return oxr_error(&log, XR_ERROR_PATH_INVALID, "(interactionProfile) is not a valid path");
		}
	}

	return oxr_conformance_set_device_active(&log, sess, topLevelPath, isActive == XR_TRUE);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateBoolEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrBool32 state)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputDeviceStateBoolEXT");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_conformance_automation);

	XrResult ret = verify_top_level_path(&log, sess, topLevelPath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	ret = verify_source_path(&log, sess, inputSourcePath);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	union xrt_input_value value;
	memset(&value, 0, sizeof(value));
	value.boolean = state == XR_TRUE;
	return oxr_conformance_set_input_value(&log, sess, topLevelPath, inputSourcePath, XRT_INPUT_TYPE_BOOLEAN, value);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateFloatEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, float state)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputDeviceStateFloatEXT");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_conformance_automation);

	XrResult ret = verify_top_level_path(&log, sess, topLevelPath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	ret = verify_source_path(&log, sess, inputSourcePath);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	union xrt_input_value value;
	memset(&value, 0, sizeof(value));
	value.vec1.x = state;
	return oxr_conformance_set_input_value(&log, sess, topLevelPath, inputSourcePath,
	                                       XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE, value);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateVector2fEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrVector2f state)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputDeviceStateVector2fEXT");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_conformance_automation);

	XrResult ret = verify_top_level_path(&log, sess, topLevelPath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	ret = verify_source_path(&log, sess, inputSourcePath);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	union xrt_input_value value;
	memset(&value, 0, sizeof(value));
	value.vec2.x = state.x;
	value.vec2.y = state.y;
	return oxr_conformance_set_input_value(&log, sess, topLevelPath, inputSourcePath,
	                                       XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE, value);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceLocationEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrSpace space, XrPosef pose)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputDeviceLocationEXT");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_conformance_automation);

	XrResult ret = verify_top_level_path(&log, sess, topLevelPath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	ret = verify_source_path(&log, sess, inputSourcePath);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	OXR_VERIFY_ARG_NOT_ZERO(&log, space);

	// NOTE: the pose is interpreted relative to the runtime's tracking origin.
	// Resolving it through the supplied XrSpace is a refinement to validate
	// against CTS pose tests on Windows (see handoff notes).
	struct xrt_pose xpose;
	xpose.position.x = pose.position.x;
	xpose.position.y = pose.position.y;
	xpose.position.z = pose.position.z;
	xpose.orientation.x = pose.orientation.x;
	xpose.orientation.y = pose.orientation.y;
	xpose.orientation.z = pose.orientation.z;
	xpose.orientation.w = pose.orientation.w;

	return oxr_conformance_set_input_pose(&log, sess, topLevelPath, inputSourcePath, xpose);
}

#endif // OXR_HAVE_EXT_conformance_automation
