// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2034): the satellite host proxy against a real host
 *         process (tests_conv_proxy_host, a scripted host speaking the real
 *         wire), through the real conversion core: the CPU lane end to end,
 *         a kill mid-stream (the stream keeps its handle and recovers), the
 *         crash-loop cutoff, the watchdog kill, a terminal exit.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_core.h"
#include "conversion/conv_host_proxy.h"
#include "conversion/conv_host_wire.h"

#include "os/os_time.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef CONV_PROXY_HOST
#error "CONV_PROXY_HOST (path of tests_conv_proxy_host) must be defined"
#endif

namespace {

constexpr uint64_t MS = 1000000ull;

uint64_t
now_ns()
{
	return os_monotonic_get_ns();
}

void
sleep_ms(uint32_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = now_ns() + (uint64_t)timeout_ms * MS;
	while (now_ns() < end) {
		if (pred()) {
			return true;
		}
		sleep_ms(5);
	}
	return pred();
}

std::atomic<uint64_t> g_key{1};

bool
cpu_alloc(void *, const conv_image_request *req, xrt_conv_image *out)
{
	const uint32_t bpp = 4; // RGBA8 views / R32F depth: all this test allocates
	const uint64_t size = (uint64_t)req->width * req->height * bpp * (req->array_layers ? req->array_layers : 1);
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp;
	out->import_key = g_key++;
	return true;
}

void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
}

struct CoreOps
{
	conv_core *c = nullptr;

	uint32_t
	row_state(uint32_t cap_id = 4)
	{
		static conv_registry reg;
		conv_core_copy_registry(c, &reg);
		const int32_t ri = conv_registry_find_row(&reg, 0, cap_id);
		return ri < 0 ? XRT_CONV_STATE_UNKNOWN : reg.rows[ri].rd.state;
	}

	uint32_t
	row_reason()
	{
		static conv_registry reg;
		conv_core_copy_registry(c, &reg);
		const int32_t ri = conv_registry_find_row(&reg, 0, 4);
		return ri < 0 ? 0u : reg.rows[ri].rd.reason;
	}

	uint64_t
	stream(uint32_t task = XRT_CONV_TASK_VIEWS_MONO, uint32_t cap_id = 4)
	{
		// A real host start costs seconds (its logger setup), more under a parallel ctest.
		REQUIRE(wait_until([&] { return row_state(cap_id) != XRT_CONV_STATE_UNKNOWN; }, 40000));
		xrt_conv_wire_stream_desc d{};
		d.struct_size = sizeof(d);
		d.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		d.task = task;
		d.outputs = task == XRT_CONV_TASK_VIEWS_MONO ? XRT_CONV_OUT_VIEWS : XRT_CONV_OUT_DEPTH;
		d.view_count = task == XRT_CONV_TASK_VIEWS_MONO ? 2u : 0u;
		d.views_layout = XRT_CONV_VIEWS_ROW;
		d.input_kind = XRT_CONV_INPUT_MONO;
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		return cr.stream_id;
	}

	bool
	submit(uint64_t sid, uint8_t fill)
	{
		conv_submit_layout lay{};
		lay.image_count = 1;
		lay.width[0] = 32;
		lay.height[0] = 16;
		lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
		lay.color_space[0] = XRT_CONV_CS_SRGB;
		lay.role[0] = XRT_CONV_IMG_COLOR;
		conv_submit_slot slot{};
		if (conv_core_stream_begin_submit(c, sid, &lay, &slot) != XRT_SUCCESS) {
			return false;
		}
		std::memset(slot.images[0].cpu_ptr, fill, (size_t)slot.images[0].byte_size);
		conv_frame_submit fr{};
		fr.source_time = (int64_t)now_ns();
		fr.rect_valid = true;
		fr.rect_w_m = 0.3f;
		fr.rect_h_m = 0.2f;
		uint64_t fid = 0;
		return conv_core_stream_commit_submit(c, sid, slot.slot, &fr, &fid) == XRT_SUCCESS;
	}

	//! Submit until a NEW result arrives (frames before READY are dropped latest-wins).
	bool
	convert(uint64_t sid, uint8_t fill, conv_result *r, uint32_t timeout_ms = 10000)
	{
		const uint64_t end = now_ns() + (uint64_t)timeout_ms * MS;
		while (now_ns() < end) {
			if (!submit(sid, fill)) {
				return false;
			}
			if (wait_until([&] { return conv_core_stream_acquire(c, sid, true, r) == XRT_SUCCESS; }, 200)) {
				return true;
			}
		}
		return false;
	}

	conv_stream_stats
	stats(uint64_t sid)
	{
		conv_stream_stats s{};
		REQUIRE(conv_core_stream_get_stats(c, sid, &s) == XRT_SUCCESS);
		return s;
	}

};

struct Fixture : CoreOps
{
	conv_host *host = nullptr; //!< owned by the core once added
	std::string marker;

	explicit Fixture(const std::vector<std::string> &extra,
	                 uint64_t watchdog_ns = 0,
	                 uint32_t crash_loop = 5,
	                 const char *host_exe = CONV_PROXY_HOST,
	                 const char *provider_id = "sat-test",
	                 const char *binary = "scripted")
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		cfg.watchdog_stream_ns = watchdog_ns;
		REQUIRE(conv_core_create(&cfg, &c) == XRT_SUCCESS);

		conv_supervisor_config sc;
		conv_supervisor_config_defaults(&sc);
		sc.backoff_initial_ns = 50 * MS;
		sc.backoff_max_ns = 200 * MS;
		sc.crash_loop_count = crash_loop;
		sc.hello_timeout_ns = 30000 * MS;

		static std::vector<std::string> keep;
		keep = extra;
		args.clear();
		for (const auto &a : keep) {
			args.push_back(a.c_str());
		}
		conv_host_proxy_config pc;
		conv_host_proxy_config_defaults(&pc);
		pc.provider_id = provider_id;
		pc.provider_binary = binary;
		pc.host_exe = host_exe;
		pc.supervisor = &sc;
		pc.scratch_bytes = 4ull * 1024 * 1024;
		pc.extra_args = args.empty() ? nullptr : args.data();
		pc.extra_arg_count = (uint32_t)args.size();
		REQUIRE(conv_host_proxy_create(&pc, &host) == XRT_SUCCESS);

		conv_provider_desc d{};
		d.id = provider_id;
		d.probe_order = 100;
		d.hosting = CONV_HOSTING_SATELLITE;
		d.enabled = true;
		d.host = host;
		REQUIRE(conv_core_add_provider(c, &d) >= 0);
	}
	~Fixture()
	{
		conv_core_destroy(&c);
	}

	conv_supervisor_status
	sup()
	{
		conv_supervisor_status st{};
		long pid = 0;
		conv_host_proxy_get_status(host, &st, &pid);
		return st;
	}

	std::vector<const char *> args;
};

std::string
temp_marker()
{
	char buf[256];
#ifdef _WIN32
	const char *t = std::getenv("TEMP");
	std::snprintf(buf, sizeof(buf), "%s\\dxr_conv_proxy_crash_%llu.flag", t != nullptr ? t : ".",
	              (unsigned long long)now_ns());
#else
	std::snprintf(buf, sizeof(buf), "/tmp/dxr_conv_proxy_crash_%llu.flag", (unsigned long long)now_ns());
#endif
	return buf;
}

//! The views result: view v's pixels are the input with blue = 0x40 + v.
void
check_views(const conv_result &r, uint8_t fill)
{
	REQUIRE((r.written & XRT_CONV_OUT_VIEWS) != 0u);
	REQUIRE(r.views.memory_kind == XRT_CONV_MEM_CPU);
	REQUIRE(r.views.cpu_ptr != nullptr);
	const auto *px = static_cast<const uint8_t *>(r.views.cpu_ptr);
	const uint32_t vw = r.views.width / 2;
	CHECK(px[0] == fill);
	CHECK(px[2] == 0x40);
	CHECK(px[(size_t)vw * 4 + 0] == fill);
	CHECK(px[(size_t)vw * 4 + 2] == 0x41);
}

} // namespace

TEST_CASE("proxy: the CPU lane end to end through a real host process", "[conv_proxy]")
{
	Fixture fx({});
	const uint64_t sid = fx.stream();
	{
		conv_registry reg;
		conv_core_copy_registry(fx.c, &reg);
		REQUIRE(reg.provider_count == 1u);
		CHECK(reg.providers[0].hosting == CONV_HOSTING_SATELLITE); // `conv list` shows satellite
		CHECK(reg.providers[0].iface == nullptr);                 // never loaded in this process
	}
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x80, &r));
	check_views(r, 0x80);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(fx.convert(sid, 0x22, &r));
	check_views(r, 0x22);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	CHECK(fx.sup().state == CONV_SUP_RUNNING);
	CHECK(fx.sup().restarts == 0u);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
}

TEST_CASE("proxy: kill mid-stream; the stream keeps its handle and recovers", "[conv_proxy]")
{
	const std::string marker = temp_marker();
	Fixture fx({"--fake-crash-after", "3", "--fake-crash-once", marker});
	const uint64_t sid = fx.stream();
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x10, &r));
	const uint64_t last = r.frame_id;
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(fx.convert(sid, 0x11, &r));
	conv_core_stream_release(fx.c, sid, r.ring_slot);

	// The 3rd convert kills the host (exit 3).
	REQUIRE(fx.submit(sid, 0x12));
	REQUIRE(wait_until([&] { return fx.sup().restarts >= 1u; }, 5000));
	CHECK(fx.sup().last_exit == 3);

	// The handle is still valid; the result comes back, flagged discontinuity.
	REQUIRE(fx.convert(sid, 0x13, &r, 15000));
	check_views(r, 0x13);
	CHECK(r.discontinuity);
	CHECK(r.frame_id != last);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	const conv_stream_stats s = fx.stats(sid);
	CHECK(s.reopens >= 1u);
	CHECK(s.rebinds == 0u);
	CHECK(fx.row_state() == XRT_CONV_STATE_READY);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	std::remove(marker.c_str());
}

TEST_CASE("proxy: a crash loop ends in FAILED(crash_loop); the stream stays alive", "[conv_proxy]")
{
	Fixture fx({"--fake-crash-after", "1"}, 0, 3);
	const uint64_t sid = fx.stream();
	// Every incarnation dies at its first convert.
	REQUIRE(wait_until(
	    [&] {
		    (void)fx.submit(sid, 0x55);
		    sleep_ms(20);
		    return fx.row_state() == XRT_CONV_STATE_FAILED;
	    },
	    20000));
	CHECK(fx.row_reason() == XRT_CONV_FAIL_CRASH_LOOP);
	CHECK(fx.sup().state == CONV_SUP_FAILED);
	conv_stream_stats s{};
	CHECK(conv_core_stream_get_stats(fx.c, sid, &s) == XRT_SUCCESS); // still a handle
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
}

TEST_CASE("proxy: the stall watchdog kills a hung host (exit 30) and the stream recovers", "[conv_proxy]")
{
	// Every incarnation hangs at its 2nd convert; the 1st is served.
	Fixture fx({"--fake-hang-after", "2"}, 400 * MS);
	const uint64_t sid = fx.stream();
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x31, &r));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(fx.submit(sid, 0x32));
	REQUIRE(wait_until([&] { return fx.sup().restarts >= 1u; }, 10000));
	CHECK(fx.sup().last_exit == CONV_HOST_EXIT_WATCHDOG);
	// The respawned host hangs again at its 2nd convert, but serves the 1st.
	REQUIRE(fx.convert(sid, 0x33, &r, 15000));
	check_views(r, 0x33);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
}

TEST_CASE("proxy: a terminal host exit (ABI reject) is FAILED, never respawned", "[conv_proxy]")
{
	Fixture fx({"--fake-exit-at-start", "10"});
	REQUIRE(wait_until([&] { return fx.sup().state == CONV_SUP_FAILED; }, 10000));
	const uint32_t spawns = fx.sup().spawns;
	sleep_ms(400);
	CHECK(fx.sup().spawns == spawns);
	CHECK(fx.sup().last_exit == 10);
}


#if defined(CONV_REAL_HOST_EXE) && defined(SIM_CONVERSION_PATH)

extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

bool
sim_setting(const char *key, char *buf, uint32_t size)
{
	const char *v = std::strcmp(key, "ENABLE") == 0 ? "1" : std::strcmp(key, "LATENCY_MS") == 0 ? "0" : nullptr;
	if (v == nullptr || size == 0) {
		return false;
	}
	std::snprintf(buf, size, "%s", v);
	return true;
}

//! The depth bytes of one DEPTH_MONO conversion of a fixed frame.
std::vector<uint8_t>
depth_bytes(CoreOps &fx)
{
	const uint64_t sid = fx.stream(XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_TASK_DEPTH_MONO);
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x5A, &r, 20000));
	REQUIRE((r.written & XRT_CONV_OUT_DEPTH) != 0u);
	REQUIRE(r.depth.memory_kind == XRT_CONV_MEM_CPU);
	const auto *b = static_cast<const uint8_t *>(r.depth.cpu_ptr);
	std::vector<uint8_t> out(b, b + r.depth.byte_size);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	return out;
}

//! The primary output bytes of one SPLATS_MONO job (the job lane through the host).
std::vector<uint8_t>
job_bytes(CoreOps &fx)
{
	std::vector<uint8_t> pixels(32 * 16 * 4, 0x6B);
	conv_job_create_info ji{};
	ji.task = XRT_CONV_TASK_SPLATS_MONO;
	ji.input_kind = XRT_CONV_INPUT_MONO;
	ji.image_count = 1;
	ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
	ji.images[0].cpu_ptr = pixels.data();
	ji.images[0].byte_size = pixels.size();
	ji.images[0].width = 32;
	ji.images[0].height = 16;
	ji.images[0].row_stride = 32 * 4;
	ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
	ji.params.struct_size = sizeof(ji.params);
	uint64_t job = 0;
	REQUIRE(wait_until([&] { return conv_core_job_submit(fx.c, 1, &ji, &job) == XRT_SUCCESS; }, 10000));
	conv_job_status st{};
	REQUIRE(wait_until(
	    [&] {
		    conv_core_job_status(fx.c, job, &st);
		    return st.status == CONV_JOB_SUCCEEDED || st.status == CONV_JOB_FAILED;
	    },
	    20000));
	REQUIRE(st.status == CONV_JOB_SUCCEEDED);
	std::vector<uint8_t> out(1u << 20);
	uint64_t n = 0, total = 0;
	REQUIRE(conv_core_job_read(fx.c, job, 0, 0, out.data(), out.size(), &n, &total) == XRT_SUCCESS);
	out.resize((size_t)n);
	CHECK(n > 0u);
	CHECK(n == total);
	conv_core_job_destroy(fx.c, job);
	return out;
}

} // namespace

TEST_CASE("proxy: the real displayxr-conversion-host with the sim provider matches in-service", "[conv_proxy]")
{
	// The host inherits the service's environment and reads the settings itself.
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");   // the sim provider lives in the build tree (#952)
	set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1"); // CI runners and this harness are elevated
	set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "1");
	set_env("DXR_CONV_SIM_CONVERSION_LATENCY_MS", "0");
	std::vector<uint8_t> satellite, satellite_job;
	{
		Fixture fx({}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
		satellite = depth_bytes(fx);
		satellite_job = job_bytes(fx);
		CHECK(fx.sup().state == CONV_SUP_RUNNING);
	}

	// The same sources linked in (sim_conversion_static), hosted in-service.
	conv_core_config cfg{};
	cfg.alloc.alloc = cpu_alloc;
	cfg.alloc.free = cpu_free;
	cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
	cfg.prewarm_task_mask = 1ull << 63;
	cfg.rear_cap_m = -1.0f;
	cfg.tick_ms = 10;
	static xrt_conversion_plugin_host_iface host{};
	host.struct_size = sizeof(host);
	host.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	host.get_setting = sim_setting;
	xrt_conversion_plugin_iface *iface = nullptr;
	uint32_t abi = 0;
	REQUIRE(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &host, &iface, &abi) ==
	        XRT_SUCCESS);
	xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(iface->probe(&inst) == XRT_SUCCESS);

	CoreOps view;
	conv_core *&c = view.c;
	REQUIRE(conv_core_create(&cfg, &c) == XRT_SUCCESS);
	conv_provider_desc d{};
	d.id = "sim-conversion";
	d.probe_order = 100;
	d.hosting = CONV_HOSTING_IN_SERVICE;
	d.enabled = true;
	d.iface = iface;
	d.inst = inst;
	REQUIRE(conv_core_add_provider(c, &d) >= 0);
	std::vector<uint8_t> in_service = depth_bytes(view);
	std::vector<uint8_t> in_service_job = job_bytes(view);
	conv_core_destroy(&c);

	REQUIRE(satellite.size() == in_service.size());
	CHECK(satellite == in_service);
	REQUIRE(satellite_job.size() == in_service_job.size());
	CHECK(satellite_job == in_service_job);
}

#endif
