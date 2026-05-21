// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for the sim_display plug-in DLL.
 *
 * v1 hardcodes the plug-in's path relative to the runtime DLL's own
 * directory. Registry-driven enumeration of arbitrary plug-ins lands
 * in the next sequencing step; this file's structure is shaped to
 * make that extension a localized change.
 *
 * Issue #256.
 *
 * @ingroup target_common
 */

#include "target_plugin_loader.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "util/u_logging.h"

#include <stddef.h>


/*
 *
 * Shared state.
 *
 */

static int g_load_attempted = 0;
static const struct xrt_plugin_iface *g_sim_display_iface = NULL;


/*
 *
 * Platform-specific loader.
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define PLUGIN_REL_PATH L"plugins\\DisplayXR-SimDisplay.dll"

static const struct xrt_plugin_iface *
load_sim_display_plugin(void)
{
	/*
	 * Find the runtime DLL's own directory by asking Windows for the
	 * module that contains the address of this function. This
	 * resolves correctly whether the runtime DLL was loaded from
	 * Program Files (installed) or from a dev `_package/bin/`.
	 */
	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&load_sim_display_plugin, &self)) {
		U_LOG_W("plugin loader: GetModuleHandleEx failed (%lu) — falling back to static sim_display.",
		        GetLastError());
		return NULL;
	}

	wchar_t runtime_path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, runtime_path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		U_LOG_W("plugin loader: GetModuleFileName failed — falling back to static sim_display.");
		return NULL;
	}

	/* Strip the file name component to get the runtime's directory. */
	wchar_t *last_slash = wcsrchr(runtime_path, L'\\');
	if (last_slash == NULL) {
		U_LOG_W("plugin loader: runtime path has no separator — falling back to static sim_display.");
		return NULL;
	}
	*last_slash = L'\0';

	/*
	 * Compose `<runtime dir>\plugins\DisplayXR-SimDisplay.dll`. The
	 * plug-in's transitive deps (cjson.dll, pthread, vulkan-1.dll if it
	 * ever picks them up) live one directory up in `<runtime dir>`;
	 * AddDllDirectory + LOAD_LIBRARY_SEARCH_USER_DIRS makes the loader
	 * search there too. LOAD_WITH_ALTERED_SEARCH_PATH additionally puts
	 * the plug-in's own directory first.
	 */
	wchar_t plugin_path[MAX_PATH];
	if (_snwprintf_s(plugin_path, MAX_PATH, _TRUNCATE, L"%s\\%s", runtime_path, PLUGIN_REL_PATH) < 0) {
		U_LOG_W("plugin loader: composed plug-in path too long — falling back to static sim_display.");
		return NULL;
	}

	(void)AddDllDirectory(runtime_path);

	HMODULE dll = LoadLibraryExW(plugin_path, NULL,
	                             LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_USER_DIRS |
	                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (dll == NULL) {
		/*
		 * Not present, version mismatch, or missing a transitive
		 * dependency. Logged at INFO since developer builds without
		 * `install` regularly take this path; the static-linked
		 * fallback keeps the runtime functional.
		 */
		U_LOG_I("plugin loader: LoadLibrary(%ls) failed (err=%lu) — falling back to static sim_display.",
		        plugin_path, GetLastError());
		return NULL;
	}

	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)(void *)GetProcAddress(dll, XRT_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		U_LOG_W("plugin loader: %ls missing entry point '%s' — falling back to static sim_display.",
		        plugin_path, XRT_PLUGIN_ENTRYPOINT_NAME);
		FreeLibrary(dll);
		return NULL;
	}

	struct xrt_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host.host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("plugin loader: %ls xrtPluginNegotiate returned %d, iface=%p — falling back to static "
		        "sim_display.",
		        plugin_path, (int)xret, (void *)iface);
		FreeLibrary(dll);
		return NULL;
	}

	/*
	 * Loaded. The DLL handle is intentionally not stored — the iface
	 * pointer keeps the DLL pinned for the runtime's lifetime via the
	 * function-pointer references stored in xsysc->info.dp_factory_*.
	 * Proper shutdown lands with registry-driven discovery.
	 */
	U_LOG_W("plugin loader: loaded %ls (id=%s, name=%s, vendor=%s, plugin_api=%u)", plugin_path,
	        iface->id ? iface->id : "?",
	        iface->display_name ? iface->display_name : "?",
	        iface->vendor ? iface->vendor : "?",
	        plugin_version);

	return iface;
}

#else /* !XRT_OS_WINDOWS */

static const struct xrt_plugin_iface *
load_sim_display_plugin(void)
{
	/*
	 * Plug-in DLL ships on Windows only in v1. Linux + macOS will get
	 * the same shape after the registry/manifest discovery lands and
	 * the per-platform install layout is settled.
	 */
	return NULL;
}

#endif /* XRT_OS_WINDOWS */


/*
 *
 * Public iface.
 *
 */

const struct xrt_plugin_iface *
target_plugin_get_sim_display(void)
{
	if (g_load_attempted) {
		return g_sim_display_iface;
	}
	g_load_attempted = 1;
	g_sim_display_iface = load_sim_display_plugin();
	return g_sim_display_iface;
}
