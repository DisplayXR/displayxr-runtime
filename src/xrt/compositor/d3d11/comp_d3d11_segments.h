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
 * @ref comp_d3d11_segments_destroy before a rebuild on @p next, the list the
 * next manager takes (ADR-047 Amendment 8, #2305). When the home is unplugged
 * in @p next (its id gone and its plug-in winning no screen), the window goes
 * to a screen of @p next whose DP is ready — it stays on the DP that holds it
 * when that one qualifies — and that DP takes the session's primary slot; no
 * DP is ever made for the departed screen (failing that, the window stays
 * with its holder). The slot's new screen is written to
 * comp_d3d11_segments_hwnd_hooks::away. Otherwise (the home listed,
 * re-claimed or re-identified) the window goes back to the primary DP as in a
 * teardown.
 */
void
comp_d3d11_segments_destroy_for_rebuild(struct comp_d3d11_segments **segs_ptr, const struct xrt_screen_list *next);

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
	/*!
	 * Optional, compositor-owned and kept across rebuilds (ADR-047 Amendment
	 * 8, #2305): the screen whose DP sits in the session's primary slot
	 * instead of the home screen's (screen_id 0 = the home's), with the
	 * plug-ins. A hand-back with the home gone sets it; the first update of a
	 * manager whose list has the home back moves that DP to its own screen's
	 * slot as the window's holder, gives the home a windowless DP, and clears
	 * it. While the home stays gone, a list in which its plug-in wins a
	 * screen again (the panel under another id), or without the slot's
	 * screen, gets a stand-in primary (comp_segments_away_decide), re-homed
	 * the same way; the record then names the stand-in.
	 */
	struct comp_segments_away *away;
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
 * ADR-060 (texture target binding): the session's primary DP is windowless by
 * construction — it phases from set_present_origin and no screen ever holds a
 * window. The split weave then feeds the primary's segment its present origin,
 * exactly as for a primary made windowless by a hand-off; unlike that case, a
 * target wholly on the primary screen is NOT forced through the split path
 * (the caller pushes the origin on its single-DP weave, ADR-060 split rule).
 */
void
comp_d3d11_segments_set_primary_phases_from_origin(struct comp_d3d11_segments *segs, bool on);

/*!
 * #2218: would this window take the split path (not wholly on the primary
 * screen)? Pure (@ref comp_segments_window_is_split on the manager's screens):
 * no lifecycle, no DPs. The weave publishes the answer for the compose step,
 * which withholds zero-copy while it is true so the frame reaches
 * @ref comp_d3d11_segments_update instead of the primary DP. Also true while the
 * primary DP is windowless (another screen holds the window): the update must
 * keep running until it has handed the window back (#2305).
 */
bool
comp_d3d11_segments_would_split(const struct comp_d3d11_segments *segs,
                                const struct comp_seg_rect *window_desktop,
                                const struct comp_seg_rect *canvas);

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
	/*!
	 * #1883: the partition @ref src_srv's pixels were PAINTED with — a routed
	 * (M3) frame's per-segment mosaic, recorded with the frame (the egress
	 * slot's own under the #918 split). Each segment's views are cropped from
	 * where they were painted and woven over the live segment, never cut at
	 * the live seam. NULL / count 0 = one view set (unrouted): the live
	 * partition is exact.
	 */
	const struct comp_segments_content *content;
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
	/*!
	 * #2245 (ADR-047 Amendment 9): the OS monitors and their current refresh
	 * rates (parallel arrays, 0 Hz = unknown). When set, every segment DP but
	 * the primary's is handed its own screen's frame period through
	 * set_frame_timing (residual 0 = unknown, the DP's heuristic) — the
	 * screen is matched to a monitor by its centre. NULL (zero-init) feeds
	 * segment DPs no timing, as before.
	 */
	const struct comp_segments_screen *rate_screens;
	const float *rate_hz;
	uint32_t rate_count;
	//! ADR-060: optional out — set true when any part of the canvas was written
	//! flat 2D (a segment that could not weave, or canvas off every screen).
	bool *out_any_flat;
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
 * The session-wide hardware 2D/3D mode. Each segment DP's lens wish follows
 * it ON THE SCREENS THE WINDOW COVERS (#2247): @p enable_3d where the last
 * update's table has a segment on the DP's screen, 2D on a screen the window
 * has wholly left for the hand-off's 0.5 s dwell
 * (comp_segments_cover_dwell); covered again, at once. Sent to each live DP on a change
 * and to every newly created one. The primary DP gets its wish from the
 * compositor's own request path, gated by
 * @ref comp_d3d11_segments_primary_covered. Weave thread, after
 * comp_d3d11_segments_update.
 */
void
comp_d3d11_segments_set_display_mode(struct comp_d3d11_segments *segs, bool enable_3d);

/*!
 * #2247: does the window cover the PRIMARY screen, per the last update's
 * table and the hand-off's 0.5 s dead band (comp_segments_cover_dwell:
 * uncovered only after that long off it, covered again at once)? True when segmentation is off, there is no primary screen, or the
 * table is empty (no coverage information) — so a window wholly on the
 * primary, and every single-screen session, keep the shipped behaviour. A
 * compositor whose session DP weaves the primary sends that DP
 * `session_3d && primary_covered`. Weave thread (it reads the table).
 */
bool
comp_d3d11_segments_primary_covered(const struct comp_d3d11_segments *segs);

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

/*!
 * #2218 (#4): publish the session's local zone mask to every segment DP that
 * is not the session's own (the primary / the window owner, which the
 * compositor publishes to itself). The vendor's local-zone verdict is per
 * PANEL — any 3D pixel on a screen puts that screen's lens in 3D — so each
 * segment DP receives the mask CROPPED to its segment
 * (@ref comp_segments_mask_crop_rect), tagged with a generation that changes
 * with the mask generation and with the crop rect, so the vendor re-evaluates
 * exactly when either moved. A segment DP without local-zone support, or a
 * screen the window has left, is cleared instead.
 *
 * @param d3d11_context  The context of the device the manager was created on
 *                       (the output device under the split) — the mask SRV must
 *                       live on that device, like the atlas.
 * @param mask_srv       The published mask (R8_UNORM), spanning the window.
 * @param mask_w/mask_h  Mask dimensions (mask px; may differ from the window).
 * @param win_w/win_h    The client window in window px (the mask's extent).
 * @param seq            The mask content generation (the compositor's
 *                       zone_publish_seq).
 *
 * Weave thread, after @ref comp_d3d11_segments_update of the same frame (it
 * reads the table that update produced).
 */
void
comp_d3d11_segments_publish_zone_mask(struct comp_d3d11_segments *segs,
                                      void *d3d11_context,
                                      void *mask_srv,
                                      uint32_t mask_w,
                                      uint32_t mask_h,
                                      uint32_t win_w,
                                      uint32_t win_h,
                                      uint64_t seq);

/*!
 * #2218 (#4): withdraw the zone mask from every segment DP that has one
 * published (the frame is no longer a zones frame, or the session is tearing
 * down). Idempotent; a manager with nothing published is a no-op.
 */
void
comp_d3d11_segments_clear_zone_mask(struct comp_d3d11_segments *segs);

#ifdef __cplusplus
}
#endif
