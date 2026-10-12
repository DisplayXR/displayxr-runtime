// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry / manifest / filename discovery + negotiation for
 *         conversion-provider plug-ins (ADR-053).
 *
 * Mirrors `target_input_plugin_loader.c` for the third plug-in type and
 * shares the display loader's plumbing (`target_plugin_build_discovery_roots`,
 * `target_plugin_preload_runtime_core_dll`, the #434 preload sanitizer, the
 * #952 dev-path guard, the prefix-taking Android filename parse and the
 * manifest-suffix router). Unlike the other two loaders it keeps no global
 * state and selects nothing: it returns an entry array, and the conversion
 * core (#2000) probes, describes and selects.
 *
 * Contract: docs/specs/runtime/conversion-provider-discovery.md §3-§4.
 *
 * @ingroup target_common
 */

#include "target_conversion_plugin_loader.h"
#include "target_plugin_loader.h"
#include "target_plugin_preload_sanitize.h"
#include "target_plugin_path_guard.h"

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

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
conv_copy(char *dst, size_t dst_size, const char *src)
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

static int
conv_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool
conv_streq_nocase(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (conv_lower((unsigned char)*a) != conv_lower((unsigned char)*b)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == *b;
}

static const char *
conv_basename(const char *path)
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
conv_is_usable(const struct xrt_conversion_plugin_entry *e)
{
	return (e->flags & XRT_CONV_ENTRY_FLAG_MALFORMED) == 0;
}

static void
conv_set_malformed(struct xrt_conversion_plugin_entry *e, const char *reason)
{
	e->flags |= XRT_CONV_ENTRY_FLAG_MALFORMED;
	e->status = XRT_CONV_LOAD_MALFORMED;
	conv_copy(e->reason, sizeof(e->reason), reason);
}

/*!
 * Defaults shared by every platform's row reader, applied after the
 * entry is zeroed.
 */
static void
conv_entry_init(struct xrt_conversion_plugin_entry *e)
{
	memset(e, 0, sizeof(*e));
	e->probe_order = XRT_CONV_LOADER_DEFAULT_PROBE_ORDER;
	e->enabled = true;
	e->hosting = XRT_CONV_HOSTING_AUTO;
	e->status = XRT_CONV_LOAD_NOT_LOADED;
	e->negotiate_result = XRT_SUCCESS;
}

/*!
 * Post-read normalisation shared by every platform: display name fallback,
 * Hosting parse, Enabled -> DISABLED status.
 */
static void
conv_entry_finish(struct xrt_conversion_plugin_entry *e)
{
	if (e->display_name[0] == '\0') {
		conv_copy(e->display_name, sizeof(e->display_name), e->id);
	}
	bool known = true;
	e->hosting = target_conversion_plugin_parse_hosting(e->hosting_raw, &known);
	if (!known) {
		e->flags |= XRT_CONV_ENTRY_FLAG_HOSTING_UNKNOWN;
	}
	if (!e->enabled) {
		e->flags |= XRT_CONV_ENTRY_FLAG_DISABLED;
		if (conv_is_usable(e)) {
			e->status = XRT_CONV_LOAD_DISABLED;
			conv_copy(e->reason, sizeof(e->reason), "disabled (Enabled = 0)");
		}
	}
}


/*
 *
 * Pure helpers (every platform).
 *
 */

uint32_t
target_conversion_plugin_parse_hosting(const char *s, bool *out_known)
{
	bool known = true;
	uint32_t h = XRT_CONV_HOSTING_AUTO;
	if (s == NULL || s[0] == '\0' || conv_streq_nocase(s, "auto")) {
		h = XRT_CONV_HOSTING_AUTO;
	} else if (conv_streq_nocase(s, "in-service")) {
		h = XRT_CONV_HOSTING_IN_SERVICE;
	} else if (conv_streq_nocase(s, "satellite")) {
		h = XRT_CONV_HOSTING_SATELLITE;
	} else {
		known = false;
	}
	if (out_known != NULL) {
		*out_known = known;
	}
	return h;
}

const char *
target_conversion_plugin_status_str(uint32_t status)
{
	switch (status) {
	case XRT_CONV_LOAD_NOT_LOADED: return "not-loaded";
	case XRT_CONV_LOAD_NEGOTIATED: return "negotiated";
	case XRT_CONV_LOAD_MALFORMED: return "malformed";
	case XRT_CONV_LOAD_DISABLED: return "disabled";
	case XRT_CONV_LOAD_SUPPRESSED: return "suppressed";
	case XRT_CONV_LOAD_PATH_REFUSED: return "path-refused";
	case XRT_CONV_LOAD_LOAD_FAILED: return "load-failed";
	case XRT_CONV_LOAD_NO_ENTRYPOINT: return "no-entrypoint";
	case XRT_CONV_LOAD_DECLINED: return "declined";
	case XRT_CONV_LOAD_NEGOTIATE_FAILED: return "negotiate-failed";
	case XRT_CONV_LOAD_ABI_MISMATCH: return "abi-mismatch";
	default: return "unknown";
	}
}

static int
conv_compare(const void *a, const void *b)
{
	const struct xrt_conversion_plugin_entry *ea = (const struct xrt_conversion_plugin_entry *)a;
	const struct xrt_conversion_plugin_entry *eb = (const struct xrt_conversion_plugin_entry *)b;
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
	// Same id twice only happens for malformed rows; keep it total anyway.
	return strcmp(ea->source, eb->source);
}

void
target_conversion_plugin_sort(struct xrt_conversion_plugin_entry *entries, int count)
{
	if (entries == NULL || count <= 0) {
		return;
	}
	if (count > 1) {
		qsort(entries, (size_t)count, sizeof(entries[0]), conv_compare);
	}
	for (int i = 0; i < count; i++) {
		entries[i].flags &= ~XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE;
	}
	for (int i = 1; i < count; i++) {
		if (entries[i - 1].probe_order != entries[i].probe_order) {
			continue;
		}
		entries[i - 1].flags |= XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE;
		entries[i].flags |= XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE;
		// INFO: enumeration re-runs on every re-describe trigger; the core
		// WARNs once per change (#1466 rule, discovery spec §3.4).
		U_LOG_I("conversion plugin loader: '%s' and '%s' share ProbeOrder %u — tie broken by id.",
		        entries[i - 1].id, entries[i].id, entries[i].probe_order);
	}
}

int
target_conversion_plugin_merge(struct xrt_conversion_plugin_entry *dst,
                               int dst_count,
                               int max,
                               const struct xrt_conversion_plugin_entry *src,
                               int src_count)
{
	if (dst == NULL || src == NULL) {
		return dst_count;
	}
	int n = dst_count;
	for (int i = 0; i < src_count; i++) {
		const struct xrt_conversion_plugin_entry *e = &src[i];
		if (conv_is_usable(e)) {
			bool dup = false;
			for (int j = 0; j < n; j++) {
				if (conv_is_usable(&dst[j]) && strcmp(dst[j].id, e->id) == 0) {
					U_LOG_I("conversion plugin loader: %s: shadowed by %s for id='%s'.", e->source,
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
			U_LOG_I("conversion plugin loader: more than %d entries — truncating.", max);
			break;
		}
		dst[n++] = *e;
	}
	return n;
}

void
target_conversion_plugin_apply_kill_switches(struct xrt_conversion_plugin_entry *entries,
                                             int count,
                                             const char *providers_env,
                                             const char *exclusive_id)
{
	const bool none = providers_env != NULL && strcmp(providers_env, "0") == 0;
	const bool exclusive = exclusive_id != NULL && exclusive_id[0] != '\0';
	if (entries == NULL || (!none && !exclusive)) {
		return;
	}
	for (int i = 0; i < count; i++) {
		struct xrt_conversion_plugin_entry *e = &entries[i];
		const char *why = NULL;
		if (none) {
			why = "suppressed by DXR_CONV_PROVIDERS=0";
		} else if (strcmp(e->id, exclusive_id) != 0) {
			why = "suppressed by DXR_CONV_EXCLUSIVE";
		}
		if (why == NULL) {
			continue;
		}
		e->flags |= XRT_CONV_ENTRY_FLAG_SUPPRESSED;
		// A malformed row keeps saying why it is malformed.
		if (conv_is_usable(e)) {
			e->status = XRT_CONV_LOAD_SUPPRESSED;
			conv_copy(e->reason, sizeof(e->reason), why);
		}
	}
}

/*!
 * ProbeOrder hint from a `NNN-<id>-conversion-provider.json` file name, or
 * false when the name has no three-digit prefix.
 */
static bool
conv_probe_order_from_filename(const char *name, uint32_t *out)
{
	if (name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9' ||
	    name[3] != '-') {
		return false;
	}
	*out = (uint32_t)((name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0'));
	return true;
}

static void
conv_json_str(const cJSON *obj, const char *field, char *dst, size_t dst_size)
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
target_conversion_plugin_parse_manifest_json(const char *json_text,
                                             const char *source,
                                             struct xrt_conversion_plugin_entry *out)
{
	if (out == NULL) {
		return false;
	}
	conv_entry_init(out);
	conv_copy(out->source, sizeof(out->source), source != NULL ? source : "");
	// Until the manifest names itself, the file name is the best id we have.
	conv_copy(out->id, sizeof(out->id), source != NULL ? conv_basename(source) : "");

	if (json_text == NULL) {
		conv_set_malformed(out, "manifest is empty");
		return false;
	}

	cJSON *root = cJSON_Parse(json_text);
	if (root == NULL) {
		conv_set_malformed(out, "manifest is not valid JSON");
		return false;
	}

	char fmt[16] = {0};
	conv_json_str(root, "file_format_version", fmt, sizeof(fmt));
	if (strcmp(fmt, "1.0") != 0) {
		char why[96];
		snprintf(why, sizeof(why), "unsupported file_format_version '%s' (want \"1.0\")", fmt);
		conv_set_malformed(out, why);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *plugin = u_json_get(root, "plugin");
	if (plugin == NULL || !cJSON_IsObject(plugin)) {
		conv_set_malformed(out, "missing 'plugin' object");
		cJSON_Delete(root);
		return false;
	}

	char id[sizeof(out->id)] = {0};
	conv_json_str(plugin, "id", id, sizeof(id));
	if (id[0] != '\0') {
		conv_copy(out->id, sizeof(out->id), id);
	}
	conv_json_str(plugin, "display_name", out->display_name, sizeof(out->display_name));
	conv_json_str(plugin, "vendor", out->vendor, sizeof(out->vendor));
	conv_json_str(plugin, "version", out->version, sizeof(out->version));
	conv_json_str(plugin, "binary_path", out->binary_path, sizeof(out->binary_path));
	conv_json_str(plugin, "hosting", out->hosting_raw, sizeof(out->hosting_raw));
	conv_json_str(plugin, "supersedes", out->supersedes, sizeof(out->supersedes));

	// ProbeOrder: the manifest field is authoritative; the file-name prefix
	// is the fallback (plugin-discovery.md §3.1), then the default.
	uint32_t hinted = 0;
	if (source != NULL && conv_probe_order_from_filename(conv_basename(source), &hinted)) {
		out->probe_order = hinted;
	}
	const cJSON *po = u_json_get(plugin, "probe_order");
	if (po != NULL) {
		int v = 0;
		if (!cJSON_IsNumber(po) || !u_json_get_int(po, &v) || v < 0) {
			conv_set_malformed(out, "'plugin.probe_order' is not a non-negative integer");
			cJSON_Delete(root);
			return false;
		}
		out->probe_order = (uint32_t)v;
	}

	const cJSON *en = u_json_get(plugin, "enabled");
	if (en != NULL) {
		if (!cJSON_IsBool(en)) {
			conv_set_malformed(out, "'plugin.enabled' is not a boolean");
			cJSON_Delete(root);
			return false;
		}
		out->enabled = cJSON_IsTrue(en);
	}

	const cJSON *packs = u_json_get(plugin, "packs");
	if (packs != NULL) {
		if (!cJSON_IsArray(packs)) {
			conv_set_malformed(out, "'plugin.packs' is not an array");
			cJSON_Delete(root);
			return false;
		}
		const cJSON *p = NULL;
		cJSON_ArrayForEach(p, packs)
		{
			if (!cJSON_IsString(p) || p->valuestring == NULL) {
				continue;
			}
			if (out->pack_count >= XRT_CONV_LOADER_MAX_PACKS) {
				U_LOG_I("conversion plugin loader: %s: more than %d packs — truncating.", out->source,
				        XRT_CONV_LOADER_MAX_PACKS);
				break;
			}
			conv_copy(out->packs[out->pack_count++], XRT_CONV_LOADER_PATH_MAX, p->valuestring);
		}
	}

	const cJSON *settings = u_json_get(plugin, "settings");
	if (settings != NULL) {
		if (!cJSON_IsObject(settings)) {
			conv_set_malformed(out, "'plugin.settings' is not an object");
			cJSON_Delete(root);
			return false;
		}
		const cJSON *kv = NULL;
		cJSON_ArrayForEach(kv, settings)
		{
			if (kv->string == NULL) {
				continue;
			}
			if (out->setting_count >= XRT_CONV_LOADER_MAX_SETTINGS) {
				U_LOG_I("conversion plugin loader: %s: more than %d settings — truncating.",
				        out->source, XRT_CONV_LOADER_MAX_SETTINGS);
				break;
			}
			struct xrt_conversion_plugin_setting *s = &out->settings[out->setting_count];
			conv_copy(s->key, sizeof(s->key), kv->string);
			if (cJSON_IsString(kv) && kv->valuestring != NULL) {
				conv_copy(s->value, sizeof(s->value), kv->valuestring);
			} else if (cJSON_IsBool(kv)) {
				conv_copy(s->value, sizeof(s->value), cJSON_IsTrue(kv) ? "1" : "0");
			} else if (cJSON_IsNumber(kv)) {
				snprintf(s->value, sizeof(s->value), "%.17g", kv->valuedouble);
			} else {
				continue; // objects / arrays / null are not settings
			}
			out->setting_count++;
		}
	}

	cJSON_Delete(root);

	if (id[0] == '\0') {
		conv_set_malformed(out, "required field 'plugin.id' missing");
		return false;
	}
	if (out->binary_path[0] == '\0') {
		conv_set_malformed(out, "required field 'plugin.binary_path' missing");
		return false;
	}

	conv_entry_finish(out);
	return true;
}


/*
 *
 * Host iface default (every platform).
 *
 */

static bool
conv_default_get_path(uint32_t kind, char *buf, uint32_t buf_size)
{
	(void)kind;
	if (buf != NULL && buf_size > 0) {
		buf[0] = '\0';
	}
	return false;
}

static bool
conv_default_get_setting(const char *key, char *buf, uint32_t buf_size)
{
	(void)key;
	if (buf != NULL && buf_size > 0) {
		buf[0] = '\0';
	}
	return false;
}

static bool
conv_default_get_adapter_hint(struct xrt_conv_adapter_hint *inout)
{
	(void)inout;
	return false;
}

static bool
conv_default_model_resolve(const char *ref, struct xrt_conv_model_location *out)
{
	(void)ref;
	(void)out;
	return false;
}

static bool
conv_default_model_request(const char *ref)
{
	(void)ref;
	return false;
}

static void
conv_default_notify_changed(uint32_t cap_id)
{
	(void)cap_id;
}

static const struct xrt_conversion_plugin_host_iface g_conv_default_host = {
    .struct_size = (uint32_t)sizeof(struct xrt_conversion_plugin_host_iface),
    .host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT,
    .host_flags = 0,
    .reserved_0 = 0,
    .get_path = conv_default_get_path,
    .get_setting = conv_default_get_setting,
    .get_adapter_hint = conv_default_get_adapter_hint,
    .model_resolve = conv_default_model_resolve,
    .model_request = conv_default_model_request,
    .notify_changed = conv_default_notify_changed,
};

const struct xrt_conversion_plugin_host_iface *
target_conversion_plugin_default_host(void)
{
	return &g_conv_default_host;
}


/*
 *
 * The negotiation gate (every platform).
 *
 */

bool
target_conversion_plugin_negotiate_with(struct xrt_conversion_plugin_entry *e,
                                        xrt_conversion_plugin_negotiate_fn_t negotiate,
                                        const struct xrt_conversion_plugin_host_iface *host)
{
	if (e == NULL) {
		return false;
	}
	e->iface = NULL;
	e->plugin_api_version = 0;
	e->negotiate_result = XRT_SUCCESS;
	e->reason[0] = '\0';

	if (negotiate == NULL) {
		e->status = XRT_CONV_LOAD_NO_ENTRYPOINT;
		snprintf(e->reason, sizeof(e->reason), "no '%s' entry point", XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		return false;
	}
	if (host == NULL) {
		host = target_conversion_plugin_default_host();
	}

	struct xrt_conversion_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, host, &iface, &plugin_version);
	e->negotiate_result = xret;
	e->plugin_api_version = plugin_version;

	if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
		e->status = XRT_CONV_LOAD_DECLINED;
		conv_copy(e->reason, sizeof(e->reason), "negotiate declined (XRT_ERROR_PROBER_NOT_SUPPORTED)");
		U_LOG_W("conversion plugin loader:   %s: negotiate declined — skipping.", e->id);
		return false;
	}

	/* ADR-020 rule 3: never dispatch through a mismatched vtable. Checked
	 * before the result code, because a provider of another major may well
	 * refuse our version with an error AND report its own. */
	if (plugin_version != 0 && plugin_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		e->status = XRT_CONV_LOAD_ABI_MISMATCH;
		snprintf(e->reason, sizeof(e->reason), "ABI major mismatch: plugin_api=%u, runtime expects %u",
		         plugin_version, (unsigned)XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
		U_LOG_E(
		    "conversion plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the provider must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    e->id, plugin_version, (unsigned)XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
		return false;
	}

	if (xret != XRT_SUCCESS || iface == NULL) {
		e->status = XRT_CONV_LOAD_NEGOTIATE_FAILED;
		snprintf(e->reason, sizeof(e->reason), "negotiate returned %d (iface=%s)", (int)xret,
		         iface != NULL ? "set" : "NULL");
		U_LOG_W("conversion plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id,
		        (int)xret, (void *)iface);
		return false;
	}

	if (plugin_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		// Success with no version written: same rule, the vtable is unknown.
		e->status = XRT_CONV_LOAD_ABI_MISMATCH;
		snprintf(e->reason, sizeof(e->reason), "ABI major mismatch: plugin_api=%u, runtime expects %u",
		         plugin_version, (unsigned)XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
		U_LOG_E(
		    "conversion plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		    "the provider must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		    e->id, plugin_version, (unsigned)XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
		return false;
	}

	/* A v1 provider fills the whole v1 vtable; anything shorter is a broken
	 * build, not an older minor (there is none). */
	if (!XRT_CONV_COVERS(iface, struct xrt_conversion_plugin_iface, destroy)) {
		e->status = XRT_CONV_LOAD_NEGOTIATE_FAILED;
		snprintf(e->reason, sizeof(e->reason), "iface struct_size %u is smaller than the v1 vtable (%u)",
		         iface->struct_size, (unsigned)sizeof(struct xrt_conversion_plugin_iface));
		U_LOG_W("conversion plugin loader:   %s: %s — skipping.", e->id, e->reason);
		return false;
	}

	if (iface->id != NULL && strcmp(iface->id, e->id) != 0) {
		// Registry / manifest id is authoritative; the mismatch is a
		// packaging smell worth one line, not a rejection.
		U_LOG_I("conversion plugin loader:   %s: provider reports id '%s' (registered id wins).", e->id,
		        iface->id);
	}

	e->iface = iface;
	e->status = XRT_CONV_LOAD_NEGOTIATED;
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

#define CONV_REG_ROOT "Software\\DisplayXR\\ConversionProviders"

static void
conv_w2u(const wchar_t *src, char *dst, int dst_size)
{
	if (dst_size <= 0) {
		return;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL) <= 0) {
		dst[0] = '\0';
	}
}

static bool
conv_u2w(const char *src, wchar_t *dst, int dst_chars)
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
conv_reg_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[XRT_CONV_LOADER_PATH_MAX];
	DWORD bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, wbuf, &bytes) != ERROR_SUCCESS) {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
		return false;
	}
	conv_w2u(wbuf, dst, dst_size);
	return true;
}

static bool
conv_reg_dword(HKEY key, const wchar_t *name, DWORD *out)
{
	DWORD v = 0;
	DWORD bytes = sizeof(v);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_DWORD, NULL, &v, &bytes) != ERROR_SUCCESS) {
		return false;
	}
	*out = v;
	return true;
}

static void
conv_reg_packs(HKEY key, struct xrt_conversion_plugin_entry *e)
{
	wchar_t wbuf[XRT_CONV_LOADER_MAX_PACKS * XRT_CONV_LOADER_PATH_MAX + 2];
	DWORD bytes = sizeof(wbuf) - 2 * sizeof(wchar_t);
	memset(wbuf, 0, sizeof(wbuf));
	if (RegGetValueW(key, NULL, L"Packs", RRF_RT_REG_MULTI_SZ, NULL, wbuf, &bytes) != ERROR_SUCCESS) {
		return;
	}
	for (const wchar_t *p = wbuf; *p != L'\0'; p += wcslen(p) + 1) {
		if (e->pack_count >= XRT_CONV_LOADER_MAX_PACKS) {
			U_LOG_I("conversion plugin loader: %s: more than %d packs — truncating.", e->id,
			        XRT_CONV_LOADER_MAX_PACKS);
			break;
		}
		conv_w2u(p, e->packs[e->pack_count++], XRT_CONV_LOADER_PATH_MAX);
	}
}

static void
conv_reg_settings(HKEY key, struct xrt_conversion_plugin_entry *e)
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
		if (e->setting_count >= XRT_CONV_LOADER_MAX_SETTINGS) {
			U_LOG_I("conversion plugin loader: %s: more than %d settings — truncating.", e->id,
			        XRT_CONV_LOADER_MAX_SETTINGS);
			break;
		}
		struct xrt_conversion_plugin_setting *s = &e->settings[e->setting_count];
		if (type == REG_SZ || type == REG_EXPAND_SZ) {
			conv_w2u((const wchar_t *)data, s->value, (int)sizeof(s->value));
		} else if (type == REG_DWORD && data_bytes == sizeof(DWORD)) {
			DWORD v;
			memcpy(&v, data, sizeof(v));
			snprintf(s->value, sizeof(s->value), "%lu", (unsigned long)v);
		} else {
			continue;
		}
		conv_w2u(name, s->key, (int)sizeof(s->key));
		e->setting_count++;
	}
	RegCloseKey(sk);
}

//! Read one `<root>\<id>` row into @p e (initialised here).
static void
conv_read_row(HKEY root, const wchar_t *subkey, const char *root_label, struct xrt_conversion_plugin_entry *e)
{
	conv_entry_init(e);
	conv_w2u(subkey, e->id, (int)sizeof(e->id));
	snprintf(e->source, sizeof(e->source), "%s\\%s", root_label, e->id);

	HKEY sub;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
		conv_set_malformed(e, "registry subkey could not be opened");
		return;
	}

	bool have_binary = conv_reg_string(sub, L"Binary", e->binary_path, (int)sizeof(e->binary_path));
	conv_reg_string(sub, L"DisplayName", e->display_name, (int)sizeof(e->display_name));
	conv_reg_string(sub, L"Vendor", e->vendor, (int)sizeof(e->vendor));
	conv_reg_string(sub, L"Version", e->version, (int)sizeof(e->version));
	conv_reg_string(sub, L"Hosting", e->hosting_raw, (int)sizeof(e->hosting_raw));
	conv_reg_string(sub, L"Supersedes", e->supersedes, (int)sizeof(e->supersedes));

	DWORD v = 0;
	if (conv_reg_dword(sub, L"ProbeOrder", &v)) {
		e->probe_order = (uint32_t)v;
	}
	if (conv_reg_dword(sub, L"Enabled", &v)) {
		e->enabled = v != 0;
	}
	conv_reg_packs(sub, e);
	conv_reg_settings(sub, e);
	RegCloseKey(sub);

	if (!have_binary || e->binary_path[0] == '\0') {
		conv_set_malformed(e, "missing required 'Binary' value");
	}
	conv_entry_finish(e);
}

static int
conv_enumerate_registry(
    HKEY hive, const char *subkey, const char *label, struct xrt_conversion_plugin_entry *out, int max)
{
	wchar_t wsub[256];
	if (!conv_u2w(subkey, wsub, (int)(sizeof(wsub) / sizeof(wsub[0])))) {
		return 0;
	}
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(hive, wsub, 0, KEY_READ | KEY_WOW64_64KEY, &root);
	if (rc != ERROR_SUCCESS) {
		U_LOG_I("conversion plugin loader: registry root %s absent (rc=%ld) — no providers.", label, rc);
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
			U_LOG_I("conversion plugin loader: RegEnumKeyEx failed (rc=%ld) at index %lu — stopping.", er,
			        (unsigned long)index);
			break;
		}
		if (count >= max) {
			U_LOG_I("conversion plugin loader: more than %d registered providers — truncating.", max);
			break;
		}
		conv_read_row(root, name, label, &out[count++]);
	}
	RegCloseKey(root);
	return count;
}

int
target_conversion_plugin_enumerate_registry_key(bool current_user,
                                                const char *subkey,
                                                struct xrt_conversion_plugin_entry *out,
                                                int max)
{
	if (subkey == NULL || out == NULL || max <= 0) {
		return 0;
	}
	char label[300];
	snprintf(label, sizeof(label), "%s\\%s", current_user ? "HKCU" : "HKLM", subkey);
	return conv_enumerate_registry(current_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, subkey, label, out, max);
}

static int
conv_discover(struct xrt_conversion_plugin_entry *out, int max)
{
	return conv_enumerate_registry(HKEY_LOCAL_MACHINE, CONV_REG_ROOT, "HKLM\\" CONV_REG_ROOT, out, max);
}

static bool
conv_load_binary(struct xrt_conversion_plugin_entry *e, xrt_conversion_plugin_negotiate_fn_t *out_fn)
{
	*out_fn = NULL;
	wchar_t wpath[XRT_CONV_LOADER_PATH_MAX];
	if (!conv_u2w(e->binary_path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
		e->status = XRT_CONV_LOAD_LOAD_FAILED;
		conv_copy(e->reason, sizeof(e->reason), "binary path is not valid UTF-8");
		return false;
	}
	// LOAD_WITH_ALTERED_SEARCH_PATH needs backslashes: with forward slashes the
	// "directory of the DLL" search rule silently does not apply.
	for (wchar_t *p = wpath; *p != L'\0'; p++) {
		if (*p == L'/') {
			*p = L'\\';
		}
	}

	// #952: same dev-path guard as the DP and input loaders.
	if (target_plugin_path_check(wpath, e->id, "conversion provider") == TARGET_PLUGIN_PATH_REFUSED) {
		e->status = XRT_CONV_LOAD_PATH_REFUSED;
		conv_copy(e->reason, sizeof(e->reason),
		          "binary lives in a build tree (set DXR_ALLOW_DEV_PLUGIN_PATHS=1 to allow, #952)");
		return false;
	}

	// Providers import DisplayXRClient.dll for the aux surface (#328), and
	// LoadLibrary runs host DLL-notification code (#434).
	target_plugin_preload_runtime_core_dll();
	target_plugin_sanitized_preload(wpath);
	U_LOG_W("conversion plugin loader:   %s: loading provider binary %s", e->id, e->binary_path);

	HMODULE dll = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (dll == NULL) {
		e->status = XRT_CONV_LOAD_LOAD_FAILED;
		snprintf(e->reason, sizeof(e->reason), "LoadLibrary failed (err=%lu)", GetLastError());
		U_LOG_W("conversion plugin loader:   %s: LoadLibrary(%s) failed (err=%lu).", e->id, e->binary_path,
		        GetLastError());
		return false;
	}

	xrt_conversion_plugin_negotiate_fn_t fn =
	    (xrt_conversion_plugin_negotiate_fn_t)(void *)GetProcAddress(dll, XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
	if (fn == NULL) {
		e->status = XRT_CONV_LOAD_NO_ENTRYPOINT;
		snprintf(e->reason, sizeof(e->reason), "no '%s' export (not a conversion provider)",
		         XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		U_LOG_W("conversion plugin loader:   %s: missing entry point '%s' — skipping.", e->id,
		        XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		FreeLibrary(dll);
		return false;
	}

	e->module = (void *)dll;
	*out_fn = fn;
	return true;
}

static void
conv_unload_module(void *module)
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

/*
 * Each platform's conv_scan_dir collects one directory's matching files,
 * orders them by file name, and merges them into the output (earlier roots
 * shadow later ones).
 */
#if defined(XRT_OS_ANDROID)

/*!
 * Android: `libdxrc<NNN>_<id>.so`. ProbeOrder lives in the file name because
 * `jniLibs` JSON is unreadable at load time (§3.6); id / display name come
 * from the file name until negotiate.
 */
static int
conv_scan_dir(const char *dir, struct xrt_conversion_plugin_entry *out, int count, int max)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		return count;
	}
	struct xrt_conversion_plugin_entry *tmp =
	    (struct xrt_conversion_plugin_entry *)calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*tmp));
	if (tmp == NULL) {
		closedir(d);
		return count;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < XRT_CONV_LOADER_MAX_ENTRIES) {
		uint32_t order = 0;
		char id[64];
		if (!target_plugin_parse_android_filename(de->d_name, TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, &order,
		                                          id, sizeof(id))) {
			continue;
		}
		struct xrt_conversion_plugin_entry *e = &tmp[n++];
		conv_entry_init(e);
		conv_copy(e->id, sizeof(e->id), id);
		e->probe_order = order;
		snprintf(e->binary_path, sizeof(e->binary_path), "%s/%s", dir, de->d_name);
		conv_copy(e->source, sizeof(e->source), e->binary_path);
		conv_entry_finish(e);
	}
	closedir(d);
	// Deterministic within a directory: readdir order is not.
	target_conversion_plugin_sort(tmp, n);
	count = target_conversion_plugin_merge(out, count, max, tmp, n);
	free(tmp);
	return count;
}

static int
conv_discover(struct xrt_conversion_plugin_entry *out, int max)
{
	int count = 0;
	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		count = conv_scan_dir(override, out, count, max);
	}
	Dl_info info;
	if (dladdr((void *)&conv_discover, &info) != 0 && info.dli_fname != NULL) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s", info.dli_fname);
		char *slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			count = conv_scan_dir(dir, out, count, max);
		}
	}
	return count;
}

#else /* macOS / desktop Linux */

static char *
conv_read_file(const char *path)
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
conv_compare_source(const void *a, const void *b)
{
	return strcmp(((const struct xrt_conversion_plugin_entry *)a)->source,
	              ((const struct xrt_conversion_plugin_entry *)b)->source);
}

static int
conv_scan_dir(const char *dir, struct xrt_conversion_plugin_entry *out, int count, int max)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		return count;
	}
	struct xrt_conversion_plugin_entry *tmp =
	    (struct xrt_conversion_plugin_entry *)calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*tmp));
	if (tmp == NULL) {
		closedir(d);
		return count;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < XRT_CONV_LOADER_MAX_ENTRIES) {
		// The suffix is the router: DP and input manifests share this dir.
		if (target_plugin_classify_manifest(de->d_name) != TARGET_PLUGIN_MANIFEST_CONVERSION) {
			continue;
		}
		char path[XRT_CONV_LOADER_PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		char *text = conv_read_file(path);
		struct xrt_conversion_plugin_entry *e = &tmp[n++];
		if (!target_conversion_plugin_parse_manifest_json(text, path, e)) {
			U_LOG_I("conversion plugin loader: %s: %s.", path, e->reason);
		}
		free(text);
	}
	closedir(d);
	// Within a root the first FILE NAME wins a duplicate id, not readdir order.
	qsort(tmp, (size_t)n, sizeof(tmp[0]), conv_compare_source);
	count = target_conversion_plugin_merge(out, count, max, tmp, n);
	free(tmp);
	return count;
}

static int
conv_discover(struct xrt_conversion_plugin_entry *out, int max)
{
	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));
	int count = 0;
	for (int r = 0; r < n_roots; r++) {
		count = conv_scan_dir(roots[r], out, count, max);
	}
	return count;
}

#endif /* Android vs desktop POSIX */

static bool
conv_load_binary(struct xrt_conversion_plugin_entry *e, xrt_conversion_plugin_negotiate_fn_t *out_fn)
{
	*out_fn = NULL;
	U_LOG_W("conversion plugin loader:   %s: loading provider binary %s", e->id, e->binary_path);
	/* RTLD_LOCAL keeps the provider's symbols private; aux symbols resolve
	 * via the runtime library already in the process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		const char *err = dlerror();
		e->status = XRT_CONV_LOAD_LOAD_FAILED;
		snprintf(e->reason, sizeof(e->reason), "dlopen failed: %s", err != NULL ? err : "?");
		U_LOG_W("conversion plugin loader:   %s: dlopen(%s) failed: %s.", e->id, e->binary_path,
		        err != NULL ? err : "?");
		return false;
	}
	dlerror();
	void *sym = dlsym(handle, XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
	if (sym == NULL || dlerror() != NULL) {
		e->status = XRT_CONV_LOAD_NO_ENTRYPOINT;
		snprintf(e->reason, sizeof(e->reason), "no '%s' export (not a conversion provider)",
		         XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		U_LOG_W("conversion plugin loader:   %s: missing entry point '%s' — skipping.", e->id,
		        XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		dlclose(handle);
		return false;
	}
	e->module = handle;
	*out_fn = (xrt_conversion_plugin_negotiate_fn_t)sym;
	return true;
}

static void
conv_unload_module(void *module)
{
	if (module != NULL) {
		dlclose(module);
	}
}

#endif /* platform */


/*
 *
 * Public iface.
 *
 */

int
target_conversion_plugin_enumerate(struct xrt_conversion_plugin_entry *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}
	int n = conv_discover(out, max);
	target_conversion_plugin_apply_kill_switches(out, n, getenv("DXR_CONV_PROVIDERS"),
	                                             getenv("DXR_CONV_EXCLUSIVE"));
	target_conversion_plugin_sort(out, n);
	U_LOG_I("conversion plugin loader: %d registered provider(s).", n);
	return n;
}

bool
target_conversion_plugin_negotiate_one(struct xrt_conversion_plugin_entry *e,
                                       const struct xrt_conversion_plugin_host_iface *host)
{
	if (e == NULL) {
		return false;
	}
	if (e->status == XRT_CONV_LOAD_NEGOTIATED && e->iface != NULL) {
		return true;
	}
	if ((e->flags & XRT_CONV_ENTRY_FLAG_MALFORMED) != 0) {
		e->status = XRT_CONV_LOAD_MALFORMED;
		U_LOG_W("conversion plugin loader:   %s: not loaded — %s.", e->id, e->reason);
		return false;
	}
	if ((e->flags & XRT_CONV_ENTRY_FLAG_DISABLED) != 0) {
		e->status = XRT_CONV_LOAD_DISABLED;
		conv_copy(e->reason, sizeof(e->reason), "disabled (Enabled = 0)");
		U_LOG_I("conversion plugin loader:   %s: disabled (Enabled=0) — skipping.", e->id);
		return false;
	}
	if ((e->flags & XRT_CONV_ENTRY_FLAG_SUPPRESSED) != 0) {
		e->status = XRT_CONV_LOAD_SUPPRESSED;
		U_LOG_I("conversion plugin loader:   %s: %s — skipping.", e->id,
		        e->reason[0] != '\0' ? e->reason : "suppressed");
		return false;
	}

	e->reason[0] = '\0';
	xrt_conversion_plugin_negotiate_fn_t fn = NULL;
	if (!conv_load_binary(e, &fn)) {
		e->iface = NULL;
		e->module = NULL;
		return false;
	}
	if (!target_conversion_plugin_negotiate_with(e, fn, host)) {
		conv_unload_module(e->module);
		e->module = NULL;
		e->iface = NULL;
		return false;
	}

	U_LOG_W(
	    "conversion plugin loader: negotiated conversion provider: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u hosting=%s path=%s",
	    e->id, e->iface->display_name != NULL ? e->iface->display_name : e->display_name,
	    e->iface->vendor != NULL ? e->iface->vendor : e->vendor, e->version, e->plugin_api_version, e->probe_order,
	    e->hosting_raw[0] != '\0' ? e->hosting_raw : "auto", e->binary_path);
	return true;
}

void
target_conversion_plugin_unload(struct xrt_conversion_plugin_entry *e)
{
	if (e == NULL) {
		return;
	}
	conv_unload_module(e->module);
	e->module = NULL;
	e->iface = NULL;
	if (e->status == XRT_CONV_LOAD_NEGOTIATED) {
		e->status = XRT_CONV_LOAD_NOT_LOADED;
		e->reason[0] = '\0';
	}
}
