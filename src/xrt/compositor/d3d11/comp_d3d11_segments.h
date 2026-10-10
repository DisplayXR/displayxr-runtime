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
 * The session's primary DP (`comp_d3d11_compositor::display_processor`) keeps
 * weaving the primary screen's segment and keeps owning everything
 * view-related. The real HWND (and so the vendor's drag phase-snap) follows
 * the screen holding the majority of the window (ADR-047 Amendment 2): when
 * another screen clearly holds it for 0.5 s, that screen's segment DP is
 * recreated WITH the window and the primary DP is swapped for a windowless
 * one (through @ref comp_d3d11_segments_hwnd_hooks), and back. A window
 * entirely on the primary screen never enters this module's record path while
 * the primary holds the window: that case stays byte-for-byte the single-DP
 * path.
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
 * is in flight (the repaint thread is stopped). With hooks set and another
 * screen holding the window, the window is handed back to the primary DP
 * first; clear the hooks before a teardown that destroys the primary DP too.
 */
void
comp_d3d11_segments_destroy(struct comp_d3d11_segments **segs_ptr);

/*!
 * How the manager moves the session's window between DPs (ADR-047
 * Amendment 2). All callbacks run on the weave thread with the compositor's
 * weave lock held.
 */
struct comp_d3d11_segments_hwnd_hooks
{
	//! The session's real window (HWND). NULL disables the hand-off.
	void *hwnd;
	void *userdata;
	/*!
	 * Install @p dp as the session's primary DP and return the previous one,
	 * which the manager destroys. The compositor re-sends its session-level
	 * state (transparency, 2D/3D mode, eye-tracking mode) to @p dp and guards
	 * the exchange against its other threads.
	 */
	struct xrt_display_processor_d3d11 *(*swap_primary)(void *userdata, struct xrt_display_processor_d3d11 *dp);
	/*!
	 * Brackets a hand-off: @p begin true before any DP holding the window is
	 * destroyed; false after, with @p hwnd_dp the DP holding the window now —
	 * NULL when it is the session's primary DP (or none does). The compositor
	 * routes its own window's drag snap to it. Optional.
	 */
	void (*bracket)(void *userdata, bool begin, struct xrt_display_processor_d3d11 *hwnd_dp);
};

/*!
 * Enable the window-handle hand-off. Call before
 * @ref comp_d3d11_segments_set_screens. NULL (or a NULL window) disables it:
 * the window stays with the primary DP.
 */
void
comp_d3d11_segments_set_hwnd_hooks(struct comp_d3d11_segments *segs,
                                   const struct comp_d3d11_segments_hwnd_hooks *hooks);

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
 * Runs the window-handle hand-off when one is due (two DP creates, between
 * two weaves).
 *
 * @return true when this frame must take the split path
 *         (@ref comp_d3d11_segments_record) — the window spans screens, or
 *         the primary DP is windowless (another screen holds the window) and
 *         needs its present origin; false = the single-DP path.
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
	//! The client presents the target itself over its own window (an
	//! XR_DXR_weave present-owner, #1884): declared to segment DPs as
	//! set_transparent_background's `client_presents`, like the session DP's.
	//! False (zero-init) for a compositor that presents its own back buffer.
	bool client_presents;
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
 * Which display processor a drag snap of the window should use (#1884, the
 * XR_DXR_weave present-owner's `xrWeaveSnapWindowRectDXR`): the screen holding
 * the majority of the window at its PROPOSED desktop rect @p window_desktop,
 * with the same hysteresis as the window-handle owner (ADR-047 Amendment 2 —
 * a clear margin held for 0.5 s), so a drag across a seam does not flip the
 * lattice under the cursor. Nothing is created or destroyed: a screen whose
 * segment DP does not exist yet keeps the current lattice until it does.
 *
 * Caller serialises with @ref comp_d3d11_segments_update (the service holds
 * its render mutex for both).
 *
 * @param[out] out_screen_id  The screen whose lattice applies (0 = none / off).
 * @return that screen's segment DP, or NULL when it is the primary screen (the
 *         session's own DP snaps) or segmentation is off.
 */
struct xrt_display_processor_d3d11 *
comp_d3d11_segments_snap_dp(struct comp_d3d11_segments *segs,
                            const struct comp_seg_rect *window_desktop,
                            uint64_t now_ns,
                            uint64_t *out_screen_id);

/*!
 * The screen whose DP holds the session's window handle right now (ADR-047
 * Amendment 2), 0 when none does or segmentation is off. Status read for the
 * display dashboard (ADR-051); the caller serialises with
 * @ref comp_d3d11_segments_update (the service holds its render mutex).
 */
uint64_t
comp_d3d11_segments_get_owner(const struct comp_d3d11_segments *segs);

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
