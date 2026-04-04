// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows implementation of EDID display enumeration.
 * @ingroup aux_os
 *
 * Algorithm:
 * 1. EnumDisplayMonitors → HMONITOR handles + screen rects
 * 2. For each GDI monitor: EnumDisplayDevices → hardware ID string
 * 3. SetupDiGetClassDevs(GUID_DEVCLASS_MONITOR) → enumerate monitor devices
 * 4. For each device: read EDID from registry, extract manufacturer+product ID
 * 5. Correlate by matching hardware ID substrings
 * 6. EnumDisplaySettings → current refresh rate
 */

#include "os_display_edid.h"

#include <windows.h>
#include <setupapi.h>
#include <string.h>
#include <stdio.h>

// Monitor device class GUID: {4D36E96E-E325-11CE-BFC1-08002BE10318}
static const GUID GUID_DEVCLASS_MONITOR_LOCAL = {
    0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

struct monitor_enum_context
{
	HMONITOR handles[OS_DISPLAY_EDID_MAX_MONITORS];
	RECT rects[OS_DISPLAY_EDID_MAX_MONITORS];
	MONITORINFOEXA infos[OS_DISPLAY_EDID_MAX_MONITORS];
	uint32_t count;
};

static BOOL CALLBACK
monitor_enum_proc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	(void)hdcMonitor;
	struct monitor_enum_context *ctx = (struct monitor_enum_context *)dwData;
	if (ctx->count >= OS_DISPLAY_EDID_MAX_MONITORS) {
		return TRUE;
	}

	MONITORINFOEXA info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	if (GetMonitorInfoA(hMonitor, (LPMONITORINFO)&info)) {
		uint32_t i = ctx->count;
		ctx->handles[i] = hMonitor;
		ctx->rects[i] = *lprcMonitor;
		ctx->infos[i] = info;
		ctx->count++;
	}
	return TRUE;
}

/*!
 * Read EDID from a device registry key.
 * Returns true if EDID was found and manufacturer/product IDs extracted.
 */
static bool
read_edid_from_regkey(HKEY hKey, uint16_t *out_manufacturer_id, uint16_t *out_product_id)
{
	BYTE edid_buf[256];
	DWORD edid_size = sizeof(edid_buf);
	DWORD type = 0;

	LONG ret = RegQueryValueExA(hKey, "EDID", NULL, &type, edid_buf, &edid_size);
	if (ret != ERROR_SUCCESS || edid_size < 16) {
		return false;
	}

	// EDID bytes 8-9: manufacturer ID, bytes 10-11: product ID
	*out_manufacturer_id = *(uint16_t *)&edid_buf[8];
	*out_product_id = *(uint16_t *)&edid_buf[10];
	return true;
}

/*!
 * Extract the hardware ID from a GDI device ID or SetupDi instance ID.
 *
 * GDI DeviceID: "MONITOR\AUO2E9A\{guid}\0001" → extracts "AUO2E9A"
 * SetupDi instance: "DISPLAY\AUO2E9A\5&1234..." → extracts "AUO2E9A"
 *
 * The hardware ID is the second segment between backslashes (or '#' in some formats).
 */
static bool
extract_hardware_id(const char *device_path, char *out_hwid, size_t hwid_size)
{
	// Try both '\' and '#' as delimiters (GDI uses '\', device paths use '#')
	const char *start = NULL;
	const char *end = NULL;

	// Look for MONITOR\xxx\ or DISPLAY#xxx# pattern
	for (const char *p = device_path; *p; p++) {
		if (*p == '\\' || *p == '#') {
			if (start == NULL) {
				start = p + 1; // After first delimiter
			} else if (end == NULL) {
				end = p; // At second delimiter
				break;
			}
		}
	}

	if (start == NULL || end == NULL || end <= start) {
		return false;
	}

	size_t len = (size_t)(end - start);
	if (len >= hwid_size) {
		len = hwid_size - 1;
	}
	memcpy(out_hwid, start, len);
	out_hwid[len] = '\0';
	return len > 0;
}

bool
os_display_edid_enumerate(struct os_display_edid_list *out_list)
{
	if (out_list == NULL) {
		return false;
	}
	memset(out_list, 0, sizeof(*out_list));

	// Step 1: Enumerate all monitors via GDI
	struct monitor_enum_context gdi_ctx;
	memset(&gdi_ctx, 0, sizeof(gdi_ctx));
	EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, (LPARAM)&gdi_ctx);

	if (gdi_ctx.count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_GDI_MONITORS;
		return false;
	}

	out_list->diag_gdi_count = gdi_ctx.count;

	// For each GDI monitor, get its hardware ID via EnumDisplayDevices
	char gdi_hwids[OS_DISPLAY_EDID_MAX_MONITORS][64];
	bool gdi_has_hwid[OS_DISPLAY_EDID_MAX_MONITORS];

	for (uint32_t g = 0; g < gdi_ctx.count; g++) {
		gdi_has_hwid[g] = false;
		gdi_hwids[g][0] = '\0';

		// Enumerate child devices of this adapter output
		DISPLAY_DEVICEA dd;
		memset(&dd, 0, sizeof(dd));
		dd.cb = sizeof(dd);

		// First call: get the adapter device (index 0 without flags)
		if (EnumDisplayDevicesA(gdi_ctx.infos[g].szDevice, 0, &dd, 0)) {
			// Store full DeviceID for diagnostics
			strncpy(out_list->diag_gdi_device_ids[g], dd.DeviceID,
			        sizeof(out_list->diag_gdi_device_ids[g]) - 1);

			// Extract hardware ID (e.g., "AUO2E9A" from "MONITOR\AUO2E9A\...")
			gdi_has_hwid[g] = extract_hardware_id(dd.DeviceID, gdi_hwids[g], sizeof(gdi_hwids[g]));
		}
	}

	// Step 2: Enumerate monitor devices via SetupAPI (device class, not interface)
	HDEVINFO dev_info = SetupDiGetClassDevsA(&GUID_DEVCLASS_MONITOR_LOCAL, NULL, NULL, DIGCF_PRESENT);

	if (dev_info == INVALID_HANDLE_VALUE) {
		out_list->diag_error = OS_EDID_DIAG_SETUPDI_FAILED;
		out_list->diag_win32_error = GetLastError();
		return false;
	}

	// Collect all SetupDi devices with their EDID and instance IDs
	struct
	{
		uint16_t mfr_id;
		uint16_t prod_id;
		char hwid[64]; // Extracted hardware ID (e.g., "AUO2E9A")
	} setupdi_devices[OS_DISPLAY_EDID_MAX_MONITORS];
	uint32_t setupdi_count = 0;

	for (DWORD idx = 0; idx < 64; idx++) {
		SP_DEVINFO_DATA dev_info_data;
		memset(&dev_info_data, 0, sizeof(dev_info_data));
		dev_info_data.cbSize = sizeof(dev_info_data);

		if (!SetupDiEnumDeviceInfo(dev_info, idx, &dev_info_data)) {
			break;
		}

		// Read EDID from device registry
		HKEY hKey = SetupDiOpenDevRegKey(dev_info, &dev_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
		if (hKey == INVALID_HANDLE_VALUE) {
			continue;
		}

		uint16_t mfr_id = 0, prod_id = 0;
		bool got_edid = read_edid_from_regkey(hKey, &mfr_id, &prod_id);
		RegCloseKey(hKey);

		if (!got_edid) {
			continue;
		}

		// Get the device instance ID for correlation
		char instance_id[256];
		if (!SetupDiGetDeviceInstanceIdA(dev_info, &dev_info_data, instance_id, sizeof(instance_id), NULL)) {
			continue;
		}

		if (setupdi_count < OS_DISPLAY_EDID_MAX_MONITORS) {
			setupdi_devices[setupdi_count].mfr_id = mfr_id;
			setupdi_devices[setupdi_count].prod_id = prod_id;
			extract_hardware_id(instance_id, setupdi_devices[setupdi_count].hwid,
			                    sizeof(setupdi_devices[setupdi_count].hwid));
			setupdi_count++;
		}
	}

	SetupDiDestroyDeviceInfoList(dev_info);

	out_list->diag_setupdi_count = setupdi_count;
	out_list->diag_edid_read_count = setupdi_count;

	if (setupdi_count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_EDID_DATA;
		return false;
	}

	// Step 3: Correlate SetupDi devices with GDI monitors by hardware ID
	for (uint32_t g = 0; g < gdi_ctx.count; g++) {
		if (!gdi_has_hwid[g]) {
			continue;
		}

		for (uint32_t s = 0; s < setupdi_count; s++) {
			if (_stricmp(gdi_hwids[g], setupdi_devices[s].hwid) != 0) {
				continue;
			}

			// Match found — populate the monitor entry
			if (out_list->count >= OS_DISPLAY_EDID_MAX_MONITORS) {
				break;
			}

			struct os_display_edid_monitor *mon = &out_list->monitors[out_list->count];
			mon->manufacturer_id = setupdi_devices[s].mfr_id;
			mon->product_id = setupdi_devices[s].prod_id;
			mon->screen_left = gdi_ctx.rects[g].left;
			mon->screen_top = gdi_ctx.rects[g].top;
			mon->pixel_width = (uint32_t)(gdi_ctx.rects[g].right - gdi_ctx.rects[g].left);
			mon->pixel_height = (uint32_t)(gdi_ctx.rects[g].bottom - gdi_ctx.rects[g].top);
			mon->is_primary = (gdi_ctx.infos[g].dwFlags & MONITORINFOF_PRIMARY) != 0;
			mon->hmonitor = (void *)gdi_ctx.handles[g];

			// Get refresh rate from current display settings
			DEVMODEA dm;
			memset(&dm, 0, sizeof(dm));
			dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsA(gdi_ctx.infos[g].szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
				mon->refresh_hz = dm.dmDisplayFrequency;
			}

			out_list->count++;
			break;
		}
	}

	if (out_list->count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_CORRELATION;
	}

	return out_list->count > 0;
}

const struct os_display_edid_monitor *
os_display_edid_find_in_table(const struct os_display_edid_list *list,
                              const uint16_t table[][2],
                              uint32_t table_len)
{
	if (list == NULL || table == NULL || table_len == 0) {
		return NULL;
	}

	for (uint32_t m = 0; m < list->count; m++) {
		for (uint32_t t = 0; t < table_len; t++) {
			if (list->monitors[m].manufacturer_id == table[t][0] &&
			    list->monitors[m].product_id == table[t][1]) {
				return &list->monitors[m];
			}
		}
	}
	return NULL;
}
