// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows implementation of the desktop-rect resolver.
 * @ingroup aux_os
 *
 * Same monitor lookup the scanout-adapter resolver does
 * (@ref d3d_scanout_helpers.cpp), minus the DXGI half: point -> HMONITOR ->
 * rect + GDI device name.
 *
 * ## Why this forces its own DPI context
 *
 * GDI answers `MonitorFromPoint`/`GetMonitorInfoW` in the DPI space of the
 * CALLING process, and this code runs inside the runtime DLL — which for every
 * in-process app class is the APP's process, not a DisplayXR executable. So the
 * `dpi_aware.manifest` that #1201 embeds in our own exes buys nothing here: a
 * DPI-unaware host (a Unity player, say, whose manifest is Unity's) would hand
 * us VIRTUALISED rects, and we would publish them through
 * `XR_DXR_display_info` as if they were physical. On a mixed-DPI rig that error
 * is the ratio of the two monitors' scale factors — the exact failure
 * displayxr-unity#263 measured (a pane whose true origin was x=2560 computed as
 * x=5120 on a 300%-primary + 150%-panel box).
 *
 * `SetProcessDpiAwarenessContext` cannot fix this — awareness is a process
 * property and a DLL must not change its host's. But
 * `SetThreadDpiAwarenessContext` is per-thread and reversible, so we pin
 * PER_MONITOR_AWARE_V2 across the query and restore the caller's context
 * immediately. The published rect is then physical virtual-desktop pixels
 * whatever the host declared, which is the contract
 * `XR_DXR_display_info` states.
 */

#include "os_display_desktop.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * Resolved dynamically rather than compile-time: SetThreadDpiAwarenessContext is
 * Windows 10 1607+, and we still build against SDKs/toolchains (MinGW) whose
 * headers may not declare it. Absence is not fatal — we fall back to the host's
 * DPI context, which is correct whenever the host is already per-monitor aware
 * (every DisplayXR exe) and no worse than not trying otherwise.
 */
typedef HANDLE os_dpi_context_t; // DPI_AWARENESS_CONTEXT
typedef os_dpi_context_t(WINAPI *pfn_set_thread_dpi_awareness_context)(os_dpi_context_t);

#define OS_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((os_dpi_context_t)(intptr_t)-4)

static pfn_set_thread_dpi_awareness_context
get_set_thread_dpi_awareness_context(void)
{
	// user32 is already loaded in every process that can have a window; a
	// module handle needs no free.
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 == NULL) {
		return NULL;
	}
	return (pfn_set_thread_dpi_awareness_context)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
}

bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	pfn_set_thread_dpi_awareness_context set_ctx = get_set_thread_dpi_awareness_context();
	os_dpi_context_t prev_ctx = NULL;
	if (set_ctx != NULL) {
		prev_ctx = set_ctx(OS_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}

	bool ok = false;
	POINT pt = {(LONG)x, (LONG)y};

	// NEAREST, not NULL: a stale origin (hotplug, rearrangement) or a gap in
	// a ragged multi-monitor arrangement still yields a placeable rect.
	HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	if (mon != NULL) {
		MONITORINFOEXW mi;
		memset(&mi, 0, sizeof(mi));
		mi.cbSize = sizeof(mi);

		if (GetMonitorInfoW(mon, (LPMONITORINFO)&mi)) {
			// rcMonitor is the full monitor rect; rcWork excludes the
			// taskbar. Placement onto the panel wants the full rect — the
			// weave covers the whole panel, and a client that wants to
			// dodge the taskbar can ask Windows itself.
			out_info->left = (int32_t)mi.rcMonitor.left;
			out_info->top = (int32_t)mi.rcMonitor.top;
			out_info->width = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			out_info->height = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
			out_info->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

			// szDevice is the GDI name (`\\.\DISPLAY1`), which is what
			// EnumDisplayDevicesW and DXGI's DXGI_OUTPUT_DESC::DeviceName
			// both key on, so a client can re-resolve this monitor after
			// an arrangement change. Truncation leaves the name empty
			// rather than half-written: a partial device name would
			// silently match nothing.
			int written = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, out_info->device_name,
			                                  (int)sizeof(out_info->device_name), NULL, NULL);
			if (written <= 0) {
				out_info->device_name[0] = '\0';
			}

			ok = true;
		}
	}

	if (set_ctx != NULL && prev_ctx != NULL) {
		set_ctx(prev_ctx);
	}

	// Second pass, now back in the CALLER's DPI context, to learn what this
	// same monitor measures in the space a DPI-sensitive API would report to
	// this process. Only a comparison basis — see the field docs. When the
	// host is already per-monitor aware (every DisplayXR exe) this is just the
	// physical size again, and the two agree.
	if (ok) {
		out_info->width_in_caller_dpi = out_info->width;
		out_info->height_in_caller_dpi = out_info->height;

		HMONITOR mon_caller = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		if (mon_caller != NULL) {
			MONITORINFO mi_caller;
			memset(&mi_caller, 0, sizeof(mi_caller));
			mi_caller.cbSize = sizeof(mi_caller);
			if (GetMonitorInfoW(mon_caller, &mi_caller)) {
				out_info->width_in_caller_dpi =
				    (uint32_t)(mi_caller.rcMonitor.right - mi_caller.rcMonitor.left);
				out_info->height_in_caller_dpi =
				    (uint32_t)(mi_caller.rcMonitor.bottom - mi_caller.rcMonitor.top);
			}
		}
	}

	if (!ok) {
		memset(out_info, 0, sizeof(*out_info));
	}

	return ok;
}

/*
 * Enumeration is unimplemented here ON PURPOSE — see the rule list on
 * @ref os_display_desktop_info_for_panel. The Windows plug-in reads the panel's
 * origin out of EDID and reports it, so selection always takes the origin rule
 * and never reaches the size rules; returning 0 keeps this platform on exactly
 * the behaviour it had before those rules existed.
 *
 * An `EnumDisplayMonitors` walk would also have to answer this file's
 * DPI-virtualisation question per monitor before its sizes could be compared
 * with anything a plug-in reports, so it is not a two-line addition — and
 * nothing needs it.
 */
uint32_t
os_display_desktop_enumerate(struct os_display_desktop_info *out_infos, uint32_t max_infos)
{
	(void)out_infos;
	(void)max_infos;

	return 0;
}
