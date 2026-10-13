// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli conv` — conversion providers (ADR-053 §7.1).
 *
 *   conv list   [--json]                 providers x tasks, selection order, `selected`, reason per row
 *   conv status [--json] [--watch [--interval ms]]   readiness, streams, jobs
 *   conv warm   <task> | <id>[/<cap>] [<task>] [--wait [--timeout s]]   explicit prewarm (service only)
 *   conv enable | disable <id> [--machine]           `Enabled` (HKCU; HKLM with --machine)
 *   conv use    <task> <id>[/<cap>] [--machine]      `PreferredProvider\<task>`
 *   conv reset  <task>|all [--machine]
 *   conv models list | prune                         the model store (P4; prints the root)
 *
 * `list` / `status` read the running service's status snapshot over DIAG
 * (`system_get_conv_*_row`, one row per reply) and fall back to the headless
 * pass (target_conv_status: enumerate, negotiate, probe, describe -- never
 * warm), labelled `source: headless`. A service that publishes no conversion
 * rows yet (before #2003 registers the core's status source) also falls back,
 * and says so.
 *
 * Registry writes follow `dp use`: per user (HKCU) by default, machine-wide
 * (HKLM, admin) with `--machine` (spec §3.1: HKCU wins over HKLM unless HKLM
 * `Policy\Lock` = 1). The running service picks a registry change up through
 * its RegNotifyChangeKeyValue watch on the root (spec §4.1 re-describe
 * trigger) -- there is no separate poke RPC.
 *
 * `conv warm` (#2029) is the DIAG `conv_warm` call: it warms the task's
 * selected capability in the running service (READY is a no-op; an
 * all-terminal task is retried) and, with --wait, follows the status rows
 * until READY or terminal. It never warms headless.
 *
 * @ingroup cli
 */

#include "cli_common.h"
#include "cli_ipc.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_status_snapshot.h"
#include "util/u_time.h"

#include "cli_query.h" // cli_query_handles + teardown (the headless instance)
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"
#include "target_plugin_loader.h" // target_plugin_get_active (the static dp-lift row)
#include "conversion/conv_readiness.h"
#include "conversion/conv_select.h"

#ifdef CLI_HAVE_IPC
#include "xrt/xrt_results.h"
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"
#endif

#include <cjson/cJSON.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define P(...) printf(__VA_ARGS__)

namespace {

/*
 *
 * Reading the rows.
 *
 */

#ifdef CLI_HAVE_IPC
bool
conv_connect(struct ipc_connection *ipc_c)
{
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli conv");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	memset(ipc_c, 0, sizeof(*ipc_c));
	return cli_ipc_connect(ipc_c, U_LOGGING_ERROR, &ii) == XRT_SUCCESS;
}

/*!
 * The service's conversion rows: the head (for the counts + generation) and
 * every row by index, all at one generation (a read that straddles a rebuild
 * starts over). False on any IPC failure.
 */
bool
fetch_over(struct ipc_connection *ipc_c, struct xrt_status_snapshot *out)
{
	for (int attempt = 0; attempt < 4; attempt++) {
		struct xrt_status_head head;
		struct xrt_status_screen screen;
		if (ipc_call_system_get_status_snapshot(ipc_c, 0, &head, &screen) != XRT_SUCCESS) {
			return false;
		}
		u_status_snapshot_set_head(out, &head);
		bool consistent = true;
		for (uint32_t i = 0; i < out->conv_provider_count && consistent; i++) {
			struct xrt_status_generation g;
			const xrt_result_t x = ipc_call_system_get_conv_provider_row(ipc_c, i, &out->conv_providers[i], &g);
			if (x != XRT_SUCCESS) {
				consistent = false;
				break;
			}
			consistent = u_status_generation_equal(&g, &head.generation);
		}
		for (uint32_t i = 0; i < out->conv_task_count && consistent; i++) {
			struct xrt_status_generation g;
			const xrt_result_t x = ipc_call_system_get_conv_task_row(ipc_c, i, &out->conv_tasks[i], &g);
			if (x != XRT_SUCCESS) {
				consistent = false;
				break;
			}
			consistent = u_status_generation_equal(&g, &head.generation);
		}
		if (consistent) {
			// Re-derive: the head's warnings are system-wide; the CONV_* ones
			// need the rows (the screens are not fetched here).
			u_status_warnings_derive(out);
			return true;
		}
	}
	return true; // never settled: the last (possibly mixed) read beats nothing
}
#endif

//! Headless rows (`source: headless`), warnings derived.
void
build_headless(struct xrt_status_snapshot *out, struct target_conv_policy *out_policy)
{
	memset(out, 0, sizeof(*out));
	out->schema = XRT_STATUS_SCHEMA;
	out->source = XRT_STATUS_SOURCE_HEADLESS;
	auto st = std::make_unique<target_conv_status>();
	// The static dp-lift row needs the active display plug-in, which only an
	// instance resolves: a no-compositor instance, as `status` builds one.
	struct cli_query_handles h;
	memset(&h, 0, sizeof(h));
	if (xrt_instance_create(nullptr, &h.xi) == XRT_SUCCESS) {
		(void)xrt_instance_create_system(h.xi, &h.xsys, &h.xsysd, &h.xso, nullptr);
	}
	target_conv_status_build_headless(target_plugin_get_active(), st.get());
	cli_query_teardown(&h);
	target_conv_status_to_snapshot(st.get(), out);
	if (out_policy != nullptr) {
		*out_policy = st->policy;
	}
	u_status_warnings_derive(out);
}

/*!
 * Service rows when the service is reachable AND publishes some, else the
 * headless pass. @p why says which and why (stderr-worthy).
 */
void
fetch_rows(struct xrt_status_snapshot *out, struct target_conv_policy *policy, std::string &why)
{
	target_conv_policy_read(policy);
#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (conv_connect(&ipc_c)) {
		const bool ok = fetch_over(&ipc_c, out);
		ipc_client_connection_fini(&ipc_c);
		if (ok && (out->conv_provider_count > 0 || out->conv_task_count > 0)) {
			return;
		}
		why = ok ? "the running service publishes no conversion rows yet (the conversion core's status source "
		           "lands with #2003) -- showing the HEADLESS describe"
		         : "the service did not answer the status read -- showing the HEADLESS describe";
	} else {
		why = "no service reached -- showing the HEADLESS describe (no model loaded)";
	}
#else
	why = "no IPC client in this build -- showing the HEADLESS describe";
#endif
	build_headless(out, nullptr);
}


/*
 *
 * Printing.
 *
 */

void
add_policy_json(cJSON *root, const struct target_conv_policy *policy)
{
	cJSON *po = cJSON_AddObjectToObject(root, "policy");
	cJSON_AddBoolToObject(po, "policy_lock", policy->policy_lock);
	cJSON_AddBoolToObject(po, "allow_research_only", policy->allow_research_only);
	cJSON *pref = cJSON_AddObjectToObject(po, "preferred");
	for (uint32_t t = 1; t < TARGET_CONV_POLICY_TASKS; t++) {
		if (policy->preferred[t][0] != '\0') {
			cJSON_AddStringToObject(pref, conv_task_name(t), policy->preferred[t]);
		}
	}
}

void
print_json(const struct xrt_status_snapshot *snap, const struct target_conv_policy *policy, bool compact)
{
	cJSON *root = u_status_conv_to_cjson(snap);
	add_policy_json(root, policy);
	char *s = compact ? cJSON_PrintUnformatted(root) : cJSON_Print(root);
	if (s != nullptr) {
		P("%s\n", s);
		cJSON_free(s);
	}
	cJSON_Delete(root);
}

const char *
provider_id_of(const struct xrt_status_snapshot *snap, const struct xrt_status_conv_task &t)
{
	if (t.provider < snap->conv_provider_count && t.provider < XRT_STATUS_MAX_CONV_PROVIDERS) {
		return snap->conv_providers[t.provider].id;
	}
	return "?";
}

void
print_providers_text(const struct xrt_status_snapshot *snap)
{
	P("PROVIDERS  (source: %s)\n", u_status_source_str(snap->source));
	if (snap->conv_provider_count == 0) {
		P("  none registered\n");
		return;
	}
	P("  %-18s %-16s %5s  %-10s %-9s %4s  %s\n", "id", "load", "order", "hosting", "status", "caps", "notes");
	for (uint32_t i = 0; i < snap->conv_provider_count && i < XRT_STATUS_MAX_CONV_PROVIDERS; i++) {
		const struct xrt_status_conv_provider &p = snap->conv_providers[i];
		std::string notes;
		if (!p.enabled) {
			notes += "disabled ";
		}
		if (p.hidden) {
			notes += "hidden(Supersedes) ";
		}
		if (p.fallback) {
			notes += "fallback ";
		}
		if (p.licence_exempt) {
			notes += "licence-exempt ";
		}
		if (p.bad_descriptors > 0) {
			notes += "MALFORMED(" + std::to_string(p.bad_descriptors) + ": " + p.descriptor_error + ") ";
		}
		if (p.hint[0] != '\0') {
			notes += std::string("- ") + p.hint;
		}
		P("  %-18s %-16s %5u  %-10s %-9s %4u  %s\n", p.id, p.load, p.probe_order,
		  u_status_conv_hosting_str(p.hosting), u_status_platform_state_str(p.platform_state), p.cap_count,
		  notes.c_str());
	}
}

void
print_list_text(const struct xrt_status_snapshot *snap, const struct target_conv_policy *policy)
{
	print_providers_text(snap);
	P("SELECTION  (per task, in order; * = selected%s%s)\n", policy->allow_research_only ? "; AllowResearchOnly" : "",
	  policy->policy_lock ? "; Policy\\Lock" : "");
	uint32_t last_task = 0;
	for (uint32_t i = 0; i < snap->conv_task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
		const struct xrt_status_conv_task &t = snap->conv_tasks[i];
		if (t.task != last_task) {
			last_task = t.task;
			const char *pref = t.task < TARGET_CONV_POLICY_TASKS ? policy->preferred[t.task] : "";
			P("  %s%s%s\n", t.task_name, pref[0] != '\0' ? "  preferred: " : "", pref);
		}
		char rank[8];
		if (t.order == XRT_STATUS_CONV_NOT_ORDERED) {
			snprintf(rank, sizeof(rank), "-");
		} else {
			snprintf(rank, sizeof(rank), "%u", t.order + 1);
		}
		P("    %s %-3s %-18s cap %-3u %-12s %-14s %s%s\n", t.selected ? "*" : " ", rank, provider_id_of(snap, t),
		  t.cap_id, t.state_name, t.verdict, u_status_conv_licence_str(t.licence_class),
		  t.composed ? "  (+ reference view synthesis)" : "");
	}
	if (snap->conv_task_count == 0) {
		P("  no capabilities described\n");
	}
	for (uint32_t i = 0; i < snap->warning_count && i < XRT_STATUS_MAX_WARNINGS; i++) {
		if (strncmp(snap->warnings[i].code, "CONV_", 5) == 0) {
			P("[%s] %s: %s\n", u_status_level_str(snap->warnings[i].level), snap->warnings[i].code,
			  snap->warnings[i].text);
		}
	}
}

void
print_status_text(const struct xrt_status_snapshot *snap)
{
	P("CONVERSION  source: %s", u_status_source_str(snap->source));
	if (snap->source == XRT_STATUS_SOURCE_SERVICE) {
		P(" (gen %llu)", (unsigned long long)snap->generation.conversion);
	}
	P("\n");
	if (snap->conv_task_count == 0) {
		P("  no capabilities described\n");
	}
	P("  %-18s %-18s %-12s %-12s %8s %7s %4s\n", "task", "provider", "state", "reason", "progress", "streams", "jobs");
	for (uint32_t i = 0; i < snap->conv_task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
		const struct xrt_status_conv_task &t = snap->conv_tasks[i];
		if (!t.selected) {
			continue;
		}
		char prog[16] = "-";
		if (t.state == XRT_CONV_STATE_WARMING || t.state == XRT_CONV_STATE_DOWNLOADING) {
			if (t.progress >= 0.0f) {
				snprintf(prog, sizeof(prog), "%3.0f%%", (double)t.progress * 100.0);
			} else {
				snprintf(prog, sizeof(prog), "...");
			}
		}
		std::string who = provider_id_of(snap, t);
		if (t.composed) {
			who += "+vs";
		}
		P("  %-18s %-18s %-12s %-12s %8s %7u %4u%s\n", t.task_name, who.c_str(), t.state_name, t.reason_name, prog,
		  t.streams, t.jobs_queued, t.cpu_staging ? "  CPU-staging" : "");
	}
	for (uint32_t i = 0; i < snap->warning_count && i < XRT_STATUS_MAX_WARNINGS; i++) {
		if (strncmp(snap->warnings[i].code, "CONV_", 5) == 0) {
			P("[%s] %s: %s\n", u_status_level_str(snap->warnings[i].level), snap->warnings[i].code,
			  snap->warnings[i].text);
		}
	}
}


/*
 *
 * list / status.
 *
 */

int
cmd_list(int argc, const char **argv, bool status_view)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	auto snap = std::make_unique<xrt_status_snapshot>();
	auto policy = std::make_unique<target_conv_policy>();
	std::string why;
	fetch_rows(snap.get(), policy.get(), why);
	if (!why.empty()) {
		fprintf(stderr, "displayxr-cli conv: %s.\n", why.c_str());
	}
	if (json) {
		print_json(snap.get(), policy.get(), false);
	} else if (status_view) {
		print_status_text(snap.get());
	} else {
		print_list_text(snap.get(), policy.get());
	}
	return 0;
}

volatile sig_atomic_t g_stop = 0;

void
on_signal(int)
{
	g_stop = 1;
}

/*!
 * `conv status --watch`: one DIAG connection, poll the generation counters,
 * re-print when one moves (one NDJSON line per change with --json). Without a
 * service: the headless rows once, then retry the connection every 2 s.
 */
int
cmd_watch(int argc, const char **argv)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	uint32_t interval_ms = 500;
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--interval") == 0) {
			const long v = strtol(argv[i + 1], nullptr, 10);
			interval_ms = v < 100 ? 100u : (uint32_t)v;
		}
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	auto snap = std::make_unique<xrt_status_snapshot>();
	auto policy = std::make_unique<target_conv_policy>();
	target_conv_policy_read(policy.get());
	auto print = [&]() {
		if (json) {
			print_json(snap.get(), policy.get(), true);
		} else {
			P("\n");
			print_status_text(snap.get());
		}
		fflush(stdout);
	};
#ifdef CLI_HAVE_IPC
	bool printed_headless = false;
	while (!g_stop) {
		struct ipc_connection ipc_c;
		if (!conv_connect(&ipc_c)) {
			if (!printed_headless) {
				fprintf(stderr, "displayxr-cli conv: no service reached -- HEADLESS rows; retrying.\n");
				build_headless(snap.get(), nullptr);
				print();
				printed_headless = true;
			}
			for (int k = 0; k < 40 && !g_stop; k++) {
				os_nanosleep(50 * U_TIME_1MS_IN_NS);
			}
			continue;
		}
		printed_headless = false;
		bool have = false;
		struct xrt_status_generation last = {0, 0, 0};
		while (!g_stop) {
			struct xrt_status_generation g;
			if (ipc_call_system_get_status_generation(&ipc_c, &g) != XRT_SUCCESS) {
				break;
			}
			if (!have || !u_status_generation_equal(&g, &last)) {
				if (!fetch_over(&ipc_c, snap.get())) {
					break;
				}
				last = snap->generation;
				have = true;
				print();
			}
			for (uint32_t k = 0; k < interval_ms / 50 && !g_stop; k++) {
				os_nanosleep(50 * U_TIME_1MS_IN_NS);
			}
		}
		ipc_client_connection_fini(&ipc_c);
	}
#else
	(void)interval_ms;
	build_headless(snap.get(), nullptr);
	print();
#endif
	return 0;
}


/*
 *
 * Registry writes (spec §3.1).
 *
 */

enum class reg_write
{
	ok,
	denied,
	unsupported,
	failed,
};

#ifdef XRT_OS_WINDOWS
reg_write
reg_set(bool machine, const std::string &sub, const char *value, DWORD type, const void *data, DWORD size)
{
	std::string path = TARGET_CONV_REG_ROOT;
	if (!sub.empty()) {
		path += "\\" + sub;
	}
	HKEY key = nullptr;
	const REGSAM sam = KEY_SET_VALUE | (machine ? KEY_WOW64_64KEY : 0);
	const LSTATUS rc = RegCreateKeyExA(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
	                                   0, sam, nullptr, &key, nullptr);
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? reg_write::denied : reg_write::failed;
	}
	const LSTATUS w = RegSetValueExA(key, value, 0, type, (const BYTE *)data, size);
	RegCloseKey(key);
	if (w != ERROR_SUCCESS) {
		return w == ERROR_ACCESS_DENIED ? reg_write::denied : reg_write::failed;
	}
	return reg_write::ok;
}

reg_write
reg_delete(bool machine, const std::string &sub, const char *value)
{
	std::string path = std::string(TARGET_CONV_REG_ROOT) + "\\" + sub;
	HKEY key = nullptr;
	const REGSAM sam = KEY_SET_VALUE | (machine ? KEY_WOW64_64KEY : 0);
	LSTATUS rc = RegOpenKeyExA(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, path.c_str(), 0, sam, &key);
	if (rc == ERROR_FILE_NOT_FOUND) {
		return reg_write::ok; // nothing to clear
	}
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? reg_write::denied : reg_write::failed;
	}
	rc = RegDeleteValueA(key, value);
	RegCloseKey(key);
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
		return rc == ERROR_ACCESS_DENIED ? reg_write::denied : reg_write::failed;
	}
	return reg_write::ok;
}
#endif

reg_write
set_dword(bool machine, const std::string &sub, const char *value, uint32_t v)
{
#ifdef XRT_OS_WINDOWS
	DWORD d = v;
	return reg_set(machine, sub, value, REG_DWORD, &d, sizeof(d));
#else
	(void)machine;
	(void)sub;
	(void)value;
	(void)v;
	return reg_write::unsupported;
#endif
}

reg_write
set_string(bool machine, const std::string &sub, const char *value, const char *s)
{
#ifdef XRT_OS_WINDOWS
	return reg_set(machine, sub, value, REG_SZ, s, (DWORD)strlen(s) + 1);
#else
	(void)machine;
	(void)sub;
	(void)value;
	(void)s;
	return reg_write::unsupported;
#endif
}

reg_write
clear_value(bool machine, const std::string &sub, const char *value)
{
#ifdef XRT_OS_WINDOWS
	return reg_delete(machine, sub, value);
#else
	(void)machine;
	(void)sub;
	(void)value;
	return reg_write::unsupported;
#endif
}

int
write_result(reg_write r, const char *what)
{
	switch (r) {
	case reg_write::ok: return 0;
	case reg_write::denied: P("FAIL: %s was denied -- run from an elevated terminal (HKLM needs admin).\n", what); return 1;
	case reg_write::unsupported:
		P("FAIL: %s -- no registry policy store on this platform (edit the provider's manifest \"enabled\").\n",
		  what);
		return 1;
	case reg_write::failed:
	default: P("FAIL: %s failed.\n", what); return 1;
	}
}

//! How a registry change reaches the running service (spec §4.1).
void
note_service_pickup(void)
{
#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (conv_connect(&ipc_c)) {
		ipc_client_connection_fini(&ipc_c);
		P("The running service picks this up from its registry watch on %s (a re-describe trigger, spec\n"
		  "§4.1); it applies to the next stream / job create. A live stream never moves.\n",
		  TARGET_CONV_REG_ROOT);
		return;
	}
#endif
	P("No DisplayXR service reachable -- applies when it next starts.\n");
}

//! Is @p id a registered provider (or the built-in adapter)? Enumeration only.
bool
provider_known(const char *id)
{
	if (strcmp(id, "dp-lift") == 0) {
		return true;
	}
	auto entries = std::make_unique<xrt_conversion_plugin_entry[]>(XRT_CONV_LOADER_MAX_ENTRIES);
	const int n = target_conversion_plugin_enumerate(entries.get(), XRT_CONV_LOADER_MAX_ENTRIES);
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

bool
valid_id(const char *id)
{
	if (id == nullptr || id[0] == '\0' || strlen(id) >= 64 || target_conv_is_policy_subkey(id)) {
		return false;
	}
	for (const char *c = id; *c != '\0'; c++) {
		if (*c == '\\' || *c == '/') {
			return false;
		}
	}
	return true;
}

int
cmd_enable(const char *id, bool enable, bool machine)
{
	if (!valid_id(id)) {
		P("error: '%s' is not a provider id.\n", id != nullptr ? id : "");
		return 1;
	}
	if (strcmp(id, "dp-lift") == 0) {
		P("error: 'dp-lift' is the built-in adapter: it has no Enabled value (a registered provider hides it\n"
		  "with Supersedes = dp-lift).\n");
		return 1;
	}
	if (!provider_known(id)) {
		P("warning: '%s' is not a registered conversion provider; writing the value anyway.\n", id);
	}
	const int rc = write_result(set_dword(machine, id, "Enabled", enable ? 1u : 0u),
	                            machine ? "writing the machine-wide Enabled value" : "writing the per-user Enabled value");
	if (rc != 0) {
		return rc;
	}
	P("%s: %s %s.\n", id, enable ? "enabled" : "disabled", machine ? "machine-wide (HKLM)" : "for this user (HKCU)");
	if (!machine) {
		struct target_conv_policy pol;
		target_conv_policy_read(&pol);
		if (pol.policy_lock) {
			P("note: HKLM Policy\\Lock is set, so the per-user value is ignored.\n");
		}
	}
	note_service_pickup();
	return 0;
}

int
cmd_use(const char *task_name, const char *spec, bool machine)
{
	const uint32_t task = conv_task_from_name(task_name);
	if (task == 0 || !conv_task_is_served(task)) {
		P("error: unknown task '%s' (views-mono, views-from-depth, depth-mono, depth-video, disparity-stereo,\n"
		  "splats-mono, splats-stereo).\n",
		  task_name);
		return 1;
	}
	std::string id(spec);
	const size_t slash = id.find('/');
	if (slash != std::string::npos) {
		id.resize(slash);
	}
	if (!valid_id(id.c_str())) {
		P("error: '%s' is not '<provider-id>[/<cap_id or model_id>]'.\n", spec);
		return 1;
	}
	if (!provider_known(id.c_str())) {
		P("warning: '%s' is not a registered conversion provider; writing the preference anyway.\n", id.c_str());
	}
	const int rc = write_result(set_string(machine, "PreferredProvider", conv_task_name(task), spec),
	                            "writing the per-task preference");
	if (rc != 0) {
		return rc;
	}
	P("%s preference for %s: %s.\n", machine ? "Machine-wide" : "Per-user", conv_task_name(task), spec);
	note_service_pickup();
	return 0;
}

int
cmd_reset(const char *task_name, bool machine)
{
	const bool all = strcmp(task_name, "all") == 0;
	int rc = 0;
	for (uint32_t t = 1; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
		if (!conv_task_is_served(t)) {
			continue;
		}
		if (!all && conv_task_from_name(task_name) != t) {
			continue;
		}
		rc |= write_result(clear_value(machine, "PreferredProvider", conv_task_name(t)),
		                   "clearing the per-task preference");
	}
	if (!all && (conv_task_from_name(task_name) == 0 || !conv_task_is_served(conv_task_from_name(task_name)))) {
		P("error: unknown task '%s'.\n", task_name);
		return 1;
	}
	if (rc == 0) {
		P("%s preference%s cleared for %s.\n", machine ? "Machine-wide" : "Per-user", all ? "s" : "", task_name);
		note_service_pickup();
	}
	return rc;
}

const char *
state_upper(uint32_t state, char *buf, size_t cap)
{
	snprintf(buf, cap, "%s", conv_state_name(state));
	for (char *c = buf; *c != '\0'; c++) {
		*c = (char)toupper((unsigned char)*c);
	}
	return buf;
}

//! The row selection picked for @p task in @p snap (NULL = none).
const struct xrt_status_conv_task *
selected_row(const struct xrt_status_snapshot *snap, uint32_t task)
{
	for (uint32_t i = 0; i < snap->conv_task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
		if (snap->conv_tasks[i].task == task && snap->conv_tasks[i].selected) {
			return &snap->conv_tasks[i];
		}
	}
	return nullptr;
}

#ifdef CLI_HAVE_IPC
/*!
 * One DIAG conv_warm (#2029) for @p task, one line out. Returns 0 accepted
 * (READY already is a no-op, also 0), 1 nothing serves the task / refused,
 * 2 the service runs no conversion core.
 */
int
warm_one(struct ipc_connection *ipc_c, uint32_t task, const struct xrt_status_snapshot *snap, uint32_t *out_state)
{
	struct xrt_conv_wire_task_request req;
	memset(&req, 0, sizeof(req));
	req.struct_size = (uint32_t)sizeof(req);
	req.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	req.task = task;
	uint32_t prev = 0, now = 0;
	const xrt_result_t xr = ipc_call_conv_warm(ipc_c, &req, &prev, &now);
	*out_state = now;
	char a[16], b[16];
	const struct xrt_status_conv_task *row = selected_row(snap, task);
	std::string who = row != nullptr
	                      ? std::string(provider_id_of(snap, *row)) + " cap " + std::to_string(row->cap_id)
	                      : std::string("no provider");
	if (xr == XRT_ERROR_FEATURE_NOT_SUPPORTED && snap->conv_provider_count == 0 && snap->conv_task_count == 0) {
		P("%s: the running service has no conversion core (DXR_CONV=0 / DXR_LIFT=0 in ITS environment, or the "
		  "core "
		  "did not start). Nothing was warmed.\n",
		  conv_task_name(task));
		return 2;
	}
	if (xr == XRT_ERROR_FEATURE_NOT_SUPPORTED) {
		P("%s: nothing serves it (%s). Nothing was warmed.\n", conv_task_name(task),
		  state_upper(prev, a, sizeof(a)));
		return 1;
	}
	if (xr != XRT_SUCCESS) {
		P("%s: the service refused the warm (xrt_result %d).\n", conv_task_name(task), (int)xr);
		return 1;
	}
	if (prev == XRT_CONV_STATE_READY) {
		P("%s: already READY (%s) -- nothing to warm.\n", conv_task_name(task), who.c_str());
		return 0;
	}
	P("%s: %s -> %s (%s)%s\n", conv_task_name(task), state_upper(prev, a, sizeof(a)),
	  state_upper(now, b, sizeof(b)), who.c_str(),
	  conv_state_is_terminal(prev) ? " -- retried (explicit warm)" : "");
	return 0;
}
#endif

/*!
 * `conv warm <task> | <id>[/<cap>] [<task>] [--wait [--timeout s]]` (#2029):
 * the DIAG conv_warm call. It warms the TASK's selected capability (an id
 * names which tasks: those where that provider's row is the selected one --
 * `conv use` changes the selection, not this verb). Never headless: the CLI
 * never loads a model (spec §7.1).
 */
int
cmd_warm(int argc, const char **argv)
{
	if (argc < 4 || argv[3][0] == '-') {
		P("usage: displayxr-cli conv warm <task> | <id>[/<cap>] [<task>] [--wait [--timeout s]]\n");
		return 1;
	}
	const char *arg = argv[3];
	const uint32_t task = conv_task_from_name(arg);
	std::string id;
	uint32_t cap = 0;
	uint32_t only_task = 0;
	if (task == 0) {
		id = arg;
		const size_t slash = id.find('/');
		if (slash != std::string::npos) {
			cap = (uint32_t)strtoul(id.c_str() + slash + 1, nullptr, 10);
			id.resize(slash);
		}
		if (!valid_id(id.c_str()) || !provider_known(id.c_str())) {
			P("error: '%s' is neither a task nor a registered provider.\n", arg);
			return 1;
		}
		if (argc >= 5 && argv[4][0] != '-') {
			only_task = conv_task_from_name(argv[4]);
			if (only_task == 0) {
				P("error: unknown task '%s'.\n", argv[4]);
				return 1;
			}
		}
	} else if (!conv_task_is_served(task)) {
		P("error: task '%s' is not served in this version.\n", arg);
		return 1;
	}
	const bool wait = cli_has_flag(argc, argv, "--wait");
	uint32_t timeout_s = 120;
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--timeout") == 0) {
			const long v = strtol(argv[i + 1], nullptr, 10);
			timeout_s = v < 1 ? 1u : (uint32_t)v;
		}
	}
#ifdef CLI_HAVE_IPC
	// A real connect, not the pipe pre-check alone: a diagnostic must not claim a
	// service it cannot reach (and never launches one, #1966).
	struct ipc_connection ipc_c;
	if (!conv_connect(&ipc_c)) {
		P("No DisplayXR service reachable. The CLI never warms a model headless (spec §7.1).\n");
		return 1;
	}
	auto snap = std::make_unique<xrt_status_snapshot>();
	memset(snap.get(), 0, sizeof(*snap));
	(void)fetch_over(&ipc_c, snap.get()); // names the selected provider; an empty read is fine

	// The tasks to warm.
	std::vector<uint32_t> tasks;
	int rc = 0;
	if (task != 0) {
		tasks.push_back(task);
	} else {
		bool matched = false;
		for (uint32_t i = 0; i < snap->conv_task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
			const struct xrt_status_conv_task &t = snap->conv_tasks[i];
			if (strcmp(provider_id_of(snap.get(), t), id.c_str()) != 0 || (cap != 0 && t.cap_id != cap) ||
			    (only_task != 0 && t.task != only_task)) {
				continue;
			}
			matched = true;
			if (t.selected) {
				if (std::find(tasks.begin(), tasks.end(), t.task) == tasks.end()) {
					tasks.push_back(t.task);
				}
				continue;
			}
			const struct xrt_status_conv_task *sel = selected_row(snap.get(), t.task);
			P("%s: %s cap %u is not the selected capability (selected: %s) -- `conv warm` warms the "
			  "selection; `conv use %s %s` first to change it.\n",
			  t.task_name, id.c_str(), t.cap_id, sel != nullptr ? provider_id_of(snap.get(), *sel) : "none",
			  t.task_name, id.c_str());
			rc = 1;
		}
		if (!matched) {
			P("%s: the running service has no row for it%s.\n", arg,
			  snap->conv_task_count == 0 ? " (no conversion rows at all)" : "");
			ipc_client_connection_fini(&ipc_c);
			return snap->conv_task_count == 0 ? 2 : 1;
		}
	}

	std::vector<uint32_t> pending;
	for (uint32_t t : tasks) {
		uint32_t now = 0;
		const int r = warm_one(&ipc_c, t, snap.get(), &now);
		rc = r > rc ? r : rc;
		if (r == 0 && now != XRT_CONV_STATE_READY && !conv_state_is_terminal(now)) {
			pending.push_back(t);
		}
	}

	// --wait: follow the selected rows through the status reads until each is
	// READY or terminal (never re-sending the warm: that would be a retry).
	if (wait && !pending.empty()) {
		const uint64_t end = os_monotonic_get_ns() + (uint64_t)timeout_s * U_TIME_1S_IN_NS;
		while (!pending.empty() && os_monotonic_get_ns() < end) {
			os_nanosleep(250 * U_TIME_1MS_IN_NS);
			if (!fetch_over(&ipc_c, snap.get())) {
				break;
			}
			for (auto it = pending.begin(); it != pending.end();) {
				const struct xrt_status_conv_task *row = selected_row(snap.get(), *it);
				const uint32_t st = row != nullptr ? row->state : XRT_CONV_STATE_ABSENT;
				if (st == XRT_CONV_STATE_READY || conv_state_is_terminal(st)) {
					char a[16];
					P("%s: %s%s%s\n", conv_task_name(*it), state_upper(st, a, sizeof(a)),
					  row != nullptr && row->reason_name[0] != '\0' && st != XRT_CONV_STATE_READY
					      ? " reason="
					      : "",
					  row != nullptr && st != XRT_CONV_STATE_READY ? row->reason_name : "");
					rc = st == XRT_CONV_STATE_READY ? rc : 1;
					it = pending.erase(it);
				} else {
					++it;
				}
			}
		}
		for (uint32_t t : pending) {
			P("%s: still warming after %u s (`conv status --watch` follows it).\n", conv_task_name(t),
			  timeout_s);
			rc = rc > 1 ? rc : 1;
		}
	} else if (!pending.empty()) {
		P("`displayxr-cli conv status --watch` follows it (or pass --wait).\n");
	}
	ipc_client_connection_fini(&ipc_c);
	return rc;
#else
	(void)wait;
	(void)timeout_s;
	(void)cap;
	(void)only_task;
	P("No IPC client in this build. The CLI never warms a model headless (spec §7.1).\n");
	return 1;
#endif
}

//! Model store root (ADR-053 decision 7; spec §5.1): first configured key wins.
std::string
model_store_root(std::string &source)
{
#ifdef XRT_OS_WINDOWS
	struct
	{
		const char *sub;
		const char *value;
	} const keys[] = {
	    {"Software\\DisplayXR\\Models", "Path"},
	    {"Software\\DisplayXR\\Browser", "ModelsPath"},
	};
	for (const auto &k : keys) {
		char buf[MAX_PATH];
		DWORD sz = sizeof(buf);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, k.sub, k.value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf,
		                 &sz) == ERROR_SUCCESS &&
		    buf[0] != '\0') {
			source = std::string("HKLM\\") + k.sub + "\\" + k.value;
			return buf;
		}
	}
	char pd[MAX_PATH];
	const DWORD n = GetEnvironmentVariableA("ProgramData", pd, sizeof(pd));
	source = "%ProgramData% default";
	return std::string(n > 0 && n < sizeof(pd) ? pd : "C:\\ProgramData") + "\\DisplayXR\\Models";
#else
	source = "not defined on this platform yet (P5)";
	return "";
#endif
}

int
cmd_models(int argc, const char **argv)
{
	const char *verb = argc >= 4 ? argv[3] : "list";
	if (strcmp(verb, "list") != 0 && strcmp(verb, "prune") != 0 && strcmp(verb, "verify") != 0) {
		P("usage: displayxr-cli conv models list|verify|prune\n");
		return 1;
	}
	std::string source;
	const std::string root = model_store_root(source);
	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "store_root", root.c_str());
		cJSON_AddStringToObject(o, "source", source.c_str());
		cJSON_AddArrayToObject(o, "models");
		cJSON_AddNumberToObject(o, "pruned", 0);
		char *s = cJSON_Print(o);
		if (s != nullptr) {
			P("%s\n", s);
			cJSON_free(s);
		}
		cJSON_Delete(o);
		return 0;
	}
	P("Model store: %s  (%s)\n", root.empty() ? "(none)" : root.c_str(), source.c_str());
	if (strcmp(verb, "prune") == 0) {
		P("Nothing to prune: the shared model store and its index arrive in P4 (ADR-053 decision 7).\n");
	} else {
		P("No store index yet: model packs (schema 2) arrive in P4 (ADR-053 decision 7).\n");
	}
	return 0;
}

int
usage(void)
{
	P("Usage: displayxr-cli conv <verb>   (conversion providers, ADR-053)\n");
	P("  list [--json]                       providers x tasks, selection order, selected, reason per row\n");
	P("  status [--json] [--watch [--interval ms]]   readiness, streams, jobs (live from the service)\n");
	P("  warm <task> | <id>[/<cap>] [<task>] [--wait [--timeout s]]\n"
	  "                                      explicit prewarm, through the running service (READY: no-op)\n");
	P("  enable | disable <id> [--machine]   per-user (HKCU) Enabled; --machine = HKLM (admin)\n");
	P("  use <task> <id>[/<cap>] [--machine] per-task preference (PreferredProvider\\<task>)\n");
	P("  reset <task>|all [--machine]        clear it\n");
	P("  models list|verify|prune [--json]   the model store (P4; prints the resolved root)\n");
	P("list/status read the running service over DIAG, else describe providers headless (no model load).\n");
	return 1;
}

} // namespace

int
cli_cmd_conv(int argc, const char **argv)
{
	if (argc < 3) {
		return usage();
	}
	const char *verb = argv[2];
	const bool machine = cli_has_flag(argc, argv, "--machine");
	if (strcmp(verb, "list") == 0) {
		return cmd_list(argc, argv, false);
	}
	if (strcmp(verb, "status") == 0) {
		return cli_has_flag(argc, argv, "--watch") ? cmd_watch(argc, argv) : cmd_list(argc, argv, true);
	}
	if (strcmp(verb, "warm") == 0) {
		return cmd_warm(argc, argv);
	}
	if (strcmp(verb, "enable") == 0 || strcmp(verb, "disable") == 0) {
		if (argc < 4) {
			return usage();
		}
		return cmd_enable(argv[3], strcmp(verb, "enable") == 0, machine);
	}
	if (strcmp(verb, "use") == 0) {
		if (argc < 5) {
			return usage();
		}
		return cmd_use(argv[3], argv[4], machine);
	}
	if (strcmp(verb, "reset") == 0) {
		if (argc < 4) {
			return usage();
		}
		return cmd_reset(argv[3], machine);
	}
	if (strcmp(verb, "models") == 0) {
		return cmd_models(argc, argv);
	}
	return usage();
}
