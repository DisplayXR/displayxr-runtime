// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1488: the XR_EXT_view_configuration_views_change state machine.
 *
 * Holds the shadow copy of @ref oxr_system::views whose
 * recommendedImageRect{Width,Height} are allowed to move, plus the
 * spec-mandated 1 Hz doorbell throttle.
 *
 * Deliberately a standalone translation unit with no dependency on
 * oxr_objects.h: the unit test (tests/tests_oxr_view_config_views_change.cpp)
 * compiles this .c straight into the test binary and drives it with an
 * injected @p now_ns, so the throttle is testable without sleeping, without a
 * compositor and without a wall clock. See the file header of that test.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"

#include "os/os_threading.h"

#include <openxr/openxr.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_rendering_mode;

/*!
 * The spec's rate limit: "The runtime must: not use this event for frequent
 * (at a rate faster than 1Hz per view configuration) adjustments of the
 * resolution." Not a tuning knob.
 */
#define OXR_VIEWS_CHANGE_MIN_PERIOD_NS (1000 * 1000 * 1000ULL)

/*!
 * Churn snapshot handed back by @ref oxr_views_change_update, read under the
 * lock so a caller never has to touch @ref oxr_views_change itself.
 *
 * The one soak line the runtime prints per doorbell is built from exactly these
 * fields, with the literal prefix "views-change:" that PR B and the hardware
 * soak grep on. Do not reword that prefix.
 */
struct oxr_views_change_stats
{
	uint32_t edges;          //!< Times the shadow was written (real size changes).
	uint32_t emitted;        //!< Doorbells authorised.
	uint32_t suppressed;     //!< Edges coalesced into a later push (edges - emitted at emit time).
	uint32_t last_w, last_h; //!< The clamped dims the shadow now holds.
};

/*!
 * Live-view-size state. Embedded by value in @ref oxr_system.
 *
 * ONE instance == ONE view configuration: the spec's rate limit is worded "per
 * view configuration", so a runtime that later advertises more than one view
 * configuration type holds one of these per type, with no API change here -
 * every entry point below takes the count (and the caller the type) explicitly.
 *
 * @ref lock guards @ref views_live, @ref valid, @ref last_w / @ref last_h,
 * @ref last_push_ns and @ref pending_push. It is a DEDICATED mutex on purpose:
 * oxr_instance::event::mutex is already held by the event push, and
 * oxr_system::sync_actions_mutex is trylock-probed by the GET_XDEV_BY_ROLE
 * macros — giving either a second purpose invites a deadlock (#1488 R3).
 */
struct oxr_views_change
{
	//! Shadow of oxr_system::views. Only the two recommended fields ever move.
	XrViewConfigurationView views_live[XRT_MAX_VIEWS];

	//! True once a real size change has been written into @ref views_live.
	bool valid;

	//! Last dims seen by @ref oxr_views_change_update, for edge detection. 0 = no sample yet.
	uint32_t last_w, last_h;

	//! Monotonic ns of the last emitted doorbell, for the 1 Hz throttle.
	uint64_t last_push_ns;

	//! A change was written but its doorbell was throttled; fire it when the window closes.
	bool pending_push;

	//! Churn counters for the soak line; see @ref oxr_views_change_stats.
	uint32_t edges, emitted, suppressed;

	struct os_mutex lock;
};

/*!
 * Zero the state and create @ref oxr_views_change::lock. Returns < 0 on failure.
 */
int
oxr_views_change_init(struct oxr_views_change *vc);

/*!
 * Destroy @ref oxr_views_change::lock.
 */
void
oxr_views_change_fini(struct oxr_views_change *vc);

/*!
 * Seed the shadow from the frozen xrCreateInstance-time snapshot. Leaves
 * @ref oxr_views_change::valid false, so the read path keeps answering with the
 * frozen array until a real change lands.
 *
 * ALSO seeds the edge detector from @p frozen[0], which is what makes the FIRST
 * frame comparable. The reference value the app holds is the frozen snapshot,
 * so if the first compositor dims already differ from it - a window created
 * after the instance, a DPI change - that IS a change the app must be told
 * about, and Design 7's rule ("emit iff enumerate would now answer
 * differently") demands it. Baselining on the first sample instead would leave
 * the app stale until the NEXT change.
 */
void
oxr_views_change_seed(struct oxr_views_change *vc, const XrViewConfigurationView *frozen, uint32_t view_count);

/*!
 * Feed the just-computed per-view render dims.
 *
 * On a real change (and only then) rewrites @ref oxr_views_change::views_live
 * from @p base with @p w / @p h substituted into recommendedImageRect{Width,
 * Height} — maxImageRect* and both sample counts are copied verbatim and never
 * assigned, so ADR-010's worst-case invariant and the extension's
 * "recommended values only" must: are structural rather than conventional.
 *
 * @p w / @p h are CLAMPED per view to that view's maxImageRect{Width,Height}.
 * The compositor getters return window/canvas x view_scale with no ceiling,
 * while the frozen snapshot is display-derived and already clamped
 * (oxr_system.c applies the same imin), so a window larger than the display —
 * DPI virtualisation, a multi-monitor span — would otherwise publish a
 * recommended value above max, which the spec forbids. Edge detection runs on
 * the CLAMPED value, so a window growing further past the ceiling produces no
 * additional edges and no additional doorbells.
 *
 * @param ext_enabled The calling session's instance has
 *        XR_EXT_view_configuration_views_change enabled AND DXR_VIEWS_CHANGE_LIVE
 *        is on. NOT DXR_VIEWS_CHANGE_EVENT - see the note below. When false this
 *        returns immediately: no lock, no copy, no event, so a non-EXT app pays
 *        nothing beyond the pre-existing code.
 * @param now_ns Monotonic nanoseconds; injected so the throttle is testable.
 * @param out_stats Optional. Receives the churn counters + the clamped dims,
 *        read under the lock so a caller never has to touch @p vc itself.
 *
 * @return true iff the caller should push XrEventDataViewConfigurationViewsChangedEXT.
 *
 * THE CALLER OWNS THE EVENT KILL SWITCH, NOT THIS FUNCTION. DXR_VIEWS_CHANGE_EVENT
 * gates only the push; it must NOT be folded into @p ext_enabled, because that
 * would skip the shadow write and silently freeze the live enumerate values too.
 * The two switches are independent by contract: EVENT=0 means "live values still
 * move, you just never get a doorbell" (the spec's explicit `may:` ignore), while
 * LIVE=0 means "frozen enumerate AND no doorbell" - a notification for a value
 * that cannot move is the reallocation hazard this design exists to prevent, so
 * LIVE=0 is the one that legitimately belongs in @p ext_enabled.
 *
 * When the caller declines to push, the throttle state has still advanced: the
 * doorbell was authorised and consumed. That is deliberate - it keeps EVENT=0
 * from accumulating a backlog that would fire in a burst if the switch flipped.
 */
bool
oxr_views_change_update(struct oxr_views_change *vc,
                        const XrViewConfigurationView *base,
                        uint32_t view_count,
                        uint32_t w,
                        uint32_t h,
                        uint64_t now_ns,
                        bool ext_enabled,
                        struct oxr_views_change_stats *out_stats);

/*!
 * Pick the array xrEnumerateViewConfigurationViews must answer from.
 *
 * @return @p frozen (the xrCreateInstance-time snapshot) unless the extension is
 *         enabled, the DXR_VIEWS_CHANGE_LIVE kill switch is on, and a change has
 *         actually landed — in which case @p scratch, filled with exactly
 *         @p count entries - the count of the REQUESTED view configuration type,
 *         never a baked-in sys->view_count.
 */
/*!
 * Default staleness ceiling for the session's cached window metrics; see
 * @ref oxr_views_change_cached_window_metrics. Generous on purpose: this is a
 * geometry hint whose consumer already edge-detects, and a 60 Hz app refreshes
 * it every ~16 ms, so the only samples it rejects come from a frame that
 * located no views at all.
 */
#define OXR_VIEWS_CHANGE_WM_MAX_AGE_NS (50 * 1000 * 1000ULL)

/*!
 * The cache policy behind oxr_session_get_window_metrics_cached(): decide
 * whether a remembered window-metrics sample may serve this frame, and COPY it
 * out when it may.
 *
 * Extracted here, with @p now_ns injected, for the same reason the throttle
 * was: this TU compiles straight into the unit test, so the policy - and in
 * particular the fact that a hit WRITES @p out_metrics - is pinned by a test
 * rather than by review. A hit that returned true without writing would be
 * silent and total: the fire site passes a zeroed struct, so every hit would
 * present valid=false to @ref oxr_views_change_size_from_window, the IPC leg
 * would produce no dims in the steady state, and the doorbell would fire only
 * on the rare stale-fallback frame.
 *
 * @param cached      The remembered sample.
 * @param cached_valid Whether anything has been remembered yet.
 * @param cached_ns   Monotonic ns at which @p cached was taken.
 * @param now_ns      Monotonic ns now; injected so staleness is testable.
 * @param max_age_ns  Ceiling, normally @ref OXR_VIEWS_CHANGE_WM_MAX_AGE_NS.
 *                    An age >= this is stale.
 * @param[out] out_metrics Written ONLY on a hit, and left untouched otherwise
 *                    so the caller can fall through to the real query.
 *
 * @return true iff @p out_metrics now holds the cached sample.
 */
bool
oxr_views_change_cached_window_metrics(const struct xrt_window_metrics *cached,
                                       bool cached_valid,
                                       uint64_t cached_ns,
                                       uint64_t now_ns,
                                       uint64_t max_age_ns,
                                       struct xrt_window_metrics *out_metrics);

/*!
 * Derive the per-view render size an IPC/shell-hosted session should publish,
 * from the window rect the service reported plus the active rendering mode.
 *
 * #1488 PR B. This is the IPC leg's stand-in for
 * comp_*_compositor_get_recommended_view_size(), and it is deliberately the
 * SAME arithmetic those getters end up performing: every native compositor
 * returns its renderer's view dims, which layer_commit recomputes each frame as
 * u_tiling_compute_canvas_view(mode, canvas-or-window px) - see
 * comp_d3d11_compositor.cpp's layer_commit and
 * comp_d3d11_compositor_get_recommended_view_size(). A `_handle` app and an
 * `_ipc` app at the same window size therefore publish the same number.
 *
 * It lives in THIS translation unit, not in oxr_session.c, for exactly the
 * reason the file header gives: this TU is compiled straight into
 * tests/tests_oxr_view_config_views_change.cpp, so the derivation is unit
 * testable with no session, no compositor and no IPC.
 *
 * NOT applied here, on purpose:
 *
 * - OXR_VIEWPORT_SCALE_PERCENTAGE (oxr_system.c's `scale`, DEFAULT 100). The
 *   frozen snapshot multiplies by it; the native getters never do. Matching the
 *   native getters is what keeps the two paths comparable, and at the default
 *   the distinction does not exist. A box that sets it to something else
 *   already sees the same native-vs-frozen skew today, on every backend - that
 *   is a pre-existing property of the compositor getters, not something the IPC
 *   leg should invent a second answer for.
 * - The maxImageRect* ceiling. oxr_views_change_update() clamps per view and
 *   edge-detects on the CLAMPED value, so doing it twice would be redundant and
 *   could disagree.
 *
 * @param mode The session's ACTIVE rendering mode. Over IPC the client proxy
 *        mirrors the whole table (ipc_client_hmd.c), and the active index is
 *        refreshed both by update_inputs and by the
 *        XRT_SESSION_EVENT_RENDERING_MODE_CHANGE handler in oxr_session.c - so
 *        even a submit-only session that runs no xrWaitFrame has it current.
 * @param wm Window metrics from oxr_session_get_window_metrics(). Under the
 *        shell these are the service-side virtual TILE rect, not the client's
 *        own HWND, which is exactly the canvas the shell composites.
 * @param legacy_app_tile_scaling xrt_system_compositor_info::legacy_app_tile_scaling.
 *        A legacy app is pinned to the compromise scale and the native
 *        compositors skip the per-frame view-dim recompute entirely for it; the
 *        IPC leg has no such compositor-side guard, so it must refuse here or
 *        #1488's R4 ("a legacy app can never observe a changed enumerate
 *        result") would stop holding on this path alone.
 *
 * @return false - leaving @p out_w / @p out_h untouched - when there is no
 *         usable answer. In particular the Linux service build's
 *         ipc_handle_compositor_get_window_metrics() has no per-client window
 *         source at all and always reports valid=false, so this returns false
 *         there and the whole IPC leg is a no-op on Linux.
 */
bool
oxr_views_change_size_from_window(const struct xrt_rendering_mode *mode,
                                  const struct xrt_window_metrics *wm,
                                  bool legacy_app_tile_scaling,
                                  uint32_t *out_w,
                                  uint32_t *out_h);

const XrViewConfigurationView *
oxr_views_change_select(struct oxr_views_change *vc,
                        const XrViewConfigurationView *frozen,
                        uint32_t count,
                        bool ext_enabled,
                        bool live_enabled,
                        XrViewConfigurationView *scratch);

#ifdef __cplusplus
}
#endif
