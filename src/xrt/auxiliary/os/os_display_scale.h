// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Is the X11 root window device pixels, and can a window land on any
 *         pixel of it? (placement quantum under XWayland global scaling).
 * @ingroup aux_os
 *
 * Joins two sources the runtime already trusts separately — the RandR monitor
 * rects (@ref os_display_desktop_enumerate) and the mode each DRM/KMS
 * connector is scanning out (sysfs, `/sys/class/drm/card*-<connector>/modes`,
 * readable by any user, no libdrm, no device open) — and hands them to the
 * pure solver in `util/u_x11_scale.h`. Read that file for the model, the three
 * measured configurations, and the solver's one blind spot.
 *
 * Linux desktop only; every other platform gets a stub that reports
 * "unknown", which callers must treat as "keep current behaviour".
 */

#pragma once

#include "util/u_x11_scale.h"
#include "os/os_display_desktop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Everything @ref os_display_scale_query learned, kept whole so a diagnostic
 * (`displayxr-cli info`) can print the per-output evidence and not only the
 * verdict.
 *
 * @ingroup aux_os
 */
struct os_display_scale_report
{
	//! The solver's conclusion. UNKNOWN when nothing could be compared.
	struct u_x11_scale_verdict verdict;

	//! Per RandR monitor: X11 size and (when a DRM connector matched) the
	//! mode it is running. `native_w == 0` ⟹ no DRM match for that output.
	struct u_x11_output_sizes outputs[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	uint32_t output_count;

	//! Bounding box of every RandR monitor rect = the X root the desktop
	//! spans, in X11 pixels. Reported, not judged: tonight's dangerous case
	//! read 7296x2160 against a true 6720x2160 of hardware pixels.
	uint32_t root_w, root_h;

	//! Sum of the native widths/heights of the matched outputs, for a
	//! side-by-side sanity read next to @ref root_w. Informational only —
	//! it assumes nothing about the arrangement and is never used to decide.
	uint32_t native_sum_w;

	//! True when `/sys/class/drm` yielded at least one connected connector.
	bool drm_available;
};

/*!
 * Enumerate the X11 monitors and the DRM connectors behind them, and solve for
 * the X11 global scale / placement quantum.
 *
 * @return true when the report is filled (even if the verdict is UNKNOWN);
 *         false when not even the X11 monitors could be enumerated.
 *
 * @ingroup aux_os
 */
bool
os_display_scale_query(struct os_display_scale_report *out);

/*!
 * One-word name for a verdict state, for logs and `displayxr-cli info`.
 *
 * @ingroup aux_os
 */
const char *
os_display_scale_state_str(enum u_x11_scale_state state);

#ifdef __cplusplus
}
#endif
