// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The class-agnostic half of every component loader (ADR-054 N2).
 *
 * See the header for the contract. Everything here is driven by the
 * @ref target_component_class row the caller passes; nothing names a class.
 *
 * @ingroup target_common
 */

#include "target_component_loader.h"
#include "target_plugin_loader.h"
#include "target_plugin_path_guard.h"
#include "target_plugin_preload_sanitize.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include "util/u_json.h"
#include "util/u_logging.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Small string helpers (portable; no strncasecmp).
 *
 */

static void
tc_copy(char *dst, size_t dst_size, const char *src)
{
	if (dst_size == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static const char *
tc_basename(const char *path)
{
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	return base;
}

static bool
tc_usable(const struct target_component_entry *e)
{
	return (e->flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0;
}

static const char *
tc_tag(const struct target_component_class *cls)
{
	return (cls != NULL && cls->log_tag != NULL) ? cls->log_tag : "component loader";
}


/*
 *
 * Entry helpers.
 *
 */

void
target_component_entry_init(struct target_component_entry *e)
{
	memset(e, 0, sizeof(*e));
	e->probe_order = TARGET_COMPONENT_DEFAULT_PROBE_ORDER;
	e->enabled = true;
}

void
target_component_mark_malformed(struct target_component_entry *e, const char *reason)
{
	if (e == NULL) {
		return;
	}
	if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0 && e->reason[0] != '\0') {
		return; // first reason wins
	}
	e->flags |= TARGET_COMPONENT_FLAG_MALFORMED;
	tc_copy(e->reason, sizeof(e->reason), reason);
}

/*!
 * Post-read normalisation shared by every platform: display-name fallback and
 * the Enabled -> DISABLED flag (with a reason, unless the row is already
 * malformed and the reason says why).
 */
static void
tc_entry_finish(struct target_component_entry *e)
{
	if (e->display_name[0] == '\0') {
		tc_copy(e->display_name, sizeof(e->display_name), e->id);
	}
	if (!e->enabled) {
		e->flags |= TARGET_COMPONENT_FLAG_DISABLED;
		if (tc_usable(e)) {
			tc_copy(e->reason, sizeof(e->reason), "disabled (Enabled = 0)");
		}
	}
}

static int
tc_compare(const void *a, const void *b)
{
	const struct target_component_entry *ea = (const struct target_component_entry *)a;
	const struct target_component_entry *eb = (const struct target_component_entry *)b;
	if (ea->probe_order < eb->probe_order) {
		return -1;
	}
	if (ea->probe_order > eb->probe_order) {
		return 1;
	}
	int c = strcmp(ea->id, eb->id);
	if (c != 0) {
		return c;
	}
	// Same id twice only happens for malformed rows; keep the order total anyway.
	return strcmp(ea->source, eb->source);
}

void
target_component_sort(const struct target_component_class *cls, struct target_component_entry *entries, int count)
{
	if (entries == NULL || count <= 0) {
		return;
	}
	if (count > 1) {
		qsort(entries, (size_t)count, sizeof(entries[0]), tc_compare);
	}
	for (int i = 0; i < count; i++) {
		entries[i].flags &= ~TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE;
	}
	for (int i = 1; i < count; i++) {
		if (entries[i - 1].probe_order != entries[i].probe_order) {
			continue;
		}
		entries[i - 1].flags |= TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE;
		entries[i].flags |= TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE;
		// INFO: enumeration may re-run (conversion re-describes); the class
		// WARNs once per change where the vendor must hear it (#1466).
		U_LOG_I("%s: '%s' and '%s' share ProbeOrder %u — tie broken by id.", tc_tag(cls), entries[i - 1].id,
		        entries[i].id, entries[i].probe_order);
	}
}

int
target_component_merge(const struct target_component_class *cls,
                       struct target_component_entry *dst,
                       int dst_count,
                       int max,
                       const struct target_component_entry *src,
                       int src_count)
{
	if (dst == NULL || src == NULL) {
		return dst_count;
	}
	int n = dst_count;
	for (int i = 0; i < src_count; i++) {
		const struct target_component_entry *e = &src[i];
		if (tc_usable(e)) {
			bool dup = false;
			for (int j = 0; j < n; j++) {
				if (tc_usable(&dst[j]) && strcmp(dst[j].id, e->id) == 0) {
					U_LOG_I("%s:   %s: shadowed by %s for id='%s'.", tc_tag(cls), e->source,
					        dst[j].source, e->id);
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
		}
		if (n >= max) {
			U_LOG_I("%s: more than %d entries — truncating.", tc_tag(cls), max);
			break;
		}
		dst[n++] = *e;
	}
	return n;
}


/*
 *
 * Manifest JSON (pure; every platform).
 *
 */

/*!
 * ProbeOrder hint from a `NNN-<id>...` file name, or false when the name has
 * no three-digit prefix.
 */
static bool
tc_probe_order_from_filename(const char *name, uint32_t *out)
{
	if (name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9' ||
	    name[3] != '-') {
		return false;
	}
	*out = (uint32_t)((name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0'));
	return true;
}

static void
tc_json_str(const cJSON *obj, const char *field, char *dst, size_t dst_size)
{
	if (dst_size == 0) {
		return;
	}
	dst[0] = '\0';
	const cJSON *node = u_json_get(obj, field);
	if (node == NULL || !cJSON_IsString(node)) {
		return;
	}
	(void)u_json_get_string_into_array(node, dst, dst_size);
}

bool
target_component_parse_manifest_json(const struct target_component_class *cls,
                                     const char *json_text,
                                     const char *source,
                                     struct target_component_entry *out)
{
	if (out == NULL) {
		return false;
	}
	target_component_entry_init(out);
	tc_copy(out->source, sizeof(out->source), source != NULL ? source : "");
	// Until the manifest names itself, the file name is the best id we have.
	tc_copy(out->id, sizeof(out->id), source != NULL ? tc_basename(source) : "");

	if (json_text == NULL) {
		target_component_mark_malformed(out, "manifest is empty");
		return false;
	}

	cJSON *root = cJSON_Parse(json_text);
	if (root == NULL) {
		target_component_mark_malformed(out, "manifest is not valid JSON");
		return false;
	}

	char fmt[16] = {0};
	tc_json_str(root, "file_format_version", fmt, sizeof(fmt));
	if (strcmp(fmt, "1.0") != 0) {
		char why[96];
		snprintf(why, sizeof(why), "unsupported file_format_version '%s' (want \"1.0\")", fmt);
		target_component_mark_malformed(out, why);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *plugin = u_json_get(root, "plugin");
	if (plugin == NULL || !cJSON_IsObject(plugin)) {
		target_component_mark_malformed(out, "missing 'plugin' object");
		cJSON_Delete(root);
		return false;
	}

	char id[sizeof(out->id)] = {0};
	tc_json_str(plugin, "id", id, sizeof(id));
	if (id[0] != '\0') {
		tc_copy(out->id, sizeof(out->id), id);
	}
	tc_json_str(plugin, "display_name", out->display_name, sizeof(out->display_name));
	tc_json_str(plugin, "vendor", out->vendor, sizeof(out->vendor));
	tc_json_str(plugin, "version", out->version, sizeof(out->version));
	tc_json_str(plugin, "binary_path", out->binary_path, sizeof(out->binary_path));

	// ProbeOrder: the manifest field is authoritative; the file-name prefix
	// is the fallback (plugin-discovery.md §3.1), then the default.
	uint32_t hinted = 0;
	if (source != NULL && tc_probe_order_from_filename(tc_basename(source), &hinted)) {
		out->probe_order = hinted;
	}
	const cJSON *po = u_json_get(plugin, "probe_order");
	if (po != NULL) {
		int v = 0;
		if (!cJSON_IsNumber(po) || !u_json_get_int(po, &v) || v < 0) {
			target_component_mark_malformed(out, "'plugin.probe_order' is not a non-negative integer");
			cJSON_Delete(root);
			return false;
		}
		out->probe_order = (uint32_t)v;
	}

	const cJSON *en = u_json_get(plugin, "enabled");
	if (en != NULL) {
		if (!cJSON_IsBool(en)) {
			target_component_mark_malformed(out, "'plugin.enabled' is not a boolean");
			cJSON_Delete(root);
			return false;
		}
		out->enabled = cJSON_IsTrue(en);
	}

	const cJSON *settings = u_json_get(plugin, "settings");
	if (settings != NULL) {
		if (!cJSON_IsObject(settings)) {
			target_component_mark_malformed(out, "'plugin.settings' is not an object");
			cJSON_Delete(root);
			return false;
		}
		const cJSON *kv = NULL;
		cJSON_ArrayForEach(kv, settings)
		{
			if (kv->string == NULL) {
				continue;
			}
			if (out->setting_count >= TARGET_COMPONENT_MAX_SETTINGS) {
				U_LOG_I("%s: %s: more than %d settings — truncating.", tc_tag(cls), out->source,
				        TARGET_COMPONENT_MAX_SETTINGS);
				break;
			}
			struct target_component_setting *s = &out->settings[out->setting_count];
			tc_copy(s->key, sizeof(s->key), kv->string);
			if (cJSON_IsString(kv) && kv->valuestring != NULL) {
				tc_copy(s->value, sizeof(s->value), kv->valuestring);
			} else if (cJSON_IsBool(kv)) {
				tc_copy(s->value, sizeof(s->value), cJSON_IsTrue(kv) ? "1" : "0");
			} else if (cJSON_IsNumber(kv)) {
				snprintf(s->value, sizeof(s->value), "%.17g", kv->valuedouble);
			} else {
				continue; // objects / arrays / null are not settings
			}
			out->setting_count++;
		}
	}

	// Class-private fields; the hook may reject the row.
	if (cls != NULL && cls->read_extra != NULL) {
		cls->read_extra(cls, plugin, TARGET_COMPONENT_RAW_MANIFEST_PLUGIN, out);
		if (!tc_usable(out)) {
			cJSON_Delete(root);
			return false;
		}
	}

	cJSON_Delete(root);

	if (id[0] == '\0') {
		target_component_mark_malformed(out, "required field 'plugin.id' missing");
		return false;
	}
	if (out->binary_path[0] == '\0') {
		target_component_mark_malformed(out, "required field 'plugin.binary_path' missing");
		return false;
	}

	tc_entry_finish(out);
	return true;
}


/*
 *
 * Platform-specific discovery + binary loading.
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static void
tc_w2u(const wchar_t *src, char *dst, int dst_size)
{
	if (dst_size <= 0) {
		return;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL) <= 0) {
		dst[0] = '\0';
	}
}

static bool
tc_u2w(const char *src, wchar_t *dst, int dst_chars)
{
	if (dst_chars <= 0) {
		return false;
	}
	if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_chars) <= 0) {
		dst[0] = L'\0';
		return false;
	}
	return true;
}

//! REG_SZ / REG_EXPAND_SZ (expanded) value as UTF-8; false when absent.
static bool
tc_reg_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[TARGET_COMPONENT_PATH_MAX];
	DWORD bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, wbuf, &bytes) != ERROR_SUCCESS) {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
		return false;
	}
	tc_w2u(wbuf, dst, dst_size);
	return true;
}

static bool
tc_reg_dword(HKEY key, const wchar_t *name, DWORD *out)
{
	DWORD v = 0;
	DWORD bytes = sizeof(v);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_DWORD, NULL, &v, &bytes) != ERROR_SUCCESS) {
		return false;
	}
	*out = v;
	return true;
}

//! The universal options contract (ADR-054 §2.7): `<row>\Settings\<KEY>` values.
static void
tc_reg_settings(const struct target_component_class *cls, HKEY key, struct target_component_entry *e)
{
	HKEY sk;
	if (RegOpenKeyExW(key, L"Settings", 0, KEY_READ, &sk) != ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0;; i++) {
		wchar_t name[64];
		DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
		DWORD type = 0;
		BYTE data[256 * sizeof(wchar_t)];
		DWORD data_bytes = sizeof(data) - sizeof(wchar_t);
		memset(data, 0, sizeof(data));
		LSTATUS rc = RegEnumValueW(sk, i, name, &name_len, NULL, &type, data, &data_bytes);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			continue; // too long a name / value: skip that one, keep going
		}
		if (e->setting_count >= TARGET_COMPONENT_MAX_SETTINGS) {
			U_LOG_I("%s: %s: more than %d settings — truncating.", tc_tag(cls), e->id,
			        TARGET_COMPONENT_MAX_SETTINGS);
			break;
		}
		struct target_component_setting *s = &e->settings[e->setting_count];
		if (type == REG_SZ || type == REG_EXPAND_SZ) {
			tc_w2u((const wchar_t *)data, s->value, (int)sizeof(s->value));
		} else if (type == REG_DWORD && data_bytes == sizeof(DWORD)) {
			DWORD v;
			memcpy(&v, data, sizeof(v));
			snprintf(s->value, sizeof(s->value), "%lu", (unsigned long)v);
		} else {
			continue;
		}
		tc_w2u(name, s->key, (int)sizeof(s->key));
		e->setting_count++;
	}
	RegCloseKey(sk);
}

//! Read one `<root>\<id>` row into @p e (initialised here).
static void
tc_read_row(const struct target_component_class *cls,
            HKEY root,
            const wchar_t *subkey,
            const char *root_label,
            struct target_component_entry *e)
{
	target_component_entry_init(e);
	tc_w2u(subkey, e->id, (int)sizeof(e->id));
	snprintf(e->source, sizeof(e->source), "%s\\%s", root_label, e->id);

	HKEY sub;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
		target_component_mark_malformed(e, "registry subkey could not be opened");
		return;
	}

	bool have_binary = tc_reg_string(sub, L"Binary", e->binary_path, (int)sizeof(e->binary_path));
	tc_reg_string(sub, L"DisplayName", e->display_name, (int)sizeof(e->display_name));
	tc_reg_string(sub, L"Vendor", e->vendor, (int)sizeof(e->vendor));
	tc_reg_string(sub, L"Version", e->version, (int)sizeof(e->version));

	DWORD v = 0;
	if (tc_reg_dword(sub, L"ProbeOrder", &v)) {
		e->probe_order = (uint32_t)v;
	}
	if (tc_reg_dword(sub, L"Enabled", &v)) {
		e->enabled = v != 0;
	}
	tc_reg_settings(cls, sub, e);

	if (!have_binary || e->binary_path[0] == '\0') {
		target_component_mark_malformed(e, "missing required 'Binary' value");
	}
	if (cls != NULL && cls->read_extra != NULL) {
		cls->read_extra(cls, (const void *)sub, TARGET_COMPONENT_RAW_REGISTRY_KEY, e);
	}
	RegCloseKey(sub);

	tc_entry_finish(e);
}

static int
tc_enumerate_registry(const struct target_component_class *cls,
                      HKEY hive,
                      const char *subkey,
                      const char *label,
                      struct target_component_entry *out,
                      int max)
{
	wchar_t wsub[256];
	if (!tc_u2w(subkey, wsub, (int)(sizeof(wsub) / sizeof(wsub[0])))) {
		return 0;
	}
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(hive, wsub, 0, KEY_READ | KEY_WOW64_64KEY, &root);
	if (rc != ERROR_SUCCESS) {
		U_LOG_I("%s: registry root %s absent (rc=%ld) — no %ss to try.", tc_tag(cls), label, rc,
		        cls->name != NULL ? cls->name : "component");
		return 0;
	}

	int count = 0;
	for (DWORD index = 0;; index++) {
		wchar_t name[256];
		DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
		LSTATUS er = RegEnumKeyExW(root, index, name, &name_len, NULL, NULL, NULL, NULL);
		if (er == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (er != ERROR_SUCCESS) {
			U_LOG_W("%s: RegEnumKeyEx failed (rc=%ld) at index %lu — stopping.", tc_tag(cls), er,
			        (unsigned long)index);
			break;
		}
		if (count >= max) {
			U_LOG_W("%s: more than %d registered %ss — truncating.", tc_tag(cls), max,
			        cls->name != NULL ? cls->name : "component");
			break;
		}
		// Policy subkeys (root values) may live beside the rows.
		if (cls->is_reserved_subkey != NULL) {
			char name_utf8[256];
			tc_w2u(name, name_utf8, (int)sizeof(name_utf8));
			if (cls->is_reserved_subkey(name_utf8)) {
				continue;
			}
		}
		tc_read_row(cls, root, name, label, &out[count++]);
	}
	RegCloseKey(root);
	return count;
}

int
target_component_enumerate_registry(const struct target_component_class *cls,
                                    bool current_user,
                                    const char *subkey,
                                    struct target_component_entry *out,
                                    int max)
{
	if (cls == NULL || subkey == NULL || out == NULL || max <= 0) {
		return 0;
	}
	char label[300];
	snprintf(label, sizeof(label), "%s\\%s", current_user ? "HKCU" : "HKLM", subkey);
	return tc_enumerate_registry(cls, current_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, subkey, label, out,
	                             max);
}

static int
tc_discover(const struct target_component_class *cls, struct target_component_entry *out, int max)
{
	if (cls->registry_root == NULL) {
		return 0;
	}
	char label[300];
	snprintf(label, sizeof(label), "HKLM\\%s", cls->registry_root);
	return tc_enumerate_registry(cls, HKEY_LOCAL_MACHINE, cls->registry_root, label, out, max);
}

enum target_component_load_result
target_component_load_binary(const struct target_component_class *cls,
                             const struct target_component_entry *e,
                             void **out_module,
                             void **out_entrypoint,
                             uint32_t *out_os_error,
                             char *reason,
                             size_t reason_cap)
{
	if (out_module != NULL) {
		*out_module = NULL;
	}
	if (out_entrypoint != NULL) {
		*out_entrypoint = NULL;
	}
	if (out_os_error != NULL) {
		*out_os_error = 0;
	}
	if (reason != NULL && reason_cap > 0) {
		reason[0] = '\0';
	}
	if (cls == NULL || e == NULL || out_module == NULL || out_entrypoint == NULL) {
		return TARGET_COMPONENT_LOAD_FAILED;
	}

	wchar_t wpath[TARGET_COMPONENT_PATH_MAX];
	if (!tc_u2w(e->binary_path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
		if (reason != NULL) {
			snprintf(reason, reason_cap, "binary path is not valid UTF-8");
		}
		return TARGET_COMPONENT_LOAD_FAILED;
	}
	// LOAD_WITH_ALTERED_SEARCH_PATH needs backslashes: with forward slashes the
	// "directory of the DLL" search rule silently does not apply.
	for (wchar_t *p = wpath; *p != L'\0'; p++) {
		if (*p == L'/') {
			*p = L'\\';
		}
	}

	// ADR-045: an orphan registration (Binary deleted, key left behind) is a
	// fact to skip, not an error to repeat.
	if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
		DWORD gle = GetLastError();
		if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) {
			U_LOG_W("%s:   %s: registered Binary '%s' does not exist — skipping (orphan registration).",
			        tc_tag(cls), e->id, e->binary_path);
			if (out_os_error != NULL) {
				*out_os_error = (uint32_t)gle;
			}
			if (reason != NULL) {
				snprintf(reason, reason_cap, "registered Binary does not exist (orphan registration)");
			}
			return TARGET_COMPONENT_LOAD_BINARY_MISSING;
		}
	}

	// #952: refuse a build-tree/worktree DLL path (the #943 footgun) unless
	// DXR_ALLOW_DEV_PLUGIN_PATHS is set; warn on other non-install paths.
	if (target_plugin_path_check(wpath, e->id, cls->name != NULL ? cls->name : "plugin") ==
	    TARGET_PLUGIN_PATH_REFUSED) {
		if (reason != NULL) {
			snprintf(reason, reason_cap,
			         "binary lives in a build tree (set DXR_ALLOW_DEV_PLUGIN_PATHS=1 to allow, #952)");
		}
		return TARGET_COMPONENT_LOAD_PATH_REFUSED;
	}

	// Components import DisplayXRClient.dll for the aux surface (#328), and
	// LoadLibrary runs host DLL-notification code (#434).
	target_plugin_preload_runtime_core_dll();
	target_plugin_sanitized_preload(wpath);

	// One-shot breadcrumb: a host-side crash during the load below leaves
	// this as the last line in the per-app log, naming the in-flight binary.
	U_LOG_W("%s:   %s: loading %s binary %s", tc_tag(cls), e->id, cls->name != NULL ? cls->name : "plug-in",
	        e->binary_path);

	HMODULE dll = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (dll == NULL) {
		DWORD gle = GetLastError();
		// The binary exists (checked above), so ERROR_MOD_NOT_FOUND means a
		// library IT imports is missing — typically the vendor platform the
		// component should resolve lazily (ADR-045 rule 1).
		const bool dep = gle == ERROR_MOD_NOT_FOUND;
		U_LOG_W("%s:   %s: LoadLibrary(%s) failed (err=%lu)%s.", tc_tag(cls), e->id, e->binary_path, gle,
		        dep ? " — a library the binary imports is missing (vendor platform not installed, or not "
		              "resolvable from this process)"
		            : "");
		if (out_os_error != NULL) {
			*out_os_error = (uint32_t)gle;
		}
		if (reason != NULL) {
			snprintf(reason, reason_cap, "%s (err=%lu)",
			         dep ? "dependency missing: a library the binary imports was not found"
			             : "LoadLibrary failed",
			         gle);
		}
		return dep ? TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING : TARGET_COMPONENT_LOAD_FAILED;
	}

	void *fn = (void *)GetProcAddress(dll, cls->entrypoint);
	if (fn == NULL) {
		U_LOG_W("%s:   %s: missing entry point '%s' — skipping.", tc_tag(cls), e->id, cls->entrypoint);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "no '%s' export (not a %s)", cls->entrypoint,
			         cls->name != NULL ? cls->name : "component of this class");
		}
		FreeLibrary(dll);
		return TARGET_COMPONENT_LOAD_NO_ENTRY_POINT;
	}

	*out_module = (void *)dll;
	*out_entrypoint = fn;
	return TARGET_COMPONENT_LOAD_OK;
}

void
target_component_unload(void *module)
{
	if (module != NULL) {
		FreeLibrary((HMODULE)module);
	}
}

#else /* POSIX: Android, macOS, desktop Linux */

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>

#if defined(XRT_OS_ANDROID)

/*!
 * Android: `<prefix><NNN>_<id>.so`. ProbeOrder lives in the file name because
 * `jniLibs` JSON is unreadable at load time; id / display name come from the
 * file name until negotiate.
 */
static int
tc_scan_dir(
    const struct target_component_class *cls, const char *dir, struct target_component_entry *out, int count, int max)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		return count;
	}
	struct target_component_entry *tmp =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*tmp));
	if (tmp == NULL) {
		closedir(d);
		return count;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < TARGET_COMPONENT_MAX_ENTRIES) {
		uint32_t order = 0;
		char id[64];
		if (!target_plugin_parse_android_filename(de->d_name, cls->android_prefix, &order, id, sizeof(id))) {
			continue;
		}
		struct target_component_entry *e = &tmp[n++];
		target_component_entry_init(e);
		tc_copy(e->id, sizeof(e->id), id);
		e->probe_order = order;
		snprintf(e->binary_path, sizeof(e->binary_path), "%s/%s", dir, de->d_name);
		tc_copy(e->source, sizeof(e->source), e->binary_path);
		if (cls->read_extra != NULL) {
			cls->read_extra(cls, NULL, TARGET_COMPONENT_RAW_NONE, e);
		}
		tc_entry_finish(e);
	}
	closedir(d);
	// Deterministic within a directory: readdir order is not.
	target_component_sort(cls, tmp, n);
	count = target_component_merge(cls, out, count, max, tmp, n);
	free(tmp);
	return count;
}

static int
tc_discover(const struct target_component_class *cls, struct target_component_entry *out, int max)
{
	if (cls->android_prefix == NULL) {
		return 0; // this class ships no Android components (yet)
	}
	int count = 0;
	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		count = tc_scan_dir(cls, override, out, count, max);
	}
	Dl_info info;
	if (dladdr((void *)&tc_discover, &info) != 0 && info.dli_fname != NULL) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s", info.dli_fname);
		char *slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			count = tc_scan_dir(cls, dir, out, count, max);
		}
	}
	return count;
}

#else /* macOS / desktop Linux */

static char *
tc_read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char *buf = NULL;
	long len = -1;
	if (fseek(f, 0, SEEK_END) == 0) {
		len = ftell(f);
	}
	if (len > 0 && len <= 64 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
		buf = (char *)malloc((size_t)len + 1);
		if (buf != NULL) {
			if (fread(buf, 1, (size_t)len, f) == (size_t)len) {
				buf[len] = '\0';
			} else {
				free(buf);
				buf = NULL;
			}
		}
	}
	fclose(f);
	return buf;
}

static int
tc_compare_source(const void *a, const void *b)
{
	return strcmp(((const struct target_component_entry *)a)->source,
	              ((const struct target_component_entry *)b)->source);
}

static int
tc_scan_dir(
    const struct target_component_class *cls, const char *dir, struct target_component_entry *out, int count, int max)
{
	if (cls->manifest_kind == TARGET_PLUGIN_MANIFEST_NONE) {
		return count; // this class has no manifest discovery
	}
	DIR *d = opendir(dir);
	if (d == NULL) {
		return count;
	}
	struct target_component_entry *tmp =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*tmp));
	if (tmp == NULL) {
		closedir(d);
		return count;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < TARGET_COMPONENT_MAX_ENTRIES) {
		// The suffix is the router: every class's manifests share this dir.
		if (target_plugin_classify_manifest(de->d_name) != cls->manifest_kind) {
			continue;
		}
		char path[TARGET_COMPONENT_PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		char *text = tc_read_file(path);
		struct target_component_entry *e = &tmp[n++];
		if (!target_component_parse_manifest_json(cls, text, path, e)) {
			U_LOG_I("%s: %s: %s.", tc_tag(cls), path, e->reason);
		}
		free(text);
	}
	closedir(d);
	// Within a root the first FILE NAME wins a duplicate id, not readdir order.
	if (n > 1) {
		qsort(tmp, (size_t)n, sizeof(tmp[0]), tc_compare_source);
	}
	count = target_component_merge(cls, out, count, max, tmp, n);
	free(tmp);
	return count;
}

static int
tc_discover(const struct target_component_class *cls, struct target_component_entry *out, int max)
{
	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));
	int count = 0;
	for (int r = 0; r < n_roots; r++) {
		count = tc_scan_dir(cls, roots[r], out, count, max);
	}
	return count;
}

bool
target_component_user_dir(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
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

#endif /* Android vs desktop POSIX */

enum target_component_load_result
target_component_load_binary(const struct target_component_class *cls,
                             const struct target_component_entry *e,
                             void **out_module,
                             void **out_entrypoint,
                             uint32_t *out_os_error,
                             char *reason,
                             size_t reason_cap)
{
	if (out_module != NULL) {
		*out_module = NULL;
	}
	if (out_entrypoint != NULL) {
		*out_entrypoint = NULL;
	}
	if (out_os_error != NULL) {
		*out_os_error = 0;
	}
	if (reason != NULL && reason_cap > 0) {
		reason[0] = '\0';
	}
	if (cls == NULL || e == NULL || out_module == NULL || out_entrypoint == NULL) {
		return TARGET_COMPONENT_LOAD_FAILED;
	}

	U_LOG_W("%s:   %s: loading %s binary %s", tc_tag(cls), e->id, cls->name != NULL ? cls->name : "plug-in",
	        e->binary_path);

	/* RTLD_LOCAL keeps the component's symbols private; aux symbols resolve
	 * via the runtime library already in the process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		const char *err = dlerror();
		struct stat st;
		const bool missing = stat(e->binary_path, &st) != 0;
		U_LOG_W("%s:   %s: dlopen(%s) failed: %s.", tc_tag(cls), e->id, e->binary_path,
		        err != NULL ? err : "?");
		if (reason != NULL) {
			snprintf(reason, reason_cap, "%s",
			         missing ? "registered binary does not exist (orphan registration)" : "dlopen failed");
			if (!missing && err != NULL) {
				snprintf(reason, reason_cap, "dlopen failed: %s", err);
			}
		}
		return missing ? TARGET_COMPONENT_LOAD_BINARY_MISSING : TARGET_COMPONENT_LOAD_FAILED;
	}
	dlerror();
	void *sym = dlsym(handle, cls->entrypoint);
	const char *err = dlerror();
	if (sym == NULL || err != NULL) {
		U_LOG_W("%s:   %s: missing entry point '%s' (%s) — skipping.", tc_tag(cls), e->id, cls->entrypoint,
		        err != NULL ? err : "null");
		if (reason != NULL) {
			snprintf(reason, reason_cap, "no '%s' export (not a %s)", cls->entrypoint,
			         cls->name != NULL ? cls->name : "component of this class");
		}
		dlclose(handle);
		return TARGET_COMPONENT_LOAD_NO_ENTRY_POINT;
	}
	*out_module = handle;
	*out_entrypoint = sym;
	return TARGET_COMPONENT_LOAD_OK;
}

void
target_component_unload(void *module)
{
	if (module != NULL) {
		dlclose(module);
	}
}

#endif /* platform */


/*
 *
 * Public iface (every platform).
 *
 */

int
target_component_enumerate(const struct target_component_class *cls, struct target_component_entry *out, int max)
{
	if (cls == NULL || out == NULL || max <= 0) {
		return 0;
	}
	int n = tc_discover(cls, out, max);
	target_component_sort(cls, out, n);
	U_LOG_I("%s: %d registered %s(s).", tc_tag(cls), n, cls->name != NULL ? cls->name : "component");
	return n;
}

const char *
target_component_load_result_str(enum target_component_load_result r)
{
	switch (r) {
	case TARGET_COMPONENT_LOAD_OK: return "ok";
	case TARGET_COMPONENT_LOAD_BINARY_MISSING: return "binary-missing";
	case TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING: return "dependency-missing";
	case TARGET_COMPONENT_LOAD_FAILED: return "load-failed";
	case TARGET_COMPONENT_LOAD_PATH_REFUSED: return "path-refused";
	case TARGET_COMPONENT_LOAD_NO_ENTRY_POINT: return "no-entrypoint";
	default: return "unknown";
	}
}

enum target_component_abi_verdict
target_component_abi_gate(const struct target_component_class *cls,
                          const char *id,
                          xrt_result_t xret,
                          const void *iface,
                          uint32_t plugin_version,
                          char *reason,
                          size_t reason_cap)
{
	const char *tag = tc_tag(cls);
	const char *pid = id != NULL ? id : "?";
	const uint32_t want = cls != NULL ? cls->abi_current : 0;
	if (reason != NULL && reason_cap > 0) {
		reason[0] = '\0';
	}

	if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
		U_LOG_W("%s:   %s: negotiate declined — skipping.", tag, pid);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "negotiate declined (XRT_ERROR_PROBER_NOT_SUPPORTED)");
		}
		return TARGET_COMPONENT_ABI_DECLINED;
	}

	/* ADR-020 rule 3: never dispatch through a mismatched vtable. Checked
	 * before the result code, because a component of another major may well
	 * refuse our version with an error AND report its own. */
	if (plugin_version != 0 && plugin_version != want) {
		U_LOG_E(
		    "%s:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the %s must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    tag, pid, plugin_version, want, cls != NULL && cls->name != NULL ? cls->name : "plug-in");
		if (reason != NULL) {
			snprintf(reason, reason_cap,
			         "ABI mismatch: plug-in reports v%u, runtime expects v%u (rebuild it)", plugin_version,
			         want);
		}
		return TARGET_COMPONENT_ABI_MISMATCH;
	}

	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("%s:   %s: negotiate returned %d (iface=%p) — skipping.", tag, pid, (int)xret, iface);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "negotiate returned %d (iface=%s)", (int)xret,
			         iface != NULL ? "set" : "NULL");
		}
		return TARGET_COMPONENT_ABI_NEGOTIATE_FAILED;
	}

	if (plugin_version != want) {
		// Success with no version written: same rule, the vtable is unknown.
		U_LOG_E(
		    "%s:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the %s must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    tag, pid, plugin_version, want, cls != NULL && cls->name != NULL ? cls->name : "plug-in");
		if (reason != NULL) {
			snprintf(reason, reason_cap,
			         "ABI mismatch: plug-in reports v%u, runtime expects v%u (rebuild it)", plugin_version,
			         want);
		}
		return TARGET_COMPONENT_ABI_MISMATCH;
	}

	return TARGET_COMPONENT_ABI_OK;
}
