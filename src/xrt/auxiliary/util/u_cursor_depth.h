// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth-aware cursor placement (XR_DXR_cursor_depth, ADR-046).
 *
 * Turns "the cursor is at canvas point (u,v) and the nearest content under it
 * is point P" into "draw a sprite of height h at position C", then smooths C
 * over time.
 *
 * Everything is solved from the views the runtime has just handed the app —
 * never from rig internals — so the result is correct for display rigs,
 * camera rigs, any m2v scale, display zones and any view count >= 2:
 *
 *  - the canvas point S under the cursor is where the two outermost view rays
 *    through (u,v) meet (Kooima frusta all frame the same canvas);
 *  - E, the cyclopean eye, is the midpoint of those two view positions;
 *  - a point's depth is t = its distance in front of E along the display
 *    normal, over S's (t = 1 on the canvas, t < 1 in front of it);
 *  - the policy works in DISPARITY d = 1 - 1/t, in units of the eye baseline
 *    (on-screen disparity = baseline * d): 0 on the canvas, < 0 in front.
 *    Disparity, not distance, is what the eye compares, so margins, clamps
 *    and slew rates are expressed there;
 *  - ONE POLICY (ADR-046 Amendment 2 A): the cursor is never behind the
 *    glass. target = d < 0 ? max(d - margin, d_min) : 0, margin 0.015, and
 *    nothing is drawn (the OS cursor is the cursor) while the filtered
 *    disparity is above the draw threshold, -0.005
 *    (u_cursor_depth_should_draw). The filter keeps stepping while nothing
 *    is drawn, so the drawn cursor hands back smoothly;
 *  - the sprite's height is scaled by t so its apparent size is constant;
 *  - WHERE laterally it goes is the anchor mode (spec v3):
 *      SCREEN  C = E + t (S - E): on the cyclopean ray. Its image on the glass
 *              ignores head motion - disparity but no motion parallax, which
 *              on a head-tracked display reads as "at the glass".
 *      WORLD   C = S - f (1 - t) e: straight in front of the pointer's canvas
 *              point along the normal (e = eye-to-canvas). Parallaxes like
 *              content, but sits ~mm off the click point off-axis.
 *      HYBRID  (default) SCREEN while the pointer moves - exact aim - storing
 *              that locate's line of sight (E0, S0, e0). While the pointer is
 *              still: C = E0 + s (S0 - E0), s = 1 - (1 - t) e / e0 - the point
 *              on the anchor-time line at the current depth in front of the
 *              glass. Continuous at the stop; a lateral head move keeps C
 *              world-fixed (parallax); a depth change slides C along the
 *              anchor-time line, so it stays on the click point. If the
 *              canvas PLANE moves (a camera rig re-converging) or s would
 *              not put C in front of the eye, it re-anchors (SCREEN).
 *
 * Pure C: time is a parameter, never read here.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Default sprite height, as a fraction of the canvas height.
#define U_CURSOR_DEPTH_DEFAULT_HEIGHT 0.03f

/*!
 * Default margin in front of content, eye-baseline units (Amendment 2 A.2):
 * about 1 mm of crossed on-screen disparity at a 63-65 mm IPD. The single
 * source for every runtime cursor path (in-process draw, zones, lifted video).
 * Halved from 0.03 on 2026-10-11 after the Windows Leia panel check: 0.03
 * floated visibly off the content.
 */
#define U_CURSOR_DEPTH_DEFAULT_MARGIN 0.015f

/*!
 * Default draw threshold, eye-baseline units (Amendment 2 A.3): a sprite is
 * drawn only while the filtered disparity is at or in front of this.
 *
 * The ADR also floors it at 0.5 px on the panel. The placement does not know
 * the panel's pixel pitch, so that floor is the drawing backend's to apply;
 * at a 65 mm IPD, -0.005 is ~0.33 mm, above 0.5 px on every panel shipped.
 */
#define U_CURSOR_DEPTH_DRAW_THRESHOLD (-0.005f)

/*!
 * One located view: the pose and fov exactly as xrLocateViews returns them.
 */
struct u_cursor_depth_view
{
	struct xrt_pose pose;
	struct xrt_fov fov;
};

/*!
 * The cursor's line of sight, solved for one locate.
 */
struct u_cursor_depth_geometry
{
	struct xrt_vec3 eye;          //!< E: cyclopean eye (midpoint of the outer views)
	struct xrt_vec3 canvas_point; //!< S: the canvas point under the cursor
	struct xrt_vec3 forward;      //!< display normal pointing AWAY from the viewer
	struct xrt_quat orientation;  //!< display-plane orientation (the views')
	float eye_to_canvas;          //!< dot(S - E, forward), > 0
	float canvas_height;          //!< canvas height in locate-space units
};

/*!
 * Placement policy. Disparities are in eye-baseline units (see file comment).
 */
struct u_cursor_depth_tuning
{
	//! How far in front of the content the cursor floats (disparity, > 0).
	float margin;
	//! Most crossed disparity the cursor may reach (comfort clamp, < 0).
	float min_disparity;
	//! Upper clamp, <= 0. Default 0 (the canvas): never behind the glass.
	//! Values > 0 are treated as 0 (Amendment 2 A.1).
	float max_disparity;
	//! Draw only while the filtered disparity is <= this (< 0; default
	//! U_CURSOR_DEPTH_DRAW_THRESHOLD; >= 0 or non-finite = the default).
	float draw_threshold;
	//! Time constant when rising toward the viewer, seconds (fast: never lag behind content).
	float rise_tau_s;
	//! Time constant when sinking away from the viewer, seconds (slow: no flicker at edges).
	float sink_tau_s;
	//! A gap between steps longer than this re-primes (snaps) the filter, seconds.
	float stale_s;
};

/*!
 * The time filter's state. Zero-initialise; the first step snaps.
 */
struct u_cursor_depth_filter
{
	bool primed;
	float disparity;
	uint64_t last_ns;
};

/*!
 * Where the sprite goes laterally (XR_DXR_cursor_depth spec v3). The values
 * equal the XrCursorDepthAnchorModeDXR ones.
 */
enum u_cursor_depth_anchor_mode
{
	//! SCREEN while the pointer moves, world-fixed while it is still. Default.
	U_CURSOR_DEPTH_ANCHOR_HYBRID = 0,
	//! On the cyclopean ray through the pointer's canvas point (spec v1/v2).
	U_CURSOR_DEPTH_ANCHOR_SCREEN = 1,
	//! In front of the pointer's canvas point along the display normal.
	U_CURSOR_DEPTH_ANCHOR_WORLD = 2,
};

/*!
 * HYBRID anchor state. Zero-initialise; reset whenever the filter re-primes.
 */
struct u_cursor_depth_anchor
{
	bool has_anchor;
	//! E0: the cyclopean eye of the last moving placement.
	struct xrt_vec3 eye;
	//! S0: the canvas point under the pointer at the last moving placement.
	struct xrt_vec3 canvas_point;
	//! e0: E0's distance to the canvas along the normal then (> 0).
	float eye_to_canvas;
	//! The canvas normal (view forward) then. With S0 it pins the canvas
	//! plane the anchor was taken against: if the plane moves (a camera rig
	//! re-converging, a rig re-pose), the anchor is re-taken.
	struct xrt_vec3 forward;
	//! Pointer UV of that placement.
	float last_u, last_v;
	//! Time of that placement.
	uint64_t last_ns;
};

//! Pointer UV change beyond which HYBRID treats the pointer as moving.
#define U_CURSOR_DEPTH_ANCHOR_MOVE_EPS 1e-4f

/*!
 * How far the canvas plane may move along its normal, as a fraction of the
 * eye-to-canvas distance, before HYBRID re-anchors. A head move leaves the
 * plane where it is (to float noise); a camera rig that re-converges, or a
 * rig re-pose, moves it - and the anchor-time line of sight then belongs to a
 * different canvas: s = 1 - (1 - t) e / e0 goes negative once the canvas
 * recedes past e0 / (1 - t), which puts C BEHIND the eye.
 */
#define U_CURSOR_DEPTH_ANCHOR_PLANE_EPS 1e-3f

void
u_cursor_depth_tuning_defaults(struct u_cursor_depth_tuning *t);

/*!
 * Solve the cursor's line of sight from two located views (normally view 0 and
 * the last active view). (u, v) is canvas-normalised, origin top-left, v down.
 *
 * @return false on degenerate input: coincident or parallel views, (u,v)
 *         outside [0,1], non-finite values, or a canvas not in front of E.
 */
bool
u_cursor_depth_geometry_solve(const struct u_cursor_depth_view *a,
                              const struct u_cursor_depth_view *b,
                              float u,
                              float v,
                              struct u_cursor_depth_geometry *out);

/*!
 * Disparity of a point (eye-baseline units, 0 on the canvas, < 0 in front).
 *
 * @return false if the point is not in front of the eye.
 */
bool
u_cursor_depth_point_disparity(const struct u_cursor_depth_geometry *g, const struct xrt_vec3 *p, float *out_disparity);

/*!
 * The unfiltered target (ADR-046 Amendment 2 A.1):
 * content_disparity < 0 ? max(content_disparity - margin, min_disparity) : 0,
 * and never above 0 for any tuning. 0 (the canvas) when there is no content
 * under the cursor, or the content is on or behind the canvas.
 */
float
u_cursor_depth_target(const struct u_cursor_depth_tuning *t, bool has_content, float content_disparity);

/*!
 * Amendment 2 A.3: should a sprite be drawn at this FILTERED disparity?
 * True only at or in front of the draw threshold. While false the OS cursor
 * is the cursor; keep stepping the filter regardless so the hand-back is
 * smooth.
 */
bool
u_cursor_depth_should_draw(const struct u_cursor_depth_tuning *t, float filtered_disparity);

/*!
 * True if the next u_cursor_depth_filter_step() at @p now_ns will re-prime
 * (snap) rather than smooth: first step, clock going backwards, or a gap
 * longer than t->stale_s. HYBRID drops its anchor on exactly these steps.
 */
bool
u_cursor_depth_filter_will_prime(const struct u_cursor_depth_filter *f,
                                 const struct u_cursor_depth_tuning *t,
                                 uint64_t now_ns);

/*!
 * Advance the filter toward @p target and return the new disparity. Calls with
 * a time not after the previous one do not advance it (a second locate in the
 * same frame is idempotent).
 */
float
u_cursor_depth_filter_step(struct u_cursor_depth_filter *f,
                           const struct u_cursor_depth_tuning *t,
                           float target,
                           uint64_t now_ns);

/*!
 * Where to draw a sprite at @p disparity, and how tall, for a requested height
 * of @p height_fraction of the canvas. SCREEN anchoring (on the cyclopean ray).
 */
void
u_cursor_depth_place(const struct u_cursor_depth_geometry *g,
                     float disparity,
                     float height_fraction,
                     struct xrt_vec3 *out_position,
                     float *out_height);

/*!
 * u_cursor_depth_place() with an anchor mode (spec v3).
 *
 * @param anchor     HYBRID state; may be NULL for SCREEN and WORLD (untouched).
 * @param u, v       the pointer UV this placement is for.
 * @param reprimed   the filter re-primed on this step (HYBRID re-anchors).
 * @param now_ns     the step's time (recorded in the anchor).
 *
 * Height is the same in every mode. An unknown mode behaves as HYBRID.
 */
void
u_cursor_depth_place_anchored(const struct u_cursor_depth_geometry *g,
                              float disparity,
                              float height_fraction,
                              enum u_cursor_depth_anchor_mode mode,
                              struct u_cursor_depth_anchor *anchor,
                              float u,
                              float v,
                              bool reprimed,
                              uint64_t now_ns,
                              struct xrt_vec3 *out_position,
                              float *out_height);


/*
 *
 * Phase 3a: the depth-layer source (XR_DXR_cursor_depth spec v2).
 *
 * The app submits depth with its projection layer (XR_KHR_composition_layer_
 * depth) and asks the runtime to find the nearest content under the cursor in
 * it. The compositor copies a cursor-sized patch of the two outermost views'
 * depth and reduces it to the nearest texel (u_cursor_depth_reduce_patch);
 * the state tracker turns that texel into a point in the layer's space
 * (u_cursor_depth_point_from_depth_sample) and feeds it to the v1 placement.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0). A compositor reads nothing,
 * allocates nothing and copies nothing unless the request it was handed
 * passes u_cursor_depth_patch_should_sample(): a zero-initialised request -
 * which is what every session that never chained XrCursorDepthSourceDXR has -
 * never does.
 *
 */

//! Most views a patch request covers (the outermost pair).
#define U_CURSOR_DEPTH_PATCH_VIEWS 2

/*!
 * Largest patch side, in texels. A ~1.5x-sprite footprint is ~50 texels on a
 * 4K panel's tile; clamping it smaller would shrink the footprint and let a
 * nearer edge beside the hotspot cut through the sprite again. 2 views x 64 x
 * 64 x 4 bytes = 32 KB per read, the most a request can ever copy.
 */
#define U_CURSOR_DEPTH_PATCH_MAX_DIM 64

/*!
 * What the state tracker asks a compositor to read at the next commit.
 *
 * Zero-initialised means "not requested".
 */
struct u_cursor_depth_patch_request
{
	//! True only for a commit whose frame chained the request.
	bool requested;
	//! Echoed in the result so the state tracker can match its frame records.
	uint64_t tag;
	//! Cursor hotspot, canvas-normalised (origin top-left, v down).
	float u, v;
	//! Footprint half-size, canvas-normalised, per axis (> 0).
	float radius_u, radius_v;
	//! Reversed-Z layer (nearZ > farZ): the nearest texel is the LARGEST value.
	bool nearest_is_max;
	//! Projection-layer view indices to read (the outermost active pair).
	uint32_t view_index[U_CURSOR_DEPTH_PATCH_VIEWS];
};

/*!
 * One view's answer: the nearest texel of the patch.
 */
struct u_cursor_depth_patch_sample
{
	bool valid;
	//! Texel centre, normalised to the view's depth sub-image (origin top-left).
	float su, sv;
	//! The raw window depth stored in that texel.
	float raw_depth;
};

/*!
 * What the compositor hands back, asynchronously (one or more frames later).
 */
struct u_cursor_depth_patch_result
{
	bool valid;
	uint64_t tag;
	struct u_cursor_depth_patch_sample view[U_CURSOR_DEPTH_PATCH_VIEWS];
};

/*!
 * The XrCompositionLayerDepthInfoKHR range values of one view.
 */
struct u_cursor_depth_layer_depth
{
	float min_depth; //!< window depth at distance near_z
	float max_depth; //!< window depth at distance far_z
	float near_z;    //!< distance of min_depth (may exceed far_z: reversed Z; may be +inf)
	float far_z;     //!< distance of max_depth (may be +inf)
};

/*!
 * The compositor-side gate. False for a zero-initialised (never requested)
 * request, and for one whose cursor is off the canvas or whose footprint is
 * degenerate - the compositor must then do no work at all.
 */
bool
u_cursor_depth_patch_should_sample(const struct u_cursor_depth_patch_request *req);

/*!
 * The texel rectangle to copy for one view: the footprint around (u, v)
 * mapped into the sub-image rect, clamped to it and to
 * U_CURSOR_DEPTH_PATCH_MAX_DIM per side.
 *
 * @return false if the rectangle is empty.
 */
bool
u_cursor_depth_patch_rect(const struct u_cursor_depth_patch_request *req,
                          int32_t sub_x,
                          int32_t sub_y,
                          int32_t sub_w,
                          int32_t sub_h,
                          int32_t *out_x,
                          int32_t *out_y,
                          int32_t *out_w,
                          int32_t *out_h);

/*!
 * Reduce a copied patch of float depth texels to the nearest one. NaN and
 * infinite texels are skipped.
 *
 * @param row_stride  texels between the starts of two rows (>= w)
 * @return false if no texel was usable.
 */
bool
u_cursor_depth_reduce_patch(const float *texels,
                            int32_t w,
                            int32_t h,
                            int32_t row_stride,
                            bool nearest_is_max,
                            int32_t *out_x,
                            int32_t *out_y,
                            float *out_raw);

/*!
 * Window depth -> distance along the view's -Z axis, for the hyperbolic
 * mapping every perspective projection writes: 1/z is linear in the window
 * depth, 1/near_z at min_depth and 1/far_z at max_depth. Covers ordinary Z,
 * reversed Z (near_z > far_z) and an infinite far (or, reversed, near) plane.
 *
 * @return false for the far end of the mapping itself (a cleared depth
 *         buffer: background, not content), values outside
 *         [min_depth, max_depth], and degenerate ranges.
 */
bool
u_cursor_depth_linear_depth(const struct u_cursor_depth_layer_depth *d, float raw_depth, float *out_z);

/*!
 * A depth texel -> the 3D point it shows, in the space of @p view's pose.
 * (su, sv) are normalised to the view's sub-image (origin top-left, v down),
 * which every Kooima frustum maps linearly onto its fov tangents.
 */
bool
u_cursor_depth_point_from_depth_sample(const struct u_cursor_depth_view *view,
                                       const struct u_cursor_depth_layer_depth *d,
                                       float su,
                                       float sv,
                                       float raw_depth,
                                       struct xrt_vec3 *out_point);


/*
 *
 * Spec v5: the runtime draws the cursor (ADR-046 Amendment 2 B).
 *
 * The state tracker turns the locate's placement (the sprite's hotspot point
 * C, the display-plane orientation, and the t-scaled canvas height) into, per
 * submitted view, where C lands in that view's tile and how one unit of
 * sprite size maps there (u_cursor_draw_view_from_placement). The compositor
 * then picks the sprite (the app's swapchain, or its default look), its size
 * in metres, aspect and hotspot, and draws one textured quad per tile
 * (u_cursor_draw_corners). Every Kooima view's image plane is parallel to the
 * display plane and the sprite lies in a plane parallel to it, so each view
 * sees the sprite as a scaled, translated rectangle: the per-view map is
 * exactly affine, and three projected points describe it.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0): a compositor draws nothing,
 * allocates nothing and builds no pipeline unless the request it was handed
 * passes u_cursor_draw_should_draw(), which a zero-initialised request - what
 * every session that never chained XrCursorDepthDrawInfoDXR has - never does.
 *
 */

//! The default look's height, in OS points (the classic arrow; the web SDK's
//! CURSOR_ARROW_CSS_PX).
#define U_CURSOR_DRAW_DEFAULT_POINTS 19.0f

//! The classic arrow's box, in px of its 19 px height: 12 x 19, tip at (0, 0).
#define U_CURSOR_ARROW_WIDTH_PX 12.0f
#define U_CURSOR_ARROW_HEIGHT_PX 19.0f

struct xrt_swapchain;

/*!
 * Why a compositor drew no cursor for a request it was handed (#1952). The
 * state tracker checks everything it can see before it hands a request over
 * (u_cursor_draw_placement_drawable); these are the reasons only the
 * compositor can see.
 */
enum u_cursor_draw_refusal
{
	U_CURSOR_DRAW_REFUSAL_NONE = 0,
	//! The commit consumed nothing, or never reached its cursor pass (an
	//! early-out frame, a path with no cursor pass). The state tracker's
	//! default, so a compositor that says nothing has refused.
	U_CURSOR_DRAW_REFUSAL_NOT_REACHED,
	//! The request failed u_cursor_draw_should_draw() compositor-side.
	U_CURSOR_DRAW_REFUSAL_INVALID_REQUEST,
	//! A zones frame with a non-zone request, or the reverse.
	U_CURSOR_DRAW_REFUSAL_ZONE_MISMATCH,
	//! The zone rect cannot be placed in the target (u_cursor_draw_into_zone).
	U_CURSOR_DRAW_REFUSAL_ZONE_UNPLACEABLE,
	//! A multi-screen routed frame: a tile is a per-segment mosaic.
	U_CURSOR_DRAW_REFUSAL_MULTI_SCREEN,
	//! No panel size / pixel pitch, or no canvas height, to size the sprite.
	U_CURSOR_DRAW_REFUSAL_NO_DISPLAY_INFO,
	//! The app's sprite swapchain image cannot be sampled (empty, layered
	//! where the backend cannot sample layers, or no view of it).
	U_CURSOR_DRAW_REFUSAL_SPRITE_UNUSABLE,
	//! The default look could not be built or uploaded.
	U_CURSOR_DRAW_REFUSAL_DEFAULT_LOOK,
	//! The cursor shaders / pipeline / descriptors could not be created.
	U_CURSOR_DRAW_REFUSAL_NO_PIPELINE,
	//! The frame took a path that has no compose pass to draw into
	//! (zero-copy, or a compose pass that is unavailable).
	U_CURSOR_DRAW_REFUSAL_NO_COMPOSE_PASS,
	//! No tile (or no tile area inside the clip) to draw into.
	U_CURSOR_DRAW_REFUSAL_NO_TILES,
	//! DXR_CURSOR_DRAW_REFUSE / _PERIOD_MS (verification hook).
	U_CURSOR_DRAW_REFUSAL_FORCED,
	U_CURSOR_DRAW_REFUSAL_COUNT,
};

/*!
 * A compositor's answer to one request: drew (or, for a report-only request,
 * would have drawn), or refused and why. Zero-initialised = no answer.
 */
struct u_cursor_draw_report
{
	bool answered;
	bool drew;
	enum u_cursor_draw_refusal reason; //!< meaningful when !drew
};

/*!
 * One view's map from sprite space to its tile, in tile-normalised
 * coordinates (origin top-left, x right, y DOWN; 0..1 spans the tile).
 */
struct u_cursor_draw_view
{
	//! Where the placement point (the sprite's hotspot) lands.
	float origin[2];
	//! Tile displacement of one unit along the sprite's +X (display right).
	float axis_x[2];
	//! Tile displacement of one unit along the sprite's +Y (display UP).
	float axis_y[2];
};

/*!
 * What the state tracker asks a compositor to draw at the next commit.
 * Zero-initialised means "not requested".
 *
 * One unit (the view axes) is the canvas height scaled by the placement's t,
 * so a sprite @c h metres tall on the panel is drawn h / canvas_height_m
 * units tall, and keeps a constant apparent size as it rises.
 */
struct u_cursor_draw_request
{
	//! True only for a commit whose frame chained the draw request.
	bool requested;
	//! Views mapped below; view i is drawn into atlas tile i.
	uint32_t view_count;
	struct u_cursor_draw_view view[XRT_MAX_VIEWS];
	//! The app's sprite swapchain (sampled at image_index, its last released
	//! image), or NULL for the default look.
	struct xrt_swapchain *image;
	uint32_t image_index;
	//! The click point in the sprite, normalised (origin top-left, v down).
	//! Ignored for the default look (it brings its own hotspot).
	float hot_u, hot_v;
	//! Sprite HEIGHT on the panel, metres; <= 0 = the OS cursor's size.
	float size_m;

	/*!
	 * XR_DXR_display_zones (#1949): the hint rode the zone-scoped locate of
	 * this 3D zone, and the views above are that zone's submitted views - so
	 * they are ZONE-normalised (0..1 spans the zone's sub-rect of each tile)
	 * and one unit is the ZONE's height. The compositor re-expresses them in
	 * its tile with u_cursor_draw_into_zone() and clips the draw to the zone.
	 * False (zero-initialised) = the whole tile, as before.
	 */
	bool zone_valid;
	//! The app's zone id (debug / logs only).
	uint32_t zone_id;
	//! The zone rect as submitted, client-window pixels, origin top-left -
	//! the same space every compositor places XRT_LAYER_ZONE_3D layers in.
	struct xrt_rect zone_rect;

	/*!
	 * #1952: where the compositor reports what it did with THIS request
	 * (u_cursor_draw_report_drew / _refused), during the commit that
	 * consumes it. Owned by the state tracker, which outlives the commit.
	 * NULL = nobody is listening.
	 */
	struct u_cursor_draw_report *report;
	/*!
	 * #1952: run every check, report the outcome, but draw nothing. The
	 * locate already told the app willDraw = false (an earlier frame was
	 * refused), so the app shows its OS cursor; drawing the sprite too
	 * would show two cursors. Probing instead of skipping is what lets a
	 * frame that WOULD draw restore willDraw = true without the report
	 * oscillating frame to frame.
	 */
	bool report_only;
};

/*!
 * The compositor-side gate: false for a zero-initialised request, fewer than
 * two views, or non-finite geometry - the compositor then does no work.
 */
bool
u_cursor_draw_should_draw(const struct u_cursor_draw_request *req);

/*!
 * #1952, compositor side: the request was drawn (or, report-only, would have
 * been). No-op for a request nobody listens to.
 */
void
u_cursor_draw_report_drew(const struct u_cursor_draw_request *req);

/*!
 * #1952, compositor side: the request was refused for @p reason. No-op for a
 * request nobody listens to. A later call for the same request overwrites an
 * earlier one, so the last word of the commit is what the state tracker sees.
 */
void
u_cursor_draw_report_refused(const struct u_cursor_draw_request *req, enum u_cursor_draw_refusal reason);

//! A short name for @p reason (logs).
const char *
u_cursor_draw_refusal_name(enum u_cursor_draw_refusal reason);

/*!
 * #1952, state-tracker side: the willDraw latch for compositor refusals.
 * Zero-initialise. A refusal latches it (the next locate reports willDraw =
 * false); a commit that drew - or, report-only, would have - clears it.
 */
struct u_cursor_draw_latch
{
	bool refused;
	//! Reasons already warned about (bit = enum value): one WARN per reason.
	uint32_t warned;
};

/*!
 * Arm @p report before handing a request over: unanswered, and NOT_REACHED
 * if read as is - a commit that never gets to its cursor pass refused.
 */
void
u_cursor_draw_report_begin(struct u_cursor_draw_report *report);

/*!
 * Fold one commit's @p report into the latch. An unanswered report counts as
 * a NOT_REACHED refusal (u_cursor_draw_report_begin).
 *
 * @return true if this is the first refusal for its reason: the caller logs
 *         one WARN (never per frame).
 */
bool
u_cursor_draw_latch_record(struct u_cursor_draw_latch *latch, const struct u_cursor_draw_report *report);

//! May the next locate report willDraw = true, as far as the compositor knows?
bool
u_cursor_draw_latch_allows(const struct u_cursor_draw_latch *latch);

/*!
 * Project a point (in the space of the view's pose) into the view's tile.
 *
 * @return false if the point is not in front of the view.
 */
bool
u_cursor_draw_project(const struct u_cursor_depth_view *view, const struct xrt_vec3 *p, float out_xy[2]);

/*!
 * One view's sprite map from the placement: @p center is the hotspot point,
 * @p orientation the display plane's (+X right, +Y up), @p unit the length of
 * one unit in the same space (the t-scaled canvas height).
 */
bool
u_cursor_draw_view_from_placement(const struct u_cursor_depth_view *view,
                                  const struct xrt_vec3 *center,
                                  const struct xrt_quat *orientation,
                                  float unit,
                                  struct u_cursor_draw_view *out);

/*!
 * THE drawability predicate (spec v5 willDraw contract): true only if a
 * placement maps to cursor pixels in these views - every view sees the
 * hotspot in front of its eye with a finite, non-degenerate sprite map
 * (u_cursor_draw_view_from_placement), and the hotspot lands inside at least
 * one view's tile (0..1, the zone's sub-rect for a zone-scoped locate).
 *
 * The state tracker evaluates it on the located views before it reports
 * willDraw = true, and again on the submitted views before it hands the
 * compositor a request, so the report and the draw cannot disagree.
 *
 * @param views      @p view_count views (>= 2), in the placement's space.
 * @param center     The hotspot point C.
 * @param orientation The display plane's orientation.
 * @param unit       One sprite unit (the t-scaled canvas height).
 * @param out_views  Optional: receives the per-view maps (@p view_count).
 */
bool
u_cursor_draw_placement_drawable(const struct u_cursor_depth_view *views,
                                 uint32_t view_count,
                                 const struct xrt_vec3 *center,
                                 const struct xrt_quat *orientation,
                                 float unit,
                                 struct u_cursor_draw_view *out_views);

/*!
 * The sprite quad's corners in the tile: a sprite @p height units tall and
 * @p aspect (width / height) wide, whose hotspot (@p hot_u, @p hot_v; origin
 * top-left, v down) lands on the view's origin. Corner order is the texture
 * UV order (0,0), (0,1), (1,0), (1,1) - top-left, bottom-left, top-right,
 * bottom-right: a triangle strip.
 */
void
u_cursor_draw_corners(
    const struct u_cursor_draw_view *v, float height, float aspect, float hot_u, float hot_v, float out_xy[4][2]);

/*!
 * XR_DXR_display_zones (#1949): re-express a zone request in TILE-normalised
 * coordinates, the way the compositor places the zone's own layer - the zone
 * rect scaled by tile / @p target (the window the zone rects are authored
 * against; in a zones frame the tile spans it).
 *
 * Rewrites every view of @p req in place (origin into the zone's sub-rect,
 * axes scaled by its extent) and returns the sub-rect, clipped to the tile, in
 * @p out_clip (x, y, w, h; tile-normalised, origin top-left): the draw must be
 * scissored to it, so a cursor at a zone edge never spills into a 2D zone or
 * a neighbouring 3D zone. A request without a zone is left untouched and
 * clips to the whole tile.
 *
 * @return false if the zone cannot be placed (no target size, or the zone lies
 *         entirely outside the window): draw nothing.
 */
bool
u_cursor_draw_into_zone(struct u_cursor_draw_request *req, uint32_t target_w, uint32_t target_h, float out_clip[4]);

/*!
 * The height, in the compositor's target pixels, of the canvas the request's
 * unit was built for: the zone's height for a zone request, else
 * @p canvas_px_h (the window / canvas the compositor already uses).
 */
uint32_t
u_cursor_draw_canvas_px_h(const struct u_cursor_draw_request *req, uint32_t canvas_px_h);

/*!
 * Rasterise the classic arrow (white fill, dark one-px outline, tip at the
 * top-left corner) into a @p w x @p h RGBA8 image with PREMULTIPLIED alpha.
 * The arrow's 12 x 19 box is scaled to the image height, so pass
 * w = ceil(h * 12 / 19). 4 x 4 supersampled.
 */
void
u_cursor_draw_arrow_rgba(uint8_t *rgba, uint32_t w, uint32_t h);

/*!
 * Recover a cursor image from two renders of it: one over opaque BLACK, one
 * over opaque WHITE. This is how the Windows default look reads ANY HCURSOR -
 * 32-bit alpha, colour + AND mask, or monochrome AND/XOR - without branching
 * on its type: whatever the OS draws, the two backgrounds reveal it.
 *
 * Per pixel, with b and w the two renders (bytes 0..2 colour, byte 3 ignored):
 * alpha = 1 - mean(w - b), and the render over black IS the premultiplied
 * colour (clamped to alpha). A pixel the cursor INVERTS (white over black,
 * black over white - the I-beam's XOR pixels) comes out opaque WHITE: a
 * compositor cannot invert what is behind it before the weave, and white is
 * the classic arrow's fill. Output: PREMULTIPLIED alpha (as the classic arrow
 * and comp_cursor_look), channels in the input's order.
 */
void
u_cursor_draw_unblend(const uint8_t *on_black, const uint8_t *on_white, uint32_t pixel_count, uint8_t *out);


/*
 *
 * Spec v7: measured disparity (ADR-046 Amendment 3, Phase 3b).
 *
 * Traditional stereo content (SBS video, stereo photos, camera pairs, legacy
 * stereo apps) has no depth - only the disparity between the views the app
 * submits. The runtime measures it under the cursor: the compositor copies a
 * small patch around the pointer out of the two OUTERMOST submitted views
 * (u_cursor_measure_make_plan picks the rectangle; a blit into a staging buffer,
 * read back a frame or two later, never waited on), and this module
 * block-matches it on the CPU (u_cursor_measure_run):
 *
 *  - luma, box-downsampled to a "work" resolution at most
 *    U_CURSOR_MEASURE_WORK_EYE_W wide per view, so the block and the search
 *    range below mean the same thing for 1080p and 4K SBS;
 *  - the web stereo-tile cursor's matcher, ported constant for constant
 *    (displayxr-web js/camera/disparity.js matchBlock + js/camera/pair-cursor.js
 *    pairFootprintDisparity): zero-mean NCC, a coarse-then-fine search over
 *    disparity and +-2 rows, a texture floor, a minimum NCC, a uniqueness
 *    margin against any rival peak >= 3 px away, a parabolic sub-pixel fit;
 *  - plus the ADR's left-right consistency check (the right block matched
 *    back into the left view lands within 1 px), which the web matcher lacks;
 *  - over the web's arrow footprint (the tip, then a ring of 9 points around
 *    the arrow's body); the content disparity is the NEAREST confident match.
 *    None confident = no content = the screen plane (= the plain OS cursor).
 *
 * The answer is a disparity in SOURCE pixels of the submitted sub-image,
 * x_B - x_A between the outer views A (index 0) and B (index n-1): < 0 is
 * crossed, in front of the glass (the ADR's sign, opposite to the web's).
 * u_cursor_measure_to_baselines() turns that into the placement's units from
 * the located views (the on-screen separation of a point at disparity d is
 * baseline * d, exactly), and u_cursor_measure_target_px() is Amendment 3's
 * pixel policy, which is A.1 in pixels.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0): a zero-initialised request
 * fails u_cursor_measure_should_sample(), so a compositor allocates, copies
 * and reads nothing for a session that never chained the request.
 *
 */

//! Block side, work px. The arrow at its OS size is ~16-20 work px tall.
#define U_CURSOR_MEASURE_BLOCK 16
//! Disparity search, +- work px (both sides: a background must win its own,
//! uncrossed, match rather than leave a false crossed one standing).
#define U_CURSOR_MEASURE_RANGE 64
//! Vertical search, +- work rows (web dyMax: a raw, unrectified pair).
#define U_CURSOR_MEASURE_DY 2
//! A block whose luma std (0..255) is below this is too flat to match (web MIN_TEXTURE).
#define U_CURSOR_MEASURE_MIN_TEXTURE 6.0f
//! A match must reach this NCC (web MIN_NCC).
#define U_CURSOR_MEASURE_MIN_NCC 0.8f
//! ...and beat every rival peak >= U_CURSOR_MEASURE_RIVAL_PX away by this (web UNIQUENESS).
#define U_CURSOR_MEASURE_UNIQUENESS 0.08f
#define U_CURSOR_MEASURE_RIVAL_PX 3
//! Left-right consistency: L->R and R->L disparities agree within this, work px.
#define U_CURSOR_MEASURE_LR_PX 1.0f
//! Per-view width the copied patch is box-downsampled to (factor = ceil(sub_w / this), <= 4).
#define U_CURSOR_MEASURE_WORK_EYE_W 960
#define U_CURSOR_MEASURE_MAX_FACTOR 4
//! The footprint box (block centres + half a block each side), work px per axis.
#define U_CURSOR_MEASURE_FOOTPRINT_MAX 64
//! Footprint sample points: the tip, then a ring of 9 (the web's cursorArrowFootprint).
#define U_CURSOR_MEASURE_POINTS 10
//! Largest work patch: the footprint dilated by the search (+1 for the sub-pixel fit).
#define U_CURSOR_MEASURE_MAX_WORK_W (U_CURSOR_MEASURE_FOOTPRINT_MAX + 2 * (U_CURSOR_MEASURE_RANGE + 1))
#define U_CURSOR_MEASURE_MAX_WORK_H (U_CURSOR_MEASURE_FOOTPRINT_MAX + 2 * U_CURSOR_MEASURE_DY)

/*!
 * What the state tracker asks a compositor to measure at the next commit.
 * Zero-initialised means "not requested".
 */
struct u_cursor_measure_request
{
	//! True only for a commit whose frame chained the request.
	bool requested;
	//! Echoed in the result so the state tracker can age it.
	uint64_t tag;
	//! Cursor hotspot, canvas-normalised (origin top-left, v down).
	float u, v;
	//! Sprite height, canvas heights (sizes the footprint; the footprint is
	//! laid out in pixels, which are square, so no aspect is needed).
	float height;
	//! Projection-layer view indices: the outermost active pair, [0] left of [1].
	uint32_t view_index[U_CURSOR_DEPTH_PATCH_VIEWS];
};

/*!
 * The copy: one rectangle, the same offset into each view's sub-image.
 */
struct u_cursor_measure_plan
{
	//! The rectangle relative to the sub-image origin, in the texture's own
	//! (storage) rows - bottom-up for a GL texture - source px.
	int32_t x, y, w, h;
	//! Box-downsample factor (source px per work px) and the work patch size.
	int32_t factor;
	int32_t work_w, work_h;
	//! The sub-image size the plan was made for (both views must match it).
	int32_t sub_w, sub_h;
	//! The texture stores rows bottom-up (GL): the copy is flipped back.
	bool bottom_up;
	//! Top-down y of the rectangle's first work row, source px.
	int32_t top_y;
};

//! Copied texel formats the matcher reads.
enum u_cursor_measure_format
{
	U_CURSOR_MEASURE_FORMAT_UNKNOWN = 0,
	U_CURSOR_MEASURE_FORMAT_RGBA8,   //!< 4 x u8, R first (UNORM or _SRGB: the bytes as stored)
	U_CURSOR_MEASURE_FORMAT_BGRA8,   //!< 4 x u8, B first
	U_CURSOR_MEASURE_FORMAT_RGBA16F, //!< 4 x half float, R first
	U_CURSOR_MEASURE_FORMAT_RGB10A2, //!< one u32: R bits 0-9, G 10-19, B 20-29
};

//! Bytes per texel of @p fmt, 0 if unknown.
uint32_t
u_cursor_measure_format_bytes(enum u_cursor_measure_format fmt);

/*!
 * One view's copy as the compositor read it back: plan.w x plan.h texels,
 * rows @p row_pitch bytes apart, in storage order.
 */
struct u_cursor_measure_patch
{
	const void *data;
	uint32_t row_pitch;
	enum u_cursor_measure_format format;
};

/*!
 * The answer for one measured frame.
 */
struct u_cursor_measure_result
{
	bool valid;
	uint64_t tag;
	//! A confident match exists; false = no content (the screen plane).
	bool confident;
	//! The nearest confident disparity, SOURCE px, x_B - x_A: < 0 = crossed (in front).
	float disparity_px;
	//! The sub-image width it was measured in (source px).
	float sub_w;
	//! Diagnostics: the winning NCC, how many footprint points matched with
	//! confidence (L->R), and how many of those failed the L-R check.
	float ncc;
	uint32_t matched;
	uint32_t lr_rejected;
	//! The patch was bit-identical to the last measured one at the same
	//! pointer (a paused video): the cached answer was reused, nothing matched.
	bool reused;
};

/*!
 * Last measured patch, so an unchanged frame is not re-measured.
 * Zero-initialise.
 */
struct u_cursor_measure_cache
{
	bool primed;
	uint64_t hash;
	struct u_cursor_measure_result result;
};

/*!
 * The compositor-side gate: false for a zero-initialised request, a cursor
 * off the canvas, or degenerate footprint values - the compositor then does
 * no work at all.
 */
bool
u_cursor_measure_should_sample(const struct u_cursor_measure_request *req);

/*!
 * The rectangle to copy out of each view's sub-image (@p sub_w x @p sub_h):
 * the footprint's blocks, dilated by the disparity search and the vertical
 * search, clamped to the sub-image.
 *
 * @return false if nothing can be measured (empty rectangle, sub-image too small).
 */
bool
u_cursor_measure_make_plan(const struct u_cursor_measure_request *req,
                           int32_t sub_w,
                           int32_t sub_h,
                           bool bottom_up,
                           struct u_cursor_measure_plan *out);

/*!
 * One block match (the web's matchBlock), on two work-resolution luma images
 * of the same size: the block of @p a at (x0, y0) searched in @p b at
 * x0 - sign * d, d in [d_min, d_max], rows y0 +- dy_max. sign = +1 for A the
 * left view (d = x_A - x_B, > 0 crossed); -1 for the reverse check.
 *
 * @return false if the block is too flat, nothing in range matched, or the
 *         best match is not unique (a repetitive texture). @p out_ncc is the
 *         best NCC (filled whenever a match was scored).
 */
bool
u_cursor_measure_match_block(const uint8_t *a,
                             const uint8_t *b,
                             int32_t w,
                             int32_t h,
                             int32_t x0,
                             int32_t y0,
                             int32_t sign,
                             int32_t d_min,
                             int32_t d_max,
                             float *out_d,
                             int32_t *out_dy,
                             float *out_ncc);

/*!
 * The nearest confident disparity of the footprint, on two work-resolution
 * luma images (A = left view). @p points are block centres, work px.
 *
 * @return true with @p out_d (work px, x_A - x_B: > 0 = crossed) if any
 *         point passed every check; false = no confident content.
 */
bool
u_cursor_measure_footprint(const uint8_t *a,
                           const uint8_t *b,
                           int32_t w,
                           int32_t h,
                           const float points[][2],
                           uint32_t point_count,
                           float *out_d,
                           struct u_cursor_measure_result *diag);

/*!
 * The full measurement of one frame: both views' copies -> luma at work
 * resolution -> the footprint match -> the result in source px.
 *
 * @param cache  optional; an unchanged patch at the same pointer reuses the
 *               last answer instead of matching again.
 * @return false only on invalid input (the result is then not valid).
 */
bool
u_cursor_measure_run(const struct u_cursor_measure_request *req,
                     const struct u_cursor_measure_plan *plan,
                     const struct u_cursor_measure_patch *view_a,
                     const struct u_cursor_measure_patch *view_b,
                     struct u_cursor_measure_cache *cache,
                     struct u_cursor_measure_result *out);

/*!
 * A measured disparity -> the placement's units (eye baselines, < 0 in
 * front), from the located views: a point at disparity d shows on the canvas
 * at x_B - x_A = baseline * d, so d = (measured / sub_w) * canvas_width /
 * baseline, the baseline taken along the canvas's +X (signed). Also returns
 * the conversion, source px per baseline - the web pair cursor's
 * pxPerBaseline (buffer px per metre on the glass x the eye separation),
 * here read off the views so it holds for any rig and m2v scale.
 *
 * @return false if the views or geometry are degenerate.
 */
bool
u_cursor_measure_to_baselines(const struct u_cursor_depth_view *a,
                              const struct u_cursor_depth_view *b,
                              const struct u_cursor_depth_geometry *g,
                              float measured_px,
                              float sub_w,
                              float *out_disparity,
                              float *out_px_per_baseline);

/*!
 * Amendment 3's policy in pixels: measured < 0 ? max(measured - margin_px,
 * cap_px) : 0, with margin_px = tuning margin x @p px_per_baseline and cap_px
 * = the comfort clamp x @p px_per_baseline. Exactly u_cursor_depth_target()
 * scaled by px_per_baseline; never behind the glass.
 */
float
u_cursor_measure_target_px(const struct u_cursor_depth_tuning *t,
                           bool confident,
                           float measured_px,
                           float px_per_baseline);

/*!
 * The footprint's block centres in work px, the web's cursorArrowFootprint:
 * the tip (u, v), then the centre of the arrow's body (0.3 h right, 0.5 h
 * down) and 8 points on a ring of 0.75 h around it. Scaled down uniformly if
 * the box (with half a block each side) would exceed
 * U_CURSOR_MEASURE_FOOTPRINT_MAX. @p tip_x / @p tip_y are the tip in work px.
 */
void
u_cursor_measure_footprint_points(float tip_x, float tip_y, float height_px, float out_points[][2]);

#ifdef __cplusplus
}
#endif
