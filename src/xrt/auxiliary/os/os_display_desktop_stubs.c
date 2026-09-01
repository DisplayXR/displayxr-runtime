// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Non-Windows stub for the desktop-rect resolver.
 * @ingroup aux_os
 *
 * Reports failure so the runtime publishes a zeroed rect and an empty device
 * name, which `XR_DXR_display_info` defines as "unknown". Apps fall back to
 * whatever placement they used before.
 *
 * macOS would resolve this through `NSScreen`/`CGDirectDisplayID` and desktop
 * Linux through XRandR; both are the follow-on noted in #1301, and both need
 * the panel origin to be real first (#715 — the Linux and macOS hosted-window
 * paths still hardcode their position).
 */

#include "os_display_desktop.h"

#include <string.h>

bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info)
{
	(void)x;
	(void)y;

	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	return false;
}
