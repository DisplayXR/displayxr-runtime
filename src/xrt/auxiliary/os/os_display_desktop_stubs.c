// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stub desktop-rect resolver for platforms with no desktop.
 * @ingroup aux_os
 *
 * Windows, macOS and desktop Linux each have a real implementation; this covers
 * Android and anything else.
 *
 * Android is not an oversight — it has no virtual desktop and no window the
 * runtime places by coordinate, so there is nothing to report. Reporting
 * failure makes the runtime publish a zeroed rect and an empty device name,
 * which `XR_DXR_display_info` defines as "unknown", and apps keep whatever
 * placement they already used.
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
