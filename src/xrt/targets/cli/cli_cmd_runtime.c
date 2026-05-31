// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `runtime` subcommand — show / set DisplayXR as the active OpenXR
 *         runtime (HKLM\Software\Khronos\OpenXR\1\ActiveRuntime).
 *
 *   runtime status     Print the current ActiveRuntime and whether it is
 *                      DisplayXR.
 *   runtime activate   Point ActiveRuntime at DisplayXR's manifest (requires
 *                      admin). Folds in the retired DisplayXRSwitcher's job —
 *                      e.g. restoring DisplayXR after a SteamVR/SR uninstall
 *                      blanks the Khronos key.
 *
 * Windows-only (ActiveRuntime is a Khronos-on-Windows registry concept); a
 * no-op note on other platforms.
 *
 * @author David Fattal
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <string.h>

#define P(...) printf(__VA_ARGS__)

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const wchar_t *KHRONOS_KEY = L"Software\\Khronos\\OpenXR\\1";
static const wchar_t *DEFAULT_MANIFEST = L"C:\\Program Files\\DisplayXR\\Runtime\\DisplayXR_win64.json";

//! Read the current ActiveRuntime value (UTF-8). Returns true if set.
static bool
get_active_runtime(char *out, int cap)
{
	out[0] = '\0';
	wchar_t wbuf[1024];
	DWORD bytes = sizeof(wbuf);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, KHRONOS_KEY, L"ActiveRuntime", RRF_RT_REG_SZ, NULL, wbuf, &bytes) !=
	    ERROR_SUCCESS) {
		return false;
	}
	WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, cap, NULL, NULL);
	return out[0] != '\0';
}

//! Locate DisplayXR's manifest: prefer the installer-written InstallPath, then
//! the default Program Files location. Returns true if a manifest file exists.
static bool
find_displayxr_manifest(wchar_t *out, int cap_chars)
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Runtime", 0, KEY_READ | KEY_WOW64_64KEY, &key) ==
	    ERROR_SUCCESS) {
		wchar_t install[MAX_PATH];
		DWORD bytes = sizeof(install);
		LSTATUS rc = RegGetValueW(key, NULL, L"InstallPath", RRF_RT_REG_SZ, NULL, install, &bytes);
		RegCloseKey(key);
		if (rc == ERROR_SUCCESS) {
			_snwprintf_s(out, cap_chars, _TRUNCATE, L"%s\\DisplayXR_win64.json", install);
			if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
				return true;
			}
		}
	}
	_snwprintf_s(out, cap_chars, _TRUNCATE, L"%s", DEFAULT_MANIFEST);
	return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static int
cmd_status(void)
{
	char active[1024];
	bool set = get_active_runtime(active, (int)sizeof(active));
	P("Active OpenXR runtime (HKLM\\Software\\Khronos\\OpenXR\\1\\ActiveRuntime):\n");
	P("  %s\n", set ? active : "<unset>");
	bool is_dxr = set && strstr(active, "DisplayXR") != NULL;
	P("  DisplayXR %s the active runtime.\n", is_dxr ? "IS" : "is NOT");
	return is_dxr ? 0 : 1;
}

static int
cmd_activate(void)
{
	wchar_t manifest[MAX_PATH];
	if (!find_displayxr_manifest(manifest, MAX_PATH)) {
		P("FAIL: could not find DisplayXR's manifest (DisplayXR_win64.json) — is the runtime installed?\n");
		return 1;
	}

	HKEY key;
	LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, KHRONOS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
	if (rc != ERROR_SUCCESS) {
		P("FAIL: cannot open the Khronos OpenXR key%s (rc=%ld).\n",
		  rc == ERROR_ACCESS_DENIED ? " — run from an elevated terminal (HKLM needs admin)" : "", rc);
		return 1;
	}
	DWORD bytes = (DWORD)((wcslen(manifest) + 1) * sizeof(wchar_t));
	rc = RegSetValueExW(key, L"ActiveRuntime", 0, REG_SZ, (const BYTE *)manifest, bytes);
	RegCloseKey(key);
	if (rc != ERROR_SUCCESS) {
		P("FAIL: could not write ActiveRuntime%s (rc=%ld).\n",
		  rc == ERROR_ACCESS_DENIED ? " — run from an elevated terminal" : "", rc);
		return 1;
	}

	char active[1024];
	get_active_runtime(active, (int)sizeof(active));
	P("DisplayXR set as the active OpenXR runtime:\n  %s\n", active);
	return 0;
}

int
cli_cmd_runtime(int argc, const char **argv)
{
	if (argc >= 3 && strcmp(argv[2], "activate") == 0) {
		return cmd_activate();
	}
	if (argc >= 3 && strcmp(argv[2], "status") == 0) {
		return cmd_status();
	}
	P("Usage: displayxr-cli runtime <status|activate>\n");
	P("  status    Show the active OpenXR runtime + whether it is DisplayXR.\n");
	P("  activate  Set DisplayXR as the active OpenXR runtime (needs admin).\n");
	return 1;
}

#else /* !XRT_OS_WINDOWS */

int
cli_cmd_runtime(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	P("'runtime' (ActiveRuntime switching) is Windows-only; on this platform set\n");
	P("XR_RUNTIME_JSON or the per-user active-runtime file instead.\n");
	return 1;
}

#endif
