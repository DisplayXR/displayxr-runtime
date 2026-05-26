// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_workspace_file_dialog API entry points (Tier 1 spatial picker).
 * @author DisplayXR
 * @ingroup oxr_api
 *
 * Async request shape: the app calls xrRequestFilePickerEXT and later
 * receives an XrEventDataFilePickerCompleteEXT via xrPollEvent. This TU
 * owns input validation and the runtime-side fallback decision; the IPC
 * dispatch + completion event push are wired in a separate PR alongside
 * the new workspace_request_file_dialog / workspace_file_dialog_result
 * RPCs.
 *
 * Pattern mirrors oxr_workspace.c (IPC-bridge forward declarations,
 * session_is_ipc_client gate, xrt_result_t translation).
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "shared/ipc_protocol.h"

#include <openxr/XR_EXT_workspace_file_dialog.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_workspace_file_dialog

// Forward declarations of the IPC client bridges — implementations live in
// ipc/client/ipc_client_compositor.c; we forward-declare here so the state
// tracker doesn't have to pull the full ipc_client include path.
struct xrt_compositor;
struct ipc_file_picker_result_path;
xrt_result_t
comp_ipc_client_compositor_session_request_file_picker(struct xrt_compositor *xc,
                                                       const struct ipc_file_picker_info *info,
                                                       uint64_t *out_request_id);
xrt_result_t
comp_ipc_client_compositor_workspace_get_file_picker_request(struct xrt_compositor *xc,
                                                             uint64_t request_id,
                                                             uint32_t *out_found,
                                                             uint32_t *out_client_id,
                                                             struct ipc_file_picker_info *out_info);
xrt_result_t
comp_ipc_client_compositor_workspace_file_dialog_result(struct xrt_compositor *xc,
                                                        uint64_t request_id,
                                                        uint32_t result_code,
                                                        const struct ipc_file_picker_result_path *path);

static bool
session_is_ipc_client(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		return false;
	}
	if (sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	    sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	    sess->is_vk_native_compositor) {
		return false;
	}
	return true;
}

/*!
 * Translate the public XrFilePickerInfoEXT to the wire-format
 * ipc_file_picker_info. The wire format is intentionally tighter than the
 * public ABI so the IPC payload fits inside IPC_BUF_SIZE — caller-supplied
 * strings longer than the wire budget are rejected with
 * XR_ERROR_PATH_FORMAT_INVALID so the picker never sees a truncated path.
 */
static XrResult
translate_info(struct oxr_logger *log,
               const XrFilePickerInfoEXT *in,
               struct ipc_file_picker_info *out)
{
	memset(out, 0, sizeof(*out));
	out->mode = (uint32_t)in->mode;
	out->flags = (uint64_t)in->flags;

	size_t title_len = strnlen(in->title, XR_MAX_FILE_PICKER_TITLE_LENGTH_EXT);
	if (title_len >= sizeof(out->title)) {
		return oxr_error(log, XR_ERROR_PATH_FORMAT_INVALID,
		                 "(info->title) length %zu exceeds wire budget %zu",
		                 title_len, sizeof(out->title) - 1);
	}
	memcpy(out->title, in->title, title_len);

	size_t path_len = strnlen(in->defaultPath, XR_MAX_FILE_PICKER_PATH_LENGTH_EXT);
	if (path_len >= sizeof(out->default_path)) {
		return oxr_error(log, XR_ERROR_PATH_FORMAT_INVALID,
		                 "(info->defaultPath) length %zu exceeds wire budget %zu",
		                 path_len, sizeof(out->default_path) - 1);
	}
	memcpy(out->default_path, in->defaultPath, path_len);

	if (in->filterCount > IPC_FILE_PICKER_FILTERS_MAX) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(info->filterCount) %u exceeds wire budget %u",
		                 in->filterCount, IPC_FILE_PICKER_FILTERS_MAX);
	}
	out->filter_count = in->filterCount;
	for (uint32_t i = 0; i < in->filterCount; i++) {
		size_t d = strnlen(in->filters[i].description, XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT);
		size_t e = strnlen(in->filters[i].extensions, XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT);
		if (d >= sizeof(out->filters[i].description) ||
		    e >= sizeof(out->filters[i].extensions)) {
			return oxr_error(log, XR_ERROR_PATH_FORMAT_INVALID,
			                 "(info->filters[%u]) field exceeds wire budget", i);
		}
		memcpy(out->filters[i].description, in->filters[i].description, d);
		memcpy(out->filters[i].extensions, in->filters[i].extensions, e);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestFilePickerEXT(XrSession session,
                           const XrFilePickerInfoEXT *info,
                           XrAsyncRequestIdEXT *requestId)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRequestFilePickerEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_workspace_file_dialog);

	if (info == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(info == NULL)");
	}
	if (info->type != XR_TYPE_FILE_PICKER_INFO_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(info->type) must be XR_TYPE_FILE_PICKER_INFO_EXT, got %d",
		                 (int)info->type);
	}
	if (info->next != NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(info->next) must be NULL in spec_version 1");
	}
	if (requestId == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(requestId == NULL)");
	}

	switch (info->mode) {
	case XR_FILE_PICKER_MODE_OPEN_EXT:
	case XR_FILE_PICKER_MODE_SAVE_EXT:
	case XR_FILE_PICKER_MODE_FOLDER_EXT: break;
	default:
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(info->mode) invalid: %d", (int)info->mode);
	}

	if (info->filterCount > XR_MAX_FILE_PICKER_FILTERS_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "(info->filterCount) %u > XR_MAX_FILE_PICKER_FILTERS_EXT (%u)",
		                 info->filterCount, XR_MAX_FILE_PICKER_FILTERS_EXT);
	}

	if ((info->flags & XR_FILE_PICKER_FLAG_MULTI_SELECT_BIT_EXT) != 0) {
		// Reserved bit; spec_version 1 picker UIs do not support multi-select.
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "MULTI_SELECT_BIT_EXT reserved for spec_version 2");
	}

	*requestId = XR_NULL_ASYNC_REQUEST_ID_EXT;

	// Standalone (non-workspace) session: the extension is workspace-scoped.
	// Apps in this state should call GetOpenFileName themselves; there's no
	// Tier 0 hook installed either, so we don't promise fallback semantics.
	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrRequestFilePickerEXT requires a workspace (IPC-mode) session");
	}

	// Workspace controller calling its own picker would be a recursion
	// (the picker IS a workspace client). Reject so we never spawn a picker
	// from within the picker process or the shell itself.
	if (sess->is_active_workspace_controller) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrRequestFilePickerEXT is not callable from a workspace controller session");
	}

	// Translate to the wire format. Length-validation rejects user paths
	// that wouldn't survive the IPC budget — the app can fall back to
	// Tier 0 (flat OS dialog) if it needs long-path support.
	struct ipc_file_picker_info ipc_info;
	XrResult tr = translate_info(&log, info, &ipc_info);
	if (tr != XR_SUCCESS) {
		return tr;
	}

	// Dispatch through the IPC bridge. The runtime allocates a monotonic
	// request_id, queues a FILE_PICKER_REQUEST event for the controller's
	// drain channel, and returns. If the bridge fails (no workspace
	// controller online, controller has not advertised file-dialog
	// support, table full, …) we surface XR_FILE_PICKER_FALLBACK_TIER0_EXT
	// so the app falls through to a flat OS dialog — Tier 0's CBT hook
	// will handle z-order / focus on its own.
	uint64_t allocated_id = 0;
	xrt_result_t xret = comp_ipc_client_compositor_session_request_file_picker(
	    &sess->xcn->base, &ipc_info, &allocated_id);
	if (xret != XRT_SUCCESS || allocated_id == 0) {
		U_LOG_I("xrRequestFilePickerEXT: bridge=%d, allocated_id=%llu — falling back to Tier 0",
		        (int)xret, (unsigned long long)allocated_id);
		return XR_FILE_PICKER_FALLBACK_TIER0_EXT;
	}

	*requestId = (XrAsyncRequestIdEXT)allocated_id;
	return XR_SUCCESS;
}

/*
 * Controller-side entrypoints (paired with the runtime-only
 * workspace_get_file_picker_request / workspace_file_dialog_result IPCs).
 * Restricted to the active workspace controller — non-controller callers
 * receive XR_ERROR_FEATURE_UNSUPPORTED.
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFilePickerRequestEXT(XrSession session,
                              XrAsyncRequestIdEXT requestId,
                              uint32_t *outClientId,
                              XrFilePickerInfoEXT *outInfo)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetFilePickerRequestEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_workspace_file_dialog);

	if (outClientId == NULL || outInfo == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(outClientId / outInfo NULL)");
	}
	if (requestId == XR_NULL_ASYNC_REQUEST_ID_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(requestId == 0)");
	}
	*outClientId = 0;
	memset(outInfo, 0, sizeof(*outInfo));

	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetFilePickerRequestEXT requires an IPC-mode session");
	}
	if (!sess->is_active_workspace_controller) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrGetFilePickerRequestEXT requires the active workspace controller session");
	}

	uint32_t found = 0;
	uint32_t client_id = 0;
	struct ipc_file_picker_info ipc_info;
	memset(&ipc_info, 0, sizeof(ipc_info));
	xrt_result_t xret = comp_ipc_client_compositor_workspace_get_file_picker_request(
	    &sess->xcn->base, (uint64_t)requestId, &found, &client_id, &ipc_info);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "workspace_get_file_picker_request: xrt_result=%d",
		                 (int)xret);
	}
	if (!found) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrGetFilePickerRequestEXT: no pending request for requestId=%llu",
		                 (unsigned long long)requestId);
	}

	*outClientId = client_id;
	outInfo->type = XR_TYPE_FILE_PICKER_INFO_EXT;
	outInfo->next = NULL;
	outInfo->mode = (XrFilePickerModeEXT)ipc_info.mode;
	outInfo->flags = (XrFilePickerFlagsEXT)ipc_info.flags;
	// All char fields fit by construction — wire budgets ≤ public budgets.
	memcpy(outInfo->title, ipc_info.title,
	       sizeof(ipc_info.title) < sizeof(outInfo->title) ? sizeof(ipc_info.title) : sizeof(outInfo->title) - 1);
	memcpy(outInfo->defaultPath, ipc_info.default_path,
	       sizeof(ipc_info.default_path) < sizeof(outInfo->defaultPath) ? sizeof(ipc_info.default_path)
	                                                                    : sizeof(outInfo->defaultPath) - 1);
	outInfo->filterCount = ipc_info.filter_count;
	for (uint32_t i = 0; i < ipc_info.filter_count && i < XR_MAX_FILE_PICKER_FILTERS_EXT; i++) {
		memcpy(outInfo->filters[i].description, ipc_info.filters[i].description,
		       sizeof(ipc_info.filters[i].description));
		memcpy(outInfo->filters[i].extensions, ipc_info.filters[i].extensions,
		       sizeof(ipc_info.filters[i].extensions));
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCompleteFilePickerEXT(XrSession session,
                            XrAsyncRequestIdEXT requestId,
                            XrFilePickerResultEXT result,
                            const char *path)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCompleteFilePickerEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_workspace_file_dialog);

	if (requestId == XR_NULL_ASYNC_REQUEST_ID_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(requestId == 0)");
	}
	if (!session_is_ipc_client(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCompleteFilePickerEXT requires an IPC-mode session");
	}
	if (!sess->is_active_workspace_controller) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCompleteFilePickerEXT requires the active workspace controller session");
	}

	struct ipc_file_picker_result_path wire_path;
	memset(&wire_path, 0, sizeof(wire_path));
	if (result == XR_FILE_PICKER_RESULT_SUCCESS_EXT && path != NULL && path[0] != '\0') {
		size_t len = strnlen(path, XR_MAX_FILE_PICKER_PATH_LENGTH_EXT);
		if (len >= sizeof(wire_path.path)) {
			return oxr_error(&log, XR_ERROR_PATH_FORMAT_INVALID,
			                 "(path) length %zu exceeds wire budget %zu", len,
			                 sizeof(wire_path.path) - 1);
		}
		memcpy(wire_path.path, path, len);
	}

	xrt_result_t xret = comp_ipc_client_compositor_workspace_file_dialog_result(
	    &sess->xcn->base, (uint64_t)requestId, (uint32_t)result, &wire_path);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "workspace_file_dialog_result: xrt_result=%d",
		                 (int)xret);
	}
	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_workspace_file_dialog
