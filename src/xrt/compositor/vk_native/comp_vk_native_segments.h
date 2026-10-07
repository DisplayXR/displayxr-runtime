// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process Vulkan
 *         compositor (multi-screen M2, ADR-047 D2).
 * @ingroup comp_vk_native
 *
 * A window whose canvas covers several screens is woven per segment: each
 * segment (canvas ∩ screen, see util/comp_segments.h) gets the display
 * processor of ITS screen, a pre-cropped atlas holding exactly that segment's
 * views, `canvas = segment rect`, and its own present origin; the results are
 * composited into the one presented surface. A segment with no DP for its
 * screen, or whose DP needs 1:1 pixels on a resampled screen, gets a flat 2D
 * blit of one view instead.
 *
 * The session's primary DP (`comp_vk_native_compositor::display_processor`)
 * keeps weaving the primary screen's segment, and it keeps owning everything
 * view-related (eye positions, window metrics) — per-segment views are M3.
 * A window entirely on the primary screen never enters this module's record
 * path: that case stays byte-for-byte the single-DP path.
 *
 * Scope: desktop Linux, X11/XWayland (root coordinates are desktop-absolute).
 * Native Wayland stays primary-only until the window-geometry service reports
 * a desktop rect (docs/specs/runtime/wayland-window-geometry.md, follow-up).
 * Design note: docs/architecture/comp-segments.md.
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_screen.h"
#include "xrt/xrt_vulkan_includes.h"

#include "util/comp_segments.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;
struct comp_vk_native_segments;

/*!
 * Create the segment manager. Nothing is allocated on the GPU until a segment
 * needs it.
 *
 * @param vk          The compositor's bundle (borrowed; outlives this object).
 * @param cmd_pool    Pool handed to segment DP factories (borrowed).
 */
struct comp_vk_native_segments *
comp_vk_native_segments_create(struct vk_bundle *vk, VkCommandPool cmd_pool);

/*!
 * Tear down every segment DP and image. The caller guarantees no submitted
 * work still references them (the compositor waits each frame's fence).
 */
void
comp_vk_native_segments_destroy(struct comp_vk_native_segments **segs_ptr);

/*!
 * Hand over the system's screens and DP registry. Segmentation is enabled only
 * when at least two screens are listed, the registry knows the system-default
 * screen, and @p pinned_display_id is 0 (`XrSessionDisplayBindingDXR` pins a
 * session to one display, which turns segmentation off).
 *
 * @param list               The screens (xrEnumerateDisplaysDXR's list).
 * @param info               System compositor info: the DP registry + active plug-in.
 * @param pinned_display_id  The session's display binding, 0 = none.
 */
void
comp_vk_native_segments_set_screens(struct comp_vk_native_segments *segs,
                                    const struct xrt_screen_list *list,
                                    const struct xrt_system_compositor_info *info,
                                    uint64_t pinned_display_id);

/*!
 * Is segmentation possible at all for this session?
 */
bool
comp_vk_native_segments_enabled(const struct comp_vk_native_segments *segs);

/*!
 * One metric update: recompute the segment table for the window, run the DP
 * lifecycle (create/destroy secondary DPs with hysteresis), log a table change
 * once at INFO. Call once per weave, before deciding which path records.
 *
 * @param window_desktop  The window's client area in desktop coordinates.
 * @param canvas          The canvas in window px (`vk_dp_canvas_rect`).
 * @param target_format   Swapchain format, for segment DP creation.
 * @return true when this frame must take the split path
 *         (@ref comp_vk_native_segments_record); false = the single-DP path.
 */
bool
comp_vk_native_segments_update(struct comp_vk_native_segments *segs,
                               const struct comp_seg_rect *window_desktop,
                               const struct comp_seg_rect *canvas,
                               int32_t target_format);

/*!
 * Everything the split path needs about this frame.
 */
struct comp_vk_native_segments_frame
{
	VkCommandBuffer cmd;

	//! The DP's input this frame (atlas cropped to content), SHADER_READ_ONLY_OPTIMAL.
	VkImage src_image;
	VkFormat src_format;
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;
	uint32_t tile_rows;

	//! The target, in COLOR_ATTACHMENT_OPTIMAL on entry; left in PRESENT_SRC_KHR.
	VkFramebuffer target_fb;
	VkImage target_image;
	VkImageView target_view;
	uint32_t target_width;
	uint32_t target_height;
	VkFormat target_format;

	//! The canvas the atlas holds, window px (whole-canvas view dims above).
	struct comp_seg_rect canvas;

	//! Clear colour alpha for the target outside every segment.
	bool transparent_background;

	//! The session's primary DP (weaves the primary screen's segment). The
	//! caller has already fed it this frame's present origin, background,
	//! timing and atlas encoding.
	struct xrt_display_processor *primary_dp;
};

/*!
 * Record the split frame: clear the target, crop each segment's views out of
 * the atlas, run each segment's DP over its canvas (primary first, then left
 * to right; viewport + scissor = segment), and blit flat 2D into the segments
 * that cannot be woven. Leaves the target in PRESENT_SRC_KHR — the state the
 * single-DP path leaves it in — so everything after the weave is unchanged.
 *
 * @return true if at least one DP wove.
 */
bool
comp_vk_native_segments_record(struct comp_vk_native_segments *segs,
                               const struct comp_vk_native_segments_frame *f);

/*!
 * The cropped DP input of segment @p index of the LAST split frame (for the
 * atlas capture: one PNG per segment proves the canvases). False past the end
 * or when the last frame was not split.
 */
bool
comp_vk_native_segments_get_capture(const struct comp_vk_native_segments *segs,
                                    uint32_t index,
                                    VkImage *out_image,
                                    uint32_t *out_w,
                                    uint32_t *out_h,
                                    uint64_t *out_screen_id,
                                    bool *out_woven);

#ifdef __cplusplus
}
#endif
