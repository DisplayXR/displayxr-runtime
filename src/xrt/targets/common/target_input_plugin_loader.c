// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry / manifest-driven loader for input-provider plug-ins.
 *
 * Mirrors `target_plugin_loader.c` (the display-processor loader) for the
 * second plug-in type (ADR-034): enumerate the discovery root, sort by
 * ProbeOrder, load → negotiate → ABI-gate → probe until one provider
 * claims the system. Shares the DP loader's plumbing where it is shaped
 * for sharing (POSIX discovery roots, the Windows runtime-core-DLL
 * preload, the #434 preload sanitizer); the enumeration itself is
 * per-type because the roots and value schemas differ.
 *
 * v1 hosts at most one active provider per process and has no refresh
 * path: provider devices are created exactly once at system build, so
 * the DP loader's #342 mid-install refresh problem (factory pointers
 * baked into long-lived compositor state) does not arise here.
 *
 * Issue #823.
 *
 * @ingroup target_common
 */

#include "target_input_plugin_loader.h"
#include "target_plugin_loader.h"
#include "target_plugin_preload_sanitize.h"

#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "util/u_logging.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Shared state.
 *
 */

static int g_input_load_attempted = 0;
static const struct xrt_input_plugin_iface *g_input_active_iface = NULL;
static struct xrt_input_plugin_instance *g_input_active_instance = NULL;

/*!
 * Outcome tally of the one discovery scan, for
 * @ref target_input_plugin_get_scan_result. "Nobody claimed the system"
 * is a normal state — every provider may legitimately decline (no
 * hardware of its type, or an opt-in like sim-input's `DXR_SIM_INPUT`
 * left unset) — so the self-test must not read it as a fault. What IS a
 * fault is a provider that could not even be dispatched: a missing entry
 * point, a failed load, or an ABI-major mismatch. Counting the two
 * separately is what lets `displayxr-cli selftest` tell them apart.
 */
static int g_input_scan_declined = 0;
static int g_input_scan_failed = 0;

/*!
 * Set by `input_try_load_one` when it returned NULL because the provider
 * declined cleanly (`XRT_ERROR_PROBER_NOT_SUPPORTED` from `negotiate` or
 * `probe`) rather than because it could not be dispatched. Read once per
 * entry by `input_discover_active`; the loader scan is single-threaded.
 */
static bool g_input_last_declined = false;

/*!
 * Maximum number of providers we enumerate. Matches the DP loader's
 * bound — a handful in practice.
 */
#define MAX_INPUT_PLUGIN_ENTRIES 16


/*
 *
 * Platform-specific discovery.
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

/*!
 * Registry-derived per-provider metadata, filled by the enumeration
 * pass under `HKLM\Software\DisplayXR\InputProviders\*`.
 */
struct input_plugin_entry
{
	wchar_t binary_path[MAX_PATH];
	char id[64]; /* subkey name (UTF-8) */
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order; /* defaults to 100 when REG value is missing */
	bool enabled;         /* `Enabled` DWORD; absent = enabled */
};

static bool
input_wide_to_utf8(const wchar_t *src, char *dst, int dst_size)
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

static void
input_read_optional_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[256];
	DWORD wbuf_bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes) == ERROR_SUCCESS) {
		input_wide_to_utf8(wbuf, dst, dst_size);
	} else {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
	}
}

/*!
 * Enumerate provider subkeys under HKLM\Software\DisplayXR\InputProviders.
 * Returns the number of entries populated (0 if the root key is absent).
 * Entries missing the required `Binary` value are skipped.
 */
static int
input_enumerate_registry(struct input_plugin_entry *entries, int max)
{
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\InputProviders", 0, KEY_READ, &root);
	if (rc != ERROR_SUCCESS) {
		U_LOG_I(
		    "input plugin loader: registry root HKLM\\Software\\DisplayXR\\InputProviders absent "
		    "(rc=%ld) — no providers to try.",
		    rc);
		return 0;
	}

	int count = 0;
	DWORD index = 0;
	for (;;) {
		if (count >= max) {
			U_LOG_W("input plugin loader: more than %d registered providers — truncating.", max);
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
			U_LOG_W("input plugin loader: RegEnumKeyEx failed (rc=%ld) at index %lu — stopping.", er,
			        (unsigned long)index);
			break;
		}

		HKEY sub;
		if (RegOpenKeyExW(root, subkey_name, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
			continue;
		}

		struct input_plugin_entry *e = &entries[count];
		memset(e, 0, sizeof(*e));

		DWORD binary_bytes = (DWORD)sizeof(e->binary_path);
		if (RegGetValueW(sub, NULL, L"Binary", RRF_RT_REG_SZ, NULL, e->binary_path, &binary_bytes) !=
		    ERROR_SUCCESS) {
			U_LOG_W("input plugin loader: subkey '%ls' missing required 'Binary' value — skipping.",
			        subkey_name);
			RegCloseKey(sub);
			continue;
		}

		input_wide_to_utf8(subkey_name, e->id, (int)sizeof(e->id));
		input_read_optional_string(sub, L"DisplayName", e->display_name, (int)sizeof(e->display_name));
		input_read_optional_string(sub, L"Vendor", e->vendor, (int)sizeof(e->vendor));
		input_read_optional_string(sub, L"Version", e->version, (int)sizeof(e->version));

		DWORD order = 100;
		DWORD order_bytes = sizeof(order);
		e->probe_order = 100;
		if (RegGetValueW(sub, NULL, L"ProbeOrder", RRF_RT_REG_DWORD, NULL, &order, &order_bytes) ==
		    ERROR_SUCCESS) {
			e->probe_order = (uint32_t)order;
		}

		DWORD enabled = 1;
		DWORD enabled_bytes = sizeof(enabled);
		e->enabled = true;
		if (RegGetValueW(sub, NULL, L"Enabled", RRF_RT_REG_DWORD, NULL, &enabled, &enabled_bytes) ==
		    ERROR_SUCCESS) {
			e->enabled = enabled != 0;
		}

		count++;
		RegCloseKey(sub);
	}

	RegCloseKey(root);
	return count;
}

static int
input_compare_by_probe_order(const void *a, const void *b)
{
	uint32_t oa = ((const struct input_plugin_entry *)a)->probe_order;
	uint32_t ob = ((const struct input_plugin_entry *)b)->probe_order;
	if (oa < ob) {
		return -1;
	}
	if (oa > ob) {
		return 1;
	}
	return 0;
}

/*!
 * Load + negotiate + ABI-check + probe one registered provider. Returns
 * the iface (writing the instance to *out_inst) on success, NULL (and
 * closes the DLL) on any failure. Successful loads intentionally leak
 * the HMODULE for the process lifetime.
 */
static const struct xrt_input_plugin_iface *
input_try_load_one(const struct input_plugin_entry *e, struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;
	g_input_last_declined = false;

	// Same host-crash breadcrumb + #434 sanitizer as the DP loader —
	// LoadLibrary of a provider runs host DLL-notification code too.
	target_plugin_sanitized_preload(e->binary_path);
	U_LOG_W("input plugin loader:   %s: loading provider binary %ls", e->id, e->binary_path);

	HMODULE dll = LoadLibraryExW(e->binary_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (dll == NULL) {
		U_LOG_W("input plugin loader:   %s: LoadLibrary(%ls) failed (err=%lu).", e->id, e->binary_path,
		        GetLastError());
		return NULL;
	}

	xrt_input_plugin_negotiate_fn_t negotiate =
	    (xrt_input_plugin_negotiate_fn_t)(void *)GetProcAddress(dll, XRT_INPUT_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		U_LOG_W("input plugin loader:   %s: missing entry point '%s' — skipping.", e->id,
		        XRT_INPUT_PLUGIN_ENTRYPOINT_NAME);
		FreeLibrary(dll);
		return NULL;
	}

	struct xrt_input_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_input_plugin_host_iface);
	host.host_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;

	struct xrt_input_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_INPUT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		g_input_last_declined = xret == XRT_ERROR_PROBER_NOT_SUPPORTED;
		U_LOG_W("input plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id, (int)xret,
		        (void *)iface);
		FreeLibrary(dll);
		return NULL;
	}

	/* ADR-020 rule 3: never dispatch through a mismatched-vtable. */
	if (plugin_version != XRT_INPUT_PLUGIN_API_VERSION_CURRENT) {
		U_LOG_E(
		    "input plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the provider must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    e->id, plugin_version, (unsigned)XRT_INPUT_PLUGIN_API_VERSION_CURRENT);
		FreeLibrary(dll);
		return NULL;
	}

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("input plugin loader:   %s: probe declined (no matching hardware).", e->id);
			g_input_last_declined = true;
			FreeLibrary(dll);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("input plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			FreeLibrary(dll);
			return NULL;
		}
	}

	U_LOG_W(
	    "input plugin loader: active input provider: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%ls",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order, e->binary_path);
	return iface;
}

static const struct xrt_input_plugin_iface *
input_discover_active(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	// Providers import DisplayXRClient.dll like DP plug-ins do (#328).
	target_plugin_preload_runtime_core_dll();

	struct input_plugin_entry entries[MAX_INPUT_PLUGIN_ENTRIES];
	int n = input_enumerate_registry(entries, MAX_INPUT_PLUGIN_ENTRIES);
	if (n == 0) {
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), input_compare_by_probe_order);

	U_LOG_I("input plugin loader: %d registered provider(s); attempting in ProbeOrder ascending.", n);
	for (int i = 0; i < n; i++) {
		if (!entries[i].enabled) {
			U_LOG_I("input plugin loader:   [%d/%d] %s disabled (Enabled=0) — skipping.", i + 1, n,
			        entries[i].id);
			continue;
		}
		U_LOG_I("input plugin loader:   [%d/%d] %s (ProbeOrder=%u, %ls)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_input_plugin_iface *iface = input_try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			return iface;
		}
		if (g_input_last_declined) {
			g_input_scan_declined++;
		} else {
			g_input_scan_failed++;
		}
	}

	U_LOG_I(
	    "input plugin loader: no registered provider claimed the system (%d declined, %d failed to load) — "
	    "qwerty keeps the hand roles.",
	    g_input_scan_declined, g_input_scan_failed);
	return NULL;
}

int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	struct input_plugin_entry entries[MAX_INPUT_PLUGIN_ENTRIES];
	int n = input_enumerate_registry(entries, MAX_INPUT_PLUGIN_ENTRIES);
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		struct target_input_plugin_desc *d = &out[i];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", entries[i].id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", entries[i].display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", entries[i].vendor);
		snprintf(d->version, sizeof(d->version), "%s", entries[i].version);
		input_wide_to_utf8(entries[i].binary_path, d->binary_path, (int)sizeof(d->binary_path));
		d->probe_order = entries[i].probe_order;
		d->enabled = entries[i].enabled;
	}

	return n;
}

bool
target_input_plugin_get_force_qwerty(void)
{
	DWORD value = 0;
	DWORD bytes = sizeof(value);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Input", L"ForceQwerty", RRF_RT_REG_DWORD,
	                          NULL, &value, &bytes);
	return rc == ERROR_SUCCESS && value != 0;
}

#elif defined(XRT_OS_ANDROID)

/*
 * Android ships no input providers in v1 — the convention-driven
 * `libdxrp*` discovery is DP-only for now. Stubs keep the builder
 * wiring platform-uniform; the qwerty fallback (where built) or the
 * shell-side input path is unaffected.
 */

static const struct xrt_input_plugin_iface *
input_discover_active(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;
	return NULL;
}

int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

bool
target_input_plugin_get_force_qwerty(void)
{
	return false;
}

#else /* POSIX: macOS / desktop Linux */

#include "util/u_json.h"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

/*
 * Discovery: `NNN-<name>-input-provider.json` manifests in the SAME
 * roots the DP loader scans (target_plugin_build_discovery_roots) —
 * input-provider-discovery.md §3. The three-digit ProbeOrder filename
 * prefix gives lexicographic ordering for free; the DP loader skips
 * the `-input-provider.json` suffix, this loader requires it.
 */

struct input_plugin_entry
{
	char manifest_path[PATH_MAX];
	char manifest_file[PATH_MAX]; /* basename, used for stable sort */
	char binary_path[PATH_MAX];
	char id[64];
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order;
	bool enabled;
};

static int
input_compare_by_filename(const void *a, const void *b)
{
	return strcmp(((const struct input_plugin_entry *)a)->manifest_file,
	              ((const struct input_plugin_entry *)b)->manifest_file);
}

static void
input_copy_optional_str(const cJSON *root, const char *field, char *dst, size_t dst_size)
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
input_parse_manifest(const char *path, struct input_plugin_entry *e)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		U_LOG_W("input plugin loader: %s: fopen failed (errno=%d).", path, errno);
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || len > 64 * 1024) {
		U_LOG_W("input plugin loader: %s: implausible manifest size %ld.", path, len);
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
		U_LOG_W("input plugin loader: %s: JSON parse failed.", path);
		return false;
	}

	char fmt[16] = {0};
	const cJSON *fmt_node = u_json_get(root, "file_format_version");
	if (fmt_node != NULL) {
		(void)u_json_get_string_into_array(fmt_node, fmt, sizeof(fmt));
	}
	if (strcmp(fmt, "1.0") != 0) {
		U_LOG_W("input plugin loader: %s: unsupported file_format_version='%s'.", path, fmt);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *plugin = u_json_get(root, "plugin");
	if (plugin == NULL) {
		U_LOG_W("input plugin loader: %s: missing 'plugin' object.", path);
		cJSON_Delete(root);
		return false;
	}

	input_copy_optional_str(plugin, "id", e->id, sizeof(e->id));
	input_copy_optional_str(plugin, "display_name", e->display_name, sizeof(e->display_name));
	input_copy_optional_str(plugin, "vendor", e->vendor, sizeof(e->vendor));
	input_copy_optional_str(plugin, "version", e->version, sizeof(e->version));
	input_copy_optional_str(plugin, "binary_path", e->binary_path, sizeof(e->binary_path));

	int probe_order = 100;
	const cJSON *po = u_json_get(plugin, "probe_order");
	if (po != NULL) {
		(void)u_json_get_int(po, &probe_order);
	}
	e->probe_order = (uint32_t)probe_order;
	e->enabled = true;

	cJSON_Delete(root);

	if (e->id[0] == '\0' || e->binary_path[0] == '\0') {
		U_LOG_W("input plugin loader: %s: required fields 'plugin.id' or 'plugin.binary_path' missing.", path);
		return false;
	}
	return true;
}

static bool
input_already_have_id(const struct input_plugin_entry *entries, int n, const char *id)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static int
input_enumerate_dir(const char *root, struct input_plugin_entry *entries, int start, int max)
{
	DIR *d = opendir(root);
	if (d == NULL) {
		return start;
	}

	static const char ip_suffix[] = "-input-provider.json";
	const size_t ip_len = sizeof(ip_suffix) - 1;

	int count = start;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (count >= max) {
			U_LOG_W("input plugin loader: more than %d entries — truncating.", max);
			break;
		}
		const char *name = de->d_name;
		size_t n = strlen(name);
		if (n <= ip_len || strcmp(name + n - ip_len, ip_suffix) != 0) {
			continue;
		}

		struct input_plugin_entry e;
		memset(&e, 0, sizeof(e));
		snprintf(e.manifest_path, sizeof(e.manifest_path), "%s/%s", root, name);
		snprintf(e.manifest_file, sizeof(e.manifest_file), "%s", name);

		if (!input_parse_manifest(e.manifest_path, &e)) {
			continue;
		}
		/* Per-user shadows system: roots are walked in priority order,
		 * so the first occurrence of an `<id>` wins. */
		if (input_already_have_id(entries, count, e.id)) {
			U_LOG_I("input plugin loader:   %s: shadowed by earlier manifest for id='%s'.", e.manifest_path,
			        e.id);
			continue;
		}
		entries[count++] = e;
	}
	closedir(d);
	return count;
}

static int
input_enumerate_all(struct input_plugin_entry *entries, int max)
{
	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));

	int n = 0;
	for (int r = 0; r < n_roots; r++) {
		n = input_enumerate_dir(roots[r], entries, n, max);
	}
	if (n > 1) {
		qsort(entries, (size_t)n, sizeof(entries[0]), input_compare_by_filename);
	}
	return n;
}

static const struct xrt_input_plugin_iface *
input_try_load_one(const struct input_plugin_entry *e, struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;
	g_input_last_declined = false;

	/* RTLD_LOCAL keeps the provider's symbols private; aux symbols
	 * resolve via the runtime dylib/.so already in the process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		U_LOG_W("input plugin loader:   %s: dlopen(%s) failed: %s.", e->id, e->binary_path, dlerror());
		return NULL;
	}

	dlerror();
	xrt_input_plugin_negotiate_fn_t negotiate =
	    (xrt_input_plugin_negotiate_fn_t)dlsym(handle, XRT_INPUT_PLUGIN_ENTRYPOINT_NAME);
	const char *err = dlerror();
	if (negotiate == NULL || err != NULL) {
		U_LOG_W("input plugin loader:   %s: missing entry point '%s' (%s) — skipping.", e->id,
		        XRT_INPUT_PLUGIN_ENTRYPOINT_NAME, err ? err : "null");
		dlclose(handle);
		return NULL;
	}

	struct xrt_input_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_input_plugin_host_iface);
	host.host_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;

	struct xrt_input_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_INPUT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		g_input_last_declined = xret == XRT_ERROR_PROBER_NOT_SUPPORTED;
		U_LOG_W("input plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id, (int)xret,
		        (void *)iface);
		dlclose(handle);
		return NULL;
	}

	/* ADR-020 rule 3: never dispatch through a mismatched vtable. */
	if (plugin_version != XRT_INPUT_PLUGIN_API_VERSION_CURRENT) {
		U_LOG_E(
		    "input plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the provider must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    e->id, plugin_version, (unsigned)XRT_INPUT_PLUGIN_API_VERSION_CURRENT);
		dlclose(handle);
		return NULL;
	}

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("input plugin loader:   %s: probe declined (no matching hardware).", e->id);
			g_input_last_declined = true;
			dlclose(handle);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("input plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			dlclose(handle);
			return NULL;
		}
	}

	U_LOG_W(
	    "input plugin loader: active input provider: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order, e->binary_path);

	/* dlopen handle intentionally leaked, same lifetime rule as the DP
	 * loader — the devices' vtables live in the provider binary. */
	return iface;
}

static const struct xrt_input_plugin_iface *
input_discover_active(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	struct input_plugin_entry entries[MAX_INPUT_PLUGIN_ENTRIES];
	int n = input_enumerate_all(entries, MAX_INPUT_PLUGIN_ENTRIES);
	if (n == 0) {
		U_LOG_I("input plugin loader: no input-provider manifests found.");
		return NULL;
	}

	U_LOG_I("input plugin loader: %d registered provider(s); attempting in filename order.", n);
	for (int i = 0; i < n; i++) {
		U_LOG_I("input plugin loader:   [%d/%d] %s (ProbeOrder=%u, %s)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_input_plugin_iface *iface = input_try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			return iface;
		}
		if (g_input_last_declined) {
			g_input_scan_declined++;
		} else {
			g_input_scan_failed++;
		}
	}

	U_LOG_I(
	    "input plugin loader: no registered provider claimed the system (%d declined, %d failed to load) — "
	    "qwerty keeps the hand roles.",
	    g_input_scan_declined, g_input_scan_failed);
	return NULL;
}

int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	struct input_plugin_entry entries[MAX_INPUT_PLUGIN_ENTRIES];
	int n = input_enumerate_all(entries, MAX_INPUT_PLUGIN_ENTRIES);
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		struct target_input_plugin_desc *d = &out[i];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", entries[i].id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", entries[i].display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", entries[i].vendor);
		snprintf(d->version, sizeof(d->version), "%s", entries[i].version);
		snprintf(d->binary_path, sizeof(d->binary_path), "%s", entries[i].binary_path);
		d->probe_order = entries[i].probe_order;
		d->enabled = entries[i].enabled;
	}

	return n;
}

/*!
 * POSIX per-user manifest dir — mirrors the DP loader's `preferred` file
 * location; the `force_qwerty` gate file lives next to it.
 */
static bool
input_user_manifest_dir(char *out, size_t cap)
{
	const char *home = getenv("HOME");
#ifdef __APPLE__
	if (home == NULL || *home == '\0') {
		return false;
	}
	snprintf(out, cap, "%s/Library/Application Support/DisplayXR/DisplayProcessors", home);
	return true;
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(out, cap, "%s/DisplayXR/DisplayProcessors", xdg);
		return true;
	}
	if (home != NULL && *home != '\0') {
		snprintf(out, cap, "%s/.local/share/DisplayXR/DisplayProcessors", home);
		return true;
	}
	return false;
#endif
}

bool
target_input_plugin_get_force_qwerty(void)
{
	char dir[PATH_MAX];
	if (!input_user_manifest_dir(dir, sizeof(dir))) {
		return false;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/force_qwerty", dir);

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	int c = fgetc(f);
	fclose(f);
	/* Presence forces qwerty; a leading '0' turns the file into a
	 * no-op so it can be toggled without deleting it. */
	return c != '0';
}

#endif /* platform-specific discovery */


/*
 *
 * Public iface.
 *
 */

const struct xrt_input_plugin_iface *
target_input_plugin_get_active(void)
{
	if (g_input_load_attempted) {
		return g_input_active_iface;
	}
	g_input_load_attempted = 1;

	g_input_active_iface = input_discover_active(&g_input_active_instance);
	return g_input_active_iface;
}

struct xrt_input_plugin_instance *
target_input_plugin_get_active_instance(void)
{
	(void)target_input_plugin_get_active();
	return g_input_active_instance;
}

void
target_input_plugin_get_scan_result(int *out_declined, int *out_failed)
{
	(void)target_input_plugin_get_active();
	if (out_declined != NULL) {
		*out_declined = g_input_scan_declined;
	}
	if (out_failed != NULL) {
		*out_failed = g_input_scan_failed;
	}
}
