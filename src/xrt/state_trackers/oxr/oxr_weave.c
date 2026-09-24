// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave API entry points (issue #625).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * A window-bound, synchronous weave service for present-owners (see
 * XR_DXR_weave.h and docs/roadmap/webxr-support.md §2.4 "Step 0"). The caller
 * owns its OS window and presents itself; the runtime's display processor
 * weaves a window sub-rect from a caller-supplied pre-weave SBS texture and
 * hands back a weaved shared texture + fence. The caller NEVER weaves
 * (ADR-007 / ADR-019).
 *
 * Availability: implemented only on the out-of-process (service / IPC) path —
 * the weave runs in the D3D11 service compositor on Windows and in the
 * comp_multi Vulkan weave engine on macOS (#759: IOSurface in/out, synchronous
 * completion, no fence handle). An in-process session reports
 * XR_ERROR_FEATURE_UNSUPPORTED — with ONE exception, xrWeaveSnapWindowRectDXR
 * (#1588) and its bulk form xrWeaveSnapWindowGridDXR (#1723), which are pure
 * queries on the display processor and therefore work in-process too (an X11
 * handle app owns and drags its own window, so it is the party that needs to
 * ask where the window may land).
 *
 * Desktop Linux (spec v10, #1699) carries the full service when the service is
 * built with its comp_multi weave engine (XRT_FEATURE_COMP_MULTI_WEAVE_LINUX):
 * a dma-buf input described by XrWeaveDmabufDescDXR, an optional acquire
 * sync_file, a per-frame release sync_file and a typed dma-buf output. A
 * service built without the engine answers every submit
 * XRT_ERROR_FEATURE_NOT_SUPPORTED, reported to the caller as
 * XR_ERROR_FEATURE_UNSUPPORTED (permanent, not a retry). fds passed IN become the runtime's only on
 * XR_SUCCESS; this file closes the caller's originals at the very end of a
 * successful xrWeaveSubmitDXR, after the service already holds its copies.
 * The entry points forward to thin IPC-client
 * bridges (defined in ipc_client_compositor.c); st_oxr does not pull the
 * ipc_client include path, so the symbols resolve at link time — same pattern
 * as oxr_capture.c / oxr_workspace.c.
 *
 * Connection loss (browser#103): every bridge result goes through
 * OXR_CHECK_XRET_MSG, so a dead pipe (XRT_ERROR_IPC_FAILURE) marks the session
 * lost and reports XR_ERROR_INSTANCE_LOST — after which
 * OXR_VERIFY_SESSION_NOT_LOST short-circuits every later call to
 * XR_ERROR_SESSION_LOST, exactly like xrEndFrame / xrLocateViews. A weave-only
 * present-owner is otherwise the one client in the runtime that never learns its
 * connection died.
 *
 * A service-side weave REFUSAL is deliberately NOT that: the service answers
 * XRT_ERROR_WEAVE_REFUSED over a healthy pipe (canonically the 4 ms input
 * AcquireSync timeout in comp_d3d11_service_weave_submit, retried next frame),
 * which stays a non-fatal XR_ERROR_RUNTIME_FAILURE and leaves the session usable.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_xret.h"
#include "oxr_weave_latch.h"

#include "util/u_trace_marker.h"
#include "util/u_logging.h"
#include "util/u_snap_grid.h"

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_chain.h"

#include "xrt/xrt_results.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_weave_dmabuf.h"

#include <openxr/XR_DXR_weave.h>

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
// Desktop Linux (#1588): xrWeaveSnapWindowRectDXR is the ONE weave entry point
// an in-process session can serve, because the snap is a pure query on the
// display processor and needs no weave service at all. st_oxr already links
// comp_vk_native on this platform (see its CMakeLists).
#include "vk_native/comp_vk_native_compositor.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef XRT_OS_LINUX_DESKTOP
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifdef OXR_HAVE_DXR_weave

// Spec v11 (#1723): the grid points are filled in place by u_snap_grid.
static_assert(sizeof(XrWeaveSnapGridPointDXR) == 2 && offsetof(XrWeaveSnapGridPointDXR, dy) == 1,
              "XrWeaveSnapGridPointDXR must be the {int8 dx, int8 dy} pair u_snap_grid writes");
static_assert(XR_WEAVE_SNAP_GRID_MAX_POINTS_DXR == U_SNAP_GRID_MAX_POINTS, "grid point bound mismatch");
static_assert(XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR == U_SNAP_GRID_MAX_AXIS, "grid axis bound mismatch");
static_assert(XR_WEAVE_SNAP_GRID_NO_DELTA_DXR == U_SNAP_GRID_NO_DELTA, "grid sentinel mismatch");

// Forward decls of the IPC-bridge wrappers (defined in ipc_client_compositor.c).
struct xrt_compositor;

//! browser#103 RC-4: the broker half of the adopt mechanism, defined in
//! ipc_client_connection.c. Same link-time-resolution pattern as the bridges
//! below — st_oxr does not pull the ipc_client include path.
xrt_result_t
ipc_client_connection_export(enum u_logging_level log_level, xrt_ipc_handle_t *out_handle);

xrt_result_t
comp_ipc_client_compositor_weave_bind_window(struct xrt_compositor *xc, uint64_t hwnd);

xrt_result_t
comp_ipc_client_compositor_weave_set_window_geometry(struct xrt_compositor *xc,
                                                     int32_t origin_x,
                                                     int32_t origin_y,
                                                     uint32_t client_w,
                                                     uint32_t client_h,
                                                     int32_t display_id);

xrt_result_t
comp_ipc_client_compositor_weave_submit(struct xrt_compositor *xc,
                                        xrt_graphics_buffer_handle_t in_handle,
                                        bool in_is_dxgi,
                                        int32_t rect_x,
                                        int32_t rect_y,
                                        uint32_t rect_w,
                                        uint32_t rect_h,
                                        uint32_t rect_count,
                                        const struct xrt_rect *rects,
                                        xrt_graphics_buffer_handle_t overlay_handle,
                                        bool overlay_is_dxgi,
                                        uint32_t overlay_rect_count,
                                        const struct xrt_rect *overlay_rects,
                                        bool weave_frame_first,
                                        const struct xrt_weave_atlas_layout *layout,
                                        uint32_t flat_rect_count,
                                        const struct xrt_rect *flat_rects,
                                        bool *out_have_output,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint64_t *out_fence_value,
                                        struct xrt_eye_positions *out_eyes);

xrt_result_t
comp_ipc_client_compositor_weave_set_screen_flat_regions(struct xrt_compositor *xc,
                                                         uint32_t rect_count,
                                                         const struct xrt_rect *screen_rects);

xrt_result_t
comp_ipc_client_compositor_weave_get_output(struct xrt_compositor *xc,
                                            bool *out_have_output,
                                            uint32_t *out_width,
                                            uint32_t *out_height,
                                            xrt_graphics_buffer_handle_t *out_handle);

xrt_result_t
comp_ipc_client_compositor_weave_get_fence(struct xrt_compositor *xc,
                                           bool *out_have_fence,
                                           xrt_graphics_sync_handle_t *out_handle);

#ifdef XRT_OS_LINUX_DESKTOP
// Spec v10 (#1699) desktop-Linux dma-buf bridges, same file + pattern as above.
xrt_result_t
comp_ipc_client_compositor_weave_submit_dmabuf(struct xrt_compositor *xc,
                                               const struct xrt_weave_dmabuf_desc *in,
                                               const struct xrt_weave_dmabuf_desc *overlay,
                                               int acquire_fence_fd,
                                               int32_t rect_x,
                                               int32_t rect_y,
                                               uint32_t rect_w,
                                               uint32_t rect_h,
                                               uint32_t rect_count,
                                               const struct xrt_rect *rects,
                                               uint32_t overlay_rect_count,
                                               bool weave_frame_first,
                                               const struct xrt_weave_atlas_layout *layout,
                                               uint32_t flat_rect_count,
                                               const struct xrt_rect *flat_rects,
                                               int *out_release_fence_fd,
                                               bool *out_have_output,
                                               uint32_t *out_width,
                                               uint32_t *out_height,
                                               uint64_t *out_fence_value,
                                               struct xrt_eye_positions *out_eyes);

xrt_result_t
comp_ipc_client_compositor_weave_get_output_dmabuf(struct xrt_compositor *xc,
                                                   bool *out_have_output,
                                                   struct xrt_weave_dmabuf_output_desc *out_desc);
#endif

xrt_result_t
comp_ipc_client_compositor_weave_snap_window_rect(struct xrt_compositor *xc,
                                                  int32_t origin_x,
                                                  int32_t origin_y,
                                                  int32_t target_x,
                                                  int32_t target_y,
                                                  bool *out_snapped,
                                                  int32_t *out_snapped_x,
                                                  int32_t *out_snapped_y);

xrt_result_t
comp_ipc_client_compositor_weave_snap_window_grid(struct xrt_compositor *xc,
                                                  const struct u_snap_grid *grid,
                                                  int8_t *out_dxdy,
                                                  bool *out_declined);

/*!
 * Is @p kind a handle kind THIS platform can accept (spec v7, #1036)?
 * PLATFORM_DEFAULT always is — it means "resolve from the platform", which is
 * what every pre-v7 caller does implicitly.
 */
static bool
weave_handle_kind_ok(XrWeaveHandleKindDXR kind)
{
	switch (kind) {
	case XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR: return true;
#if defined(XR_USE_PLATFORM_WIN32)
	case XR_WEAVE_HANDLE_KIND_D3D11_NT_DXR:
	case XR_WEAVE_HANDLE_KIND_D3D11_DXGI_DXR: return true;
#elif defined(XR_USE_PLATFORM_ANDROID)
	case XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR: return true;
#elif defined(XR_USE_PLATFORM_MACOS)
	case XR_WEAVE_HANDLE_KIND_IOSURFACE_DXR: return true;
#elif defined(XRT_OS_LINUX_DESKTOP)
	// Spec v10 (#1699). DMABUF additionally requires its descriptor chain and
	// OPAQUE_FD forbids one — cross-checked in xrWeaveSubmitDXR.
	case XR_WEAVE_HANDLE_KIND_DMABUF_DXR:
	case XR_WEAVE_HANDLE_KIND_OPAQUE_FD_DXR: return true;
#endif
	default: return false;
	}
}

#ifdef XRT_OS_LINUX_DESKTOP
//! DRM_FORMAT_MOD_INVALID ("implicit modifier"), spelled out: no libdrm header.
#define OXR_WEAVE_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

// XrWeaveOverlayDmabufDescDXR is XrWeaveDmabufDescDXR under a second structure
// type (a chain may carry a type once). Pin the layouts equal so one reader
// serves both.
static_assert(sizeof(XrWeaveOverlayDmabufDescDXR) == sizeof(XrWeaveDmabufDescDXR), "dmabuf desc layouts diverged");
static_assert(offsetof(XrWeaveOverlayDmabufDescDXR, bufferId) == offsetof(XrWeaveDmabufDescDXR, bufferId),
              "dmabuf desc layouts diverged");
static_assert(XR_WEAVE_DMABUF_MAX_PLANES_DXR == XRT_WEAVE_DMABUF_MAX_PLANES, "plane bound mismatch");

/*!
 * Validate one v10 input descriptor and convert it (fd included) to the xrt
 * form. The fd is NOT taken here — ownership only moves on XR_SUCCESS, which
 * xrWeaveSubmitDXR settles at its very end.
 */
static bool
weave_dmabuf_from_xr(const XrWeaveDmabufDescDXR *src, struct xrt_weave_dmabuf_desc *dst, const char **why)
{
	if (src->fd < 0) {
		*why = "fd must be >= 0";
		return false;
	}
	if (src->width == 0 || src->height == 0) {
		*why = "width and height must be non-zero";
		return false;
	}
	if (src->drmFourcc == 0) {
		*why = "drmFourcc must be a DRM_FORMAT_* code";
		return false;
	}
	if (src->drmModifier == OXR_WEAVE_DRM_FORMAT_MOD_INVALID) {
		*why = "drmModifier must not be DRM_FORMAT_MOD_INVALID (resolve an implicit modifier first)";
		return false;
	}
	if (src->planeCount < 1 || src->planeCount > XR_WEAVE_DMABUF_MAX_PLANES_DXR) {
		*why = "planeCount must be 1..XR_WEAVE_DMABUF_MAX_PLANES_DXR";
		return false;
	}
	memset(dst, 0, sizeof(*dst));
	dst->fd = src->fd;
	dst->width = src->width;
	dst->height = src->height;
	dst->drm_fourcc = src->drmFourcc;
	dst->drm_modifier = src->drmModifier;
	dst->plane_count = src->planeCount;
	for (uint32_t i = 0; i < src->planeCount; i++) {
		dst->offsets[i] = src->offsets[i];
		dst->strides[i] = src->strides[i];
	}
	dst->buffer_id = src->bufferId;
	return true;
}

/*!
 * The v9 contract for a caller that did not chain XrWeaveOutputSyncDXR: the
 * output must be complete when xrWeaveSubmitDXR returns, so wait the release
 * sync_file on the CPU (a sync_file polls readable once signalled), then close
 * it. Bounded at 1 s like the Android engine's own wait; a timeout is logged
 * once and the frame is still returned — the next submit's acquire covers the
 * reuse hazard on the service side.
 */
static void
weave_wait_and_close_release_fd(int fd)
{
	if (fd < 0) {
		return;
	}
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	int r;
	do {
		r = poll(&pfd, 1, 1000);
	} while (r < 0 && errno == EINTR);
	if (r == 0) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W("xrWeaveSubmitDXR: release fence not signalled after 1 s — returning the frame "
			        "anyway (#1699)");
		}
	}
	close(fd);
}
#endif // XRT_OS_LINUX_DESKTOP

//! IPC sessions hold a native-compositor handle (the IPC client compositor) but
//! none of the in-process native-compositor flags are set. Mirrors oxr_capture.c.
static bool
session_is_ipc(struct oxr_session *sess)
{
	if (sess == NULL || sess->xcn == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		return false;
	}
	bool inprocess = sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                 sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                 sess->is_vk_native_compositor;
	return !inprocess;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveBindWindowDXR(XrSession session, void *windowHandle)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveBindWindowDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveBindWindowDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_bind_window(
	    &sess->xcn->base, (uint64_t)(uintptr_t)windowHandle);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindowDXR: bind failed (xrt_result=%d)", (int)xret);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveBindWindow2DXR(XrSession session, const XrWeaveBindWindowInfoDXR *bindInfo)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveBindWindow2DXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, bindInfo, XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveBindWindow2DXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	// The handle half is exactly xrWeaveBindWindowDXR (NULL is legal — Android
	// has no window handle at all and binds by geometry alone).
	xrt_result_t xret = comp_ipc_client_compositor_weave_bind_window(
	    &sess->xcn->base, (uint64_t)(uintptr_t)bindInfo->windowHandle);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindow2DXR: bind failed (xrt_result=%d)", (int)xret);

	// Spec v7 (#1036): explicit client-area geometry on the panel. Optional on
	// Windows / macOS (the runtime can read it off the window handle there),
	// required on Android. Forwarded to the DP's per-window phase slot.
	const XrWeaveWindowGeometryDXR *geom =
	    OXR_GET_INPUT_FROM_CHAIN(bindInfo, XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR, XrWeaveWindowGeometryDXR);
	if (geom != NULL) {
		if (geom->clientSize.width <= 0 || geom->clientSize.height <= 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveBindWindow2DXR: XrWeaveWindowGeometryDXR::clientSize "
			                 "(%dx%d) must be positive",
			                 geom->clientSize.width, geom->clientSize.height);
		}
		xret = comp_ipc_client_compositor_weave_set_window_geometry(
		    &sess->xcn->base, geom->windowOriginOnScreen.x, geom->windowOriginOnScreen.y,
		    (uint32_t)geom->clientSize.width, (uint32_t)geom->clientSize.height, geom->displayId);
		OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveBindWindow2DXR: geometry update failed (xrt_result=%d)",
		                   (int)xret);
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSubmitDXR(XrSession session, const XrWeaveSubmitInfoDXR *submitInfo, XrWeaveOutputDXR *output)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSubmitDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, submitInfo, XR_TYPE_WEAVE_SUBMIT_INFO_DXR);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, output, XR_TYPE_WEAVE_OUTPUT_DXR);

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSubmitDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	// Spec v3 batched submit: a chained XrWeaveSubmitRectsDXR switches the
	// input-layout contract (window-sized input, content at each rect's own
	// window position; base rect ignored). Absent chain = legacy single-rect,
	// byte-equivalent to spec v2.
	uint32_t rect_count = 0;
	struct xrt_rect rects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	const XrWeaveSubmitRectsDXR *batch =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_RECTS_DXR, XrWeaveSubmitRectsDXR);
	if (batch != NULL) {
		if (batch->rectCount < 1 || batch->rectCount > XR_WEAVE_SUBMIT_MAX_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitRectsDXR::rectCount (%u) must be "
			                 "1..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (%u)",
			                 batch->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
		}
		OXR_VERIFY_ARG_NOT_NULL(&log, batch->rects);
		rect_count = batch->rectCount;
		for (uint32_t i = 0; i < rect_count; i++) {
			// xrt_offset names its fields w/h (see the @todo in xrt_defines.h);
			// they are x/y here.
			rects[i].offset.w = batch->rects[i].offset.x;
			rects[i].offset.h = batch->rects[i].offset.y;
			rects[i].extent.w = (int)batch->rects[i].extent.width;
			rects[i].extent.h = (int)batch->rects[i].extent.height;
		}
	}

	// Spec v4 (browser#18): a chained XrWeaveSubmitOverlaysDXR carries a
	// window-sized premul-RGBA overlay atlas (a second shared texture) the DP
	// composites over the woven output; overlayCount 0 = whole atlas.
	xrt_graphics_buffer_handle_t overlay_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	bool overlay_is_dxgi = false;
	uint32_t overlay_rect_count = 0;
	struct xrt_rect overlay_rects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	const XrWeaveSubmitOverlaysDXR *ov =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR, XrWeaveSubmitOverlaysDXR);
	if (ov != NULL && ov->overlayTexture != NULL) {
		if (ov->rectCount > XR_WEAVE_SUBMIT_MAX_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitOverlaysDXR::rectCount (%u) must be "
			                 "0..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (%u)",
			                 ov->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
		}
		if (ov->rectCount > 0) {
			OXR_VERIFY_ARG_NOT_NULL(&log, ov->rects);
		}
		overlay_handle = (xrt_graphics_buffer_handle_t)(intptr_t)ov->overlayTexture; // fd int on Linux
		overlay_is_dxgi = ov->overlayIsDxgi == XR_TRUE;
		overlay_rect_count = ov->rectCount;
		for (uint32_t i = 0; i < overlay_rect_count; i++) {
			overlay_rects[i].offset.w = ov->rects[i].offset.x;
			overlay_rects[i].offset.h = ov->rects[i].offset.y;
			overlay_rects[i].extent.w = (int)ov->rects[i].extent.width;
			overlay_rects[i].extent.h = (int)ov->rects[i].extent.height;
		}
	}

	// Spec v7 (#1036): a chained XrWeaveSubmitHandlesDXR declares what the
	// handles ARE. It changes no layout contract — it only lets us reject a
	// cross-platform mistake here instead of dereferencing an alien pointer in
	// the service. PLATFORM_DEFAULT (and an absent chain) = pre-v7 behaviour.
	const XrWeaveSubmitHandlesDXR *kinds =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR, XrWeaveSubmitHandlesDXR);
	if (kinds != NULL) {
		if (!weave_handle_kind_ok(kinds->inputKind)) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitHandlesDXR::inputKind (%d) is not a "
			                 "handle kind this platform accepts",
			                 (int)kinds->inputKind);
		}
		if (overlay_handle != XRT_GRAPHICS_BUFFER_HANDLE_INVALID && !weave_handle_kind_ok(kinds->overlayKind)) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitHandlesDXR::overlayKind (%d) is not a "
			                 "handle kind this platform accepts",
			                 (int)kinds->overlayKind);
		}
	}

	// Spec v8 (browser#88): a chained XrWeaveSubmitFlatRegionsDXR names the
	// regions of this submit that must be PHYSICALLY FLAT. The service subtracts
	// them from the weave rects to derive the per-region hardware wish it
	// publishes to the DP. Purely advisory and hardware-only (ADR-027 D6 /
	// ADR-030) — it gates no content, so validation here is bounds only and an
	// absent chain is byte-for-byte pre-v8.
	uint32_t flat_rect_count = 0;
	struct xrt_rect flat_rects[XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR];
	const XrWeaveSubmitFlatRegionsDXR *flat = OXR_GET_INPUT_FROM_CHAIN(
	    submitInfo, XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR, XrWeaveSubmitFlatRegionsDXR);
	if (flat != NULL && flat->rectCount > 0) {
		if (flat->rectCount > XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitFlatRegionsDXR::rectCount (%u) must be "
			                 "0..XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR (%u)",
			                 flat->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR);
		}
		OXR_VERIFY_ARG_NOT_NULL(&log, flat->rects);
		flat_rect_count = flat->rectCount;
		for (uint32_t i = 0; i < flat_rect_count; i++) {
			// xrt_offset names its fields w/h; they are x/y here.
			flat_rects[i].offset.w = flat->rects[i].offset.x;
			flat_rects[i].offset.h = flat->rects[i].offset.y;
			flat_rects[i].extent.w = (int)flat->rects[i].extent.width;
			flat_rects[i].extent.h = (int)flat->rects[i].extent.height;
		}
	}

	// Spec v6 (#774): a chained XrWeaveSubmitLayoutDXR declares that the input
	// is a worst-case-sized N-view atlas (tiles packed contiguously from the
	// top-left at contentViewWidth/Height) instead of per-rect squeezed SBS.
	// Absent chain = legacy layout, byte-for-byte unchanged.
	struct xrt_weave_atlas_layout layout = {0};
	const XrWeaveSubmitLayoutDXR *lay =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR, XrWeaveSubmitLayoutDXR);
	if (lay != NULL) {
		if (lay->tileColumns == 0 || lay->tileRows == 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::tileColumns (%u) and "
			                 "tileRows (%u) must both be non-zero",
			                 lay->tileColumns, lay->tileRows);
		}
		if (lay->viewCount == 0 || lay->viewCount > XR_WEAVE_MAX_EYES_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::viewCount (%u) must be "
			                 "1..%u",
			                 lay->viewCount, (uint32_t)XR_WEAVE_MAX_EYES_DXR);
		}
		if (lay->viewCount != lay->tileColumns * lay->tileRows) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR::viewCount (%u) must equal "
			                 "tileColumns * tileRows (%u * %u)",
			                 lay->viewCount, lay->tileColumns, lay->tileRows);
		}
		if (lay->contentViewWidth == 0 || lay->contentViewHeight == 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: XrWeaveSubmitLayoutDXR content view dims "
			                 "(%ux%u) must both be non-zero",
			                 lay->contentViewWidth, lay->contentViewHeight);
		}
		layout.view_count = lay->viewCount;
		layout.tile_columns = lay->tileColumns;
		layout.tile_rows = lay->tileRows;
		layout.content_view_w = lay->contentViewWidth;
		layout.content_view_h = lay->contentViewHeight;
	}

	/*
	 * Spec v10 (#1699): desktop-Linux dma-buf transport. An input descriptor
	 * selects the DMABUF kind (fd + DRM fourcc + modifier + plane layout instead
	 * of an opaque pointer); the sync chain adds an acquire sync_file. The OUT
	 * chains (XrWeaveOutputDmabufDXR / XrWeaveOutputSyncDXR) are portable —
	 * every platform writes -1 into what it has nothing for — so they are read
	 * unconditionally below.
	 */
	const XrWeaveDmabufDescDXR *dmabuf_in =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_DMABUF_DESC_DXR, XrWeaveDmabufDescDXR);
	const XrWeaveOverlayDmabufDescDXR *dmabuf_ov = OXR_GET_INPUT_FROM_CHAIN(
	    submitInfo, XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR, XrWeaveOverlayDmabufDescDXR);
	const XrWeaveSubmitSyncDXR *sync_in =
	    OXR_GET_INPUT_FROM_CHAIN(submitInfo, XR_TYPE_WEAVE_SUBMIT_SYNC_DXR, XrWeaveSubmitSyncDXR);
	XrWeaveOutputDmabufDXR *out_dmabuf =
	    OXR_GET_OUTPUT_FROM_CHAIN(output, XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR, XrWeaveOutputDmabufDXR);
	XrWeaveOutputSyncDXR *out_sync =
	    OXR_GET_OUTPUT_FROM_CHAIN(output, XR_TYPE_WEAVE_OUTPUT_SYNC_DXR, XrWeaveOutputSyncDXR);

#ifndef XRT_OS_LINUX_DESKTOP
	if (dmabuf_in != NULL || dmabuf_ov != NULL || sync_in != NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrWeaveSubmitDXR: XrWeaveDmabufDescDXR / XrWeaveOverlayDmabufDescDXR / "
		                 "XrWeaveSubmitSyncDXR are desktop-Linux only (spec v10)");
	}
#else
	struct xrt_weave_dmabuf_desc in_desc = {0};
	struct xrt_weave_dmabuf_desc ov_desc = {0};
	int acquire_fd = -1;
	if (kinds != NULL) {
		// DMABUF needs its descriptor; OPAQUE_FD (and every non-Linux kind,
		// already refused above) contradicts one.
		if (kinds->inputKind == XR_WEAVE_HANDLE_KIND_DMABUF_DXR && dmabuf_in == NULL) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: inputKind DMABUF requires a chained XrWeaveDmabufDescDXR");
		}
		if (dmabuf_in != NULL && kinds->inputKind != XR_WEAVE_HANDLE_KIND_DMABUF_DXR &&
		    kinds->inputKind != XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: a chained XrWeaveDmabufDescDXR requires inputKind DMABUF "
			                 "or PLATFORM_DEFAULT (got %d)",
			                 (int)kinds->inputKind);
		}
		if (dmabuf_ov != NULL && kinds->overlayKind != XR_WEAVE_HANDLE_KIND_DMABUF_DXR &&
		    kinds->overlayKind != XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: a chained XrWeaveOverlayDmabufDescDXR requires overlayKind "
			                 "DMABUF or PLATFORM_DEFAULT (got %d)",
			                 (int)kinds->overlayKind);
		}
		if (kinds->overlayKind == XR_WEAVE_HANDLE_KIND_DMABUF_DXR && ov != NULL && dmabuf_ov == NULL) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: overlayKind DMABUF requires a chained "
			                 "XrWeaveOverlayDmabufDescDXR");
		}
	}
	if (dmabuf_in == NULL && (dmabuf_ov != NULL || sync_in != NULL)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrWeaveSubmitDXR: XrWeaveOverlayDmabufDescDXR and XrWeaveSubmitSyncDXR are only "
		                 "valid with a dma-buf input (XrWeaveDmabufDescDXR)");
	}
	if (dmabuf_in != NULL) {
		const char *why = "";
		if (!weave_dmabuf_from_xr(dmabuf_in, &in_desc, &why)) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "xrWeaveSubmitDXR: XrWeaveDmabufDescDXR: %s",
			                 why);
		}
		// One transport per submit: a dma-buf input takes a dma-buf overlay.
		if (overlay_handle != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "xrWeaveSubmitDXR: with a dma-buf input the overlay must be an "
			                 "XrWeaveOverlayDmabufDescDXR (overlayTexture must be NULL)");
		}
		if (dmabuf_ov != NULL) {
			if (ov == NULL) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "xrWeaveSubmitDXR: XrWeaveOverlayDmabufDescDXR needs the "
				                 "XrWeaveSubmitOverlaysDXR it describes");
			}
			if (ov->rectCount > XR_WEAVE_SUBMIT_MAX_RECTS_DXR) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "xrWeaveSubmitDXR: XrWeaveSubmitOverlaysDXR::rectCount (%u) must be "
				                 "0..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (%u)",
				                 ov->rectCount, (uint32_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
			}
			overlay_rect_count = ov->rectCount; // hint count only; rects never cross the wire
			if (!weave_dmabuf_from_xr((const XrWeaveDmabufDescDXR *)(const void *)dmabuf_ov, &ov_desc, &why)) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "xrWeaveSubmitDXR: XrWeaveOverlayDmabufDescDXR: %s", why);
			}
		}
		if (sync_in != NULL) {
			if (sync_in->acquireFenceFd < -1) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "xrWeaveSubmitDXR: XrWeaveSubmitSyncDXR::acquireFenceFd (%d) must be "
				                 ">= 0 or -1",
				                 sync_in->acquireFenceFd);
			}
			acquire_fd = sync_in->acquireFenceFd;
		}
	}
#endif

	bool have_out = false;
	uint32_t w = 0, h = 0;
	uint64_t fence_value = 0;
	struct xrt_eye_positions eyes = {0};
	int release_fd = -1; // v10: per-frame release sync_file, owned by us until handed out
	xrt_result_t xret;
#ifdef XRT_OS_LINUX_DESKTOP
	if (dmabuf_in != NULL) {
		// The bridge sends copies of every fd (SCM_RIGHTS); the caller's
		// originals are closed only once this call is certain to return
		// XR_SUCCESS (bottom of this function) — the spec's Vulkan-style rule.
		xret = comp_ipc_client_compositor_weave_submit_dmabuf(
		    &sess->xcn->base, &in_desc, dmabuf_ov != NULL ? &ov_desc : NULL, acquire_fd,
		    submitInfo->rect.offset.x, submitInfo->rect.offset.y, (uint32_t)submitInfo->rect.extent.width,
		    (uint32_t)submitInfo->rect.extent.height, rect_count, rect_count > 0 ? rects : NULL,
		    overlay_rect_count, submitInfo->firstChunk == XR_TRUE, layout.view_count > 0 ? &layout : NULL,
		    flat_rect_count, flat_rect_count > 0 ? flat_rects : NULL, &release_fd, &have_out, &w, &h,
		    &fence_value, &eyes);
	} else
#endif
	{
		xret = comp_ipc_client_compositor_weave_submit(
		    &sess->xcn->base, (xrt_graphics_buffer_handle_t)(intptr_t)submitInfo->inputTexture,
		    submitInfo->inputIsDxgi == XR_TRUE, submitInfo->rect.offset.x, submitInfo->rect.offset.y,
		    (uint32_t)submitInfo->rect.extent.width, (uint32_t)submitInfo->rect.extent.height, rect_count,
		    rect_count > 0 ? rects : NULL, overlay_handle, overlay_is_dxgi, overlay_rect_count,
		    overlay_rect_count > 0 ? overlay_rects : NULL, submitInfo->firstChunk == XR_TRUE,
		    layout.view_count > 0 ? &layout : NULL, flat_rect_count, flat_rect_count > 0 ? flat_rects : NULL,
		    &have_out, &w, &h, &fence_value, &eyes);
	}
	if (xret == XRT_ERROR_FEATURE_NOT_SUPPORTED) {
		// The service has no weave engine for this platform (desktop Linux
		// built without XRT_FEATURE_COMP_MULTI_WEAVE_LINUX, #1699): permanent,
		// not a retry — the same answer an in-process session gets. No other
		// platform's service ever returns it for a submit.
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSubmitDXR: this runtime's service carries no weave engine");
	}
	// XRT_ERROR_WEAVE_REFUSED (the service said "not this frame") lands on the
	// non-fatal branch; only a dead pipe loses the session. See the file header.
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSubmitDXR: weave failed (xrt_result=%d)", (int)xret);

	// Per-frame scalars are always valid; the shared HANDLEs are handed back
	// only on the first submit and on re-allocation (resize → dims change).
	output->weavedTexture = NULL;
	output->width = w;
	output->height = h;
	output->fence = NULL;
	output->fenceValue = fence_value;
	if (out_dmabuf != NULL) {
		// Steady-state default; filled below on the export frames only.
		out_dmabuf->fd = -1;
		out_dmabuf->width = 0;
		out_dmabuf->height = 0;
		out_dmabuf->drmFourcc = 0;
		out_dmabuf->drmModifier = 0;
		out_dmabuf->planeCount = 0;
		memset(out_dmabuf->offsets, 0, sizeof(out_dmabuf->offsets));
		memset(out_dmabuf->strides, 0, sizeof(out_dmabuf->strides));
		out_dmabuf->size = 0;
	}
	if (out_sync != NULL) {
		out_sync->releaseFenceFd = -1;
	}

	// Eyes flow OUT: the caller renders its NEXT pre-weave frame's off-axis
	// projection from these tracked positions (look-around). The interlace
	// itself is DP-internal.
	uint32_t ec = eyes.count;
	if (ec > XR_WEAVE_MAX_EYES_DXR) {
		ec = XR_WEAVE_MAX_EYES_DXR;
	}
	output->eyeCount = ec;
	for (uint32_t i = 0; i < ec; i++) {
		output->eyes[i].x = eyes.eyes[i].x;
		output->eyes[i].y = eyes.eyes[i].y;
		output->eyes[i].z = eyes.eyes[i].z;
	}
	output->eyesValid = eyes.valid ? XR_TRUE : XR_FALSE;
	output->eyesTracking = eyes.is_tracking ? XR_TRUE : XR_FALSE;

	bool need_export = !sess->weave.exported || w != sess->weave.last_w || h != sess->weave.last_h;
	if (have_out && w != 0 && h != 0 && need_export) {
		bool have_tex = false;
		bool got_tex = false;
#ifdef XRT_OS_LINUX_DESKTOP
		// v10: a caller that chained XrWeaveOutputDmabufDXR gets the woven output
		// as a typed dma-buf (independent of the INPUT's kind); one that did not
		// keeps the stage-A OPAQUE_FD export in weavedTexture below.
		struct xrt_weave_dmabuf_output_desc od = {0};
		od.fd = -1;
		if (out_dmabuf != NULL) {
			xret = comp_ipc_client_compositor_weave_get_output_dmabuf(&sess->xcn->base, &have_tex, &od);
			if (xret != XRT_SUCCESS && release_fd >= 0) {
				close(release_fd); // not handing out a frame: drop its fence
			}
			OXR_CHECK_XRET_MSG(&log, sess, xret,
			                   "xrWeaveSubmitDXR: weave dma-buf output export failed (xrt_result=%d)", (int)xret);
			got_tex = have_tex && od.fd >= 0;
			if (got_tex) {
				out_dmabuf->fd = od.fd; // caller-owned from here (spec v10)
				out_dmabuf->width = od.width;
				out_dmabuf->height = od.height;
				out_dmabuf->drmFourcc = od.drm_fourcc;
				out_dmabuf->drmModifier = od.drm_modifier;
				out_dmabuf->planeCount = od.plane_count;
				for (uint32_t i = 0; i < od.plane_count && i < XR_WEAVE_DMABUF_MAX_PLANES_DXR; i++) {
					out_dmabuf->offsets[i] = od.offsets[i];
					out_dmabuf->strides[i] = od.strides[i];
				}
				out_dmabuf->size = od.size;
			}
		} else
#endif
		{
			uint32_t gw = 0, gh = 0;
			xrt_graphics_buffer_handle_t tex_h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
			xret = comp_ipc_client_compositor_weave_get_output(&sess->xcn->base, &have_tex, &gw, &gh, &tex_h);
#ifdef XRT_OS_LINUX_DESKTOP
			if (xret != XRT_SUCCESS && release_fd >= 0) {
				close(release_fd);
			}
#endif
			// #1427: a designed "nothing to hand out" is XRT_SUCCESS with
			// have_tex == false (Windows before the output exists; Android while
			// the #1277 weave satellite presents it itself —
			// ipc_handle_weave_get_output never turns that into an error), and
			// stays a retry on the next frame. A real transport failure is NOT
			// that: XRT_ERROR_IPC_FAILURE must reach the caller as
			// XR_ERROR_INSTANCE_LOST like every other bridge here, instead of
			// being swallowed by an `== XRT_SUCCESS` test as "no handle".
			OXR_CHECK_XRET_MSG(&log, sess, xret,
			                   "xrWeaveSubmitDXR: weave output export failed (xrt_result=%d)", (int)xret);
			got_tex = have_tex && tex_h != XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
			if (got_tex) {
				// (intptr_t hop: on desktop Linux the handle is an OPAQUE_FD int.)
				output->weavedTexture = (void *)(intptr_t)tex_h;
			}
		}

		bool have_fence = false;
		xrt_graphics_sync_handle_t fence_h = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		xret = comp_ipc_client_compositor_weave_get_fence(&sess->xcn->base, &have_fence, &fence_h);
#ifdef XRT_OS_LINUX_DESKTOP
		if (xret != XRT_SUCCESS) {
			// Not handing out this frame: drop what it already produced.
			if (release_fd >= 0) {
				close(release_fd);
			}
			if (out_dmabuf != NULL && out_dmabuf->fd >= 0) {
				close(out_dmabuf->fd);
				out_dmabuf->fd = -1;
			}
		}
#endif
		OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSubmitDXR: weave fence export failed (xrt_result=%d)",
		                   (int)xret);
		bool got_fence = have_fence && fence_h != XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		if (got_fence) {
			// (intptr_t hop: the handle is an fd int on POSIX — macOS never
			// exports a fence (#759, completion is synchronous), and desktop
			// Linux's fence is v10's per-frame XrWeaveOutputSyncDXR instead, so
			// this branch fires on Windows HANDLEs only.)
			output->fence = (void *)(intptr_t)fence_h;
		}

		// #1427: arm the one-shot latch ONLY when every export this platform is
		// expected to produce actually produced a handle — the texture
		// everywhere, plus the fence on Windows (the caller cannot safely sample
		// the woven texture without it). Latching on a miss is what turned a
		// transient into a NULL weavedTexture for the life of the session.
		// The rule itself lives in oxr_weave_latch.h so it can be pinned on the
		// host without a service. v10's per-frame release fence is deliberately
		// NOT part of it (see oxr_weave_latch.h).
		if (oxr_weave_should_latch_export(oxr_weave_platform_exports_fence(), got_tex, got_fence)) {
			sess->weave.exported = true;
			sess->weave.last_w = w;
			sess->weave.last_h = h;
			// One-off (never per-frame): pairs with the miss WARN below, so a
			// field log shows the whole latch class in two lines.
			if (sess->weave.warned_export_miss && !sess->weave.warned_export_recovered) {
				sess->weave.warned_export_recovered = true;
				U_LOG_W(
				    "xrWeaveSubmitDXR: weave handle export RECOVERED at %ux%u — the caller now "
				    "has the woven texture%s (#1427).",
				    w, h, got_fence ? " + fence" : "");
			}
		} else if (!sess->weave.warned_export_miss) {
			// One-off (never per-frame): the per-frame retry is otherwise
			// silent, so this is the only trace that a session ever ran without
			// a woven handle.
			sess->weave.warned_export_miss = true;
			U_LOG_W(
			    "xrWeaveSubmitDXR: weave handle export produced nothing at %ux%u (texture=%s, "
			    "fence=%s) — NOT latching, will retry every frame (#1427).",
			    w, h, got_tex ? "yes" : "no", got_fence ? "yes" : "no");
		}
	}

#ifdef XRT_OS_LINUX_DESKTOP
	// v10 release fence, PER FRAME: to the caller if it asked for it, otherwise
	// honour the v9 synchronous contract for it by waiting here.
	if (out_sync != NULL) {
		out_sync->releaseFenceFd = release_fd; // caller-owned (or -1 = already complete)
	} else {
		weave_wait_and_close_release_fd(release_fd);
	}

	// XR_SUCCESS is now certain: the fds the caller passed IN are ours. The
	// service holds its own copies, so these originals are simply closed.
	if (dmabuf_in != NULL) {
		close(in_desc.fd);
		if (dmabuf_ov != NULL) {
			close(ov_desc.fd);
		}
		if (acquire_fd >= 0) {
			close(acquire_fd);
		}
	}
#else
	(void)release_fd;
#endif

	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSnapWindowRectDXR(XrSession session,
                             const XrRect2Di *originRect,
                             const XrRect2Di *targetRect,
                             XrRect2Di *snappedRect)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSnapWindowRectDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_NOT_NULL(&log, originRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, targetRect);
	OXR_VERIFY_ARG_NOT_NULL(&log, snappedRect);

	// Only the top-left is phase-snapped; the size passes through unchanged.
	// On no DP snap support every route below returns the target unchanged, so
	// the result is always a valid rect.
	int32_t sx = targetRect->offset.x, sy = targetRect->offset.y;

	if (!session_is_ipc(sess)) {
		/*
		 * In-process route (#1588). Unlike every other entry point here
		 * this one needs no weave service: it moves no pixels and owns no
		 * transport — it asks the display processor a question. The window
		 * owner on desktop Linux is the APP (an X11 handle app dragging its
		 * own undecorated window), and it is in-process by construction, so
		 * refusing here would put the snap out of reach of the only caller
		 * that can act on it.
		 */
#if defined(XRT_HAVE_VK_NATIVE_COMPOSITOR) && defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
		if (sess->is_vk_native_compositor) {
			// Identity when the DP has no snap_window_rect slot
			// (sim_display, any plug-in built before #1588) — the outputs
			// are written either way, so the return value only says
			// whether the position moved.
			(void)comp_vk_native_compositor_snap_window_rect(&sess->xcn->base, originRect->offset.x,
			                                                 originRect->offset.y, targetRect->offset.x,
			                                                 targetRect->offset.y, &sx, &sy);
			snappedRect->offset.x = sx;
			snappedRect->offset.y = sy;
			snappedRect->extent = targetRect->extent;
			return XR_SUCCESS;
		}
#endif
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSnapWindowRectDXR: no in-process snap route for this session's "
		                 "compositor (the rest of the weave service is out-of-process only)");
	}

	bool snapped = false;
	xrt_result_t xret = comp_ipc_client_compositor_weave_snap_window_rect(
	    &sess->xcn->base, originRect->offset.x, originRect->offset.y, targetRect->offset.x, targetRect->offset.y,
	    &snapped, &sx, &sy);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSnapWindowRectDXR: snap failed (xrt_result=%d)", (int)xret);

	snappedRect->offset.x = sx;
	snappedRect->offset.y = sy;
	snappedRect->extent = targetRect->extent;
	return XR_SUCCESS;
}

#if defined(XRT_HAVE_VK_NATIVE_COMPOSITOR) && defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
//! u_snap_grid_point_fn over the in-process vk_native DP's per-point snap.
static bool
grid_point_vk_native(void *userdata,
                     int32_t origin_x,
                     int32_t origin_y,
                     int32_t target_x,
                     int32_t target_y,
                     int32_t *out_x,
                     int32_t *out_y)
{
	return comp_vk_native_compositor_snap_window_rect((struct xrt_compositor *)userdata, origin_x, origin_y,
	                                                  target_x, target_y, out_x, out_y);
}
#endif

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSnapWindowGridDXR(XrSession session,
                             const XrWeaveSnapGridInfoDXR *gridInfo,
                             uint32_t pointCapacityInput,
                             uint32_t *pointCountOutput,
                             XrWeaveSnapGridPointDXR *points,
                             XrBool32 *declined)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSnapWindowGridDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, gridInfo, XR_TYPE_WEAVE_SNAP_GRID_INFO_DXR);
	OXR_VERIFY_ARG_NOT_NULL(&log, pointCountOutput);

	// Same shape the IPC wire and every service arm validate (u_snap_grid.h):
	// bounded before anything is allocated or looped.
	const struct u_snap_grid grid = {
	    .origin_x = gridInfo->originRect.offset.x,
	    .origin_y = gridInfo->originRect.offset.y,
	    .first_x = gridInfo->firstTarget.x,
	    .first_y = gridInfo->firstTarget.y,
	    .step_x = gridInfo->step.width,
	    .step_y = gridInfo->step.height,
	    .count_x = gridInfo->countX,
	    .count_y = gridInfo->countY,
	};
	const char *why = NULL;
	if (!u_snap_grid_validate(&grid, &why)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(gridInfo) %s", why);
	}
	const uint32_t n = u_snap_grid_point_count(&grid);
	*pointCountOutput = n;
	if (pointCapacityInput == 0) {
		return XR_SUCCESS; // two-call idiom: the count only, nothing evaluated
	}
	if (pointCapacityInput < n) {
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "(pointCapacityInput == %u) need %u",
		                 pointCapacityInput, n);
	}
	OXR_VERIFY_ARG_NOT_NULL(&log, points);
	// XrWeaveSnapGridPointDXR is exactly the {int8 dx, int8 dy} pair
	// u_snap_grid writes, so the loop fills the caller's array directly.
	int8_t *out = (int8_t *)points;
	bool dec = false;

	if (!session_is_ipc(sess)) {
		// In-process: the same route the per-point call takes (#1588), looped
		// here. Anywhere that call reports FEATURE_UNSUPPORTED, so does this.
#if defined(XRT_HAVE_VK_NATIVE_COMPOSITOR) && defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
		if (sess->is_vk_native_compositor) {
			u_snap_grid_eval(&grid, grid_point_vk_native, &sess->xcn->base, out, &dec, NULL, NULL);
			if (declined != NULL) {
				*declined = dec ? XR_TRUE : XR_FALSE;
			}
			return XR_SUCCESS;
		}
#endif
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSnapWindowGridDXR: no in-process snap route for this session's "
		                 "compositor (the rest of the weave service is out-of-process only)");
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_snap_window_grid(&sess->xcn->base, &grid, out, &dec);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSnapWindowGridDXR: grid snap failed (xrt_result=%d)", (int)xret);
	if (declined != NULL) {
		*declined = dec ? XR_TRUE : XR_FALSE;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSetScreenFlatRegionsDXR(XrSession session, uint32_t rectCount, const XrRect2Di *screenRects)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrWeaveSetScreenFlatRegionsDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_weave);

	if (rectCount > XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrWeaveSetScreenFlatRegionsDXR: rectCount (%u) must be "
		                 "0..XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR (%u)",
		                 rectCount, (uint32_t)XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR);
	}
	// rectCount 0 CLEARS the latch, so screenRects is only required when there is
	// something to read.
	if (rectCount > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, screenRects);
	}

	if (!session_is_ipc(sess)) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveSetScreenFlatRegionsDXR: the weave service is only available on the "
		                 "out-of-process (service) path");
	}

	struct xrt_rect rects[XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR];
	for (uint32_t i = 0; i < rectCount; i++) {
		// xrt_offset names its fields w/h; they are x/y here. Absolute PHYSICAL
		// screen pixels — unlike the per-submit list these are not
		// window-relative, so no origin is applied on this side.
		rects[i].offset.w = screenRects[i].offset.x;
		rects[i].offset.h = screenRects[i].offset.y;
		rects[i].extent.w = (int)screenRects[i].extent.width;
		rects[i].extent.h = (int)screenRects[i].extent.height;
	}

	xrt_result_t xret = comp_ipc_client_compositor_weave_set_screen_flat_regions(&sess->xcn->base, rectCount,
	                                                                            rectCount > 0 ? rects : NULL);
	OXR_CHECK_XRET_MSG(&log, sess, xret, "xrWeaveSetScreenFlatRegionsDXR: latch failed (xrt_result=%d)", (int)xret);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveExportIpcConnectionDXR(XrInstance instance, XrWeaveIpcConnectionDXR *connection)
{
	OXR_TRACE_MARKER();

	struct oxr_instance *inst = NULL;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrWeaveExportIpcConnectionDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_weave);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, connection, XR_TYPE_WEAVE_IPC_CONNECTION_DXR);

	connection->handle = NULL;
	connection->fd = -1;

	// Instance-level, so there is no session to read session_is_ipc() off — and
	// deliberately so: a broker is not required to be a present-owner, and may
	// well export before it ever creates a session. Reject only when we can
	// POSITIVELY prove this instance is in-process (a system compositor exists
	// and says so); otherwise let the connect itself be the test.
	struct xrt_system_compositor *xsysc = inst->system.xsysc;
	if (xsysc != NULL && !xsysc->info.is_service_mode) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "xrWeaveExportIpcConnectionDXR: there is no IPC connection to export on an "
		                 "in-process session");
	}

	xrt_ipc_handle_t exported = XRT_IPC_HANDLE_INVALID;
	xrt_result_t xret = ipc_client_connection_export(u_log_get_global_level(), &exported);
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "xrWeaveExportIpcConnectionDXR: could not open a service endpoint "
		                 "(xrt_result=%d)",
		                 (int)xret);
	}

#ifdef XRT_OS_WINDOWS
	connection->handle = (void *)exported;
#else
	connection->fd = (int32_t)exported;
#endif
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_weave
