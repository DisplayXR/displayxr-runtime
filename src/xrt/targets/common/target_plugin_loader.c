// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side registry-driven loader for vendor plug-in DLLs.
 *
 * Enumerates `HKLM\Software\DisplayXR\DisplayProcessors\*`, sorts by
 * `ProbeOrder`, then `LoadLibraryExW` → `GetProcAddress` → negotiate
 * → probe each in turn. First plug-in whose probe returns
 * `XRT_SUCCESS` wins and is cached. The plug-in's chosen instance
 * handle (returned by `probe()`) is kept alongside the iface so the
 * runtime can hand it back to subsequent vtable calls.
 *
 * v1 hosts at most one active plug-in per process; later sequencing
 * steps may relax that for multi-display heterogeneous setups, per
 * `docs/roadmap/vendor-plugin-architecture.md` §3 non-goals.
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
#include <stdlib.h>
#include <string.h>


/*
 *
 * Shared state.
 *
 */

static int g_load_attempted = 0;
static const struct xrt_plugin_iface *g_active_iface = NULL;
static struct xrt_plugin_instance *g_active_instance = NULL;


/*
 *
 * Platform-specific loader.
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/*!
 * Maximum number of plug-ins we enumerate from the registry. 16 is
 * generous — the design admits at most a handful in practice (vendor
 * panels + the sim_display fallback).
 */
#define MAX_PLUGIN_ENTRIES 16

/*!
 * Registry-derived per-plug-in metadata. Filled in by the enumeration
 * pass, sorted, then iterated by the probe pass.
 */
struct plugin_entry
{
	wchar_t binary_path[MAX_PATH];
	char id[64];           /* subkey name (UTF-8) */
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order; /* defaults to 100 when REG value is missing */
};


/*
 *
 * Helpers.
 *
 */

/*!
 * Add the runtime DLL's own directory to the loader search path so
 * plug-ins' transitive deps (cjson.dll, pthread, future vulkan-1.dll)
 * resolve from the runtime's bin/. One-shot — repeated calls are
 * silently no-op'd.
 *
 * Pair with LOAD_LIBRARY_SEARCH_USER_DIRS on the LoadLibraryExW call.
 */
static void
add_runtime_dll_directory(void)
{
	static int added = 0;
	if (added) {
		return;
	}
	added = 1;

	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&add_runtime_dll_directory, &self)) {
		U_LOG_W("plugin loader: GetModuleHandleEx failed (%lu); transitive deps may not resolve.",
		        GetLastError());
		return;
	}

	wchar_t runtime_path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, runtime_path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		U_LOG_W("plugin loader: GetModuleFileName failed; transitive deps may not resolve.");
		return;
	}

	wchar_t *last_slash = wcsrchr(runtime_path, L'\\');
	if (last_slash == NULL) {
		return;
	}
	*last_slash = L'\0';

	(void)AddDllDirectory(runtime_path);
}

/*!
 * UTF-16 wchar_t string → UTF-8 char buffer. Returns true on success.
 */
static bool
wide_to_utf8(const wchar_t *src, char *dst, int dst_size)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL);
	if (n <= 0) {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
		return false;
	}
	return true;
}

/*!
 * Read an optional REG_SZ value into the supplied UTF-8 buffer. On
 * miss, leaves dst as an empty string.
 */
static void
read_optional_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[256];
	DWORD wbuf_bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes) == ERROR_SUCCESS) {
		wide_to_utf8(wbuf, dst, dst_size);
	} else {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
	}
}

/*!
 * Enumerate plug-in subkeys under HKLM\Software\DisplayXR\DisplayProcessors.
 *
 * Returns the number of entries populated (0 if the root key is
 * absent / unreadable). Entries with no Binary value are skipped —
 * they don't satisfy the spec's "required" rule.
 */
static int
enumerate_registry(struct plugin_entry *entries, int max)
{
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
	                           L"Software\\DisplayXR\\DisplayProcessors",
	                           0, KEY_READ, &root);
	if (rc != ERROR_SUCCESS) {
		U_LOG_I("plugin loader: registry root HKLM\\Software\\DisplayXR\\DisplayProcessors absent (rc=%ld) "
		        "— no plug-ins to try.",
		        rc);
		return 0;
	}

	int count = 0;
	DWORD index = 0;
	for (;;) {
		if (count >= max) {
			U_LOG_W("plugin loader: more than %d registered plug-ins — truncating.", max);
			break;
		}

		wchar_t subkey_name[256];
		DWORD subkey_name_len = (DWORD)(sizeof(subkey_name) / sizeof(wchar_t));
		LSTATUS er = RegEnumKeyExW(root, index, subkey_name, &subkey_name_len, NULL, NULL, NULL, NULL);
		index++;
		if (er == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (er != ERROR_SUCCESS) {
			U_LOG_W("plugin loader: RegEnumKeyEx failed (rc=%ld) at index %lu — stopping enumeration.",
			        er, (unsigned long)index);
			break;
		}

		HKEY sub;
		if (RegOpenKeyExW(root, subkey_name, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
			continue;
		}

		struct plugin_entry *e = &entries[count];
		memset(e, 0, sizeof(*e));

		/* Binary is required. */
		DWORD binary_bytes = (DWORD)sizeof(e->binary_path);
		if (RegGetValueW(sub, NULL, L"Binary", RRF_RT_REG_SZ, NULL, e->binary_path, &binary_bytes) !=
		    ERROR_SUCCESS) {
			U_LOG_W("plugin loader: subkey '%ls' missing required 'Binary' value — skipping.",
			        subkey_name);
			RegCloseKey(sub);
			continue;
		}

		wide_to_utf8(subkey_name, e->id, (int)sizeof(e->id));

		read_optional_string(sub, L"DisplayName", e->display_name, (int)sizeof(e->display_name));
		read_optional_string(sub, L"Vendor", e->vendor, (int)sizeof(e->vendor));
		read_optional_string(sub, L"Version", e->version, (int)sizeof(e->version));

		DWORD order = 100;
		DWORD order_bytes = sizeof(order);
		if (RegGetValueW(sub, NULL, L"ProbeOrder", RRF_RT_REG_DWORD, NULL, &order, &order_bytes) ==
		    ERROR_SUCCESS) {
			e->probe_order = (uint32_t)order;
		} else {
			e->probe_order = 100;
		}

		count++;
		RegCloseKey(sub);
	}

	RegCloseKey(root);
	return count;
}

static int
compare_by_probe_order(const void *a, const void *b)
{
	uint32_t oa = ((const struct plugin_entry *)a)->probe_order;
	uint32_t ob = ((const struct plugin_entry *)b)->probe_order;
	if (oa < ob) {
		return -1;
	}
	if (oa > ob) {
		return 1;
	}
	return 0;
}

/*!
 * Try one registered plug-in. Returns the iface on success and writes
 * the probed instance handle to *out_inst; returns NULL (and closes
 * the DLL) on any failure. Caller never sees the HMODULE — successful
 * loads intentionally leak it for the process lifetime so the iface's
 * function pointers remain callable.
 */
static const struct xrt_plugin_iface *
try_load_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;

	HMODULE dll = LoadLibraryExW(e->binary_path, NULL,
	                             LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_USER_DIRS |
	                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (dll == NULL) {
		U_LOG_W("plugin loader:   %s: LoadLibrary(%ls) failed (err=%lu).", e->id, e->binary_path,
		        GetLastError());
		return NULL;
	}

	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)(void *)GetProcAddress(dll, XRT_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		U_LOG_W("plugin loader:   %s: missing entry point '%s' — skipping.", e->id,
		        XRT_PLUGIN_ENTRYPOINT_NAME);
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
		U_LOG_W("plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id,
		        (int)xret, (void *)iface);
		FreeLibrary(dll);
		return NULL;
	}

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("plugin loader:   %s: probe declined (no matching device).", e->id);
			FreeLibrary(dll);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			FreeLibrary(dll);
			return NULL;
		}
	}

	U_LOG_W(
	    "plugin loader: active plug-in: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%ls",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order,
	    e->binary_path);

	return iface;
}

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = enumerate_registry(entries, MAX_PLUGIN_ENTRIES);
	if (n == 0) {
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_probe_order);

	add_runtime_dll_directory();

	U_LOG_I("plugin loader: %d registered plug-in(s); attempting in ProbeOrder ascending.", n);
	for (int i = 0; i < n; i++) {
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %ls)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			return iface;
		}
	}

	U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
	return NULL;
}

#else /* !XRT_OS_WINDOWS */

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;
	/*
	 * Plug-in DLL discovery ships on Windows first. macOS adopts a
	 * `~/Library/Application Support/DisplayXR/DisplayProcessors/`
	 * JSON-manifest discovery shape; Linux mirrors that under
	 * `$XDG_DATA_HOME/DisplayXR/DisplayProcessors/`. Both land with
	 * the cross-platform port.
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
target_plugin_get_active(void)
{
	if (g_load_attempted) {
		return g_active_iface;
	}
	g_load_attempted = 1;
	g_active_iface = discover_active_plugin(&g_active_instance);
	return g_active_iface;
}
