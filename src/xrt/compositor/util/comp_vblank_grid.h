// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A vsync-locked vblank grid built from RETROSPECTIVE present timing.
 * @ingroup comp_util
 *
 * ## Why this exists
 *
 * Late weave and the #868 repaint loop both need the same thing: "when is the
 * next scanout, and how far ahead of it am I?" On Windows that comes from DXGI
 * frame statistics (`comp_weave_latency_win.h`). On Android there is no
 * `VK_KHR_present_wait` on Adreno at all — measured on the NP02J: 113 device
 * extensions, `present_wait=0`, `present_id=0`, `VK_GOOGLE_display_timing=1` —
 * so the blocking primitive the VK pacing path is built around never resolves,
 * and both consumers fall back to an OPEN-LOOP guess.
 *
 * That guess is measurably wrong. The repaint loop announces "repainting last
 * atlas at 60.0 Hz" while the platform is switching the panel between 60, 90,
 * 120 and 144 underneath it (nubia NP02J: `SUPPORTED_RR: [60, 90, 120, 144]`,
 * `setDesiredConfig fps:120`), and the panel lands at ~43 fps with the GPU only
 * ~71% busy — headroom to spare, and still not panel rate, because the presents
 * are scheduled against a clock that does not exist.
 *
 * This module is the source-agnostic half of the fix: feed it observations of
 * where presents ACTUALLY landed plus a measured refresh period, and it answers
 * the two scheduling questions. It deliberately knows nothing about Vulkan or
 * DXGI so the logic can be unit-tested on the host, which is where the real
 * bugs are — drift, staleness, and mode changes.
 *
 * ## What "trusted" means
 *
 * A grid is trusted only when a measured period exists AND a recent observation
 * anchors it. Both halves matter: a period with no anchor cannot place a vblank,
 * and an anchor with no period cannot step. An untrusted grid answers 0 and the
 * caller keeps its existing fallback — never a guessed answer that looks real,
 * which is the failure mode that made the open-loop path so hard to diagnose.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! An observation older than this cannot anchor the grid (ns).
#define COMP_VBLANK_GRID_STALE_NS (500ULL * 1000ULL * 1000ULL)

//! Plausible refresh periods: 24 Hz .. 500 Hz. Anything outside is a bad read.
#define COMP_VBLANK_GRID_MIN_PERIOD_NS (2ULL * 1000ULL * 1000ULL)
#define COMP_VBLANK_GRID_MAX_PERIOD_NS (42ULL * 1000ULL * 1000ULL)

/*!
 * A vblank grid: a phase anchor plus a period, both derived from measurement.
 */
struct comp_vblank_grid
{
	//! Measured refresh period (ns). 0 = unknown.
	uint64_t period_ns;
	//! Timestamp of the most recent observed present (ns, monotonic).
	uint64_t anchor_ns;
	//! Whether @ref anchor_ns has ever been set.
	bool have_anchor;
	//! Observations accepted since the last reset — diagnostics only.
	uint32_t samples;
	//! Period changes seen (a platform mode switch) — diagnostics only.
	uint32_t period_changes;
};

/*!
 * Publish a measured refresh period. Implausible values are REFUSED rather than
 * clamped: a clamp would silently manufacture a grid out of a bad read, and the
 * whole point here is that an untrusted grid says so.
 *
 * @return true if the period was accepted.
 */
static inline bool
comp_vblank_grid_set_period(struct comp_vblank_grid *g, uint64_t period_ns)
{
	if (g == NULL || period_ns < COMP_VBLANK_GRID_MIN_PERIOD_NS ||
	    period_ns > COMP_VBLANK_GRID_MAX_PERIOD_NS) {
		return false;
	}
	if (g->period_ns != 0 && g->period_ns != period_ns) {
		// The platform switched the panel mode. Keep the anchor: it is still
		// a real vblank, and the new period steps correctly from it.
		g->period_changes++;
	}
	g->period_ns = period_ns;
	return true;
}

/*!
 * Feed the grid a present that actually reached the panel.
 *
 * Only ever moves the anchor FORWARD. Presentation timing arrives out of order
 * on some drivers, and letting a late-delivered older sample rewind the anchor
 * would drag the whole schedule backwards.
 *
 * @return true if this observation moved the anchor.
 */
static inline bool
comp_vblank_grid_observe(struct comp_vblank_grid *g, uint64_t actual_present_ns)
{
	if (g == NULL || actual_present_ns == 0) {
		return false;
	}
	if (g->have_anchor && actual_present_ns <= g->anchor_ns) {
		return false;
	}
	g->anchor_ns = actual_present_ns;
	g->have_anchor = true;
	g->samples++;
	return true;
}

/*!
 * Is the grid usable right now? Needs a period, an anchor, and the anchor must
 * not be stale — a feed that stopped (app paused, surface lost) must not leave
 * a confident-looking grid behind.
 */
static inline bool
comp_vblank_grid_trusted(const struct comp_vblank_grid *g, uint64_t now_ns)
{
	if (g == NULL || g->period_ns == 0 || !g->have_anchor) {
		return false;
	}
	if (now_ns < g->anchor_ns) {
		return true; // anchor in the future: a just-observed present
	}
	return (now_ns - g->anchor_ns) < COMP_VBLANK_GRID_STALE_NS;
}

/*!
 * The first vblank strictly after @p now_ns, projected from the anchor.
 *
 * @return 0 when the grid is not trusted — never a guess.
 */
static inline uint64_t
comp_vblank_grid_next_vblank_after(const struct comp_vblank_grid *g, uint64_t now_ns)
{
	if (!comp_vblank_grid_trusted(g, now_ns)) {
		return 0;
	}
	if (now_ns < g->anchor_ns) {
		return g->anchor_ns;
	}
	const uint64_t elapsed = now_ns - g->anchor_ns;
	const uint64_t steps = (elapsed / g->period_ns) + 1;
	return g->anchor_ns + steps * g->period_ns;
}

/*!
 * How long from @p now_ns until the frame woven now reaches photons — the
 * FORWARD horizon a vendor eye predictor wants (the #206 quantity, computed
 * from the same grid rather than from a smoothed retrospective estimate).
 *
 * @return 0 when the grid is not trusted.
 */
static inline uint64_t
comp_vblank_grid_forward_horizon(const struct comp_vblank_grid *g, uint64_t now_ns)
{
	const uint64_t next = comp_vblank_grid_next_vblank_after(g, now_ns);
	if (next == 0 || next <= now_ns) {
		return 0;
	}
	return next - now_ns;
}

/*!
 * Drop the anchor but KEEP the period. Used on a swapchain recreate: the phase
 * is meaningless across a new chain, the panel's period usually is not.
 */
static inline void
comp_vblank_grid_reset_phase(struct comp_vblank_grid *g)
{
	if (g == NULL) {
		return;
	}
	g->anchor_ns = 0;
	g->have_anchor = false;
}

#ifdef __cplusplus
}
#endif
