// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Current display refresh rate for the monitor a window lives on
 *         (Windows-only, header-only, C-compatible).
 *
 * Every compositor hands the display processor a frame period so the vendor
 * eye predictor can size its display term, and the late-weave governor judges
 * saturation against it. Before this helper each backend either hardcoded
 * 60 Hz or (D3D11) walked the mode list and took the *highest supported*
 * rate — both wrong on a high-refresh panel: a 165 Hz display running at
 * 60 Hz reported 165, and a 165 Hz display actually running at 165 reported
 * 60 on every backend but D3D11.
 *
 * `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` reports the mode the monitor
 * is *currently* in, which is the number we want. It is integer-rounded
 * (59.94 Hz reads as 60, 239.76 as 240) — fine for a display term, and the
 * measured weave→scanout feedback loop carries the precision that matters.
 *
 * @ingroup comp_util
 */

#pragma once

#ifdef XRT_OS_WINDOWS

#include <windows.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Refresh rate in Hz of the monitor containing @p hwnd (NULL ⟹ the primary
 * monitor). Returns 0.0f when the mode cannot be read, so callers keep
 * whatever default they already hold rather than dividing by zero.
 */
static inline float
comp_display_refresh_hz_win(void *hwnd)
{
	DEVMODEW dm;
	MONITORINFOEXW mi;
	HMONITOR mon = NULL;

	if (hwnd != NULL) {
		mon = MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST);
	} else {
		POINT origin = {0, 0};
		mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	}

	if (mon != NULL) {
		memset(&mi, 0, sizeof(mi));
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
			memset(&dm, 0, sizeof(dm));
			dm.dmSize = sizeof(dm);
			// dmDisplayFrequency of 0 or 1 means "hardware default" —
			// not a usable rate, fall through to the primary probe.
			if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
			    dm.dmDisplayFrequency > 1) {
				return (float)dm.dmDisplayFrequency;
			}
		}
	}

	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
		return (float)dm.dmDisplayFrequency;
	}

	return 0.0f;
}

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_WINDOWS
