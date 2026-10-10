// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process D3D12
 *         compositor (multi-screen M6, ADR-047 D2 on Windows).
 * @ingroup comp_d3d12
 *
 * The D3D12 twin of d3d11/comp_d3d11_segments.h (itself the twin of
 * vk_native/comp_vk_native_segments.h). A window whose canvas covers several
 * monitors is woven per segment: each segment (canvas ∩ screen, see
 * util/comp_segments.h) gets the display processor of ITS screen, a
 * pre-cropped plain 2D atlas holding exactly that segment's views,
 * `canvas = segment rect`, and its own present origin; the results land in the
 * one DXGI back buffer, all recorded onto the compositor's one weave command
 * list. A segment with no DP for its screen (a plug-in without
 * `create_dp_d3d12_for_screen`), or whose DP needs 1:1 pixels on a resampled
 * screen, gets a flat 2D blit of one view instead.
 *
 * The session's primary DP (`comp_d3d12_compositor::display_processor`, the
 * one holding the real HWND and so the vendor's drag phase-snap) keeps weaving
 * the primary screen's segment and keeps owning everything view-related. A
 * window entirely on the primary screen never enters this module's record
 * path: that case stays byte-for-byte the single-DP path.
 *
 * GPU lifetime: every weave of the in-process D3D12 compositor ends in a full
 * wait on its queue, so by the time the next weave runs this module's update
 * nothing it recorded is still in flight — retired DPs, crop textures and the
 * descriptors are released or rewritten there without a deferred-release list.
 *
 * Design note: docs/architecture/comp-segments.md (§ Windows / D3D12).
 */
#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_screen.h"
#include "util/comp_segments.h"

#ifdef __cplusplus
extern "C" {
#endif

struct comp_d3d12_segments;
struct comp_d3d12_renderer;

/*!
 * Create the segment manager. Only the small SRV descriptor heap is allocated
 * up front; nothing else until a segment needs it.
 *
 * @param d3d12_device  The device segment DPs and crop textures are created on
 *                      (ID3D12Device*, borrowed).
 * @param d3d12_queue   The queue handed to new segment DPs
 *                      (ID3D12CommandQueue*, borrowed).
 */
struct comp_d3d12_segments *
comp_d3d12_segments_create(void *d3d12_device, void *d3d12_queue);

/*!
 * Tear down every segment DP, crop texture and the descriptor heap. The caller
 * guarantees no weave is in flight (the repaint thread is stopped and the GPU
 * is idle).
 */
void
comp_d3d12_segments_destroy(struct comp_d3d12_segments **segs_ptr);

/*!
 * Hand over the system's screens and DP registry. Segmentation is enabled only
 * when at least two screens are listed, the registry knows the system-default
 * screen, and @p pinned_display_id is 0 (`XrSessionDisplayBindingDXR` pins a
 * session to one display, which turns segmentation off).
 */
void
comp_d3d12_segments_set_screens(struct comp_d3d12_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id);

//! Is segmentation possible at all for this session?
bool
comp_d3d12_segments_enabled(const struct comp_d3d12_segments *segs);

/*!
 * One metric update: recompute the segment table for the window, run the DP
 * lifecycle (create/destroy secondary DPs with hysteresis), log a table change
 * once at INFO. Call once per weave, before deciding which path records.
 *
 * @param window_desktop  The window's client area in desktop device px.
 * @param canvas          The canvas in window px (NULL / empty = whole window).
 * @param mode_index      The head's active rendering-mode index; a change
 *                        re-reads each segment DP's resample tolerance.
 * @return true when this frame must take the split path
 *         (@ref comp_d3d12_segments_record); false = the single-DP path.
 */
bool
comp_d3d12_segments_update(struct comp_d3d12_segments *segs,
                           const struct comp_seg_rect *window_desktop,
                           const struct comp_seg_rect *canvas,
                           uint32_t mode_index);

//! Everything the split path needs about this frame.
struct comp_d3d12_segments_frame
{
	//! The open weave list (ID3D12GraphicsCommandList*).
	void *cmd_list;
	//! The DP's input this frame: the atlas cropped to content
	//! (ID3D12Resource*, plain 2D, in COMMON on entry and on return).
	void *src_resource;
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;
	uint32_t tile_rows;
	//! The format declared to the DPs (DXGI_FORMAT of the atlas).
	uint32_t src_format;
	//! The back buffer (ID3D12Resource*, in RENDER_TARGET), its RTV
	//! (D3D12_CPU_DESCRIPTOR_HANDLE::ptr) and its size.
	void *target_resource;
	uint64_t target_rtv;
	uint32_t target_width;
	uint32_t target_height;
	//! The canvas the atlas holds, window px (whole-canvas view dims above).
	struct comp_seg_rect canvas;
	/*!
	 * #1883: the partition @ref src_resource's pixels were PAINTED with — a
	 * routed (M3) frame's per-segment mosaic, recorded with the frame. Each
	 * segment's views are cropped from where they were painted and woven over
	 * the live segment, never cut at the live seam. NULL / count 0 = one view
	 * set (unrouted): the live partition is exact.
	 */
	const struct comp_segments_content *content;
	//! Clear alpha for the target outside every segment; also re-declared to
	//! segment DPs when it changes.
	bool transparent_background;
	//! The session's primary DP (weaves the primary screen's segment). The
	//! caller has already fed it this frame's background and timing.
	struct xrt_display_processor_d3d12 *primary_dp;
	//! For the flat-2D fill (comp_d3d12_renderer_blit_rect).
	struct comp_d3d12_renderer *renderer;
};

/*!
 * Record the split frame onto @ref comp_d3d12_segments_frame::cmd_list: clear
 * the target, crop each segment's views out of the atlas, run each segment's
 * DP over its canvas (primary first, then left to right), and blit flat 2D
 * into the segments that cannot be woven. Leaves the back buffer bound as the
 * render target, viewport + scissor = the whole target.
 *
 * @return true if at least one DP wove.
 */
bool
comp_d3d12_segments_record(struct comp_d3d12_segments *segs, const struct comp_d3d12_segments_frame *f);

/*!
 * The session-wide hardware 2D/3D mode. Every segment DP follows it: sent to
 * each live DP on a change and to every newly created one (the primary DP
 * gets it from the compositor's own request path). Weave thread.
 */
void
comp_d3d12_segments_set_display_mode(struct comp_d3d12_segments *segs, bool enable_3d);

/*!
 * The predicted eyes of screen @p screen_id's segment DP, in that screen's
 * display space. Thread-safe against the weave creating or destroying segment
 * DPs. False when the screen has no segment DP (the primary's eyes come from
 * the session's own DP).
 */
bool
comp_d3d12_segments_get_eyes(struct comp_d3d12_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out);

/*!
 * Multi-screen M3: the last update's segment table as per-segment view
 * metrics (geometry, each screen's physical size + nominal viewer, whether it
 * is woven). Eyes are NOT filled — they are predicted per query
 * (@ref comp_d3d12_segments_get_eyes).
 *
 * @return false when the last update did not split the window, or split it
 *         into more than XRT_MAX_SEGMENTS segments (one view set then).
 */
bool
comp_d3d12_segments_get_metrics(const struct comp_d3d12_segments *segs,
                                const struct comp_seg_rect *window_desktop,
                                const struct comp_seg_rect *canvas,
                                bool primary_has_dp,
                                struct xrt_segment_metrics *out);

#ifdef __cplusplus
}
#endif
