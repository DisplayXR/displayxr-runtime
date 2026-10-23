// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The trunk's viewer source for look-around eyes (ADR-054 §4.1, N6 Part 2):
 *         the runtime-internal interface the OpenXR layer — and, for IPC
 *         clients, the service's view computation — asks "whose eyes do the
 *         views on screen S follow, and where are they at time T".
 *
 * Implemented by the target (`target_viewer_tracking.c`), which runs the
 * per-screen arbitration — the display processor's own eyes when it tracks,
 * else the best enabled viewer-tracking provider when the display's viewer
 * policy is EXTERNAL_OK, else the nominal viewer — and keeps the provider's
 * samples in an eye-pair ring (`util/u_eye_history.h`) it samples at the
 * caller's target time, mapped into the display space of the screen asked
 * about.
 *
 * Hung off @ref xrt_system_devices::viewer_tracking. NULL = no viewer
 * arbitration in this process (an IPC client: the service owns the views and
 * hosts the provider; a process with no display). The OpenXR layer then uses
 * the answer it was given unchanged.
 *
 * Runtime-internal: no plug-in ever sees this struct, so it is not ABI.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_limits.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Whose eyes the views on a screen follow. Stable names: "dp" / "provider" / "nominal".
enum xrt_viewer_source
{
	XRT_VIEWER_SOURCE_NOMINAL = 0,  //!< the nominal viewer (no tracker, or the display is FIXED)
	XRT_VIEWER_SOURCE_DP = 1,       //!< the display processor's own tracked eyes
	XRT_VIEWER_SOURCE_PROVIDER = 2, //!< a viewer-tracking provider (`xrt_tracking_plugin.h`)
};

/*!
 * The screen id meaning "the primary screen" — the one the system's own
 * (session) display processor weaves: the active plug-in's panel. Any other
 * value is a registry monitor id (`xrt_screen::id`, `xrt_segment_metric::screen_id`).
 */
#define XRT_VIEWER_SCREEN_PRIMARY ((uint64_t)0)

struct xrt_viewer_tracking
{
	/*!
	 * The source the arbitration picked for @p screen_id
	 * (@ref XRT_VIEWER_SCREEN_PRIMARY or a monitor id). The primary's is
	 * static per instance; another screen's follows the display topology
	 * (re-evaluated on hotplug). A screen the arbitration does not know reads
	 * as DP: the caller keeps the display processor's answer.
	 */
	enum xrt_viewer_source (*get_source)(struct xrt_viewer_tracking *vt, uint64_t screen_id);

	/*!
	 * When @p screen_id's source is PROVIDER: the provider's eye pair at
	 * @p target_ns (monotonic) in THAT screen's display space, `count` = 2,
	 * sampled from the ring (interpolated / extrapolated / stale →
	 * `is_tracking` false). Starts the provider on the first call (streams
	 * open on demand). False when there is no sample yet or the source is not
	 * PROVIDER — the caller keeps the DP's answer. Any thread; never blocks on
	 * the provider.
	 */
	bool (*get_eyes_at)(struct xrt_viewer_tracking *vt,
	                    uint64_t screen_id,
	                    int64_t target_ns,
	                    struct xrt_eye_positions *out);
};

static inline enum xrt_viewer_source
xrt_viewer_tracking_get_source(struct xrt_viewer_tracking *vt, uint64_t screen_id)
{
	return vt != NULL && vt->get_source != NULL ? vt->get_source(vt, screen_id) : XRT_VIEWER_SOURCE_DP;
}

static inline bool
xrt_viewer_tracking_get_eyes_at(struct xrt_viewer_tracking *vt,
                                uint64_t screen_id,
                                int64_t target_ns,
                                struct xrt_eye_positions *out)
{
	return vt != NULL && vt->get_eyes_at != NULL && vt->get_eyes_at(vt, screen_id, target_ns, out);
}

/*!
 * Lay a viewer-tracking provider's eye pair @p pe over the display
 * processor's answer @p dp (which may be absent, @p have_dp false — a
 * headless session asks no compositor). The provider supplies a PAIR; the
 * DP's answer supplies the layout of the active mode:
 *  - no DP answer, or a 2-eye answer: the pair as is;
 *  - a 1-eye (2D) answer: the pair's midpoint;
 *  - an N > 2 answer: the DP's layout translated so its centroid sits on the
 *    pair's midpoint (the DP owns multi-view fill; the trunk never invents views).
 * Validity, tracking state and time are the provider's. Shared by the
 * in-process locate (`oxr_session.c`) and the service's view path
 * (`ipc_server_handler.c`), so both splice identically.
 */
static inline void
xrt_viewer_tracking_splice(struct xrt_eye_positions *dp, bool have_dp, const struct xrt_eye_positions *pe)
{
	const struct xrt_eye_position l = pe->eyes[0];
	const struct xrt_eye_position r = pe->eyes[1];
	const struct xrt_eye_position mid = {(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
	if (!have_dp || !dp->valid || dp->count == 0 || dp->count == 2 || dp->count > XRT_MAX_VIEWS) {
		dp->count = 2;
		dp->eyes[0] = l;
		dp->eyes[1] = r;
	} else if (dp->count == 1) {
		dp->eyes[0] = mid;
	} else {
		struct xrt_eye_position c = {0, 0, 0};
		for (uint32_t i = 0; i < dp->count; i++) {
			c.x += dp->eyes[i].x;
			c.y += dp->eyes[i].y;
			c.z += dp->eyes[i].z;
		}
		const float inv = 1.0f / (float)dp->count;
		const struct xrt_eye_position d = {mid.x - c.x * inv, mid.y - c.y * inv, mid.z - c.z * inv};
		for (uint32_t i = 0; i < dp->count; i++) {
			dp->eyes[i].x += d.x;
			dp->eyes[i].y += d.y;
			dp->eyes[i].z += d.z;
		}
	}
	dp->valid = true;
	dp->is_tracking = pe->is_tracking;
	dp->timestamp_ns = pe->timestamp_ns;
}

/*!
 * The one call a view path makes: when the arbitration routed @p screen_id to
 * a provider and the provider has a sample, splice it over @p inout
 * (@ref xrt_viewer_tracking_splice) and return true; otherwise leave
 * @p inout untouched and return false. @p vt may be NULL (no arbitration in
 * this process).
 */
static inline bool
xrt_viewer_tracking_override_eyes(struct xrt_viewer_tracking *vt,
                                  uint64_t screen_id,
                                  int64_t target_ns,
                                  struct xrt_eye_positions *inout,
                                  bool have_dp)
{
	if (inout == NULL || xrt_viewer_tracking_get_source(vt, screen_id) != XRT_VIEWER_SOURCE_PROVIDER) {
		return false;
	}
	struct xrt_eye_positions pe;
	pe.count = 0;
	if (!xrt_viewer_tracking_get_eyes_at(vt, screen_id, target_ns, &pe) || pe.count < 2) {
		return false;
	}
	xrt_viewer_tracking_splice(inout, have_dp, &pe);
	return true;
}

#ifdef __cplusplus
}
#endif
