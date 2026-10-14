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
 *   conv options <id> [--json]                       the provider's settings: declared + stored, with source
 *   conv set    <id> <KEY> <value> [--machine]       `Settings\<KEY>` (HKCU; HKLM with --machine) — N3c
 *   conv unset  <id> <KEY> [--machine]
 *   conv models list|verify|prune|fetch|init        the runtime-owned model store (P4)
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
 * `Policy\Lock` = 1). Every write ends with a poke of the running service
 * (`system_request_component_reselect`, #2051): it re-reads the policy, hands
 * `PreferredProvider` to the core and re-applies `Enabled`, so the NEXT
 * stream / job create selects under the new policy (a live stream never
 * moves, spec §3.4 rule 5) and `conv status --watch` prints a line. The
 * service's policy watch also catches a registry edit made without the CLI.
 *
 * `conv warm` (#2029) is the DIAG `conv_warm` call: it warms the task's
 * selected capability in the running service (READY is a no-op; an
 * all-terminal task is retried) and, with --wait, follows the status rows
 * until READY or terminal. It never warms headless.
 *
 * @ingroup cli
 */

#include "cli_common.h"
#include "cli_component_options.h"
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
#include "target_component_loader.h" // target_component_provenance_str (N4b), TARGET_COMPONENT_CLASS_CONVERSION (#2051)
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"
#include "target_plugin_loader.h" // target_plugin_get_active (the static dp-lift row)
#include "conversion/conv_readiness.h"
#include "conversion/conv_select.h"
#include "conversion/conv_model_store.h"

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

//! ADR-053 P4: the model store's packs, read from disk (never over IPC).
void
fill_store(struct xrt_status_snapshot *out)
{
	target_conv_store_fill_snapshot(out);
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

/*!
 * ADR-054 N4b: the rows come over DIAG IPC (or the headless describe) and
 * carry no provenance; a LISTER asks explicitly — the shared load path's
 * verdict for the registered binary, cached per file. UNKNOWN for the
 * built-in dp-lift adapter (no binary).
 */
struct conv_provenance
{
	uint32_t verdict = 0;
	char reason[160] = {0};
	char signer[128] = {0};
};

conv_provenance
provenance_of(const struct xrt_status_conv_provider &p)
{
	conv_provenance out;
	(void)target_conversion_plugin_check_provenance(p.binary, &out.verdict, out.reason, sizeof(out.reason),
	                                                out.signer, sizeof(out.signer));
	return out;
}

void
print_json(const struct xrt_status_snapshot *snap, const struct target_conv_policy *policy, bool compact)
{
	cJSON *root = u_status_conv_to_cjson(snap);
	add_policy_json(root, policy);
	// N4b: join the lister's provenance verdict onto each provider row by id.
	cJSON *providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
	for (uint32_t i = 0; i < snap->conv_provider_count && i < XRT_STATUS_MAX_CONV_PROVIDERS; i++) {
		cJSON *row = cJSON_GetArrayItem(providers, (int)i);
		if (row == nullptr) {
			break;
		}
		const conv_provenance pv = provenance_of(snap->conv_providers[i]);
		cJSON_AddStringToObject(row, "provenance", target_component_provenance_str(pv.verdict));
		cJSON_AddStringToObject(row, "provenance_reason", pv.reason);
		cJSON_AddStringToObject(row, "signer", pv.signer);
	}
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
	P("  %-18s %-16s %5s  %-10s %-9s %4s  %-12s %s\n", "id", "load", "order", "hosting", "status", "caps",
	  "provenance", "notes");
	for (uint32_t i = 0; i < snap->conv_provider_count && i < XRT_STATUS_MAX_CONV_PROVIDERS; i++) {
		const struct xrt_status_conv_provider &p = snap->conv_providers[i];
		const conv_provenance pv = provenance_of(p);
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
		P("  %-18s %-16s %5u  %-10s %-9s %4u  %-12s %s\n", p.id, p.load, p.probe_order,
		  u_status_conv_hosting_str(p.hosting), u_status_platform_state_str(p.platform_state), p.cap_count,
		  target_component_provenance_str(pv.verdict), notes.c_str());
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
	fill_store(snap.get());
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
		fill_store(snap.get());
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

//! How a policy write reaches the running service (#2051; spec §3.4 rule 5, §4.1).
void
note_service_pickup(void)
{
#ifdef CLI_HAVE_IPC
	xrt_result_t xret = XRT_SUCCESS;
	switch (cli_ipc_poke_component_reselect(TARGET_COMPONENT_CLASS_CONVERSION, &xret)) {
	case CLI_IPC_POKE_ACCEPTED:
		P("The running service re-selects now: the new preference / Enabled applies to the next stream /\n"
		  "job create (a live stream never moves); `conv status --watch` prints the change.\n");
		return;
	case CLI_IPC_POKE_NO_WATCH:
		P("The running service has no live re-select -- applies when it next starts.\n");
		return;
	case CLI_IPC_POKE_REFUSED:
		P("The running service refused the re-select request (xrt_result=%d) -- its registry watch on %s\n"
		  "still applies the write within ~1 s; otherwise at its next start.\n",
		  (int)xret, TARGET_CONV_REG_ROOT);
		return;
	case CLI_IPC_POKE_UNREACHABLE:
	default: break;
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

/*
 *
 * options / set / unset (N3c) — the shared verbs over the conversion class.
 *
 */

//! Registered plug-in provider? The built-in `dp-lift` adapter has no row and no settings.
bool
options_registered(const char *id)
{
	return strcmp(id, "dp-lift") != 0 && provider_known(id);
}

const struct cli_component_class k_conv_options_class = {
    /* verb */ "conv",
    /* noun */ "conversion provider",
    /* cls */ nullptr, // filled at first use (a static initialiser cannot call the accessor)
    /* registered */ options_registered,
    /* describe */ target_conversion_plugin_describe_settings,
    /* note_pickup */ note_service_pickup,
    /* posix_machine_hint */ "the manifest's \"settings\" object (NNN-<id>-conversion-provider.json)",
};

int
cmd_options_verbs(int argc, const char **argv)
{
	struct cli_component_class k = k_conv_options_class;
	k.cls = target_conversion_plugin_component_class();
	return cli_component_options_dispatch(&k, argc, argv);
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

/*
 *
 * `conv models` (ADR-053 P4, #2108): the runtime-owned model store, local.
 *
 */

//! Value of `--<name> <v>` (nullptr = absent).
const char *
flag_value(int argc, const char **argv, const char *name)
{
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return nullptr;
}

std::string
mb(uint64_t bytes)
{
	char b[48];
	if (bytes >= 1024ull * 1024ull) {
		snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	} else {
		snprintf(b, sizeof(b), "%llu bytes", (unsigned long long)bytes);
	}
	return b;
}

void
fetch_progress(void *user, const char *sha, uint64_t done, uint64_t total)
{
	(void)user;
	P("  %.12s…  %s / %s\n", sha, mb(done).c_str(), mb(total).c_str());
}

struct store_guard
{
	conv_model_store *s = nullptr;
	~store_guard()
	{
		conv_model_store_destroy(&s);
	}
};

//! `conv models fetch <pack-url|path> [--prefetch] [--model <name>] [--installer]`.
int
fetch_one(const char *src, int argc, const char **argv, conv_model_store *s, bool json)
{
	const bool prefetch = cli_has_flag(argc, argv, "--prefetch");
	const bool installer = cli_has_flag(argc, argv, "--installer");
	const char *only = flag_value(argc, argv, "--model");
	std::unique_ptr<conv_pack> pack(new conv_pack());
	char err[192];
	if (!conv_pack_load_url(src, pack.get(), err, sizeof(err))) {
		P("conv models fetch: %s\n", err);
		return 2;
	}
	cJSON *o = json ? cJSON_CreateObject() : nullptr;
	cJSON *arr = o != nullptr ? cJSON_AddArrayToObject(o, "models") : nullptr;
	if (o != nullptr) {
		cJSON_AddStringToObject(o, "pack", pack->pack);
		cJSON_AddStringToObject(o, "store_root", conv_model_store_root(s));
	} else {
		P("Pack '%s' (%u models) -> %s\n", pack->pack, pack->model_count, conv_model_store_root(s));
	}
	int failed = 0;
	for (uint32_t i = 0; i < pack->model_count; i++) {
		const conv_pack_model &m = pack->models[i];
		const bool wanted = only != nullptr ? strcmp(only, m.name) == 0 : prefetch ? m.prefetch : true;
		if (!wanted) {
			continue;
		}
		const char *result;
		if (installer && !m.commercial) {
			// A signed installer never pre-fetches a non-commercial blob (roadmap §11.1 c).
			result = "skipped-non-commercial";
		} else {
			result = conv_store_blob_result_str(
			    conv_model_store_fetch_model(s, pack.get(), &m, nullptr, /*skip_stamped=*/installer));
			if (strcmp(result, "ok") != 0 && strcmp(result, "skipped") != 0) {
				failed++;
			}
		}
		if (arr != nullptr) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", m.name);
			cJSON_AddStringToObject(e, "result", result);
			cJSON_AddItemToArray(arr, e);
		} else {
			P("  %-28s %s\n", m.name, result);
		}
	}
	if (o != nullptr) {
		cJSON_AddNumberToObject(o, "failed", failed);
		char *t = cJSON_Print(o);
		P("%s\n", t);
		cJSON_free(t);
		cJSON_Delete(o);
	}
	return failed > 0 ? 2 : 0;
}

/*!
 * `fetch <pack-url|path>`: that pack. `fetch --prefetch` with no URL: every
 * registered provider's pack (Packs / packs[]), its prefetch models only.
 */
int
models_fetch(int argc, const char **argv, conv_model_store *s, bool json)
{
	if (argc >= 5 && argv[4][0] != '-') {
		return fetch_one(argv[4], argc, argv, s, json);
	}
	if (!cli_has_flag(argc, argv, "--prefetch")) {
		P("usage: displayxr-cli conv models fetch <pack-url|path> [--prefetch] [--model <name>] [--installer]\n"
		  "       displayxr-cli conv models fetch --prefetch   (every registered pack's prefetch models)\n");
		return 1;
	}
	static char packs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][512];
	const uint32_t n = target_conv_store_registered_packs(packs, XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS);
	if (n == 0) {
		P("No registered model pack to pre-fetch.\n");
		return 0;
	}
	int rc = 0;
	for (uint32_t i = 0; i < n; i++) {
		const int r = fetch_one(packs[i], argc, argv, s, json);
		rc = r > rc ? r : rc;
	}
	return rc;
}

int
cmd_models(int argc, const char **argv)
{
	const char *verb = argc >= 4 && argv[3][0] != '-' ? argv[3] : "list";
	const bool json = cli_has_flag(argc, argv, "--json");
	if (strcmp(verb, "list") != 0 && strcmp(verb, "prune") != 0 && strcmp(verb, "verify") != 0 &&
	    strcmp(verb, "fetch") != 0 && strcmp(verb, "init") != 0) {
		P("usage: displayxr-cli conv models list|verify|prune|init [--json] [--root <dir>]\n"
		  "       displayxr-cli conv models fetch <pack-url|path> [--prefetch] [--model <name>] [--installer]\n");
		return 1;
	}
	char root[512] = "", source[64] = "";
	if (const char *r = flag_value(argc, argv, "--root")) {
		snprintf(root, sizeof(root), "%s", r);
		snprintf(source, sizeof(source), "--root");
	} else if (!conv_store_root_resolve(root, sizeof(root), source, sizeof(source))) {
		P("No model store root on this machine.\n");
		return 1;
	}
	struct target_conv_policy policy;
	target_conv_policy_read(&policy);
	conv_model_store_config cfg;
	conv_model_store_config_defaults(&cfg);
	snprintf(cfg.root, sizeof(cfg.root), "%s", root);
	const bool installer = cli_has_flag(argc, argv, "--installer");
	cfg.allow_downloads = true; // an explicit fetch is the user's (or the installer's) own request
	cfg.allow_research_only = !installer && policy.allow_research_only;
	if (installer) {
		snprintf(cfg.nonce, sizeof(cfg.nonce), "installer");
	}
	if (strcmp(verb, "fetch") == 0 && !json) {
		cfg.on_progress = fetch_progress;
	}
	store_guard g;
	if (!conv_model_store_create(&cfg, &g.s)) {
		P("Could not open the model store at %s\n", root);
		return 1;
	}

	if (strcmp(verb, "init") == 0) {
		const bool ok = conv_model_store_ensure_root(g.s);
		P("%s %s\n", ok ? "Model store ready:" : "Could not create the model store at", root);
		return ok ? 0 : 1;
	}
	if (strcmp(verb, "fetch") == 0) {
		return models_fetch(argc, argv, g.s, json);
	}

	// list / verify / prune: the registered providers' packs.
	static char packs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][512];
	const uint32_t np = target_conv_store_registered_packs(packs, XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS);
	for (uint32_t i = 0; i < np; i++) {
		(void)conv_model_store_add_pack_file(g.s, packs[i]);
	}
	const uint32_t n = conv_model_store_pack_count(g.s);

	if (strcmp(verb, "prune") == 0) {
		if (n == 0 && !cli_has_flag(argc, argv, "--force")) {
			// No registered pack: every blob would look unreferenced (e.g. the
			// open provider is not installed yet, or a browser owns them).
			P("No registered model pack: nothing pruned (pass --force to delete every blob in %s).\n", root);
			return 0;
		}
		uint64_t bytes = 0;
		const uint32_t deleted = conv_model_store_prune(g.s, &bytes);
		if (json) {
			P("{ \"store_root\": \"%s\", \"pruned\": %u, \"bytes\": %llu }\n", root, deleted,
			  (unsigned long long)bytes);
		} else {
			P("Pruned %u blob(s), %s, from %s\n", deleted, mb(bytes).c_str(), root);
		}
		return 0;
	}

	std::unique_ptr<conv_pack> p(new conv_pack());
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "store_root", root);
	cJSON_AddStringToObject(o, "source", source);
	cJSON *ja = cJSON_AddArrayToObject(o, "packs");
	if (!json) {
		P("Model store: %s  (%s)\n", root, source);
		if (n == 0) {
			P("No registered model pack (a provider registers one with Packs / packs[]).\n");
		}
	}
	int bad = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (!conv_model_store_get_pack(g.s, i, p.get())) {
			continue;
		}
		conv_store_pack_summary sum;
		(void)conv_model_store_pack_summary(g.s, i, &sum);
		cJSON *jp = cJSON_CreateObject();
		cJSON_AddItemToArray(ja, jp);
		cJSON_AddStringToObject(jp, "pack", p->pack);
		cJSON_AddStringToObject(jp, "provider", p->provider);
		cJSON_AddStringToObject(jp, "state", sum.state);
		cJSON_AddStringToObject(jp, "manifest", p->manifest_path);
		cJSON *jm = cJSON_AddArrayToObject(jp, "models");
		if (!json) {
			P("\n%s  [%s]  %u/%u models, %s of %s  (%s)\n", p->pack, sum.state, sum.models_installed, sum.models,
			  mb(sum.bytes_installed).c_str(), mb(sum.bytes_total).c_str(), p->manifest_path);
		}
		for (uint32_t m = 0; m < p->model_count; m++) {
			const conv_pack_model &pm = p->models[m];
			cJSON *e = cJSON_CreateObject();
			cJSON_AddItemToArray(jm, e);
			cJSON_AddStringToObject(e, "name", pm.name);
			cJSON_AddStringToObject(e, "task", pm.task);
			cJSON_AddBoolToObject(e, "commercial", pm.commercial);
			cJSON_AddBoolToObject(e, "prefetch", pm.prefetch);
			cJSON *jf = cJSON_AddArrayToObject(e, "files");
			std::string line;
			for (uint32_t v = 0; v < pm.variant_count; v++) {
				for (uint32_t f = 0; f < pm.variants[v].file_count; f++) {
					const conv_pack_file &pf = pm.variants[v].files[f];
					conv_store_blob_disk d;
					conv_model_store_blob_disk(g.s, pf.sha256, &d);
					const char *st = d.present && d.stamped && d.size == pf.size ? "installed"
					                 : d.part_bytes > 0                          ? "downloading"
					                 : d.present                                 ? "unstamped"
					                                                             : "missing";
					if (strcmp(verb, "verify") == 0 && d.present) {
						st = conv_store_blob_result_str(conv_model_store_verify_blob(g.s, pf.sha256, pf.size));
						bad += strcmp(st, "ok") != 0 ? 1 : 0;
					}
					cJSON *x = cJSON_CreateObject();
					cJSON_AddItemToArray(jf, x);
					cJSON_AddStringToObject(x, "variant", pm.variants[v].id);
					cJSON_AddStringToObject(x, "sha256", pf.sha256);
					cJSON_AddNumberToObject(x, "size", (double)pf.size);
					cJSON_AddStringToObject(x, "state", st);
					cJSON_AddNumberToObject(x, "part_bytes", (double)d.part_bytes);
					line += std::string(line.empty() ? "" : ", ") + pm.variants[v].id + ":" + st;
				}
			}
			if (!json) {
				P("  %-28s %-16s %-14s %s\n", pm.name, pm.task, pm.commercial ? "commercial" : "NON-COMMERCIAL",
				  line.c_str());
			}
		}
	}
	if (json) {
		cJSON_AddNumberToObject(o, "bad", bad);
		char *t = cJSON_Print(o);
		P("%s\n", t);
		cJSON_free(t);
	} else if (strcmp(verb, "verify") == 0) {
		P("\n%s\n", bad == 0 ? "Every blob present hashes to its name." : "Some blobs failed (deleted where allowed).");
	}
	cJSON_Delete(o);
	return bad > 0 ? 1 : 0;
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
	P("  options <id> [--json]               the provider's settings: key, type, default, value, source (N3c)\n");
	P("  set <id> <KEY> <value> [--machine]  Settings\\<KEY> on the provider's row (HKCU; --machine = HKLM)\n");
	P("  unset <id> <KEY> [--machine]        clear it; DXR_CONV_<ID>_<KEY> in the service's env always wins\n");
	P("  models list|verify|prune|init [--json] [--root <dir>]   the runtime-owned model store\n");
	P("  models fetch <pack-url|path> [--prefetch] [--model <name>] [--installer]   write protocol, here\n");
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
	const int rc = cmd_options_verbs(argc, argv); // options | set | unset (N3c)
	if (rc >= 0) {
		return rc;
	}
	return usage();
}
