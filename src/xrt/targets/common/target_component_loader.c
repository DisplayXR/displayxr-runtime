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

#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include "util/u_camera_consent.h"
#include "util/u_json.h"
#include "util/u_logging.h"
#include "util/u_setting.h"
#include "util/u_sha256.h"

#include "os/os_threading.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


/*
 *
 * One open handle on a component binary (ADR-054 N4b review).
 *
 * The provenance gate hashes, verifies and loads the SAME file: the binary is
 * opened once, hashed and (Windows) Authenticode-verified through that handle,
 * and the handle stays open until the load returns. On Windows the share mode
 * (FILE_SHARE_READ only) refuses every writer, renamer and deleter meanwhile;
 * on POSIX the path is re-checked against the hashed descriptor's identity
 * before and after dlopen. Definitions live with the provenance code below.
 *
 */

struct tc_bin
{
#ifdef XRT_OS_WINDOWS
	HANDLE h;
#else
	int fd;
#endif
	bool open;
	bool missing;      //!< the open failed because the file does not exist
	uint32_t os_error; //!< GetLastError() / errno of a failed open
	uint64_t dev;      //!< volume serial number / st_dev
	uint64_t ino;      //!< file index / st_ino
	uint64_t size;
	uint64_t mtime; //!< FILETIME ticks / nanoseconds since the epoch
};

static bool
tc_bin_open(const char *path, struct tc_bin *b);
static void
tc_bin_close(struct tc_bin *b);
#ifndef XRT_OS_WINDOWS
static bool
tc_bin_path_is_bin(const char *path, const struct tc_bin *b);
#endif
static bool
tc_prov_cache_lock(void);
static void
tc_prov_cache_unlock(void);
static void
tc_prov_evaluate(struct target_component_entry *e, struct tc_bin *b);
static enum target_component_load_result
tc_prov_gate(const struct target_component_class *cls,
             struct target_component_entry *e,
             struct tc_bin *b,
             bool quiet,
             char *reason,
             size_t reason_cap);


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

bool
target_component_path_is_absolute(const char *path, bool windows_rules)
{
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	const bool sep0 = path[0] == '/' || path[0] == '\\';
	const bool sep1 = path[1] == '/' || path[1] == '\\';
	// `X:\` / `X:/` — a drive letter alone (`X:foo`) is drive-relative.
	const bool drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	                   path[1] == ':' && (path[2] == '\\' || path[2] == '/');
	// `\\server\share`, `\\?\…`, `\\.\…`.
	const bool unc = sep0 && sep1;
	if (windows_rules) {
		// A single leading separator is relative to the current drive.
		return drive || unc;
	}
	// Manifest paths: POSIX-absolute, or a Windows absolute path (a test or
	// cross-platform manifest), never anything cwd- or search-relative.
	return path[0] == '/' || drive || unc;
}

static bool
tc_usable(const struct target_component_entry *e)
{
	return (e->flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0;
}

//! ASCII case-insensitive equality (setting keys are registry value names).
static bool
tc_ci_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	for (;; a++, b++) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca >= 'a' && ca <= 'z') {
			ca -= 'a' - 'A';
		}
		if (cb >= 'a' && cb <= 'z') {
			cb -= 'a' - 'A';
		}
		if (ca != cb) {
			return false;
		}
		if (ca == '\0') {
			return true;
		}
	}
}

static void
tc_upper_copy(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t i = 0;
	for (; src != NULL && src[i] != '\0' && i + 1 < cap; i++) {
		const unsigned char c = (unsigned char)src[i];
		dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : (char)c;
	}
	dst[i] = '\0';
}

/*
 * ADR-054 N4b, PR B: the enforce default. -1 = follow the platform knob
 * (always, outside tests), 0 = audit, 1 = enforce. Only tests set it: the
 * knob lives in HKLM / /etc, which a unit test must not touch, and the
 * build-tree stubs they load are unsigned and unlisted by construction.
 */
static int g_trust_enforce_override = -1;

void
target_component_trust_set_enforce_override(int mode)
{
	g_trust_enforce_override = mode < 0 ? -1 : (mode != 0 ? 1 : 0);
}

bool
target_component_trust_policy(bool knob_present, bool knob_zero, bool platform_default)
{
	// An explicit knob always wins: 0 = audit, anything else = enforce.
	// Without one, the platform's default applies (Windows enforces since
	// PR B; POSIX still audits: no signer check, unsigned tarball lane).
	if (knob_present) {
		return !knob_zero;
	}
	return platform_default;
}

static const char *
tc_tag(const struct target_component_class *cls)
{
	return (cls != NULL && cls->log_tag != NULL) ? cls->log_tag : "component loader";
}

/*!
 * WARN by default, INFO under TARGET_COMPONENT_LOAD_FLAG_QUIET: a class that
 * re-attempts the same binary on world events pays one WARN per process for a
 * binary that keeps failing the same way, not one per retry (ADR-045).
 */
#define TC_LOG_LOAD(quiet, ...)                                                                                        \
	do {                                                                                                           \
		if (quiet) {                                                                                           \
			U_LOG_I(__VA_ARGS__);                                                                          \
		} else {                                                                                               \
			U_LOG_W(__VA_ARGS__);                                                                          \
		}                                                                                                      \
	} while (0)


/*
 *
 * Routing (ADR-053) — platform-neutral, pure. Moved here from the
 * display-processor loader (ADR-054 N2); the names stay.
 *
 */

enum target_plugin_manifest_kind
target_plugin_classify_manifest(const char *filename)
{
	static const char json_suffix[] = ".json";
	static const char ip_suffix[] = "-input-provider.json";
	static const char cp_suffix[] = "-conversion-provider.json";
	/*
	 * ADR-053 P4 model packs (`<pack>.models.json`) are not manifests. They
	 * ship beside the conversion provider that lists them in `packs[]` — in
	 * this same discovery dir — so the router must skip them: read as a DP
	 * manifest each one was logged malformed ("unsupported
	 * file_format_version") on every enumeration and took one of
	 * TARGET_COMPONENT_MAX_ENTRIES.
	 */
	static const char pack_suffix[] = ".models.json";

	if (filename == NULL) {
		return TARGET_PLUGIN_MANIFEST_NONE;
	}
	const size_t n = strlen(filename);
	const size_t json_len = sizeof(json_suffix) - 1;
	const size_t ip_len = sizeof(ip_suffix) - 1;
	const size_t cp_len = sizeof(cp_suffix) - 1;
	const size_t pack_len = sizeof(pack_suffix) - 1;

	/* A bare suffix is not a manifest: there must be a name in front. */
	if (n <= json_len || strcmp(filename + n - json_len, json_suffix) != 0) {
		return TARGET_PLUGIN_MANIFEST_NONE;
	}
	if (n >= pack_len && strcmp(filename + n - pack_len, pack_suffix) == 0) {
		return TARGET_PLUGIN_MANIFEST_NONE;
	}
	if (n >= ip_len && strcmp(filename + n - ip_len, ip_suffix) == 0) {
		return n > ip_len ? TARGET_PLUGIN_MANIFEST_INPUT : TARGET_PLUGIN_MANIFEST_NONE;
	}
	if (n >= cp_len && strcmp(filename + n - cp_len, cp_suffix) == 0) {
		return n > cp_len ? TARGET_PLUGIN_MANIFEST_CONVERSION : TARGET_PLUGIN_MANIFEST_NONE;
	}
	return TARGET_PLUGIN_MANIFEST_DISPLAY;
}

const char *
target_component_class_id_str(uint32_t class_id)
{
	switch (class_id) {
	case TARGET_COMPONENT_CLASS_DISPLAY: return "display";
	case TARGET_COMPONENT_CLASS_INPUT: return "input";
	case TARGET_COMPONENT_CLASS_CONVERSION: return "conversion";
	default: return "none";
	}
}

bool
target_plugin_parse_android_filename(
    const char *name, const char *prefix, uint32_t *out_order, char *out_id, size_t id_size)
{
	if (name == NULL || prefix == NULL || out_order == NULL || out_id == NULL || id_size == 0) {
		return false;
	}
	const size_t prefix_len = strlen(prefix);
	const size_t n = strlen(name);

	/* Min plausible: "<prefix>NNN_X.so" = prefix + 3 + 1 + 1 + 3 chars. */
	if (prefix_len == 0 || n < prefix_len + 8 || strncmp(name, prefix, prefix_len) != 0) {
		return false;
	}
	if (strcmp(name + n - 3, ".so") != 0) {
		return false;
	}
	for (size_t i = 0; i < 3; i++) {
		if (!(name[prefix_len + i] >= '0' && name[prefix_len + i] <= '9')) {
			return false;
		}
	}
	if (name[prefix_len + 3] != '_') {
		return false;
	}

	const size_t id_start = prefix_len + 4;
	const size_t id_len = n - 3 - id_start;
	if (id_len == 0 || id_len >= id_size) {
		return false;
	}

	*out_order = (uint32_t)((name[prefix_len + 0] - '0') * 100 + (name[prefix_len + 1] - '0') * 10 +
	                        (name[prefix_len + 2] - '0'));
	memcpy(out_id, name + id_start, id_len);
	out_id[id_len] = '\0';
	return true;
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
	// Nothing resolves a manifest's binary_path against the manifest's own
	// directory, so a relative one would be resolved by dlopen against the
	// process cwd / library search path: never what the manifest meant.
	if (!target_component_path_is_absolute(out->binary_path, false)) {
		target_component_mark_malformed(out, "'plugin.binary_path' is not an absolute path");
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
	} else if (!target_component_path_is_absolute(e->binary_path, true)) {
		// LoadLibrary would resolve it against the host's search order.
		target_component_mark_malformed(e, "'Binary' is not an absolute path");
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


/*
 *
 * Provenance hooks (Windows): Authenticode via the consent store's verifier
 * (ADR-043 already answers "is this exe validly signed, by whom"), the
 * admin-only allowlist as value names under HKLM\Software\DisplayXR\Trust\Allowlist,
 * token elevation, and the Trust\Enforce knob.
 *
 */

static bool
tc_prov_signed(const char *path, const struct tc_bin *b, char *signer, size_t cap)
{
	// Verified through the handle the caller hashed and will load from. A
	// valid chain with no leaf CN is signed "(unknown subject)", not
	// unverified (the consent store's helper needs a name; this gate does not).
	return u_authenticode_file_signer(path, b->open ? (void *)b->h : NULL, signer, cap);
}

static bool
tc_prov_allowlisted(const char *sha256_hex)
{
	if (sha256_hex == NULL || sha256_hex[0] == '\0') {
		return false;
	}
	wchar_t wname[80];
	if (!tc_u2w(sha256_hex, wname, (int)(sizeof(wname) / sizeof(wname[0])))) {
		return false;
	}
	// Any value type counts; the data is a note for humans (which plug-in,
	// who wrote it). The 64-bit view is where installers and the dev script
	// write.
	DWORD type = 0;
	DWORD bytes = 0;
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Trust\\Allowlist", wname,
	                          RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, &type, NULL, &bytes);
	return rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA;
}

static bool
tc_prov_platform_trusted(const char *path, const struct tc_bin *b)
{
	(void)path; // No platform rule on Windows: a path is not a provenance.
	(void)b;
	return false;
}

bool
target_component_owner_chain_ok(
    const char *resolved_path, uint32_t owner_uid, const char *stop_dir, char *why, size_t why_cap)
{
	(void)resolved_path;
	(void)owner_uid;
	(void)stop_dir;
	if (why != NULL && why_cap > 0) {
		snprintf(why, why_cap, "no POSIX ownership rule on Windows");
	}
	return false;
}

bool
target_component_platform_trusted_path(const char *path, char *why, size_t why_cap)
{
	(void)path;
	if (why != NULL && why_cap > 0) {
		snprintf(why, why_cap, "no platform rule on Windows: a path is not a provenance");
	}
	return false;
}

bool
target_component_process_elevated(void)
{
	TOKEN_ELEVATION e = {0};
	DWORD len = 0;
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return false;
	}
	BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
	CloseHandle(tok);
	return ok && e.TokenIsElevated != 0;
}

bool
target_component_dev_override_set(void)
{
	// Same reading as the #952 path guard: the Win32 environment, not the
	// CRT copy a DLL may hold.
	wchar_t buf[8];
	DWORD n = GetEnvironmentVariableW(L"DXR_ALLOW_DEV_PLUGIN_PATHS", buf, 8);
	return n > 0 && !(n == 1 && buf[0] == L'0');
}

bool
target_component_trust_enforced(void)
{
	if (g_trust_enforce_override >= 0) {
		return g_trust_enforce_override != 0;
	}
	DWORD v = 0;
	DWORD bytes = sizeof(v);
	const bool present = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Trust", L"Enforce",
	                                  RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &v,
	                                  &bytes) == ERROR_SUCCESS;
	// PR B: enforcing is the Windows default; `Enforce` = 0 (DWORD) is the
	// one-release escape hatch, `Enforce` = 1 says it explicitly.
	return target_component_trust_policy(present, present && v == 0, true);
}

/*!
 * Pin the runtime core DLL (DisplayXRClient.dll) into the process by absolute
 * path before any component is loaded. Runs once.
 *
 * Component DLLs statically import DisplayXRClient.dll, which ships in the
 * runtime install dir. That dir is intentionally NOT on PATH (issue #104),
 * and the per-component LOAD_WITH_ALTERED_SEARCH_PATH searches only the
 * component's own dir + System32 + cwd + PATH — none of which hold
 * DisplayXRClient.dll except cwd when it happens to equal the runtime dir. So
 * at logon (Run-key launches with cwd=System32) and when the workspace
 * controller spawns the service, that import fails with ERROR_MOD_NOT_FOUND
 * (126) and every load aborts → no display processor → the service exits
 * (issue #328).
 *
 * Pre-loading DisplayXRClient.dll by absolute path here pins it into the
 * process by base name; the loader then satisfies each component's import
 * from the already-resident module with no disk search, independent of
 * cwd/PATH. The per-component load deliberately keeps
 * LOAD_WITH_ALTERED_SEARCH_PATH so vendor side DLLs (e.g. the SR platform at
 * C:\Program Files\LeiaSR\Platform\bin) still resolve via PATH.
 *
 * No process-wide search-order mutation (cf. target_dll_init.c) — this is
 * the surgical option and leaves PATH resolution intact for vendors.
 */
void
target_plugin_preload_runtime_core_dll(void)
{
	static int done = 0;
	if (done) {
		return;
	}
	done = 1;

	// Directory of whatever module this loader code lives in — the
	// service exe or DisplayXRClient.dll, both of which sit in the
	// runtime install dir. UNCHANGED_REFCOUNT: we only want its path.
	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&target_plugin_preload_runtime_core_dll, &self)) {
		U_LOG_W("plugin loader: GetModuleHandleEx(self) failed (err=%lu) — cannot locate runtime dir to "
		        "pre-load DisplayXRClient.dll.",
		        GetLastError());
		return;
	}

	wchar_t modpath[MAX_PATH];
	DWORD len = GetModuleFileNameW(self, modpath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		U_LOG_W("plugin loader: GetModuleFileName(self) failed/truncated — skipping DisplayXRClient.dll "
		        "pre-load.");
		return;
	}

	// Strip the module filename to leave the runtime dir.
	wchar_t *slash = wcsrchr(modpath, L'\\');
	if (slash == NULL) {
		return;
	}
	*slash = L'\0';

	wchar_t corepath[MAX_PATH];
	if (swprintf_s(corepath, MAX_PATH, L"%s\\DisplayXRClient.dll", modpath) <= 0) {
		return;
	}

	// If it is already resident (in-process app target, where the host
	// loaded it via the OpenXR loader) this just bumps the refcount and
	// the component import binds to it. Otherwise it loads now so the
	// import resolves. LOAD_WITH_ALTERED_SEARCH_PATH so the core DLL's
	// own siblings (cjson.dll, pthreadVC3.dll) resolve from the runtime
	// dir rather than the host exe dir.
	if (LoadLibraryExW(corepath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL) {
		U_LOG_W("plugin loader: pre-load of %ls failed (err=%lu); plug-in loads may fail when cwd is not "
		        "the runtime dir.",
		        corepath, GetLastError());
	} else {
		U_LOG_I("plugin loader: pinned runtime core DLL for plug-in import resolution: %ls", corepath);
	}
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

//! Is the file @p dll was mapped from the file @p b holds open (volume serial + file index)?
static bool
tc_module_is_bin(HMODULE dll, const struct tc_bin *b)
{
	wchar_t mpath[TARGET_COMPONENT_PATH_MAX];
	DWORD n = GetModuleFileNameW(dll, mpath, (DWORD)(sizeof(mpath) / sizeof(mpath[0])));
	if (n == 0 || n >= (DWORD)(sizeof(mpath) / sizeof(mpath[0]))) {
		return false;
	}
	// Attributes only: not subject to the share mode the gate's handle holds.
	HANDLE h = CreateFileW(mpath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	BY_HANDLE_FILE_INFORMATION fi;
	const bool ok = GetFileInformationByHandle(h, &fi) != 0;
	CloseHandle(h);
	return ok && (uint64_t)fi.dwVolumeSerialNumber == b->dev &&
	       (((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow) == b->ino;
}

enum target_component_load_result
target_component_load_binary_ex(const struct target_component_class *cls,
                                struct target_component_entry *e,
                                uint32_t flags,
                                void **out_module,
                                void **out_entrypoint,
                                uint32_t *out_os_error,
                                char *reason,
                                size_t reason_cap)
{
	const bool quiet = (flags & TARGET_COMPONENT_LOAD_FLAG_QUIET) != 0;
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
	// Discovery marks such rows MALFORMED; this catches a caller-built entry.
	if (!target_component_path_is_absolute(e->binary_path, true)) {
		if (reason != NULL) {
			snprintf(reason, reason_cap, "binary path is not absolute");
		}
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
			TC_LOG_LOAD(quiet, "%s:   %s: registered Binary '%s' does not exist — skipping (orphan registration).",
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

	// ADR-054 N4b: provenance, on ONE handle. The binary is opened here with
	// FILE_SHARE_READ only, hashed and Authenticode-verified through that
	// handle, and the handle is held until LoadLibraryExW has returned: while
	// it is open nobody can open the file for writing, rename it or delete
	// it, so the bytes LoadLibrary maps are the bytes the gate judged.
	struct tc_bin bin;
	if (!tc_bin_open(e->binary_path, &bin) && bin.missing) {
		TC_LOG_LOAD(quiet, "%s:   %s: registered Binary '%s' does not exist — skipping (orphan registration).",
		            tc_tag(cls), e->id, e->binary_path);
		if (out_os_error != NULL) {
			*out_os_error = bin.os_error;
		}
		if (reason != NULL) {
			snprintf(reason, reason_cap, "registered Binary does not exist (orphan registration)");
		}
		return TARGET_COMPONENT_LOAD_BINARY_MISSING;
	}
	enum target_component_load_result gate = tc_prov_gate(cls, e, &bin, quiet, reason, reason_cap);
	if (gate != TARGET_COMPONENT_LOAD_OK) {
		tc_bin_close(&bin);
		return gate;
	}

	// Components import DisplayXRClient.dll for the aux surface (#328), and
	// LoadLibrary runs host DLL-notification code (#434).
	target_plugin_preload_runtime_core_dll();
	target_plugin_sanitized_preload(wpath);

	// One-shot breadcrumb: a host-side crash during the load below leaves
	// this as the last line in the per-app log, naming the in-flight binary.
	TC_LOG_LOAD(quiet, "%s:   %s: loading %s binary %s", tc_tag(cls), e->id, cls->name != NULL ? cls->name : "plug-in",
	        e->binary_path);

	HMODULE dll = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	const DWORD load_gle = dll == NULL ? GetLastError() : 0; // before CloseHandle can touch it
	if (dll != NULL && bin.open && !tc_module_is_bin(dll, &bin)) {
		// The share mode makes this unreachable for the file itself; a
		// renamed parent directory is what is left. Same answer either way.
		U_LOG_E(
		    "%s:   %s: REFUSING %s — the module LoadLibrary mapped is not the file that was verified "
		    "(ADR-054 N4b).",
		    tc_tag(cls), e->id, e->binary_path);
		FreeLibrary(dll);
		tc_bin_close(&bin);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "untrusted: the binary changed between verification and load");
		}
		return TARGET_COMPONENT_LOAD_UNTRUSTED;
	}
	tc_bin_close(&bin);
	if (dll == NULL) {
		DWORD gle = load_gle;
		// The binary exists (checked above), so ERROR_MOD_NOT_FOUND means a
		// library IT imports is missing — typically the vendor platform the
		// component should resolve lazily (ADR-045 rule 1).
		const bool dep = gle == ERROR_MOD_NOT_FOUND;
		TC_LOG_LOAD(quiet, "%s:   %s: LoadLibrary(%s) failed (err=%lu)%s.", tc_tag(cls), e->id, e->binary_path, gle,
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
		TC_LOG_LOAD(quiet, "%s:   %s: missing entry point '%s' — skipping.", tc_tag(cls), e->id, cls->entrypoint);
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


/*
 *
 * Component settings store (Windows): `<hive>\<root>\<id>\Settings\<KEY>`.
 * HKLM is the 64-bit view (where installers and the rows live); HKCU is
 * read as-is, like the N3a/N3b `Enabled` overlay.
 *
 */

//! HKLM `<root>\Policy\Lock` = 1: the machine's values win over the user's (kiosk rule).
static bool
tc_policy_lock(const struct target_component_class *cls)
{
	if (cls == NULL || cls->registry_root == NULL) {
		return false;
	}
	wchar_t wroot[256];
	if (!tc_u2w(cls->registry_root, wroot, (int)(sizeof(wroot) / sizeof(wroot[0])))) {
		return false;
	}
	wchar_t wpolicy[320];
	swprintf(wpolicy, sizeof(wpolicy) / sizeof(wpolicy[0]), L"%ls\\Policy", wroot);
	DWORD lock = 0;
	DWORD bytes = sizeof(lock);
	return RegGetValueW(HKEY_LOCAL_MACHINE, wpolicy, L"Lock", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &lock,
	                    &bytes) == ERROR_SUCCESS &&
	       lock == 1;
}

//! `<root>\<id>\Settings` as a wide path; false when a part does not convert / fit.
static bool
tc_settings_key_path(const struct target_component_class *cls, const char *id, wchar_t *out, size_t cap_chars)
{
	if (cls == NULL || cls->registry_root == NULL || id == NULL || id[0] == '\0' || out == NULL || cap_chars == 0) {
		return false;
	}
	wchar_t wroot[256];
	wchar_t wid[TARGET_COMPONENT_SETTING_KEY_MAX];
	if (!tc_u2w(cls->registry_root, wroot, (int)(sizeof(wroot) / sizeof(wroot[0]))) ||
	    !tc_u2w(id, wid, (int)(sizeof(wid) / sizeof(wid[0])))) {
		return false;
	}
	const int n = swprintf(out, cap_chars, L"%ls\\%ls\\Settings", wroot, wid);
	return n > 0 && (size_t)n < cap_chars;
}

static bool
tc_store_lock(const struct target_component_class *cls)
{
	return tc_policy_lock(cls);
}

//! One tier's value: the user (HKCU) or machine (HKLM, 64-bit view) `Settings\<KEY>`.
static bool
tc_store_read(
    const struct target_component_class *cls, const char *id, const char *key, bool user, char *buf, size_t cap)
{
	wchar_t wpath[400];
	wchar_t wkey[TARGET_COMPONENT_SETTING_KEY_MAX];
	if (!tc_settings_key_path(cls, id, wpath, sizeof(wpath) / sizeof(wpath[0])) ||
	    !tc_u2w(key, wkey, (int)(sizeof(wkey) / sizeof(wkey[0])))) {
		return false;
	}
	const HKEY hive = user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	const DWORD flags = RRF_RT_ANY | (user ? 0u : RRF_SUBKEY_WOW6464KEY);
	DWORD type = 0;
	BYTE data[TARGET_COMPONENT_SETTING_VALUE_MAX * sizeof(wchar_t)];
	DWORD bytes = sizeof(data) - sizeof(wchar_t);
	memset(data, 0, sizeof(data));
	if (RegGetValueW(hive, wpath, wkey, flags, &type, data, &bytes) != ERROR_SUCCESS) {
		return false;
	}
	if (type == REG_SZ || type == REG_EXPAND_SZ) {
		tc_w2u((const wchar_t *)data, buf, (int)cap);
		return true;
	}
	if (type == REG_DWORD && bytes == sizeof(DWORD)) {
		DWORD v;
		memcpy(&v, data, sizeof(v));
		snprintf(buf, cap, "%lu", (unsigned long)v);
		return true;
	}
	return false;
}

static enum target_component_store_result
tc_store_write(
    const struct target_component_class *cls, const char *id, const char *key, const char *value, bool machine)
{
	wchar_t wpath[400];
	wchar_t wkey[TARGET_COMPONENT_SETTING_KEY_MAX];
	if (!tc_settings_key_path(cls, id, wpath, sizeof(wpath) / sizeof(wpath[0])) ||
	    !tc_u2w(key, wkey, (int)(sizeof(wkey) / sizeof(wkey[0])))) {
		return TARGET_COMPONENT_STORE_FAILED;
	}
	const HKEY hive = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	const REGSAM sam = KEY_SET_VALUE | (machine ? KEY_WOW64_64KEY : 0);
	HKEY k = NULL;
	LSTATUS rc;
	if (value != NULL) {
		rc = RegCreateKeyExW(hive, wpath, 0, NULL, 0, sam, NULL, &k, NULL);
	} else {
		rc = RegOpenKeyExW(hive, wpath, 0, sam, &k);
		if (rc == ERROR_FILE_NOT_FOUND) {
			return TARGET_COMPONENT_STORE_OK; // nothing to clear
		}
	}
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? TARGET_COMPONENT_STORE_DENIED : TARGET_COMPONENT_STORE_FAILED;
	}
	if (value != NULL) {
		wchar_t wval[TARGET_COMPONENT_SETTING_VALUE_MAX];
		if (!tc_u2w(value, wval, (int)(sizeof(wval) / sizeof(wval[0])))) {
			RegCloseKey(k);
			return TARGET_COMPONENT_STORE_FAILED;
		}
		rc = RegSetValueExW(k, wkey, 0, REG_SZ, (const BYTE *)wval,
		                    (DWORD)((wcslen(wval) + 1) * sizeof(wchar_t)));
	} else {
		rc = RegDeleteValueW(k, wkey);
		if (rc == ERROR_FILE_NOT_FOUND) {
			rc = ERROR_SUCCESS;
		}
	}
	RegCloseKey(k);
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? TARGET_COMPONENT_STORE_DENIED : TARGET_COMPONENT_STORE_FAILED;
	}
	return TARGET_COMPONENT_STORE_OK;
}

//! Every value name under one tier's `Settings` key.
static int
tc_store_keys(const struct target_component_class *cls,
              const char *id,
              bool user,
              char keys[][TARGET_COMPONENT_SETTING_KEY_MAX],
              int max)
{
	wchar_t wpath[400];
	if (!tc_settings_key_path(cls, id, wpath, sizeof(wpath) / sizeof(wpath[0]))) {
		return 0;
	}
	const HKEY hive = user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	HKEY k = NULL;
	if (RegOpenKeyExW(hive, wpath, 0, KEY_READ | (user ? 0 : KEY_WOW64_64KEY), &k) != ERROR_SUCCESS) {
		return 0;
	}
	int n = 0;
	for (DWORD i = 0; n < max; i++) {
		wchar_t name[TARGET_COMPONENT_SETTING_KEY_MAX];
		DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
		DWORD type = 0;
		LSTATUS rc = RegEnumValueW(k, i, name, &name_len, NULL, &type, NULL, NULL);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_DWORD)) {
			continue;
		}
		tc_w2u(name, keys[n], TARGET_COMPONENT_SETTING_KEY_MAX);
		if (keys[n][0] != '\0') {
			n++;
		}
	}
	RegCloseKey(k);
	return n;
}

static bool
tc_store_path(const struct target_component_class *cls, const char *id, bool machine, char *out, size_t cap)
{
	if (cls == NULL || cls->registry_root == NULL || id == NULL) {
		return false;
	}
	const int n = snprintf(out, cap, "%s\\%s\\%s\\Settings", machine ? "HKLM" : "HKCU", cls->registry_root, id);
	return n > 0 && (size_t)n < cap;
}

static int
tc_store_manifest_schema(const struct target_component_class *cls,
                         const char *id,
                         struct target_component_setting_desc *out,
                         int max)
{
	(void)cls;
	(void)id;
	(void)out;
	(void)max;
	return 0; // registry discovery carries no manifest; the binary declares (describe_settings)
}

#else /* POSIX: Android, macOS, desktop Linux */

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>

/*
 *
 * Provenance hooks (POSIX). No Authenticode; the admin-only allowlist is
 * /etc/displayxr/trust/allowlist (one hex digest per line, `#` comments),
 * honoured only when it and every parent directory are root-owned and not
 * group/world-writable; platform trust is the package manager's (the RESOLVED
 * path under /usr, /opt or the .pkg payload, the file and every parent
 * directory root-owned and not group/world-writable) and, on Android, the
 * APK signature that already covers everything inside it.
 *
 */

#include <unistd.h>

static bool
tc_prov_signed(const char *path, const struct tc_bin *b, char *signer, size_t cap)
{
	(void)path;
	(void)b;
	if (cap > 0) {
		signer[0] = '\0';
	}
	return false;
}

bool
target_component_owner_chain_ok(
    const char *resolved_path, uint32_t owner_uid, const char *stop_dir, char *why, size_t why_cap)
{
	if (why != NULL && why_cap > 0) {
		why[0] = '\0';
	}
	if (resolved_path == NULL || resolved_path[0] != '/') {
		if (why != NULL) {
			snprintf(why, why_cap, "not an absolute resolved path");
		}
		return false;
	}
	const char *stop = (stop_dir != NULL && stop_dir[0] != '\0') ? stop_dir : "/";
	char cur[PATH_MAX];
	tc_copy(cur, sizeof(cur), resolved_path);
	for (;;) {
		struct stat st;
		// lstat: a resolved path has no symlinks left; one appearing now is a swap.
		if (lstat(cur, &st) != 0) {
			if (why != NULL) {
				snprintf(why, why_cap, "cannot stat %s", cur);
			}
			return false;
		}
		if (S_ISLNK(st.st_mode)) {
			if (why != NULL) {
				snprintf(why, why_cap, "%s is a symlink", cur);
			}
			return false;
		}
		if ((uint32_t)st.st_uid != owner_uid) {
			if (why != NULL) {
				snprintf(why, why_cap, "%s is owned by uid %lu, not %lu", cur, (unsigned long)st.st_uid,
				         (unsigned long)owner_uid);
			}
			return false;
		}
		if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
			if (why != NULL) {
				snprintf(why, why_cap, "%s is group- or world-writable", cur);
			}
			return false;
		}
		if (strcmp(cur, stop) == 0 || strcmp(cur, "/") == 0) {
			return true;
		}
		char *slash = strrchr(cur, '/');
		if (slash == NULL) {
			return false;
		}
		if (slash == cur) {
			cur[1] = '\0'; // parent is the root
		} else {
			*slash = '\0';
		}
	}
}

bool
target_component_platform_trusted_path(const char *path, char *why, size_t why_cap)
{
	if (why != NULL && why_cap > 0) {
		why[0] = '\0';
	}
#if defined(XRT_OS_ANDROID)
	// Everything the runtime can dlopen here came out of a signed APK.
	(void)path;
	return true;
#else
	char resolved[PATH_MAX];
	if (path == NULL || realpath(path, resolved) == NULL) {
		if (why != NULL) {
			snprintf(why, why_cap, "path does not resolve");
		}
		return false;
	}
	// The prefix is checked on the RESOLVED path: `/usr/../home/x/lib.so` or
	// a /usr symlink into a user directory is not under /usr. /usr and /opt
	// (the .deb, /usr/local), plus the macOS .pkg's root-owned payload under
	// /Library/Application Support/DisplayXR.
	if (strncmp(resolved, "/usr/", 5) != 0 && strncmp(resolved, "/opt/", 5) != 0 &&
	    strncmp(resolved, "/Library/Application Support/DisplayXR/", 39) != 0) {
		if (why != NULL) {
			snprintf(why, why_cap, "%s is not under /usr, /opt or the .pkg payload", resolved);
		}
		return false;
	}
	// Root-owned and not group/world-writable: the file AND every directory
	// up to /, so nobody but root can have replaced or re-pointed any of it.
	return target_component_owner_chain_ok(resolved, 0, "/", why, why_cap);
#endif
}

#if !defined(XRT_OS_ANDROID)
/*!
 * An admin-only trust file (the allowlist, the enforce knob) counts only if
 * it and every directory above it are root-owned and not group/world
 * writable; otherwise it is ignored, with one WARN per file per process.
 * Absent = false, silently. @p resolved receives the path to open.
 */
static bool
tc_admin_file_trusted(const char *path, bool *warned, char *resolved, size_t resolved_cap)
{
	char tmp[PATH_MAX];
	if (realpath(path, tmp) == NULL) {
		return false; // absent: nothing to trust or ignore
	}
	char why[PATH_MAX + 64];
	if (!target_component_owner_chain_ok(tmp, 0, "/", why, sizeof(why))) {
		bool say = false;
		if (tc_prov_cache_lock()) {
			say = !*warned;
			*warned = true;
			tc_prov_cache_unlock();
		}
		if (say) {
			U_LOG_W(
			    "component loader: IGNORING %s — %s; an admin-only trust file must be root-owned and not "
			    "group/world-writable, with every parent directory the same (ADR-054 N4b).",
			    path, why);
		}
		return false;
	}
	tc_copy(resolved, resolved_cap, tmp);
	return true;
}
#endif

static bool
tc_prov_allowlisted(const char *sha256_hex)
{
	if (sha256_hex == NULL || sha256_hex[0] == '\0') {
		return false;
	}
#if defined(XRT_OS_ANDROID)
	FILE *f = fopen("/etc/displayxr/trust/allowlist", "rb");
#else
	static bool warned = false;
	char resolved[PATH_MAX];
	if (!tc_admin_file_trusted("/etc/displayxr/trust/allowlist", &warned, resolved, sizeof(resolved))) {
		return false;
	}
	FILE *f = fopen(resolved, "rb");
#endif
	if (f == NULL) {
		return false;
	}
	bool found = false;
	char line[256];
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '#' || *p == '\n' || *p == '\0') {
			continue;
		}
		size_t n = 0;
		while (p[n] != '\0' && p[n] != '\n' && p[n] != '\r' && p[n] != ' ' && p[n] != '\t' && p[n] != '#') {
			n++;
		}
		if (n == 64) {
			found = true;
			for (size_t i = 0; i < 64; i++) {
				int a = p[i], b = sha256_hex[i];
				if (a >= 'A' && a <= 'F') {
					a = a - 'A' + 'a';
				}
				if (b >= 'A' && b <= 'F') {
					b = b - 'A' + 'a';
				}
				if (a != b) {
					found = false;
					break;
				}
			}
		}
	}
	fclose(f);
	return found;
}

static bool
tc_prov_platform_trusted(const char *path, const struct tc_bin *b)
{
#if defined(XRT_OS_ANDROID)
	// Everything the runtime can dlopen here came out of a signed APK.
	(void)path;
	(void)b;
	return true;
#else
	if (path == NULL || !b->open) {
		return false;
	}
	// The resolved path must name the descriptor that was hashed and will be
	// loaded, and pass the location + ownership rule.
	char resolved[PATH_MAX];
	struct stat st;
	if (realpath(path, resolved) == NULL || stat(resolved, &st) != 0 || (uint64_t)st.st_dev != b->dev ||
	    (uint64_t)st.st_ino != b->ino) {
		return false;
	}
	return target_component_platform_trusted_path(resolved, NULL, 0);
#endif
}

bool
target_component_process_elevated(void)
{
	return geteuid() == 0;
}

bool
target_component_dev_override_set(void)
{
	const char *v = getenv("DXR_ALLOW_DEV_PLUGIN_PATHS");
	return v != NULL && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

bool
target_component_trust_enforced(void)
{
	if (g_trust_enforce_override >= 0) {
		return g_trust_enforce_override != 0;
	}
#if defined(XRT_OS_ANDROID)
	// Everything dlopen'd here is platform-trusted (inside the APK); the
	// gate has nothing to refuse.
	return false;
#else
	// The file's first non-blank byte decides: "0" opts out, anything else
	// (an empty file included) enforces. Absent = audit: POSIX has no signer
	// check and the tarball's user-level install is unsigned and unlisted,
	// so the default stays audit here until those lanes allowlist (PR B
	// flipped Windows only). A knob file a non-root user could have written
	// is ignored (WARN once) — the platform default applies.
	static bool warned = false;
	char resolved[PATH_MAX];
	FILE *f = NULL;
	if (tc_admin_file_trusted("/etc/displayxr/trust/enforce", &warned, resolved, sizeof(resolved))) {
		f = fopen(resolved, "rb");
	}
	if (f == NULL) {
		return target_component_trust_policy(false, false, false);
	}
	int c = fgetc(f);
	while (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
		c = fgetc(f);
	}
	fclose(f);
	return target_component_trust_policy(true, c == '0', false);
#endif
}

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

/*
 * Component settings store (Android): file-name discovery carries no
 * manifest and there is no per-user dir, so the environment
 * (`debug.xrt.DXR_<CLASS>_<ID>_<KEY>` / the process environment) is the only
 * tier. Writes are refused.
 */
static bool
tc_store_lock(const struct target_component_class *cls)
{
	(void)cls;
	return false;
}

static bool
tc_store_read(
    const struct target_component_class *cls, const char *id, const char *key, bool user, char *buf, size_t cap)
{
	(void)cls;
	(void)id;
	(void)key;
	(void)user;
	(void)buf;
	(void)cap;
	return false;
}

static enum target_component_store_result
tc_store_write(
    const struct target_component_class *cls, const char *id, const char *key, const char *value, bool machine)
{
	(void)cls;
	(void)id;
	(void)key;
	(void)value;
	(void)machine;
	return TARGET_COMPONENT_STORE_UNSUPPORTED;
}

static int
tc_store_keys(const struct target_component_class *cls,
              const char *id,
              bool user,
              char keys[][TARGET_COMPONENT_SETTING_KEY_MAX],
              int max)
{
	(void)cls;
	(void)id;
	(void)user;
	(void)keys;
	(void)max;
	return 0;
}

static bool
tc_store_path(const struct target_component_class *cls, const char *id, bool machine, char *out, size_t cap)
{
	(void)cls;
	(void)id;
	(void)machine;
	if (out != NULL && cap > 0) {
		out[0] = '\0';
	}
	return false;
}

static int
tc_store_manifest_schema(const struct target_component_class *cls,
                         const char *id,
                         struct target_component_setting_desc *out,
                         int max)
{
	(void)cls;
	(void)id;
	(void)out;
	(void)max;
	return 0;
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

static void
tc_append_root(char roots[][PATH_MAX], int max_roots, int *n_roots, const char *path)
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

/*!
 * Discovery contract: see docs/specs/runtime/plugin-discovery.md §3.
 *
 * Roots searched, in priority order:
 *   1. $XRT_PLUGIN_SEARCH_PATH — dev override, colon-separated list
 *   2. macOS: ~/Library/Application Support/DisplayXR/DisplayProcessors/
 *      Linux: $XDG_DATA_HOME/DisplayXR/DisplayProcessors/
 *             (defaults to ~/.local/share/DisplayXR/DisplayProcessors/)
 *   3. macOS: /Library/Application Support/DisplayXR/DisplayProcessors/
 *      Linux: /usr/lib/displayxr/plugins (the .deb's packaged dir, #781),
 *             /usr/local/share/displayxr/DisplayProcessors/
 *             /usr/share/displayxr/DisplayProcessors/
 *
 * Per-user shadows system: roots are walked in priority order and the first
 * occurrence of an id wins (@ref target_component_merge).
 */
int
target_plugin_build_discovery_roots(char roots[][PATH_MAX], int max_roots)
{
	int n_roots = 0;

	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		/* Colon-separated list, like PATH. */
		const char *p = override;
		while (*p && n_roots < max_roots) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			if (len > 0 && len < PATH_MAX) {
				char buf[PATH_MAX];
				memcpy(buf, p, len);
				buf[len] = '\0';
				tc_append_root(roots, max_roots, &n_roots, buf);
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
		snprintf(user_root, sizeof(user_root), "%s/Library/Application Support/DisplayXR/DisplayProcessors",
		         home);
		tc_append_root(roots, max_roots, &n_roots, user_root);
	}
	/* System-wide root for `.pkg`-style installs that run as root and
	 * write to /Library/Application Support/ rather than ~. Per-user
	 * entries above shadow system-wide entries. Issue #274. */
	tc_append_root(roots, max_roots, &n_roots, "/Library/Application Support/DisplayXR/DisplayProcessors");
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/DisplayXR/DisplayProcessors", xdg);
		tc_append_root(roots, max_roots, &n_roots, user_root);
	} else if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/.local/share/DisplayXR/DisplayProcessors", home);
		tc_append_root(roots, max_roots, &n_roots, user_root);
	}
	/* Built-in default packaged plug-in dir — the Linux `.deb` drops the
	 * sim-display .so + its `200-sim-display.json` manifest here, so an
	 * installed box needs NO XRT_PLUGIN_SEARCH_PATH (#781). The env override
	 * above is appended first, so a dev build still wins when it is set. This
	 * is the POSIX analogue of the Windows DisplayProcessors registry root. */
	tc_append_root(roots, max_roots, &n_roots, "/usr/lib/displayxr/plugins");
	tc_append_root(roots, max_roots, &n_roots, "/usr/local/share/displayxr/DisplayProcessors");
	tc_append_root(roots, max_roots, &n_roots, "/usr/share/displayxr/DisplayProcessors");
#endif

	return n_roots;
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


/*
 *
 * Component settings store (desktop POSIX): the manifest's `settings` object
 * is the machine tier (edited by hand; `--machine` writes are refused), the
 * per-user `<id>.settings.json` next to `preferred` is the user tier.
 *
 */

//! The registered, usable entry @p id of @p cls, re-enumerated; false when absent.
static bool
tc_posix_find_entry(const struct target_component_class *cls, const char *id, struct target_component_entry *out)
{
	if (cls == NULL || id == NULL || out == NULL) {
		return false;
	}
	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return false;
	}
	const int n = tc_discover(cls, entries, TARGET_COMPONENT_MAX_ENTRIES);
	bool found = false;
	for (int i = 0; i < n; i++) {
		if (tc_usable(&entries[i]) && strcmp(entries[i].id, id) == 0) {
			*out = entries[i];
			found = true;
			break;
		}
	}
	free(entries);
	return found;
}

static bool
tc_user_settings_path(const char *id, char *out, size_t cap)
{
	char dir[PATH_MAX];
	if (id == NULL || id[0] == '\0' || !target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	const int n = snprintf(out, cap, "%s/%s.settings.json", dir, id);
	return n > 0 && (size_t)n < cap;
}

static bool
tc_store_lock(const struct target_component_class *cls)
{
	(void)cls; // no kiosk policy store on POSIX: the per-user file always wins
	return false;
}

static bool
tc_store_read(
    const struct target_component_class *cls, const char *id, const char *key, bool user, char *buf, size_t cap)
{
	if (user) {
		char path[PATH_MAX];
		if (!tc_user_settings_path(id, path, sizeof(path))) {
			return false;
		}
		char *text = tc_read_file(path);
		if (text == NULL) {
			return false;
		}
		const bool ok = target_component_user_settings_json_lookup(text, key, buf, cap);
		free(text);
		return ok;
	}
	struct target_component_entry *e = (struct target_component_entry *)calloc(1, sizeof(*e));
	if (e == NULL) {
		return false;
	}
	bool ok = false;
	if (tc_posix_find_entry(cls, id, e)) {
		for (uint32_t i = 0; i < e->setting_count && i < TARGET_COMPONENT_MAX_SETTINGS; i++) {
			if (tc_ci_equal(e->settings[i].key, key)) {
				tc_copy(buf, cap, e->settings[i].value);
				ok = true;
				break;
			}
		}
	}
	free(e);
	return ok;
}

static enum target_component_store_result
tc_store_write(
    const struct target_component_class *cls, const char *id, const char *key, const char *value, bool machine)
{
	(void)cls;
	if (machine) {
		return TARGET_COMPONENT_STORE_UNSUPPORTED; // the manifest's "settings" object is the machine store
	}
	char path[PATH_MAX];
	if (!tc_user_settings_path(id, path, sizeof(path))) {
		return TARGET_COMPONENT_STORE_UNSUPPORTED;
	}
	char *text = tc_read_file(path);
	cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
	free(text);
	if (root == NULL && value == NULL) {
		return TARGET_COMPONENT_STORE_OK; // nothing to clear (absent or unreadable file)
	}
	if (root != NULL && !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		root = NULL;
	}
	if (root == NULL) {
		root = cJSON_CreateObject();
		if (root == NULL) {
			return TARGET_COMPONENT_STORE_FAILED;
		}
	}
	// Keys match case-insensitively: drop every spelling before adding ours.
	for (;;) {
		cJSON *victim = NULL;
		cJSON *kv = NULL;
		cJSON_ArrayForEach(kv, root)
		{
			if (kv->string != NULL && tc_ci_equal(kv->string, key)) {
				victim = kv;
				break;
			}
		}
		if (victim == NULL) {
			break;
		}
		cJSON_DetachItemViaPointer(root, victim);
		cJSON_Delete(victim);
	}
	if (value != NULL) {
		char upper[TARGET_COMPONENT_SETTING_KEY_MAX];
		tc_upper_copy(upper, sizeof(upper), key);
		if (cJSON_AddStringToObject(root, upper, value) == NULL) {
			cJSON_Delete(root);
			return TARGET_COMPONENT_STORE_FAILED;
		}
	}
	enum target_component_store_result r = TARGET_COMPONENT_STORE_OK;
	if (cJSON_GetArraySize(root) == 0) {
		if (remove(path) != 0 && errno != ENOENT) {
			r = errno == EACCES ? TARGET_COMPONENT_STORE_DENIED : TARGET_COMPONENT_STORE_FAILED;
		}
	} else {
		char dir[PATH_MAX];
		if (target_component_user_dir(dir, sizeof(dir))) {
			(void)mkdir(dir, 0755); // best-effort, single level (as `preferred`)
		}
		char *printed = cJSON_Print(root);
		FILE *f = printed != NULL ? fopen(path, "wb") : NULL;
		if (f == NULL) {
			r = errno == EACCES ? TARGET_COMPONENT_STORE_DENIED : TARGET_COMPONENT_STORE_FAILED;
		} else {
			fprintf(f, "%s\n", printed);
			fclose(f);
		}
		cJSON_free(printed);
	}
	cJSON_Delete(root);
	return r;
}

static int
tc_store_keys(const struct target_component_class *cls,
              const char *id,
              bool user,
              char keys[][TARGET_COMPONENT_SETTING_KEY_MAX],
              int max)
{
	int n = 0;
	if (user) {
		char path[PATH_MAX];
		if (!tc_user_settings_path(id, path, sizeof(path))) {
			return 0;
		}
		char *text = tc_read_file(path);
		if (text == NULL) {
			return 0;
		}
		cJSON *root = cJSON_Parse(text);
		free(text);
		if (root == NULL) {
			return 0;
		}
		const cJSON *kv = NULL;
		cJSON_ArrayForEach(kv, root)
		{
			if (n >= max || kv->string == NULL ||
			    !(cJSON_IsString(kv) || cJSON_IsNumber(kv) || cJSON_IsBool(kv))) {
				continue;
			}
			tc_copy(keys[n++], TARGET_COMPONENT_SETTING_KEY_MAX, kv->string);
		}
		cJSON_Delete(root);
		return n;
	}
	struct target_component_entry *e = (struct target_component_entry *)calloc(1, sizeof(*e));
	if (e == NULL) {
		return 0;
	}
	if (tc_posix_find_entry(cls, id, e)) {
		for (uint32_t i = 0; i < e->setting_count && i < TARGET_COMPONENT_MAX_SETTINGS && n < max; i++) {
			tc_copy(keys[n++], TARGET_COMPONENT_SETTING_KEY_MAX, e->settings[i].key);
		}
	}
	free(e);
	return n;
}

static bool
tc_store_path(const struct target_component_class *cls, const char *id, bool machine, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (!machine) {
		return tc_user_settings_path(id, out, cap);
	}
	struct target_component_entry *e = (struct target_component_entry *)calloc(1, sizeof(*e));
	if (e == NULL) {
		return false;
	}
	const bool found = tc_posix_find_entry(cls, id, e);
	if (found) {
		tc_copy(out, cap, e->source);
	}
	free(e);
	return found;
}

static int
tc_store_manifest_schema(const struct target_component_class *cls,
                         const char *id,
                         struct target_component_setting_desc *out,
                         int max)
{
	struct target_component_entry *e = (struct target_component_entry *)calloc(1, sizeof(*e));
	if (e == NULL) {
		return 0;
	}
	int n = 0;
	char *text = NULL;
	if (tc_posix_find_entry(cls, id, e)) {
		text = tc_read_file(e->source);
	}
	free(e);
	if (text == NULL) {
		return 0;
	}
	cJSON *root = cJSON_Parse(text);
	free(text);
	if (root == NULL) {
		return 0;
	}
	const cJSON *plugin = u_json_get(root, "plugin");
	const cJSON *schema = plugin != NULL ? u_json_get(plugin, "settings_schema") : NULL;
	if (schema != NULL && cJSON_IsArray(schema)) {
		const cJSON *item = NULL;
		cJSON_ArrayForEach(item, schema)
		{
			if (n >= max || !cJSON_IsObject(item)) {
				continue;
			}
			struct target_component_setting_desc *d = &out[n];
			memset(d, 0, sizeof(*d));
			tc_json_str(item, "key", d->key, sizeof(d->key));
			if (!target_component_setting_key_valid(d->key)) {
				U_LOG_I("%s: %s: settings_schema entry with an invalid key '%s' — skipped.",
				        tc_tag(cls), id, d->key);
				continue;
			}
			bool dup = false;
			for (int i = 0; i < n; i++) {
				if (tc_ci_equal(out[i].key, d->key)) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			tc_upper_copy(d->key, sizeof(d->key), d->key);
			tc_json_str(item, "type", d->type, sizeof(d->type));
			tc_json_str(item, "default", d->default_value, sizeof(d->default_value));
			tc_json_str(item, "help", d->help, sizeof(d->help));
			n++;
		}
	}
	cJSON_Delete(root);
	return n;
}

#endif /* Android vs desktop POSIX */

enum target_component_load_result
target_component_load_binary_ex(const struct target_component_class *cls,
                                struct target_component_entry *e,
                                uint32_t flags,
                                void **out_module,
                                void **out_entrypoint,
                                uint32_t *out_os_error,
                                char *reason,
                                size_t reason_cap)
{
	const bool quiet = (flags & TARGET_COMPONENT_LOAD_FLAG_QUIET) != 0;
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
	// Discovery marks such rows MALFORMED; this catches a caller-built entry
	// (a bare name would make dlopen search the library path).
	if (!target_component_path_is_absolute(e->binary_path, false)) {
		if (reason != NULL) {
			snprintf(reason, reason_cap, "binary path is not absolute");
		}
		return TARGET_COMPONENT_LOAD_FAILED;
	}

	// ADR-054 N4b: provenance, on ONE descriptor. The binary is opened here,
	// hashed through that descriptor, and its identity (st_dev, st_ino, size,
	// mtime) pinned; the path is re-checked against it just before and just
	// after dlopen. (dlopen("/proc/self/fd/N") would load the descriptor
	// itself on Linux, but it makes the module's name /proc/self/fd/N, which
	// breaks $ORIGIN and the dladdr() own-directory lookups plug-ins use to
	// find their resources — sim_display's ONNX model, the Leia plug-in.)
	struct tc_bin bin;
	if (!tc_bin_open(e->binary_path, &bin) && bin.missing) {
		TC_LOG_LOAD(quiet, "%s:   %s: registered binary '%s' does not exist — skipping (orphan registration).",
		            tc_tag(cls), e->id, e->binary_path);
		if (out_os_error != NULL) {
			*out_os_error = bin.os_error;
		}
		if (reason != NULL) {
			snprintf(reason, reason_cap, "registered binary does not exist (orphan registration)");
		}
		return TARGET_COMPONENT_LOAD_BINARY_MISSING;
	}
	enum target_component_load_result gate = tc_prov_gate(cls, e, &bin, quiet, reason, reason_cap);
	if (gate == TARGET_COMPONENT_LOAD_OK && bin.open && !tc_bin_path_is_bin(e->binary_path, &bin)) {
		U_LOG_E("%s:   %s: REFUSING %s — the path no longer names the file that was verified (ADR-054 N4b).",
		        tc_tag(cls), e->id, e->binary_path);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "untrusted: the binary changed between verification and load");
		}
		gate = TARGET_COMPONENT_LOAD_UNTRUSTED;
	}
	if (gate != TARGET_COMPONENT_LOAD_OK) {
		tc_bin_close(&bin);
		return gate;
	}

	TC_LOG_LOAD(quiet, "%s:   %s: loading %s binary %s", tc_tag(cls), e->id, cls->name != NULL ? cls->name : "plug-in",
	        e->binary_path);

	/* RTLD_LOCAL keeps the component's symbols private; aux symbols resolve
	 * via the runtime library already in the process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle != NULL && bin.open && !tc_bin_path_is_bin(e->binary_path, &bin)) {
		// Swapped while dlopen ran: its constructors have run, but nothing
		// gets an entry point out of it.
		U_LOG_E("%s:   %s: REFUSING %s — the file changed while it was being loaded (ADR-054 N4b).",
		        tc_tag(cls), e->id, e->binary_path);
		dlclose(handle);
		tc_bin_close(&bin);
		if (reason != NULL) {
			snprintf(reason, reason_cap, "untrusted: the binary changed between verification and load");
		}
		return TARGET_COMPONENT_LOAD_UNTRUSTED;
	}
	tc_bin_close(&bin);
	if (handle == NULL) {
		const char *err = dlerror();
		struct stat st;
		const bool missing = stat(e->binary_path, &st) != 0;
		TC_LOG_LOAD(quiet, "%s:   %s: dlopen(%s) failed: %s.", tc_tag(cls), e->id, e->binary_path,
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
		TC_LOG_LOAD(quiet, "%s:   %s: missing entry point '%s' (%s) — skipping.", tc_tag(cls), e->id, cls->entrypoint,
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


/*
 *
 * Provenance (ADR-054 N4b) — platform-neutral part.
 *
 */

const char *
target_component_provenance_str(uint32_t p)
{
	switch (p) {
	case TARGET_COMPONENT_PROVENANCE_UNKNOWN: return "unknown";
	case TARGET_COMPONENT_PROVENANCE_SIGNED: return "signed";
	case TARGET_COMPONENT_PROVENANCE_ALLOWLISTED: return "allowlisted";
	case TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED: return "platform-trusted";
	case TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE: return "dev-override";
	case TARGET_COMPONENT_PROVENANCE_UNVERIFIED: return "unverified";
	default: return "unknown";
	}
}

uint32_t
target_component_provenance_classify(bool signed_ok,
                                     bool allowlisted,
                                     bool platform_trusted,
                                     bool dev_override,
                                     bool elevated,
                                     const char *sha256_hex,
                                     char *reason,
                                     size_t reason_cap)
{
	if (reason != NULL && reason_cap > 0) {
		reason[0] = '\0';
	}
#define TC_REASON(...)                                                                                                 \
	do {                                                                                                           \
		if (reason != NULL && reason_cap > 0) {                                                                \
			snprintf(reason, reason_cap, __VA_ARGS__);                                                     \
		}                                                                                                      \
	} while (0)
	if (signed_ok) {
		TC_REASON("valid Authenticode chain to a trusted root");
		return TARGET_COMPONENT_PROVENANCE_SIGNED;
	}
	if (allowlisted) {
		TC_REASON("SHA-256 is in the admin allowlist");
		return TARGET_COMPONENT_PROVENANCE_ALLOWLISTED;
	}
	if (platform_trusted) {
		TC_REASON("platform-trusted location (package-managed / APK)");
		return TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED;
	}
	const char *hex = (sha256_hex != NULL && sha256_hex[0] != '\0') ? sha256_hex : "<unreadable>";
	if (dev_override) {
		if (elevated) {
			TC_REASON("unsigned, not allowlisted; DXR_ALLOW_DEV_PLUGIN_PATHS refused: process is elevated "
			          "(allowlist it instead: Trust\\Allowlist\\%s)",
			          hex);
			return TARGET_COMPONENT_PROVENANCE_UNVERIFIED;
		}
		TC_REASON("unsigned, not allowlisted; admitted by DXR_ALLOW_DEV_PLUGIN_PATHS on a non-elevated host "
		          "(sha256 %s)",
		          hex);
		return TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE;
	}
	TC_REASON("unsigned and not allowlisted (to allow: Trust\\Allowlist\\%s)", hex);
	return TARGET_COMPONENT_PROVENANCE_UNVERIFIED;
#undef TC_REASON
}

static void
tc_hex_digest(const uint8_t digest[32], char *out_hex)
{
	static const char hexd[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out_hex[i * 2] = hexd[digest[i] >> 4];
		out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
	}
	out_hex[64] = '\0';
}

bool
target_component_sha256_file(const char *path, char *out_hex, size_t cap)
{
	if (out_hex == NULL || cap == 0) {
		return false;
	}
	out_hex[0] = '\0';
	if (path == NULL || cap < 65) {
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	struct u_sha256 st;
	u_sha256_init(&st);
	unsigned char buf[64 * 1024];
	size_t got;
	bool ok = true;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		u_sha256_update(&st, buf, got);
	}
	if (ferror(f)) {
		ok = false;
	}
	fclose(f);
	if (!ok) {
		return false;
	}
	uint8_t digest[32];
	u_sha256_final(&st, digest);
	tc_hex_digest(digest, out_hex);
	return true;
}


/*
 *
 * The open binary (struct tc_bin, declared at the top of the file).
 *
 */

#ifdef XRT_OS_WINDOWS

static bool
tc_bin_open(const char *path, struct tc_bin *b)
{
	memset(b, 0, sizeof(*b));
	b->h = INVALID_HANDLE_VALUE;
	wchar_t wpath[TARGET_COMPONENT_PATH_MAX];
	if (path == NULL || !tc_u2w(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
		return false;
	}
	for (wchar_t *p = wpath; *p != L'\0'; p++) {
		if (*p == L'/') {
			*p = L'\\';
		}
	}
	// FILE_SHARE_READ only: while this handle is open, nobody can open the
	// file for writing, rename it or delete it (and the open fails if someone
	// already holds it for writing — not a state to verify bytes in).
	b->h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (b->h == INVALID_HANDLE_VALUE) {
		const DWORD gle = GetLastError();
		b->os_error = (uint32_t)gle;
		b->missing = gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND;
		return false;
	}
	BY_HANDLE_FILE_INFORMATION fi;
	if (!GetFileInformationByHandle(b->h, &fi)) {
		b->os_error = (uint32_t)GetLastError();
		CloseHandle(b->h);
		b->h = INVALID_HANDLE_VALUE;
		return false;
	}
	b->dev = (uint64_t)fi.dwVolumeSerialNumber;
	b->ino = ((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow;
	b->size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
	b->mtime = ((uint64_t)fi.ftLastWriteTime.dwHighDateTime << 32) | fi.ftLastWriteTime.dwLowDateTime;
	b->open = true;
	return true;
}

static void
tc_bin_close(struct tc_bin *b)
{
	if (b->open && b->h != INVALID_HANDLE_VALUE) {
		CloseHandle(b->h);
	}
	b->h = INVALID_HANDLE_VALUE;
	b->open = false;
}

//! Read up to @p cap bytes at @p offset; returns the count, 0 at EOF, -1 on error.
static int64_t
tc_bin_read_at(struct tc_bin *b, uint64_t offset, void *buf, size_t cap)
{
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xFFFFFFFFu);
	ov.OffsetHigh = (DWORD)(offset >> 32);
	DWORD got = 0;
	if (!ReadFile(b->h, buf, (DWORD)cap, &got, &ov)) {
		return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
	}
	return (int64_t)got;
}

#else /* POSIX */

static uint64_t
tc_stat_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__)
	return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st->st_mtimespec.tv_nsec;
#else
	return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

static bool
tc_bin_open(const char *path, struct tc_bin *b)
{
	memset(b, 0, sizeof(*b));
	b->fd = -1;
	if (path == NULL) {
		return false;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		b->os_error = (uint32_t)errno;
		b->missing = errno == ENOENT || errno == ENOTDIR;
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		b->os_error = (uint32_t)errno;
		close(fd);
		return false;
	}
	b->fd = fd;
	b->dev = (uint64_t)st.st_dev;
	b->ino = (uint64_t)st.st_ino;
	b->size = (uint64_t)st.st_size;
	b->mtime = tc_stat_mtime_ns(&st);
	b->open = true;
	return true;
}

static void
tc_bin_close(struct tc_bin *b)
{
	if (b->open && b->fd >= 0) {
		close(b->fd);
	}
	b->fd = -1;
	b->open = false;
}

static int64_t
tc_bin_read_at(struct tc_bin *b, uint64_t offset, void *buf, size_t cap)
{
	for (;;) {
		ssize_t got = pread(b->fd, buf, cap, (off_t)offset);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		return (int64_t)got;
	}
}

/*!
 * Does @p path still name the file @p b holds (same st_dev, st_ino, size,
 * mtime)? The POSIX load path asks just before and just after dlopen.
 */
static bool
tc_bin_path_is_bin(const char *path, const struct tc_bin *b)
{
	struct stat st;
	if (path == NULL || stat(path, &st) != 0) {
		return false;
	}
	return (uint64_t)st.st_dev == b->dev && (uint64_t)st.st_ino == b->ino && (uint64_t)st.st_size == b->size &&
	       tc_stat_mtime_ns(&st) == b->mtime;
}

#endif /* platform */

//! SHA-256 of everything @p b holds, read through the handle / descriptor.
static bool
tc_bin_sha256(struct tc_bin *b, char *out_hex, size_t cap)
{
	if (out_hex == NULL || cap < 65) {
		return false;
	}
	out_hex[0] = '\0';
	if (!b->open) {
		return false;
	}
	struct u_sha256 st;
	u_sha256_init(&st);
	unsigned char buf[64 * 1024];
	uint64_t off = 0;
	for (;;) {
		int64_t got = tc_bin_read_at(b, off, buf, sizeof(buf));
		if (got < 0) {
			return false;
		}
		if (got == 0) {
			break;
		}
		u_sha256_update(&st, buf, (size_t)got);
		off += (uint64_t)got;
	}
	uint8_t digest[32];
	u_sha256_final(&st, digest);
	tc_hex_digest(digest, out_hex);
	return true;
}


/*
 *
 * The verdict.
 *
 */

/*!
 * Per-process cache of the one expensive file fact: the Authenticode
 * verification (WinVerifyTrust parses the whole PE and builds a chain). It is
 * keyed on the file's IDENTITY (volume serial + file index / st_dev + st_ino),
 * size, mtime AND the SHA-256 just read through the open handle — the hash
 * is recomputed on every check (cheap next to LoadLibrary), so a file whose
 * bytes changed with size and mtime put back misses the cache instead of
 * inheriting the old verdict. The allowlist and platform facts are not cached
 * at all: they are cheap lookups, and an admin may remove an allowlist row at
 * any time. A signature verdict is only ever cached from a verification that
 * read through the same handle the digest came from.
 *
 * One mutex guards lookup + insert, and rows are copied out under it: the DP
 * refresh, IPC status snapshots (target_plugin_enumerate), and the input and
 * conversion loaders all reach this from their own threads. The slow
 * verification itself runs outside the lock (two threads may both verify a
 * new file once; harmless). Small and linear: a box has a handful of
 * components.
 */
#define TC_PROV_CACHE 32
struct tc_prov_cache_row
{
	uint64_t dev;
	uint64_t ino;
	uint64_t size;
	uint64_t mtime;
	char sha256[65];
	bool signed_ok;
	char signer[128];
	bool valid;
};
static struct tc_prov_cache_row g_tc_prov_cache[TC_PROV_CACHE];
static int g_tc_prov_cache_next = 0;
static struct os_mutex g_tc_prov_mutex;
static pthread_once_t g_tc_prov_once = PTHREAD_ONCE_INIT;
static bool g_tc_prov_mutex_ok = false;

static void
tc_prov_mutex_init(void)
{
	g_tc_prov_mutex_ok = os_mutex_init(&g_tc_prov_mutex) == 0;
}

//! Lock the cache; false (and the cache is not touched) if the mutex could not be created.
static bool
tc_prov_cache_lock(void)
{
	pthread_once(&g_tc_prov_once, tc_prov_mutex_init);
	if (!g_tc_prov_mutex_ok) {
		return false;
	}
	os_mutex_lock(&g_tc_prov_mutex);
	return true;
}

static void
tc_prov_cache_unlock(void)
{
	os_mutex_unlock(&g_tc_prov_mutex);
}

//! Copy the cached signature verdict for @p b + @p sha256 out; false on a miss.
static bool
tc_prov_cache_get(const struct tc_bin *b, const char *sha256, bool *out_signed, char *signer, size_t signer_cap)
{
	if (!tc_prov_cache_lock()) {
		return false;
	}
	bool hit = false;
	for (int i = 0; i < TC_PROV_CACHE && !hit; i++) {
		const struct tc_prov_cache_row *r = &g_tc_prov_cache[i];
		if (r->valid && r->dev == b->dev && r->ino == b->ino && r->size == b->size && r->mtime == b->mtime &&
		    strcmp(r->sha256, sha256) == 0) {
			*out_signed = r->signed_ok;
			tc_copy(signer, signer_cap, r->signer);
			hit = true;
		}
	}
	tc_prov_cache_unlock();
	return hit;
}

static void
tc_prov_cache_put(const struct tc_bin *b, const char *sha256, bool signed_ok, const char *signer)
{
	if (!tc_prov_cache_lock()) {
		return;
	}
	struct tc_prov_cache_row *r = NULL;
	// Refresh this file's row in place if it has one (new bytes, same file).
	for (int i = 0; i < TC_PROV_CACHE && r == NULL; i++) {
		if (g_tc_prov_cache[i].valid && g_tc_prov_cache[i].dev == b->dev && g_tc_prov_cache[i].ino == b->ino) {
			r = &g_tc_prov_cache[i];
		}
	}
	if (r == NULL) {
		r = &g_tc_prov_cache[g_tc_prov_cache_next];
		g_tc_prov_cache_next = (g_tc_prov_cache_next + 1) % TC_PROV_CACHE;
	}
	memset(r, 0, sizeof(*r));
	r->dev = b->dev;
	r->ino = b->ino;
	r->size = b->size;
	r->mtime = b->mtime;
	tc_copy(r->sha256, sizeof(r->sha256), sha256);
	r->signed_ok = signed_ok;
	tc_copy(r->signer, sizeof(r->signer), signer);
	r->valid = true;
	tc_prov_cache_unlock();
}

static void
tc_prov_evaluate(struct target_component_entry *e, struct tc_bin *b)
{
	e->provenance = TARGET_COMPONENT_PROVENANCE_UNKNOWN;
	e->provenance_reason[0] = '\0';
	e->signer[0] = '\0';
	e->sha256[0] = '\0';

	// The dev override and elevation are per process, not per file, and
	// cheap: re-read every time.
	const bool dev_override = target_component_dev_override_set();
	const bool elevated = target_component_process_elevated();

	// Every file fact comes from the one open handle / descriptor, and the
	// digest is always re-read: it is what the cache and the allowlist key on.
	const bool have_hash = tc_bin_sha256(b, e->sha256, sizeof(e->sha256));

	bool signed_ok = false;
	if (have_hash && !tc_prov_cache_get(b, e->sha256, &signed_ok, e->signer, sizeof(e->signer))) {
		signed_ok = tc_prov_signed(e->binary_path, b, e->signer, sizeof(e->signer));
		tc_prov_cache_put(b, e->sha256, signed_ok, e->signer);
	}
	const bool allowlisted = !signed_ok && have_hash && tc_prov_allowlisted(e->sha256);
	const bool platform_trusted = !signed_ok && !allowlisted && tc_prov_platform_trusted(e->binary_path, b);

	e->provenance =
	    target_component_provenance_classify(signed_ok, allowlisted, platform_trusted, dev_override, elevated,
	                                         e->sha256, e->provenance_reason, sizeof(e->provenance_reason));
}

bool
target_component_check_provenance(const struct target_component_class *cls, struct target_component_entry *e)
{
	(void)cls;
	if (e == NULL) {
		return false;
	}
	// A lister's question: open, judge, close. Nothing is loaded, so there is
	// no window to close; the load path holds its handle across the load.
	struct tc_bin b;
	(void)tc_bin_open(e->binary_path, &b);
	tc_prov_evaluate(e, &b);
	tc_bin_close(&b);
	return e->provenance != TARGET_COMPONENT_PROVENANCE_UNVERIFIED;
}

static enum target_component_load_result
tc_prov_gate(const struct target_component_class *cls,
             struct target_component_entry *e,
             struct tc_bin *b,
             bool quiet,
             char *reason,
             size_t reason_cap)
{
	// Audit mode says so and loads anyway; the enforcing gate refuses. The
	// verdict stays on the entry for diagnostics.
	tc_prov_evaluate(e, b);
	if (e->provenance == TARGET_COMPONENT_PROVENANCE_UNVERIFIED) {
		if (target_component_trust_enforced()) {
			U_LOG_E(
			    "%s:   %s: REFUSING unverified binary %s — %s (enforcing; Trust\\Enforce = 0 opts out, "
			    "ADR-054 N4b).",
			    tc_tag(cls), e->id, e->binary_path, e->provenance_reason);
			if (reason != NULL && reason_cap > 0) {
				snprintf(reason, reason_cap, "untrusted: %s", e->provenance_reason);
			}
			return TARGET_COMPONENT_LOAD_UNTRUSTED;
		}
		TC_LOG_LOAD(quiet, "%s:   %s: provenance UNVERIFIED — %s (audit mode: loading anyway; ADR-054 N4b).",
		            tc_tag(cls), e->id, e->provenance_reason);
	} else {
		U_LOG_I("%s:   %s: provenance %s%s%s", tc_tag(cls), e->id,
		        target_component_provenance_str(e->provenance), e->signer[0] != '\0' ? " by " : "", e->signer);
	}
	return TARGET_COMPONENT_LOAD_OK;
}

enum target_component_load_result
target_component_load_binary(const struct target_component_class *cls,
                             struct target_component_entry *e,
                             void **out_module,
                             void **out_entrypoint,
                             uint32_t *out_os_error,
                             char *reason,
                             size_t reason_cap)
{
	return target_component_load_binary_ex(cls, e, 0, out_module, out_entrypoint, out_os_error, reason, reason_cap);
}

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

int
target_component_apply_user_overlay(const struct target_component_class *cls,
                                    struct target_component_entry *entries,
                                    size_t n)
{
#ifdef XRT_OS_WINDOWS
	if (cls == NULL || cls->registry_root == NULL || entries == NULL || n == 0) {
		return 0;
	}
	wchar_t wroot[256];
	if (!tc_u2w(cls->registry_root, wroot, (int)(sizeof(wroot) / sizeof(wroot[0])))) {
		return 0;
	}

	// HKLM `<root>\Policy\Lock` = 1: IT policy, the machine value wins for
	// every row (64-bit view, where the rows live).
	wchar_t wpolicy[320];
	swprintf(wpolicy, sizeof(wpolicy) / sizeof(wpolicy[0]), L"%ls\\Policy", wroot);
	DWORD lock = 0;
	DWORD bytes = sizeof(lock);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, wpolicy, L"Lock", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &lock,
	                 &bytes) == ERROR_SUCCESS &&
	    lock == 1) {
		U_LOG_I("%s: Policy\\Lock is set — per-user Enabled values are ignored.", tc_tag(cls));
		return 0;
	}

	int applied = 0;
	for (size_t i = 0; i < n; i++) {
		struct target_component_entry *e = &entries[i];
		if (!tc_usable(e)) {
			continue;
		}
		wchar_t wid[64];
		if (!tc_u2w(e->id, wid, (int)(sizeof(wid) / sizeof(wid[0])))) {
			continue;
		}
		wchar_t wpath[384];
		swprintf(wpath, sizeof(wpath) / sizeof(wpath[0]), L"%ls\\%ls", wroot, wid);
		DWORD v = 0;
		bytes = sizeof(v);
		if (RegGetValueW(HKEY_CURRENT_USER, wpath, L"Enabled", RRF_RT_REG_DWORD, NULL, &v, &bytes) !=
		    ERROR_SUCCESS) {
			continue;
		}
		e->enabled = v != 0;
		e->flags |= TARGET_COMPONENT_FLAG_USER_OVERLAY;
		if (e->enabled) {
			e->flags &= ~TARGET_COMPONENT_FLAG_DISABLED;
			e->reason[0] = '\0';
		} else {
			e->flags |= TARGET_COMPONENT_FLAG_DISABLED;
			tc_copy(e->reason, sizeof(e->reason), "disabled for this user (HKCU Enabled = 0)");
		}
		applied++;
	}
	return applied;
#else
	(void)cls;
	(void)entries;
	(void)n;
	return 0;
#endif
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
	case TARGET_COMPONENT_LOAD_UNTRUSTED: return "untrusted";
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


/*
 *
 * Component settings (every platform) — the one options contract, N3c.
 *
 */

uint32_t
target_component_policy_resolve(bool user_present,
                                uint32_t user_value,
                                bool machine_present,
                                uint32_t machine_value,
                                bool lock,
                                uint32_t *out_value)
{
	if (user_present && !lock) {
		if (out_value != NULL) {
			*out_value = user_value;
		}
		return TARGET_COMPONENT_SETTING_USER;
	}
	if (machine_present) {
		if (out_value != NULL) {
			*out_value = machine_value;
		}
		return TARGET_COMPONENT_SETTING_MACHINE;
	}
	return TARGET_COMPONENT_SETTING_ABSENT;
}

uint32_t
target_component_read_policy_dword(const struct target_component_class *cls, const char *name, uint32_t *out_value)
{
#ifdef XRT_OS_WINDOWS
	if (cls == NULL || cls->registry_root == NULL || name == NULL || name[0] == '\0') {
		return TARGET_COMPONENT_SETTING_ABSENT;
	}
	wchar_t wroot[256];
	wchar_t wname[64];
	if (!tc_u2w(cls->registry_root, wroot, (int)(sizeof(wroot) / sizeof(wroot[0]))) ||
	    !tc_u2w(name, wname, (int)(sizeof(wname) / sizeof(wname[0])))) {
		return TARGET_COMPONENT_SETTING_ABSENT;
	}
	wchar_t wpolicy[320];
	swprintf(wpolicy, sizeof(wpolicy) / sizeof(wpolicy[0]), L"%ls\\Policy", wroot);

	DWORD user_v = 0;
	DWORD bytes = sizeof(user_v);
	const bool user_present =
	    RegGetValueW(HKEY_CURRENT_USER, wpolicy, wname, RRF_RT_REG_DWORD, NULL, &user_v, &bytes) == ERROR_SUCCESS;
	DWORD machine_v = 0;
	bytes = sizeof(machine_v);
	const bool machine_present =
	    RegGetValueW(HKEY_LOCAL_MACHINE, wpolicy, wname, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &machine_v,
	                 &bytes) == ERROR_SUCCESS;
	return target_component_policy_resolve(user_present, (uint32_t)user_v, machine_present, (uint32_t)machine_v,
	                                       tc_policy_lock(cls), out_value);
#else
	(void)cls;
	(void)name;
	(void)out_value;
	return TARGET_COMPONENT_SETTING_ABSENT;
#endif
}

const char *
target_component_setting_source_str(uint32_t source)
{
	switch (source) {
	case TARGET_COMPONENT_SETTING_ABSENT: return "absent";
	case TARGET_COMPONENT_SETTING_ENVIRONMENT: return "environment";
	case TARGET_COMPONENT_SETTING_USER: return "user";
	case TARGET_COMPONENT_SETTING_MACHINE: return "machine";
	case TARGET_COMPONENT_SETTING_MANIFEST: return "manifest";
	default: return "absent";
	}
}

bool
target_component_setting_key_valid(const char *key)
{
	if (key == NULL || key[0] == '\0' || (key[0] >= '0' && key[0] <= '9')) {
		return false;
	}
	size_t n = 0;
	for (const char *c = key; *c != '\0'; c++, n++) {
		const bool ok =
		    (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
		if (!ok) {
			return false;
		}
	}
	return n < TARGET_COMPONENT_SETTING_KEY_MAX;
}

bool
target_component_setting_env_name(const char *env_class, const char *id, const char *key, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (env_class == NULL || env_class[0] == '\0' || id == NULL || id[0] == '\0' ||
	    !target_component_setting_key_valid(key)) {
		return false;
	}
	const int n = snprintf(out, cap, "DXR_%s_%s_%s", env_class, id, key);
	if (n <= 0 || (size_t)n >= cap) {
		out[0] = '\0';
		return false;
	}
	for (char *c = out; *c != '\0'; c++) {
		if (*c == '-') {
			*c = '_';
		} else if (*c >= 'a' && *c <= 'z') {
			*c = (char)(*c - ('a' - 'A'));
		} else if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) {
			out[0] = '\0';
			return false; // an id that cannot name an environment variable
		}
	}
	return true;
}

const char *
target_component_setting_resolve(const char *env_value,
                                 const char *user_value,
                                 const char *stored_value,
                                 bool stored_is_manifest,
                                 bool lock,
                                 char *buf,
                                 size_t cap,
                                 uint32_t *out_source)
{
	uint32_t src = TARGET_COMPONENT_SETTING_ABSENT;
	const char *ret = NULL;
	if (buf == NULL || cap == 0) {
		goto out;
	}
	buf[0] = '\0';
	// 1. The environment always wins (u_setting rule) — a set-but-empty
	//    variable is "unset", as the conversion read side always treated it.
	if (env_value != NULL && env_value[0] != '\0') {
		tc_copy(buf, cap, env_value);
		src = TARGET_COMPONENT_SETTING_ENVIRONMENT;
		ret = buf;
		goto out;
	}
	// 2. The per-user store, unless the machine's policy lock hides it.
	if (!lock && user_value != NULL) {
		tc_copy(buf, cap, user_value);
		src = TARGET_COMPONENT_SETTING_USER;
		ret = buf;
		goto out;
	}
	// 3. The machine store / the manifest.
	if (stored_value != NULL) {
		tc_copy(buf, cap, stored_value);
		src = stored_is_manifest ? TARGET_COMPONENT_SETTING_MANIFEST : TARGET_COMPONENT_SETTING_MACHINE;
		ret = buf;
	}
out:
	if (out_source != NULL) {
		*out_source = src;
	}
	return ret;
}

bool
target_component_user_settings_json_lookup(const char *json_text, const char *key, char *buf, size_t cap)
{
	if (json_text == NULL || key == NULL || buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	cJSON *root = cJSON_Parse(json_text);
	if (root == NULL) {
		return false;
	}
	bool ok = false;
	if (cJSON_IsObject(root)) {
		const cJSON *kv = NULL;
		cJSON_ArrayForEach(kv, root)
		{
			if (kv->string == NULL || !tc_ci_equal(kv->string, key)) {
				continue;
			}
			if (cJSON_IsString(kv) && kv->valuestring != NULL) {
				tc_copy(buf, cap, kv->valuestring);
				ok = true;
			} else if (cJSON_IsBool(kv)) {
				tc_copy(buf, cap, cJSON_IsTrue(kv) ? "1" : "0");
				ok = true;
			} else if (cJSON_IsNumber(kv)) {
				snprintf(buf, cap, "%.17g", kv->valuedouble);
				ok = true;
			}
			break;
		}
	}
	cJSON_Delete(root);
	return ok;
}

//! The chain; tier 3 is the store unless @p use_row, then @p row_value (may be NULL).
static bool
tc_get_setting(const struct target_component_class *cls,
               const char *id,
               const char *key,
               bool use_row,
               const char *row_value,
               char *buf,
               size_t cap,
               uint32_t *out_source)
{
	if (out_source != NULL) {
		*out_source = TARGET_COMPONENT_SETTING_ABSENT;
	}
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (cls == NULL || id == NULL || id[0] == '\0' || !target_component_setting_key_valid(key)) {
		return false;
	}

	// Tier 1: the hosting process's environment, through u_setting's env
	// reader (Windows getenv_s, Android debug.xrt.* properties, POSIX
	// getenv) — an unmanaged name never touches u_setting's own stores.
	char env_name[TARGET_COMPONENT_SETTING_ENV_MAX];
	char env_buf[TARGET_COMPONENT_SETTING_VALUE_MAX];
	const char *env_value = NULL;
	if (target_component_setting_env_name(cls->env_class, id, key, env_name, sizeof(env_name))) {
		enum u_setting_source us = U_SETTING_SOURCE_DEFAULT;
		if (u_setting_get_raw(env_name, env_buf, sizeof(env_buf), &us) != NULL && us == U_SETTING_SOURCE_ENV) {
			env_value = env_buf;
		}
	}

	// Tiers 2 + 3, read only when needed (each is a registry / file read).
	char user_buf[TARGET_COMPONENT_SETTING_VALUE_MAX];
	char stored_buf[TARGET_COMPONENT_SETTING_VALUE_MAX];
	const char *user_value = NULL;
	const char *stored_value = NULL;
	bool lock = false;
	if (env_value == NULL || env_value[0] == '\0') {
		lock = tc_store_lock(cls);
		if (!lock && tc_store_read(cls, id, key, true, user_buf, sizeof(user_buf))) {
			user_value = user_buf;
		}
		if (user_value == NULL) {
			if (use_row) {
				stored_value = row_value;
			} else if (tc_store_read(cls, id, key, false, stored_buf, sizeof(stored_buf))) {
				stored_value = stored_buf;
			}
		}
	}
#if defined(XRT_OS_WINDOWS)
	const bool stored_is_manifest = false;
#else
	const bool stored_is_manifest = true;
#endif
	return target_component_setting_resolve(env_value, user_value, stored_value, stored_is_manifest, lock, buf, cap,
	                                        out_source) != NULL;
}

bool
target_component_get_setting(const struct target_component_class *cls,
                             const char *id,
                             const char *key,
                             char *buf,
                             size_t cap,
                             uint32_t *out_source)
{
	return tc_get_setting(cls, id, key, false, NULL, buf, cap, out_source);
}

bool
target_component_get_setting_ex(const struct target_component_class *cls,
                                const char *id,
                                const char *key,
                                const char *row_value,
                                char *buf,
                                size_t cap,
                                uint32_t *out_source)
{
	return tc_get_setting(cls, id, key, true, row_value, buf, cap, out_source);
}

enum target_component_store_result
target_component_set_setting(
    const struct target_component_class *cls, const char *id, const char *key, const char *value, bool machine)
{
	if (cls == NULL || id == NULL || id[0] == '\0' || !target_component_setting_key_valid(key) || value == NULL ||
	    strlen(value) >= TARGET_COMPONENT_SETTING_VALUE_MAX) {
		return TARGET_COMPONENT_STORE_FAILED;
	}
	char upper[TARGET_COMPONENT_SETTING_KEY_MAX];
	tc_upper_copy(upper, sizeof(upper), key);
	return tc_store_write(cls, id, upper, value, machine);
}

enum target_component_store_result
target_component_unset_setting(const struct target_component_class *cls, const char *id, const char *key, bool machine)
{
	if (cls == NULL || id == NULL || id[0] == '\0' || !target_component_setting_key_valid(key)) {
		return TARGET_COMPONENT_STORE_FAILED;
	}
	char upper[TARGET_COMPONENT_SETTING_KEY_MAX];
	tc_upper_copy(upper, sizeof(upper), key);
	return tc_store_write(cls, id, upper, NULL, machine);
}

int
target_component_list_settings(const struct target_component_class *cls,
                               const char *id,
                               struct target_component_setting_view *out,
                               int max)
{
	if (cls == NULL || id == NULL || out == NULL || max <= 0) {
		return 0;
	}
	char keys[2 * TARGET_COMPONENT_MAX_SETTINGS][TARGET_COMPONENT_SETTING_KEY_MAX];
	const int cap = (int)(sizeof(keys) / sizeof(keys[0]));
	int n = tc_store_keys(cls, id, false, keys, cap);
	n += tc_store_keys(cls, id, true, keys + n, cap - n);

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		if (!target_component_setting_key_valid(keys[i])) {
			continue;
		}
		bool dup = false;
		for (int j = 0; j < count; j++) {
			if (tc_ci_equal(out[j].key, keys[i])) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		struct target_component_setting_view *v = &out[count];
		memset(v, 0, sizeof(*v));
		tc_upper_copy(v->key, sizeof(v->key), keys[i]);
		(void)target_component_get_setting(cls, id, v->key, v->value, sizeof(v->value), &v->source);
		count++;
	}
	return count;
}

bool
target_component_setting_store_path(
    const struct target_component_class *cls, const char *id, bool machine, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	return tc_store_path(cls, id, machine, out, cap);
}

int
target_component_manifest_settings_schema(const struct target_component_class *cls,
                                          const char *id,
                                          struct target_component_setting_desc *out,
                                          int max)
{
	if (cls == NULL || id == NULL || out == NULL || max <= 0) {
		return 0;
	}
	return tc_store_manifest_schema(cls, id, out, max);
}

int
target_component_merge_setting_descs(struct target_component_setting_desc *out,
                                     int existing,
                                     int max,
                                     const struct xrt_component_setting_desc *in,
                                     uint32_t n)
{
	if (out == NULL || existing < 0 || existing > max) {
		return existing;
	}
	int count = existing;
	for (uint32_t i = 0; in != NULL && i < n && count < max; i++) {
		const struct xrt_component_setting_desc *d = &in[i];
		if (!target_component_setting_key_valid(d->key)) {
			continue;
		}
		bool dup = false;
		for (int j = 0; j < count; j++) {
			if (tc_ci_equal(out[j].key, d->key)) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		struct target_component_setting_desc *o = &out[count++];
		memset(o, 0, sizeof(*o));
		tc_upper_copy(o->key, sizeof(o->key), d->key);
		tc_copy(o->type, sizeof(o->type), d->type);
		tc_copy(o->default_value, sizeof(o->default_value), d->default_value);
		tc_copy(o->help, sizeof(o->help), d->help);
	}
	return count;
}
