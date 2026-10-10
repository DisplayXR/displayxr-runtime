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
 * screen (a plug-in without `create_dp_vk_for_screen`), or whose DP needs 1:1 pixels on a resampled screen, gets a flat 2D
 * blit of one view instead.
 *
 * The session's primary DP (`comp_vk_native_compositor::display_processor`)
 * keeps weaving the primary screen's segment, and it keeps owning everything
 * view-related (eye positions, window metrics) — per-segment views are M3.
 * A window entirely on the primary screen never enters this module's record
 * path while the primary holds the window: that case stays byte-for-byte the
 * single-DP path.
 *
 * Scope: desktop Linux, X11/XWayland (root coordinates are desktop-absolute),
 * and Windows (multi-screen M6: the window rect is the client area from
 * ClientToScreen in device px). Native Wayland stays primary-only until the
 * window-geometry service reports a desktop rect
 * (docs/specs/runtime/wayland-window-geometry.md, follow-up).
 *
 * Windows only: the real HWND (and so the vendor's drag phase-snap) follows the
 * screen holding the majority of the window (ADR-047 Amendment 2), through
 * @ref comp_vk_native_segments_hwnd_hooks — the same policy the D3D11 manager
 * runs. On Linux every segment DP is windowless and no hooks are set.
 * Under the Vulkan #918 split the weave is a D3D11 one on the scanout adapter
 * and this module is not used: the split segments with the D3D11 manager
 * (comp_vk_native_split.cpp).
 * Design note: docs/architecture/comp-segments.md.
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
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
 * @param dp_queue    #868 runtime-owned queue the DPs must capture at creation
 *                    (VK_NULL_HANDLE = none).
 */
struct comp_vk_native_segments *
comp_vk_native_segments_create(struct vk_bundle *vk, VkCommandPool cmd_pool, VkQueue dp_queue);

/*!
 * Tear down every segment DP and image (including everything still on the
 * retire list). The caller guarantees no submitted work still references them:
 * the repaint thread is stopped and every fence waited.
 */
void
comp_vk_native_segments_destroy(struct comp_vk_native_segments **segs_ptr);

/*!
 * How the manager moves the session's window between DPs (ADR-047
 * Amendment 2, Windows). All callbacks run on the weave thread with the
 * compositor's lock held, inside @ref comp_vk_native_segments_update (or
 * @ref comp_vk_native_segments_destroy / set_screens for the hand-back).
 */
struct comp_vk_native_segments_hwnd_hooks
{
	//! The session's real window (HWND). NULL disables the hand-off.
	void *hwnd;
	void *userdata;
	/*!
	 * Install @p dp as the session's primary DP and return the previous one.
	 * The manager RETIRES the returned DP (deferred until no fill is in
	 * flight — a parked repaint may still execute a command buffer that
	 * references it), so the compositor must not destroy it. The compositor
	 * re-sends its session-level state (transparency, 2D/3D mode,
	 * eye-tracking mode, encoding latch) to @p dp and guards the exchange
	 * against its app-thread readers.
	 */
	struct xrt_display_processor *(*swap_primary)(void *userdata, struct xrt_display_processor *dp);
	/*!
	 * Brackets a hand-off: @p begin true before any DP holding the window is
	 * retired; false after, with @p hwnd_dp the DP holding the window now —
	 * NULL when it is the session's primary DP (or none does). Optional.
	 */
	void (*bracket)(void *userdata, bool begin, struct xrt_display_processor *hwnd_dp);
};

/*!
 * Enable the window-handle hand-off. Call before
 * @ref comp_vk_native_segments_set_screens. NULL (or a NULL window) disables
 * it: the window stays with the primary DP. Clear it before a teardown that
 * destroys the primary DP too.
 */
void
comp_vk_native_segments_set_hwnd_hooks(struct comp_vk_native_segments *segs,
                                       const struct comp_vk_native_segments_hwnd_hooks *hooks);

/*!
 * The screen whose DP holds the session's window handle right now, 0 when
 * none does or segmentation is off. Weave thread (status read).
 */
uint64_t
comp_vk_native_segments_get_owner(const struct comp_vk_native_segments *segs);

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
 * @param release_safe    No repaint ("fill") is parked with its command buffer
 *                        in flight (#1264 S1 fence-park). Only then are retired
 *                        DPs / images handed back to Vulkan; otherwise they
 *                        wait on the retire list.
 * @param mode_index      The head's active rendering-mode index; a change
 *                        re-reads each segment DP's resample tolerance.
 * Runs the window-handle hand-off when one is due (Windows, hooks set): two
 * DP creates between two weaves; the compositor's primary DP may change.
 * @return true when this frame must take the split path
 *         (@ref comp_vk_native_segments_record) — the window spans screens,
 *         or the primary DP is windowless (another screen holds the window)
 *         and needs its present origin; false = the single-DP path.
 */
bool
comp_vk_native_segments_update(struct comp_vk_native_segments *segs,
                               const struct comp_seg_rect *window_desktop,
                               const struct comp_seg_rect *canvas,
                               int32_t target_format,
                               bool release_safe,
                               uint32_t mode_index);

/*!
 * Everything the split path needs about this frame.
 */
struct comp_vk_native_segments_frame
{
	VkCommandBuffer cmd;

	//! A repaint replay (#868) rather than an app frame: selects the frame
	//! class's own DP input images, so an app frame never writes what an
	//! in-flight fill reads.
	bool is_repaint;

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

	//! Clear colour alpha for the target outside every segment; also
	//! re-declared to segment DPs when it changes.
	bool transparent_background;

	//! The atlas encoding to declare to segment DPs (`enum
	//! xrt_atlas_encoding`), or -1 to declare nothing (DXR_VK_ATLAS_ENCODING=off).
	int atlas_encoding;

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

/*!
 * The session-wide hardware 2D/3D mode. Every segment DP follows it: sent to
 * each live DP on a change and to every newly created one (the primary DP
 * gets it from the compositor's own request path). Weave thread.
 */
void
comp_vk_native_segments_set_display_mode(struct comp_vk_native_segments *segs, bool enable_3d);

/*!
 * Multi-screen M3: the last update's segment table as per-segment view
 * metrics (geometry, each screen's physical size + nominal viewer, whether it
 * is woven). Eyes are NOT filled — they are predicted per query
 * (@ref comp_vk_native_segments_get_eyes).
 *
 * @param window_desktop  The window rect the table was computed for.
 * @param canvas          The canvas it was cut from, window px.
 * @param primary_has_dp  The session's primary DP exists.
 * @return false when the last update did not split the window, or split it
 *         into more than XRT_MAX_SEGMENTS segments (one view set then).
 */
bool
comp_vk_native_segments_get_metrics(const struct comp_vk_native_segments *segs,
                                    const struct comp_seg_rect *window_desktop,
                                    const struct comp_seg_rect *canvas,
                                    bool primary_has_dp,
                                    struct xrt_segment_metrics *out);

/*!
 * Multi-screen M3: the predicted eyes of screen @p screen_id's segment DP, in
 * that screen's display space. Thread-safe against the weave creating or
 * destroying segment DPs. False when the screen has no segment DP (the
 * primary's eyes come from the session's own DP).
 */
bool
comp_vk_native_segments_get_eyes(struct comp_vk_native_segments *segs,
                                 uint64_t screen_id,
                                 struct xrt_eye_positions *out);

#ifdef __cplusplus
}
#endif
