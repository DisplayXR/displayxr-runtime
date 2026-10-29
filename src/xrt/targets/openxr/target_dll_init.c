// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief DllMain + delay-load hook for DisplayXRClient.dll on Windows.
 *
 * Resolves /DELAYLOAD'd dependencies (e.g. vulkan-1.dll) from the runtime's
 * own install directory without requiring $INSTDIR to be on the system PATH.
 * We're loaded into the host app's process and must not mutate process-wide
 * DLL search behavior (no SetDefaultDllDirectories) — the delay-load
 * notification hook is the surgical option. (Vendor SR weaver DLLs are not
 * delay-loaded by the runtime; the Leia plug-in loads its own.)
 *
 * The same hook resolves the system graphics DLLs from System32 (#2461).
 */

#ifdef _WIN32

#include "util/u_logging.h"

#include <windows.h>
#include <delayimp.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t g_runtime_dir[MAX_PATH] = {0};

/*
 * System graphics DLLs (#2461).
 *
 * d3d11.dll, dxgi.dll and d3d12.dll are delay-loaded and always resolved
 * from System32 by full path. A host process may ship its own copies next
 * to its executable (DXVK translates D3D11 to Vulkan, a ReShade-style proxy
 * wraps it), and a by-name load would bind the runtime to those.
 *
 * Loading by full path is not enough on its own: Windows binds a DLL's
 * by-name imports to a module already loaded under that name, so the
 * system d3d11.dll's own import of dxgi.dll (CreateDXGIFactory2) lands on
 * the host's dxgi.dll. With a null adapter D3D11CreateDevice then builds
 * its factory through it. When that happens, the system d3d11.dll's dxgi.dll
 * import entries are repointed to the system dxgi.dll.
 */

static SRWLOCK g_system_lock = SRWLOCK_INIT;
static bool g_shadow_logged = false;

static bool
is_system_graphics_dll(const char *name)
{
	return _stricmp(name, "d3d11.dll") == 0 || _stricmp(name, "dxgi.dll") == 0 || _stricmp(name, "d3d12.dll") == 0;
}

static HMODULE
load_system_dll(const char *name)
{
	wchar_t system_dir[MAX_PATH];
	UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return NULL;
	}

	wchar_t path[MAX_PATH];
	if (swprintf_s(path, MAX_PATH, L"%s\\%S", system_dir, name) <= 0) {
		return NULL;
	}

	// LOAD_LIBRARY_SEARCH_SYSTEM32: dependencies not loaded yet come from
	// System32 too, never from the host's executable directory.
	return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

//! Repoints @p module's import entries for @p dll to the exports of @p target.
static void
repoint_imports(HMODULE module, const char *dll, HMODULE target)
{
	BYTE *base = (BYTE *)module;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0) {
		return;
	}

	for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name != 0;
	     desc++) {
		if (_stricmp((const char *)(base + desc->Name), dll) != 0 || desc->OriginalFirstThunk == 0) {
			continue;
		}

		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
		IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
		for (; names->u1.AddressOfData != 0; names++, addrs++) {
			FARPROC fn = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
			                 ? GetProcAddress(target, MAKEINTRESOURCEA(IMAGE_ORDINAL(names->u1.Ordinal)))
			                 : GetProcAddress(target, ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name);
			if (fn == NULL || addrs->u1.Function == (ULONG_PTR)fn) {
				continue;
			}

			DWORD old_protect;
			if (VirtualProtect(&addrs->u1.Function, sizeof(addrs->u1.Function), PAGE_READWRITE, &old_protect)) {
				addrs->u1.Function = (ULONG_PTR)fn;
				VirtualProtect(&addrs->u1.Function, sizeof(addrs->u1.Function), old_protect, &old_protect);
			}
		}
	}
}

//! True when a dxgi.dll other than the system one is loaded in the process.
static bool
dxgi_is_shadowed(HMODULE system_dxgi, wchar_t *shadow_path, DWORD shadow_path_size)
{
	HMODULE by_name = GetModuleHandleW(L"dxgi.dll");
	if (by_name == NULL || by_name == system_dxgi) {
		return false;
	}
	GetModuleFileNameW(by_name, shadow_path, shadow_path_size);
	return true;
}

/*!
 * Loads a system graphics DLL (d3d11.dll, dxgi.dll, d3d12.dll) from System32,
 * whatever copies the host process ships. Exported for display plug-ins,
 * which use it from their own delay-load hook (#2461).
 */
__declspec(dllexport) HMODULE
displayxr_load_system_graphics_dll(const char *name)
{
	if (name == NULL || !is_system_graphics_dll(name)) {
		return NULL;
	}

	HMODULE module = load_system_dll(name);
	if (module == NULL || _stricmp(name, "d3d11.dll") != 0) {
		return module;
	}

	HMODULE system_dxgi = load_system_dll("dxgi.dll");
	wchar_t shadow[MAX_PATH] = {0};
	if (system_dxgi == NULL || !dxgi_is_shadowed(system_dxgi, shadow, MAX_PATH)) {
		return module;
	}

	AcquireSRWLockExclusive(&g_system_lock);
	repoint_imports(module, "dxgi.dll", system_dxgi);
	bool log = !g_shadow_logged;
	g_shadow_logged = true;
	ReleaseSRWLockExclusive(&g_system_lock);

	if (log) {
		// One-off: this host ships its own dxgi.dll (#2461)
		char shadow_utf8[MAX_PATH * 3] = {0};
		WideCharToMultiByte(CP_UTF8, 0, shadow, -1, shadow_utf8, sizeof(shadow_utf8), NULL, NULL);
		U_LOG_W("#2461: the host process loads its own dxgi.dll (%s); the runtime uses the System32 "
		        "d3d11.dll/dxgi.dll/d3d12.dll",
		        shadow_utf8);
	}
	return module;
}

static FARPROC WINAPI
displayxr_dli_hook(unsigned dli_notify, PDelayLoadInfo pdli)
{
	if (dli_notify != dliNotePreLoadLibrary || pdli == NULL || pdli->szDll == NULL) {
		return NULL;
	}

	if (is_system_graphics_dll(pdli->szDll)) {
		return (FARPROC)displayxr_load_system_graphics_dll(pdli->szDll);
	}

	if (g_runtime_dir[0] == L'\0') {
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
