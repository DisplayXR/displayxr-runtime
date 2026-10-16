// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-057 S1 + S2: the generic remote conversion provider against a
 *         local contract-v1 test endpoint (tests/conv_remote_test_server.py),
 *         plus the pack url / auth parsing, the camera-block writer and the
 *         off-box consent store.
 *
 * The provider is linked statically (remote_conversion_static) and driven
 * through its negotiate entry with a fake host (settings map + consent flag).
 * Tokens come from a test credential reader (u_credential_set_reader_for_tests):
 * no real keychain, no real endpoint, no network beyond 127.0.0.1. The
 * endpoint tests SKIP when no Python 3 was found at configure time.
 */

#include "xrt/xrt_conversion_plugin.h"

#include "os/os_time.h"
#include "util/u_camera_consent.h"
#include "util/u_credential.h"

#include "conversion/conv_camera_block.h"
#include "conversion/conv_host_wire.h"
#include "conversion/conv_model_store.h"
#include "remote_conv_http.h"

#include <cjson/cJSON.h>

#include "catch_amalgamated.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <process.h>
#else
#include <unistd.h>
#endif

extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {

/*
 * Fake host.
 */

std::map<std::string, std::string> g_settings;
bool g_consent = false;

bool
host_get_setting(const char *key, char *buf, uint32_t size)
{
	auto it = g_settings.find(key);
	if (it == g_settings.end()) {
		buf[0] = 0;
		return false;
	}
	snprintf(buf, size, "%s", it->second.c_str());
	return true;
}

bool
host_get_consent(uint32_t kind)
{
	return kind == XRT_CONV_CONSENT_OFFBOX && g_consent;
}

void
host_notify(uint32_t)
{}

struct xrt_conversion_plugin_host_iface g_host_iface;

u_credential_result
test_reader(const char *name, char *out, size_t size)
{
	if (strcmp(name, "dxr-test-cred") == 0) {
		snprintf(out, size, "test-token");
		return U_CREDENTIAL_OK;
	}
	return U_CREDENTIAL_NOT_FOUND;
}

struct provider
{
	xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;

	~provider()
	{
		if (inst != nullptr) {
			iface->destroy(inst);
		}
	}
};

//! Negotiate + probe with the current g_settings / g_consent.
bool
make_provider(provider &p)
{
	memset(&g_host_iface, 0, sizeof(g_host_iface));
	g_host_iface.struct_size = sizeof(g_host_iface);
	g_host_iface.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	g_host_iface.get_setting = host_get_setting;
	g_host_iface.notify_changed = host_notify;
	g_host_iface.get_consent = host_get_consent;
	u_credential_set_reader_for_tests(test_reader);
	uint32_t api = 0;
	if (xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &g_host_iface, &p.iface, &api) !=
	    XRT_SUCCESS) {
		return false;
	}
	return p.iface->probe(&p.inst) == XRT_SUCCESS && p.inst != nullptr;
}

xrt_conv_state
state_of(provider &p, uint32_t cap)
{
	xrt_conv_state st{};
	st.struct_size = sizeof(st);
	REQUIRE(p.iface->get_state(p.inst, cap, &st) == XRT_SUCCESS);
	return st;
}

xrt_conv_capability
cap_of(provider &p, uint32_t index)
{
	xrt_conv_capability c{};
	c.struct_size = sizeof(c);
	REQUIRE(p.iface->describe_cap(p.inst, index, &c, nullptr) == XRT_SUCCESS);
	return c;
}


/*
 * The test endpoint (a child process).
 */

struct server
{
	FILE *pipe = nullptr;
	int port = 0;
	std::string base;
};

bool
have_python()
{
#ifdef DXR_TEST_PYTHON
	return true;
#else
	return false;
#endif
}

bool
http_get(const std::string &url, std::string &body, long &status)
{
	rc_http_request rq{};
	rq.method = "GET";
	rq.url = url.c_str();
	rq.timeout_ms = 5000;
	rc_http_response resp;
	rc_http_perform(&rq, &resp);
	status = resp.status;
	body.assign(resp.body != nullptr ? (const char *)resp.body : "", resp.body_size);
	const bool ok = resp.error == RC_HTTP_OK;
	rc_http_response_free(&resp);
	return ok;
}

bool
start_server(server &s, const std::string &args)
{
#ifdef DXR_TEST_PYTHON
	std::string cmd = std::string("\"") + DXR_TEST_PYTHON + "\" \"" + DXR_TEST_SERVER_PY + "\" " + args;
#ifdef _WIN32
	cmd = "\"" + cmd + "\""; // cmd.exe /c strips one pair of quotes
#endif
	s.pipe = popen(cmd.c_str(), "r");
	if (s.pipe == nullptr) {
		return false;
	}
	char line[64] = {0};
	if (fgets(line, sizeof(line), s.pipe) == nullptr || sscanf(line, "PORT %d", &s.port) != 1) {
		pclose(s.pipe);
		s.pipe = nullptr;
		return false;
	}
	s.base = "http://127.0.0.1:" + std::to_string(s.port);
	return true;
#else
	(void)s;
	(void)args;
	return false;
#endif
}

void
stop_server(server &s)
{
	if (s.pipe == nullptr) {
		return;
	}
	std::string body;
	long st = 0;
	(void)http_get(s.base + "/__quit", body, st);
	pclose(s.pipe);
	s.pipe = nullptr;
}

struct server_guard
{
	server &s;
	~server_guard()
	{
		stop_server(s);
	}
};

cJSON *
stats(server &s)
{
	std::string body;
	long st = 0;
	REQUIRE(http_get(s.base + "/__stats", body, st));
	REQUIRE(st == 200);
	return cJSON_Parse(body.c_str());
}

int
stat_jobs(server &s)
{
	cJSON *o = stats(s);
	const int n = cJSON_GetObjectItem(o, "jobs")->valueint;
	cJSON_Delete(o);
	return n;
}


/*
 * A job.
 */

struct blob_sink
{
	std::vector<uint8_t> bytes;
	uint32_t format = 0;
};

bool
sink_begin(void *ctx, uint32_t fmt, uint64_t)
{
	auto *b = (blob_sink *)ctx;
	b->format = fmt;
	b->bytes.clear();
	return true;
}

bool
sink_write(void *ctx, const void *data, uint64_t n)
{
	auto *b = (blob_sink *)ctx;
	b->bytes.insert(b->bytes.end(), (const uint8_t *)data, (const uint8_t *)data + n);
	return true;
}

int64_t g_cancel_at_ns = 0;

bool
cancel_after(void *)
{
	return g_cancel_at_ns != 0 && os_monotonic_get_ns() >= g_cancel_at_ns;
}

struct job_result
{
	xrt_result_t r = XRT_SUCCESS;
	xrt_conv_job_out out{};
	xrt_conv_splat_result sr{};
	blob_sink blob;
};

void
run_job(
    provider &p, uint32_t cap, job_result &jr, uint32_t job_flags = 0, float baseline = 0.0f, bool two_images = false)
{
	static std::vector<uint8_t> px(64 * 48 * 4, 0);
	for (size_t i = 0; i < px.size(); i++) {
		px[i] = (uint8_t)(i * 7);
	}
	xrt_conv_job_in in{};
	in.struct_size = sizeof(in);
	in.input_kind = two_images ? XRT_CONV_INPUT_STEREO_PAIR : XRT_CONV_INPUT_MONO;
	in.image_count = two_images ? 2 : 1;
	for (uint32_t i = 0; i < in.image_count; i++) {
		in.images[i].memory_kind = XRT_CONV_MEM_CPU;
		in.images[i].cpu_ptr = px.data();
		in.images[i].byte_size = px.size();
		in.images[i].width = 64;
		in.images[i].height = 48;
		in.images[i].row_stride = 64 * 4;
		in.images[i].format = XRT_CONV_FMT_RGBA8_SRGB;
		in.image_role[i] = i == 0 ? XRT_CONV_IMG_COLOR : XRT_CONV_IMG_COLOR_RIGHT;
	}
	in.job_flags = job_flags;
	xrt_conv_params params{};
	params.struct_size = sizeof(params);
	params.stereo.baseline_m = baseline;
	xrt_conv_blob_sink sink{};
	sink.struct_size = sizeof(sink);
	sink.role = XRT_CONV_OUT_SPLAT_BLOB;
	sink.ctx = &jr.blob;
	sink.begin = sink_begin;
	sink.write = sink_write;
	xrt_conv_job_control ctl{};
	ctl.struct_size = sizeof(ctl);
	ctl.sink_count = 1;
	ctl.sinks = &sink;
	ctl.is_cancelled = cancel_after;
	jr.out.struct_size = sizeof(jr.out);
	jr.sr.struct_size = sizeof(jr.sr);
	jr.out.splat_result = &jr.sr;
	jr.r = p.iface->job_execute(p.inst, cap, &in, &params, &ctl, &jr.out);
}

void
base_settings(const std::string &endpoints)
{
	g_settings.clear();
	g_settings["ENABLE"] = "1";
	g_settings["ENDPOINTS"] = endpoints;
	g_settings["CREDENTIAL"] = "dxr-test-cred";
	g_settings["TIMEOUT_S"] = "10";
	g_consent = true;
	g_cancel_at_ns = 0;
}

} // namespace


/*
 *
 * Provider gates that need no endpoint.
 *
 */

TEST_CASE("remote provider: off unless ENABLE, consent-gated, endpoint list validated", "[remote_conversion]")
{
	g_settings.clear();
	{
		provider p;
		CHECK_FALSE(make_provider(p)); // ENABLE absent: declines like the sims
	}
	base_settings("http://127.0.0.1:9");
	g_consent = false;
	{
		provider p;
		REQUIRE(make_provider(p));
		const xrt_conv_state st = state_of(p, 1);
		CHECK(st.state == XRT_CONV_STATE_DISABLED);
		CHECK(strstr(st.detail, "consent required") != nullptr);
		CHECK(p.iface->warm(p.inst, 1, nullptr, nullptr) == XRT_ERROR_NOT_AUTHORIZED);
		job_result jr;
		run_job(p, 1, jr);
		CHECK(jr.r == XRT_ERROR_NOT_AUTHORIZED);
		CHECK(jr.blob.bytes.empty());
		const xrt_conv_capability c = cap_of(p, 0);
		CHECK(c.task == XRT_CONV_TASK_SPLATS_MONO);
		CHECK((c.flags & XRT_CONV_CAP_REMOTE) != 0);
		CHECK(c.lanes == XRT_CONV_LANE_JOB);
		CHECK(c.licence_class == XRT_CONV_LICENCE_UNKNOWN); // until a describe says otherwise
	}
	base_settings("http://example.com");
	{
		provider p;
		REQUIRE(make_provider(p));
		const xrt_conv_state st = state_of(p, 1);
		CHECK(st.state == XRT_CONV_STATE_DISABLED);
		CHECK(strstr(st.detail, "https") != nullptr); // a token never crosses a network in clear
	}
	base_settings("");
	{
		provider p;
		REQUIRE(make_provider(p));
		CHECK(state_of(p, 2).state == XRT_CONV_STATE_DISABLED);
	}
}


/*
 *
 * Against the test endpoint.
 *
 */

TEST_CASE("remote provider: mono success carries the blob, the facts and the build stamp", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3: the contract-v1 test endpoint cannot run");
	}
	server s;
	REQUIRE(start_server(s, "--token test-token"));
	server_guard g{s};
	base_settings(s.base);
	provider p;
	REQUIRE(make_provider(p));
	CHECK(state_of(p, 1).state == XRT_CONV_STATE_INSTALLED);
	REQUIRE(p.iface->warm(p.inst, 1, nullptr, nullptr) == XRT_SUCCESS);
	CHECK(state_of(p, 1).state == XRT_CONV_STATE_READY);
	const xrt_conv_capability c = cap_of(p, 0);
	CHECK(c.licence_class == XRT_CONV_LICENCE_COMMERCIAL);
	CHECK(c.commercial_ok == 1);
	CHECK(c.max_input_edge == 1536);

	job_result jr;
	run_job(p, 1, jr);
	REQUIRE(jr.r == XRT_SUCCESS);
	REQUIRE(jr.blob.bytes.size() > 4);
	CHECK(memcmp(jr.blob.bytes.data(), "PK", 2) == 0);
	CHECK(jr.blob.format == XRT_CONV_BLOB_SOG);
	CHECK(jr.out.written == XRT_CONV_OUT_SPLAT_BLOB);
	CHECK(jr.out.splat_info.grid_width == 640);
	CHECK(jr.out.splat_info.fx == Catch::Approx(1000.0f));
	CHECK(jr.out.splat_info.metric == 0); // no baseline given: never metric
	CHECK(jr.out.splat_info.baseline_m == 0.0f);
	CHECK(jr.out.splat_info.splat_count == 1179648u);
	CHECK((jr.sr.valid & XRT_CONV_SPLAT_VALID_BUILD) != 0);
	CHECK(std::string(jr.sr.build_provider) == "test-build-1");
	std::string sha;
	for (int i = 0; i < 32; i++) {
		sha += "ab";
	}
	CHECK(std::string(jr.sr.build_model_sha256) == sha);
	CHECK(jr.sr.host_kind == XRT_CONV_HOST_KIND_REMOTE);
	CHECK(strstr(jr.sr.build_host_id, "host-a") != nullptr);
	CHECK(jr.sr.refusal_code == XRT_CONV_REFUSAL_NONE);
	CHECK(jr.sr.layer_count == 2);
	CHECK(jr.sr.layer_flags[0] == XRT_CONV_LAYER_VISIBLE);
	CHECK(jr.sr.layer_flags[1] == XRT_CONV_LAYER_INPAINTED);
	CHECK((jr.sr.valid & XRT_CONV_SPLAT_VALID_FOCUS) != 0);
	CHECK(jr.sr.focus_point[2] == Catch::Approx(1.9f));
	CHECK(jr.sr.focus_source == XRT_CONV_FOCUS_SOURCE_NEAREST_CLUMP);

	cJSON *st = stats(s);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(st, "last_auth")));
	const cJSON *lp = cJSON_GetObjectItem(st, "last_params");
	CHECK(std::string(cJSON_GetObjectItem(lp, "task")->valuestring) == "splats-mono");
	CHECK(std::string(cJSON_GetObjectItem(lp, "format")->valuestring) == "sog");
	CHECK(cJSON_GetObjectItem(lp, "baseline_m") == nullptr); // unknown = omitted, never defaulted
	cJSON_Delete(st);
}

TEST_CASE("remote provider: stereo is metric only with a baseline; the pair is declared, never guessed",
          "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server s;
	REQUIRE(start_server(s, ""));
	server_guard g{s};
	base_settings(s.base);
	g_settings.erase("CREDENTIAL"); // an open endpoint: no Authorization header
	provider p;
	REQUIRE(make_provider(p));
	REQUIRE(p.iface->warm(p.inst, 2, nullptr, nullptr) == XRT_SUCCESS);

	job_result undeclared;
	run_job(p, 2, undeclared, 0, 0.065f, true);
	CHECK(undeclared.r == XRT_ERROR_INPUT_UNSUPPORTED);
	CHECK(undeclared.sr.refusal_code == XRT_CONV_REFUSAL_UNRECTIFIED_PAIR);
	job_result raw;
	run_job(p, 2, raw, XRT_CONV_JOB_PAIR_RAW, 0.065f, true); // the endpoint says rectified_required
	CHECK(raw.sr.refusal_code == XRT_CONV_REFUSAL_UNRECTIFIED_PAIR);
	CHECK(stat_jobs(s) == 0); // both refused before any upload

	job_result jr;
	run_job(p, 2, jr, XRT_CONV_JOB_PAIR_RECTIFIED, 0.065f, true);
	REQUIRE(jr.r == XRT_SUCCESS);
	CHECK(jr.out.splat_info.metric == 1);
	CHECK(jr.out.splat_info.baseline_m == Catch::Approx(0.065f));
	CHECK((jr.sr.valid & XRT_CONV_SPLAT_VALID_METRIC_FIT) != 0);
	CHECK(jr.sr.metric_scale == Catch::Approx(1.25f));
	CHECK(jr.sr.metric_source == XRT_CONV_METRIC_SOURCE_STEREO);
	CHECK(jr.sr.principal_point_shift_px == Catch::Approx(4.0f));
	cJSON *st = stats(s);
	const cJSON *lp = cJSON_GetObjectItem(st, "last_params");
	CHECK(cJSON_GetObjectItem(lp, "baseline_m")->valuedouble == Catch::Approx(0.065));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(lp, "rectified")));
	CHECK_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(st, "last_auth")));
	cJSON_Delete(st);
}

TEST_CASE("remote provider: a refused metric fit still returns the non-metric splat", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server s;
	REQUIRE(start_server(s, "--mode refuse-metric"));
	server_guard g{s};
	base_settings(s.base);
	provider p;
	REQUIRE(make_provider(p));
	job_result jr;
	run_job(p, 2, jr, XRT_CONV_JOB_PAIR_RECTIFIED, 0.065f, true);
	REQUIRE(jr.r == XRT_SUCCESS); // warm happened inside job_execute
	CHECK_FALSE(jr.blob.bytes.empty());
	CHECK(jr.out.splat_info.metric == 0);
	CHECK(jr.sr.refusal_code == XRT_CONV_REFUSAL_METRIC_FIT_REFUSED);
	CHECK(strstr(jr.sr.refusal_reason, "inliers") != nullptr);
}

TEST_CASE("remote provider: 422 is bad input and never retried", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server a, b;
	REQUIRE(start_server(a, "--mode 422 --host-id host-a"));
	server_guard ga{a};
	REQUIRE(start_server(b, "--host-id host-b"));
	server_guard gb{b};
	base_settings(a.base + "," + b.base);
	provider p;
	REQUIRE(make_provider(p));
	job_result jr;
	run_job(p, 1, jr);
	CHECK(jr.r == XRT_ERROR_INPUT_UNSUPPORTED);
	CHECK(jr.sr.refusal_code == XRT_CONV_REFUSAL_UNRECTIFIED_PAIR);
	CHECK(strstr(jr.sr.refusal_reason, "rectified") != nullptr);
	CHECK(stat_jobs(a) == 1);
	CHECK(stat_jobs(b) == 0);
	CHECK(state_of(p, 1).state == XRT_CONV_STATE_READY); // the input's fault, not the provider's
}

TEST_CASE("remote provider: 503 falls through to the next host", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server a, b;
	REQUIRE(start_server(a, "--mode 503 --host-id host-a --build build-a"));
	server_guard ga{a};
	REQUIRE(start_server(b, "--host-id host-b --build build-b"));
	server_guard gb{b};
	base_settings(a.base + " " + b.base);
	provider p;
	REQUIRE(make_provider(p));
	job_result jr;
	run_job(p, 1, jr);
	REQUIRE(jr.r == XRT_SUCCESS);
	CHECK(stat_jobs(a) == 1);
	CHECK(stat_jobs(b) == 1);
	CHECK(strstr(jr.sr.build_host_id, "host-b") != nullptr); // the stamp of the host that ANSWERED
	CHECK(std::string(jr.sr.build_provider) == "build-b");
}

TEST_CASE("remote provider: every host down = REMOTE_ERROR and FAILED(NETWORK, retryable)", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server a;
	REQUIRE(start_server(a, "--mode 503"));
	server_guard ga{a};
	base_settings(a.base + ",http://127.0.0.1:9"); // the second refuses connections
	provider p;
	REQUIRE(make_provider(p));
	job_result jr;
	run_job(p, 1, jr);
	CHECK(jr.r != XRT_SUCCESS);
	CHECK(jr.sr.refusal_code == XRT_CONV_REFUSAL_REMOTE_ERROR);
	const xrt_conv_state st = state_of(p, 1);
	CHECK(st.state == XRT_CONV_STATE_FAILED);
	CHECK(st.failure_reason == XRT_CONV_FAIL_NETWORK);
	CHECK(st.retryable == 1);
}

TEST_CASE("remote provider: a missing or refused credential is FAILED(LICENCE)", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server s;
	REQUIRE(start_server(s, "--token another-token"));
	server_guard g{s};
	base_settings(s.base);
	g_settings["CREDENTIAL"] = "dxr-test-missing";
	{
		provider p;
		REQUIRE(make_provider(p));
		CHECK(p.iface->warm(p.inst, 1, nullptr, nullptr) != XRT_SUCCESS);
		const xrt_conv_state st = state_of(p, 1);
		CHECK(st.state == XRT_CONV_STATE_FAILED);
		CHECK(st.failure_reason == XRT_CONV_FAIL_LICENCE);
		CHECK(strstr(st.detail, "dxr-test-missing") != nullptr);
		cJSON *o = stats(s);
		CHECK(cJSON_GetObjectItem(o, "describe")->valueint == 0); // nothing was sent without the token
		cJSON_Delete(o);
	}
	g_settings["CREDENTIAL"] = "dxr-test-cred"; // present, but the endpoint wants another
	{
		provider p;
		REQUIRE(make_provider(p));
		CHECK(p.iface->warm(p.inst, 1, nullptr, nullptr) != XRT_SUCCESS);
		const xrt_conv_state st = state_of(p, 1);
		CHECK(st.state == XRT_CONV_STATE_FAILED);
		CHECK(st.failure_reason == XRT_CONV_FAIL_LICENCE);
	}
}

TEST_CASE("remote provider: research-only is reported as-is", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server s;
	REQUIRE(start_server(s, "--licence research_only"));
	server_guard g{s};
	base_settings(s.base);
	provider p;
	REQUIRE(make_provider(p));
	REQUIRE(p.iface->warm(p.inst, 1, nullptr, nullptr) == XRT_SUCCESS);
	const xrt_conv_capability c = cap_of(p, 0);
	CHECK(c.licence_class == XRT_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(c.commercial_ok == 0);
}

TEST_CASE("remote provider: cancel closes the connection promptly", "[remote_conversion]")
{
	if (!have_python()) {
		SKIP("no Python 3");
	}
	server s;
	REQUIRE(start_server(s, "--mode slow --delay 8"));
	server_guard g{s};
	base_settings(s.base);
	provider p;
	REQUIRE(make_provider(p));
	REQUIRE(p.iface->warm(p.inst, 1, nullptr, nullptr) == XRT_SUCCESS);
	const int64_t t0 = os_monotonic_get_ns();
	g_cancel_at_ns = t0 + 300ll * 1000 * 1000;
	job_result jr;
	run_job(p, 1, jr);
	const double secs = (double)(os_monotonic_get_ns() - t0) / 1e9;
	CHECK(jr.r != XRT_SUCCESS);
	CHECK(jr.blob.bytes.empty());
	CHECK(secs < 3.0);
	CHECK(state_of(p, 1).state == XRT_CONV_STATE_READY); // a cancel is not a provider failure
	g_cancel_at_ns = 0;
}


/*
 *
 * Camera block: facts -> block, never over an authored block.
 *
 */

namespace {

xrt_conv_splat_info
sample_info(bool metric)
{
	xrt_conv_splat_info si{};
	si.blob_format = XRT_CONV_BLOB_SOG;
	si.coords = XRT_CONV_COORDS_OPENCV;
	si.metric = metric ? 1u : 0u;
	si.fx = si.fy = 1000.0f;
	si.cx = 316.0f;
	si.cy = 240.0f;
	si.grid_width = 640;
	si.grid_height = 480;
	si.baseline_m = metric ? 0.065f : 0.0f;
	si.splat_count = 1179648;
	return si;
}

xrt_conv_splat_result
sample_result()
{
	xrt_conv_splat_result sr{};
	sr.struct_size = sizeof(sr);
	sr.valid = XRT_CONV_SPLAT_VALID_BUILD | XRT_CONV_SPLAT_VALID_METRIC_FIT | XRT_CONV_SPLAT_VALID_FOCUS;
	snprintf(sr.build_provider, sizeof(sr.build_provider), "b1");
	sr.host_kind = XRT_CONV_HOST_KIND_REMOTE;
	sr.metric_scale = 1.25f;
	sr.metric_source = XRT_CONV_METRIC_SOURCE_STEREO;
	sr.metric_inlier_fraction = 0.9f;
	sr.metric_mad_log = 0.05f;
	sr.principal_point_shift_px = 4.0f;
	sr.focus_point[2] = 1.9f;
	sr.focus_subject_m = 1.9f;
	sr.focus_source = XRT_CONV_FOCUS_SOURCE_NEAREST_CLUMP;
	return sr;
}

} // namespace

TEST_CASE("camera block: facts become a v2 block marked source auto; metric facts only when metric",
          "[remote_conversion][camera_block]")
{
	const xrt_conv_splat_result sr = sample_result();
	for (int metric = 0; metric < 2; metric++) {
		const xrt_conv_splat_info si = sample_info(metric != 0);
		cJSON *facts = conv_splat_facts_to_json(&si, &sr);
		char err[128];
		cJSON *b = conv_camera_block_from_facts(facts, err, sizeof(err));
		REQUIRE(b != nullptr);
		CHECK(std::string(cJSON_GetObjectItem(b, "source")->valuestring) == "auto");
		CHECK(std::string(cJSON_GetObjectItem(b, "convention")->valuestring) == "opencv");
		CHECK(std::string(cJSON_GetObjectItem(b, "rig")->valuestring) == "camera");
		const cJSON *in = cJSON_GetObjectItem(b, "intrinsics");
		CHECK(cJSON_GetObjectItem(in, "cx")->valuedouble == Catch::Approx(316.0));
		CHECK(cJSON_GetObjectItem(in, "width")->valueint == 640);
		CHECK((cJSON_GetObjectItem(b, "stereo") != nullptr) == (metric != 0));
		CHECK((cJSON_GetObjectItem(b, "metric") != nullptr) == (metric != 0));
		const cJSON *f = cJSON_GetObjectItem(b, "focus");
		REQUIRE(f != nullptr);
		CHECK(std::string(cJSON_GetObjectItem(f, "source")->valuestring) == "auto");
		CHECK(cJSON_GetObjectItem(b, "dxr") != nullptr);
		cJSON_Delete(b);
		cJSON_Delete(facts);
	}
	// No intrinsics, no block.
	xrt_conv_splat_info none{};
	cJSON *facts = conv_splat_facts_to_json(&none, nullptr);
	char err[128];
	CHECK(conv_camera_block_from_facts(facts, err, sizeof(err)) == nullptr);
	cJSON_Delete(facts);
}

TEST_CASE("camera block: inserted after count; an authored block is never overwritten; auto is replaced",
          "[remote_conversion][camera_block]")
{
	const xrt_conv_splat_info si = sample_info(false);
	const xrt_conv_splat_result sr = sample_result();
	cJSON *facts = conv_splat_facts_to_json(&si, &sr);
	cJSON *block = conv_camera_block_from_facts(facts, nullptr, 0);
	REQUIRE(block != nullptr);
	char *out = nullptr;
	char err[128];

	const char *plain = "{\"version\": 2, \"count\": 5, \"means\": {\"files\": [\"a.webp\"]}}";
	CHECK(conv_camera_block_apply_meta(plain, block, &out, err, sizeof(err)) == CONV_CAMERA_BLOCK_WRITTEN);
	REQUIRE(out != nullptr);
	cJSON *m = cJSON_Parse(out);
	const cJSON *k = m->child;
	CHECK(std::string(k->string) == "version");
	CHECK(std::string(k->next->string) == "count");
	CHECK(std::string(k->next->next->string) == "camera");
	CHECK(std::string(k->next->next->next->string) == "means");
	cJSON_Delete(m);
	free(out);

	const char *authored = "{\"count\": 5, \"camera\": {\"rig\": \"display\", \"focus\": {\"point\": [0,0,3]}}}";
	CHECK(conv_camera_block_apply_meta(authored, block, &out, err, sizeof(err)) == CONV_CAMERA_BLOCK_KEPT_AUTHORED);
	CHECK(out == nullptr);
	const char *authored_manual = "{\"count\": 5, \"camera\": {\"source\": \"manual\", \"rig\": \"camera\"}}";
	CHECK(conv_camera_block_apply_meta(authored_manual, block, &out, err, sizeof(err)) ==
	      CONV_CAMERA_BLOCK_KEPT_AUTHORED);

	const char *auto_old = "{\"count\": 5, \"camera\": {\"source\": \"auto\", \"rig\": \"camera\"}, \"x\": 1}";
	CHECK(conv_camera_block_apply_meta(auto_old, block, &out, err, sizeof(err)) == CONV_CAMERA_BLOCK_REPLACED_AUTO);
	REQUIRE(out != nullptr);
	m = cJSON_Parse(out);
	CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "camera"), "intrinsics") != nullptr);
	CHECK(cJSON_GetObjectItem(m, "x") != nullptr);
	cJSON_Delete(m);
	free(out);
	cJSON_Delete(block);
	cJSON_Delete(facts);
}

TEST_CASE("camera block: a bundled SOG is rewritten entry by entry; authored stays byte-identical",
          "[remote_conversion][camera_block]")
{
	if (!have_python()) {
		SKIP("no Python 3: the SOG fixture comes from the test endpoint");
	}
	server s;
	REQUIRE(start_server(s, ""));
	server_guard g{s};
	base_settings(s.base);
	g_settings.erase("CREDENTIAL");
	provider p;
	REQUIRE(make_provider(p));
	job_result jr;
	run_job(p, 1, jr);
	REQUIRE(jr.r == XRT_SUCCESS);

	char path[512];
#ifdef _WIN32
	snprintf(path, sizeof(path), "%s\\dxr_cb_test_%d.sog", getenv("TEMP") != nullptr ? getenv("TEMP") : ".",
	         (int)_getpid());
#else
	snprintf(path, sizeof(path), "/tmp/dxr_cb_test_%d.sog", (int)getpid());
#endif
	FILE *f = fopen(path, "wb");
	REQUIRE(f != nullptr);
	fwrite(jr.blob.bytes.data(), 1, jr.blob.bytes.size(), f);
	fclose(f);

	cJSON *facts = conv_splat_facts_to_json(&jr.out.splat_info, &jr.sr);
	cJSON *block = conv_camera_block_from_facts(facts, nullptr, 0);
	REQUIRE(block != nullptr);
	char err[256];
	REQUIRE(conv_camera_block_write(path, block, err, sizeof(err)) == CONV_CAMERA_BLOCK_WRITTEN);
	char *meta = conv_sog_read_meta(path, err, sizeof(err));
	REQUIRE(meta != nullptr);
	cJSON *m = cJSON_Parse(meta);
	const cJSON *cam = cJSON_GetObjectItem(m, "camera");
	REQUIRE(cam != nullptr);
	CHECK(std::string(cJSON_GetObjectItem(cam, "source")->valuestring) == "auto");
	CHECK(cJSON_GetObjectItem(m, "means") != nullptr); // the rest survived
	cJSON_Delete(m);
	free(meta);

	// Second auto write replaces our own block.
	CHECK(conv_camera_block_write(path, block, err, sizeof(err)) == CONV_CAMERA_BLOCK_REPLACED_AUTO);

	// An author edits the block (source manual); a re-run never touches the file.
	const char *edited_meta =
	    "{\"version\": 2, \"count\": 1, \"camera\": {\"source\": \"manual\", \"rig\": "
	    "\"display\"}}";
	char mpath[600];
	snprintf(mpath, sizeof(mpath), "%s.meta.json", path);
	f = fopen(mpath, "wb");
	REQUIRE(f != nullptr);
	fputs(edited_meta, f);
	fclose(f);
	CHECK(conv_camera_block_write(mpath, block, err, sizeof(err)) == CONV_CAMERA_BLOCK_KEPT_AUTHORED);
	char *after = conv_sog_read_meta(mpath, err, sizeof(err));
	REQUIRE(after != nullptr);
	CHECK(std::string(after) == edited_meta);
	free(after);
	remove(mpath);
	remove(path);
	cJSON_Delete(block);
	cJSON_Delete(facts);
}


/*
 *
 * Pack url + auth (ADR-057 §5).
 *
 */

namespace {

std::string
pack_json(const std::string &model_extra, const std::string &files)
{
	return std::string(
	           "{\"schema\": 2, \"pack\": \"p\", \"blobBaseUrl\": \"https://example.invalid/blobs\", "
	           "\"models\": [{\"name\": \"m\", \"version\": \"1\", \"task\": \"splats-mono\", "
	           "\"license\": {\"spdx\": \"LicenseRef-x\", \"commercial\": false}") +
	       model_extra + ", \"variants\": [" + files + "]}]}";
}

const std::string k_sha_a(64, 'a');
const std::string k_sha_b(64, 'b');

std::string
variant(const std::string &id, const std::string &model_sha, const std::string &extra_file = "")
{
	return "{\"id\": \"" + id + "\", \"files\": [{\"role\": \"model\", \"format\": \"pt\", \"sha256\": \"" +
	       model_sha + "\", \"size\": 10}" + extra_file + "]}";
}

} // namespace

TEST_CASE("pack schema 2: a per-model url overrides blobBaseUrl for its model file; auth names a credential",
          "[remote_conversion][pack]")
{
	conv_pack *p = new conv_pack;
	char err[256];
	const std::string json = pack_json(
	    ", \"url\": \"https://example.invalid/resolve/main/model.pt\", \"auth\": {\"kind\": \"huggingface\", "
	    "\"credential\": \"hf-token\"}",
	    variant("a", k_sha_a,
	            ", {\"role\": \"data\", \"format\": \"bin\", \"sha256\": \"" + k_sha_b + "\", \"size\": 5}"));
	REQUIRE(conv_pack_parse(json.c_str(), "/tmp", p, err, sizeof(err)));
	const conv_pack_model &m = p->models[0];
	CHECK(m.auth.kind == CONV_PACK_AUTH_HUGGINGFACE);
	CHECK(std::string(m.auth.credential) == "hf-token");
	CHECK(std::string(m.url_sha256) == k_sha_a);
	char url[512];
	conv_pack_file_url(p, &m, &m.variants[0].files[0], url, sizeof(url));
	CHECK(std::string(url) == "https://example.invalid/resolve/main/model.pt");
	conv_pack_file_url(p, &m, &m.variants[0].files[1], url, sizeof(url));
	CHECK(std::string(url) == "https://example.invalid/blobs/" + k_sha_b + ".bin"); // sha256 + size still identify

	// Two variants with different model files make one url ambiguous.
	const std::string amb = pack_json(", \"url\": \"https://example.invalid/m.pt\"",
	                                  variant("a", k_sha_a) + ", " + variant("b", k_sha_b));
	CHECK_FALSE(conv_pack_parse(amb.c_str(), "/tmp", p, err, sizeof(err)));
	CHECK(strstr(err, "ambiguous") != nullptr);
	// An unknown auth kind, an empty credential, and auth over plain http are refused.
	const std::string bad_kind =
	    pack_json(", \"auth\": {\"kind\": \"basic\", \"credential\": \"x\"}", variant("a", k_sha_a));
	CHECK_FALSE(conv_pack_parse(bad_kind.c_str(), "/tmp", p, err, sizeof(err)));
	const std::string no_cred =
	    pack_json(", \"auth\": {\"kind\": \"bearer\", \"credential\": \"\"}", variant("a", k_sha_a));
	CHECK_FALSE(conv_pack_parse(no_cred.c_str(), "/tmp", p, err, sizeof(err)));
	const std::string http_auth = pack_json(
	    ", \"url\": \"http://example.invalid/m.pt\", \"auth\": {\"kind\": \"bearer\", \"credential\": \"x\"}",
	    variant("a", k_sha_a));
	CHECK_FALSE(conv_pack_parse(http_auth.c_str(), "/tmp", p, err, sizeof(err)));
	// No url: the schema-1 naming, unchanged.
	const std::string plain = pack_json("", variant("a", k_sha_a));
	REQUIRE(conv_pack_parse(plain.c_str(), "/tmp", p, err, sizeof(err)));
	conv_pack_file_url(p, &p->models[0], &p->models[0].variants[0].files[0], url, sizeof(url));
	CHECK(std::string(url) == "https://example.invalid/blobs/" + k_sha_a + ".pt");
	CHECK(p->models[0].auth.kind == CONV_PACK_AUTH_NONE);
	delete p;
}

TEST_CASE("satellite wire: the splat result crosses as a byte-equal twin, within the receiver's extent",
          "[remote_conversion][wire]")
{
	xrt_conv_splat_result src = sample_result();
	snprintf(src.refusal_reason, sizeof(src.refusal_reason), "why");
	src.refusal_code = XRT_CONV_REFUSAL_METRIC_FIT_REFUSED;
	conv_host_wire_job_done jd;
	conv_host_wire_init(&jd, sizeof(jd));
	conv_host_wire_pack_splat_result(&src, &jd.splat_result);
	CHECK(jd.splat_result.struct_size == sizeof(src));
	xrt_conv_splat_result dst{};
	dst.struct_size = sizeof(dst);
	REQUIRE(conv_host_wire_unpack_splat_result(&jd.splat_result, &dst));
	CHECK(memcmp(&src, &dst, sizeof(src)) == 0);
	// An older receiver (smaller struct) gets its prefix only.
	struct
	{
		xrt_conv_splat_result r;
		uint8_t guard[8];
	} small{};
	small.r.struct_size = 16;
	memset(small.guard, 0x5a, sizeof(small.guard));
	REQUIRE(conv_host_wire_unpack_splat_result(&jd.splat_result, &small.r));
	CHECK(small.r.struct_size == 16u);
	CHECK(small.r.valid == src.valid);
	CHECK(small.r.build_provider[0] == 'b');
	CHECK(small.r.build_provider[8] == 0); // past 16 bytes: untouched
	// A host that predates ADR-057 sends none (zero-extended on receive).
	conv_host_wire_splat_result none{};
	CHECK_FALSE(conv_host_wire_unpack_splat_result(&none, &dst));
}

TEST_CASE("credential reader: names are validated; the test store stands in for the OS one",
          "[remote_conversion][credential]")
{
	CHECK(u_credential_name_valid("dxr-test-cred"));
	CHECK_FALSE(u_credential_name_valid(""));
	CHECK_FALSE(u_credential_name_valid("bad\nname"));
	u_credential_set_reader_for_tests(test_reader);
	char tok[64];
	char err[128];
	CHECK(u_credential_read("dxr-test-cred", tok, sizeof(tok), err, sizeof(err)) == U_CREDENTIAL_OK);
	CHECK(std::string(tok) == "test-token");
	CHECK(u_credential_read("nope", tok, sizeof(tok), err, sizeof(err)) == U_CREDENTIAL_NOT_FOUND);
	CHECK(tok[0] == 0);
}


/*
 *
 * Off-box consent store (ADR-057 §2, beside ADR-043's).
 *
 */

TEST_CASE("off-box consent: grant, status, revoke; ids validated", "[remote_conversion][consent]")
{
#ifndef _WIN32
	// The POSIX store lives in the user config dir: point it at a scratch one.
	char dir[] = "/tmp/dxr_consent_XXXXXX";
	REQUIRE(mkdtemp(dir) != nullptr);
	const char *old = getenv("XDG_CONFIG_HOME");
	std::string saved = old != nullptr ? old : "";
	setenv("XDG_CONFIG_HOME", dir, 1);
	const std::string id = "dxr-test-provider";
#else
	const std::string id = "dxr-test-provider-" + std::to_string(_getpid());
#endif
	CHECK_FALSE(u_offbox_consent_granted(id.c_str()));
	REQUIRE(u_offbox_consent_set(id.c_str(), true));
	CHECK(u_offbox_consent_granted(id.c_str()));
	CHECK_FALSE(u_offbox_consent_granted("another-provider"));
	REQUIRE(u_offbox_consent_set(id.c_str(), false));
	CHECK_FALSE(u_offbox_consent_granted(id.c_str()));
	CHECK_FALSE(u_offbox_consent_set("bad id", true));
	CHECK_FALSE(u_offbox_consent_set("", true));
#ifndef _WIN32
	if (old != nullptr) {
		setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
	} else {
		unsetenv("XDG_CONFIG_HOME");
	}
#endif
}
