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
#include <stdlib.h>

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

/*!
 * Decode an EDID manufacturer + product pair from a PnP hardware-ID string
 * such as "DEL4147" (3-letter vendor code + 4 hex product digits). Windows
 * derives this hwid directly from the monitor's EDID, so the values it yields
 * are bit-identical to the raw EDID bytes the registry path reads — and to
 * the {manufacturer_id, product_id} entries the vendor match tables hold:
 *   - manufacturer: 3 letters → 5-bit packed big-endian word → byte-swapped
 *     to the little-endian form EDID bytes 8-9 are read as (DEL → 0xAC10).
 *   - product: 4 hex chars → 16-bit value matching EDID bytes 10-11 LE
 *     (4147 → 0x4147).
 * Returns false if the string is not a well-formed PnP id.
 */
static bool
pnp_hwid_to_ids(const char *hwid, uint16_t *out_mfr, uint16_t *out_prod)
{
	if (hwid == NULL || strlen(hwid) < 7) {
		return false;
	}

	// 3 vendor letters → 5-bit packed (A=1) big-endian word.
	uint16_t packed = 0;
	for (int i = 0; i < 3; i++) {
		char c = hwid[i];
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		if (c < 'A' || c > 'Z') {
			return false;
		}
		packed = (uint16_t)((packed << 5) | (uint16_t)(c - 'A' + 1));
	}
	// EDID stores that word big-endian; match the little-endian read the
	// registry path (read_edid_from_regkey) and the tables use.
	uint16_t mfr = (uint16_t)((packed >> 8) | (packed << 8));

	// 4 hex product digits.
	uint16_t prod = 0;
	for (int i = 3; i < 7; i++) {
		char c = hwid[i];
		uint16_t v;
		if (c >= '0' && c <= '9') {
			v = (uint16_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v = (uint16_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v = (uint16_t)(c - 'A' + 10);
		} else {
			return false;
		}
		prod = (uint16_t)((prod << 4) | v);
	}

	*out_mfr = mfr;
	*out_prod = prod;
	return true;
}

/*!
 * Pull the PnP hardware id out of a DisplayConfig monitorDevicePath such as
 * L"\\?\DISPLAY#DEL4147#5&abcd&0&UID256#{guid}" → "DEL4147". (The leading
 * "\\?\" defeats the generic extract_hardware_id delimiter scan, so this
 * keys off the "DISPLAY#" segment instead.) ASCII output.
 */
static bool
extract_pnp_hwid_from_devicepath(const wchar_t *device_path, char *out_hwid, size_t hwid_size)
{
	if (device_path == NULL) {
		return false;
	}
	const wchar_t *tag = L"DISPLAY#";
	const wchar_t *p = wcsstr(device_path, tag);
	if (p == NULL) {
		return false;
	}
	p += wcslen(tag);

	size_t n = 0;
	while (p[n] != L'\0' && p[n] != L'#' && n + 1 < hwid_size) {
		out_hwid[n] = (char)p[n]; // PnP ids are ASCII
		n++;
	}
	out_hwid[n] = '\0';
	return n > 0;
}

/*!
 * Durable EDID fallback via the DisplayConfig (CCD) API.
 *
 * The SetupAPI path reads the cached EDID *blob* from each monitor's
 * registry "Device Parameters\\EDID" value. That value is frequently absent
 * — behind DisplayPort-MST hubs, docking stations, KVMs, or with generic /
 * indirect monitor drivers that never cache it — even though the panel is
 * present and GDI sees it (issue: CZ Dell SR Pro 2 reported gdi=1 but
 * setupdi=0/edid_reads=0). QueryDisplayConfig + DISPLAYCONFIG_TARGET_DEVICE_NAME
 * report each active target's EDID manufacturer/product directly from the
 * OS display topology without touching the registry blob, so they resolve
 * panels the registry path misses.
 *
 * Appends any monitor not already in @p out_list, correlating screen
 * geometry / refresh / HMONITOR back to the GDI enumeration by source GDI
 * device name. Best-effort: any failure simply contributes no entries.
 */
static void
enumerate_via_displayconfig(struct os_display_edid_list *out_list, const struct monitor_enum_context *gdi_ctx)
{
	UINT32 num_paths = 0;
	UINT32 num_modes = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS) {
		return;
	}

	DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(num_paths, sizeof(*paths));
	DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(num_modes, sizeof(*modes));
	if (paths == NULL || modes == NULL) {
		free(paths);
		free(modes);
		return;
	}

	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, NULL) != ERROR_SUCCESS) {
		free(paths);
		free(modes);
		return;
	}

	for (UINT32 p = 0; p < num_paths && out_list->count < OS_DISPLAY_EDID_MAX_MONITORS; p++) {
		// Target name → EDID ids + monitor device path.
		DISPLAYCONFIG_TARGET_DEVICE_NAME tname;
		memset(&tname, 0, sizeof(tname));
		tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tname.header.size = sizeof(tname);
		tname.header.adapterId = paths[p].targetInfo.adapterId;
		tname.header.id = paths[p].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tname.header) != ERROR_SUCCESS) {
			continue;
		}

		// Prefer the device-path hwid (proven byte-order match to the
		// tables); fall back to the numeric EDID ids when present.
		uint16_t mfr = 0;
		uint16_t prod = 0;
		bool got = false;
		char hwid[64];
		if (extract_pnp_hwid_from_devicepath(tname.monitorDevicePath, hwid, sizeof(hwid))) {
			got = pnp_hwid_to_ids(hwid, &mfr, &prod);
		}
		if (!got && tname.flags.edidIdsValid && tname.edidManufactureId != 0) {
			mfr = tname.edidManufactureId;
			prod = tname.edidProductCodeId;
			got = true;
		}
		if (!got) {
			continue;
		}

		struct os_display_edid_monitor *mon = &out_list->monitors[out_list->count];
		memset(mon, 0, sizeof(*mon));
		mon->manufacturer_id = mfr;
		mon->product_id = prod;

		// Source name → GDI device name → screen geometry / refresh / HMONITOR.
		DISPLAYCONFIG_SOURCE_DEVICE_NAME sname;
		memset(&sname, 0, sizeof(sname));
		sname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sname.header.size = sizeof(sname);
		sname.header.adapterId = paths[p].sourceInfo.adapterId;
		sname.header.id = paths[p].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&sname.header) == ERROR_SUCCESS) {
			char gdi_name[32];
			WideCharToMultiByte(CP_ACP, 0, sname.viewGdiDeviceName, -1, gdi_name, sizeof(gdi_name), NULL,
			                    NULL);
			for (uint32_t g = 0; g < gdi_ctx->count; g++) {
				if (_stricmp(gdi_name, gdi_ctx->infos[g].szDevice) != 0) {
					continue;
				}
				mon->screen_left = gdi_ctx->rects[g].left;
				mon->screen_top = gdi_ctx->rects[g].top;
				mon->pixel_width = (uint32_t)(gdi_ctx->rects[g].right - gdi_ctx->rects[g].left);
				mon->pixel_height = (uint32_t)(gdi_ctx->rects[g].bottom - gdi_ctx->rects[g].top);
				mon->is_primary = (gdi_ctx->infos[g].dwFlags & MONITORINFOF_PRIMARY) != 0;
				mon->hmonitor = (void *)gdi_ctx->handles[g];

				DEVMODEA dm;
				memset(&dm, 0, sizeof(dm));
				dm.dmSize = sizeof(dm);
				if (EnumDisplaySettingsA(gdi_ctx->infos[g].szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
					mon->refresh_hz = dm.dmDisplayFrequency;
				}
				break;
			}
		}

		out_list->count++;
		out_list->diag_displayconfig_count++;
	}

	free(paths);
	free(modes);
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
		// SetupAPI itself is unavailable — still try the DisplayConfig path.
		enumerate_via_displayconfig(out_list, &gdi_ctx);
		return out_list->count > 0;
	}

	// Collect all SetupDi devices with their EDID and instance IDs
	struct
	{
		uint16_t mfr_id;
		uint16_t prod_id;
		char hwid[64]; // Extracted hardware ID (e.g., "AUO2E9A")
	} setupdi_devices[OS_DISPLAY_EDID_MAX_MONITORS];
	uint32_t setupdi_count = 0;     // devices that yielded a readable EDID blob
	uint32_t setupdi_enumerated = 0; // monitor-class devices enumerated at all

	for (DWORD idx = 0; idx < 64; idx++) {
		SP_DEVINFO_DATA dev_info_data;
		memset(&dev_info_data, 0, sizeof(dev_info_data));
		dev_info_data.cbSize = sizeof(dev_info_data);

		if (!SetupDiEnumDeviceInfo(dev_info, idx, &dev_info_data)) {
			break;
		}
		setupdi_enumerated++;

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

	// Split counters so a field report distinguishes "no monitor devices"
	// (setupdi_devices=0) from "devices present but no cached EDID blob"
	// (setupdi_devices=N, edid_reads=0) — both previously collapsed to the
	// same diag=3.
	out_list->diag_setupdi_count = setupdi_enumerated;
	out_list->diag_edid_read_count = setupdi_count;

	// Step 3: Correlate SetupDi devices with GDI monitors by hardware ID.
	// (No-op when setupdi_count == 0; the DisplayConfig fallback below picks
	// up that case.)
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

	// Durable fallback: the registry EDID blob was missing or uncorrelated
	// on this box. Resolve panels straight from the OS display topology.
	if (out_list->count == 0) {
		enumerate_via_displayconfig(out_list, &gdi_ctx);
	}

	if (out_list->count == 0) {
		// Report the primary failure that drove us to (a failed) fallback.
		out_list->diag_error =
		    (setupdi_count == 0) ? OS_EDID_DIAG_NO_EDID_DATA : OS_EDID_DIAG_NO_CORRELATION;
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
