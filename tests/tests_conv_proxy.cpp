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

struct Fixture
{
	conv_core *c = nullptr;
	conv_host *host = nullptr; //!< owned by the core once added
	std::string marker;

	explicit Fixture(const std::vector<std::string> &extra, uint64_t watchdog_ns = 0, uint32_t crash_loop = 5)
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
		sc.hello_timeout_ns = 10000 * MS;

		static std::vector<std::string> keep;
		keep = extra;
		args.clear();
		for (const auto &a : keep) {
			args.push_back(a.c_str());
		}
		conv_host_proxy_config pc;
		conv_host_proxy_config_defaults(&pc);
		pc.provider_id = "sat-test";
		pc.provider_binary = "scripted";
		pc.host_exe = CONV_PROXY_HOST;
		pc.supervisor = &sc;
		pc.scratch_bytes = 4ull * 1024 * 1024;
		pc.extra_args = args.empty() ? nullptr : args.data();
		pc.extra_arg_count = (uint32_t)args.size();
		REQUIRE(conv_host_proxy_create(&pc, &host) == XRT_SUCCESS);

		conv_provider_desc d{};
		d.id = "sat-test";
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

	uint32_t
	row_state()
	{
		static conv_registry reg;
		conv_core_copy_registry(c, &reg);
		const int32_t ri = conv_registry_find_row(&reg, 0, 4);
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

	conv_supervisor_status
	sup()
	{
		conv_supervisor_status st{};
		long pid = 0;
		conv_host_proxy_get_status(host, &st, &pid);
		return st;
	}

	uint64_t
	stream()
	{
		REQUIRE(wait_until([&] { return row_state() != XRT_CONV_STATE_UNKNOWN; }, 10000));
		xrt_conv_wire_stream_desc d{};
		d.struct_size = sizeof(d);
		d.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		d.task = XRT_CONV_TASK_VIEWS_MONO;
		d.outputs = XRT_CONV_OUT_VIEWS;
		d.view_count = 2;
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
