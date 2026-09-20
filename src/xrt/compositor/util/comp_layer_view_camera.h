// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-view camera selection for non-projection composition layers.
 * @author David Fattal
 * @ingroup comp_util
 *
 * #1580 — THE INVARIANT: one camera per view per frame, shared by every layer
 * type. That camera is the {pose, fov} pair `xrLocateViews` handed the app,
 * expressed in the compositor's head-relative layer space; because the
 * projection layer is drawn as an identity-MVP fullscreen blit, the view tile
 * IS that frustum, so quad / cylinder / equirect / cube layers must be
 * projected through the SAME one or their world pose lands on different
 * display pixels than the projection content at that pose.
 *
 * Head-relative layer space: `handle_space()` in oxr_session_frame_end.c
 * resolves EVERY layer pose — `data.quad.pose` and `data.proj.v[i].pose`
 * alike — into the head device's space (and, for VIEW reference spaces,
 * composes the #1502 eye-centroid offset in exactly as a locate does). So a
 * projection layer's per-view pose is already in the space quad poses live in
 * and needs no re-basing.
 *
 * This lives in comp_util (Vulkan-free, C) so every backend — D3D11, D3D12,
 * Metal, GL, vk_native — consumes ONE implementation instead of each renderer
 * inventing a camera.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_layer_accum;

/*!
 * Where a resolved per-view camera came from.
 *
 * @ingroup comp_util
 */
enum comp_layer_view_camera_source
{
	//! The frame's first projection-class layer: the app's own camera.
	COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION = 0,
	//! Synthesized from the DP eye + canvas metres via the shared Kooima core.
	COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D = 1,
	//! Neither was available — the legacy ±32 mm / ±45° placeholder.
	COMP_LAYER_VIEW_CAMERA_FALLBACK = 2,
};

/*!
 * A per-view camera: what a renderer needs to place a 3D-positioned layer.
 *
 * @ingroup comp_util
 */
struct comp_layer_view_camera
{
	//! View pose in the head-relative layer space (see file comment).
	struct xrt_pose pose;
	//! Signed, possibly asymmetric (Kooima off-axis) FOV angles, radians.
	struct xrt_fov fov;
	//! Which branch produced this camera.
	enum comp_layer_view_camera_source source;
};

/*!
 * Resolve the camera for one view of one frame (#1580).
 *
 * Priority:
 *  (a) the frame's FIRST projection-class layer (projection, projection+depth
 *      or a 3D zone) that covers @p view_index → its `data.proj.v[view].pose`
 *      and `.fov` verbatim. This is the app's own camera, already in the
 *      layer space, so nothing is re-based.
 *  (b) else, if an eye position and a canvas size are known → identity
 *      orientation at @p eye_pos, FOV from `dxr_display3d_compute_fov()` — the
 *      SHARED Kooima core the state tracker itself runs, so the synthesized
 *      camera is the one `xrLocateViews` would have handed out.
 *  (c) else the legacy placeholder camera ({∓0.032, 0, 0}, symmetric ±0.785
 *      rad) plus one process-lifetime U_LOG_W naming the fallback. Never
 *      logged per frame.
 *
 * @param accum      The frame's accumulated layers (may be NULL → skip (a)).
 * @param view_index View to resolve.
 * @param eye_pos    This view's eye, in the head-relative layer space
 *                   (nullable → skip (b)). Used verbatim as the view pose
 *                   position in branch (b).
 * @param canvas_w_m Canvas (window) width in metres; <= 0 → skip (b).
 * @param canvas_h_m Canvas (window) height in metres; <= 0 → skip (b).
 * @param[out] out   ALWAYS fully populated when non-NULL, on every branch.
 *
 * @return true when the camera came from real data ((a) or (b)); false when
 *         branch (c) supplied the placeholder. @p out is valid either way —
 *         a false return is a diagnostic, not "don't draw".
 *
 * @ingroup comp_util
 */
bool
comp_layer_view_camera_select(const struct comp_layer_accum *accum,
                              uint32_t view_index,
                              const struct xrt_vec3 *eye_pos,
                              float canvas_w_m,
                              float canvas_h_m,
                              struct comp_layer_view_camera *out);

/*!
 * @copybrief comp_layer_view_camera_select
 *
 * Same resolver, plus the canvas centre — needed wherever the canvas is NOT
 * centred on the origin of the layer space (the in-process Windows path: the
 * head sits at the display-plane centre while the app's window sits at
 * `xrt_window_metrics::window_center_offset_*_m`). The FOV is a function of
 * the eye RELATIVE TO the canvas centre, so branch (b) rebases the eye by
 * @p canvas_center before calling the Kooima core — while the view POSE stays
 * at @p eye_pos, because that is the frame layer poses are expressed in. The
 * FOV is origin-independent, so a zero offset and a rebased eye agree.
 *
 * @param canvas_center Canvas centre in the head-relative layer space
 *                      (nullable → the origin, i.e. identical to
 *                      @ref comp_layer_view_camera_select).
 *
 * @ingroup comp_util
 */
bool
comp_layer_view_camera_select_ex(const struct comp_layer_accum *accum,
                                 uint32_t view_index,
                                 const struct xrt_vec3 *eye_pos,
                                 const struct xrt_vec3 *canvas_center,
                                 float canvas_w_m,
                                 float canvas_h_m,
                                 struct comp_layer_view_camera *out);

/*!
 * N-view eye-visibility, the generalisation of the stereo parity rule.
 *
 * `is_layer_view_visible()` in comp_render_helpers.h answers LEFT/RIGHT with
 * `view_index % 2`, which is right for 2 views and meaningless for a 2x2 quad
 * mode. Here the views are ordered left-to-right across the rig, and the two
 * halves OVERLAP by one view when N is odd, so the centre view is drawn for
 * BOTH eyes rather than for neither:
 *  - LEFT  bit -> `view_index < (view_count + 1) / 2`
 *  - RIGHT bit -> `view_index >= view_count / 2`
 *
 * For `view_count <= 2` the parity rule is used verbatim instead, so stereo
 * and mono behaviour is bit-for-bit what it always was.
 *
 * Lives here rather than in comp_render_helpers.h because that header pulls in
 * render_interface.h / comp_base.h (Vulkan) and the D3D11, D3D12, Metal and GL
 * renderers cannot include it; comp_render_helpers.h includes this one so the
 * name is reachable from both sides.
 *
 * @ingroup comp_util
 */
static inline bool
is_view_index_right_n(uint32_t view_index, uint32_t view_count)
{
	if (view_count <= 2) {
		return view_index % 2 == 1;
	}
	return view_index >= view_count / 2;
}

/*!
 * @copybrief is_view_index_right_n
 *
 * LEFT-eye side of the same rule.
 *
 * @ingroup comp_util
 */
static inline bool
is_view_index_left_n(uint32_t view_index, uint32_t view_count)
{
	if (view_count <= 2) {
		return view_index % 2 == 0;
	}
	return view_index < (view_count + 1) / 2;
}

/*!
 * View-count-aware eye visibility for a layer.
 *
 * Same answers as `is_layer_view_visible()` for 1- and 2-view frames; correct
 * for N > 2 (see @ref is_view_index_right_n). Projection-class layers are
 * always visible.
 *
 * @ingroup comp_util
 */
static inline bool
is_layer_view_visible_n(const struct xrt_layer_data *data, uint32_t view_index, uint32_t view_count)
{
	enum xrt_layer_eye_visibility visibility;

	if (view_count == 0) {
		view_count = 2;
	}

	switch (data->type) {
	case XRT_LAYER_CUBE: visibility = data->cube.visibility; break;
	case XRT_LAYER_CYLINDER: visibility = data->cylinder.visibility; break;
	case XRT_LAYER_EQUIRECT1: visibility = data->equirect1.visibility; break;
	case XRT_LAYER_EQUIRECT2: visibility = data->equirect2.visibility; break;
	case XRT_LAYER_QUAD: visibility = data->quad.visibility; break;
	default: return true; // Projection-class layers are visible in every view.
	}

	switch (visibility) {
	case XRT_LAYER_EYE_VISIBILITY_LEFT_BIT: return is_view_index_left_n(view_index, view_count);
	case XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT: return is_view_index_right_n(view_index, view_count);
	case XRT_LAYER_EYE_VISIBILITY_BOTH: return true;
	case XRT_LAYER_EYE_VISIBILITY_NONE:
	default: return false;
	}
}

#ifdef __cplusplus
}
#endif
