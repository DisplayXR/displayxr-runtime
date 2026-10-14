// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR-SimConversion (ADR-053, discovery spec §6.1): the
 *         hardware-free reference conversion provider, driven through its own
 *         exported entry point, linked statically (sim_conversion_static) —
 *         no dlopen, no registry, no GPU (CPU images only).
 *
 * What must hold:
 *  - negotiate + describe_cap are well-formed per the selftest rules (spec
 *    §7.2): ABI 1, known task, lanes ≠ 0, min ≤ max ≤ 8, VIEWS_* ⇒
 *    EXPLICIT_VIEWPOINTS, known licence class, unique cap_id — and every task
 *    of the v1 vocabulary is present, each importing CPU memory;
 *  - probe declines unless setting ENABLE = 1;
 *  - a scripted readiness sequence (Settings SCRIPT / WARM_MS / STATE_<cap>)
 *    replays as written: INSTALLED → DOWNLOADING (progress) → WARMING →
 *    READY, FAILED(MODEL) vs FAILED(VERSION), ABSENT, crash → CRASH_LOOP;
 *  - explicit viewpoints produce the expected parallax sign and magnitude,
 *    per view (roadmap §10: the old fake ignored them);
 *  - callers with a smaller struct_size are accepted, and nothing past their
 *    struct_size is written (roadmap §10: the old fake refused them);
 *  - cancel is observed within 100 ms, in warm and in a job.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_conversion_plugin.h"

#include "sim_conv_synth.h"

#include "conversion/conv_model_store.h"
#include "util/u_sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {

using clk = std::chrono::steady_clock;

/*
 *
 * Fake host: settings map, notify + model_request counters.
 *
 */

std::map<std::string, std::string> g_settings;
std::atomic<int> g_notifies{0};
std::atomic<int> g_model_requests{0};

bool
host_get_setting(const char *key, char *buf, uint32_t buf_size)
{
	auto it = g_settings.find(key);
	if (it == g_settings.end() || buf_size == 0) {
		return false;
	}
	std::snprintf(buf, buf_size, "%s", it->second.c_str());
	return true;
}

void
host_notify_changed(uint32_t cap_id)
{
	(void)cap_id;
	g_notifies++;
}

bool
host_model_request(const char *ref)
{
	(void)ref;
	g_model_requests++;
	return true;
}

struct xrt_conversion_plugin_host_iface
make_host(uint32_t flags = 0)
{
	struct xrt_conversion_plugin_host_iface h;
	std::memset(&h, 0, sizeof(h));
	h.struct_size = (uint32_t)sizeof(h);
	h.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	h.host_flags = flags;
	h.get_setting = host_get_setting;
	h.notify_changed = host_notify_changed;
	h.model_request = host_model_request;
	return h;
}

struct xrt_conversion_plugin_host_iface g_host_storage;

using settings = std::map<std::string, std::string>;

//! Negotiate + probe with the given settings (ENABLE=1 and LATENCY_MS=0 added unless present).
struct provider
{
	struct xrt_conversion_plugin_iface *iface = nullptr;
	struct xrt_conversion_plugin_instance *inst = nullptr;

	explicit provider(std::map<std::string, std::string> settings, bool enable = true)
	{
		g_settings = std::move(settings);
		if (enable && g_settings.find("ENABLE") == g_settings.end()) {
			g_settings["ENABLE"] = "1";
		}
		if (g_settings.find("LATENCY_MS") == g_settings.end()) {
			g_settings["LATENCY_MS"] = "0";
		}
		g_notifies = 0;
		g_model_requests = 0;
		g_host_storage = make_host();
		uint32_t abi = 0;
		REQUIRE(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &g_host_storage, &iface,
		                                     &abi) == XRT_SUCCESS);
		REQUIRE(abi == XRT_CONVERSION_PLUGIN_API_VERSION_1);
		REQUIRE(iface != nullptr);
		if (enable) {
			REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
			REQUIRE(inst != nullptr);
		}
	}
	~provider()
	{
		if (inst != nullptr) {
			iface->destroy(inst);
		}
	}

	struct xrt_conv_state
	state(uint32_t cap_id)
	{
		struct xrt_conv_state s;
		std::memset(&s, 0, sizeof(s));
		s.struct_size = (uint32_t)sizeof(s);
		REQUIRE(iface->get_state(inst, cap_id, &s) == XRT_SUCCESS);
		return s;
	}
};

struct cancel_flag
{
	std::atomic<bool> set{false};
	clk::time_point when;
};

bool
is_cancelled(void *ctx)
{
	return static_cast<cancel_flag *>(ctx)->set.load();
}

/*
 *
 * Images.
 *
 */

struct cpu_img
{
	std::vector<uint8_t> bytes;
	struct xrt_conv_image img;

	cpu_img(uint32_t w, uint32_t h, uint32_t bpp, uint32_t fmt, uint32_t layers = 1)
	{
		bytes.assign((size_t)w * h * bpp * layers, 0);
		std::memset(&img, 0, sizeof(img));
		img.memory_kind = XRT_CONV_MEM_CPU;
		img.cpu_ptr = bytes.data();
		img.byte_size = bytes.size();
		img.width = w;
		img.height = h;
		img.format = fmt;
		img.row_stride = w * bpp;
	}
};

//! An RGBA8 image, black, with one white column at @p col.
cpu_img
column_image(uint32_t w, uint32_t h, uint32_t col)
{
	cpu_img im(w, h, 4, XRT_CONV_FMT_RGBA8_UNORM);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *p = &im.bytes[((size_t)y * w + x) * 4];
			const uint8_t v = x == col ? 255 : 0;
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	return im;
}

//! The x of the white column in view @p v of a ROW target (n views, each w wide), row 0.
int
white_column(const cpu_img &views, uint32_t n, uint32_t v)
{
	const uint32_t w = views.img.width / n;
	for (uint32_t x = 0; x < w; x++) {
		const uint8_t *p = &views.bytes[(size_t)(v * w + x) * 4];
		if (p[0] == 255) {
			return (int)x;
		}
	}
	return -1;
}

struct xrt_conv_params
base_params()
{
	struct xrt_conv_params p;
	std::memset(&p, 0, sizeof(p));
	p.struct_size = (uint32_t)sizeof(p);
	p.strength = 1.0f;
	p.convergence_mode = XRT_CONV_CONVERGENCE_AUTO;
	return p;
}

uint64_t
make_views_stream(provider &pv,
                  uint32_t view_count,
                  uint32_t outputs,
                  uint32_t info_size = sizeof(xrt_conv_stream_info))
{
	struct xrt_conv_stream_info si;
	std::memset(&si, 0, sizeof(si));
	si.struct_size = info_size;
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.outputs = outputs;
	si.content_hint = XRT_CONV_CONTENT_PHOTO;
	si.view_count = view_count;
	si.views_layout = XRT_CONV_VIEWS_ROW;
	uint64_t id = 0;
	REQUIRE(pv.iface->stream_create(pv.inst, XRT_CONV_TASK_VIEWS_MONO, &si, &id) == XRT_SUCCESS);
	REQUIRE(id != 0);
	return id;
}

} // namespace

/*
 *
 * negotiate + describe.
 *
 */

TEST_CASE("sim_conversion: negotiate, probe gate, provider status", "[sim_conversion]")
{
	{
		provider pv({}, /*enable=*/false);
		CHECK(std::string(pv.iface->id) == "sim-conversion");
		CHECK(pv.iface->struct_size == sizeof(struct xrt_conversion_plugin_iface));
		struct xrt_conversion_plugin_instance *inst = nullptr;
		CHECK(pv.iface->probe(&inst) == XRT_ERROR_PROBER_NOT_SUPPORTED); // no ENABLE: clean decline
		CHECK(inst == nullptr);
		g_settings["ENABLE"] = "0";
		CHECK(pv.iface->probe(&inst) == XRT_ERROR_PROBER_NOT_SUPPORTED);

		struct xrt_conv_provider_status st;
		std::memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		REQUIRE(pv.iface->get_provider_status(&st));
		CHECK((st.flags & XRT_CONV_PROVIDER_FLAG_FALLBACK) != 0);
	}
	// Every slot of v1 is filled.
	provider pv({});
	const auto *i = pv.iface;
	CHECK(i->get_provider_status != nullptr);
	CHECK(i->probe != nullptr);
	CHECK(i->describe_cap != nullptr);
	CHECK(i->get_generation != nullptr);
	CHECK(i->get_state != nullptr);
	CHECK(i->warm != nullptr);
	CHECK(i->evict != nullptr);
	CHECK(i->stream_create != nullptr);
	CHECK(i->stream_get_output_desc != nullptr);
	CHECK(i->stream_convert != nullptr);
	CHECK(i->stream_destroy != nullptr);
	CHECK(i->job_execute != nullptr);
	CHECK(i->destroy != nullptr);
}

TEST_CASE("sim_conversion: probe meets the 100 ms budget (no device, no model)", "[sim_conversion]")
{
	// Spec §2.3: probe validates settings only. The D3D11 I/O device is
	// created at the first GPU input or output, never here; every satellite
	// respawn pays for the probe, so it is part of the recovery cost.
	provider pv({}, /*enable=*/false);
	g_settings["ENABLE"] = "1";
	// The first probe of a process is also its first u_log line, which pays
	// for the file logger's setup (seconds on a box with a large log folder,
	// #2044): measured and printed, but not the probe's own cost. The host
	// logs the probe alone ("probe returned 0 in N ms").
	double first_ms = 0.0;
	{
		struct xrt_conversion_plugin_instance *inst = nullptr;
		const auto t0 = clk::now();
		REQUIRE(pv.iface->probe(&inst) == XRT_SUCCESS);
		first_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
		pv.iface->destroy(inst);
	}
	std::printf("sim_conversion probe: first call (incl. logger setup) %.3f ms\n", first_ms);
	double worst_ms = 0.0;
	for (int i = 0; i < 5; i++) {
		struct xrt_conversion_plugin_instance *inst = nullptr;
		const auto t0 = clk::now();
		REQUIRE(pv.iface->probe(&inst) == XRT_SUCCESS);
		const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
		worst_ms = std::max(worst_ms, ms);
		REQUIRE(inst != nullptr);
		pv.iface->destroy(inst);
	}
	std::printf("sim_conversion probe: worst of 5 = %.3f ms\n", worst_ms);
	CHECK(worst_ms < 100.0);
}

TEST_CASE("sim_conversion: describe_cap is well-formed per the selftest rules", "[sim_conversion]")
{
	provider pv({});

	uint32_t count = 0;
	REQUIRE(pv.iface->describe_cap(pv.inst, 0, nullptr, &count) == XRT_SUCCESS);
	REQUIRE(count == 7);
	REQUIRE(count <= XRT_CONV_MAX_CAPABILITIES);

	const uint64_t gen0 = pv.iface->get_generation(pv.inst);
	std::set<uint32_t> ids, tasks;
	for (uint32_t idx = 0; idx < count; idx++) {
		struct xrt_conv_capability c;
		std::memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		REQUIRE(pv.iface->describe_cap(pv.inst, idx, &c, nullptr) == XRT_SUCCESS);
		INFO("capability " << idx << " task " << c.task);
		CHECK(c.cap_id != 0);
		CHECK(ids.insert(c.cap_id).second); // unique
		CHECK(c.task >= XRT_CONV_TASK_DEPTH_MONO);
		CHECK(c.task <= XRT_CONV_TASK_SPLATS_STEREO); // known, never a reserved task
		tasks.insert(c.task);
		CHECK(c.lanes != 0);
		CHECK((c.lanes & ~(XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB)) == 0);
		CHECK(c.min_views <= c.max_views);
		CHECK(c.max_views <= XRT_CONV_MAX_VIEWS);
		if (c.task == XRT_CONV_TASK_VIEWS_MONO || c.task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
			CHECK((c.flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0);
			CHECK(c.min_views >= 2);
		}
		CHECK(c.licence_class >= XRT_CONV_LICENCE_PERMISSIVE);
		CHECK(c.licence_class <= XRT_CONV_LICENCE_RESEARCH_ONLY);
		CHECK((c.import_memory_kinds & XRT_CONV_MEM_CPU) != 0); // §2.4 CPU path
		CHECK((c.import_sync_kinds & (1u << XRT_CONV_SYNC_NONE)) != 0);
		CHECK((c.flags & XRT_CONV_CAP_DETERMINISTIC) != 0);
		CHECK(c.outputs != 0);
		CHECK(c.input_kinds != 0);
		CHECK(c.model_id[0] != 0);
		CHECK(c.display_name[0] != 0);
#ifdef _WIN32
		CHECK((c.import_memory_kinds & XRT_CONV_MEM_WIN32_NT_HANDLE) != 0);
		CHECK((c.import_sync_kinds & (1u << XRT_CONV_SYNC_FENCE_NT)) != 0);
#else
		CHECK(c.import_memory_kinds == XRT_CONV_MEM_CPU);
#endif
		if ((c.lanes & XRT_CONV_LANE_JOB) != 0 && (c.outputs & XRT_CONV_OUT_SPLAT_BLOB) != 0) {
			CHECK((c.splat_formats & (1u << XRT_CONV_BLOB_PLY_3DGS)) != 0);
		}
	}
	// Every task of the v1 vocabulary.
	CHECK(tasks == std::set<uint32_t>{1, 2, 3, 4, 5, 6, 7});
	CHECK(pv.iface->get_generation(pv.inst) == gen0);

	// index ≥ count: refused, *out untouched.
	struct xrt_conv_capability past;
	std::memset(&past, 0xAB, sizeof(past));
	past.struct_size = (uint32_t)sizeof(past);
	CHECK(pv.iface->describe_cap(pv.inst, count, &past, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(past.cap_id == 0xABABABABu);
}

TEST_CASE("sim_conversion: LICENCE=research flips the licence class", "[sim_conversion]")
{
	provider pv(settings{{"LICENCE", "research"}});
	struct xrt_conv_capability c;
	std::memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);
	REQUIRE(pv.iface->describe_cap(pv.inst, 0, &c, nullptr) == XRT_SUCCESS);
	CHECK(c.licence_class == XRT_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(c.commercial_ok == 0);
}

/*
 *
 * Scripted readiness.
 *
 */

TEST_CASE("sim_conversion: scripted state sequence replays", "[sim_conversion]")
{
	provider pv({
	    {"SCRIPT",
	     "views-mono.state=downloading:300,warming:300,ready; depth-video.fail=model; "
	     "DEPTH_MONO.state=failed(version)"},
	    {"STATE_7", "absent"},
	    {"WARM_MS", "200"},
	});

	// Before any warm: multi-step scripts are cold; one-step terminals are static.
	CHECK(pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_INSTALLED);
	CHECK(pv.state(XRT_CONV_TASK_SPLATS_MONO).state == XRT_CONV_STATE_INSTALLED); // WARM_MS default
	CHECK(pv.state(XRT_CONV_TASK_SPLATS_STEREO).state == XRT_CONV_STATE_ABSENT);
	{
		auto s = pv.state(XRT_CONV_TASK_DEPTH_MONO);
		CHECK(s.state == XRT_CONV_STATE_FAILED);
		CHECK(s.failure_reason == XRT_CONV_FAIL_VERSION);
		CHECK(s.retryable == 0);
	}

	// A stream on a cold capability is refused.
	struct xrt_conv_stream_info si;
	std::memset(&si, 0, sizeof(si));
	si.struct_size = (uint32_t)sizeof(si);
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.view_count = 2;
	uint64_t id = 0;
	CHECK(pv.iface->stream_create(pv.inst, XRT_CONV_TASK_VIEWS_MONO, &si, &id) != XRT_SUCCESS);

	// warm(): DOWNLOADING (with progress) → WARMING → READY, sampled from another thread.
	cancel_flag never;
	std::atomic<bool> done{false};
	xrt_result_t warm_result = XRT_ERROR_IPC_FAILURE;
	std::thread t([&] {
		warm_result = pv.iface->warm(pv.inst, XRT_CONV_TASK_VIEWS_MONO, is_cancelled, &never);
		done = true;
	});
	std::vector<uint32_t> seen;
	bool progress_mid = false;
	const auto t0 = clk::now();
	while (!done && clk::now() - t0 < std::chrono::seconds(5)) {
		auto s = pv.state(XRT_CONV_TASK_VIEWS_MONO);
		// (INSTALLED until the warm thread has started its script.)
		if (s.state != XRT_CONV_STATE_INSTALLED && (seen.empty() || seen.back() != s.state)) {
			seen.push_back(s.state);
		}
		if (s.state == XRT_CONV_STATE_DOWNLOADING && s.progress > 0.05f && s.progress < 0.95f) {
			progress_mid = true;
			CHECK(s.eta_ns > 0);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	t.join();
	const auto elapsed = clk::now() - t0;
	CHECK(warm_result == XRT_SUCCESS);
	CHECK(elapsed >= std::chrono::milliseconds(550));
	CHECK(pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY);
	// Exactly the scripted order (the final READY may land after the sampler stops).
	REQUIRE(seen.size() >= 2);
	CHECK(seen[0] == XRT_CONV_STATE_DOWNLOADING);
	CHECK(seen[1] == XRT_CONV_STATE_WARMING);
	if (seen.size() > 2) {
		CHECK(seen[2] == XRT_CONV_STATE_READY);
		CHECK(seen.size() == 3);
	}
	CHECK(progress_mid);
	CHECK(g_notifies >= 3);       // pushed transitions: DOWNLOADING, WARMING, READY
	CHECK(g_model_requests == 1); // DOWNLOADING goes through the host's store protocol

	// Now a stream is accepted.
	CHECK(pv.iface->stream_create(pv.inst, XRT_CONV_TASK_VIEWS_MONO, &si, &id) == XRT_SUCCESS);
	pv.iface->stream_destroy(pv.inst, id);

	// fail=model: WARM_MS of WARMING, then a terminal non-retryable FAILED(MODEL).
	CHECK(pv.iface->warm(pv.inst, XRT_CONV_TASK_DEPTH_VIDEO, is_cancelled, &never) != XRT_SUCCESS);
	{
		auto s = pv.state(XRT_CONV_TASK_DEPTH_VIDEO);
		CHECK(s.state == XRT_CONV_STATE_FAILED);
		CHECK(s.failure_reason == XRT_CONV_FAIL_MODEL);
		CHECK(s.retryable == 0);
	}
	// ABSENT stays ABSENT through warm.
	CHECK(pv.iface->warm(pv.inst, XRT_CONV_TASK_SPLATS_STEREO, is_cancelled, &never) != XRT_SUCCESS);
	CHECK(pv.state(XRT_CONV_TASK_SPLATS_STEREO).state == XRT_CONV_STATE_ABSENT);

	// evict → EVICTED; warm → READY again (no download the second time: the script reruns).
	CHECK(pv.iface->evict(pv.inst, XRT_CONV_TASK_VIEWS_MONO) == XRT_SUCCESS);
	CHECK(pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_EVICTED);
}

TEST_CASE("sim_conversion: in-service crash reports FAILED(CRASH_LOOP)", "[sim_conversion]")
{
	provider pv(settings{{"CRASH_AFTER", "2"}});
	const uint64_t id = make_views_stream(pv, 2, XRT_CONV_OUT_VIEWS);
	cpu_img src = column_image(32, 2, 10);
	cpu_img views(64, 2, 4, XRT_CONV_FMT_RGBA8_UNORM);
	struct xrt_conv_frame_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.image_count = 1;
	in.images[0] = src.img;
	struct xrt_conv_params p = base_params();
	for (int k = 0; k < 3; k++) {
		struct xrt_conv_frame_out out;
		std::memset(&out, 0, sizeof(out));
		out.struct_size = (uint32_t)sizeof(out);
		out.views = views.img;
		const xrt_result_t r = pv.iface->stream_convert(pv.inst, id, &in, &p, &out);
		if (k < 2) {
			CHECK(r == XRT_SUCCESS);
		} else {
			CHECK(r != XRT_SUCCESS);
		}
	}
	auto s = pv.state(XRT_CONV_TASK_VIEWS_MONO);
	CHECK(s.state == XRT_CONV_STATE_FAILED);
	CHECK(s.failure_reason == XRT_CONV_FAIL_CRASH_LOOP);
	CHECK(pv.state(XRT_CONV_TASK_DEPTH_MONO).failure_reason == XRT_CONV_FAIL_CRASH_LOOP);
	CHECK(g_notifies >= 1);
	pv.iface->stream_destroy(pv.inst, id);
}

/*
 *
 * Viewpoints.
 *
 */

TEST_CASE("sim_conversion: explicit viewpoints set parallax sign and magnitude", "[sim_conversion]")
{
	// Per view: sample offset = 0.1 · x / rect_width of the view width (sim_conv_synth.h).
	REQUIRE(SIM_CONV_SYNTH_PARALLAX_GAIN == 0.1f);

	provider pv({});
	constexpr uint32_t W = 200, H = 4, COL = 100;
	const uint64_t id = make_views_stream(pv, 2, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH);

	struct xrt_conv_output_desc od;
	std::memset(&od, 0, sizeof(od));
	od.struct_size = (uint32_t)sizeof(od);
	REQUIRE(pv.iface->stream_get_output_desc(pv.inst, id, W, H, &od) == XRT_SUCCESS);
	CHECK(od.views_width == 2 * W);
	CHECK(od.views_height == H);
	CHECK(od.depth_width == W);
	CHECK(od.depth_format == XRT_CONV_FMT_R32_FLOAT);

	cpu_img src = column_image(W, H, COL);
	struct xrt_conv_frame_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.image_count = 1;
	in.image_role[0] = XRT_CONV_IMG_COLOR;
	in.images[0] = src.img;

	auto run = [&](float x0, float x1, int &col0, int &col1, struct xrt_conv_frame_out *keep = nullptr) {
		cpu_img views(od.views_width, od.views_height, 4, XRT_CONV_FMT_RGBA8_UNORM);
		cpu_img depth(od.depth_width, od.depth_height, 4, XRT_CONV_FMT_R32_FLOAT);
		struct xrt_conv_params p = base_params();
		p.view_count = 2;
		p.viewpoint_source = XRT_CONV_VIEWPOINTS_EXPLICIT;
		p.viewpoint_frame = XRT_CONV_VIEWPOINT_FRAME_RECT;
		p.viewpoint_count = 2;
		p.viewpoints_xyz[0][0] = x0;
		p.viewpoints_xyz[0][2] = 0.6f;
		p.viewpoints_xyz[1][0] = x1;
		p.viewpoints_xyz[1][2] = 0.6f;
		p.rect_width_m = 0.3f;
		p.baseline_m = x1 - x0;
		struct xrt_conv_frame_out out;
		std::memset(&out, 0, sizeof(out));
		out.struct_size = (uint32_t)sizeof(out);
		out.views = views.img;
		out.depth = depth.img;
		REQUIRE(pv.iface->stream_convert(pv.inst, id, &in, &p, &out) == XRT_SUCCESS);
		CHECK((out.written & XRT_CONV_OUT_VIEWS) != 0);
		CHECK((out.written & XRT_CONV_OUT_DEPTH) != 0);
		CHECK(out.views_info.view_count == 2);
		CHECK(out.views_info.eye_xyz[0][0] == x0);
		CHECK(out.views_info.eye_xyz[1][0] == x1);
		// Aux depth: the metric ramp 1 m (top) .. 2 m, decoded through depth_info.
		CHECK(out.depth_info.kind == XRT_CONV_DEPTH_METRIC);
		CHECK(out.depth_info.same_inference == 1);
		const float *d = reinterpret_cast<const float *>(depth.bytes.data());
		const float top = out.depth_info.value_scale * d[0] + out.depth_info.value_shift;
		const float bot = out.depth_info.value_scale * d[(H - 1) * W] + out.depth_info.value_shift;
		CHECK(top > 1.0f);
		CHECK(top < bot);
		CHECK(bot < 2.0f);
		col0 = white_column(views, 2, 0);
		col1 = white_column(views, 2, 1);
		if (keep != nullptr) {
			*keep = out;
		}
	};

	// Symmetric pair, ±30 mm on a 0.3 m rect: offsets ±0.01 · 200 = ±2 px. A
	// right-hand viewpoint samples further right, so the feature appears further
	// LEFT in the right view (crossed disparity).
	int c0 = -1, c1 = -1;
	run(-0.03f, 0.03f, c0, c1);
	CHECK(c0 == (int)COL + 2);
	CHECK(c1 == (int)COL - 2);
	CHECK(c1 < c0);

	// Asymmetric pair (left eye at the centre): view 0 unshifted, view 1 by 4 px —
	// each view follows ITS viewpoint, not a fixed baseline around the middle.
	run(0.0f, 0.06f, c0, c1);
	CHECK(c0 == (int)COL);
	CHECK(c1 == (int)COL - 4);

	// Mirrored pair flips the sign.
	run(0.03f, -0.03f, c0, c1);
	CHECK(c0 == (int)COL - 2);
	CHECK(c1 == (int)COL + 2);

	pv.iface->stream_destroy(pv.inst, id);
}

/*
 *
 * Shorter caller structs.
 *
 */

TEST_CASE("sim_conversion: smaller caller struct_size is accepted, never overrun", "[sim_conversion]")
{
	provider pv({});

	// Capability: stop before the identity strings; nothing past struct_size is written.
	{
		struct xrt_conv_capability c;
		std::memset(&c, 0xCD, sizeof(c));
		std::memset(&c, 0, offsetof(struct xrt_conv_capability, licence_spdx));
		c.struct_size = (uint32_t)offsetof(struct xrt_conv_capability, licence_spdx);
		REQUIRE(pv.iface->describe_cap(pv.inst, 3, &c, nullptr) == XRT_SUCCESS);
		CHECK(c.task == XRT_CONV_TASK_VIEWS_MONO);
		const auto *bytes = reinterpret_cast<const uint8_t *>(&c);
		bool untouched = true;
		for (size_t b = offsetof(struct xrt_conv_capability, licence_spdx); b < sizeof(c); b++) {
			untouched = untouched && bytes[b] == 0xCD;
		}
		CHECK(untouched);
	}
	// State: stop before version.
	{
		struct xrt_conv_state s;
		std::memset(&s, 0xCD, sizeof(s));
		s.struct_size = (uint32_t)offsetof(struct xrt_conv_state, version);
		REQUIRE(pv.iface->get_state(pv.inst, XRT_CONV_TASK_VIEWS_MONO, &s) == XRT_SUCCESS);
		CHECK(s.state == XRT_CONV_STATE_READY);
		CHECK(static_cast<uint8_t>(s.version[0]) == 0xCD);
	}

	// Stream info without max_input_*; params that stop before the viewpoint
	// policy (an older runtime): the historical fixed fan (±1 % of 200 = ±2 px).
	const uint64_t id = make_views_stream(pv, 2, XRT_CONV_OUT_VIEWS,
	                                      (uint32_t)offsetof(struct xrt_conv_stream_info, max_input_width));
	constexpr uint32_t W = 200, H = 2, COL = 100;
	cpu_img src = column_image(W, H, COL);
	cpu_img views(2 * W, H, 4, XRT_CONV_FMT_RGBA8_UNORM);

	struct xrt_conv_frame_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.image_count = 1;
	in.images[0] = src.img;

	struct xrt_conv_params p = base_params();
	p.viewpoint_count = 2; // set but NOT covered by struct_size: must be ignored
	p.viewpoints_xyz[0][0] = 0.0f;
	p.viewpoints_xyz[1][0] = 0.3f;
	p.rect_width_m = 0.3f;
	p.struct_size = (uint32_t)offsetof(struct xrt_conv_params, viewpoint_frame);

	// Frame out that ends before views_info: neither info struct may be touched.
	struct xrt_conv_frame_out out;
	std::memset(&out, 0xCD, sizeof(out));
	out.written = 0;
	out.views = views.img;
	std::memset(&out.depth, 0, sizeof(out.depth));
	std::memset(&out.confidence, 0, sizeof(out.confidence));
	std::memset(&out.disparity, 0, sizeof(out.disparity));
	out.struct_size = (uint32_t)offsetof(struct xrt_conv_frame_out, views_info);
	REQUIRE(pv.iface->stream_convert(pv.inst, id, &in, &p, &out) == XRT_SUCCESS);
	CHECK(out.written == XRT_CONV_OUT_VIEWS);
	CHECK(white_column(views, 2, 0) == (int)COL + 2);
	CHECK(white_column(views, 2, 1) == (int)COL - 2);
	CHECK(out.views_info.view_count == 0xCDCDCDCDu);
	CHECK(out.depth_info.kind == 0xCDCDCDCDu);

	pv.iface->stream_destroy(pv.inst, id);
}

/*
 *
 * Cancel.
 *
 */

TEST_CASE("sim_conversion: cancel is observed within 100 ms", "[sim_conversion]")
{
	provider pv({{"WARM_MS", "5000"}, {"JOB_MS", "5000"}});

	auto cancel_after = [](cancel_flag &f, int ms) {
		return std::thread([&f, ms] {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			f.when = clk::now();
			f.set = true;
		});
	};

	// warm.
	{
		cancel_flag f;
		std::thread t = cancel_after(f, 150);
		const xrt_result_t r = pv.iface->warm(pv.inst, XRT_CONV_TASK_DEPTH_MONO, is_cancelled, &f);
		const auto lag = clk::now() - f.when;
		t.join();
		CHECK(r != XRT_SUCCESS);
		CHECK(lag < std::chrono::milliseconds(100));
		CHECK(pv.state(XRT_CONV_TASK_DEPTH_MONO).state == XRT_CONV_STATE_WARMING);
	}

	// job (SPLATS_MONO), once its capability is READY.
	provider ready(settings{{"JOB_MS", "5000"}});
	cpu_img src = column_image(16, 16, 3);
	struct xrt_conv_job_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.input_kind = XRT_CONV_INPUT_MONO;
	in.image_count = 1;
	in.images[0] = src.img;
	struct xrt_conv_params p = base_params();
	struct xrt_conv_job_out out;
	std::memset(&out, 0, sizeof(out));
	out.struct_size = (uint32_t)sizeof(out);

	cancel_flag f;
	std::atomic<int> progress_calls{0};
	struct ctx_t
	{
		cancel_flag *f;
		std::atomic<int> *progress;
	} ctx{&f, &progress_calls};
	struct xrt_conv_job_control ctl;
	std::memset(&ctl, 0, sizeof(ctl));
	ctl.struct_size = (uint32_t)sizeof(ctl);
	ctl.ctx = &ctx;
	ctl.progress = [](void *c, float, const char *) { (*static_cast<ctx_t *>(c)->progress)++; };
	ctl.is_cancelled = [](void *c) { return static_cast<ctx_t *>(c)->f->set.load(); };

	std::thread t = cancel_after(f, 200);
	const xrt_result_t r = ready.iface->job_execute(ready.inst, XRT_CONV_TASK_SPLATS_MONO, &in, &p, &ctl, &out);
	const auto lag = clk::now() - f.when;
	t.join();
	CHECK(r != XRT_SUCCESS);
	CHECK(lag < std::chrono::milliseconds(100));
	CHECK(progress_calls >= 2);
	CHECK(out.written == 0);
}

TEST_CASE("sim_conversion: splat job writes a PLY through the sink", "[sim_conversion]")
{
	provider pv(settings{{"JOB_MS", "0"}});
	cpu_img src = column_image(16, 16, 3);
	struct xrt_conv_job_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.input_kind = XRT_CONV_INPUT_MONO;
	in.image_count = 1;
	in.images[0] = src.img;
	struct xrt_conv_params p = base_params();

	struct sink_ctx
	{
		uint32_t format = 0;
		std::vector<uint8_t> bytes;
	} sc;
	struct xrt_conv_blob_sink sink;
	std::memset(&sink, 0, sizeof(sink));
	sink.struct_size = (uint32_t)sizeof(sink);
	sink.role = XRT_CONV_OUT_SPLAT_BLOB;
	sink.ctx = &sc;
	sink.begin = [](void *c, uint32_t fmt, uint64_t) {
		static_cast<sink_ctx *>(c)->format = fmt;
		return true;
	};
	sink.write = [](void *c, const void *b, uint64_t n) {
		auto *s = static_cast<sink_ctx *>(c);
		s->bytes.insert(s->bytes.end(), (const uint8_t *)b, (const uint8_t *)b + n);
		return true;
	};
	struct xrt_conv_job_control ctl;
	std::memset(&ctl, 0, sizeof(ctl));
	ctl.struct_size = (uint32_t)sizeof(ctl);
	ctl.sink_count = 1;
	ctl.sinks = &sink;
	struct xrt_conv_job_out out;
	std::memset(&out, 0, sizeof(out));
	out.struct_size = (uint32_t)sizeof(out);

	REQUIRE(pv.iface->job_execute(pv.inst, XRT_CONV_TASK_SPLATS_MONO, &in, &p, &ctl, &out) == XRT_SUCCESS);
	CHECK(sc.format == XRT_CONV_BLOB_PLY_3DGS);
	REQUIRE(sc.bytes.size() > 4);
	CHECK(std::string(sc.bytes.begin(), sc.bytes.begin() + 4) == "ply\n");
	CHECK(out.written == XRT_CONV_OUT_SPLAT_BLOB);
	CHECK(out.splat_info.blob_format == XRT_CONV_BLOB_PLY_3DGS);
	CHECK(out.splat_info.metric == 0);
	CHECK(out.splat_info.intrinsics_source == XRT_CONV_INTRINSICS_ASSUMED);
	CHECK(out.splat_info.splat_count > 0);

	// SPLATS_STEREO is metric only with a known baseline (never defaulted).
	in.input_kind = XRT_CONV_INPUT_STEREO_PAIR;
	p.stereo.baseline_m = 0.065f;
	sc.bytes.clear();
	REQUIRE(pv.iface->job_execute(pv.inst, XRT_CONV_TASK_SPLATS_STEREO, &in, &p, &ctl, &out) == XRT_SUCCESS);
	CHECK(out.splat_info.metric == 1);
}

TEST_CASE("sim_conversion: DEPTH_VIDEO is temporal and FRAME_RESET snaps the phase", "[sim_conversion]")
{
	provider pv({});
	struct xrt_conv_stream_info si;
	std::memset(&si, 0, sizeof(si));
	si.struct_size = (uint32_t)sizeof(si);
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.outputs = XRT_CONV_OUT_DEPTH;
	uint64_t id = 0;
	REQUIRE(pv.iface->stream_create(pv.inst, XRT_CONV_TASK_DEPTH_VIDEO, &si, &id) == XRT_SUCCESS);

	cpu_img src = column_image(8, 8, 1);
	struct xrt_conv_frame_in in;
	std::memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.image_count = 1;
	in.images[0] = src.img;

	auto frame = [&](uint32_t flags) {
		cpu_img depth(8, 8, 4, XRT_CONV_FMT_R32_FLOAT);
		struct xrt_conv_params p = base_params();
		p.frame_flags = flags;
		struct xrt_conv_frame_out out;
		std::memset(&out, 0, sizeof(out));
		out.struct_size = (uint32_t)sizeof(out);
		out.depth = depth.img;
		REQUIRE(pv.iface->stream_convert(pv.inst, id, &in, &p, &out) == XRT_SUCCESS);
		CHECK(out.written == XRT_CONV_OUT_DEPTH);
		CHECK(out.depth_info.temporal == 1);
		CHECK(out.depth_info.kind == XRT_CONV_DEPTH_AFFINE);
		return out.depth_info.range_max;
	};
	const float f0 = frame(0);
	const float f1 = frame(0);
	CHECK(f1 != f0); // the phase moved
	const float reset = frame(XRT_CONV_FRAME_RESET);
	CHECK(reset == f0); // snapped back to phase 0
	pv.iface->stream_destroy(pv.inst, id);
}

/*
 *
 * ADR-053 P4 (#2108): DOWNLOAD_MS through the host model store.
 *
 */

namespace {

struct store_env
{
	std::filesystem::path dir;
	std::string manifest;
	explicit store_env(bool allow_downloads)
	{
		dir = std::filesystem::temp_directory_path() /
		      ("dxr_sim_store_" + std::to_string((unsigned long long)clk::now().time_since_epoch().count()));
		std::filesystem::create_directories(dir / "src");
		std::string blob;
		for (int i = 0; i < 64; i++) {
			blob += "0123456789abcdef";
		}
		uint8_t d[32];
		u_sha256(blob.data(), blob.size(), d);
		char hex[65];
		for (int i = 0; i < 32; i++) {
			std::snprintf(hex + i * 2, 3, "%02x", d[i]);
		}
		std::ofstream(dir / "src" / (std::string(hex) + ".bin"), std::ios::binary) << blob;
		std::ofstream(dir / "sim.models.json")
		    << "{\"schema\":2,\"pack\":\"sim\",\"provider\":\"sim-conversion\",\"blobBaseUrl\":\"src\",\"models\":[{"
		       "\"name\":\"fake\",\"license\":{\"commercial\":true},\"variants\":[{\"id\":\"cpu\",\"files\":[{"
		       "\"format\":\"bin\",\"sha256\":\""
		    << hex << "\",\"size\":1024}]}]}]}";
		manifest = (dir / "sim.models.json").u8string();
		const std::string root = (dir / "Models").u8string();
#ifdef _WIN32
		_putenv_s("DXR_CONV_MODELS_ROOT", root.c_str());
#else
		setenv("DXR_CONV_MODELS_ROOT", root.c_str(), 1);
#endif
		const char *packs[] = {manifest.c_str()};
		conv_store_host_configure(packs, 1, allow_downloads, false);
	}
	~store_env()
	{
		conv_store_host_shutdown();
#ifdef _WIN32
		_putenv_s("DXR_CONV_MODELS_ROOT", "");
#else
		unsetenv("DXR_CONV_MODELS_ROOT");
#endif
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
};

} // namespace

TEST_CASE("sim_conversion: DOWNLOADING runs the host model store (file:// fake pack)", "[sim_conversion]")
{
	store_env env(true);
	provider pv({{"SCRIPT", "views-mono.state=downloading:200,ready"}});
	g_host_storage.model_resolve = conv_store_host_model_resolve;
	g_host_storage.model_request = conv_store_host_model_request;

	cancel_flag never;
	std::atomic<bool> done{false};
	xrt_result_t warm_result = XRT_ERROR_IPC_FAILURE;
	std::thread t([&] {
		warm_result = pv.iface->warm(pv.inst, XRT_CONV_TASK_VIEWS_MONO, is_cancelled, &never);
		done = true;
	});
	bool saw_downloading = false;
	const auto t0 = clk::now();
	while (!done && clk::now() - t0 < std::chrono::seconds(5)) {
		saw_downloading |= pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_DOWNLOADING;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	t.join();
	CHECK(warm_result == XRT_SUCCESS);
	CHECK(saw_downloading);
	CHECK(pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY);
	// The host wrote the blob with the write protocol.
	xrt_conv_model_location loc{};
	loc.struct_size = sizeof(loc);
	REQUIRE(conv_store_host_model_resolve("sim/fake", &loc));
	CHECK(loc.state == XRT_CONV_STATE_READY);
	CHECK(std::filesystem::exists(std::filesystem::u8path(loc.path)));
}

TEST_CASE("sim_conversion: a download forbidden by policy is ABSENT(policy)", "[sim_conversion]")
{
	store_env env(false);
	provider pv({{"SCRIPT", "views-mono.state=downloading:100,ready"}});
	g_host_storage.model_resolve = conv_store_host_model_resolve;
	g_host_storage.model_request = conv_store_host_model_request;
	cancel_flag never;
	CHECK(pv.iface->warm(pv.inst, XRT_CONV_TASK_VIEWS_MONO, is_cancelled, &never) != XRT_SUCCESS);
	auto s = pv.state(XRT_CONV_TASK_VIEWS_MONO);
	CHECK(s.state == XRT_CONV_STATE_ABSENT);
	CHECK(s.failure_reason == XRT_CONV_FAIL_POLICY);

	// Policy lifted: the next warm restarts the script and downloads.
	const char *packs[] = {env.manifest.c_str()};
	conv_store_host_configure(packs, 1, true, false);
	CHECK(pv.iface->warm(pv.inst, XRT_CONV_TASK_VIEWS_MONO, is_cancelled, &never) == XRT_SUCCESS);
	CHECK(pv.state(XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY);
}
