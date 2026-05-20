// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief DllMain + delay-load hook for DisplayXRClient.dll on Windows.
 *
 * Resolves /DELAYLOAD'd dependencies (SimulatedRealityVulkanBeta.dll) from
 * the runtime's own install directory without requiring $INSTDIR to be on
 * the system PATH. We're loaded into the host app's process and must not
 * mutate process-wide DLL search behavior (no SetDefaultDllDirectories) —
 * the delay-load notification hook is the surgical option.
 */

#ifdef _WIN32

#include <windows.h>
#include <delayimp.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t g_runtime_dir[MAX_PATH] = {0};

static FARPROC WINAPI
displayxr_dli_hook(unsigned dli_notify, PDelayLoadInfo pdli)
{
	if (dli_notify != dliNotePreLoadLibrary || g_runtime_dir[0] == L'\0' || pdli == NULL ||
	    pdli->szDll == NULL) {
		return NULL;
	}

	wchar_t dll_wide[MAX_PATH];
	int converted = MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, dll_wide, MAX_PATH);
	if (converted <= 0) {
		return NULL;
	}

	wchar_t full_path[MAX_PATH];
	int written = swprintf_s(full_path, MAX_PATH, L"%s\\%s", g_runtime_dir, dll_wide);
	if (written <= 0) {
		return NULL;
	}

	// LOAD_WITH_ALTERED_SEARCH_PATH so any transitive imports of the
	// loaded DLL are resolved against its own directory (i.e. our install
	// dir), not the host app's exe directory.
	HMODULE h = LoadLibraryExW(full_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	return (FARPROC)h;
}

// MSVC delay-load runtime picks this up by name. Falls back to the default
// resolver when the hook returns NULL. Note: modern delayimp.h declares
// this with `const` qualification — definition must match.
const PfnDliHook __pfnDliNotifyHook2 = displayxr_dli_hook;

BOOL APIENTRY
DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	(void)lpReserved;
	if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hModule);
		if (GetModuleFileNameW(hModule, g_runtime_dir, MAX_PATH) > 0) {
			wchar_t *last_slash = wcsrchr(g_runtime_dir, L'\\');
			if (last_slash != NULL) {
				*last_slash = L'\0';
			} else {
				g_runtime_dir[0] = L'\0';
			}
		}
	}
	return TRUE;
}

#endif // _WIN32
