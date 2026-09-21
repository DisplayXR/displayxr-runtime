// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-view camera selection and the shared layer-composition policy.
 * @author David Fattal
 * @ingroup comp_util
 *
 * Two things live here, and both for the same reason: the answer must not
 * depend on which backend is asking.
 *
 *  1. the per-view CAMERA every layer type is projected through (#1580), and
 *  2. the composition POLICY a renderer applies once it has that camera —
 *     eye visibility (@ref is_layer_view_visible_n), quad facing
 *     (@ref comp_layer_quad_is_front_facing, #1590), the src/dst blend rule
 *     (@ref comp_layer_blend_mode, #1599) and the painter's-order first-layer
 *     gate (@ref comp_layer_tile_blend_mode, #1598/#1600).
 *
 * Every one of them is a PREDICATE the renderer consults, never a pipeline
 * state it inherits — see comp_layer_quad_is_front_facing() for why facing in
 * particular cannot be rasterizer culling here.
 *
 * #1580 — THE INVARIANT: one camera per view per frame, shared by every layer
 * type. That camera is the {pose, fov} pair `xrLocateViews` handed the app,
 * expressed in the compositor's layer frame; because the projection layer is
 * drawn as an identity-MVP fullscreen blit, the view tile IS that frustum, so
 * quad / cylinder / equirect / cube layers must be projected through the SAME
 * one or their world pose lands on different display pixels than the
 * projection content at that pose.
 *
 * THE LAYER FRAME is the head device's TRACKING-ORIGIN ("root") space
 * (#1594/#1607). `handle_space()` in oxr_session_frame_end.c resolves EVERY
 * layer pose — `data.quad.pose` and `data.proj.v[i].pose` alike — into it
 * through one call on the head device, VIEW reference spaces included (the
 * #1502 eye-centroid offset rides along as that locate's base offset). So a
 * projection layer's per-view pose, and the frame cameras the state tracker
 * carries on @ref xrt_layer_frame_data, need no re-basing.
 *
 * What DOES need re-basing is the display processor's eye: the DP reports eyes
 * relative to the head (the display plane), so branch (b) below lifts one into
 * the layer frame with `xrt_layer_frame_data::head_pose` (`T_root_head`). Before
 * #1594 the layer frame WAS head-relative and no lift was needed; a branch-(b)
 * camera left head-relative today puts VIEW quads right and LOCAL quads a whole
 * head pose wrong.
 *
 * This lives in comp_util (Vulkan-free, C) so every backend — D3D11, D3D12,
 * Metal, GL, vk_native — consumes ONE implementation instead of each renderer
 * inventing a camera.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stddef.h> // NULL
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
	/*!
	 * The frame's own cameras, carried on @ref xrt_layer_frame_data by the
	 * state tracker: LITERALLY what `xrLocateViews` reported for this
	 * frame's display time, not a re-derivation of it.
	 */
	COMP_LAYER_VIEW_CAMERA_FROM_FRAME = 3,
};

/*!
 * A per-view camera: what a renderer needs to place a 3D-positioned layer.
 *
 * @ingroup comp_util
 */
struct comp_layer_view_camera
{
	//! View pose in the layer frame — ROOT, see the file comment.
	struct xrt_pose pose;
	//! Signed, possibly asymmetric (Kooima off-axis) FOV angles, radians.
	struct xrt_fov fov;
	//! Which branch produced this camera.
	enum comp_layer_view_camera_source source;
	/*!
	 * #1594: true when @ref pose could NOT be lifted into the layer frame
	 * and is therefore still HEAD-RELATIVE — the frame carried no valid
	 * `xrt_layer_frame_data::head_pose`, so branches (b) and (c) fell back
	 * to what they returned before #1594. Never silent: the same condition
	 * emits one process-lifetime `U_LOG_W`.
	 *
	 * Always false on branches (a) and (b'), whose poses come from the state
	 * tracker already in the layer frame.
	 */
	bool head_relative_fallback;
};

/*!
 * Resolve the camera for one view of one frame (#1580).
 *
 * Priority:
 *  (a) the frame's FIRST projection-class layer (projection, projection+depth
 *      or a 3D zone) that covers @p view_index → its `data.proj.v[view].pose`
 *      and `.fov` verbatim. This is the app's own camera, already in the
 *      layer space, so nothing is re-based. FIRST because the projection draw
 *      is an identity-MVP blit: the view tile IS that frustum, so anything
 *      composed beside it must use the same one even if the frame also carries
 *      cameras.
 *  (b') else, the frame's own cameras on `accum->data` (@ref
 *      xrt_layer_frame_data::cameras), when `cameras_valid`. These are what
 *      `xrLocateViews` reported for this frame — the state tracker measured
 *      them; nothing here re-derives them. This is the ONLY branch that can be
 *      right when the session runs a camera-centric rig (the qwerty default,
 *      or a chained XR_DXR_view_rig), whose frustum is a fixed vFOV sheared by
 *      the convergence and has no relation to the panel's Kooima frustum.
 *  (b) else, if an eye position and a canvas size are known → identity
 *      orientation at @p eye_pos, FOV from `dxr_display3d_compute_fov()`,
 *      the whole pose then lifted out of head-relative space into the layer
 *      frame by `accum->data.head_pose` (#1594). A best-effort DISPLAY-centric
 *      reconstruction, kept for callers with no frame data (and for a session
 *      whose locate had no valid pose bits yet).
 *  (c) else the legacy placeholder camera ({∓0.032, 0, 0}, symmetric ±0.785
 *      rad) plus one process-lifetime U_LOG_W naming the fallback. Never
 *      logged per frame.
 *
 * @param accum      The frame's accumulated layers (may be NULL → skip (a)).
 * @param view_index View to resolve.
 * @param eye_pos    This view's eye, relative to the HEAD device (the display
 *                   plane) exactly as the DP reports it — not in the layer
 *                   frame (nullable → skip (b)). Branch (b) uses it verbatim as
 *                   the view pose position and then lifts the result by
 *                   `head_pose`.
 * @param canvas_w_m Canvas (window) width in metres; <= 0 → skip (b).
 * @param canvas_h_m Canvas (window) height in metres; <= 0 → skip (b).
 * @param[out] out   ALWAYS fully populated when non-NULL, on every branch.
 *
 * @return true when the camera came from real data ((a), (b') or (b)); false
 *         when branch (c) supplied the placeholder.
 *
 * @warning NEVER gate the draw on the return value. @p out is usable on every
 *          branch, so `if (!select(...)) continue;` turns a fallback frame into
 *          a silently empty one. The only genuine failure is @p out == NULL,
 *          which a caller passing a stack object cannot hit. (#1581.)
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
 * at @p eye_pos (then lifted by `head_pose`, #1594). The FOV is
 * origin-independent, so a zero offset and a rebased eye agree.
 *
 * @param canvas_center Canvas centre in the SAME head-relative space as
 *                      @p eye_pos (nullable → the origin, i.e. identical to
 *                      @ref comp_layer_view_camera_select).
 *
 * @warning The return value is a diagnostic, not a validity flag — see
 *          @ref comp_layer_view_camera_select. Never gate the draw on it.
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
 * @copybrief comp_layer_view_camera_select
 *
 * The full-fidelity entry point: takes the DP's WHOLE per-view eye set plus the
 * frame's ACTIVE view count, and derives branch (b)'s render eye with exactly
 * the two rules the state tracker applies before it computes the pose and FOV
 * it hands the app at `xrLocateViews`. Both rules are invisible to a caller
 * that only has a left/right pair, and both make the compositor's camera
 * disagree with the located view when they bite:
 *
 *  - **Mono collapse.** When the active rendering mode has ONE view but the DP
 *    still reports N >= 2 eyes, the render eye is the CENTROID of that set, not
 *    eye 0. Pairing the centred pose oxr reports with eye 0's off-axis frustum
 *    is the 2D lateral shift of modelviewer#100; the collapse lives in
 *    `oxr_session_locate_views()` (oxr_session.c, the `active_view_count == 1`
 *    branch) in-process and in `ipc_try_get_sr_view_poses()`
 *    (ipc_server_handler.c, #521/#575) over IPC. This is its compositor twin.
 *
 *  - **Per-view eyes, surplus views clamped.** View i renders from eye i, not
 *    from a left/right pair — a 4-view (2x2 quad) mode has four DISTINCT eyes,
 *    two of them a full 64 mm apart vertically. Views past the reported eye
 *    count reuse the LAST eye, which is the same surplus-slot rule the state
 *    tracker's #615 eye-set coherence guard applies.
 *
 * @param eyes              The DP's per-view eye set, relative to the HEAD
 *                          device (nullable / count 0 -> skip (b)).
 * @param active_view_count The active rendering mode's view count; 0 means
 *                          "same as @p eyes->count" (no collapse).
 *
 * @warning The return value is a diagnostic, not a validity flag — see
 *          @ref comp_layer_view_camera_select. A NULL / empty eye set only
 *          skips branch (b); @p out is still populated and must still be
 *          drawn with. Never gate the draw on it.
 *
 * @ingroup comp_util
 */
bool
comp_layer_view_camera_select_eyes(const struct comp_layer_accum *accum,
                                   uint32_t view_index,
                                   const struct xrt_eye_positions *eyes,
                                   uint32_t active_view_count,
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

/*!
 * How a layer's texture alpha composites onto what is already in the tile.
 *
 * @ingroup comp_util
 */
enum comp_layer_blend_mode
{
	/*!
	 * The texture's alpha is IGNORED (treated as 1) and the layer covers
	 * what is under it: blending off, the source written verbatim.
	 *
	 * Verbatim, not "alpha forced to 1": the destination alpha the runtime
	 * hands the display processor is load-bearing (#225 — the DP lerps the
	 * desktop under the atlas alpha), and a transparent-background app's
	 * single projection layer reaches the atlas through exactly this mode.
	 * For the COLOUR channels the result is identical either way.
	 */
	COMP_LAYER_BLEND_REPLACE = 0,
	//! `out.rgb = src.rgb + dst.rgb * (1 - src.a)` — source already scaled.
	COMP_LAYER_BLEND_PREMULTIPLIED = 1,
	//! `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)` — straight alpha.
	COMP_LAYER_BLEND_STRAIGHT = 2,
};

/*!
 * The OpenXR §10.6.2 three-way blend rule, from a layer's composition flags.
 *
 * Two flags, three outcomes, and BOTH flags matter:
 *
 *  - no `XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
 *      → @ref COMP_LAYER_BLEND_REPLACE. The spec initialises the layer alpha
 *        to one, i.e. the layer is an opaque cover. This is NOT "blend it
 *        premultiplied", which is what the D3D11 in-process renderer used to
 *        do (#1599): an inverted two-way test that blended opaque layers and
 *        ignored the unpremultiplied bit entirely.
 *  - `SOURCE_ALPHA_BIT` alone → @ref COMP_LAYER_BLEND_PREMULTIPLIED.
 *  - `SOURCE_ALPHA_BIT` + `XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT`
 *      → @ref COMP_LAYER_BLEND_STRAIGHT.
 *
 * @param layer_flags @ref xrt_layer_data::flags.
 *
 * @ingroup comp_util
 */
enum comp_layer_blend_mode
comp_layer_blend_mode(uint32_t layer_flags);

/*!
 * Is a quad layer's front face turned toward the camera (#1590)?
 *
 * The spec is normative: *"Only front face of the quad surface is visible; the
 * back face is not visible and must not be drawn by the runtime"*, and a quad
 * with an identity rotation has *"its front face normal vector coinciding with
 * the +z axis"*. So the normal is `orientation * (0, 0, 1)` — **+Z**, not -Z —
 * and the quad is visible exactly when
 *
 *     dot(normal, camera_pos - quad_pos) > 0
 *
 * Edge-on (dot == 0) counts as NOT visible: a zero-area sliver whose facing is
 * undefined.
 *
 * A PREDICATE, deliberately, not rasterizer culling. Every pipeline in the tree
 * is created `CULL_NONE` (D3D11 in-process and service, D3D12, vk_native, and
 * GL explicitly `glDisable(GL_CULL_FACE)`), so switching to culling would mean
 * a per-backend winding audit for no benefit. In-tree precedent for the dot
 * product: the inherited compute path in shaders/layer.comp rejects a quad
 * backface the same way.
 *
 * @param quad_pose  The quad's pose, in the compositor's head-relative layer
 *                   space (@ref xrt_layer_quad_data::pose).
 * @param camera_pos The view's camera position in that SAME space — i.e.
 *                   @ref comp_layer_view_camera::pose.position for this view,
 *                   never a display-space eye.
 *
 * @return true when the quad must be drawn. NULL arguments return true: a
 *         missing input is a caller bug, and dropping content is the worse
 *         failure.
 *
 * @ingroup comp_util
 */
bool
comp_layer_quad_is_front_facing(const struct xrt_pose *quad_pose, const struct xrt_vec3 *camera_pos);

/*!
 * Painter's-order state for ONE tile (one view) of one frame (#1598).
 *
 * Zero-initialise per view per frame; a backend keeps one of these per tile it
 * paints and hands it to @ref comp_layer_tile_blend_mode for every layer it is
 * about to composite.
 *
 * @ingroup comp_util
 */
struct comp_layer_tile_state
{
	//! Has anything been composited into this tile this frame yet?
	bool composited;
};

/*!
 * Has nothing been composited into this tile yet?
 *
 * @ingroup comp_util
 */
static inline bool
comp_layer_is_first_in_tile(const struct comp_layer_tile_state *tile)
{
	return tile == NULL || !tile->composited;
}

/*!
 * Record that a layer was composited into this tile.
 *
 * @ingroup comp_util
 */
static inline void
comp_layer_tile_mark_composited(struct comp_layer_tile_state *tile)
{
	if (tile != NULL) {
		tile->composited = true;
	}
}

/*!
 * The blend mode for the next layer into a tile, and mark the tile painted.
 *
 * THE FIRST layer composited into a tile is a @ref COMP_LAYER_BLEND_REPLACE,
 * whatever its flags say, and only later layers blend per
 * @ref comp_layer_blend_mode. That gate is flag-INDEPENDENT on purpose:
 * transparent-background apps set `SOURCE_ALPHA_BIT` on their SINGLE
 * projection layer (every `cube_handle_*`, every `cube_zones_*`), and the
 * shaped / compose-under contract requires that first blit to be a replace so
 * the app's alpha reaches the atlas verbatim (#225). Blending it over the
 * clear instead would drive `dst.a` to 1 everywhere and silently kill the
 * display processor's alpha gate. As a side effect the single-layer frame
 * every shipping app submits keeps exactly the state it has today.
 *
 * Nothing is under the first layer but the clear, so REPLACE is also what the
 * spec's painter's algorithm reduces to there.
 *
 * @param tile        This view's tile state; NULL means "treat as first".
 * @param layer_flags @ref xrt_layer_data::flags.
 *
 * @ingroup comp_util
 */
static inline enum comp_layer_blend_mode
comp_layer_tile_blend_mode(struct comp_layer_tile_state *tile, uint32_t layer_flags)
{
	const bool first = comp_layer_is_first_in_tile(tile);
	comp_layer_tile_mark_composited(tile);
	return first ? COMP_LAYER_BLEND_REPLACE : comp_layer_blend_mode(layer_flags);
}

#ifdef __cplusplus
}
#endif
