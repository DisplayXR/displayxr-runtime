// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window-system presentation feedback -> vblank grid (#902, desktop Linux).
 * @ingroup comp_util
 *
 * ## Why this exists
 *
 * Late weave's grid tier (comp_vblank_grid.h) needs two measurements: a refresh
 * period and the timestamp of a present that actually reached the panel. The
 * Vulkan target gets both from `VK_GOOGLE_display_timing` where the driver has
 * it. Desktop Linux drivers mostly do not (measured on an Intel iGPU:
 * `has_ext=0 get_past=0 get_refresh=0`), so the grid never anchored and late
 * weave sat dormant on every Linux Vulkan session.
 *
 * The window system already knows both numbers. On native Wayland the
 * compositor reports, per commit, `wp_presentation_feedback.presented` with the
 * presentation timestamp, the refresh period and the hardware vblank counter;
 * on X11 the Present extension sends `PresentCompleteNotify` with `ust`
 * (microseconds) and `msc` (the vblank counter) for every swapchain present.
 * This header is the source-agnostic half: it turns one such sample into grid
 * updates, deciding what may be trusted. It knows nothing about Wayland, XCB or
 * Vulkan, so the decisions are unit-tested on the host
 * (tests/tests_comp_present_feedback.cpp).
 *
 * ## The rules
 *
 * - A sample that was NOT vsync'd never touches the grid. A tearing or
 *   software-timed present is not a vblank, and anchoring on one would put the
 *   whole projected schedule off-phase.
 * - A discarded sample (superseded content, hidden surface, X11 `SKIP`) carries
 *   no timestamp and is only counted.
 * - The timestamp must be in the caller's clock. The first usable sample is
 *   checked against `now`: more than a second either way is a different clock,
 *   and the feed is refused for good rather than projected into a clock nobody
 *   else uses (the same guard the display-timing feeder has).
 * - The period comes from the window system's own refresh value when it gives
 *   one; otherwise (Wayland VRR / unknown refresh sends 0, X11 never sends one)
 *   it is DERIVED from two vsync'd samples and their vblank counters:
 *   `(t2 - t1) / (seq2 - seq1)`. That is exact for any number of skipped
 *   vblanks, which a plain timestamp delta is not.
 * - Every state change is reported once (@ref comp_present_feedback_state),
 *   debounced so that one odd sample among good ones does not produce a log
 *   line per frame.
 */

#pragma once

#include "util/comp_vblank_grid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A first sample further than this from "now" is in a different clock (ns).
#define COMP_PRESENT_FEEDBACK_TIMEBASE_NS (1000ULL * 1000ULL * 1000ULL)

//! Consecutive refusals of one kind before the feed is reported as being in
//! that state. One odd sample among good ones is noise, not a state.
#define COMP_PRESENT_FEEDBACK_DEBOUNCE 8

/*!
 * One presentation-feedback sample, already converted to the caller's
 * monotonic clock in ns.
 */
struct comp_present_feedback_sample
{
	//! When the content turned into light (ns). 0 = none (discarded).
	uint64_t present_ns;
	//! Refresh period the window system reports (ns); 0 = not reported.
	uint64_t refresh_ns;
	//! Hardware vblank counter (Wayland seq, X11 msc).
	uint64_t seq;
	//! @ref seq is meaningful. False when the window system has no counter.
	bool seq_valid;
	//! Presentation was synchronised to the vertical retrace.
	bool vsync;
	//! The timestamp came from the display hardware (diagnostic only).
	bool hw_clock;
	//! The content update never reached the panel.
	bool discarded;
};

//! What the feed is doing, for the one-line-per-state-change log.
enum comp_present_feedback_state
{
	//! No sample yet.
	COMP_PRESENT_FEEDBACK_STATE_WAITING = 0,
	//! Vsync'd samples are anchoring the grid.
	COMP_PRESENT_FEEDBACK_STATE_FEEDING,
	//! Samples arrive, but none is vsync'd: the grid is not fed.
	COMP_PRESENT_FEEDBACK_STATE_NOT_VSYNC,
	//! Every recent content update was discarded.
	COMP_PRESENT_FEEDBACK_STATE_DISCARDING,
	//! The timestamps are in a different clock; the feed is refused for good.
	COMP_PRESENT_FEEDBACK_STATE_TIMEBASE_REFUSED,
};

static inline const char *
comp_present_feedback_state_str(enum comp_present_feedback_state s)
{
	switch (s) {
	case COMP_PRESENT_FEEDBACK_STATE_WAITING: return "waiting";
	case COMP_PRESENT_FEEDBACK_STATE_FEEDING: return "feeding";
	case COMP_PRESENT_FEEDBACK_STATE_NOT_VSYNC: return "not-vsync";
	case COMP_PRESENT_FEEDBACK_STATE_DISCARDING: return "discarding";
	case COMP_PRESENT_FEEDBACK_STATE_TIMEBASE_REFUSED: return "timebase-refused";
	}
	return "?";
}

//! Per-feed bookkeeping. Zero-initialise.
struct comp_present_feedback
{
	enum comp_present_feedback_state state;
	bool timebase_checked;
	bool timebase_ok;
	//! Last ACCEPTED sample, for deriving a period from the vblank counter.
	uint64_t last_present_ns;
	uint64_t last_seq;
	bool have_last_seq;
	uint32_t consecutive_not_vsync;
	uint32_t consecutive_discarded;
	//! Totals — diagnostics only.
	uint64_t accepted;
	uint64_t not_vsync;
	uint64_t discarded;
};

//! What one @ref comp_present_feedback_apply did.
struct comp_present_feedback_result
{
	//! The sample moved the grid's anchor.
	bool anchored;
	//! The grid's period changed (initial value or a mode switch).
	bool period_changed;
	//! Where the period came from when it changed: true = derived from the
	//! vblank counter, false = the window system's refresh value.
	bool period_derived;
	//! The window system reported a period the grid refused as implausible.
	bool period_refused;
	//! @ref comp_present_feedback::state changed; log it.
	bool state_changed;
};

static inline void
comp_present_feedback_set_state(struct comp_present_feedback *f,
                                enum comp_present_feedback_state s,
                                struct comp_present_feedback_result *r)
{
	if (f->state != s) {
		f->state = s;
		r->state_changed = true;
	}
}

/*!
 * Apply one sample to @p grid.
 *
 * @param now_ns the caller's monotonic "now", for the one-time timebase check.
 */
static inline struct comp_present_feedback_result
comp_present_feedback_apply(struct comp_present_feedback *f,
                            struct comp_vblank_grid *grid,
                            const struct comp_present_feedback_sample *s,
                            uint64_t now_ns)
{
	struct comp_present_feedback_result r = {false, false, false, false, false};
	if (f == NULL || grid == NULL || s == NULL) {
		return r;
	}
	if (f->state == COMP_PRESENT_FEEDBACK_STATE_TIMEBASE_REFUSED) {
		return r; // latched: a different clock does not become ours later
	}

	if (s->discarded || s->present_ns == 0) {
		f->discarded++;
		f->consecutive_not_vsync = 0;
		if (++f->consecutive_discarded >= COMP_PRESENT_FEEDBACK_DEBOUNCE) {
			comp_present_feedback_set_state(f, COMP_PRESENT_FEEDBACK_STATE_DISCARDING, &r);
		}
		return r;
	}
	if (!s->vsync) {
		f->not_vsync++;
		f->consecutive_discarded = 0;
		// The sample is not a vblank, and the NEXT one must not derive a
		// period across it either: the counter pairing assumes both ends are
		// real vblanks.
		f->have_last_seq = false;
		if (++f->consecutive_not_vsync >= COMP_PRESENT_FEEDBACK_DEBOUNCE) {
			comp_present_feedback_set_state(f, COMP_PRESENT_FEEDBACK_STATE_NOT_VSYNC, &r);
		}
		return r;
	}

	if (!f->timebase_checked) {
		f->timebase_checked = true;
		const uint64_t d = s->present_ns > now_ns ? s->present_ns - now_ns : now_ns - s->present_ns;
		f->timebase_ok = d < COMP_PRESENT_FEEDBACK_TIMEBASE_NS;
		if (!f->timebase_ok) {
			comp_present_feedback_set_state(f, COMP_PRESENT_FEEDBACK_STATE_TIMEBASE_REFUSED, &r);
			return r;
		}
	}

	f->consecutive_not_vsync = 0;
	f->consecutive_discarded = 0;

	// Period: the window system's own value wins; otherwise derive it from the
	// vblank counter across the last accepted sample.
	uint64_t period = 0;
	bool derived = false;
	if (s->refresh_ns != 0) {
		period = s->refresh_ns;
	} else if (s->seq_valid && f->have_last_seq && s->seq > f->last_seq && s->present_ns > f->last_present_ns) {
		period = (s->present_ns - f->last_present_ns) / (s->seq - f->last_seq);
		derived = true;
	}
	if (period != 0) {
		const uint64_t before = grid->period_ns;
		if (comp_vblank_grid_set_period(grid, period)) {
			if (grid->period_ns != before) {
				r.period_changed = true;
				r.period_derived = derived;
			}
		} else if (!derived) {
			// A derived value can be implausible for a benign reason (a
			// counter reset); a REPORTED one is a bad read worth saying.
			r.period_refused = true;
		}
	}

	r.anchored = comp_vblank_grid_observe(grid, s->present_ns);
	f->accepted++;
	if (s->seq_valid) {
		f->last_seq = s->seq;
		f->have_last_seq = true;
	} else {
		f->have_last_seq = false;
	}
	f->last_present_ns = s->present_ns;
	comp_present_feedback_set_state(f, COMP_PRESENT_FEEDBACK_STATE_FEEDING, &r);
	return r;
}

/*!
 * A new swapchain (or surface): the phase and the counter pairing are gone, the
 * timebase verdict and the totals are not.
 */
static inline void
comp_present_feedback_reset_phase(struct comp_present_feedback *f)
{
	if (f == NULL) {
		return;
	}
	f->have_last_seq = false;
	f->last_present_ns = 0;
}

#ifdef __cplusplus
}
#endif
