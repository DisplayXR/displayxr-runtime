// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Conversion-provider diagnostics (ADR-053, #2006): the status
 *        snapshot's conversion rows, their DIAG reply size, the `conversion`
 *        generation, the CONV_* warnings, the `conv list --json` schema, and
 *        the headless pass + selftest exit-13 path on stub providers.
 *
 * Host-only, no GPU, no registry: stub DLLs are handed to the headless pass
 * by path (tests_conv_snapshot_stub.c), like tests_conversion_loader.
 */

#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include "util/u_status_snapshot.h"
#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_display_status.h"

#ifdef TESTS_HAVE_CLI_QUERY
#include "cli_query.h"
#endif

#include "catch_amalgamated.hpp"

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

void
cpy(char *dst, size_t cap, const char *s)
{
	std::snprintf(dst, cap, "%s", s);
}

xrt_status_conv_provider
provider(const char *id)
{
	xrt_status_conv_provider p;
	std::memset(&p, 0, sizeof(p));
	cpy(p.id, sizeof(p.id), id);
	cpy(p.name, sizeof(p.name), id);
	cpy(p.load, sizeof(p.load), "NEGOTIATED");
	p.abi = 1;
	p.probe_order = 100;
	p.enabled = true;
	p.described = true;
	p.cap_count = 1;
	p.licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	return p;
}

xrt_status_conv_task
task(uint32_t prov, uint32_t t, const char *name, uint32_t state, bool selected, uint32_t order)
{
	xrt_status_conv_task r;
	std::memset(&r, 0, sizeof(r));
	r.provider = prov;
	r.task = t;
	cpy(r.task_name, sizeof(r.task_name), name);
	r.cap_id = 1;
	r.state = state;
	cpy(r.state_name, sizeof(r.state_name), "READY");
	cpy(r.reason_name, sizeof(r.reason_name), "none");
	r.selected = selected;
	r.order = order;
	cpy(r.verdict, sizeof(r.verdict), selected ? "selected" : "behind");
	r.lanes = XRT_CONV_LANE_STREAM;
	r.licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	return r;
}

//! A healthy service snapshot with one provider serving depth-video.
std::unique_ptr<xrt_status_snapshot>
healthy()
{
	auto s = std::make_unique<xrt_status_snapshot>();
	std::memset(s.get(), 0, sizeof(*s));
	s->schema = XRT_STATUS_SCHEMA;
	s->source = XRT_STATUS_SOURCE_SERVICE;
	s->generation = {3, 7, 11};
	s->conv_provider_count = 1;
	s->conv_providers[0] = provider("sim-conversion");
	s->conv_task_count = 1;
	s->conv_tasks[0] = task(0, XRT_CONV_TASK_DEPTH_VIDEO, "depth-video", XRT_CONV_STATE_READY, true, 0);
	return s;
}

bool
has_warning(const xrt_status_snapshot &s, const char *code)
{
	for (uint32_t i = 0; i < s.warning_count; i++) {
		if (std::strcmp(s.warnings[i].code, code) == 0) {
			return true;
		}
	}
	return false;
}

const xrt_status_warning *
find_warning(const xrt_status_snapshot &s, const char *code)
{
	for (uint32_t i = 0; i < s.warning_count; i++) {
		if (std::strcmp(s.warnings[i].code, code) == 0) {
			return &s.warnings[i];
		}
	}
	return nullptr;
}

} // namespace


/*
 *
 * Wire: one row per DIAG reply, under 1 KB.
 *
 */

TEST_CASE("conv snapshot: each row reply fits the 1 KB DIAG limit", "[conv_snapshot]")
{
	// What a system_get_conv_*_row reply carries: result code + row + generation.
	struct provider_reply
	{
		int32_t result;
		xrt_status_conv_provider row;
		xrt_status_generation generation;
	};
	struct task_reply
	{
		int32_t result;
		xrt_status_conv_task row;
		xrt_status_generation generation;
	};
	STATIC_REQUIRE(sizeof(provider_reply) <= XRT_STATUS_CONV_ROW_REPLY_MAX);
	STATIC_REQUIRE(sizeof(task_reply) <= XRT_STATUS_CONV_ROW_REPLY_MAX);
	STATIC_REQUIRE(XRT_STATUS_CONV_ROW_REPLY_MAX == 1024);
	// The roadmap's caps (§6.1): <= 8 providers, <= 32 task rows.
	STATIC_REQUIRE(XRT_STATUS_MAX_CONV_PROVIDERS == 8);
	STATIC_REQUIRE(XRT_STATUS_MAX_CONV_TASKS == 32);
	// The head does NOT carry the rows (it would grow by ~20 KB): counts only.
	STATIC_REQUIRE(sizeof(xrt_status_head) + sizeof(xrt_status_conv_provider) < sizeof(xrt_status_snapshot));
	INFO("provider row " << sizeof(xrt_status_conv_provider) << " B, task row " << sizeof(xrt_status_conv_task)
	                     << " B");
	CHECK(sizeof(provider_reply) <= 1024);
}

TEST_CASE("conv snapshot: the head carries the row counts, and round-trips them", "[conv_snapshot]")
{
	auto s = healthy();
	xrt_status_head head;
	u_status_snapshot_get_head(s.get(), &head);
	CHECK(head.conv_provider_count == 1);
	CHECK(head.conv_task_count == 1);
	CHECK(head.generation.conversion == 11);

	auto back = std::make_unique<xrt_status_snapshot>();
	u_status_snapshot_set_head(back.get(), &head);
	CHECK(back->conv_provider_count == 1);
	CHECK(back->conv_task_count == 1);
	CHECK(back->conv_providers[0].id[0] == '\0'); // rows come separately, by index

	// Clamped, never trusted.
	head.conv_provider_count = 1000;
	head.conv_task_count = 1000;
	u_status_snapshot_set_head(back.get(), &head);
	CHECK(back->conv_provider_count == XRT_STATUS_MAX_CONV_PROVIDERS);
	CHECK(back->conv_task_count == XRT_STATUS_MAX_CONV_TASKS);
}

TEST_CASE("conv snapshot: a readiness transition moves the generation", "[conv_snapshot]")
{
	xrt_status_generation a = {3, 7, 11};
	xrt_status_generation b = a;
	CHECK(u_status_generation_equal(&a, &b));
	b.conversion++; // the core bumps it on every readiness transition
	CHECK_FALSE(u_status_generation_equal(&a, &b));
	b = a;
	b.status++;
	CHECK_FALSE(u_status_generation_equal(&a, &b));
}

namespace {
struct ActivityCall
{
	std::string provider;
	uint32_t cap_id;
	bool composed;
};

//! A scripted target_conv_activity_fn: "fake" cap 1 has 2 streams + 1 job, its composed row 3 streams.
void
fake_activity(void *ud, const char *provider_id, uint32_t cap_id, bool composed, uint32_t *streams, uint32_t *jobs)
{
	auto *calls = static_cast<std::vector<ActivityCall> *>(ud);
	calls->push_back({provider_id, cap_id, composed});
	*streams = 0;
	*jobs = 0;
	if (std::strcmp(provider_id, "fake") == 0 && cap_id == 1) {
		*streams = composed ? 3u : 2u;
		*jobs = composed ? 0u : 1u;
	}
}
} // namespace

TEST_CASE("conv snapshot: streams / jobs columns come from the live core per task row (#2029)", "[conv_snapshot]")
{
	auto st = std::make_unique<target_conv_status>();
	std::memset(st.get(), 0, sizeof(*st));
	st->provider_count = 2;
	st->providers[0] = provider("other");
	st->providers[1] = provider("fake");
	st->task_count = 4;
	st->tasks[0] = task(1, XRT_CONV_TASK_DEPTH_VIDEO, "depth-video", XRT_CONV_STATE_READY, true, 0);
	st->tasks[1] = task(1, XRT_CONV_TASK_VIEWS_MONO, "views-mono", XRT_CONV_STATE_READY, true, 0);
	st->tasks[1].composed = true;
	st->tasks[2] = task(0, XRT_CONV_TASK_DEPTH_VIDEO, "depth-video", XRT_CONV_STATE_READY, false, 1);
	// A row whose provider fell off the provider table: never asked, reads 0.
	st->tasks[3] = task(XRT_STATUS_CONV_NO_PROVIDER, XRT_CONV_TASK_DEPTH_MONO, "depth-mono",
	                    XRT_CONV_STATE_READY, false, 0);
	st->tasks[3].streams = 9; // stale values are cleared

	std::vector<ActivityCall> calls;
	target_conv_status_fill_activity(st.get(), fake_activity, &calls);
	REQUIRE(calls.size() == 3);
	CHECK(calls[0].provider == "fake");
	CHECK_FALSE(calls[0].composed);
	CHECK(calls[1].composed);
	CHECK(calls[2].provider == "other");
	CHECK(st->tasks[0].streams == 2);
	CHECK(st->tasks[0].jobs_queued == 1);
	CHECK(st->tasks[1].streams == 3);
	CHECK(st->tasks[1].jobs_queued == 0);
	CHECK(st->tasks[2].streams == 0);
	CHECK(st->tasks[3].streams == 0);

	// The columns reach the snapshot (and so `conv status` / the dashboard).
	auto snap = std::make_unique<xrt_status_snapshot>();
	std::memset(snap.get(), 0, sizeof(*snap));
	target_conv_status_to_snapshot(st.get(), snap.get());
	CHECK(snap->conv_tasks[0].streams == 2);
	CHECK(snap->conv_tasks[0].jobs_queued == 1);

	// No callback (the headless pass): untouched, no crash.
	target_conv_status_fill_activity(st.get(), nullptr, nullptr);
	CHECK(st->tasks[0].streams == 2);
}


/*
 *
 * Warnings.
 *
 */

TEST_CASE("conv snapshot: a healthy table raises no CONV_* warning", "[conv_snapshot][warnings]")
{
	auto s = healthy();
	u_status_warnings_derive(s.get());
	for (uint32_t i = 0; i < s->warning_count; i++) {
		CHECK(std::strncmp(s->warnings[i].code, "CONV_", 5) != 0);
	}
}

TEST_CASE("conv snapshot: CONV_CRASH_LOOP on FAILED(crash_loop)", "[conv_snapshot][warnings]")
{
	auto s = healthy();
	s->conv_tasks[0].state = XRT_CONV_STATE_FAILED;
	s->conv_tasks[0].reason = XRT_CONV_FAIL_CRASH_LOOP;
	s->conv_tasks[0].selected = false;
	u_status_warnings_derive(s.get());
	const xrt_status_warning *w = find_warning(*s, U_STATUS_W_CONV_CRASH_LOOP);
	REQUIRE(w != nullptr);
	CHECK(w->level == XRT_STATUS_LEVEL_CRITICAL);
	CHECK(std::string(w->text).find("sim-conversion") != std::string::npos);

	// Any other FAILED reason is not a crash loop.
	s->conv_tasks[0].reason = XRT_CONV_FAIL_MODEL;
	u_status_warnings_derive(s.get());
	CHECK_FALSE(has_warning(*s, U_STATUS_W_CONV_CRASH_LOOP));
}

TEST_CASE("conv snapshot: CONV_RESEARCH_MODEL_ENABLED only for a non-exempt selected row", "[conv_snapshot][warnings]")
{
	auto s = healthy();
	s->conv_tasks[0].licence_class = XRT_CONV_LICENCE_RESEARCH_ONLY;
	u_status_warnings_derive(s.get());
	const xrt_status_warning *w = find_warning(*s, U_STATUS_W_CONV_RESEARCH_MODEL_ENABLED);
	REQUIRE(w != nullptr);
	CHECK(w->level == XRT_STATUS_LEVEL_WARN);

	// UNKNOWN counts as research-only ...
	s->conv_tasks[0].licence_class = XRT_CONV_LICENCE_UNKNOWN;
	u_status_warnings_derive(s.get());
	CHECK(has_warning(*s, U_STATUS_W_CONV_RESEARCH_MODEL_ENABLED));
	// ... except for the grandfathered dp-lift adapter.
	s->conv_providers[0].licence_exempt = true;
	u_status_warnings_derive(s.get());
	CHECK_FALSE(has_warning(*s, U_STATUS_W_CONV_RESEARCH_MODEL_ENABLED));
	// Not selected: nothing serves with it.
	s->conv_providers[0].licence_exempt = false;
	s->conv_tasks[0].selected = false;
	s->conv_tasks[0].order = XRT_STATUS_CONV_NOT_ORDERED;
	u_status_warnings_derive(s.get());
	CHECK_FALSE(has_warning(*s, U_STATUS_W_CONV_RESEARCH_MODEL_ENABLED));
}

TEST_CASE("conv snapshot: CONV_TASK_UNSERVED when ordered candidates all fell through", "[conv_snapshot][warnings]")
{
	auto s = healthy();
	s->conv_task_count = 2;
	s->conv_tasks[0] = task(0, XRT_CONV_TASK_VIEWS_MONO, "views-mono", XRT_CONV_STATE_FAILED, false, 0);
	s->conv_tasks[1] = task(0, XRT_CONV_TASK_VIEWS_MONO, "views-mono", XRT_CONV_STATE_ABSENT, false, 1);
	u_status_warnings_derive(s.get());
	const xrt_status_warning *w = find_warning(*s, U_STATUS_W_CONV_TASK_UNSERVED);
	REQUIRE(w != nullptr);
	CHECK(std::string(w->text).find("views-mono") != std::string::npos);

	// Every row dropped by the filter (e.g. a disabled provider): deliberate, no warning.
	s->conv_tasks[0].order = XRT_STATUS_CONV_NOT_ORDERED;
	s->conv_tasks[1].order = XRT_STATUS_CONV_NOT_ORDERED;
	u_status_warnings_derive(s.get());
	CHECK_FALSE(has_warning(*s, U_STATUS_W_CONV_TASK_UNSERVED));

	// A selected row serves it.
	s->conv_tasks[1].order = 0;
	s->conv_tasks[1].selected = true;
	u_status_warnings_derive(s.get());
	CHECK_FALSE(has_warning(*s, U_STATUS_W_CONV_TASK_UNSERVED));
}

TEST_CASE("conv snapshot: CONV_CPU_STAGING is info", "[conv_snapshot][warnings]")
{
	auto s = healthy();
	s->conv_tasks[0].cpu_staging = true;
	u_status_warnings_derive(s.get());
	const xrt_status_warning *w = find_warning(*s, U_STATUS_W_CONV_CPU_STAGING);
	REQUIRE(w != nullptr);
	CHECK(w->level == XRT_STATUS_LEVEL_INFO);
}


/*
 *
 * JSON: the snapshot keys and the `conv list --json` document.
 *
 */

TEST_CASE("conv snapshot: status JSON adds conv_providers / conv_tasks, schema stays 1", "[conv_snapshot][json]")
{
	auto s = healthy();
	u_status_warnings_derive(s.get());
	cJSON *root = u_status_snapshot_to_cjson(s.get());
	REQUIRE(root != nullptr);
	CHECK(cJSON_GetObjectItem(root, "schema")->valuedouble == 1.0);
	cJSON *gen = cJSON_GetObjectItem(root, "generation");
	REQUIRE(gen != nullptr);
	CHECK(cJSON_GetObjectItem(gen, "conversion")->valuedouble == 11.0);
	CHECK(cJSON_GetObjectItem(gen, "topology") != nullptr); // existing keys untouched
	cJSON *cp = cJSON_GetObjectItem(root, "conv_providers");
	REQUIRE(cJSON_IsArray(cp));
	CHECK(cJSON_GetArraySize(cp) == 1);
	cJSON *ct = cJSON_GetObjectItem(root, "conv_tasks");
	REQUIRE(cJSON_IsArray(ct));
	CHECK(cJSON_GetArraySize(ct) == 1);
	cJSON_Delete(root);
}

TEST_CASE("conv snapshot: `conv list --json` schema", "[conv_snapshot][json]")
{
	auto s = healthy();
	s->conv_tasks[0].licence_class = XRT_CONV_LICENCE_RESEARCH_ONLY;
	u_status_warnings_derive(s.get());
	cJSON *root = u_status_conv_to_cjson(s.get());
	REQUIRE(root != nullptr);

	for (const char *k : {"schema", "source", "generation", "providers", "tasks", "warnings"}) {
		INFO(k);
		CHECK(cJSON_GetObjectItem(root, k) != nullptr);
	}
	CHECK(std::string(cJSON_GetObjectItem(root, "source")->valuestring) == "service");

	cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "providers"), 0);
	REQUIRE(p != nullptr);
	for (const char *k : {"id", "name", "vendor", "version", "load", "binary", "abi", "probe_order", "hosting",
	                      "status", "hint", "enabled", "fallback", "hidden", "licence_exempt", "research_opt_in",
	                      "described", "host_pid", "restarts", "last_exit_code", "adapter_luid", "backend",
	                      "vram_used_mb", "licence_class", "licence_spdx", "cap_count", "bad_descriptors",
	                      "descriptor_error"}) {
		INFO(k);
		CHECK(cJSON_GetObjectItem(p, k) != nullptr);
	}
	CHECK(std::string(cJSON_GetObjectItem(p, "hosting")->valuestring) == "in_service");
	CHECK(std::string(cJSON_GetObjectItem(p, "licence_class")->valuestring) == "PERMISSIVE");

	cJSON *t = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "tasks"), 0);
	REQUIRE(t != nullptr);
	for (const char *k : {"provider_index", "provider", "task", "cap_id", "composed", "state", "reason", "progress",
	                      "selected", "order", "verdict", "stream", "job", "min_views", "max_views",
	                      "licence_class", "backend", "model_id", "typical_latency_ms", "streams", "jobs_queued",
	                      "cpu_staging"}) {
		INFO(k);
		CHECK(cJSON_GetObjectItem(t, k) != nullptr);
	}
	CHECK(std::string(cJSON_GetObjectItem(t, "provider")->valuestring) == "sim-conversion");
	CHECK(std::string(cJSON_GetObjectItem(t, "task")->valuestring) == "depth-video");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(t, "selected")));

	// Only CONV_* warnings travel in this document.
	cJSON *w = cJSON_GetObjectItem(root, "warnings");
	REQUIRE(cJSON_GetArraySize(w) == 1);
	CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(w, 0), "code")->valuestring) ==
	      U_STATUS_W_CONV_RESEARCH_MODEL_ENABLED);
	cJSON_Delete(root);
}

TEST_CASE("conv snapshot: text shows the CONVERSION section only when there are rows", "[conv_snapshot][text]")
{
	auto s = healthy();
	std::string t(u_status_snapshot_format_text(s.get(), nullptr, 0) + 1, '\0');
	u_status_snapshot_format_text(s.get(), &t[0], t.size());
	CHECK(t.find("CONVERSION  1 provider") != std::string::npos);
	CHECK(t.find("depth-video") != std::string::npos);

	s->conv_provider_count = 0;
	s->conv_task_count = 0;
	std::string e(u_status_snapshot_format_text(s.get(), nullptr, 0) + 1, '\0');
	u_status_snapshot_format_text(s.get(), &e[0], e.size());
	CHECK(e.find("CONVERSION") == std::string::npos);
}


/*
 *
 * The headless pass on stub providers.
 *
 */

TEST_CASE("conv headless: policy subkeys are never providers", "[conv_snapshot][headless]")
{
	CHECK(target_conv_is_policy_subkey("Policy"));
	CHECK(target_conv_is_policy_subkey("PreferredProvider"));
	CHECK(target_conv_is_policy_subkey("Prewarm"));
	CHECK_FALSE(target_conv_is_policy_subkey("sim-conversion"));
	CHECK_FALSE(target_conv_is_policy_subkey(nullptr));
}

TEST_CASE("conv headless: no provider is a PASS with zero rows", "[conv_snapshot][headless]")
{
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(nullptr, 0, nullptr, nullptr, st.get());
	CHECK(st->provider_count == 0);
	CHECK(st->task_count == 0);
	CHECK(st->bad_descriptors == 0);
	CHECK_FALSE(st->dp_lift_declared);
}

#if defined(CONV_SNAP_STUB_GOOD) && defined(CONV_SNAP_STUB_MALFORMED) && defined(CONV_SNAP_STUB_BAD_STATE)

namespace {

xrt_conversion_plugin_entry
stub_entry(const char *id, const char *path, uint32_t order)
{
	xrt_conversion_plugin_entry e;
	std::memset(&e, 0, sizeof(e));
	cpy(e.id, sizeof(e.id), id);
	cpy(e.display_name, sizeof(e.display_name), id);
	cpy(e.binary_path, sizeof(e.binary_path), path);
	cpy(e.source, sizeof(e.source), "test");
	e.probe_order = order;
	e.enabled = true;
	e.status = XRT_CONV_LOAD_NOT_LOADED;
	return e;
}

} // namespace

TEST_CASE("conv headless: a well-formed provider describes, selects, and never warms", "[conv_snapshot][headless]")
{
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1"); // stubs live in the build tree (#952 guard)
	xrt_conversion_plugin_entry e[1] = {stub_entry("stub-good", CONV_SNAP_STUB_GOOD, 250)};
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(e, 1, nullptr, nullptr, st.get());
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);

	CHECK(st->registered == 1);
	CHECK(st->negotiated == 1);
	CHECK(st->bad_descriptors == 0);
	REQUIRE(st->provider_count == 1);
	const xrt_status_conv_provider &p = st->providers[0];
	CHECK(std::string(p.id) == "stub-good");
	CHECK(std::string(p.load) == "NEGOTIATED");
	CHECK(p.abi == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	CHECK(p.cap_count == 2);
	CHECK(p.described);
	CHECK(std::string(p.licence_spdx) == "MIT");

	// depth-video (direct) + views-mono (direct) + views-mono (composed from depth-video).
	bool depth_sel = false, views_direct_sel = false, views_composed_behind = false;
	for (uint32_t i = 0; i < st->task_count; i++) {
		const xrt_status_conv_task &t = st->tasks[i];
		CHECK(t.provider == 0);
		CHECK(t.state == XRT_CONV_STATE_INSTALLED); // the provider's own answer; nothing warmed
		if (t.task == XRT_CONV_TASK_DEPTH_VIDEO && t.selected) {
			depth_sel = true;
		}
		if (t.task == XRT_CONV_TASK_VIEWS_MONO && !t.composed && t.selected) {
			views_direct_sel = true;
			CHECK(t.order == 0);
		}
		if (t.task == XRT_CONV_TASK_VIEWS_MONO && t.composed && !t.selected) {
			views_composed_behind = true;
			CHECK(std::string(t.verdict) == "behind");
		}
	}
	CHECK(depth_sel);
	CHECK(views_direct_sel);
	CHECK(views_composed_behind); // a direct capability before a composed pipeline

	// The binary was unloaded again.
	CHECK(e[0].module == nullptr);
}

TEST_CASE("conv headless: a malformed descriptor is counted (selftest exit 13)", "[conv_snapshot][headless][selftest]")
{
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	xrt_conversion_plugin_entry e[2] = {stub_entry("stub-good", CONV_SNAP_STUB_GOOD, 100),
	                                    stub_entry("stub-malformed", CONV_SNAP_STUB_MALFORMED, 200)};
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(e, 2, nullptr, nullptr, st.get());
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);

	CHECK(st->negotiated == 2);
	CHECK(st->bad_descriptors == 1);
	CHECK(std::string(st->bad_note).find("stub-malformed") != std::string::npos);
	CHECK(std::string(st->bad_note).find("EXPLICIT_VIEWPOINTS") != std::string::npos);
	REQUIRE(st->provider_count == 2);
	CHECK(st->providers[1].bad_descriptors == 1);
	CHECK(st->providers[1].cap_count == 0); // the malformed row was dropped

#ifdef TESTS_HAVE_CLI_QUERY
	// The selftest verdict: PASS -> CLI_SELFTEST_BAD_CONVERSION (exit 13).
	STATIC_REQUIRE(CLI_SELFTEST_BAD_CONVERSION == 13);
	auto r = std::make_unique<cli_query_result>();
	std::memset(r.get(), 0, sizeof(*r));
	r->result_code = CLI_SELFTEST_PASS;
	cli_query_apply_conversion(r.get(), st.get());
	CHECK(r->result_code == CLI_SELFTEST_BAD_CONVERSION);
	CHECK((int)r->result_code == 13);
	CHECK(std::string(r->conv_note).find("MALFORMED") != std::string::npos);

	// An earlier failure keeps its own code.
	r->result_code = CLI_SELFTEST_BAD_INFO;
	cli_query_apply_conversion(r.get(), st.get());
	CHECK(r->result_code == CLI_SELFTEST_BAD_INFO);

	// The JSON carries the check and the object CI reads.
	r->result_code = CLI_SELFTEST_PASS;
	cli_query_apply_conversion(r.get(), st.get());
	cJSON *root = cli_query_selftest_to_cjson(r.get());
	cJSON *obj = cJSON_GetObjectItem(root, "conversion_providers");
	REQUIRE(obj != nullptr);
	CHECK(cJSON_GetObjectItem(obj, "count")->valuedouble == 2.0);
	CHECK(cJSON_GetObjectItem(obj, "bad_descriptors")->valuedouble == 1.0);
	bool found = false;
	cJSON *checks = cJSON_GetObjectItem(root, "checks");
	for (int i = 0; i < cJSON_GetArraySize(checks); i++) {
		cJSON *c = cJSON_GetArrayItem(checks, i);
		if (std::string(cJSON_GetObjectItem(c, "name")->valuestring) == "conversion_providers") {
			found = true;
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(c, "ok")));
		}
	}
	CHECK(found);
	CHECK(cJSON_GetObjectItem(root, "result_code")->valuedouble == 13.0);
	cJSON_Delete(root);

	// Absence never fails.
	auto none = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(nullptr, 0, nullptr, nullptr, none.get());
	r->result_code = CLI_SELFTEST_PASS;
	cli_query_apply_conversion(r.get(), none.get());
	CHECK(r->result_code == CLI_SELFTEST_PASS);
#endif
}

TEST_CASE("conv headless: an out-of-range get_state is malformed too", "[conv_snapshot][headless][selftest]")
{
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	xrt_conversion_plugin_entry e[1] = {stub_entry("stub-bad-state", CONV_SNAP_STUB_BAD_STATE, 100)};
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(e, 1, nullptr, nullptr, st.get());
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	CHECK(st->bad_descriptors == 1);
	CHECK(std::string(st->bad_note).find("out of range") != std::string::npos);
}

TEST_CASE("conv headless: a per-user Enabled = 0 override disables without loading", "[conv_snapshot][headless]")
{
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	xrt_conversion_plugin_entry e[1] = {stub_entry("stub-good", CONV_SNAP_STUB_GOOD, 100)};
	auto pol = std::make_unique<target_conv_policy>();
	std::memset(pol.get(), 0, sizeof(*pol));
	pol->override_count = 1;
	cpy(pol->overrides[0].id, sizeof(pol->overrides[0].id), "stub-good");
	pol->overrides[0].enabled = false;
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(e, 1, nullptr, pol.get(), st.get());
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	CHECK(st->negotiated == 0);
	REQUIRE(st->provider_count == 1);
	CHECK_FALSE(st->providers[0].enabled);
	CHECK(std::string(st->providers[0].load) == "DISABLED");

	// Policy\Lock ignores the per-user override.
	pol->policy_lock = true;
	xrt_conversion_plugin_entry e2[1] = {stub_entry("stub-good", CONV_SNAP_STUB_GOOD, 100)};
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	target_conv_status_build_from_entries(e2, 1, nullptr, pol.get(), st.get());
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	CHECK(st->negotiated == 1);
	CHECK(st->providers[0].enabled);
}

#endif

#if defined(CONV_SNAP_SIM_STATIC)
// The shipped sim provider, linked in (sim_conversion_static) -- its DLL needs
// the runtime's own DLLs next to it, which only _package provides.
extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {
//! A pre-negotiated sim entry, through the headless host (as the loader would).
xrt_conversion_plugin_entry
sim_entry()
{
	xrt_conversion_plugin_entry e;
	std::memset(&e, 0, sizeof(e));
	cpy(e.id, sizeof(e.id), "sim-conversion");
	cpy(e.display_name, sizeof(e.display_name), "DisplayXR Sim Conversion");
	e.probe_order = 250;
	e.enabled = true;
	REQUIRE(target_conversion_plugin_negotiate_with(&e, xrtConversionPluginNegotiate,
	                                                target_conv_status_headless_host()));
	return e;
}
} // namespace

TEST_CASE("conv headless: the shipped sim provider describes cleanly (the CI selftest gate)", "[conv_snapshot][headless]")
{
	// What CI's `selftest --json` sees once sim-conversion is registered with
	// Settings ENABLE = 1: a negotiated provider whose every descriptor passes
	// validation. A failure here is a CI exit 13 waiting to happen.
	set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "1"); // the headless host's env override
	xrt_conversion_plugin_entry e = sim_entry();
	auto st = std::make_unique<target_conv_status>();
	target_conv_status_build_from_entries(&e, 1, nullptr, nullptr, st.get());
	set_env("DXR_CONV_SIM_CONVERSION_ENABLE", nullptr);

	INFO("bad_note: " << st->bad_note);
	CHECK(st->negotiated == 1);
	CHECK(st->bad_descriptors == 0);
	REQUIRE(st->provider_count == 1);
	CHECK(std::string(st->providers[0].load) == "NEGOTIATED");
	CHECK(st->providers[0].fallback);
	CHECK(st->providers[0].cap_count > 0);
	CHECK(st->task_count > 0);

	// The registry Settings value reaches it too (CI registers ENABLE that way).
	xrt_conversion_plugin_entry e2 = sim_entry();
	e2.setting_count = 1;
	cpy(e2.settings[0].key, sizeof(e2.settings[0].key), "ENABLE");
	cpy(e2.settings[0].value, sizeof(e2.settings[0].value), "1");
	target_conv_status_build_from_entries(&e2, 1, nullptr, nullptr, st.get());
	CHECK(st->bad_descriptors == 0);
	REQUIRE(st->provider_count == 1);
	CHECK(st->providers[0].cap_count > 0);

	// Without ENABLE the provider declines its probe cleanly: listed, never failed.
	xrt_conversion_plugin_entry e3 = sim_entry();
	target_conv_status_build_from_entries(&e3, 1, nullptr, nullptr, st.get());
	CHECK(st->bad_descriptors == 0);
	REQUIRE(st->provider_count == 1);
	CHECK(std::string(st->providers[0].load) == "PROBE_DECLINED");
	CHECK(st->providers[0].cap_count == 0);
}
#endif
