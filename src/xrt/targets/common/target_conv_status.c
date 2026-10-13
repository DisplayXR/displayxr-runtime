// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless conversion-provider diagnostics (ADR-053 §7): enumerate,
 *         negotiate, probe, describe, select; never warm.
 * @ingroup target_common
 */

#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_plugin.h"

#include "conversion/conv_dp_lift_describe.h"
#include "conversion/conv_registry.h"
#include "conversion/conv_readiness.h"
#include "conversion/conv_select.h"

#include "os/os_time.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define DP_LIFT_ID CONV_DP_LIFT_ID
#define DP_LIFT_PROBE_ORDER CONV_DP_LIFT_PROBE_ORDER

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	(void)snprintf(dst, cap, "%s", src);
}

static void
copy_upper(char *dst, size_t cap, const char *src)
{
	copy_str(dst, cap, src);
	for (char *c = dst; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)toupper((unsigned char)*c); // "abi-mismatch" -> "ABI_MISMATCH"
	}
}

bool
target_conv_is_policy_subkey(const char *name)
{
	if (name == NULL) {
		return false;
	}
	static const char *const k_names[] = {"Policy", "PreferredProvider", "Prewarm"};
	for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
#ifdef XRT_OS_WINDOWS
		if (_stricmp(name, k_names[i]) == 0) {
#else
		if (strcmp(name, k_names[i]) == 0) {
#endif
			return true;
		}
	}
	return false;
}

static const char *
cap_error_str(uint32_t err)
{
	switch (err) {
	case CONV_CAP_OK: return "";
	case CONV_CAP_ERR_CAP_ID_ZERO: return "cap_id is 0";
	case CONV_CAP_ERR_TASK: return "unknown task";
	case CONV_CAP_ERR_LANES: return "lanes 0 or unknown lane bit";
	case CONV_CAP_ERR_VIEWS: return "min_views > max_views or > 8";
	case CONV_CAP_ERR_VIEWPOINTS: return "VIEWS_* without EXPLICIT_VIEWPOINTS";
	case CONV_CAP_ERR_LICENCE: return "unknown licence class";
	case CONV_CAP_ERR_DUPLICATE_CAP_ID: return "duplicate cap_id";
	case CONV_CAP_ERR_STATE: return "get_state out of range";
	case CONV_CAP_ERR_COUNT: return "more than 32 capabilities";
	case CONV_CAP_ERR_DESCRIBE_FAILED: return "describe_cap failed";
	default: return "invalid descriptor";
	}
}

static const char *
verdict_str(uint32_t v)
{
	switch (v) {
	case CONV_ROW_OTHER_TASK: return "other-task";
	case CONV_ROW_DROP_DISABLED: return "drop-disabled";
	case CONV_ROW_DROP_UNUSABLE: return "drop-unusable";
	case CONV_ROW_DROP_HIDDEN: return "drop-hidden";
	case CONV_ROW_DROP_LICENCE: return "drop-licence";
	case CONV_ROW_DROP_INPUT: return "drop-input";
	case CONV_ROW_DROP_LANE: return "drop-lane";
	case CONV_ROW_DROP_VIEWS: return "drop-views";
	case CONV_ROW_DROP_VIEWPOINTS: return "drop-viewpoints";
	case CONV_ROW_FELL_THROUGH: return "fell-through";
	case CONV_ROW_SELECTED: return "selected";
	case CONV_ROW_BEHIND: return "behind";
	default: return "?";
	}
}


/*
 *
 * Policy (spec §3.1).
 *
 */

#ifdef XRT_OS_WINDOWS
//! DWORD under HKCU/HKLM <root>[\sub]; -1 when absent.
static int
reg_dword(HKEY hive, const char *sub, const char *value)
{
	char path[256];
	(void)snprintf(path, sizeof(path), "%s%s%s", TARGET_CONV_REG_ROOT, sub != NULL ? "\\" : "",
	               sub != NULL ? sub : "");
	DWORD v = 0;
	DWORD sz = sizeof(v);
	DWORD flags = RRF_RT_REG_DWORD | (hive == HKEY_LOCAL_MACHINE ? RRF_SUBKEY_WOW6464KEY : 0);
	if (RegGetValueA(hive, path, value, flags, NULL, &v, &sz) != ERROR_SUCCESS) {
		return -1;
	}
	return (int)v;
}

static bool
reg_sz(HKEY hive, const char *sub, const char *value, char *out, DWORD cap)
{
	char path[256];
	(void)snprintf(path, sizeof(path), "%s\\%s", TARGET_CONV_REG_ROOT, sub);
	out[0] = '\0';
	DWORD sz = cap;
	DWORD flags = RRF_RT_REG_SZ | (hive == HKEY_LOCAL_MACHINE ? RRF_SUBKEY_WOW6464KEY : 0);
	if (RegGetValueA(hive, path, value, flags, NULL, out, &sz) != ERROR_SUCCESS) {
		out[0] = '\0';
		return false;
	}
	return out[0] != '\0';
}

static void
read_enabled_overrides(struct target_conv_policy *out)
{
	HKEY root = NULL;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, TARGET_CONV_REG_ROOT, 0, KEY_READ, &root) != ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0; out->override_count < TARGET_CONV_POLICY_MAX_OVERRIDES; i++) {
		char name[64];
		DWORD len = sizeof(name);
		if (RegEnumKeyExA(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
			break;
		}
		if (target_conv_is_policy_subkey(name)) {
			continue;
		}
		const int en = reg_dword(HKEY_CURRENT_USER, name, "Enabled");
		if (en < 0) {
			continue;
		}
		copy_str(out->overrides[out->override_count].id, sizeof(out->overrides[0].id), name);
		out->overrides[out->override_count].enabled = en != 0;
		out->override_count++;
	}
	RegCloseKey(root);
}
#endif

void
target_conv_policy_read(struct target_conv_policy *out)
{
	memset(out, 0, sizeof(*out));
#ifdef XRT_OS_WINDOWS
	out->policy_lock = reg_dword(HKEY_LOCAL_MACHINE, "Policy", "Lock") == 1;
	out->allow_research_only =
	    conv_select_resolve_research_only(reg_dword(HKEY_CURRENT_USER, NULL, "AllowResearchOnly"),
	                                      reg_dword(HKEY_LOCAL_MACHINE, NULL, "AllowResearchOnly"), out->policy_lock);
	for (uint32_t task = 1; task < TARGET_CONV_POLICY_TASKS; task++) {
		if (!conv_task_is_served(task)) {
			continue;
		}
		const char *name = conv_task_name(task);
		char hkcu[96], hklm[96];
		(void)reg_sz(HKEY_CURRENT_USER, "PreferredProvider", name, hkcu, sizeof(hkcu));
		(void)reg_sz(HKEY_LOCAL_MACHINE, "PreferredProvider", name, hklm, sizeof(hklm));
		copy_str(out->preferred[task], sizeof(out->preferred[task]),
		         conv_select_resolve_preference(hkcu, hklm, out->policy_lock));
	}
	if (!out->policy_lock) {
		read_enabled_overrides(out);
	}
#endif
}

static int
policy_enabled_override(const struct target_conv_policy *p, const char *id)
{
	if (p == NULL || p->policy_lock) {
		return -1;
	}
	for (uint32_t i = 0; i < p->override_count; i++) {
		if (strcmp(p->overrides[i].id, id) == 0) {
			return p->overrides[i].enabled ? 1 : 0;
		}
	}
	return -1;
}


bool
target_conv_policy_apply_override(const struct target_conv_policy *policy, struct xrt_conversion_plugin_entry *e)
{
	const int ov = policy_enabled_override(policy, e->id);
	if (ov == 0) {
		e->enabled = false;
		if (e->status == XRT_CONV_LOAD_NOT_LOADED) {
			e->status = XRT_CONV_LOAD_DISABLED;
			copy_str(e->reason, sizeof(e->reason), "disabled for this user (HKCU Enabled = 0)");
		}
	} else if (ov == 1) {
		e->enabled = true;
		e->flags &= ~XRT_CONV_ENTRY_FLAG_DISABLED;
		if (e->status == XRT_CONV_LOAD_DISABLED) {
			e->status = XRT_CONV_LOAD_NOT_LOADED;
			e->reason[0] = '\0';
		}
	}
	return e->enabled;
}

bool
target_conv_policy_enabled(const struct target_conv_policy *policy, const char *id, bool dflt)
{
	const int ov = policy_enabled_override(policy, id);
	return ov < 0 ? dflt : ov == 1;
}


/*
 *
 * The static dp-lift describe (spec §4.5). Runtime-internal: no DLL, no
 * negotiate, no factory call. The capabilities come from the one static
 * describe the in-service adapter refines (conv_dp_lift_describe_static,
 * #2030), so headless and in-service rows never disagree.
 *
 */

static bool
dpl_status(struct xrt_conv_provider_status *out)
{
	out->state = 1; // XRT_PLUGIN_PLATFORM_STATE_READY: the display plug-in is loaded
	copy_str(out->hint, sizeof(out->hint), "static describe: the lift module was not instantiated");
	return true;
}

static xrt_result_t
dpl_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
dpl_describe(struct xrt_conversion_plugin_instance *inst,
             uint32_t index,
             struct xrt_conv_capability *out,
             uint32_t *out_count)
{
	(void)inst;
	if (out_count != NULL) {
		*out_count = CONV_DP_LIFT_CAP_COUNT;
	}
	if (out == NULL) {
		return XRT_SUCCESS;
	}
	return conv_dp_lift_describe_static(index, out) ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static xrt_result_t
dpl_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	(void)inst;
	(void)cap_id;
	out->state = XRT_CONV_STATE_INSTALLED; // "not yet asked" is never terminal
	return XRT_SUCCESS;
}

static const struct xrt_conversion_plugin_iface k_dp_lift_iface = {
    .struct_size = sizeof(struct xrt_conversion_plugin_iface),
    .id = DP_LIFT_ID,
    .display_name = "Display plug-in lift slots (legacy adapter)",
    .vendor = NULL,
    .version = NULL,
    .get_provider_status = dpl_status,
    .probe = dpl_probe,
    .describe_cap = dpl_describe,
    .get_state = dpl_get_state,
};

//! Does @p dp expose the lift-only factory (struct_size-gated)?
static bool
dp_has_lift_factory(const struct xrt_plugin_iface *dp)
{
	if (dp == NULL) {
		return false;
	}
	const size_t need = offsetof(struct xrt_plugin_iface, create_dp_d3d11_lift) + sizeof(dp->create_dp_d3d11_lift);
	return dp->struct_size >= need && dp->create_dp_d3d11_lift != NULL;
}


/*
 *
 * The headless host (spec §2.1 host iface). Only get_setting differs from the
 * loader's default host: it answers `DXR_CONV_<ID>_<KEY>` from the environment,
 * then the provider's registry `Settings\*` / manifest "settings" -- exactly the
 * keys the service host will serve -- so a provider that gates its probe on a
 * setting (the sim provider's ENABLE) is described here as it would be there.
 *
 * The callbacks carry no provider context, so the pass runs providers ONE AT A
 * TIME and points @ref g_headless_entry at the one being negotiated / probed /
 * described. Single-threaded by construction (CLI / tests); after the pass the
 * pointer is NULL and every lookup answers "absent".
 *
 */

static const struct xrt_conversion_plugin_entry *g_headless_entry = NULL;
static struct xrt_conversion_plugin_host_iface g_headless_host;
static bool g_headless_host_ready = false;

static bool
headless_get_setting(const char *key, char *buf, uint32_t buf_size)
{
	const struct xrt_conversion_plugin_entry *e = g_headless_entry;
	if (e == NULL || key == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	char env[160];
	size_t n = (size_t)snprintf(env, sizeof(env), "DXR_CONV_%s_%s", e->id, key);
	if (n >= sizeof(env)) {
		return false;
	}
	for (char *c = env; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)toupper((unsigned char)*c);
	}
	const char *v = getenv(env);
	if (v != NULL && v[0] != '\0') {
		copy_str(buf, buf_size, v);
		return true;
	}
	for (uint32_t i = 0; i < e->setting_count && i < XRT_CONV_LOADER_MAX_SETTINGS; i++) {
#ifdef XRT_OS_WINDOWS
		if (_stricmp(e->settings[i].key, key) == 0) {
#else
		if (strcmp(e->settings[i].key, key) == 0) {
#endif
			copy_str(buf, buf_size, e->settings[i].value);
			return true;
		}
	}
	return false;
}

const struct xrt_conversion_plugin_host_iface *
target_conv_status_headless_host(void)
{
	if (!g_headless_host_ready) {
		g_headless_host = *target_conversion_plugin_default_host();
		g_headless_host.get_setting = headless_get_setting;
		g_headless_host.host_flags |= 2u; // bit1: headless / CI
		g_headless_host_ready = true;
	}
	return &g_headless_host;
}


/*
 *
 * The pass.
 *
 */

//! Per-registry-provider facts the rows need beyond the registry's.
struct prov_extra
{
	char name[XRT_STATUS_NAME_MAX];
	char vendor[XRT_STATUS_NAME_MAX];
	char version[32];
	char load[24];
	char hint[128];
	uint32_t abi;
	bool licence_exempt;
	bool built_in;
	int entry; //!< index into the caller's entries, -1 for built-ins
};

static void
note_bad(struct target_conv_status *out, const char *id, const char *why)
{
	out->bad_descriptors++;
	if (out->bad_note[0] == '\0') {
		(void)snprintf(out->bad_note, sizeof(out->bad_note), "%s: %s", id, why);
	}
}

/*!
 * Overwrite each of provider @p pi's rows with the provider's own get_state
 * answer (headless shows what the provider says, never a warm), validating it.
 */
static void
apply_provider_states(struct conv_registry *reg, uint32_t pi, struct target_conv_status *out)
{
	struct conv_provider *p = &reg->providers[pi];
	if (p->iface == NULL || !XRT_CONV_IFACE_HAS(p->iface, get_state) || p->iface->get_state == NULL) {
		return;
	}
	for (uint32_t r = 0; r < reg->row_count; r++) {
		struct conv_cap_row *row = &reg->rows[r];
		if (row->provider != pi) {
			continue;
		}
		struct xrt_conv_state st;
		memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		if (p->iface->get_state(p->inst, row->cap.cap_id, &st) != XRT_SUCCESS) {
			continue;
		}
		if (conv_cap_validate_state(&st) != CONV_CAP_OK) {
			char why[64];
			(void)snprintf(why, sizeof(why), "cap %u: get_state answered %u (out of range)", row->cap.cap_id,
			               st.state);
			note_bad(out, p->id, why);
			if (p->bad_descriptors++ == 0) {
				p->first_error = CONV_CAP_ERR_STATE;
			}
			continue;
		}
		if (st.state != XRT_CONV_STATE_UNKNOWN) {
			row->rd.state = st.state;
			row->rd.reason = st.failure_reason;
			row->rd.progress = st.progress;
		}
	}
}

static void
fill_provider_row(const struct conv_registry *reg,
                  uint32_t pi,
                  const struct prov_extra *x,
                  const struct target_conv_policy *policy,
                  struct xrt_status_conv_provider *o)
{
	const struct conv_provider *p = &reg->providers[pi];
	memset(o, 0, sizeof(*o));
	copy_str(o->id, sizeof(o->id), p->id);
	copy_str(o->name, sizeof(o->name), x->name);
	copy_str(o->vendor, sizeof(o->vendor), x->vendor);
	copy_str(o->version, sizeof(o->version), x->version);
	copy_str(o->load, sizeof(o->load), x->load);
	copy_str(o->binary, sizeof(o->binary), p->binary_path);
	o->abi = x->abi;
	o->probe_order = p->probe_order;
	o->hosting = p->hosting == CONV_HOSTING_SATELLITE ? XRT_STATUS_CONV_HOSTING_SATELLITE
	                                                  : XRT_STATUS_CONV_HOSTING_IN_SERVICE;
	o->platform_state = p->platform_state;
	copy_str(o->hint, sizeof(o->hint), x->hint);
	o->enabled = p->enabled;
	o->fallback = (p->flags & CONV_PROV_FALLBACK) != 0;
	o->hidden = conv_registry_is_hidden(reg, pi);
	o->licence_exempt = x->licence_exempt;
	o->research_opt_in = policy != NULL && policy->allow_research_only;
	o->described = p->described;
	o->bad_descriptors = p->bad_descriptors;
	copy_str(o->descriptor_error, sizeof(o->descriptor_error), cap_error_str(p->first_error));
	for (uint32_t r = 0; r < reg->row_count; r++) {
		if (reg->rows[r].provider != pi) {
			continue;
		}
		if (o->cap_count == 0) {
			copy_str(o->backend, sizeof(o->backend), reg->rows[r].cap.backend);
			o->licence_class = reg->rows[r].cap.licence_class;
			copy_str(o->licence_spdx, sizeof(o->licence_spdx), reg->rows[r].cap.licence_spdx);
			o->adapter_luid = reg->rows[r].cap.device_luid;
		}
		o->cap_count++;
	}
}

static void
build_task_rows(const struct conv_registry *reg,
                const uint32_t *status_index, // registry provider -> status provider index (or NO_PROVIDER)
                const struct target_conv_policy *policy,
                struct target_conv_status *out)
{
	struct conv_select_row cand[CONV_REGISTRY_MAX_ROWS * 2];
	uint32_t cap_row[CONV_REGISTRY_MAX_ROWS * 2];
	uint32_t verdicts[CONV_REGISTRY_MAX_ROWS * 2];
	uint32_t order[CONV_REGISTRY_MAX_ROWS * 2];
	const uint32_t max = CONV_REGISTRY_MAX_ROWS * 2;

	for (uint32_t task = 1; task < XRT_CONV_TASK_RESERVED_FIRST; task++) {
		if (!conv_task_is_served(task)) {
			continue;
		}
		const uint32_t n = conv_registry_build_candidates(reg, task, true, cand, cap_row, max);
		if (n == 0) {
			continue;
		}
		// The registry's own rows carry the readiness; the select rows only copy it.
		struct conv_select_request req;
		memset(&req, 0, sizeof(req));
		req.task = task;
		// "Could this task be served at all": the smallest multi-view count.
		// A real create resolves view_count from the weave (spec §3.4).
		if (task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
			req.view_count = 2;
		}
		struct conv_select_policy pol;
		memset(&pol, 0, sizeof(pol));
		pol.preferred = policy != NULL && task < TARGET_CONV_POLICY_TASKS && policy->preferred[task][0] != '\0'
		                    ? policy->preferred[task]
		                    : NULL;
		pol.allow_research_only = policy != NULL && policy->allow_research_only;
		const struct conv_select_result res = conv_select(cand, n, &req, &pol, verdicts, order);

		// Ordered rows first (selection order), then the dropped ones.
		uint32_t rank_of[CONV_REGISTRY_MAX_ROWS * 2];
		for (uint32_t i = 0; i < n; i++) {
			rank_of[i] = XRT_STATUS_CONV_NOT_ORDERED;
		}
		for (uint32_t k = 0; k < res.candidate_count && k < n; k++) {
			rank_of[order[k]] = k;
		}
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t k = 0; k < n; k++) {
				const uint32_t i = pass == 0 ? (k < res.candidate_count ? order[k] : n) : k;
				if (i >= n) {
					continue;
				}
				if (pass == 1 && rank_of[i] != XRT_STATUS_CONV_NOT_ORDERED) {
					continue;
				}
				if (out->task_count >= XRT_STATUS_MAX_CONV_TASKS) {
					return;
				}
				const struct conv_select_row *c = &cand[i];
				const struct conv_cap_row *row = &reg->rows[cap_row[i]];
				struct xrt_status_conv_task *t = &out->tasks[out->task_count++];
				memset(t, 0, sizeof(*t));
				t->provider = status_index[row->provider];
				t->task = task;
				copy_str(t->task_name, sizeof(t->task_name), conv_task_name(task));
				t->cap_id = c->cap_id;
				t->composed = c->composed;
				t->selected = res.row == (int32_t)i;
				t->state = c->state;
				t->reason = c->reason;
				copy_upper(t->state_name, sizeof(t->state_name), conv_state_name(c->state));
				copy_str(t->reason_name, sizeof(t->reason_name), conv_reason_name(c->reason));
				t->progress = row->rd.progress;
				t->order = rank_of[i];
				copy_str(t->verdict, sizeof(t->verdict), verdict_str(verdicts[i]));
				t->lanes = c->lanes;
				t->min_views = c->min_views;
				t->max_views = c->max_views;
				t->licence_class = c->licence_class;
				copy_str(t->backend, sizeof(t->backend), row->cap.backend);
				copy_str(t->model_id, sizeof(t->model_id), row->cap.model_id);
				t->typical_latency_ms = row->cap.est_latency_ms;
			}
		}
	}
}

static void
fill_extra_from_entry(const struct xrt_conversion_plugin_entry *e, struct prov_extra *x)
{
	copy_str(x->name, sizeof(x->name), e->display_name);
	copy_str(x->vendor, sizeof(x->vendor), e->vendor);
	copy_str(x->version, sizeof(x->version), e->version);
	copy_upper(x->load, sizeof(x->load), target_conversion_plugin_status_str(e->status));
	copy_str(x->hint, sizeof(x->hint), e->reason);
	x->abi = e->plugin_api_version;
}

void
target_conv_status_build_from_entries(struct xrt_conversion_plugin_entry *entries,
                                      int count,
                                      const struct xrt_plugin_iface *active_dp,
                                      const struct target_conv_policy *policy,
                                      struct target_conv_status *out)
{
	memset(out, 0, sizeof(*out));
	if (policy != NULL) {
		out->policy = *policy;
	}
	struct conv_registry *reg = calloc(1, sizeof(*reg));
	struct prov_extra *extra = calloc(CONV_REGISTRY_MAX_PROVIDERS, sizeof(*extra));
	if (reg == NULL || extra == NULL) {
		free(reg);
		free(extra);
		return;
	}
	conv_registry_init(reg);
	const uint64_t now = os_monotonic_get_ns();

	for (int i = 0; i < count; i++) {
		struct xrt_conversion_plugin_entry *e = &entries[i];
		if (target_conv_is_policy_subkey(e->id)) {
			continue; // a policy subkey the loader read as a provider row
		}
		out->registered++;
		if (e->status == XRT_CONV_LOAD_MALFORMED) {
			out->malformed_rows++;
		}
		(void)target_conv_policy_apply_override(policy, e);
		g_headless_entry = e; // the host's get_setting answers for this provider only
		if (e->status == XRT_CONV_LOAD_NOT_LOADED) {
			(void)target_conversion_plugin_negotiate_one(e, target_conv_status_headless_host());
		}

		struct conv_provider_desc d;
		memset(&d, 0, sizeof(d));
		d.id = e->id;
		d.probe_order = e->probe_order;
		d.hosting = e->hosting == XRT_CONV_HOSTING_SATELLITE ? CONV_HOSTING_SATELLITE
		            : e->hosting == XRT_CONV_HOSTING_IN_SERVICE ? CONV_HOSTING_IN_SERVICE
		                                                        : CONV_HOSTING_AUTO;
		d.supersedes = e->supersedes;
		d.enabled = e->enabled && e->status != XRT_CONV_LOAD_DISABLED;
		d.binary_path = e->binary_path;
		d.iface = e->status == XRT_CONV_LOAD_NEGOTIATED ? e->iface : NULL;
		const int32_t pi = conv_registry_add(reg, &d);
		if (pi < 0) {
			continue;
		}
		struct prov_extra *x = &extra[pi];
		x->entry = i;
		fill_extra_from_entry(e, x);
		if (d.iface == NULL) {
			conv_registry_set_usable(reg, (uint32_t)pi, false);
			continue;
		}
		out->negotiated++;

		// probe: [S], <= 100 ms, no model load (spec §2.3).
		struct xrt_conversion_plugin_instance *inst = NULL;
		if (XRT_CONV_IFACE_HAS(d.iface, probe) && d.iface->probe != NULL) {
			const xrt_result_t pr = d.iface->probe(&inst);
			if (pr != XRT_SUCCESS) {
				conv_registry_set_usable(reg, (uint32_t)pi, false);
				copy_str(x->load, sizeof(x->load), pr == XRT_ERROR_PROBER_NOT_SUPPORTED ? "PROBE_DECLINED"
				                                                                       : "PROBE_FAILED");
				(void)snprintf(x->hint, sizeof(x->hint), "probe returned %d", (int)pr);
				continue;
			}
		}
		reg->providers[pi].inst = inst;
		if (conv_registry_describe(reg, (uint32_t)pi, now) < 0) {
			note_bad(out, e->id, "describe_cap could not report its count");
			continue;
		}
		if (reg->providers[pi].bad_descriptors > 0) {
			for (uint32_t b = 0; b < reg->providers[pi].bad_descriptors; b++) {
				note_bad(out, e->id, cap_error_str(reg->providers[pi].first_error));
			}
		}
		apply_provider_states(reg, (uint32_t)pi, out);
	}
	g_headless_entry = NULL;

	// The static dp-lift row (Windows: the lift slots are a D3D11-service feature).
#ifdef XRT_OS_WINDOWS
	if (dp_has_lift_factory(active_dp)) {
		struct conv_provider_desc d;
		memset(&d, 0, sizeof(d));
		d.id = DP_LIFT_ID;
		d.probe_order = DP_LIFT_PROBE_ORDER;
		d.hosting = CONV_HOSTING_IN_SERVICE;
		d.enabled = true;
		d.flags = CONV_PROV_LICENCE_EXEMPT;
		d.iface = &k_dp_lift_iface;
		const int32_t pi = conv_registry_add(reg, &d);
		if (pi >= 0) {
			struct prov_extra *x = &extra[pi];
			x->entry = -1;
			x->built_in = true;
			x->licence_exempt = true;
			copy_str(x->name, sizeof(x->name), k_dp_lift_iface.display_name);
			copy_str(x->load, sizeof(x->load), "BUILT_IN");
			copy_str(x->hint, sizeof(x->hint), "static describe: lift-only factory present, not instantiated");
			(void)conv_registry_describe(reg, (uint32_t)pi, now);
			apply_provider_states(reg, (uint32_t)pi, out);
			out->dp_lift_declared = true;
		}
		(void)snprintf(out->dp_lift_note, sizeof(out->dp_lift_note),
		               "dp-lift row: static describe (lift-only factory present; DEPTH_MONO, DEPTH_VIDEO, "
		               "VIEWS_MONO INSTALLED, not instantiated)");
	} else {
		(void)snprintf(out->dp_lift_note, sizeof(out->dp_lift_note),
		               active_dp == NULL ? "dp-lift row: no active display plug-in (OK)"
		                                 : "dp-lift row: not declared headless (no lift-only factory; OK)");
	}
#else
	(void)active_dp;
	(void)dp_has_lift_factory;
	(void)k_dp_lift_iface;
	(void)snprintf(out->dp_lift_note, sizeof(out->dp_lift_note),
	               "dp-lift row: the lift slots are a D3D11-service feature (Windows-only; OK)");
#endif

	// Provider rows (registry order: the loader's sorted order, then built-ins).
	uint32_t status_index[CONV_REGISTRY_MAX_PROVIDERS];
	for (uint32_t pi = 0; pi < reg->provider_count; pi++) {
		status_index[pi] = XRT_STATUS_CONV_NO_PROVIDER;
		if (out->provider_count >= XRT_STATUS_MAX_CONV_PROVIDERS) {
			continue;
		}
		status_index[pi] = out->provider_count;
		fill_provider_row(reg, pi, &extra[pi], policy, &out->providers[out->provider_count++]);
	}
	build_task_rows(reg, status_index, policy, out);

	// Tear down: destroy each instance, then unload its binary. Nothing above
	// keeps a pointer into a provider (every string was copied).
	for (uint32_t pi = 0; pi < reg->provider_count; pi++) {
		struct conv_provider *p = &reg->providers[pi];
		if (p->iface != NULL && p->inst != NULL && XRT_CONV_IFACE_HAS(p->iface, destroy) &&
		    p->iface->destroy != NULL) {
			p->iface->destroy(p->inst);
		}
		p->inst = NULL;
	}
	for (int i = 0; i < count; i++) {
		target_conversion_plugin_unload(&entries[i]);
	}
	free(extra);
	free(reg);
}

void
target_conv_status_from_registry(const struct conv_registry *reg,
                                 const struct xrt_conversion_plugin_entry *entries,
                                 int count,
                                 const struct target_conv_policy *policy,
                                 struct target_conv_status *out)
{
	memset(out, 0, sizeof(*out));
	if (policy != NULL) {
		out->policy = *policy;
	}
	struct prov_extra *extra = calloc(CONV_REGISTRY_MAX_PROVIDERS, sizeof(*extra));
	if (extra == NULL) {
		return;
	}
	uint32_t status_index[CONV_REGISTRY_MAX_PROVIDERS];
	for (uint32_t pi = 0; pi < reg->provider_count; pi++) {
		struct prov_extra *x = &extra[pi];
		x->entry = -1;
		for (int i = 0; entries != NULL && i < count; i++) {
			if (strcmp(entries[i].id, reg->providers[pi].id) == 0) {
				x->entry = i;
				fill_extra_from_entry(&entries[i], x);
				break;
			}
		}
		if (x->entry < 0) {
			x->built_in = true;
			copy_str(x->name, sizeof(x->name), reg->providers[pi].id);
			copy_str(x->load, sizeof(x->load), "BUILT_IN");
		}
		x->licence_exempt = (reg->providers[pi].flags & CONV_PROV_LICENCE_EXEMPT) != 0;
		out->registered++;
		status_index[pi] = XRT_STATUS_CONV_NO_PROVIDER;
		if (out->provider_count >= XRT_STATUS_MAX_CONV_PROVIDERS) {
			continue;
		}
		status_index[pi] = out->provider_count;
		fill_provider_row(reg, pi, x, policy, &out->providers[out->provider_count++]);
		out->bad_descriptors += reg->providers[pi].bad_descriptors;
	}
	build_task_rows(reg, status_index, policy, out);
	free(extra);
}

void
target_conv_status_fill_activity(struct target_conv_status *st, target_conv_activity_fn fn, void *ud)
{
	if (st == NULL || fn == NULL) {
		return;
	}
	for (uint32_t i = 0; i < st->task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
		struct xrt_status_conv_task *t = &st->tasks[i];
		t->streams = 0;
		t->jobs_queued = 0;
		if (t->provider >= st->provider_count || t->provider >= XRT_STATUS_MAX_CONV_PROVIDERS) {
			continue;
		}
		fn(ud, st->providers[t->provider].id, t->cap_id, t->composed, &t->streams, &t->jobs_queued);
	}
}

static bool
env_is_zero(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && strcmp(v, "0") == 0;
}

const char *
target_conv_kill_switch(void)
{
	if (env_is_zero("DXR_CONV")) {
		return "DXR_CONV";
	}
	if (env_is_zero("DXR_LIFT")) {
		return "DXR_LIFT"; // the pre-ADR-053 lift kill switch, kept as the alias
	}
	return NULL;
}

void
target_conv_status_build_headless(const struct xrt_plugin_iface *active_dp, struct target_conv_status *out)
{
	const char *ks = target_conv_kill_switch();
	if (ks != NULL) {
		// No conversion service at all (Â§3.2): nothing enumerated, nothing loaded.
		memset(out, 0, sizeof(*out));
		target_conv_policy_read(&out->policy);
		copy_str(out->kill_switch, sizeof(out->kill_switch), ks);
		(void)snprintf(out->dp_lift_note, sizeof(out->dp_lift_note),
		               "dp-lift row: none (%s=0, no conversion service)", ks);
		return;
	}
	struct xrt_conversion_plugin_entry *entries =
	    calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(struct xrt_conversion_plugin_entry));
	struct target_conv_policy *policy = calloc(1, sizeof(*policy));
	if (entries == NULL || policy == NULL) {
		free(entries);
		free(policy);
		memset(out, 0, sizeof(*out));
		return;
	}
	target_conv_policy_read(policy);
	const int n = target_conversion_plugin_enumerate(entries, XRT_CONV_LOADER_MAX_ENTRIES);
	target_conv_status_build_from_entries(entries, n < 0 ? 0 : n, active_dp, policy, out);
	free(policy);
	free(entries);
}

void
target_conv_status_to_snapshot(const struct target_conv_status *st, struct xrt_status_snapshot *snap)
{
	snap->conv_provider_count = st->provider_count;
	memcpy(snap->conv_providers, st->providers, sizeof(snap->conv_providers));
	snap->conv_task_count = st->task_count;
	memcpy(snap->conv_tasks, st->tasks, sizeof(snap->conv_tasks));
}
