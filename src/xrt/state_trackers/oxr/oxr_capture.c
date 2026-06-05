// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_atlas_capture API entry point.
 * @author David Fattal
 * @ingroup oxr_api
 *
 * xrCaptureAtlasEXT lets any session snapshot the multi-view atlas the runtime
 * composes for it, at a caller-selected stage, without the app doing its own
 * GPU readback.
 *
 * - **In-process** native compositors (d3d11/d3d12/gl/vk/metal): the entry
 *   point LATCHES the capture onto the compositor's existing
 *   @c u_capture_intent (via @ref oxr_mcp_tools_submit_capture, the same bridge
 *   the MCP @c capture_frame tool and the dev trigger files use). It must be
 *   non-blocking — the call runs on the app's xrEndFrame thread, which also
 *   drives the in-process compositor's layer_commit poll, so blocking would
 *   deadlock. The readback happens at the next layer_commit.
 * - **IPC / service-mode** sessions: routed over the existing capture IPC
 *   bridge; the service compositor owns the readback. PROJECTION_ONLY captures
 *   the calling session's own per-client projection atlas (the service reads the
 *   active client's content-sized crop atlas), so it is per-session correct and
 *   works for a single (non-workspace) IPC client. POST_COMPOSE maps to the
 *   multi-compositor's combined (whole-workspace) atlas, which only exists in
 *   workspace mode — so POST_COMPOSE over IPC is reported UNSUPPORTED for a
 *   single non-workspace client.
 *
 * Full design: docs/roadmap/unified-atlas-capture.md (W6 of issue #396).
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_mcp_tools.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"
#include "util/u_capture_dims.h"

#include "os/os_time.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_results.h"

// IPC_CAPTURE_FLAG_* stage selectors for the IPC capture bridge. The CMake
// include path (state_trackers/oxr/../../ipc) lets us write "shared/...".
#include "shared/ipc_protocol.h"

#include <openxr/XR_EXT_atlas_capture.h>

// enum mcp_capture_mode values; XrAtlasCaptureStageEXT is defined to match.
#include <displayxr_mcp/mcp_capture.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef OXR_HAVE_EXT_atlas_capture

// Forward decl of the IPC-bridge wrapper (defined in ipc_client_compositor.c).
// st_oxr does not pull the ipc_client include path; the runtime DLL links it so
// the symbol resolves at link time. Same pattern as oxr_workspace.c.
struct xrt_compositor;
xrt_result_t
comp_ipc_client_compositor_workspace_capture_frame(struct xrt_compositor *xc,
                                                    const char *path_prefix,
                                                    uint32_t flags,
                                                    uint64_t *out_timestamp_ns,
                                                    uint32_t *out_atlas_w,
                                                    uint32_t *out_atlas_h,
                                                    uint32_t *out_eye_w,
                                                    uint32_t *out_eye_h,
                                                    uint32_t *out_views_written,
                                                    uint32_t *out_tile_columns,
                                                    uint32_t *out_tile_rows,
                                                    float *out_display_w_m,
                                                    float *out_display_h_m,
                                                    float out_eye_left_m[3],
                                                    float out_eye_right_m[3]);

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

static bool
session_is_ipc(struct oxr_session *sess)
{
	// IPC sessions hold a native-compositor handle (the IPC client compositor)
	// but none of the in-process native-compositor flags are set.
	return sess != NULL && sess->xcn != NULL && sess->sys != NULL && sess->sys->xsysc != NULL &&
	       !session_is_inprocess_native(sess);
}

//! In-process path: latch onto the compositor's capture intent (non-blocking).
static XrResult
capture_inprocess(struct oxr_logger *log,
                  struct oxr_session *sess,
                  const XrAtlasCaptureInfoEXT *info,
                  XrAtlasCaptureResultEXT *result)
{
	// Resolve the active rendering mode's tile layout so the suffix can encode
	// the atlas geometry (issue #425). This also surfaces tileColumns/tileRows
	// on the in-process path, which previously reported zero.
	struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
	uint32_t cols = 0, rows = 0;
	if (head != NULL && head->hmd != NULL) {
		uint32_t idx = head->hmd->active_rendering_mode_index;
		if (idx < head->rendering_mode_count) {
			cols = head->rendering_modes[idx].tile_columns;
			rows = head->rendering_modes[idx].tile_rows;
		}
	}
	if (cols == 0) {
		cols = 1;
	}
	if (rows == 0) {
		rows = 1;
	}
	uint32_t view_count = cols * rows;

	// The runtime appends "_atlas_<viewCount>_<cols>x<rows>.png" so consumers
	// don't re-derive the multi-view atlas geometry — same suffix the workspace
	// (IPC) capture path emits.
	char path[XR_ATLAS_CAPTURE_PATH_MAX_EXT + 32];
	int n = snprintf(path, sizeof(path), "%.*s_atlas_%u_%ux%u.png",
	                 (int)(XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1), info->pathPrefix, view_count, cols, rows);
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "xrCaptureAtlasEXT: path prefix too long");
	}

	// Stage values are defined to match enum mcp_capture_mode (no translation).
	uint32_t mode = (info->stage == XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT)
	                    ? (uint32_t)MCP_CAPTURE_MODE_PROJECTION_ONLY
	                    : (uint32_t)MCP_CAPTURE_MODE_POST_COMPOSE;

	if (!oxr_mcp_tools_submit_capture(path, mode)) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrCaptureAtlasEXT: no in-process compositor capture handler is installed");
	}

	if (result != NULL) {
		const struct xrt_system_compositor_info *sci = &sess->sys->xsysc->info;
		result->type = XR_TYPE_ATLAS_CAPTURE_RESULT_EXT;
		result->next = NULL;
		result->timestampNs = (uint64_t)os_monotonic_get_ns();

		// Prefer the compositor's CURRENT window-scaled dims — what the capture
		// actually writes — over the static nominal system-compositor info,
		// which disagrees with the PNG whenever the window differs from the
		// display (#431). Falls back to nominal dims if no provider is
		// registered (gl/vk/metal) or the renderer isn't sized yet.
		uint32_t qvw = 0, qvh = 0, qcols = 0, qrows = 0;
		if (u_capture_dims_query(&qvw, &qvh, &qcols, &qrows) && qvw > 0 && qvh > 0) {
			if (qcols > 0) {
				cols = qcols;
			}
			if (qrows > 0) {
				rows = qrows;
			}
			result->eyeWidth = qvw;
			result->eyeHeight = qvh;
			result->atlasWidth = cols * qvw;
			result->atlasHeight = rows * qvh;
		} else {
			result->atlasWidth = sci->atlas_width_pixels;
			result->atlasHeight = sci->atlas_height_pixels;
			// Per-view recommended dims approximate the captured eye-tile size.
			result->eyeWidth = sci->views[0].recommended.width_pixels;
			result->eyeHeight = sci->views[0].recommended.height_pixels;
		}
		// Tile layout from the active rendering mode (resolved above), unless
		// the provider supplied a current layout. Eye poses are still not
		// surfaced on the in-process path — eye-pose plumbing stops at the
		// display processor; the IPC path returns them.
		result->tileColumns = cols;
		result->tileRows = rows;
		result->displayWidthM = sci->display_width_m;
		result->displayHeightM = sci->display_height_m;
		result->eyeLeftM[0] = result->eyeLeftM[1] = result->eyeLeftM[2] = 0.0f;
		result->eyeRightM[0] = result->eyeRightM[1] = result->eyeRightM[2] = 0.0f;
	}
	return XR_SUCCESS;
}

//! IPC path: route over the existing workspace-capture bridge. The service
//! compositor owns the readback (and full metadata, including eye poses).
static XrResult
capture_ipc(struct oxr_logger *log,
            struct oxr_session *sess,
            const XrAtlasCaptureInfoEXT *info,
            XrAtlasCaptureResultEXT *result)
{
	// Map the stage to the IPC stage-selector flag. The service writes the
	// matching bit into views_written iff it actually captured that stage.
	uint32_t want_flag = (info->stage == XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT)
	                         ? IPC_CAPTURE_FLAG_PROJECTION_ONLY
	                         : IPC_CAPTURE_FLAG_ATLAS;

	uint64_t ts_ns = 0;
	uint32_t aw = 0, ah = 0, ew = 0, eh = 0, vw = 0, tc = 0, tr = 0;
	float dw_m = 0.0f, dh_m = 0.0f;
	float eye_l[3] = {0}, eye_r[3] = {0};
	xrt_result_t xret = comp_ipc_client_compositor_workspace_capture_frame(
	    &sess->xcn->base, info->pathPrefix, want_flag, &ts_ns, &aw, &ah, &ew, &eh, &vw, &tc, &tr, &dw_m,
	    &dh_m, eye_l, eye_r);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrCaptureAtlasEXT: IPC capture failed (xrt_result=%d)", (int)xret);
	}

	// The service reports which stage it actually wrote. If the requested stage
	// wasn't written, it isn't available on this session: POST_COMPOSE needs the
	// multi-compositor's combined atlas (workspace mode), absent for a single
	// non-workspace IPC client.
	if ((vw & want_flag) == 0) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrCaptureAtlasEXT: the requested capture stage is not available for this "
		                 "IPC session (post-compose requires workspace mode)");
	}

	if (result != NULL) {
		result->type = XR_TYPE_ATLAS_CAPTURE_RESULT_EXT;
		result->next = NULL;
		result->timestampNs = ts_ns;
		result->atlasWidth = aw;
		result->atlasHeight = ah;
		result->eyeWidth = ew;
		result->eyeHeight = eh;
		result->tileColumns = tc;
		result->tileRows = tr;
		result->displayWidthM = dw_m;
		result->displayHeightM = dh_m;
		result->eyeLeftM[0] = eye_l[0];
		result->eyeLeftM[1] = eye_l[1];
		result->eyeLeftM[2] = eye_l[2];
		result->eyeRightM[0] = eye_r[0];
		result->eyeRightM[1] = eye_r[1];
		result->eyeRightM[2] = eye_r[2];
	}
	return XR_SUCCESS;
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

	if (session_is_inprocess_native(sess)) {
		return capture_inprocess(&log, sess, info, result);
	}
	if (session_is_ipc(sess)) {
		return capture_ipc(&log, sess, info, result);
	}

	return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
	                 "xrCaptureAtlasEXT: session has no capture-capable compositor");
}

#endif // OXR_HAVE_EXT_atlas_capture
