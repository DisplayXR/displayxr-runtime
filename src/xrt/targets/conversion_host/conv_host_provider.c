// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: provider load + the satellite host iface.
 * @ingroup conv_host
 */

#include "conv_host_provider.h"
#include "conv_host_exit.h"
#include "conv_host_log.h"

#include "conversion/conv_host_inproc.h"
#include "conversion/conv_model_store.h"
#include "target_component_loader.h"
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/*
 *
 * The satellite host iface. get_setting carries no context (spec §2.1), and a
 * host serves exactly one provider, so its state is process-global.
 *
 */

static struct
{
	struct os_mutex mtx;
	bool mtx_ok;
	char provider_id[64];
	char env_prefix[96]; //!< "DXR_CONV_<ID>_" (upper case, '-' -> '_')
	uint32_t setting_count;
	struct xrt_conversion_plugin_setting settings[CONV_HOST_MAX_SETTINGS];
	uint64_t adapter_luid;

	bool changed;
	uint32_t changed_cap;
} g_cfg;

static struct xrt_conversion_plugin_host_iface g_iface;

//! Main thread, before any provider code runs.
static void
cfg_init(void)
{
	if (!g_cfg.mtx_ok) {
		g_cfg.mtx_ok = os_mutex_init(&g_cfg.mtx) == 0;
	}
}

static void
cfg_lock(void)
{
	os_mutex_lock(&g_cfg.mtx);
}

static void
cfg_unlock(void)
{
	os_mutex_unlock(&g_cfg.mtx);
}

static bool
host_get_setting(const char *key, char *buf, uint32_t buf_size)
{
	if (key == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	// The one options contract (ADR-054 N3c), exactly as the service's
	// in-process host answers it: tier 1 the environment (the supervisor
	// spawns this host with the service's block), tier 2 the per-user store
	// (HKCU `Settings\<KEY>` / <id>.settings.json, read live), tier 3 the
	// registration row as enumerated at start. Before this the host answered
	// from tiers 1 and 3 only, so a `conv set <id> KEY value` never reached a
	// satellite provider while it reached the same provider in-service.
	const char *row_value = NULL;
	char id[64];
	cfg_lock();
	(void)snprintf(id, sizeof(id), "%s", g_cfg.provider_id);
	for (uint32_t i = 0; i < g_cfg.setting_count; i++) {
#if defined(XRT_OS_WINDOWS)
		if (_stricmp(g_cfg.settings[i].key, key) == 0) {
#else
		if (strcasecmp(g_cfg.settings[i].key, key) == 0) {
#endif
			row_value = g_cfg.settings[i].value;
			break;
		}
	}
	cfg_unlock();
	return target_component_get_setting_ex(target_conversion_plugin_component_class(), id, key, row_value, buf,
	                                       buf_size, NULL);
}

static bool
host_get_adapter_hint(struct xrt_conv_adapter_hint *inout)
{
	if (inout == NULL || !XRT_CONV_COVERS(inout, struct xrt_conv_adapter_hint, preferred_luid)) {
		return false;
	}
	cfg_lock();
	const uint64_t luid = g_cfg.adapter_luid;
	cfg_unlock();
	if (luid == 0) {
		return false;
	}
	inout->preferred_luid = luid;
	inout->flags |= 1u; // the panel's render (ingest) adapter
	return true;
}

static void
host_notify_changed(uint32_t cap_id)
{
	// Any provider thread: only flag it; the control loop forwards it.
	cfg_lock();
	g_cfg.changed_cap = g_cfg.changed ? 0u : cap_id;
	g_cfg.changed = true;
	cfg_unlock();
}

static void
init_iface(void)
{
	if (g_iface.struct_size != 0) {
		return;
	}
	g_iface = *target_conversion_plugin_default_host();
	g_iface.host_flags |= 1u; // hosted out of the service (satellite)
	g_iface.get_setting = host_get_setting;
	g_iface.get_adapter_hint = host_get_adapter_hint;
	g_iface.notify_changed = host_notify_changed;
	// ADR-053 P4 (#2108): the host performs downloads and verification itself,
	// into the runtime-owned store, under the store's ACL as this (user) process.
	g_iface.model_resolve = conv_store_host_model_resolve;
	g_iface.model_request = conv_store_host_model_request;
}

const struct xrt_conversion_plugin_host_iface *
conv_host_provider_host_iface(void)
{
	init_iface();
	return &g_iface;
}

void
conv_host_provider_set_config(const char *provider_id,
                              const struct xrt_conversion_plugin_setting *settings,
                              uint32_t count,
                              uint64_t adapter_luid)
{
	cfg_init();
	cfg_lock();
	(void)snprintf(g_cfg.provider_id, sizeof(g_cfg.provider_id), "%s", provider_id != NULL ? provider_id : "");
	(void)snprintf(g_cfg.env_prefix, sizeof(g_cfg.env_prefix), "DXR_CONV_%s_", provider_id != NULL ? provider_id : "");
	for (char *c = g_cfg.env_prefix; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)toupper((unsigned char)*c);
	}
	g_cfg.setting_count = 0;
	for (uint32_t i = 0; settings != NULL && i < count && i < CONV_HOST_MAX_SETTINGS; i++) {
		g_cfg.settings[i] = settings[i];
		g_cfg.settings[i].key[sizeof(g_cfg.settings[i].key) - 1] = '\0';
		g_cfg.settings[i].value[sizeof(g_cfg.settings[i].value) - 1] = '\0';
		g_cfg.setting_count++;
	}
	g_cfg.adapter_luid = adapter_luid;
	cfg_unlock();
}

void
conv_host_provider_config_from_registration(const char *provider_id, uint64_t adapter_luid)
{
	// The service's own source of these values (registry row / manifest).
	// Enumeration only reads; it never loads a binary.
	struct xrt_conversion_plugin_entry *rows = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*rows));
	const struct xrt_conversion_plugin_entry *mine = NULL;
	int n = 0;
	if (rows != NULL) {
		n = target_conversion_plugin_enumerate(rows, XRT_CONV_LOADER_MAX_ENTRIES);
		for (int i = 0; i < n; i++) {
			if (strcmp(rows[i].id, provider_id) == 0) {
				mine = &rows[i];
				break;
			}
		}
	}
	conv_host_provider_set_config(provider_id, mine != NULL ? mine->settings : NULL,
	                              mine != NULL ? mine->setting_count : 0, adapter_luid);
	// Its own packs only, under the policy as this process reads it (the
	// service's environment: the supervisor spawns the host with it).
	struct target_conv_policy policy;
	target_conv_policy_read(&policy);
	target_conv_store_configure(mine, mine != NULL ? 1 : 0, &policy);
	U_LOG_I("conversion host: %u registered setting(s) for '%s' (%s)", mine != NULL ? mine->setting_count : 0,
	        provider_id, mine != NULL ? mine->source : "no registration found");
	free(rows);
}

bool
conv_host_provider_take_changed(uint32_t *out_cap_id)
{
	cfg_lock();
	const bool c = g_cfg.changed;
	if (out_cap_id != NULL) {
		*out_cap_id = g_cfg.changed_cap;
	}
	g_cfg.changed = false;
	g_cfg.changed_cap = 0;
	cfg_unlock();
	return c;
}


/*
 *
 * Load.
 *
 */

int
conv_host_provider_load(struct conv_host_provider *p,
                        const char *provider_id,
                        const char *binary_path,
                        char *out_reason,
                        uint32_t reason_size)
{
	char dummy[8];
	if (out_reason == NULL || reason_size == 0) {
		out_reason = dummy;
		reason_size = sizeof(dummy);
	}
	out_reason[0] = '\0';
	memset(p, 0, sizeof(*p));
	cfg_init();
	init_iface();

	p->entry = calloc(1, sizeof(*p->entry));
	if (p->entry == NULL) {
		(void)snprintf(out_reason, reason_size, "out of memory");
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	struct xrt_conversion_plugin_entry *e = p->entry;
	// A row as the service saw it: the supervisor passes the id and the
	// binary it resolved, so the host does not re-run discovery (HKLM /
	// manifests) and cannot disagree with the service about which file.
	(void)snprintf(e->id, sizeof(e->id), "%s", provider_id);
	(void)snprintf(e->display_name, sizeof(e->display_name), "%s", provider_id);
	(void)snprintf(e->binary_path, sizeof(e->binary_path), "%s", binary_path);
	(void)snprintf(e->source, sizeof(e->source), "conversion-host --binary");
	e->enabled = true;
	e->status = XRT_CONV_LOAD_NOT_LOADED;

	if (!target_conversion_plugin_negotiate_one(e, &g_iface)) {
		(void)snprintf(out_reason, reason_size, "%s: %s", target_conversion_plugin_status_str(e->status),
		               e->reason);
		return e->status == XRT_CONV_LOAD_ABI_MISMATCH ? CONV_HOST_EXIT_ABI_REJECT : CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	conv_host_log_adopt_provider_logger(e->module);

	if (e->iface->id != NULL && strcmp(e->iface->id, provider_id) != 0) {
		U_LOG_W("conversion host: provider registered as '%s' calls itself '%s'; serving it as '%s'",
		        provider_id, e->iface->id, provider_id);
	}

	xrt_result_t pr = XRT_SUCCESS;
	if (XRT_CONV_IFACE_HAS(e->iface, probe)) {
		const uint64_t t0 = os_monotonic_get_ns();
		pr = e->iface->probe(&p->inst); // <= 100 ms, no model load (spec §2.3)
		const uint64_t probe_us = (os_monotonic_get_ns() - t0) / 1000u;
		// One line per host start: the probe is part of every respawn's cost.
		U_LOG_W("conversion host: probe returned %d in %llu.%03llu ms%s", (int)pr,
		        (unsigned long long)(probe_us / 1000u), (unsigned long long)(probe_us % 1000u),
		        probe_us > 100000u ? " (over the 100 ms probe budget)" : "");
	}
	if (pr != XRT_SUCCESS) {
		(void)snprintf(out_reason, reason_size, "probe returned %d", (int)pr);
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	p->probed = true;

	// A describe that fails outright is a negotiate-class failure (exit 11):
	// the service would list the provider with no capability anyway.
	if (!XRT_CONV_IFACE_HAS(e->iface, describe_cap)) {
		(void)snprintf(out_reason, reason_size, "no describe_cap slot");
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}

	const xrt_result_t hr = conv_host_inproc_create(provider_id, e->iface, p->inst, &p->host);
	if (hr != XRT_SUCCESS) {
		(void)snprintf(out_reason, reason_size, "exec host creation failed (%d)", (int)hr);
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	p->inst = NULL; // owned by the exec host now

	uint32_t count = 0;
	if (conv_host_describe_cap(p->host, 0, NULL, &count) != XRT_SUCCESS) {
		(void)snprintf(out_reason, reason_size, "describe_cap(count) failed");
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	U_LOG_W("conversion host: provider '%s' loaded from %s (ABI %u), %u capabilities", provider_id, binary_path,
	        e->plugin_api_version, count);
	return CONV_HOST_EXIT_ORDERLY;
}

void
conv_host_provider_unload(struct conv_host_provider *p)
{
	if (p == NULL) {
		return;
	}
	if (p->host != NULL) {
		conv_host_destroy(&p->host);
	} else if (p->inst != NULL && p->entry != NULL && XRT_CONV_IFACE_HAS(p->entry->iface, destroy)) {
		p->entry->iface->destroy(p->inst);
	}
	p->inst = NULL;
	// The binary stays mapped: target_conversion_plugin_unload only for a
	// provider with no live code left, which an abandoned thread breaks.
	free(p->entry);
	p->entry = NULL;
}
