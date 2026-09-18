// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1488: the XR_EXT_view_configuration_views_change state machine.
 * @ingroup oxr_main
 */

#include "oxr_views_change.h"

#include "util/u_tiling.h"

#include <string.h>

int
oxr_views_change_init(struct oxr_views_change *vc)
{
	memset(vc, 0, sizeof(*vc));
	return os_mutex_init(&vc->lock);
}

void
oxr_views_change_fini(struct oxr_views_change *vc)
{
	os_mutex_destroy(&vc->lock);
}

void
oxr_views_change_seed(struct oxr_views_change *vc, const XrViewConfigurationView *frozen, uint32_t view_count)
{
	if (view_count > XRT_MAX_VIEWS) {
		view_count = XRT_MAX_VIEWS;
	}
	for (uint32_t i = 0; i < view_count; i++) {
		vc->views_live[i] = frozen[i];
	}
	vc->valid = false;

	// Seed the edge detector from the snapshot, NOT from the first frame.
	// The reference value the app holds is the frozen array, so a first
	// frame whose compositor dims already differ from it (a window created
	// after the instance, a DPI change) IS a change the app must hear
	// about - Design 7's rule is "emit iff enumerate would now answer
	// differently", and it already would. Baselining on the first sample
	// would leave the app stale until the NEXT change.
	if (view_count > 0) {
		vc->last_w = frozen[0].recommendedImageRectWidth;
		vc->last_h = frozen[0].recommendedImageRectHeight;
	}
}

bool
oxr_views_change_update(struct oxr_views_change *vc,
                        const XrViewConfigurationView *base,
                        uint32_t view_count,
                        uint32_t w,
                        uint32_t h,
                        uint64_t now_ns,
                        bool ext_enabled,
                        struct oxr_views_change_stats *out_stats)
{
	// A client that has not enabled XR_EXT_view_configuration_views_change
	// (or that has DXR_VIEWS_CHANGE_LIVE off) pays nothing here: no lock,
	// no copy, no event. DXR_VIEWS_CHANGE_EVENT deliberately does NOT reach
	// this gate - folding it in here would skip the shadow write and freeze
	// the live enumerate values, which is not what that switch means. The
	// bespoke XrEventDataLocal3DZoneViewSizeChangedDXR path at the call site
	// is entirely separate and stays byte-identical.
	if (!ext_enabled) {
		return false;
	}
	if (w == 0 || h == 0) {
		return false;
	}
	if (view_count == 0) {
		return false; // base[0] is the clamp + edge reference below
	}
	if (view_count > XRT_MAX_VIEWS) {
		view_count = XRT_MAX_VIEWS;
	}

	bool push = false;

	os_mutex_lock(&vc->lock);

	// CLAMP FIRST, then edge-detect on the clamped value. The compositor
	// getters return window/canvas x view_scale with no ceiling, while the
	// frozen snapshot is display-derived and already clamped the same way, so
	// an oversized window (DPI virtualisation, a multi-monitor span) would
	// otherwise publish recommended > max, which the spec forbids. Detecting
	// on the clamped value also means a window growing further past the
	// ceiling produces no extra edges and no extra doorbells.
	//
	// view 0 is the edge-detection representative, matching the seed.
	const uint32_t cw = w < base[0].maxImageRectWidth ? w : base[0].maxImageRectWidth;
	const uint32_t ch = h < base[0].maxImageRectHeight ? h : base[0].maxImageRectHeight;

	// Edge detection against the seeded snapshot (see oxr_views_change_seed),
	// so the very first frame can already be a change. The last_w != 0 test
	// survives only as a never-seeded fallback - it is not the baseline-on-
	// first-sample behaviour the DXR doorbell at the call site still uses,
	// and that one is left byte-identical on purpose.
	if (vc->last_w != 0 && (vc->last_w != cw || vc->last_h != ch)) {
		for (uint32_t i = 0; i < view_count; i++) {
			// Copy the whole struct verbatim, THEN move only the
			// two recommended fields. maxImageRect{Width,Height}
			// and both sample counts are never assigned in this
			// file, so ADR-010's worst-case swapchain invariant and
			// the extension's "must: only change the content of the
			// recommended values" are enforced structurally rather
			// than by convention.
			// max* is per view, so clamp per view rather than
			// reusing view 0's ceiling.
			vc->views_live[i] = base[i];
			vc->views_live[i].recommendedImageRectWidth =
			    w < base[i].maxImageRectWidth ? w : base[i].maxImageRectWidth;
			vc->views_live[i].recommendedImageRectHeight =
			    h < base[i].maxImageRectHeight ? h : base[i].maxImageRectHeight;
		}
		vc->valid = true;
		vc->edges++;

		// THE Design-7 GUARANTEE (#1488). This is the only assignment of
		// pending_push to true anywhere, and it sits in the same critical
		// section as the views_live write above. Since the doorbell below
		// is a pure function of pending_push, the event is structurally
		// unreachable unless xrEnumerateViewConfigurationViews would now
		// answer differently. A consumer of the LOVR shape - which calls
		// createSwapchains() unconditionally on the event - therefore
		// cannot be made to reallocate for a value that did not move.
		vc->pending_push = true;
	}
	vc->last_w = cw;
	vc->last_h = ch;

	// Spec: "The runtime must: not use this event for frequent (at a rate
	// faster than 1Hz per view configuration) adjustments of the
	// resolution."
	//
	// Coalescing: views_live is written on EVERY change, so the next
	// xrEnumerateViewConfigurationViews always answers with the latest
	// value - only the doorbell is throttled. pending_push survives the
	// suppression and is re-tested here on every later frame end, change or
	// not, so a drag-resize burst yields exactly one event, carrying the
	// coalesced final size, at most ~1 s late.
	//
	// RESIDUAL GAP (#1488): this re-test rides xrEndFrame, so an app that
	// stops submitting frames immediately after a suppressed change gets no
	// doorbell until its next xrEndFrame. There is no timer thread on this
	// path, and an app that is not submitting frames is not rendering - the
	// stale value it would act on is one it is not using.
	if (vc->pending_push && (now_ns - vc->last_push_ns) >= OXR_VIEWS_CHANGE_MIN_PERIOD_NS) {
		vc->pending_push = false;
		vc->last_push_ns = now_ns;
		vc->emitted++;
		// Edges that got folded into this one doorbell.
		vc->suppressed = vc->edges - vc->emitted;
		push = true;
	}

	if (out_stats != NULL) {
		out_stats->edges = vc->edges;
		out_stats->emitted = vc->emitted;
		out_stats->suppressed = vc->suppressed;
		out_stats->last_w = vc->last_w;
		out_stats->last_h = vc->last_h;
	}

	os_mutex_unlock(&vc->lock);

	return push;
}

const XrViewConfigurationView *
oxr_views_change_select(struct oxr_views_change *vc,
                        const XrViewConfigurationView *frozen,
                        uint32_t count,
                        bool ext_enabled,
                        bool live_enabled,
                        XrViewConfigurationView *scratch)
{
	// The whole back-compat story: an app that has not enabled the
	// extension keeps the frozen xrCreateInstance-time snapshot
	// bit-for-bit, which is the core spec's unconditional "always return
	// identical buffer contents" rule. Every shipping consumer today is in
	// this bucket.
	if (!ext_enabled || !live_enabled) {
		return frozen;
	}
	if (count > XRT_MAX_VIEWS) {
		count = XRT_MAX_VIEWS;
	}

	const XrViewConfigurationView *out = frozen;

	os_mutex_lock(&vc->lock);
	// `valid` is tested INSIDE the critical section - it is written by the
	// xrEndFrame thread under this same lock, and this read can come from
	// any thread (xrEnumerateViewConfigurationViews is instance-level).
	if (vc->valid) {
		for (uint32_t i = 0; i < count; i++) {
			scratch[i] = vc->views_live[i];
		}
		out = scratch;
	}
	os_mutex_unlock(&vc->lock);

	return out;
}

bool
oxr_views_change_size_from_window(const struct xrt_rendering_mode *mode,
                                  const struct xrt_window_metrics *wm,
                                  bool legacy_app_tile_scaling,
                                  uint32_t *out_w,
                                  uint32_t *out_h)
{
	if (mode == NULL || wm == NULL || out_w == NULL || out_h == NULL) {
		return false;
	}

	// R4 on the IPC path. The native compositors get this for free -
	// layer_commit's `if (!c->legacy_app_tile_scaling && ...)` means their
	// renderer view dims never move for a legacy app, so the getter keeps
	// answering the compromise size and no edge is ever detected. Nothing
	// equivalent exists on this side of the IPC boundary.
	if (legacy_app_tile_scaling) {
		return false;
	}

	if (!wm->valid || wm->window_pixel_width == 0 || wm->window_pixel_height == 0) {
		return false;
	}

	uint32_t w = 0;
	uint32_t h = 0;
	u_tiling_compute_canvas_view(mode, wm->window_pixel_width, wm->window_pixel_height, &w, &h);
	if (w == 0 || h == 0) {
		return false;
	}

	*out_w = w;
	*out_h = h;
	return true;
}
