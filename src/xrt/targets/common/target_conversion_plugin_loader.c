// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry / manifest / filename discovery + negotiation for
 *         conversion-provider plug-ins (ADR-053).
 *
 * The conversion-provider class loader, written as ADR-054 N2 intends:
 * discovery, the #952 / #328 / #434 load path and the ADR-020 ABI gate are the
 * shared component loader's (`target_component_loader.h`), driven by this
 * class's table row below. What is left here is what only this class knows:
 * its row fields (`Hosting`, `Supersedes`, `Packs`), its reserved policy
 * subkeys, its kill switches, the default host iface, and the
 * `xrt_conversion_plugin_entry` the conversion core (#2000) consumes. It
 * keeps no global state and selects nothing: it returns an entry array, and
 * the core probes, describes and selects.
 *
 * Contract: docs/specs/runtime/conversion-provider-discovery.md §3-§4.
 *
 * @ingroup target_common
 */

#include "target_conversion_plugin_loader.h"
#include "target_component_loader.h"

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "util/u_json.h"
#include "util/u_logging.h"

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


/*
 *
 * The class row (ADR-054 §2) and its private row fields.
 *
 */

/*!
 * What this class reads from a row beyond the common fields. Lives in the
 * shared entry's `extra` bytes between the shared reader and the mapping
 * into @ref xrt_conversion_plugin_entry.
 */
struct conv_extra
{
	char hosting_raw[32];
	char supersedes[64];
	uint32_t pack_count;
	char packs[XRT_CONV_LOADER_MAX_PACKS][XRT_CONV_LOADER_PATH_MAX];
};

static void
conv_read_extra(const struct target_component_class *cls,
                const void *raw,
                enum target_component_raw_kind raw_kind,
                struct target_component_entry *e);

static const struct target_component_class g_conv_class = {
    .name = "conversion provider",
    .log_tag = "conversion plugin loader",
    .entrypoint = XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME,
    .abi_current = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT,
    .registry_root = "Software\\DisplayXR\\ConversionProviders",
    .manifest_kind = TARGET_PLUGIN_MANIFEST_CONVERSION,
    .android_prefix = TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION,
    .is_reserved_subkey = target_conversion_plugin_is_reserved_subkey,
    .read_extra = conv_read_extra,
};

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

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

static void
conv_reg_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[XRT_CONV_LOADER_PATH_MAX];
	DWORD bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, wbuf, &bytes) != ERROR_SUCCESS) {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
		return;
	}
	conv_w2u(wbuf, dst, dst_size);
}

static void
conv_reg_packs(HKEY key, const char *id, struct conv_extra *x)
{
	wchar_t wbuf[XRT_CONV_LOADER_MAX_PACKS * XRT_CONV_LOADER_PATH_MAX + 2];
	DWORD bytes = sizeof(wbuf) - 2 * sizeof(wchar_t);
	memset(wbuf, 0, sizeof(wbuf));
	if (RegGetValueW(key, NULL, L"Packs", RRF_RT_REG_MULTI_SZ, NULL, wbuf, &bytes) != ERROR_SUCCESS) {
		return;
	}
	for (const wchar_t *p = wbuf; *p != L'\0'; p += wcslen(p) + 1) {
		if (x->pack_count >= XRT_CONV_LOADER_MAX_PACKS) {
			U_LOG_I("conversion plugin loader: %s: more than %d packs — truncating.", id,
			        XRT_CONV_LOADER_MAX_PACKS);
			break;
		}
		conv_w2u(p, x->packs[x->pack_count++], XRT_CONV_LOADER_PATH_MAX);
	}
}

#endif /* XRT_OS_WINDOWS */

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

static void
conv_read_extra(const struct target_component_class *cls,
                const void *raw,
                enum target_component_raw_kind raw_kind,
                struct target_component_entry *e)
{
	(void)cls;
	struct conv_extra *x = (struct conv_extra *)e->extra;
	// The shared entry reserves TARGET_COMPONENT_EXTRA_BYTES; this class's
	// private record must fit in it. (A build-time check, not a runtime one.)
	typedef char conv_extra_fits[sizeof(struct conv_extra) <= TARGET_COMPONENT_EXTRA_BYTES ? 1 : -1];
	(void)sizeof(conv_extra_fits);

	switch (raw_kind) {
#ifdef XRT_OS_WINDOWS
	case TARGET_COMPONENT_RAW_REGISTRY_KEY: {
		HKEY sub = (HKEY)(uintptr_t)raw;
		conv_reg_string(sub, L"Hosting", x->hosting_raw, (int)sizeof(x->hosting_raw));
		conv_reg_string(sub, L"Supersedes", x->supersedes, (int)sizeof(x->supersedes));
		conv_reg_packs(sub, e->id, x);
		break;
	}
#endif
	case TARGET_COMPONENT_RAW_MANIFEST_PLUGIN: {
		const cJSON *plugin = (const cJSON *)raw;
		conv_json_str(plugin, "hosting", x->hosting_raw, sizeof(x->hosting_raw));
		conv_json_str(plugin, "supersedes", x->supersedes, sizeof(x->supersedes));
		const cJSON *packs = u_json_get(plugin, "packs");
		if (packs != NULL) {
			if (!cJSON_IsArray(packs)) {
				target_component_mark_malformed(e, "'plugin.packs' is not an array");
				return;
			}
			const cJSON *p = NULL;
			cJSON_ArrayForEach(p, packs)
			{
				if (!cJSON_IsString(p) || p->valuestring == NULL) {
					continue;
				}
				if (x->pack_count >= XRT_CONV_LOADER_MAX_PACKS) {
					U_LOG_I("conversion plugin loader: %s: more than %d packs — truncating.",
					        e->source, XRT_CONV_LOADER_MAX_PACKS);
					break;
				}
				conv_copy(x->packs[x->pack_count++], XRT_CONV_LOADER_PATH_MAX, p->valuestring);
			}
		}
		break;
	}
	default: break;
	}
}

/*!
 * Defaults shared by every row source, applied after the entry is zeroed.
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
 * One shared-loader row -> this class's entry. The shared flags map onto
 * the conversion flags + status one to one; the class-private bytes become
 * Hosting / Supersedes / Packs.
 */
static void
conv_from_common(const struct target_component_entry *c, struct xrt_conversion_plugin_entry *e)
{
	conv_entry_init(e);
	conv_copy(e->id, sizeof(e->id), c->id);
	conv_copy(e->display_name, sizeof(e->display_name), c->display_name);
	conv_copy(e->vendor, sizeof(e->vendor), c->vendor);
	conv_copy(e->version, sizeof(e->version), c->version);
	conv_copy(e->binary_path, sizeof(e->binary_path), c->binary_path);
	conv_copy(e->source, sizeof(e->source), c->source);
	e->probe_order = c->probe_order;
	e->enabled = c->enabled;

	for (uint32_t i = 0; i < c->setting_count && i < XRT_CONV_LOADER_MAX_SETTINGS; i++) {
		conv_copy(e->settings[i].key, sizeof(e->settings[i].key), c->settings[i].key);
		conv_copy(e->settings[i].value, sizeof(e->settings[i].value), c->settings[i].value);
		e->setting_count++;
	}

	const struct conv_extra *x = (const struct conv_extra *)c->extra;
	conv_copy(e->hosting_raw, sizeof(e->hosting_raw), x->hosting_raw);
	conv_copy(e->supersedes, sizeof(e->supersedes), x->supersedes);
	for (uint32_t i = 0; i < x->pack_count && i < XRT_CONV_LOADER_MAX_PACKS; i++) {
		conv_copy(e->packs[i], XRT_CONV_LOADER_PATH_MAX, x->packs[i]);
		e->pack_count++;
	}

	bool known = true;
	e->hosting = target_conversion_plugin_parse_hosting(e->hosting_raw, &known);
	if (!known) {
		e->flags |= XRT_CONV_ENTRY_FLAG_HOSTING_UNKNOWN;
	}

	if ((c->flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) != 0) {
		e->flags |= XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE;
	}
	if ((c->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
		conv_set_malformed(e, c->reason);
	}
	if (!e->enabled) {
		e->flags |= XRT_CONV_ENTRY_FLAG_DISABLED;
		if (conv_is_usable(e)) {
			e->status = XRT_CONV_LOAD_DISABLED;
			conv_copy(e->reason, sizeof(e->reason), "disabled (Enabled = 0)");
		}
	}
}

/*!
 * Map a whole shared-loader array into @p out (same count, same order).
 * The shared array is heap-allocated here because its rows are ~11 KB.
 */
static int
conv_map_all(struct target_component_entry *common, int n, struct xrt_conversion_plugin_entry *out)
{
	for (int i = 0; i < n; i++) {
		conv_from_common(&common[i], &out[i]);
	}
	return n;
}


/*
 *
 * Pure helpers (every platform).
 *
 */

const char *const target_conversion_plugin_reserved_subkeys[] = XRT_CONV_RESERVED_SUBKEYS_INIT;

bool
target_conversion_plugin_is_reserved_subkey(const char *name)
{
	if (name == NULL) {
		return false;
	}
	for (int i = 0; target_conversion_plugin_reserved_subkeys[i] != NULL; i++) {
		if (conv_streq_nocase(name, target_conversion_plugin_reserved_subkeys[i])) {
			return true;
		}
	}
	return false;
}

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

bool
target_conversion_plugin_hosting_env_name(const char *id, char *buf, size_t buf_size)
{
	if (id == NULL || id[0] == '\0' || buf == NULL || buf_size == 0) {
		return false;
	}
	const int n = snprintf(buf, buf_size, "DXR_CONV_%s_HOSTING", id);
	if (n < 0 || (size_t)n >= buf_size) {
		buf[0] = '\0';
		return false;
	}
	for (char *c = buf; *c != '\0'; c++) {
		if (*c == '-' || *c == '.') {
			*c = '_';
		} else if (*c >= 'a' && *c <= 'z') {
			*c = (char)(*c - 'a' + 'A');
		}
	}
	return true;
}

uint32_t
target_conversion_plugin_resolve_hosting(uint32_t registered, const char *env_value, bool *out_from_env)
{
	uint32_t h = registered;
	bool from_env = false;
	if (env_value != NULL && env_value[0] != '\0') {
		bool known = false;
		const uint32_t e = target_conversion_plugin_parse_hosting(env_value, &known);
		if (known) {
			h = e;
			from_env = true;
		}
	}
	if (out_from_env != NULL) {
		*out_from_env = from_env;
	}
	return h == XRT_CONV_HOSTING_IN_SERVICE ? XRT_CONV_HOSTING_IN_SERVICE : XRT_CONV_HOSTING_SATELLITE;
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

bool
target_conversion_plugin_check_provenance(const char *binary_path,
                                          uint32_t *out_provenance,
                                          char *out_reason,
                                          size_t reason_cap,
                                          char *out_signer,
                                          size_t signer_cap)
{
	if (out_provenance != NULL) {
		*out_provenance = TARGET_COMPONENT_PROVENANCE_UNKNOWN;
	}
	if (out_reason != NULL && reason_cap > 0) {
		out_reason[0] = '\0';
	}
	if (out_signer != NULL && signer_cap > 0) {
		out_signer[0] = '\0';
	}
	if (binary_path == NULL || binary_path[0] == '\0') {
		return false;
	}
	// The shared check wants a row (~11 KB): heap, never a hot stack.
	struct target_component_entry *c = (struct target_component_entry *)calloc(1, sizeof(*c));
	if (c == NULL) {
		return false;
	}
	target_component_entry_init(c);
	conv_copy(c->binary_path, sizeof(c->binary_path), binary_path);
	const bool ok = target_component_check_provenance(&g_conv_class, c);
	if (out_provenance != NULL) {
		*out_provenance = c->provenance;
	}
	if (out_reason != NULL && reason_cap > 0) {
		conv_copy(out_reason, reason_cap, c->provenance_reason);
	}
	if (out_signer != NULL && signer_cap > 0) {
		conv_copy(out_signer, signer_cap, c->signer);
	}
	free(c);
	return ok;
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

bool
target_conversion_plugin_parse_manifest_json(const char *json_text,
                                             const char *source,
                                             struct xrt_conversion_plugin_entry *out)
{
	if (out == NULL) {
		return false;
	}
	struct target_component_entry *c = (struct target_component_entry *)calloc(1, sizeof(*c));
	if (c == NULL) {
		conv_entry_init(out);
		conv_set_malformed(out, "out of memory");
		return false;
	}
	const bool ok = target_component_parse_manifest_json(&g_conv_class, json_text, source, c);
	conv_from_common(c, out);
	free(c);
	return ok;
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

	// ADR-020 rule 3, classified the same way for every class.
	switch (target_component_abi_gate(&g_conv_class, e->id, xret, iface, plugin_version, e->reason,
	                                  sizeof(e->reason))) {
	case TARGET_COMPONENT_ABI_DECLINED: e->status = XRT_CONV_LOAD_DECLINED; return false;
	case TARGET_COMPONENT_ABI_MISMATCH:
		e->status = XRT_CONV_LOAD_ABI_MISMATCH;
		snprintf(e->reason, sizeof(e->reason), "ABI major mismatch: plugin_api=%u, runtime expects %u",
		         plugin_version, (unsigned)XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
		return false;
	case TARGET_COMPONENT_ABI_NEGOTIATE_FAILED: e->status = XRT_CONV_LOAD_NEGOTIATE_FAILED; return false;
	case TARGET_COMPONENT_ABI_OK: break;
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
 * Public iface.
 *
 */

#ifdef _WIN32
int
target_conversion_plugin_enumerate_registry_key(bool current_user,
                                                const char *subkey,
                                                struct xrt_conversion_plugin_entry *out,
                                                int max)
{
	if (subkey == NULL || out == NULL || max <= 0) {
		return 0;
	}
	if (max > TARGET_COMPONENT_MAX_ENTRIES) {
		max = TARGET_COMPONENT_MAX_ENTRIES;
	}
	struct target_component_entry *common =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*common));
	if (common == NULL) {
		return 0;
	}
	int n = target_component_enumerate_registry(&g_conv_class, current_user, subkey, common, max);
	n = conv_map_all(common, n, out);
	free(common);
	return n;
}
#endif

int
target_conversion_plugin_enumerate(struct xrt_conversion_plugin_entry *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}
	if (max > TARGET_COMPONENT_MAX_ENTRIES) {
		max = TARGET_COMPONENT_MAX_ENTRIES;
	}
	struct target_component_entry *common =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*common));
	if (common == NULL) {
		return 0;
	}
	int n = target_component_enumerate(&g_conv_class, common, max);
	n = conv_map_all(common, n, out);
	free(common);

	target_conversion_plugin_apply_kill_switches(out, n, getenv("DXR_CONV_PROVIDERS"),
	                                             getenv("DXR_CONV_EXCLUSIVE"));
	// Already sorted by the shared loader; re-run for the class's own tie
	// flags (idempotent).
	target_conversion_plugin_sort(out, n);
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
	e->iface = NULL;
	e->module = NULL;

	// The shared load path wants a shared entry; only id + binary_path matter to it.
	struct target_component_entry *c = (struct target_component_entry *)calloc(1, sizeof(*c));
	if (c == NULL) {
		e->status = XRT_CONV_LOAD_LOAD_FAILED;
		conv_copy(e->reason, sizeof(e->reason), "out of memory");
		return false;
	}
	target_component_entry_init(c);
	conv_copy(c->id, sizeof(c->id), e->id);
	conv_copy(c->binary_path, sizeof(c->binary_path), e->binary_path);
	conv_copy(c->source, sizeof(c->source), e->source);

	void *module = NULL;
	void *fn = NULL;
	const enum target_component_load_result lr =
	    target_component_load_binary(&g_conv_class, c, &module, &fn, NULL, e->reason, sizeof(e->reason));
	free(c);
	switch (lr) {
	case TARGET_COMPONENT_LOAD_OK: break;
	case TARGET_COMPONENT_LOAD_PATH_REFUSED: e->status = XRT_CONV_LOAD_PATH_REFUSED; return false;
	case TARGET_COMPONENT_LOAD_NO_ENTRY_POINT: e->status = XRT_CONV_LOAD_NO_ENTRYPOINT; return false;
	case TARGET_COMPONENT_LOAD_BINARY_MISSING:
	case TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING:
	case TARGET_COMPONENT_LOAD_FAILED:
	default: e->status = XRT_CONV_LOAD_LOAD_FAILED; return false;
	}

	e->module = module;
	if (!target_conversion_plugin_negotiate_with(e, (xrt_conversion_plugin_negotiate_fn_t)fn, host)) {
		target_component_unload(e->module);
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
	target_component_unload(e->module);
	e->module = NULL;
	e->iface = NULL;
	if (e->status == XRT_CONV_LOAD_NEGOTIATED) {
		e->status = XRT_CONV_LOAD_NOT_LOADED;
		e->reason[0] = '\0';
	}
}
