// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `conv models` for locally built models (see cli_conv_models_build.h).
 *
 * The Dashboard's Build / Cancel / Remove / accept actions run exactly these
 * verbs (§5.6.5); nothing else in the runtime builds a model.
 *
 * @ingroup cli
 */

#include "cli_conv_models_build.h"

#include "conversion/conv_model_build.h"
#include "target_conv_build.h"
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include <cjson/cJSON.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

volatile sig_atomic_t g_interrupt = 0;

#ifdef _WIN32
BOOL WINAPI
on_console(DWORD ev)
{
	if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT || ev == CTRL_CLOSE_EVENT) {
		g_interrupt = 1;
		return TRUE;
	}
	return FALSE;
}
#else
void
on_signal(int)
{
	g_interrupt = 1;
}
#endif

//! Ctrl+C / SIGTERM cancel the build the documented way (its cancel file), never leave it running.
struct interrupt_scope
{
#ifdef _WIN32
	interrupt_scope()
	{
		g_interrupt = 0;
		SetConsoleCtrlHandler(on_console, TRUE);
	}
	~interrupt_scope()
	{
		SetConsoleCtrlHandler(on_console, FALSE);
	}
#else
	struct sigaction old_int, old_term, old_hup;
	interrupt_scope()
	{
		g_interrupt = 0;
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_signal;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, &old_int);
		sigaction(SIGTERM, &sa, &old_term);
		sigaction(SIGHUP, &sa, &old_hup);
	}
	~interrupt_scope()
	{
		sigaction(SIGINT, &old_int, nullptr);
		sigaction(SIGTERM, &old_term, nullptr);
		sigaction(SIGHUP, &old_hup, nullptr);
	}
#endif
};

struct event_sink
{
	bool events; //!< forward NDJSON to stdout
	bool json;   //!< quiet (the JSON object is printed at the end)
};

void
on_event(void *user, const char *line)
{
	const event_sink *k = static_cast<const event_sink *>(user);
	if (k->events) {
		fprintf(stdout, "%s\n", line);
		fflush(stdout);
		return;
	}
	if (k->json) {
		return;
	}
	cJSON *o = cJSON_Parse(line);
	const cJSON *ev = cJSON_GetObjectItemCaseSensitive(o, "event");
	const cJSON *ph = cJSON_GetObjectItemCaseSensitive(o, "phase");
	if (cJSON_IsString(ev) && strcmp(ev->valuestring, "phase") == 0 && cJSON_IsString(ph)) {
		const cJSON *step = cJSON_GetObjectItemCaseSensitive(o, "step");
		const cJSON *steps = cJSON_GetObjectItemCaseSensitive(o, "steps");
		printf("  %-16s step %d of %d\n", ph->valuestring, cJSON_IsNumber(step) ? step->valueint : 0,
		       cJSON_IsNumber(steps) ? steps->valueint : 0);
		fflush(stdout);
	} else if (cJSON_IsString(ev) && strcmp(ev->valuestring, "warning") == 0) {
		const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "message");
		printf("  warning: %s\n", cJSON_IsString(m) ? m->valuestring : "");
	}
	cJSON_Delete(o);
}

std::string
mb_of(uint64_t bytes)
{
	char b[48];
	snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	return b;
}

//! Load every registered pack into @p s, plus @p extra (a manifest path) when given.
void
add_registered(conv_model_store *s, const char *extra)
{
	static char packs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][512];
	const uint32_t n =
	    target_conv_store_registered_packs(packs, XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS);
	for (uint32_t i = 0; i < n; i++) {
		(void)conv_model_store_add_pack_file(s, packs[i]);
	}
	if (extra != nullptr && extra[0] != '\0') {
		(void)conv_model_store_add_pack_file(s, extra);
	}
}

//! Pack id of a `<pack>` argument (an id, or a manifest path / URL).
bool
pack_id_of(const char *arg, std::string &id, std::string &path)
{
	if (!cli_conv_resolve_pack(arg, path)) {
		return false;
	}
	std::unique_ptr<conv_pack> p(new conv_pack());
	char err[192];
	if (!conv_pack_load_url(path.c_str(), p.get(), err, sizeof(err))) {
		fprintf(stderr, "displayxr-cli conv models: %s\n", err);
		return false;
	}
	id = p->pack;
	return true;
}

void
print_json(cJSON *o)
{
	char *t = cJSON_Print(o);
	printf("%s\n", t);
	cJSON_free(t);
}

} // namespace

std::vector<std::string>
cli_flag_values(int argc, const char **argv, const char *name)
{
	std::vector<std::string> v;
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			v.emplace_back(argv[i + 1]);
			i++;
		}
	}
	return v;
}

bool
cli_conv_resolve_pack(const char *arg, std::string &out)
{
	if (arg == nullptr || arg[0] == '\0') {
		return false;
	}
	const bool path_like = strstr(arg, "://") != nullptr || strchr(arg, '/') != nullptr ||
	                       strchr(arg, '\\') != nullptr || strstr(arg, ".json") != nullptr;
	if (path_like) {
		out = arg;
		return true;
	}
	// A pack id: the registered manifest that declares it.
	static char packs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][512];
	const uint32_t n =
	    target_conv_store_registered_packs(packs, XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS);
	std::unique_ptr<conv_pack> p(new conv_pack());
	for (uint32_t i = 0; i < n; i++) {
		if (conv_pack_load(packs[i], p.get(), nullptr, 0) && strcmp(p->pack, arg) == 0) {
			out = packs[i];
			return true;
		}
	}
	FILE *f = fopen(arg, "rb"); // a manifest in the current directory
	if (f != nullptr) {
		fclose(f);
		out = arg;
		return true;
	}
	fprintf(stderr, "displayxr-cli conv models: no registered pack '%s' (and no such file)\n", arg);
	return false;
}

int
cli_conv_build_models(conv_model_store *s,
                      const conv_pack *pack,
                      const std::vector<std::string> &names,
                      int argc,
                      const char **argv,
                      bool json,
                      bool events,
                      cJSON *arr)
{
	if (names.empty()) {
		return 0;
	}
	target_conv_policy policy;
	target_conv_policy_read(&policy);
	bool per_run = false;
	for (int i = 2; i < argc; i++) {
		per_run = per_run || strcmp(argv[i], "--allow-research-only") == 0;
	}
	event_sink sink{events, json};
	conv_build_ops ops;
	target_conv_build_ops_fill(&ops, &policy, per_run);
	ops.user = &sink;
	ops.on_event = on_event;
	// The record functions ignore their user pointer; on_event reads it.
	const std::vector<std::string> accept = cli_flag_values(argc, argv, "--accept-license");
	std::vector<const char *> accept_c;
	for (const std::string &a : accept) {
		accept_c.push_back(a.c_str());
	}
	conv_build_options opt;
	conv_build_options_defaults(&opt);
	opt.accept_licences = accept_c.empty() ? nullptr : accept_c.data();
	opt.accept_count = (uint32_t)accept_c.size();
	opt.no_wait = events; // §5.6.3 step 6: the Dashboard is told `busy`, a terminal waits
	opt.interrupt = &g_interrupt;

	// The CLI prints the licence it records (§5.6.3 step 2), on stderr when stdout is JSON.
	FILE *human = json || events ? stderr : stdout;
	for (const std::string &a : accept) {
		for (uint32_t b = 0; b < pack->build_count; b++) {
			for (uint32_t k = 0; k < pack->builds[b].source_count; k++) {
				const conv_pack_source_license &l = pack->builds[b].sources[k].license;
				if (a != l.id) {
					continue;
				}
				char text[CONV_STORE_PATH_MAX];
				(void)conv_pack_license_text_path(pack, &l, text, sizeof(text));
				fprintf(human, "Accepting licence '%s': %s\n  %s\n  full text: %s\n", l.id, l.name,
				        l.link, text);
			}
		}
	}
	std::vector<const char *> names_c;
	for (const std::string &n : names) {
		names_c.push_back(n.c_str());
	}
	std::vector<conv_build_model_result> res(names.size());
	if (!json && !events) {
		printf(
		    "Building %zu model(s) on this machine from the official files (this can take a while; Ctrl+C "
		    "cancels)\n",
		    names.size());
	}
	uint32_t failed = 0;
	{
		interrupt_scope scope;
		failed =
		    conv_model_store_build(s, pack, names_c.data(), (uint32_t)names_c.size(), &ops, &opt, res.data());
	}
	for (const conv_build_model_result &r : res) {
		const char *word = conv_build_result_str(r.result);
		if (arr != nullptr) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", r.name);
			cJSON_AddStringToObject(e, "result", word);
			if (r.hint[0] != '\0') {
				cJSON_AddStringToObject(e, "hint", r.hint);
			}
			if (r.ran) {
				cJSON *b = cJSON_AddObjectToObject(e, "build");
				cJSON_AddStringToObject(b, "tool", r.tool);
				cJSON_AddStringToObject(b, "tool_version", r.tool_version);
				cJSON_AddStringToObject(b, "provenance", r.provenance);
				cJSON_AddStringToObject(b, "staging", r.staging);
				cJSON_AddStringToObject(b, "log", r.log);
				cJSON_AddNumberToObject(b, "exit_code", r.exit_code);
			}
			cJSON_AddItemToArray(arr, e);
		} else {
			printf("  %-28s %s%s%s\n", r.name, word, r.hint[0] != '\0' ? "  -- " : "", r.hint);
			if (r.ran && r.log[0] != '\0' && r.result != CONV_BUILD_BUILT) {
				printf("  %-28s builder log: %s\n", "", r.log);
			}
		}
	}
	return (int)failed;
}

int
cli_conv_models_cancel(int argc, const char **argv, conv_model_store *s, bool json)
{
	const std::vector<std::string> models = cli_flag_values(argc, argv, "--model");
	if (argc < 5 || argv[4][0] == '-' || models.empty()) {
		printf("usage: displayxr-cli conv models cancel <pack> --model <name> [--json]\n");
		return 1;
	}
	std::string id, path;
	if (!pack_id_of(argv[4], id, path)) {
		return 1;
	}
	int found = 0;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "pack", id.c_str());
	cJSON *a = cJSON_AddArrayToObject(o, "models");
	for (const std::string &m : models) {
		const bool ok = conv_model_store_build_cancel(s, id.c_str(), m.c_str());
		found += ok ? 1 : 0;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", m.c_str());
		cJSON_AddBoolToObject(e, "cancelled", ok);
		cJSON_AddItemToArray(a, e);
		if (!json) {
			printf("%s/%s: %s\n", id.c_str(), m.c_str(),
			       ok ? "cancel requested (the build stops within seconds; nothing is installed)"
			          : "no running build");
		}
	}
	if (json) {
		print_json(o);
	}
	cJSON_Delete(o);
	return found == (int)models.size() ? 0 : 1;
}

int
cli_conv_models_remove(int argc, const char **argv, conv_model_store *s, bool json)
{
	const std::vector<std::string> models = cli_flag_values(argc, argv, "--model");
	if (argc < 5 || argv[4][0] == '-' || models.empty()) {
		printf("usage: displayxr-cli conv models remove <pack> --model <name> [--json]\n");
		return 1;
	}
	std::string id, path;
	if (!pack_id_of(argv[4], id, path)) {
		return 1;
	}
	add_registered(s, path.c_str());
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "pack", id.c_str());
	cJSON_AddStringToObject(o, "store_root", conv_model_store_root(s));
	cJSON *a = cJSON_AddArrayToObject(o, "models");
	for (const std::string &m : models) {
		uint64_t bytes = 0;
		const uint32_t n = conv_model_store_remove_model(s, id.c_str(), m.c_str(), &bytes);
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", m.c_str());
		cJSON_AddNumberToObject(e, "blobs_deleted", n);
		cJSON_AddNumberToObject(e, "bytes", (double)bytes);
		cJSON_AddItemToArray(a, e);
		if (!json) {
			printf(
			    "%s/%s: removed %u blob(s), %s (AllowResearchOnly and the licence record are unchanged)\n",
			    id.c_str(), m.c_str(), n, mb_of(bytes).c_str());
		}
	}
	if (json) {
		print_json(o);
	}
	cJSON_Delete(o);
	return 0;
}

int
cli_conv_models_license(int argc, const char **argv, conv_model_store *s, bool json)
{
	// conv models license <accept|withdraw|status> <licence-id> [--pack <pack>]
	if (argc < 6 ||
	    (strcmp(argv[4], "accept") != 0 && strcmp(argv[4], "withdraw") != 0 && strcmp(argv[4], "status") != 0)) {
		printf(
		    "usage: displayxr-cli conv models license accept|withdraw|status <licence-id> [--pack <pack>] "
		    "[--json]\n");
		return 1;
	}
	const char *verb = argv[4];
	const char *lid = argv[5];
	std::string extra;
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--pack") == 0 && !cli_conv_resolve_pack(argv[i + 1], extra)) {
			return 1;
		}
	}
	add_registered(s, extra.c_str());
	// Find the licence in the packs: its name, link, text and the text's hash.
	std::unique_ptr<conv_pack> p(new conv_pack());
	const conv_pack_source_license *found = nullptr;
	conv_pack_source_license lic{};
	std::string text_path;
	bool verified = false, conflict = false;
	char why[256] = "";
	for (uint32_t i = 0; i < conv_model_store_pack_count(s); i++) {
		if (!conv_model_store_get_pack(s, i, p.get())) {
			continue;
		}
		for (uint32_t b = 0; b < p->build_count; b++) {
			for (uint32_t k = 0; k < p->builds[b].source_count; k++) {
				const conv_pack_source_license &l = p->builds[b].sources[k].license;
				if (strcmp(l.id, lid) != 0) {
					continue;
				}
				if (found != nullptr && strcmp(lic.text_sha256, l.text_sha256) != 0) {
					conflict = true;
					continue;
				}
				if (found == nullptr) {
					lic = l;
					found = &lic;
					char tp[CONV_STORE_PATH_MAX];
					(void)conv_pack_license_text_path(p.get(), &l, tp, sizeof(tp));
					text_path = tp;
					verified = conv_pack_license_text_verify(p.get(), &l, why, sizeof(why));
				}
			}
		}
	}
	char rec_sha[80] = "";
	uint64_t when = 0;
	const bool have = target_conv_licence_read(lid, rec_sha, sizeof(rec_sha), &when);
	int rc = 0;
	std::string outcome;
	if (strcmp(verb, "accept") == 0) {
		if (found == nullptr) {
			outcome = "unknown-licence";
			rc = 2;
		} else if (conflict) {
			outcome = "ambiguous";
			rc = 2;
		} else if (!verified) {
			outcome = "build-license-text";
			rc = 2;
		} else if (!target_conv_licence_write(lid, lic.text_sha256)) {
			outcome = "io";
			rc = 2;
		} else {
			outcome = "accepted";
			if (!json) {
				printf("Accepted licence '%s': %s\n  %s\n  full text: %s\n", lid, lic.name, lic.link,
				       text_path.c_str());
				printf("Research-only models also need AllowResearchOnly (`conv research on`).\n");
			}
		}
	} else if (strcmp(verb, "withdraw") == 0) {
		outcome = target_conv_licence_withdraw(lid) ? "withdrawn" : "io";
		rc = outcome == "io" ? 2 : 0;
		if (!json && rc == 0) {
			printf(
			    "Withdrew licence '%s': models under it are no longer handed out to this user (their files "
			    "stay until `conv models remove`).\n",
			    lid);
		}
	} else {
		outcome = !have                                                       ? "not-accepted"
		          : found != nullptr && strcmp(rec_sha, lic.text_sha256) != 0 ? "outdated"
		                                                                      : "accepted";
		if (!json) {
			if (have) {
				char date[32] = "";
				const time_t t = (time_t)when;
				struct tm tmv;
#ifdef _WIN32
				gmtime_s(&tmv, &t);
#else
				gmtime_r(&t, &tmv);
#endif
				strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
				printf("Licence '%s': %s on %s (text %.12s…)\n", lid, outcome.c_str(), date, rec_sha);
			} else {
				printf("Licence '%s': not accepted by this user\n", lid);
			}
		}
	}
	if (!json && rc != 0) {
		printf("Licence '%s': %s%s%s\n", lid, outcome.c_str(), why[0] != '\0' ? " -- " : "", why);
	}
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "license", lid);
		cJSON_AddStringToObject(o, "result", outcome.c_str());
		if (found != nullptr) {
			cJSON_AddStringToObject(o, "name", lic.name);
			cJSON_AddStringToObject(o, "link", lic.link);
			cJSON_AddStringToObject(o, "text", text_path.c_str());
			cJSON_AddStringToObject(o, "text_sha256", lic.text_sha256);
			cJSON_AddBoolToObject(o, "text_verified", verified);
		}
		char sha2[80] = "";
		uint64_t when2 = 0;
		const bool now_have = target_conv_licence_read(lid, sha2, sizeof(sha2), &when2);
		cJSON_AddBoolToObject(o, "accepted",
		                      now_have && (found == nullptr || strcmp(sha2, lic.text_sha256) == 0));
		if (now_have) {
			cJSON_AddStringToObject(o, "accepted_text_sha256", sha2);
			cJSON_AddNumberToObject(o, "accepted_at", (double)when2);
		}
		print_json(o);
		cJSON_Delete(o);
	}
	return rc;
}

void
cli_conv_models_list_model(cJSON *e, const conv_pack *p, const conv_pack_model *m, conv_model_store *s)
{
	cJSON_AddBoolToObject(e, "installed", [&] {
		for (uint32_t v = 0; v < m->variant_count; v++) {
			bool all = true;
			for (uint32_t f = 0; f < m->variants[v].file_count && all; f++) {
				conv_store_blob_disk d;
				conv_model_store_blob_disk(s, m->variants[v].files[f].sha256, &d);
				all = d.present && d.stamped && d.size == m->variants[v].files[f].size;
			}
			if (all) {
				return true;
			}
		}
		return false;
	}());
	uint64_t installed_size = 0;
	if (m->variant_count > 0) {
		for (uint32_t f = 0; f < m->variants[0].file_count; f++) {
			installed_size += m->variants[0].files[f].size;
		}
	}
	cJSON_AddNumberToObject(e, "size", (double)installed_size);
	const conv_pack_build *b = conv_pack_model_build(p, m);
	if (b == nullptr) {
		return;
	}
	cJSON *lb = cJSON_AddObjectToObject(e, "local_build");
	cJSON_AddStringToObject(lb, "recipe", b->recipe);
	cJSON_AddStringToObject(lb, "variant", b->variant);
	cJSON *t = cJSON_AddObjectToObject(lb, "tool");
	cJSON_AddStringToObject(t, "id", b->tool_id);
	cJSON_AddStringToObject(t, "catalog_id", b->tool_catalog_id);
	cJSON_AddStringToObject(t, "min_version", b->tool_min_version);
	conv_build_tool tool;
	const bool have_tool = target_conv_build_find_tool(nullptr, b->tool_id, &tool);
	cJSON_AddBoolToObject(lb, "tool_installed", have_tool);
	cJSON_AddStringToObject(lb, "tool_version", have_tool ? tool.version : "");
	cJSON_AddBoolToObject(
	    lb, "tool_current",
	    have_tool && (tool.version[0] == '\0' || conv_build_version_cmp(tool.version, b->tool_min_version) >= 0));
	cJSON *n = cJSON_AddObjectToObject(lb, "needs");
	cJSON_AddNumberToObject(n, "ram_mb", (double)b->needs_ram_mb);
	cJSON_AddNumberToObject(n, "disk_mb", (double)b->needs_disk_mb);
	cJSON *os = cJSON_AddArrayToObject(n, "os");
	for (uint32_t i = 0; i < b->os_count; i++) {
		cJSON_AddItemToArray(os, cJSON_CreateString(b->os[i]));
	}
	cJSON *srcs = cJSON_AddArrayToObject(lb, "sources");
	bool accepted = true;
	uint64_t download = 0;
	for (uint32_t k = 0; k < b->source_count; k++) {
		const conv_pack_source &sc = b->sources[k];
		download += sc.size;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddItemToArray(srcs, x);
		cJSON_AddStringToObject(x, "role", sc.role);
		cJSON_AddNumberToObject(x, "size", (double)sc.size);
		cJSON_AddStringToObject(x, "url", sc.url);
		cJSON *l = cJSON_AddObjectToObject(x, "license");
		cJSON_AddStringToObject(l, "id", sc.license.id);
		cJSON_AddStringToObject(l, "name", sc.license.name);
		cJSON_AddStringToObject(l, "link", sc.license.link);
		char tp[CONV_STORE_PATH_MAX];
		(void)conv_pack_license_text_path(p, &sc.license, tp, sizeof(tp));
		cJSON_AddStringToObject(l, "text", tp);
		cJSON_AddStringToObject(l, "text_sha256", sc.license.text_sha256);
		char why[256];
		cJSON_AddBoolToObject(l, "text_verified",
		                      conv_pack_license_text_verify(p, &sc.license, why, sizeof(why)));
		char sha[80];
		uint64_t when = 0;
		const bool rec = target_conv_licence_read(sc.license.id, sha, sizeof(sha), &when) &&
		                 strcmp(sha, sc.license.text_sha256) == 0;
		cJSON_AddBoolToObject(l, "accepted", rec);
		if (rec) {
			cJSON_AddNumberToObject(l, "accepted_at", (double)when);
		}
		accepted = accepted && rec;
	}
	cJSON_AddNumberToObject(lb, "download_size", (double)download);
	cJSON_AddBoolToObject(e, "license_accepted", accepted);
}

void
cli_conv_models_list_root(cJSON *root, conv_model_store *s, const target_conv_policy *policy)
{
	conv_build_ops ops;
	target_conv_build_ops_fill(&ops, policy, false);
	cJSON *r = cJSON_AddObjectToObject(root, "research");
	cJSON_AddBoolToObject(r, "allow_research_only", ops.allow_research_only);
	cJSON_AddBoolToObject(r, "locked_off", ops.research_locked_off);
	conv_build_machine mc;
	(void)conv_build_machine_read(conv_model_store_root(s), &mc);
	cJSON *m = cJSON_AddObjectToObject(root, "machine");
	if (mc.phys_known) {
		cJSON_AddNumberToObject(m, "phys_mb", (double)mc.phys_mb);
	}
	if (mc.free_known) {
		cJSON_AddNumberToObject(m, "free_mb", (double)mc.free_mb);
	}
	cJSON_AddStringToObject(m, "os", mc.os);
	cJSON_AddStringToObject(m, "os_version", mc.os_version);
}
