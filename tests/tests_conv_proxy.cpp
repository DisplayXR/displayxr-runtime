// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2034): the satellite host proxy against a real host
 *         process (tests_conv_proxy_host, a scripted host speaking the real
 *         wire), through the real conversion core: the CPU lane end to end,
 *         a kill mid-stream (the stream keeps its handle and recovers), the
 *         crash-loop cutoff, the watchdog kill, a terminal exit; and the
 *         update flow (#2056): exit 20 -> WARMING(updating), no respawn while
 *         the maintenance mutex is held, respawn on its release -- driven by a
 *         real Restart Manager session on Windows and by SIGTERM on POSIX.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_core.h"
#include "conversion/conv_host_proxy.h"
#include "conversion/conv_host_wire.h"
#include "conversion/conv_readiness.h"

#include "os/os_process.h"

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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <restartmanager.h>
#else
#include <signal.h>
#include <sys/types.h>
#endif

#include "os/os_process.h"

#include <cctype>

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

//! The fake maintenance mutex (every OS): held while true.
std::atomic<bool> g_maint_held{false};

bool
fake_maintenance(void *, const char *)
{
	return g_maint_held.load();
}

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

	bool
	row_restarting(uint32_t cap_id = 4)
	{
		static conv_registry reg;
		conv_core_copy_registry(c, &reg);
		const int32_t ri = conv_registry_find_row(&reg, 0, cap_id);
		return ri >= 0 && reg.rows[ri].rd.restarting;
	}

	uint32_t
	row_reason(uint32_t cap_id = 4)
	{
		static conv_registry reg;
		conv_core_copy_registry(c, &reg);
		const int32_t ri = conv_registry_find_row(&reg, 0, cap_id);
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
	                 const char *binary = "scripted",
	                 bool spawn_for_describe = true,
	                 const std::vector<uint8_t> *cache = nullptr,
	                 bool fake_mutex = false)
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
		pc.spawn_for_describe = spawn_for_describe; // these tests start with no describe cache
		if (fake_mutex) {
			pc.maintenance_held = fake_maintenance; // else the real OS check
		}
		REQUIRE(conv_host_proxy_create(&pc, &host) == XRT_SUCCESS);

		conv_provider_desc d{};
		d.id = provider_id;
		d.probe_order = 100;
		d.hosting = CONV_HOSTING_SATELLITE;
		d.enabled = true;
		d.host = host;
		REQUIRE(conv_core_add_provider(c, &d) >= 0);
		if (cache != nullptr) {
			REQUIRE(conv_core_load_describe_cache(c, cache->data(), cache->size()) == 1);
		}
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

TEST_CASE("proxy: core teardown with a lane stuck mid-convert on a hung host returns (bounded)", "[conv_proxy]")
{
	// The host hangs at its 1st convert and the core's stall watchdog is far
	// away, so at teardown a stream lane is still waiting on the host. The
	// control thread (which runs the watchdog) stops first; without the
	// satellite proxies shut down before the lane join, destroy never returns.
	Fixture fx({"--fake-hang-after", "1"}, 600000 * MS);
	const uint64_t sid = fx.stream();
	REQUIRE(fx.submit(sid, 0x41));
	REQUIRE(wait_until(
	    [&] {
		    conv_host_heartbeat hb{};
		    conv_host_get_heartbeat(fx.host, &hb);
		    return hb.oldest_op == CONV_OP_CONVERT && hb.oldest_since_ns != 0;
	    },
	    20000));
	std::atomic<bool> done{false};
	conv_core *core = fx.c;
	fx.c = nullptr; // destroyed here, not by the fixture
	std::thread t([&done, core]() mutable {
		conv_core_destroy(&core);
		done = true;
	});
	const bool returned = wait_until([&] { return done.load(); }, 20000);
	if (returned) {
		t.join();
	} else {
		t.detach(); // leaked on failure: the assertion below reports it
	}
	REQUIRE(returned);
}

TEST_CASE("proxy: the provider's status (FALLBACK, platform state) crosses the wire", "[conv_proxy]")
{
	Fixture fx({});
	REQUIRE(wait_until([&] { return fx.row_state() != XRT_CONV_STATE_UNKNOWN; }, 40000));
	static conv_registry reg;
	REQUIRE(wait_until(
	    [&] {
		    conv_core_copy_registry(fx.c, &reg);
		    return (reg.providers[0].flags & CONV_PROV_FALLBACK) != 0;
	    },
	    5000));
	CHECK(reg.providers[0].platform_state == 1u); // READY, as the provider said
	xrt_conv_provider_status st{};
	REQUIRE(fx.host->ops->get_provider_status(fx.host, &st));
	CHECK(std::string(st.hint) == "scripted test host");
}

TEST_CASE("proxy: the ingest adapter LUID reaches the host; a change restarts it, not a crash", "[conv_proxy]")
{
	Fixture fx({});
	const uint64_t sid = fx.stream();
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x21, &r));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	uint64_t spawned = 1, hello = 1;
	conv_host_proxy_get_adapter(fx.host, &spawned, &hello);
	CHECK(spawned == 0u); // the GPU side had not attached yet
	CHECK(hello == 0u);

	// The GPU hook reports the service's ingest adapter.
	constexpr uint64_t LUID = 0x00000001ABCD1234ull;
	conv_host_proxy_set_adapter(fx.host, LUID, nullptr);
	REQUIRE(wait_until(
	    [&] {
		    conv_host_proxy_get_adapter(fx.host, &spawned, &hello);
		    return hello == LUID;
	    },
	    40000));
	CHECK(spawned == LUID);
	CHECK(fx.sup().spawns == 2u);
	CHECK(fx.sup().restarts == 0u); // closed for update: no crash accounting
	CHECK(fx.sup().last_exit == CONV_HOST_EXIT_UPDATE);

	// The stream keeps its handle and runs on the new host.
	REQUIRE(fx.convert(sid, 0x22, &r, 20000));
	check_views(r, 0x22);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	CHECK(fx.stats(sid).reopens >= 1u);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
}

TEST_CASE("proxy: with a warm describe cache nothing spawns until a warm / create", "[conv_proxy]")
{
	std::vector<uint8_t> cache;
	{
		Fixture first({});
		REQUIRE(wait_until([&] { return first.row_state() != XRT_CONV_STATE_UNKNOWN; }, 40000));
		const size_t n = conv_core_save_describe_cache(first.c, nullptr, 0);
		REQUIRE(n > 0u);
		cache.resize(n);
		REQUIRE(conv_core_save_describe_cache(first.c, cache.data(), n) == n);
	}
	Fixture fx({}, 0, 5, CONV_PROXY_HOST, "sat-test", "scripted", false, &cache);
	CHECK(fx.row_state() == XRT_CONV_STATE_INSTALLED); // answered from the cache
	sleep_ms(1000);
	CHECK(fx.sup().spawns == 0u);
	CHECK(fx.sup().state == CONV_SUP_IDLE);

	// The first create warms the row: that is the demand that spawns the host.
	const uint64_t sid = fx.stream();
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x44, &r, 40000));
	check_views(r, 0x44);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	CHECK(fx.sup().spawns == 1u);
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

TEST_CASE("proxy: exit 20 = WARMING(updating); no respawn while the maintenance mutex is held", "[conv_proxy]")
{
	// #2056 (roadmap §3.6), the state machine on every OS: the scripted host
	// exits 20 at its 3rd convert, once.
	const std::string marker = temp_marker();
	g_maint_held = false;
	Fixture fx({"--fake-update-after", "3", "--fake-update-once", marker}, 0, 5, CONV_PROXY_HOST, "sat-test",
	           "scripted", true, nullptr, true);
	const uint64_t sid = fx.stream();
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x61, &r));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(fx.convert(sid, 0x62, &r));
	const uint64_t last = r.frame_id;
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	const uint32_t spawns = fx.sup().spawns;

	// The installer takes the mutex, then closes the host.
	g_maint_held = true;
	REQUIRE(fx.submit(sid, 0x63));
	REQUIRE(wait_until([&] { return fx.sup().last_exit == CONV_HOST_EXIT_UPDATE; }, 10000));
	REQUIRE(wait_until([&] { return fx.row_reason() == CONV_REASON_UPDATING; }, 5000));
	CHECK(fx.sup().state == CONV_SUP_UPDATING);
	CHECK(fx.row_state() == XRT_CONV_STATE_WARMING);
	CHECK(fx.sup().restarts == 0u); // no crash accounting
	// (The frame in flight when the host exited is its one casualty.)
	const uint64_t failed_at_update = fx.stats(sid).failed;

	// Held: the producer keeps submitting on a valid handle; nothing respawns,
	// for well past the 1 Hz probe and any backoff.
	for (int i = 0; i < 30; i++) {
		CHECK(fx.submit(sid, 0x64));
		sleep_ms(50);
	}
	CHECK(fx.sup().spawns == spawns);
	CHECK(fx.sup().state == CONV_SUP_UPDATING);
	CHECK(fx.row_state() == XRT_CONV_STATE_WARMING);
	conv_stream_stats st0{};
	CHECK(conv_core_stream_get_stats(fx.c, sid, &st0) == XRT_SUCCESS);

	// No unbounded waits: a command to a host that is down is refused at once
	// (well under 1 ms), and the frames above stayed pending, never failed.
	CHECK(conv_host_is_down(fx.host));
	{
		conv_host_cmd probe{};
		probe.op = CONV_OP_WARM;
		probe.cap_id = 4;
		probe.on_complete = [](conv_host_cmd *) {};
		const uint64_t t0 = now_ns();
		const xrt_result_t pr = fx.host->ops->submit(fx.host, &probe);
		const uint64_t dt = now_ns() - t0;
		CHECK(pr != XRT_SUCCESS);
		CHECK((probe.blocked & CONV_CMD_HOST_DOWN) != 0u);
		CHECK(dt < 1 * MS);
	}
	{
		const uint64_t t0 = now_ns();
		CHECK(fx.submit(sid, 0x64));
		CHECK(now_ns() - t0 < 1 * MS);
	}
	CHECK(st0.failed == failed_at_update);

	SECTION("a registration change clears the update gate, the spawn still waits for the mutex")
	{
		conv_host_proxy_config_changed(fx.host);
		sleep_ms(300);
		CHECK(fx.sup().state == CONV_SUP_IDLE);
		CHECK(fx.sup().spawns == spawns);
	}

	// Released: the owed respawn; the stream converts again (discontinuity).
	g_maint_held = false;
	REQUIRE(fx.convert(sid, 0x65, &r, 20000));
	check_views(r, 0x65);
	CHECK(r.frame_id != last);
	CHECK(fx.stats(sid).reopens >= 1u); // the provider stream re-opened on the new host (implicit reset)
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	CHECK(fx.sup().spawns == spawns + 1u);
	CHECK(fx.sup().restarts == 0u);
	CHECK(fx.row_state() == XRT_CONV_STATE_READY);
	CHECK(fx.row_reason() == 0u);
	CHECK(fx.stats(sid).rebinds == 0u);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	std::remove(marker.c_str());
}

#ifdef _WIN32
TEST_CASE("proxy: RmForceShutdown of a host that cannot answer is the crash path", "[conv_proxy]")
{
	// #2056: a provider dependency's non-pumping window makes Restart Manager
	// kill the host. Whatever exit code Windows reports for that kill, the core
	// follows the supervisor's verdict (abnormal: WARMING(restarting)), never the
	// raw code, and the respawn waits for the maintenance mutex.
	char id[48];
	std::snprintf(id, sizeof(id), "sat-rmforce-%lu", (unsigned long)os_process_current_pid());
	std::string host_copy(CONV_PROXY_HOST);
	host_copy.replace(host_copy.size() - 4, 4, std::string("-") + id + ".exe");
	REQUIRE(CopyFileA(CONV_PROXY_HOST, host_copy.c_str(), FALSE));
	{
		g_maint_held = false;
		Fixture fx({"--fake-rm-unresponsive", "1"}, 0, 5, host_copy.c_str(), id, "scripted", true, nullptr, true);
		const uint64_t sid = fx.stream();
		conv_result r{};
		REQUIRE(fx.convert(sid, 0x71, &r));
		conv_core_stream_release(fx.c, sid, r.ring_slot);
		const uint32_t restarts = fx.sup().restarts;
		conv_supervisor_status sst{};
		long host_pid = 0;
		conv_host_proxy_get_status(fx.host, &sst, &host_pid);
		REQUIRE(host_pid > 0);

		std::wstring wfile(host_copy.begin(), host_copy.end());
		for (auto &ch : wfile) {
			ch = ch == L'/' ? L'\\' : ch;
		}
		DWORD session = 0;
		WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
		REQUIRE(RmStartSession(&session, 0, key) == ERROR_SUCCESS);
		LPCWSTR files[1] = {wfile.c_str()};
		REQUIRE(RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS);
		g_maint_held = true; // the installer's session
		const uint64_t t0 = now_ns();
		const DWORD rs = RmShutdown(session, RmForceShutdown, nullptr);
		const uint64_t dt_ms = (now_ns() - t0) / MS;
		REQUIRE(wait_until([&] { return fx.sup().restarts > restarts; }, 30000));
		const int32_t raw = fx.sup().last_exit;
		std::fprintf(stderr, "RmForceShutdown: result %lu after %llu ms; exit code seen by the supervisor %d (0x%x)\n",
		             (unsigned long)rs, (unsigned long long)dt_ms, (int)raw, (unsigned)raw);
		UNSCOPED_INFO("RmShutdown(RmForceShutdown) = " << rs << " after " << dt_ms << " ms; exit code " << raw);
		CHECK(rs == ERROR_SUCCESS);
		CHECK(fx.sup().state == CONV_SUP_BACKOFF); // abnormal, waiting for the mutex
		REQUIRE(wait_until([&] { return fx.row_restarting(); }, 5000));
		CHECK(fx.row_state() == XRT_CONV_STATE_WARMING);
		CHECK(fx.row_reason() == 0u); // restarting, not updating
		sleep_ms(1200);
		CHECK(fx.sup().state == CONV_SUP_BACKOFF); // no respawn while held
		CHECK(RmEndSession(session) == ERROR_SUCCESS);

		g_maint_held = false;
		REQUIRE(fx.convert(sid, 0x72, &r, 20000));
		check_views(r, 0x72);
		conv_core_stream_release(fx.c, sid, r.ring_slot);
		CHECK(fx.row_state() == XRT_CONV_STATE_READY);
		REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	}
	(void)DeleteFileA(host_copy.c_str());
}
#endif


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


/*
 *
 * ADR-053 P2 (#2035): kill tests against the REAL host through the core.
 *
 * The scripted host above dies on cue from inside; these kill the real
 * displayxr-conversion-host from OUTSIDE (TerminateProcess / SIGKILL, the
 * shape of a crash in a model runtime) while a "compositor" thread keeps
 * acquiring results on its own cadence, and check what §3.5 promises: the
 * in-flight completion is reported failed, the stream keeps its handle, the
 * dispatcher never blocks a producer or a consumer, and a respawned host
 * serves the next submit. Then the crash-loop cutoff with real kills and the
 * §2.4 rebind to another candidate, and the hop-cost harness.
 *
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#endif

#include <algorithm>
#include <mutex>

namespace {

//! Kill a host the way a crash does: no orderly anything.
bool
kill_hard(long pid)
{
	if (pid <= 0) {
		return false;
	}
#ifdef _WIN32
	HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
	if (h == nullptr) {
		return false;
	}
	const BOOL ok = TerminateProcess(h, 0xC0000005u); // what an access violation exits with
	CloseHandle(h);
	return ok != FALSE;
#else
	return kill((pid_t)pid, SIGKILL) == 0;
#endif
}

void
real_host_env(const char *latency_ms)
{
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1");
	set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "1");
	set_env("DXR_CONV_SIM_CONVERSION_LATENCY_MS", latency_ms);
}

constexpr uint32_t DEPTH = XRT_CONV_TASK_DEPTH_MONO;

long
host_pid(Fixture &fx)
{
	conv_supervisor_status st{};
	long pid = 0;
	conv_host_proxy_get_status(fx.host, &st, &pid);
	return st.state == CONV_SUP_RUNNING ? pid : 0;
}

conv_readiness
row_rd(CoreOps &fx, int32_t provider_index, uint32_t cap_id)
{
	static conv_registry reg;
	conv_core_copy_registry(fx.c, &reg);
	const int32_t ri = conv_registry_find_row(&reg, provider_index, cap_id);
	conv_readiness rd{};
	if (ri >= 0) {
		rd = reg.rows[ri].rd;
	}
	return rd;
}

struct Dist
{
	std::vector<double> v;
	double
	pct(double p)
	{
		if (v.empty()) {
			return 0.0;
		}
		std::sort(v.begin(), v.end());
		const size_t i = std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5));
		return v[i];
	}
	double
	max()
	{
		return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
	}
};

/*!
 * The compositor's side of a stream: acquire the newest result on a 60 Hz
 * cadence, never waiting for one. Records how long each acquire took and
 * the interval between ticks.
 */
struct Compositor
{
	CoreOps &fx;
	uint64_t sid;
	std::atomic<bool> stop{false};
	std::thread th;
	std::mutex m;
	Dist acquire_ms, tick_ms;
	uint64_t results = 0;
	std::atomic<bool> saw_discontinuity{false};

	Compositor(CoreOps &f, uint64_t s) : fx(f), sid(s)
	{
		th = std::thread([this] { run(); });
	}
	~Compositor()
	{
		finish();
	}
	void
	finish()
	{
		stop = true;
		if (th.joinable()) {
			th.join();
		}
	}
	void
	run()
	{
		const uint64_t period = 16666667ull;
		uint64_t next = now_ns();
		uint64_t last = 0;
		while (!stop) {
			const uint64_t t0 = now_ns();
			conv_result r{};
			if (conv_core_stream_acquire(fx.c, sid, true, &r) == XRT_SUCCESS) {
				if (r.discontinuity) {
					saw_discontinuity = true;
				}
				conv_core_stream_release(fx.c, sid, r.ring_slot);
				results++;
			}
			const uint64_t t1 = now_ns();
			{
				std::lock_guard<std::mutex> lk(m);
				acquire_ms.v.push_back((double)(t1 - t0) / 1e6);
				if (last != 0) {
					tick_ms.v.push_back((double)(t0 - last) / 1e6);
				}
			}
			last = t0;
			next += period;
			const uint64_t n = now_ns();
			if (next > n) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(next - n));
			} else {
				next = n;
			}
		}
	}
};

} // namespace

TEST_CASE("proxy: real host killed between submit and completion: failed, never a hang; the stream recovers",
          "[conv_proxy][conv_kill]")
{
	// 300 ms per convert: the kill lands inside the provider call, after the
	// submit and before its completion record.
	real_host_env("300");
	Fixture fx({}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
	const uint64_t sid = fx.stream(DEPTH, DEPTH);
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x40, &r, 20000));
	const uint64_t frame_before = r.frame_id;
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	const conv_stream_stats before = fx.stats(sid);

	Compositor comp(fx, sid);
	std::this_thread::sleep_for(std::chrono::milliseconds(200)); // a steady cadence first

	const long pid = host_pid(fx);
	REQUIRE(pid > 0);
	REQUIRE(fx.submit(sid, 0x41));
	std::this_thread::sleep_for(std::chrono::milliseconds(100)); // inside the 300 ms convert
	const uint64_t t_kill = now_ns();
	REQUIRE(kill_hard(pid));

	// Producers keep submitting through the outage (latest-wins); no submit blocks.
	Dist submit_ms;
	bool restarted = false;
	while (now_ns() - t_kill < 10000 * MS) {
		const uint64_t t0 = now_ns();
		(void)fx.submit(sid, 0x42);
		submit_ms.v.push_back((double)(now_ns() - t0) / 1e6);
		if (fx.sup().restarts >= 1u) {
			restarted = true;
			break;
		}
		sleep_ms(16);
	}
	REQUIRE(restarted);
	const double detect_ms = (double)(now_ns() - t_kill) / 1e6;
	const int32_t observed_exit = fx.sup().last_exit;

	// The same handle serves again once the host is back.
	REQUIRE(wait_until([&] { return fx.sup().state == CONV_SUP_RUNNING; }, 30000));
	const double running_ms = (double)(now_ns() - t_kill) / 1e6;
	REQUIRE(wait_until([&] { return fx.row_state(DEPTH) == XRT_CONV_STATE_READY; }, 30000));
	const double ready_ms = (double)(now_ns() - t_kill) / 1e6;
	REQUIRE(fx.convert(sid, 0x43, &r, 30000));
	const double recover_ms = (double)(now_ns() - t_kill) / 1e6;
	CHECK(r.frame_id != frame_before);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	comp.finish();
	// The first result after the restart is flagged; the compositor thread
	// may have been the one to acquire it.
	CHECK((r.discontinuity || comp.saw_discontinuity.load()));

	const conv_stream_stats after = fx.stats(sid);
	CHECK(after.failed >= before.failed + 1); // the in-flight convert completed as failed
	CHECK(after.reopens >= 1u);
	CHECK(after.rebinds == 0u);
	CHECK(fx.sup().state == CONV_SUP_RUNNING);

	std::printf("kill mid-convert: exit 0x%08x seen after %.0f ms; host RUNNING after %.0f ms, row READY after %.0f "
	            "ms, served again after %.0f ms; failed %llu -> %llu\n",
	            (unsigned)observed_exit, detect_ms, running_ms, ready_ms, recover_ms,
	            (unsigned long long)before.failed, (unsigned long long)after.failed);
	std::printf("  compositor acquire p50 %.3f / p99 %.3f / max %.3f ms; tick p50 %.1f / p99 %.1f / max %.1f ms "
	            "(%zu ticks)\n",
	            comp.acquire_ms.pct(0.5), comp.acquire_ms.pct(0.99), comp.acquire_ms.max(), comp.tick_ms.pct(0.5),
	            comp.tick_ms.pct(0.99), comp.tick_ms.max(), comp.tick_ms.v.size());
	std::printf("  producer submit during the outage p50 %.3f / max %.3f ms (%zu submits)\n", submit_ms.pct(0.5),
	            submit_ms.max(), submit_ms.v.size());

#ifdef _WIN32
	CHECK((uint32_t)observed_exit == 0xC0000005u); // the supervisor saw the real exit code
#else
	CHECK(observed_exit != 0);
#endif
	// The dispatcher never blocked the consumer or the producer: an acquire
	// or a submit is a lock and a copy, never a wait on the host.
	CHECK(comp.acquire_ms.max() < 20.0);
	CHECK(submit_ms.max() < 50.0);
	// The cadence held through the kill (generous for a loaded CI runner).
	CHECK(comp.tick_ms.pct(0.99) < 50.0);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
}

TEST_CASE("proxy: five real-host kills -> FAILED(crash_loop); the stream rebinds to the in-service candidate",
          "[conv_proxy][conv_kill]")
{
	real_host_env("0");
	// The satellite first (probe order 100), the same provider in-service as
	// the other candidate (200): the §2.4 rebinding rule.
	Fixture fx({}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
	static xrt_conversion_plugin_host_iface hostif{};
	hostif.struct_size = sizeof(hostif);
	hostif.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	hostif.get_setting = sim_setting;
	xrt_conversion_plugin_iface *iface = nullptr;
	uint32_t abi = 0;
	REQUIRE(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &hostif, &iface, &abi) ==
	        XRT_SUCCESS);
	xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
	conv_provider_desc d{};
	d.id = "sim-in-service";
	d.probe_order = 200;
	d.hosting = CONV_HOSTING_IN_SERVICE;
	d.enabled = true;
	d.iface = iface;
	d.inst = inst;
	REQUIRE(conv_core_add_provider(fx.c, &d) >= 0);

	const uint64_t sid = fx.stream(DEPTH, DEPTH);
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x60, &r, 20000));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(std::string(fx.stats(sid).provider_id) == "sim-conversion");

	uint32_t kills = 0;
	std::vector<int32_t> exits;
	// Five real host starts: seconds each, far more when other suites start
	// hosts at the same time (a parallel ctest on a shared box).
	const uint64_t end = now_ns() + 300000 * MS;
	while (fx.sup().state != CONV_SUP_FAILED && now_ns() < end) {
		long pid = 0;
		REQUIRE(wait_until(
		    [&] {
			    // Up AND brought up (the row is READY again): a kill during
			    // bring-up is a different case (see the report on #2035).
			    pid = host_pid(fx);
			    return (pid > 0 && fx.row_state(DEPTH) == XRT_CONV_STATE_READY) ||
			           fx.sup().state == CONV_SUP_FAILED;
		    },
		    60000));
		if (fx.sup().state == CONV_SUP_FAILED) {
			break;
		}
		const uint32_t restarts = fx.sup().restarts;
		REQUIRE(kill_hard(pid));
		kills++;
		REQUIRE(wait_until([&] { return fx.sup().restarts > restarts; }, 10000));
		exits.push_back(fx.sup().last_exit);
	}
	std::printf("crash loop: exits seen:");
	for (int32_t e : exits) {
		std::printf(" 0x%08x", (unsigned)e);
	}
	std::printf("\n");
	CHECK(kills == 5u);
	REQUIRE(fx.sup().state == CONV_SUP_FAILED);
	const conv_readiness rd = row_rd(fx, 0, DEPTH);
	CHECK(rd.state == XRT_CONV_STATE_FAILED);
	CHECK(rd.reason == XRT_CONV_FAIL_CRASH_LOOP);
	for (int32_t e : exits) {
#ifdef _WIN32
		CHECK((uint32_t)e == 0xC0000005u);
#else
		CHECK(e != 0);
#endif
	}

	// The handle survives and moves to the other candidate.
	REQUIRE(fx.convert(sid, 0x61, &r, 20000));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	const conv_stream_stats s = fx.stats(sid);
	CHECK(s.rebinds >= 1u);
	CHECK(std::string(s.provider_id) == "sim-in-service");
	std::printf("crash loop: %u kills -> FAILED(crash_loop), rebinds %llu, now on %s\n", kills,
	            (unsigned long long)s.rebinds, s.provider_id);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	conv_core_destroy(&fx.c);
}

namespace {

/*!
 * @p n single submits, each waited for: the core's own submit -> publish
 * time (conv_result.latency_ns) and the caller's wall time.
 */
void
hop_cost(CoreOps &fx, uint64_t sid, uint32_t n, Dist &core_ms, Dist &wall_ms, uint32_t &lost)
{
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x70, &r, 20000)); // warm
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	for (uint32_t i = 0; i < n; i++) {
		const uint64_t t0 = now_ns();
		REQUIRE(fx.submit(sid, (uint8_t)(0x10 + (i & 0x3f))));
		bool got = false;
		while (now_ns() - t0 < 2000 * MS) {
			if (conv_core_stream_acquire(fx.c, sid, true, &r) == XRT_SUCCESS) {
				got = true;
				break;
			}
			std::this_thread::yield();
		}
		if (!got) {
			lost++;
			continue;
		}
		wall_ms.v.push_back((double)(now_ns() - t0) / 1e6);
		core_ms.v.push_back((double)r.latency_ns / 1e6);
		conv_core_stream_release(fx.c, sid, r.ring_slot);
	}
}

void
print_dist(const char *what, Dist &core, Dist &wall)
{
	std::printf("%-11s core submit->result p50 %.3f p90 %.3f p99 %.3f max %.3f ms | wall p50 %.3f p99 %.3f ms (n=%zu)\n",
	            what, core.pct(0.5), core.pct(0.9), core.pct(0.99), core.max(), wall.pct(0.5), wall.pct(0.99),
	            core.v.size());
}

} // namespace

TEST_CASE("proxy: hop cost, CPU lane: 200 submits in-host vs in-service (sim provider, 0 ms convert)",
          "[conv_proxy][hopcost]")
{
	// The headless half of the §3.7 budget: what the satellite hop (rings,
	// wakes, shm copies in and out, the host's own dispatch) adds to one
	// submit -> result, against the same provider in-service. The GPU half
	// (NT-handle open, CPU fence observe, the copy into the ring target, the
	// weave-on-scanout split) needs the panel box: see the #2035 recipe.
	real_host_env("0");
	constexpr uint32_t N = 200;
	Dist host_core, host_wall, svc_core, svc_wall;
	uint32_t host_lost = 0, svc_lost = 0;
	{
		Fixture fx({}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
		const uint64_t sid = fx.stream(DEPTH, DEPTH);
		hop_cost(fx, sid, N, host_core, host_wall, host_lost);
		REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	}
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		static xrt_conversion_plugin_host_iface hostif{};
		hostif.struct_size = sizeof(hostif);
		hostif.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
		hostif.get_setting = sim_setting;
		xrt_conversion_plugin_iface *iface = nullptr;
		uint32_t abi = 0;
		REQUIRE(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &hostif, &iface, &abi) ==
		        XRT_SUCCESS);
		xrt_conversion_plugin_instance *inst = nullptr;
		REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
		CoreOps view;
		REQUIRE(conv_core_create(&cfg, &view.c) == XRT_SUCCESS);
		conv_provider_desc d{};
		d.id = "sim-conversion";
		d.probe_order = 100;
		d.hosting = CONV_HOSTING_IN_SERVICE;
		d.enabled = true;
		d.iface = iface;
		d.inst = inst;
		REQUIRE(conv_core_add_provider(view.c, &d) >= 0);
		const uint64_t sid = view.stream(DEPTH, DEPTH);
		hop_cost(view, sid, N, svc_core, svc_wall, svc_lost);
		REQUIRE(conv_core_stream_destroy(view.c, sid) == XRT_SUCCESS);
		conv_core_destroy(&view.c);
	}
	print_dist("in-host", host_core, host_wall);
	print_dist("in-service", svc_core, svc_wall);
	std::printf("hop (p50 core, in-host - in-service): %.3f ms\n", host_core.pct(0.5) - svc_core.pct(0.5));
	CHECK(host_lost == 0u);
	CHECK(svc_lost == 0u);
	CHECK(host_core.v.size() == N);
	CHECK(svc_core.v.size() == N);
}

TEST_CASE("proxy: close / evict / teardown with the real host NOT running never blocks and never spawns",
          "[conv_proxy][conv_kill]")
{
	real_host_env("0");
	SECTION("the host idle-exited")
	{
		Fixture fx({"--idle-exit-ms", "1500"}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
		const uint64_t sid = fx.stream(DEPTH, DEPTH);
		conv_result r{};
		REQUIRE(fx.convert(sid, 0x50, &r, 20000));
		conv_core_stream_release(fx.c, sid, r.ring_slot);
		REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
		// The warm row pins the capability in the host; evicting it (host
		// running) leaves nothing bound, and the host leaves on its own.
		conv_core_evict_task(fx.c, DEPTH);
		REQUIRE(wait_until([&] { return fx.sup().last_exit != INT32_MIN; }, 20000)); // gone once
		REQUIRE(host_pid(fx) == 0);
		const uint32_t spawns = fx.sup().spawns;

		// Teardown right away, whatever the supervisor does next (it may be
		// respawning: see the idle-exit accounting case below).
		uint64_t t0 = now_ns();
		conv_core_evict_task(fx.c, DEPTH);
		const double evict_ms = (double)(now_ns() - t0) / 1e6;
		const conv_supervisor_status before = fx.sup();
		t0 = now_ns();
		conv_core_destroy(&fx.c); // frees the proxy: no fx.sup() after this
		const double destroy_ms = (double)(now_ns() - t0) / 1e6;
		std::printf("idle-exited host (exit %d, state %s): evict %.3f ms, core teardown %.3f ms, spawns %u -> %u\n",
		            before.last_exit, conv_supervisor_state_str(before.state), evict_ms, destroy_ms, spawns,
		            before.spawns);
		CHECK(evict_ms < 100.0);
		CHECK(destroy_ms < 2000.0);
	}
	SECTION("the host is FAILED (crash loop) with a stream still open")
	{
		Fixture fx({}, 0, 1, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
		const uint64_t sid = fx.stream(DEPTH, DEPTH);
		conv_result r{};
		REQUIRE(fx.convert(sid, 0x51, &r, 20000));
		conv_core_stream_release(fx.c, sid, r.ring_slot);
		long pid = 0;
		REQUIRE(wait_until([&] { return (pid = host_pid(fx)) > 0; }, 10000));
		REQUIRE(kill_hard(pid));
		REQUIRE(wait_until([&] { return fx.sup().state == CONV_SUP_FAILED; }, 10000));
		const uint32_t spawns = fx.sup().spawns;

		uint64_t t0 = now_ns();
		CHECK(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
		const double close_ms = (double)(now_ns() - t0) / 1e6;
		t0 = now_ns();
		conv_core_evict_task(fx.c, DEPTH);
		const double evict_ms = (double)(now_ns() - t0) / 1e6;
		sleep_ms(300);
		CHECK(fx.sup().spawns == spawns);
		t0 = now_ns();
		conv_core_destroy(&fx.c);
		const double destroy_ms = (double)(now_ns() - t0) / 1e6;
		std::printf("FAILED host: stream close %.3f ms, evict %.3f ms, core teardown %.3f ms\n", close_ms, evict_ms,
		            destroy_ms);
		CHECK(close_ms < 100.0);
		CHECK(evict_ms < 100.0);
		CHECK(destroy_ms < 2000.0);
	}
}

TEST_CASE("proxy: a kill during bring-up: the next host comes up, no exit-30 kill", "[conv_proxy][conv_kill]")
{
	// Every DESCRIBE reply takes 1.5 s: the bring-up (hello accepted, nothing
	// described yet) is a window to kill the host in.
	Fixture fx({"--fake-describe-ms", "1500"});
	long pid = 0;
	REQUIRE(wait_until(
	    [&] {
		    pid = host_pid(fx);
		    return pid > 0 && fx.sup().state == CONV_SUP_RUNNING;
	    },
	    40000));
	REQUIRE(fx.row_state() == XRT_CONV_STATE_UNKNOWN); // still in bring-up
	REQUIRE(kill_hard(pid));
	REQUIRE(wait_until([&] { return fx.sup().restarts >= 1u; }, 20000));
	const int32_t first_exit = fx.sup().last_exit;

	// The next incarnation (as slow) reaches RUNNING and describes.
	REQUIRE(wait_until([&] { return fx.row_state() != XRT_CONV_STATE_UNKNOWN; }, 60000));
	const conv_supervisor_status st = fx.sup();
	std::printf("bring-up kill: first exit %d, then state %s, spawns %u, restarts %u, last exit %d\n",
	            (int)first_exit, conv_supervisor_state_str(st.state), st.spawns, st.restarts, st.last_exit);
	CHECK(st.state == CONV_SUP_RUNNING);
	CHECK(st.spawns == 2u);   // no extra kill / respawn
	CHECK(st.restarts == 1u); // the one kill, nothing more
#ifdef _WIN32
	CHECK((uint32_t)first_exit == 0xC0000005u); // the real code, not our 30
#else
	CHECK(first_exit == OS_PROCESS_EXIT_SIGNAL_BASE + 9);
#endif
}

TEST_CASE("proxy: a host's own idle exit is orderly: no crash accounting, no respawn without demand",
          "[conv_proxy][conv_kill]")
{
	// The host closes its channel before its exit 0 lands: the proxy waits
	// for the real exit code instead of killing it (exit 30), and an exit 0
	// with nothing demanding the host is its own idle exit -- orderly, no
	// crash accounting, no respawn until a warm / create needs it.
	real_host_env("0");
	Fixture fx({"--idle-exit-ms", "1500"}, 0, 5, CONV_REAL_HOST_EXE, "sim-conversion", SIM_CONVERSION_PATH);
	const uint64_t sid = fx.stream(DEPTH, DEPTH);
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x52, &r, 20000));
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	conv_core_evict_task(fx.c, DEPTH);
	REQUIRE(wait_until([&] { return fx.sup().last_exit != INT32_MIN; }, 20000));
	sleep_ms(1000);
	const conv_supervisor_status st = fx.sup();
	std::printf("idle exit seen as exit %d, state %s, restarts %u, spawns %u\n", st.last_exit,
	            conv_supervisor_state_str(st.state), st.restarts, st.spawns);
	CHECK(st.last_exit == CONV_HOST_EXIT_ORDERLY);
	CHECK(st.restarts == 0u);
	CHECK(st.state == CONV_SUP_IDLE);
	CHECK(st.spawns == 1u);
}

TEST_CASE("proxy: update flow on the real host (Restart Manager on Windows, SIGTERM on POSIX)", "[conv_proxy]")
{
	// #2056 (roadmap §3.6): the shipping displayxr-conversion-host with the sim
	// provider DLL, closed the way a vendor installer closes it.
	char id[48];
	std::snprintf(id, sizeof(id), "sim-conv-upd-%lu", (unsigned long)os_process_current_pid());
	char env_enable[96];
	std::snprintf(env_enable, sizeof(env_enable), "DXR_CONV_%s_ENABLE", id);
	for (char *c = env_enable; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)std::toupper((unsigned char)*c);
	}
	std::string env_latency(env_enable);
	env_latency.replace(env_latency.size() - std::strlen("ENABLE"), std::strlen("ENABLE"), "LATENCY_MS");
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1");
	set_env(env_enable, "1");
	set_env(env_latency.c_str(), "0");

#ifdef _WIN32
	const bool fake_mutex = false; // the real Global\ mutex
	// Restart Manager closes EVERY process mapping a registered file, and ctest
	// runs other tests on the same host binary in parallel: register private
	// copies (beside the originals, so the host still finds the runtime DLL).
	auto unique_copy = [&](const char *src, const char *ext) {
		std::string dst(src);
		dst.replace(dst.size() - std::strlen(ext), std::strlen(ext), std::string("-rmtest-") + id + ext);
		REQUIRE(CopyFileA(src, dst.c_str(), FALSE));
		return dst;
	};
	const std::string host_copy = unique_copy(CONV_REAL_HOST_EXE, ".exe");
	const std::string dll_copy = unique_copy(SIM_CONVERSION_PATH, ".dll");
	const char *host_exe = host_copy.c_str();
	const char *provider_dll = dll_copy.c_str();
#else
	const bool fake_mutex = true; // POSIX has no installer mutex
	g_maint_held = false;
	const char *host_exe = CONV_REAL_HOST_EXE;
	const char *provider_dll = SIM_CONVERSION_PATH;
#endif
	{
	Fixture fx({}, 0, 5, host_exe, id, provider_dll, true, nullptr, fake_mutex);
	const uint32_t cap = XRT_CONV_TASK_DEPTH_MONO;
	const uint64_t sid = fx.stream(XRT_CONV_TASK_DEPTH_MONO, cap);
	conv_result r{};
	REQUIRE(fx.convert(sid, 0x5A, &r, 20000));
	REQUIRE((r.written & XRT_CONV_OUT_DEPTH) != 0u);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	const uint32_t spawns = fx.sup().spawns;
	conv_supervisor_status sst{};
	long host_pid = 0;
	conv_host_proxy_get_status(fx.host, &sst, &host_pid);
	REQUIRE(host_pid > 0);

#ifdef _WIN32
	// 1. The installer takes the maintenance mutex.
	char mname[160];
	conv_host_proxy_maintenance_mutex_name(id, mname, sizeof(mname));
	wchar_t wmname[160];
	REQUIRE(MultiByteToWideChar(CP_UTF8, 0, mname, -1, wmname, 160) > 0);
	HANDLE mutex = CreateMutexW(nullptr, TRUE, wmname);
	REQUIRE(mutex != nullptr);
	REQUIRE(GetLastError() != ERROR_ALREADY_EXISTS);

	// 2. A Restart Manager session on the host binary + the provider DLL.
	auto widen_path = [](const char *u) {
		std::wstring w(MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, u, -1, &w[0], (int)w.size());
		w.resize(w.size() - 1);
		for (auto &ch : w) {
			ch = ch == L'/' ? L'\\' : ch;
		}
		return w;
	};
	const std::wstring host_file = widen_path(host_exe);
	const std::wstring dll_file = widen_path(provider_dll);
	DWORD session = 0;
	WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
	REQUIRE(RmStartSession(&session, 0, key) == ERROR_SUCCESS);
	LPCWSTR files[2] = {host_file.c_str(), dll_file.c_str()};
	REQUIRE(RmRegisterResources(session, 2, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS);

	// 3. RmGetList lists the host, never the service (here: this process).
	std::vector<RM_PROCESS_INFO> procs(8);
	UINT needed = 0, count = (UINT)procs.size();
	DWORD reasons = 0;
	DWORD gl = RmGetList(session, &needed, &count, procs.data(), &reasons);
	if (gl == ERROR_MORE_DATA) {
		procs.resize(needed);
		count = needed;
		gl = RmGetList(session, &needed, &count, procs.data(), &reasons);
	}
	REQUIRE(gl == ERROR_SUCCESS);
	bool host_listed = false, self_listed = false;
	for (UINT i = 0; i < count; i++) {
		const DWORD pid = procs[i].Process.dwProcessId;
		UNSCOPED_INFO("RmGetList: pid " << pid << " type " << (int)procs[i].ApplicationType << " restartable "
		                                << (int)procs[i].bRestartable);
		host_listed = host_listed || pid == (DWORD)host_pid;
		self_listed = self_listed || pid == GetCurrentProcessId();
	}
	CHECK(host_listed);
	CHECK_FALSE(self_listed);
	CHECK(reasons == RmRebootReasonNone);
	for (UINT i = 0; i < count; i++) {
		if (procs[i].Process.dwProcessId == (DWORD)host_pid) {
			CHECK(procs[i].ApplicationType == RmOtherWindow); // the hidden session-end window
			CHECK_FALSE(procs[i].bRestartable);               // never RegisterApplicationRestart
		}
	}

	// 4. RmShutdown: the host answers WM_QUERYENDSESSION(ENDSESSION_CLOSEAPP) and exits 20.
	CHECK(RmShutdown(session, 0, nullptr) == ERROR_SUCCESS);
#else
	// The POSIX stand-in for Restart Manager's close request, under what an
	// installer's maintenance lock would be.
	g_maint_held = true;
	REQUIRE(kill((pid_t)host_pid, SIGTERM) == 0);
#endif

	REQUIRE(wait_until([&] { return fx.sup().last_exit == CONV_HOST_EXIT_UPDATE; }, 15000));
	REQUIRE(wait_until([&] { return fx.row_reason(cap) == CONV_REASON_UPDATING; }, 5000));
	CHECK(fx.row_state(cap) == XRT_CONV_STATE_WARMING);
	CHECK(fx.sup().state == CONV_SUP_UPDATING);
	CHECK(fx.sup().restarts == 0u);

	// The stream keeps its handle; nothing respawns while the mutex is held.
	for (int i = 0; i < 30; i++) {
		CHECK(fx.submit(sid, 0x5B));
		sleep_ms(50);
	}
	CHECK(fx.sup().spawns == spawns);
	CHECK(fx.sup().state == CONV_SUP_UPDATING);
	conv_stream_stats st0{};
	CHECK(conv_core_stream_get_stats(fx.c, sid, &st0) == XRT_SUCCESS);

	// A job submitted during the update window waits (no thread blocks on the
	// host) and runs after the respawn (§7.4).
	std::vector<uint8_t> job_px(32 * 16 * 4, 0x6B);
	conv_job_create_info ji{};
	ji.task = XRT_CONV_TASK_SPLATS_MONO;
	ji.input_kind = XRT_CONV_INPUT_MONO;
	ji.image_count = 1;
	ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
	ji.images[0].cpu_ptr = job_px.data();
	ji.images[0].byte_size = job_px.size();
	ji.images[0].width = 32;
	ji.images[0].height = 16;
	ji.images[0].row_stride = 32 * 4;
	ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
	ji.params.struct_size = sizeof(ji.params);
	uint64_t job = 0;
	REQUIRE(wait_until([&] { return conv_core_job_submit(fx.c, 1, &ji, &job) == XRT_SUCCESS; }, 5000));
	conv_job_status js{};
	sleep_ms(500);
	conv_core_job_status(fx.c, job, &js);
	CHECK(js.status == CONV_JOB_WAITING_FOR_PROVIDER);
	CHECK(fx.sup().spawns == spawns); // the job is demand, but the mutex is held

	// 5. The installer replaces its files (nothing to do here), ends the
	// session and releases the mutex: the host respawns and converts again.
#ifdef _WIN32
	CHECK(RmEndSession(session) == ERROR_SUCCESS);
	CHECK(ReleaseMutex(mutex));
	CloseHandle(mutex);
#else
	g_maint_held = false;
#endif
	REQUIRE(fx.convert(sid, 0x5C, &r, 40000));
	REQUIRE((r.written & XRT_CONV_OUT_DEPTH) != 0u);
	CHECK(fx.stats(sid).reopens >= 1u);
	conv_core_stream_release(fx.c, sid, r.ring_slot);
	// >= : on a loaded box a real host start can overrun the channel-accept
	// timeout and be retried (a crash-path restart AFTER the update, which is
	// not what this test is about; the update itself was checked above).
	CHECK(fx.sup().spawns >= spawns + 1u);
	CHECK(fx.row_state(cap) == XRT_CONV_STATE_READY);
	long new_pid = 0;
	conv_host_proxy_get_status(fx.host, &sst, &new_pid);
	CHECK(new_pid != host_pid);
	REQUIRE(wait_until(
	    [&] {
		    conv_core_job_status(fx.c, job, &js);
		    return js.status == CONV_JOB_SUCCEEDED || js.status == CONV_JOB_FAILED;
	    },
	    30000));
	CHECK(js.status == CONV_JOB_SUCCEEDED);
	conv_core_job_destroy(fx.c, job);
	REQUIRE(conv_core_stream_destroy(fx.c, sid) == XRT_SUCCESS);
	} // the proxy ends its host here
#ifdef _WIN32
	(void)DeleteFileA(host_copy.c_str());
	(void)DeleteFileA(dll_copy.c_str());
#endif
}

namespace {

void
kill_pid(long pid)
{
#ifdef _WIN32
	HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
	REQUIRE(h != nullptr);
	CHECK(TerminateProcess(h, 3));
	CloseHandle(h);
#else
	CHECK(kill((pid_t)pid, SIGKILL) == 0);
#endif
}

} // namespace

TEST_CASE("proxy: a job whose host dies mid-run is retried once; a second loss fails it", "[conv_proxy]")
{
	// #2056: same job id and handle; partial output discarded; WAITING_FOR_PROVIDER in between.
	char id[48];
	std::snprintf(id, sizeof(id), "sim-conv-job-%lu", (unsigned long)os_process_current_pid());
	char env[96];
	std::snprintf(env, sizeof(env), "DXR_CONV_%s_", id);
	for (char *c = env; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)std::toupper((unsigned char)*c);
	}
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1");
	set_env((std::string(env) + "ENABLE").c_str(), "1");
	set_env((std::string(env) + "LATENCY_MS").c_str(), "0");
	g_maint_held = false;
	Fixture fx({}, 0, 5, CONV_REAL_HOST_EXE, id, SIM_CONVERSION_PATH, true, nullptr, true);
	REQUIRE(wait_until([&] { return fx.row_state(XRT_CONV_TASK_SPLATS_MONO) != XRT_CONV_STATE_UNKNOWN; }, 40000));

	std::vector<uint8_t> px(32 * 16 * 4, 0x6B);
	conv_job_create_info ji{};
	ji.task = XRT_CONV_TASK_SPLATS_MONO;
	ji.input_kind = XRT_CONV_INPUT_MONO;
	ji.image_count = 1;
	ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
	ji.images[0].cpu_ptr = px.data();
	ji.images[0].byte_size = px.size();
	ji.images[0].width = 32;
	ji.images[0].height = 16;
	ji.images[0].row_stride = 32 * 4;
	ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
	ji.params.struct_size = sizeof(ji.params);
	uint64_t job = 0;
	REQUIRE(wait_until([&] { return conv_core_job_submit(fx.c, 1, &ji, &job) == XRT_SUCCESS; }, 10000));
	conv_job_status js{};
	auto running_on_new_host = [&](long not_pid, long *out_pid) {
		return wait_until(
		    [&] {
			    conv_core_job_status(fx.c, job, &js);
			    conv_supervisor_status st{};
			    long pid = 0;
			    conv_host_proxy_get_status(fx.host, &st, &pid);
			    *out_pid = pid;
			    return js.status == CONV_JOB_RUNNING && pid > 0 && pid != not_pid;
		    },
		    40000);
	};
	long pid1 = 0;
	REQUIRE(running_on_new_host(0, &pid1));
	sleep_ms(300); // well inside the sim's 2 s job
	kill_pid(pid1);

	SECTION("one loss: retried on the respawned host, same job, completes")
	{
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(fx.c, job, &js);
			    return js.status == CONV_JOB_SUCCEEDED || js.status == CONV_JOB_FAILED;
		    },
		    40000));
		CHECK(js.status == CONV_JOB_SUCCEEDED);
		CHECK(js.job_id == job);
		std::vector<uint8_t> out(1u << 20);
		uint64_t n = 0, total = 0;
		REQUIRE(conv_core_job_read(fx.c, job, 0, 0, out.data(), out.size(), &n, &total) == XRT_SUCCESS);
		CHECK(n > 0u);
		CHECK(n == total);
	}
	SECTION("two losses: FAILED with the crash reason")
	{
		long pid2 = 0;
		REQUIRE(running_on_new_host(pid1, &pid2));
		sleep_ms(300);
		kill_pid(pid2);
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(fx.c, job, &js);
			    return js.status == CONV_JOB_SUCCEEDED || js.status == CONV_JOB_FAILED;
		    },
		    40000));
		CHECK(js.status == CONV_JOB_FAILED);
		CHECK(js.failure_reason == XRT_CONV_FAIL_CRASH_LOOP);
	}
	conv_core_job_destroy(fx.c, job);
}

#endif
