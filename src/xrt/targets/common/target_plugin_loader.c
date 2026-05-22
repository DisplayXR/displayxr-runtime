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

#include <errno.h>
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

/*
 * (Earlier revisions of this loader called AddDllDirectory(runtime dir)
 * before LoadLibraryExW with LOAD_LIBRARY_SEARCH_USER_DIRS, but that flag
 * is mutually exclusive with LOAD_WITH_ALTERED_SEARCH_PATH per MSDN —
 * combining them returns ERROR_INVALID_PARAMETER for plug-ins that have
 * transitive deps the loader had to actually walk. We rely on
 * LOAD_WITH_ALTERED_SEARCH_PATH alone now: it puts the plug-in DLL's
 * own directory first in the search, and the legacy strategy continues
 * to PATH, so SR DLLs at `C:\Program Files\LeiaSR\Platform\bin` and
 * cjson.dll / vulkan-1.dll alongside the runtime resolve through their
 * existing PATH entries.)
 */

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

	HMODULE dll = LoadLibraryExW(e->binary_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
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

#include "util/u_json.h"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

/*
 * Same upper bound as the Windows path — admits a vendor plug-in plus
 * the sim_display fallback with plenty of headroom.
 */
#define MAX_PLUGIN_ENTRIES 16

/*
 * Discovery contract: see docs/specs/runtime/plugin-discovery.md §3.
 *
 * Each plug-in publishes a JSON manifest with the filename convention
 * `<probe_order>-<id>.json` (three-digit zero-padded ProbeOrder prefix
 * gives lexicographic ordering — `050-leia-sr.json` runs before
 * `200-sim-display.json` without parsing the JSON to sort).
 *
 * Roots searched, in priority order:
 *   1. $XRT_PLUGIN_SEARCH_PATH — dev override, single colon-separated list
 *   2. macOS: ~/Library/Application Support/DisplayXR/DisplayProcessors/
 *      Linux: $XDG_DATA_HOME/DisplayXR/DisplayProcessors/
 *             (defaults to ~/.local/share/DisplayXR/DisplayProcessors/)
 *   3. Linux only: /usr/local/share/displayxr/DisplayProcessors/
 *                  /usr/share/displayxr/DisplayProcessors/
 *
 * Per-user shadows system entries via lexicographic sort tie-break on
 * the absolute path (later inserts win — the loop inserts in priority
 * order, so per-user comes first and a duplicate `<id>` from a system
 * root is dropped at insert time).
 */

struct plugin_entry
{
	char manifest_path[PATH_MAX]; /* absolute path to the JSON */
	char manifest_file[PATH_MAX]; /* basename, used for stable sort */
	char binary_path[PATH_MAX];   /* absolute path to the dylib/.so */
	char id[64];                  /* manifest "plugin.id" */
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order;
};

static int
compare_by_filename(const void *a, const void *b)
{
	const struct plugin_entry *pa = (const struct plugin_entry *)a;
	const struct plugin_entry *pb = (const struct plugin_entry *)b;
	return strcmp(pa->manifest_file, pb->manifest_file);
}

static void
copy_optional_str(const cJSON *root, const char *field, char *dst, size_t dst_size)
{
	if (dst_size == 0) {
		return;
	}
	dst[0] = '\0';
	const cJSON *node = u_json_get(root, field);
	if (node == NULL) {
		return;
	}
	(void)u_json_get_string_into_array(node, dst, dst_size);
}

static bool
parse_manifest(const char *path, struct plugin_entry *e)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		U_LOG_W("plugin loader: %s: fopen failed (errno=%d).", path, errno);
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || len > 64 * 1024) {
		U_LOG_W("plugin loader: %s: implausible manifest size %ld.", path, len);
		fclose(f);
		return false;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	char *buf = (char *)malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		free(buf);
		return false;
	}
	buf[len] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (root == NULL) {
		U_LOG_W("plugin loader: %s: JSON parse failed.", path);
		return false;
	}

	char fmt[16] = {0};
	const cJSON *fmt_node = u_json_get(root, "file_format_version");
	if (fmt_node != NULL) {
		(void)u_json_get_string_into_array(fmt_node, fmt, sizeof(fmt));
	}
	if (strcmp(fmt, "1.0") != 0) {
		U_LOG_W("plugin loader: %s: unsupported file_format_version='%s'.", path, fmt);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *plugin = u_json_get(root, "plugin");
	if (plugin == NULL) {
		U_LOG_W("plugin loader: %s: missing 'plugin' object.", path);
		cJSON_Delete(root);
		return false;
	}

	copy_optional_str(plugin, "id", e->id, sizeof(e->id));
	copy_optional_str(plugin, "display_name", e->display_name, sizeof(e->display_name));
	copy_optional_str(plugin, "vendor", e->vendor, sizeof(e->vendor));
	copy_optional_str(plugin, "version", e->version, sizeof(e->version));
	copy_optional_str(plugin, "binary_path", e->binary_path, sizeof(e->binary_path));

	int probe_order = 100;
	const cJSON *po = u_json_get(plugin, "probe_order");
	if (po != NULL) {
		(void)u_json_get_int(po, &probe_order);
	}
	e->probe_order = (uint32_t)probe_order;

	cJSON_Delete(root);

	if (e->id[0] == '\0' || e->binary_path[0] == '\0') {
		U_LOG_W("plugin loader: %s: required fields 'plugin.id' or 'plugin.binary_path' missing.", path);
		return false;
	}
	return true;
}

static bool
already_have_id(const struct plugin_entry *entries, int n, const char *id)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static int
enumerate_dir(const char *root, struct plugin_entry *entries, int start, int max)
{
	DIR *d = opendir(root);
	if (d == NULL) {
		return start;
	}

	int count = start;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (count >= max) {
			U_LOG_W("plugin loader: more than %d entries — truncating.", max);
			break;
		}
		const char *name = de->d_name;
		size_t n = strlen(name);
		if (n < 6 || strcmp(name + n - 5, ".json") != 0) {
			continue;
		}

		struct plugin_entry e;
		memset(&e, 0, sizeof(e));
		snprintf(e.manifest_path, sizeof(e.manifest_path), "%s/%s", root, name);
		snprintf(e.manifest_file, sizeof(e.manifest_file), "%s", name);

		if (!parse_manifest(e.manifest_path, &e)) {
			continue;
		}
		/* Per-user shadows system: roots are walked in priority order,
		 * so the first occurrence of an `<id>` wins. */
		if (already_have_id(entries, count, e.id)) {
			U_LOG_I("plugin loader:   %s: shadowed by earlier manifest for id='%s'.", e.manifest_path,
			        e.id);
			continue;
		}
		entries[count++] = e;
	}
	closedir(d);
	return count;
}

static void
append_roots(char roots[][PATH_MAX], int max_roots, int *n_roots, const char *path)
{
	if (path == NULL || *path == '\0' || *n_roots >= max_roots) {
		return;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	snprintf(roots[(*n_roots)++], PATH_MAX, "%s", path);
}

static const struct xrt_plugin_iface *
try_load_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;

	/* RTLD_LOCAL keeps the plug-in's symbols private; aux symbols
	 * resolve via the dependent runtime dylib that ld already linked
	 * into our process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		U_LOG_W("plugin loader:   %s: dlopen(%s) failed: %s.", e->id, e->binary_path, dlerror());
		return NULL;
	}

	dlerror();
	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)dlsym(handle, XRT_PLUGIN_ENTRYPOINT_NAME);
	const char *err = dlerror();
	if (negotiate == NULL || err != NULL) {
		U_LOG_W("plugin loader:   %s: missing entry point '%s' (%s) — skipping.", e->id,
		        XRT_PLUGIN_ENTRYPOINT_NAME, err ? err : "null");
		dlclose(handle);
		return NULL;
	}

	struct xrt_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host.host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id, (int)xret,
		        (void *)iface);
		dlclose(handle);
		return NULL;
	}

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("plugin loader:   %s: probe declined (no matching device).", e->id);
			dlclose(handle);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			dlclose(handle);
			return NULL;
		}
	}

	U_LOG_W(
	    "plugin loader: active plug-in: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order, e->binary_path);

	/* dlopen handle intentionally leaked: the iface's function pointers
	 * remain reachable into the dylib for the process's lifetime. */
	return iface;
}

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;

	char roots[8][PATH_MAX];
	int n_roots = 0;

	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		/* Colon-separated list, like PATH. */
		const char *p = override;
		while (*p && n_roots < (int)(sizeof(roots) / sizeof(roots[0]))) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			if (len > 0 && len < PATH_MAX) {
				char buf[PATH_MAX];
				memcpy(buf, p, len);
				buf[len] = '\0';
				append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots, buf);
			}
			if (!colon) {
				break;
			}
			p = colon + 1;
		}
	}

	const char *home = getenv("HOME");
	char user_root[PATH_MAX];
#ifdef __APPLE__
	if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root),
		         "%s/Library/Application Support/DisplayXR/DisplayProcessors", home);
		append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots, user_root);
	}
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/DisplayXR/DisplayProcessors", xdg);
		append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots, user_root);
	} else if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/.local/share/DisplayXR/DisplayProcessors", home);
		append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots, user_root);
	}
	append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots,
	             "/usr/local/share/displayxr/DisplayProcessors");
	append_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])), &n_roots,
	             "/usr/share/displayxr/DisplayProcessors");
#endif

	if (n_roots == 0) {
		U_LOG_I("plugin loader: no discovery roots present — no plug-ins to try.");
		return NULL;
	}

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = 0;
	for (int r = 0; r < n_roots; r++) {
		n = enumerate_dir(roots[r], entries, n, MAX_PLUGIN_ENTRIES);
	}
	if (n == 0) {
		U_LOG_I("plugin loader: %d root(s) searched, no JSON manifests found.", n_roots);
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_filename);

	U_LOG_I("plugin loader: %d registered plug-in(s); attempting in filename order.", n);
	for (int i = 0; i < n; i++) {
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %s)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			return iface;
		}
	}

	U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
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

struct xrt_plugin_instance *
target_plugin_get_active_instance(void)
{
	/*
	 * Force a discovery pass if the caller hits this before
	 * target_plugin_get_active(). Order-independent at the cost of
	 * one extra branch.
	 */
	(void)target_plugin_get_active();
	return g_active_instance;
}
