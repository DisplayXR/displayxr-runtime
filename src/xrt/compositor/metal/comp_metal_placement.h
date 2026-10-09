// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned, phase-snapped, atomic window placement (drag +
 *         resize) for the in-process Metal compositor on macOS (ADR-050).
 * @ingroup comp_metal
 *
 * ## Why the runtime owns the gesture
 *
 * A lenticular weave is a function of where the window's pixels land on the
 * panel. AppKit's native title-bar drag is performed by the WindowServer: it
 * slides the LAST presented frame to every intermediate position without the
 * app re-weaving it, so the 3D stutters for the whole drag; its live resize
 * runs a tracking loop inside `-[NSApp sendEvent:]` that blocks the render
 * loop. Neither can be fixed from the display processor.
 *
 * So, for every eligible Metal session, this module takes the gesture:
 *
 *  - the window is made `movable = NO`; a local NSEvent monitor swallows the
 *    left-mouse-down that would start a title-bar drag or an edge / corner
 *    resize (hit-tested at the EVENT's location — the cursor may have left
 *    the 8 pt edge zone by the time a queued event is pumped) and the drags
 *    that follow, turning them into a pending content rect;
 *  - the zoom button / title-bar double-click is intercepted through a
 *    delegate PROXY (`windowShouldZoom:toFrame:` declines AppKit's animated
 *    zoom and queues the proposed frame on the same path) that forwards every
 *    other message to the app's own delegate, so an app delegate is never
 *    clobbered;
 *  - the compositor applies the pending rect, phase-snapped by the display
 *    processor (`snap_window_rect`, anchored at the gesture start), in the
 *    SAME Core Animation transaction that presents the frame woven for it
 *    (`presentsWithTransaction`), so the window and its weave move together.
 *
 * ## Units
 *
 * Gesture state is kept in AppKit global points (bottom-up). The snap runs in
 * BACKING pixels (points × the window's backing scale); only the displacement
 * from the gesture-start anchor matters, so the absolute frame cancels. On a
 * Retina screen a window origin can only land on whole points, i.e. on
 * `anchor + scale·Z` backing px — the reachable lattice is searched the way
 * the vk_native X11 drag does (u_x11_reachable_round), so the window never
 * lands one pixel off the phase the DP asked for.
 *
 * ## Threading
 *
 * The event monitor and the delegate proxy run on the main thread and only
 * write the pending rect under a lock. Everything else runs on the thread
 * calling xrEndFrame. When that IS the main thread (the test apps, hosted
 * sessions) the move and the present are one transaction. When it is not,
 * the move + present is handed to the main thread and waited on for about one
 * refresh; on timeout the commit thread presents itself and the move lands
 * non-atomically later (at most one off-phase frame) — never a deadlock.
 *
 * ## App-initiated moves
 *
 * A frame change made outside this path (setFrameOrigin from the app, a
 * display reconfiguration) is detected at the next present by comparing the
 * window frame with the last frame this module applied; the new origin is
 * snapped relative to the last PRESENTED origin and applied on that present.
 * The one frame the app's own move showed is off-phase (≤ 1 frame; documented
 * limit).
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_placement;

/*!
 * The snap provider: the compositor answers with the display processor of
 * the screen holding the content origin. Called on the commit thread only.
 *
 * @param userdata  As passed to @ref comp_metal_placement_create.
 * @param probe_x   Top-down global points of the content origin (to pick the
 *                  screen / DP).
 * @param probe_y   See @p probe_x.
 * @param origin_x  Gesture-start content left, backing px.
 * @param origin_y  Gesture-start content top, backing px.
 * @param target_x  Proposed content left, backing px (same frame).
 * @param target_y  Proposed content top, backing px (same frame).
 * @param[out] out_x  Snapped content left (valid on true).
 * @param[out] out_y  Snapped content top (valid on true).
 * @return true when a DP produced a snap; false = use the target.
 */
typedef bool (*comp_metal_placement_snap_fn)(void *userdata,
                                             double probe_x,
                                             double probe_y,
                                             int32_t origin_x,
                                             int32_t origin_y,
                                             int32_t target_x,
                                             int32_t target_y,
                                             int32_t *out_x,
                                             int32_t *out_y);

/*!
 * Does this session get runtime-owned placement? False (with a one-line
 * reason in @p out_why) for: `DXR_MACOS_NATIVE_DRAG=1`, an app that set
 * `XR_COCOA_WINDOW_PLACEMENT_APP_OWNED_BIT_DXR`, an offscreen or
 * shared-IOSurface session, a workspace (shell) session, a view that is not
 * its window's contentView, and no window at all.
 *
 * @param ns_view            The NSView the compositor presents into (borrowed).
 * @param offscreen          Offscreen session.
 * @param shared_iosurface   Shared-IOSurface (`_texture`) session.
 * @param app_owned          The app opted out through the Cocoa binding.
 */
bool
comp_metal_placement_eligible(
    void *ns_view, bool offscreen, bool shared_iosurface, bool app_owned, const char **out_why);

/*!
 * Take over the window's drag + resize. Runs its AppKit setup on the main
 * thread (synchronously). NULL on failure.
 *
 * @param ns_view   The bound view (its window's contentView).
 * @param snap      Snap provider (may be NULL = never snapped, still atomic).
 * @param userdata  Passed to @p snap.
 */
struct comp_metal_placement *
comp_metal_placement_create(void *ns_view, comp_metal_placement_snap_fn snap, void *userdata);

/*!
 * Give the window back to AppKit (restores movable + the app's delegate) and
 * free. Safe with NULL. Runs its AppKit teardown on the main thread.
 */
void
comp_metal_placement_destroy(struct comp_metal_placement **p_ptr);

/*!
 * One present's placement decision, filled by
 * @ref comp_metal_placement_begin_present and consumed by
 * @ref comp_metal_placement_end_present.
 */
struct comp_metal_placement_frame
{
	//! The content rect this present is woven for, top-down global POINTS
	//! (origin = the main display's top-left — the CGDisplayBounds space).
	double content_x, content_y, content_w, content_h;
	//! The window's backing scale.
	double scale;
	//! The present origin for @ref content_x / @ref content_y: backing px
	//! relative to the top-left of the screen holding the content origin
	//! (what xrt_display_processor_metal::set_present_origin takes).
	int32_t present_origin_x, present_origin_y;
	//! The content top-left in global top-down backing px (points × scale).
	int32_t origin_px_x, origin_px_y;
	//! True when this present moves or resizes the window.
	bool moves;
	bool resizes;
	//! Internal: a CA transaction is open on this thread (main path).
	bool txn_open;
	//! Internal: the move is handed to the main thread at present.
	bool deferred;
	//! Internal: the frame rect (AppKit, bottom-up points) to apply.
	double frame_x, frame_y, frame_w, frame_h;
};

/*!
 * Start a present: decide where this frame goes, and — on the main thread —
 * open the CA transaction and apply the move / resize (setFrameOrigin or
 * setFrame:display:NO + drawableSize) BEFORE the drawable is taken. Call
 * before `-nextDrawable`.
 *
 * @param p            The placement.
 * @param metal_layer  The CAMetalLayer (`presentsWithTransaction` is set YES).
 * @param[out] out     This present's decision.
 * @return false when placement is not active this frame (window gone,
 *         full-screen, minimised): present the usual way.
 */
bool
comp_metal_placement_begin_present(struct comp_metal_placement *p,
                                   void *metal_layer,
                                   struct comp_metal_placement_frame *out);

/*!
 * Finish a present started with @ref comp_metal_placement_begin_present.
 * The caller has committed @p command_buffer WITHOUT `presentDrawable`; this
 * waits until it is scheduled, presents @p drawable and commits the
 * transaction (or hands both to the main thread). @p drawable may be nil
 * (frame skipped) — the transaction is still closed.
 */
void
comp_metal_placement_end_present(struct comp_metal_placement *p,
                                 void *command_buffer,
                                 void *drawable,
                                 struct comp_metal_placement_frame *f);

#ifdef __cplusplus
}
#endif
