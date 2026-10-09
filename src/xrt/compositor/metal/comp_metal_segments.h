// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process Metal
 *         compositor (multi-screen on macOS, ADR-047 D2).
 * @ingroup comp_metal
 *
 * The Metal twin of d3d11/comp_d3d11_segments.h. A window whose content view
 * covers several displays is woven per segment: each segment (canvas ∩
 * screen, see util/comp_segments.h) gets the display processor of ITS screen
 * (`xrt_plugin_iface::create_dp_metal_for_screen`, windowless), a pre-cropped
 * atlas holding exactly that segment's views, `canvas = segment rect`, and its
 * own present origin; the results land in the one CAMetalLayer drawable. A
 * segment with no DP for its screen, or whose DP needs 1:1 pixels on a
 * resampled screen, gets a flat 2D blit of one view instead.
 *
 * **Coordinate spaces (the macOS-specific part).** The screen registry holds
 * each display's `CGDisplayBounds` in top-down POINTS plus its native backing
 * px; the window is read in the same points. The segment table is cut in
 * points (so a seam is exact whatever the scales), then converted:
 *
 *   - window rect (the DP canvas) → the DRAWABLE's px (points × the drawable's
 *     scale, edges rounded independently so neighbours share the seam);
 *   - present origin → THAT screen's backing px relative to its
 *     `CGDisplayBounds` origin, chosen so `origin + canvas offset` = the
 *     segment's top-left on the panel (what the Leia SR Metal weaver takes);
 *   - 1:1 → the screen's backing scale equals the drawable's AND its points ×
 *     scale equal its native mode (a "looks like" scaled mode, or a window
 *     straddling a 1x and a 2x display, is resampled by the WindowServer);
 *   - published metrics (M3 per-segment views) → points × the drawable's
 *     scale everywhere: one window pixel on screen i covers 1/scale points of
 *     it, so the per-pixel physical pitch stays right on every screen.
 *
 * The session's primary DP (`comp_metal_compositor::display_processor`, made
 * for the system-default display with the real NSView) keeps weaving the
 * primary screen's segment. A window entirely on the primary screen never
 * enters this module's record path: that case stays the single-DP path.
 *
 * Design note: docs/architecture/comp-segments.md (§ macOS / Metal).
 */
#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_screen.h"
#include "util/comp_segments.h"

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_segments;

/*!
 * Create the segment manager. Nothing is allocated on the GPU until a segment
 * needs it.
 *
 * @param mtl_device     id<MTLDevice> (borrowed).
 * @param command_queue  id<MTLCommandQueue> (borrowed) handed to segment DPs.
 */
struct comp_metal_segments *
comp_metal_segments_create(void *mtl_device, void *command_queue);

//! Tear down every segment DP and crop texture.
void
comp_metal_segments_destroy(struct comp_metal_segments **segs_ptr);

/*!
 * Hand over the system's screens and DP registry. Segmentation is enabled only
 * when at least two screens are listed, the registry knows the system-default
 * screen, @p pinned_display_id is 0 and `DXR_SEGMENTS` is not `0`.
 */
void
comp_metal_segments_set_screens(struct comp_metal_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id);

//! Is segmentation possible at all for this session?
bool
comp_metal_segments_enabled(const struct comp_metal_segments *segs);

/*!
 * Where the window is this frame.
 */
struct comp_metal_seg_window
{
	//! The content view in top-down global POINTS (origin = the main
	//! display's top-left, the CGDisplayBounds space), rounded to whole points.
	struct comp_seg_rect frame_pt;
	//! The drawable (the target), px.
	uint32_t drawable_w;
	uint32_t drawable_h;
};

/*!
 * One metric update: recompute the table, run the DP lifecycle (create /
 * destroy secondary DPs with hysteresis), log a table change once at INFO.
 *
 * @param mode_index  The head's active rendering-mode index; a change
 *                    re-reads each segment DP's resample tolerance.
 * @return true when this frame must take the split path.
 */
bool
comp_metal_segments_update(struct comp_metal_segments *segs,
                           const struct comp_metal_seg_window *win,
                           uint32_t mode_index);

//! Everything the split path needs about this frame.
struct comp_metal_segments_frame
{
	void *command_buffer; //!< id<MTLCommandBuffer> the whole frame records on
	void *src_texture;    //!< id<MTLTexture>: the atlas (tiles at view_width/height stride)
	uint32_t view_width;  //!< one tile, px (the whole drawable's canvas at view scale)
	uint32_t view_height;
	uint32_t tile_columns;
	uint32_t tile_rows;
	void *target_texture; //!< id<MTLTexture>: the drawable
	uint32_t target_width;
	uint32_t target_height;
	//! Clear alpha for the target outside every segment.
	bool transparent_background;
	//! The session's primary DP (weaves the primary screen's segment).
	struct xrt_display_processor_metal *primary_dp;
};

/*!
 * Record the split frame on the frame's command buffer: clear the target,
 * crop each segment's views out of the atlas, run each segment's DP over its
 * canvas (primary first, then left to right), and blit flat 2D into the
 * segments that cannot be woven and the canvas no segment covers.
 *
 * @return true if at least one DP wove.
 */
bool
comp_metal_segments_record(struct comp_metal_segments *segs, const struct comp_metal_segments_frame *f);

/*!
 * The session-wide hardware 2D/3D mode; every segment DP follows it.
 */
void
comp_metal_segments_set_display_mode(struct comp_metal_segments *segs, bool enable_3d);

/*!
 * The predicted eyes of screen @p screen_id's segment DP, in that screen's
 * display space. Thread-safe against the weave creating or destroying DPs.
 */
bool
comp_metal_segments_get_eyes(struct comp_metal_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out);

/*!
 * M3: the last update's table as per-segment view metrics (window rects in
 * drawable px; screen rects / desktop rects in points × the drawable scale).
 * Eyes are not filled.
 *
 * @return false when the last update did not split the window, or split it
 *         into more than XRT_MAX_SEGMENTS segments.
 */
bool
comp_metal_segments_get_metrics(const struct comp_metal_segments *segs,
                                bool primary_has_dp,
                                struct xrt_segment_metrics *out);

#ifdef __cplusplus
}
#endif
