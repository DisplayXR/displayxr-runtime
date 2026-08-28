// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vblank slot partition: app every Dth vblank, repaints the rest (#1257 follow-up).
 *
 * The goal is a SLOW app with a PANEL-RATE weave: the app presents at
 * panel_rate / D while the runtime re-weaves the last atlas with a fresh
 * eye pose on every other vblank. Five gap-filling repaint schedules
 * failed at this on hardware because they let the app self-pace and then
 * raced its commits (every collision vsync-snaps into a whole-vblank
 * slip). The partition inverts the ownership: the RUNTIME paces the app
 * — xrWaitFrame, the spec's throttle point, blocks until the app's next
 * slot — so commits are phase-locked to a schedule the repaint loop
 * knows, and the fire/commit collision never exists by construction.
 * This is exactly the property that made the DXR_WEAVE_REPAINT_FORCE
 * probe succeed where every gap-filling schedule lost (measured
 * independently: render-30/weave-60 on Arc at -9.5 GPU pts, "really
 * crisp"; Unity iGPU at -14.5 GPU pts with the display rate untouched)
 * — minus FORCE's fighting-the-app cost, because here the app is
 * released deliberately instead of being crowded out.
 *
 * Env: DXR_APP_FRAME_DIVISOR = D (2..8). Unset/1 = off. Explicit
 * opt-in; the repaint gate sees the same divisor and switches to a
 * TRUSTED N = D fill schedule (no cadence estimation, no engagement
 * flapping — the hz30 visual verdict was that the eye grades cadence
 * STABILITY, not average rate, so the fill must be steady).
 *
 * Deliberately NOT wired into the Metal compositor (it has no repaint
 * loop — throttling the app there would slow it with nothing filling
 * the panel) nor the IPC/service path yet (in-process first, where the
 * measurements are).
 *
 * @ingroup aux_util
 */

#pragma once

#include "os/os_time.h"
#include "util/u_logging.h"

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Cached probe of DXR_APP_FRAME_DIVISOR. 0/1 = partition off; else 2..8.
static inline uint32_t
u_app_partition_divisor(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("DXR_APP_FRAME_DIVISOR");
		int v = (e != NULL) ? atoi(e) : 0;
		if (v < 2) {
			v = 0; // off
		}
		if (v > 8) {
			v = 8;
		}
		cached = v;
	}
	return (uint32_t)cached;
}

//! Per-compositor throttle state. Zero-init is a valid initial state.
struct u_app_partition
{
	uint64_t next_release_ns; //!< Monotonic time of the app's next slot.
	int logged;               //!< One-shot activation log.
};

/*!
 * Block the calling xrWaitFrame until the app's next partition slot.
 * No-op when the divisor is off. Call BEFORE taking any compositor lock
 * — this sleeps up to D-1 panel periods, and the repaint loop must keep
 * weaving underneath it.
 *
 * Scheduling: releases are one divisor-stride apart, anchored to the
 * first call. An app that runs slower than its slot (heavy frame,
 * debugger, hitch) does not accumulate a backlog — missed slots are
 * skipped and the schedule re-anchors, so the app is never released
 * into a burst.
 */
static inline void
u_app_partition_throttle(struct u_app_partition *p, uint64_t period_ns)
{
	const uint32_t d = u_app_partition_divisor();
	if (d < 2 || period_ns == 0) {
		return;
	}
	const uint64_t stride_ns = (uint64_t)d * period_ns;
	uint64_t now_ns = os_monotonic_get_ns();

	if (p->next_release_ns == 0) {
		// First frame passes immediately; the schedule anchors here.
		p->next_release_ns = now_ns + stride_ns;
		if (!p->logged) {
			p->logged = 1;
			U_LOG_W("#1257 partition: xrWaitFrame throttles the app to every %uth vblank "
			        "(panel/%u); repaints fill the other %u slot(s) at panel rate "
			        "(DXR_APP_FRAME_DIVISOR)",
			        d, d, d - 1);
		}
		return;
	}

	if (p->next_release_ns > now_ns) {
		// timeBeginPeriod(1) is held for the instance's lifetime (#589),
		// so this lands within ~1-2 ms — well inside the fill schedule's
		// half-period margins.
		os_nanosleep((int64_t)(p->next_release_ns - now_ns));
		now_ns = os_monotonic_get_ns();
	}

	p->next_release_ns += stride_ns;
	if (p->next_release_ns <= now_ns) {
		// The app overran its slot: skip the missed release(s), never burst.
		p->next_release_ns = now_ns + stride_ns;
	}
}

#ifdef __cplusplus
}
#endif
