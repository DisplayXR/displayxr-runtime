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
 * Scheduling: releases sit on a FIXED GRID of divisor-strides anchored
 * at the first call — never re-anchored. This is load-bearing: the
 * first partition cut re-anchored on overrun, and on tiers whose
 * present blocks on vsync the app's cycle (its own pacing + the vsync
 * quantization) ran just past the stride, so every frame re-anchored
 * and the schedule SLID — the app measured 14/s against a 20/s
 * schedule (70 ms cycles: ~50 ms of app-side pacing + a 16.7 ms vsync
 * snap) while the non-blocking bridge tier held 20.0 exactly. On a
 * fixed grid a late app is released immediately and the NEXT release
 * is the next grid slot, so the phase holds and the app converges back
 * onto its slots instead of free-running at cycle + stride. Missed
 * slots are skipped in O(1), never bursted.
 */
static inline void
u_app_partition_throttle(struct u_app_partition *p, uint64_t period_ns, bool tier_supported)
{
	const uint32_t d = u_app_partition_divisor();
	if (d < 2 || period_ns == 0) {
		return;
	}
	/*
	 * Tier gate: refuse cleanly, never collapse the display. The
	 * measured supported tier is the VK compositor over the #918
	 * output-device split (the d3d11 bridge): its fill loop ticks at
	 * ~400/s and its schedule is vsync-locked — verified steady 60 with
	 * the eyeball sign-off. The in-process vk_native/d3d12 tiers tick at
	 * 100-175/s (17-19 ms intervals, same build, same box) and cannot
	 * fill the schedule; throttling the app there collapses the panel to
	 * ~35 updates/s, strictly worse than stock. So on an unsupported
	 * tier the throttle refuses (app runs unthrottled; the gate sees
	 * next_release_ns == 0 and stays legacy). DXR_APP_FRAME_DIVISOR_ANY_TIER=1
	 * overrides for the follow-up bring-up work.
	 */
	if (!tier_supported) {
		static int any_tier = -1;
		if (any_tier < 0) {
			const char *e = getenv("DXR_APP_FRAME_DIVISOR_ANY_TIER");
			any_tier = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (any_tier != 1) {
			if (!p->logged) {
				p->logged = 1;
				U_LOG_W("#1257 partition: DXR_APP_FRAME_DIVISOR=%u REFUSED on this "
				        "tier (fill loop cannot sustain the schedule; measured "
				        "supported tier is the d3d11-bridge/hybrid path). App runs "
				        "unthrottled; DXR_APP_FRAME_DIVISOR_ANY_TIER=1 overrides "
				        "for bring-up",
				        d);
			}
			return;
		}
	}
	const uint64_t stride_ns = (uint64_t)d * period_ns;
	uint64_t now_ns = os_monotonic_get_ns();

	if (p->next_release_ns == 0) {
		// First frame passes immediately; the grid anchors here.
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

	// Advance to the next GRID slot strictly after now — phase preserved,
	// missed slots skipped in one step, no burst, no re-anchor.
	if (now_ns >= p->next_release_ns) {
		const uint64_t behind = now_ns - p->next_release_ns;
		p->next_release_ns += (behind / stride_ns + 1) * stride_ns;
	} else {
		p->next_release_ns += stride_ns;
	}
}

#ifdef __cplusplus
}
#endif
