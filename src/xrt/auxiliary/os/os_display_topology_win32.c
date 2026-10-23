// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows topology read — see os_display_topology.h.
 * @ingroup aux_os
 *
 * `EnumDisplayDevicesW` + `EnumDisplaySettingsExW` only: no SetupAPI, no
 * registry, no EDID blob, no window and no message loop — callable from the
 * runtime's own worker thread inside any host process. The thread's DPI
 * awareness is pinned to per-monitor-v2 across the read and restored after
 * (docs/reference/dpi-awareness.md), exactly as os_display_desktop_win32.c
 * does, so a DPI-unaware host (a game engine's player) reads the same numbers
 * as a DisplayXR executable.
 *
 * A monitor driven by an indirect display driver (a casting tool's virtual
 * "extend" monitor) shows up here like any other output — the signature is
 * a TRIGGER only. Whether a monitor can be the panel is decided by the claim
 * registry the sweep rebuilds, never by this list.
 */

#include "os_display_topology.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef EDD_GET_DEVICE_INTERFACE_NAME
#define EDD_GET_DEVICE_INTERFACE_NAME 0x00000001
#endif

typedef HANDLE os_topo_dpi_context_t; // DPI_AWARENESS_CONTEXT
typedef os_topo_dpi_context_t(WINAPI *pfn_set_thread_dpi_ctx)(os_topo_dpi_context_t);
#define OS_TOPO_DPI_PER_MONITOR_AWARE_V2 ((os_topo_dpi_context_t)(intptr_t)-4)

static pfn_set_thread_dpi_ctx
get_set_thread_dpi_ctx(void)
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 == NULL) {
		return NULL;
	}
	return (pfn_set_thread_dpi_ctx)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
}

static void
wide_to_utf8(const WCHAR *w, char *out, size_t size)
{
	int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL);
	if (written <= 0) {
		out[0] = '\0';
	}
}

uint32_t
os_display_topology_read(struct os_display_topology_entry *out, uint32_t max)
{
	if (out == NULL || max == 0) {
		return 0;
	}
	pfn_set_thread_dpi_ctx set_ctx = get_set_thread_dpi_ctx();
	os_topo_dpi_context_t prev = NULL;
	if (set_ctx != NULL) {
		prev = set_ctx(OS_TOPO_DPI_PER_MONITOR_AWARE_V2);
	}

	uint32_t n = 0;
	for (DWORD a = 0; n < max && a < 64; a++) {
		DISPLAY_DEVICEW adapter;
		memset(&adapter, 0, sizeof(adapter));
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(NULL, a, &adapter, 0)) {
			break;
		}
		// Outputs that drive nothing are noise; an output that GAINS a
		// desktop attachment changes its StateFlags and so the hash anyway.
		if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
			continue;
		}
		struct os_display_topology_entry *e = &out[n];
		memset(e, 0, sizeof(*e));

		char adapter_name[64];
		wide_to_utf8(adapter.DeviceName, adapter_name, sizeof(adapter_name));

		// The monitor(s) on this output: the device-interface name carries
		// the EDID hardware id, so another panel on the same output moves it.
		char monitors[128];
		monitors[0] = '\0';
		size_t used = 0;
		uint32_t mon_state = 0;
		for (DWORD m = 0; m < 4; m++) {
			DISPLAY_DEVICEW mon;
			memset(&mon, 0, sizeof(mon));
			mon.cb = sizeof(mon);
			if (!EnumDisplayDevicesW(adapter.DeviceName, m, &mon, EDD_GET_DEVICE_INTERFACE_NAME)) {
				break;
			}
			char id[128];
			wide_to_utf8(mon.DeviceID, id, sizeof(id));
			int w = snprintf(monitors + used, sizeof(monitors) - used, "%s%s", used > 0 ? "+" : "", id);
			if (w > 0) {
				used += (size_t)w < sizeof(monitors) - used ? (size_t)w : sizeof(monitors) - used - 1;
			}
			mon_state |= (uint32_t)(mon.StateFlags & 0xffffu);
		}
		(void)snprintf(e->key, sizeof(e->key), "%s|%s", adapter_name, monitors);
		e->state = (uint32_t)(adapter.StateFlags & 0xffffu) | (mon_state << 16);

		DEVMODEW dm;
		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
			e->left = (int32_t)dm.dmPosition.x;
			e->top = (int32_t)dm.dmPosition.y;
			e->width = (uint32_t)dm.dmPelsWidth;
			e->height = (uint32_t)dm.dmPelsHeight;
		}
		n++;
	}

	if (set_ctx != NULL && prev != NULL) {
		set_ctx(prev);
	}
	return n;
}
