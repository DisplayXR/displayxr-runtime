// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows implementation of EDID display enumeration.
 * @ingroup aux_os
 *
 * Algorithm (mirrors SR SDK's getMonitorList):
 * 1. EnumDisplayMonitors → HMONITOR handles + screen rects
 * 2. SetupDiGetClassDevs → enumerate all monitor device interfaces
 * 3. For each device: read EDID from registry, parse manufacturer+product ID
 * 4. Correlate SetupDi entries with HMONITOR entries via device path matching
 * 5. EnumDisplaySettings → current refresh rate
 */

#include "os_display_edid.h"

#include <windows.h>
#include <setupapi.h>
#include <string.h>
#include <stdio.h>

// {E6F07B5F-EE97-4A90-B076-335F7BF4EAA7}
static const GUID GUID_DEVINTERFACE_MONITOR_LOCAL = {
    0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0x5f, 0x7b, 0xf4, 0xea, 0xa7}};

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
 * Get device interface path for a GDI monitor.
 * Uses EnumDisplayDevicesA with EDD_GET_DEVICE_INTERFACE_NAME.
 * Writes a prefix (up to the '{' GUID delimiter) into out_prefix.
 */
static bool
get_monitor_device_prefix(const char *device_name, char *out_prefix, size_t prefix_size)
{
	DISPLAY_DEVICEA dd;
	memset(&dd, 0, sizeof(dd));
	dd.cb = sizeof(dd);

	if (!EnumDisplayDevicesA(device_name, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) {
		return false;
	}

	// Extract prefix before '{' (the GUID part)
	const char *brace = strchr(dd.DeviceID, '{');
	size_t len = brace ? (size_t)(brace - dd.DeviceID) : strlen(dd.DeviceID);
	if (len >= prefix_size) {
		len = prefix_size - 1;
	}
	memcpy(out_prefix, dd.DeviceID, len);
	out_prefix[len] = '\0';
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
		return false;
	}

	// Build device path prefixes for each GDI monitor
	char gdi_prefixes[OS_DISPLAY_EDID_MAX_MONITORS][256];
	bool gdi_has_prefix[OS_DISPLAY_EDID_MAX_MONITORS];
	for (uint32_t i = 0; i < gdi_ctx.count; i++) {
		gdi_has_prefix[i] =
		    get_monitor_device_prefix(gdi_ctx.infos[i].szDevice, gdi_prefixes[i], sizeof(gdi_prefixes[i]));
	}

	// Step 2: Enumerate monitor devices via SetupAPI and read EDID
	HDEVINFO dev_info =
	    SetupDiGetClassDevsA(&GUID_DEVINTERFACE_MONITOR_LOCAL, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

	if (dev_info == INVALID_HANDLE_VALUE) {
		return false;
	}

	for (DWORD idx = 0;; idx++) {
		SP_DEVICE_INTERFACE_DATA iface_data;
		memset(&iface_data, 0, sizeof(iface_data));
		iface_data.cbSize = sizeof(iface_data);

		if (!SetupDiEnumDeviceInterfaces(dev_info, NULL, &GUID_DEVINTERFACE_MONITOR_LOCAL, idx, &iface_data)) {
			break; // No more devices
		}

		// Get device interface detail (variable size)
		DWORD required_size = 0;
		SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, NULL, 0, &required_size, NULL);
		if (required_size == 0) {
			continue;
		}

		BYTE detail_buf[512];
		if (required_size > sizeof(detail_buf)) {
			continue;
		}
		SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)detail_buf;
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		SP_DEVINFO_DATA dev_info_data;
		memset(&dev_info_data, 0, sizeof(dev_info_data));
		dev_info_data.cbSize = sizeof(dev_info_data);

		if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, detail, required_size, NULL,
		                                      &dev_info_data)) {
			continue;
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

		// Step 3: Correlate with GDI monitors by matching device path prefix
		// The SetupDi device path looks like: \\?\display#xxx#yyy#{guid}
		// The GDI device ID looks like:       \\?\DISPLAY#xxx#yyy#{guid}
		// Match on the prefix before '{'
		char setup_prefix[256];
		const char *brace = strchr(detail->DevicePath, '{');
		size_t plen = brace ? (size_t)(brace - detail->DevicePath) : strlen(detail->DevicePath);
		if (plen >= sizeof(setup_prefix)) {
			plen = sizeof(setup_prefix) - 1;
		}
		memcpy(setup_prefix, detail->DevicePath, plen);
		setup_prefix[plen] = '\0';

		for (uint32_t g = 0; g < gdi_ctx.count; g++) {
			if (!gdi_has_prefix[g]) {
				continue;
			}
			if (_stricmp(gdi_prefixes[g], setup_prefix) != 0) {
				continue;
			}

			// Match found — populate the monitor entry
			if (out_list->count >= OS_DISPLAY_EDID_MAX_MONITORS) {
				break;
			}

			struct os_display_edid_monitor *mon = &out_list->monitors[out_list->count];
			mon->manufacturer_id = mfr_id;
			mon->product_id = prod_id;
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
			break; // This GDI monitor is matched, move to next SetupDi device
		}
	}

	SetupDiDestroyDeviceInfoList(dev_info);
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
