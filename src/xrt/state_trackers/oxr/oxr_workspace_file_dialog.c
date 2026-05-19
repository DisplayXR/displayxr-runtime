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
 * Pattern mirrors oxr_app_launcher.c (IPC-bridge forward declarations,
 * session_is_ipc_client gate, xrt_result_t translation).
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include <openxr/XR_EXT_workspace_file_dialog.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef OXR_HAVE_EXT_workspace_file_dialog

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

	// PR C boundary: input is validated and we are in a position where the
	// IPC dispatch SHOULD happen. The IPC RPC + capability lookup + Tier 0
	// vs Tier 1 routing lands in PR B (workspace_request_file_dialog).
	// Until then, any caller gets the documented success-class fallback
	// signal so apps can be written and tested against the final ABI; once
	// PR B lands, sessions whose controller advertises
	// `SupportsFileDialog = 1` will instead receive a real request id and a
	// later completion event.
	static bool s_logged_stub = false;
	if (!s_logged_stub) {
		s_logged_stub = true;
		U_LOG_W("xrRequestFilePickerEXT: returning XR_FILE_PICKER_FALLBACK_TIER0_EXT "
		        "(IPC dispatch not yet implemented — see PR B for #228)");
	}
	return XR_FILE_PICKER_FALLBACK_TIER0_EXT;
}

#endif // OXR_HAVE_EXT_workspace_file_dialog
