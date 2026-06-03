// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_atlas_capture API entry point.
 * @author David Fattal
 * @ingroup oxr_api
 *
 * Phase 1 (in-process). xrCaptureAtlasEXT lets any session snapshot the
 * multi-view atlas the runtime composes for it, at a caller-selected stage,
 * without the app doing its own GPU readback. For an in-process native
 * compositor the entry point drives the same blocking capture bridge the MCP
 * @c capture_frame tool and the dev trigger files already use
 * (@ref oxr_mcp_tools_submit_capture → the compositor's @c u_capture_intent).
 * IPC-session routing (sharing the workspace capture bridge) and the workspace
 * PROJECTION_ONLY flag are a Phase-2 follow-up; see
 * docs/roadmap/unified-atlas-capture.md (W6 of issue #396).
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_mcp_tools.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "os/os_time.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include <openxr/XR_EXT_atlas_capture.h>

// enum mcp_capture_mode values; XrAtlasCaptureStageEXT is defined to match.
#include <displayxr_mcp/mcp_capture.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_atlas_capture

static bool
session_is_inprocess_native(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL) {
		return false;
	}
	return sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	       sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	       sess->is_vk_native_compositor;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCaptureAtlasEXT(XrSession session, const XrAtlasCaptureInfoEXT *info, XrAtlasCaptureResultEXT *result)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCaptureAtlasEXT");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_atlas_capture);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_ATLAS_CAPTURE_INFO_EXT);
	// result is optional: NULL captures the PNG only.

	if (info->stage != XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT &&
	    info->stage != XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrCaptureAtlasEXT: info->stage %d is not a valid XrAtlasCaptureStageEXT",
		                 (int)info->stage);
	}

	if (info->pathPrefix[0] == '\0') {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "xrCaptureAtlasEXT: info->pathPrefix is empty");
	}

	// Phase 1: only in-process native compositors are supported. IPC sessions
	// (workspace/WebXR) route through the workspace capture bridge in Phase 2.
	if (!session_is_inprocess_native(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCaptureAtlasEXT currently supports in-process sessions only");
	}

	// Build the output path. The runtime appends "_atlas.png" — same suffix
	// convention as the workspace capture path.
	char path[XR_ATLAS_CAPTURE_PATH_MAX_EXT + 16];
	int n = snprintf(path, sizeof(path), "%.*s_atlas.png", (int)(XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1),
	                 info->pathPrefix);
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "xrCaptureAtlasEXT: path prefix too long");
	}

	// Stage values are defined to match enum mcp_capture_mode (no translation).
	uint32_t mode = (info->stage == XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT)
	                    ? (uint32_t)MCP_CAPTURE_MODE_PROJECTION_ONLY
	                    : (uint32_t)MCP_CAPTURE_MODE_POST_COMPOSE;

	// Latch the capture intent onto the in-process compositor (non-blocking).
	// The readback + PNG encode happen at the next layer_commit poll — i.e. the
	// app's next xrEndFrame. We cannot block here: this runs on the app's
	// xrEndFrame thread, which is the same thread that drives the in-process
	// compositor's poll, so blocking would deadlock. The PNG therefore exists
	// shortly after the next composed frame, not when this call returns.
	if (!oxr_mcp_tools_submit_capture(path, mode)) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrCaptureAtlasEXT: no in-process compositor capture handler is installed");
	}

	if (result != NULL) {
		const struct xrt_system_compositor_info *sci = &sess->sys->xsysc->info;
		result->type = XR_TYPE_ATLAS_CAPTURE_RESULT_EXT;
		result->next = NULL;
		result->timestampNs = (uint64_t)os_monotonic_get_ns();
		result->atlasWidth = sci->atlas_width_pixels;
		result->atlasHeight = sci->atlas_height_pixels;
		// Per-view recommended dims approximate the captured eye-tile size.
		result->eyeWidth = sci->views[0].recommended.width_pixels;
		result->eyeHeight = sci->views[0].recommended.height_pixels;
		// Tile layout and eye poses are not surfaced to oxr on the in-process
		// path (the compositor knows them, but there is no accessor today and
		// eye-pose plumbing stops at the display processor). Left zero for
		// Phase 1; populated on the IPC/workspace path. See the W6 design doc.
		result->tileColumns = 0;
		result->tileRows = 0;
		result->displayWidthM = sci->display_width_m;
		result->displayHeightM = sci->display_height_m;
		result->eyeLeftM[0] = result->eyeLeftM[1] = result->eyeLeftM[2] = 0.0f;
		result->eyeRightM[0] = result->eyeRightM[1] = result->eyeRightM[2] = 0.0f;
	}

	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_atlas_capture
