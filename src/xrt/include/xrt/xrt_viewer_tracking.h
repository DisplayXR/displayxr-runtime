// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The trunk's viewer source for look-around eyes (ADR-054 §4.1, N6 Part 2):
 *         the runtime-internal interface the OpenXR layer asks "whose eyes do the
 *         views follow on the primary screen, and where are they at time T".
 *
 * Implemented by the target (`target_viewer_tracking.c`), which runs the
 * per-screen arbitration — the display processor's own eyes when it tracks,
 * else the best enabled viewer-tracking provider when the display's viewer
 * policy is EXTERNAL_OK, else the nominal viewer — and keeps the provider's
 * samples in an eye-pair ring (`util/u_eye_history.h`) it samples at the
 * caller's target time.
 *
 * Hung off @ref xrt_system_devices::viewer_tracking. NULL = no viewer
 * arbitration in this process (an IPC client: the service owns the views; a
 * process with no display). The OpenXR layer then uses the DP's answer
 * unchanged, exactly as before N6 Part 2.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h"

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

struct xrt_viewer_tracking
{
	//! The source the arbitration picked for the primary screen (static per instance).
	enum xrt_viewer_source (*get_source)(struct xrt_viewer_tracking *vt);

	/*!
	 * When the source is PROVIDER: the provider's eye pair at @p target_ns
	 * (monotonic), display space, `count` = 2, sampled from the ring
	 * (interpolated / extrapolated / stale → `is_tracking` false). Starts the
	 * provider on the first call (streams open on demand). False when there is
	 * no sample yet or the source is not PROVIDER — the caller keeps the DP's
	 * answer. Any thread; never blocks on the provider.
	 */
	bool (*get_eyes_at)(struct xrt_viewer_tracking *vt, int64_t target_ns, struct xrt_eye_positions *out);
};

static inline enum xrt_viewer_source
xrt_viewer_tracking_get_source(struct xrt_viewer_tracking *vt)
{
	return vt != NULL && vt->get_source != NULL ? vt->get_source(vt) : XRT_VIEWER_SOURCE_DP;
}

static inline bool
xrt_viewer_tracking_get_eyes_at(struct xrt_viewer_tracking *vt, int64_t target_ns, struct xrt_eye_positions *out)
{
	return vt != NULL && vt->get_eyes_at != NULL && vt->get_eyes_at(vt, target_ns, out);
}

#ifdef __cplusplus
}
#endif
