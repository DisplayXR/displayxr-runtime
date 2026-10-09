// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process D3D11
 *         compositor (multi-screen M6, ADR-047 D2 on Windows).
 * @ingroup comp_d3d11
 *
 * The D3D11 twin of vk_native/comp_vk_native_segments.h. A window whose
 * canvas covers several monitors is woven per segment: each segment (canvas ∩
 * screen, see util/comp_segments.h) gets the display processor of ITS screen,
 * a pre-cropped atlas holding exactly that segment's views, `canvas = segment
 * rect`, and its own present origin; the results land in the one DXGI back
 * buffer. A segment with no DP for its screen (a plug-in without
 * `create_dp_d3d11_for_screen`), or whose DP needs 1:1 pixels on a resampled
 * screen, gets a flat 2D blit of one view instead.
 *
 * The session's primary DP (`comp_d3d11_compositor::display_processor`, the
 * one holding the real HWND and so the vendor's drag phase-snap) keeps weaving
 * the primary screen's segment and keeps owning everything view-related. A
 * window entirely on the primary screen never enters this module's record
 * path: that case stays byte-for-byte the single-DP path.
 *
 * Design note: docs/architecture/comp-segments.md (§ Windows / D3D11).
 */
#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_screen.h"
#include "util/comp_segments.h"

#ifdef __cplusplus
extern "C" {
#endif

struct comp_d3d11_segments;
struct comp_d3d11_outcomp;

/*!
 * Create the segment manager. Nothing is allocated on the GPU until a segment
 * needs it.
 *
 * @param d3d11_device  The device segment DPs and crop textures are created on
 *                      (ID3D11Device*, borrowed): the device that PRESENTS,
 *                      i.e. the one the back buffer lives on — under the #918
 *                      weave-on-scanout split (ADR-039) that is the OUTPUT
 *                      device, off it the app device.
 */
struct comp_d3d11_segments *
comp_d3d11_segments_create(void *d3d11_device);

/*!
 * Tear down every segment DP and crop texture. The caller guarantees no weave
 * is in flight (the repaint thread is stopped).
 */
void
comp_d3d11_segments_destroy(struct comp_d3d11_segments **segs_ptr);

/*!
 * Hand over the system's screens and DP registry. Segmentation is enabled only
 * when at least two screens are listed, the registry knows the system-default
 * screen, and @p pinned_display_id is 0 (`XrSessionDisplayBindingDXR` pins a
 * session to one display, which turns segmentation off).
 */
void
comp_d3d11_segments_set_screens(struct comp_d3d11_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id);

//! Is segmentation possible at all for this session?
bool
comp_d3d11_segments_enabled(const struct comp_d3d11_segments *segs);

/*!
 * One metric update: recompute the segment table for the window, run the DP
 * lifecycle (create/destroy secondary DPs with hysteresis), log a table change
 * once at INFO. Call once per weave, before deciding which path records.
 *
 * @param window_desktop  The window's client area in desktop device px.
 * @param canvas          The canvas in window px (NULL / empty = whole window).
 * @param d3d11_context   The context handed to new segment DPs.
 * @param mode_index      The head's active rendering-mode index; a change
 *                        re-reads each segment DP's resample tolerance.
 * @return true when this frame must take the split path
 *         (@ref comp_d3d11_segments_record); false = the single-DP path.
 */
bool
comp_d3d11_segments_update(struct comp_d3d11_segments *segs,
                           const struct comp_seg_rect *window_desktop,
                           const struct comp_seg_rect *canvas,
                           void *d3d11_context,
                           uint32_t mode_index);

//! Everything the split path needs about this frame.
struct comp_d3d11_segments_frame
{
	//! The context to record on (ID3D11DeviceContext*; the output context under the split).
	void *context;
	//! The DP's input this frame: the atlas cropped to content (ID3D11ShaderResourceView*).
	//! Under the #918 split this is the output-side copy of the composed atlas
	//! (the egress slot being woven), so it lives on the same device as the DPs.
	void *src_srv;
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;
	uint32_t tile_rows;
	//! The format declared to the DPs (DXGI_FORMAT of the atlas).
	uint32_t src_format;
	//! The back buffer (ID3D11RenderTargetView*) and its size.
	void *target_rtv;
	uint32_t target_width;
	uint32_t target_height;
	//! The canvas the atlas holds, window px (whole-canvas view dims above).
	struct comp_seg_rect canvas;
	//! Clear alpha for the target outside every segment; also re-declared to
	//! segment DPs when it changes.
	bool transparent_background;
	//! The atlas encoding to declare to segment DPs (`enum
	//! xrt_atlas_encoding`), or -1 to declare nothing.
	int atlas_encoding;
	//! The session's primary DP (weaves the primary screen's segment). The
	//! caller has already fed it this frame's background, timing and encoding.
	struct xrt_display_processor_d3d11 *primary_dp;
	//! For the flat-2D fill (comp_d3d11_outcomp_blit_rect): the output
	//! composite unit, which lives on the same device as the target.
	struct comp_d3d11_outcomp *outcomp;
};

/*!
 * Record the split frame: clear the target, crop each segment's views out of
 * the atlas, run each segment's DP over its canvas (primary first, then left
 * to right; viewport + scissor = segment), and blit flat 2D into the segments
 * that cannot be woven. Leaves the back buffer bound as the render target.
 *
 * @return true if at least one DP wove.
 */
bool
comp_d3d11_segments_record(struct comp_d3d11_segments *segs, const struct comp_d3d11_segments_frame *f);

/*!
 * The session-wide hardware 2D/3D mode. Every segment DP follows it: sent to
 * each live DP on a change and to every newly created one (the primary DP
 * gets it from the compositor's own request path). Weave thread.
 */
void
comp_d3d11_segments_set_display_mode(struct comp_d3d11_segments *segs, bool enable_3d);

/*!
 * The predicted eyes of screen @p screen_id's segment DP, in that screen's
 * display space. Thread-safe against the weave creating or destroying segment
 * DPs. False when the screen has no segment DP (the primary's eyes come from
 * the session's own DP).
 */
bool
comp_d3d11_segments_get_eyes(struct comp_d3d11_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out);

/*!
 * Multi-screen M3: the last update's segment table as per-segment view
 * metrics (geometry, each screen's physical size + nominal viewer, whether it
 * is woven). Eyes are NOT filled — they are predicted per query
 * (@ref comp_d3d11_segments_get_eyes).
 *
 * @return false when the last update did not split the window, or split it
 *         into more than XRT_MAX_SEGMENTS segments (one view set then).
 */
bool
comp_d3d11_segments_get_metrics(const struct comp_d3d11_segments *segs,
                                const struct comp_seg_rect *window_desktop,
                                const struct comp_seg_rect *canvas,
                                bool primary_has_dp,
                                struct xrt_segment_metrics *out);

#ifdef __cplusplus
}
#endif
