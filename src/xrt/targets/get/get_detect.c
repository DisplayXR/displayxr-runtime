// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get detection inputs: monitor EDID ids (SetupAPI) and HKLM
 *         registry reads. The RULES live in the signed catalog (P2, #2705);
 *         this file only gathers evidence.
 */

#include "get.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <winsvc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
dxr_detect_edids(struct dxr_edid_id *out, int max)
{
	int n = 0;
	HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, NULL, NULL, DIGCF_PRESENT);
	if (di == INVALID_HANDLE_VALUE) {
		return 0;
	}
	SP_DEVINFO_DATA dd;
	dd.cbSize = sizeof(dd);
	for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(di, i, &dd); i++) {
		HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
		if (k == INVALID_HANDLE_VALUE) {
			continue;
		}
		// 8K panels carry DisplayID extension blocks: 128 bytes per block, up to 255 blocks.
		BYTE edid[32768];
		DWORD sz = sizeof(edid), type = 0;
		if (RegQueryValueExW(k, L"EDID", NULL, &type, edid, &sz) == ERROR_SUCCESS && sz >= 12) {
			// Same reading as the Leia plug-in's table (leia_edid_table.h): both
			// words little-endian, so DEL = 0x10,0xAC -> 0xAC10 = 44048.
			struct dxr_edid_id id;
			id.man = (uint16_t)(edid[8] | (edid[9] << 8));
			id.prod = (uint16_t)(edid[10] | (edid[11] << 8));
			bool dup = false;
			for (int j = 0; j < n; j++) {
				dup = dup || (out[j].man == id.man && out[j].prod == id.prod);
			}
			if (!dup) {
				out[n++] = id;
			}
		}
		RegCloseKey(k);
	}
	SetupDiDestroyDeviceInfoList(di);
	return n;
}

static const char *
strip_hklm(const char *s)
{
	static const char *const prefixes[] = {"HKLM\\", "HKEY_LOCAL_MACHINE\\"};
	for (int i = 0; i < 2; i++) {
		size_t l = strlen(prefixes[i]);
		if (_strnicmp(s, prefixes[i], l) == 0) {
			return s + l;
		}
	}
	return NULL;
}

bool
dxr_reg_read_dword(const char *subkey, const char *value, uint32_t *out)
{
	const char *sk = strip_hklm(subkey);
	if (sk == NULL) {
		return false;
	}
	BYTE buf[64];
	DWORD sz = sizeof(buf) - 1, type = 0;
	if (RegGetValueA(HKEY_LOCAL_MACHINE, sk, value, RRF_RT_REG_DWORD | RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type,
	                 buf, &sz) != ERROR_SUCCESS) {
		return false;
	}
	if (type == REG_DWORD) {
		memcpy(out, buf, 4);
		return true;
	}
	buf[sizeof(buf) - 1] = 0;
	char *end = NULL;
	unsigned long v = strtoul((const char *)buf, &end, 10);
	if (end == (char *)buf) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static bool
read_path_sz(const char *full, char *out, size_t out_size, DWORD view)
{
	const char *sk = strip_hklm(full);
	if (sk == NULL) {
		return false;
	}
	char key[512];
	(void)snprintf(key, sizeof(key), "%s", sk);
	char *slash = strrchr(key, '\\');
	if (slash == NULL) {
		return false;
	}
	*slash = '\0';
	DWORD sz = (DWORD)out_size;
	return RegGetValueA(HKEY_LOCAL_MACHINE, key, slash + 1, RRF_RT_REG_SZ | view, NULL, out, &sz) == ERROR_SUCCESS;
}

bool
dxr_reg_read_path_sz(const char *full, char *out, size_t out_size)
{
	return read_path_sz(full, out, out_size, RRF_SUBKEY_WOW6464KEY);
}

bool
dxr_reg_read_path_sz_32(const char *full, char *out, size_t out_size)
{
	return read_path_sz(full, out, out_size, RRF_SUBKEY_WOW6432KEY);
}

bool
dxr_reg_key_exists(const char *key)
{
	const char *sk = strip_hklm(key);
	HKEY k;
	if (sk == NULL ||
	    RegOpenKeyExA(HKEY_LOCAL_MACHINE, sk, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) {
		return false;
	}
	RegCloseKey(k);
	return true;
}

bool
dxr_file_exists(const char *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool
dxr_service_exists(const char *name)
{
	// Installed is the question, not running: a vendor service that is stopped
	// is the vendor's (or the user's) business, not a missing prerequisite.
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm == NULL) {
		return false;
	}
	SC_HANDLE svc = OpenServiceA(scm, name, SERVICE_QUERY_STATUS);
	const bool found = svc != NULL;
	if (svc != NULL) {
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return found;
}
