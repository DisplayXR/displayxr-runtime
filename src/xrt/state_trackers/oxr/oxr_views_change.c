// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1488: the XR_EXT_view_configuration_views_change state machine.
 * @ingroup oxr_main
 */

#include "oxr_views_change.h"

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
}

bool
oxr_views_change_update(struct oxr_views_change *vc,
                        const XrViewConfigurationView *base,
                        uint32_t view_count,
                        uint32_t w,
                        uint32_t h,
                        uint64_t now_ns,
                        bool ext_enabled)
{
	// A client that has not enabled XR_EXT_view_configuration_views_change
	// (or that has a kill switch off) pays nothing here: no lock, no copy,
	// no event. The bespoke XrEventDataLocal3DZoneViewSizeChangedDXR path
	// at the call site is entirely separate and stays byte-identical.
	if (!ext_enabled) {
		return false;
	}
	if (w == 0 || h == 0) {
		return false;
	}
	if (view_count > XRT_MAX_VIEWS) {
		view_count = XRT_MAX_VIEWS;
	}

	bool push = false;

	os_mutex_lock(&vc->lock);

	// Edge detection. The first sample only baselines - it does not fire -
	// mirroring the #441 pattern the DXR doorbell above already uses.
	if (vc->last_w != 0 && (vc->last_w != w || vc->last_h != h)) {
		for (uint32_t i = 0; i < view_count; i++) {
			// Copy the whole struct verbatim, THEN move only the
			// two recommended fields. maxImageRect{Width,Height}
			// and both sample counts are never assigned in this
			// file, so ADR-010's worst-case swapchain invariant and
			// the extension's "must: only change the content of the
			// recommended values" are enforced structurally rather
			// than by convention.
			vc->views_live[i] = base[i];
			vc->views_live[i].recommendedImageRectWidth = w;
			vc->views_live[i].recommendedImageRectHeight = h;
		}
		vc->valid = true;

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
	vc->last_w = w;
	vc->last_h = h;

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
		push = true;
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
