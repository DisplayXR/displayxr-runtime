// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The out-of-process viewer-tracking host end to end
 *         (tracking-provider-discovery.md §4a): this test plays the service —
 *         it creates the viewer pose page, runs `displayxr-tracking-host
 *         --probe` (the real sim_tracking plug-in through manifest discovery,
 *         the provenance gate under DXR_ALLOW_DEV_PLUGIN_PATHS), then the
 *         supervisor spawns `--serve` on demand; the test reads the scripted
 *         sweep off the page from its own process, kills the host, watches
 *         the page go stale, the supervisor restart it with a new generation
 *         and the samples resume, and finally stops it orderly.
 *
 * POSIX (manifest discovery via XRT_PLUGIN_SEARCH_PATH); Windows discovery
 * is registry-only and is covered by the CI conformance step.
 */

#include "catch_amalgamated.hpp"

#include "target_viewer_host.h"
#include "target_viewer_tracking.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_viewer_tracking.h"

#include "os/os_process.h"
#include "os/os_shm.h"
#include "os/os_time.h"
#include "util/u_viewer_pose_page.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <signal.h>
#include <thread>
#include <vector>

#ifndef TRACKING_HOST_EXE
#error "TRACKING_HOST_EXE must be defined by the build"
#endif

namespace {

constexpr int64_t kMs = 1000ll * 1000ll;

void
sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

//! Drain the page for @p ms, returning every sample read.
std::vector<xrt_tracking_viewer_sample>
drain_for(void *page, u_vpp_reader &r, int ms)
{
	std::vector<xrt_tracking_viewer_sample> all;
	xrt_tracking_viewer_sample buf[U_VPP_RING_SIZE];
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		const uint32_t n = u_vpp_read_new(page, &r, buf, U_VPP_RING_SIZE);
		all.insert(all.end(), buf, buf + n);
		sleep_ms(20);
	}
	return all;
}

//! Wait until @p pred or @p ms elapsed.
template <typename F>
bool
wait_for(F pred, int ms)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		if (pred()) {
			return true;
		}
		sleep_ms(25);
	}
	return pred();
}

} // namespace

TEST_CASE("tracking host: probe, serve on demand, crash restart, orderly stop", "[tracking_host]")
{
	char name[64];
	std::snprintf(name, sizeof(name), "/dxr-vpth-%ld", os_process_current_pid());
	os_shm shm;
	REQUIRE(os_shm_create(name, U_VPP_PAGE_BYTES, &shm) == 0);
	REQUIRE(u_vpp_init(shm.map, shm.size, os_process_current_pid(), os_monotonic_get_ns()));

	// 1. Probe: the host scans the class (this process loads nothing).
	char id[64] = {0};
	bool ran = false;
	const bool usable = target_viewer_host_probe(TRACKING_HOST_EXE, name, shm.map, -1, 10000, id, sizeof(id), &ran);
	REQUIRE(ran);
	REQUIRE(usable);
	CHECK(std::string(id) == "sim-tracking");
	u_vpp_producer_status ps;
	REQUIRE(u_vpp_read_producer(shm.map, &ps));
	CHECK(ps.pid == 0); // a probe never produces
	CHECK((ps.tracks & XRT_TRACKING_TRACKS_EYE_PAIR) != 0);
	INFO("source: " << ps.source);

	// 2. The manager: nothing runs until demand.
	target_vhost_sup_config sup;
	target_vhost_sup_config_defaults(&sup);
	sup.backoff_initial_ns = 200 * kMs;
	target_viewer_host_config cfg = {};
	cfg.host_exe = TRACKING_HOST_EXE;
	cfg.page_name = name;
	cfg.page_mem = shm.map;
	cfg.provider_id = id;
	cfg.geometry.struct_size = sizeof(cfg.geometry);
	cfg.geometry.display_width_m = 0.60f;
	cfg.geometry.display_height_m = 0.34f;
	cfg.geometry.nominal_viewer_z_m = 0.60f;
	cfg.have_geometry = true;
	cfg.trust_enforce = -1;
	cfg.sup = &sup;
	target_viewer_host *host = target_viewer_host_create(&cfg);
	REQUIRE(host != nullptr);

	sleep_ms(300);
	target_viewer_host_status hs;
	target_viewer_host_get_status(host, &hs);
	CHECK(hs.state == TARGET_VHOST_SUP_IDLE);
	CHECK(hs.pid == 0);
	CHECK(u_vpp_read_write_count(shm.map) == 0);

	// A READER's demand (posted on the page, as an in-process runtime beside
	// the service does) is enough to start it.
	u_vpp_post_demand(shm.map, os_monotonic_get_ns());
	REQUIRE(wait_for(
	    [&] {
		    target_viewer_host_get_status(host, &hs);
		    return hs.state == TARGET_VHOST_SUP_RUNNING;
	    },
	    10000));
	const int64_t pid1 = hs.pid;
	REQUIRE(pid1 > 0);
	CHECK(pid1 != os_process_current_pid());

	// 3. The scripted sweep arrives on the page, in order, moving.
	u_vpp_reader r = {};
	auto s1 = drain_for(shm.map, r, 600);
	REQUIRE(s1.size() >= 10);
	float minx = 1e9f, maxx = -1e9f;
	for (size_t i = 0; i < s1.size(); i++) {
		CHECK((s1[i].flags & XRT_TRACKING_SAMPLE_EYES_VALID) != 0);
		if (i > 0) {
			CHECK(s1[i].capture_ns > s1[i - 1].capture_ns);
		}
		const float mid = 0.5f * (s1[i].left_eye[0] + s1[i].right_eye[0]);
		minx = std::min(minx, mid);
		maxx = std::max(maxx, mid);
	}
	CHECK(maxx - minx > 0.005f); // the head sweeps (100 ms period below, amplitude 0.1 m)
	CHECK(s1.back().right_eye[0] - s1.back().left_eye[0] == Catch::Approx(0.063f).margin(0.002f));
	REQUIRE(u_vpp_read_producer(shm.map, &ps));
	CHECK(ps.pid == pid1);
	CHECK(ps.generation == 1);
	CHECK(ps.provider_status == XRT_TRACKING_PROVIDER_STATUS_RUNNING);
	u_vpp_owner_status os;
	REQUIRE(u_vpp_read_owner(shm.map, &os));
	CHECK(os.state == U_VPP_STATE_RUNNING);
	CHECK(os.host_pid == pid1);
	CHECK(std::string(os.provider_id) == "sim-tracking");
	CHECK(os_monotonic_get_ns() - u_vpp_read_heartbeat(shm.map) < U_VPP_HEARTBEAT_STALE_NS);

	// 4. Crash: kill the host. The page goes stale (no samples, the
	// producer pid gone); the supervisor restarts it after the backoff.
	REQUIRE(kill((pid_t)pid1, SIGKILL) == 0);
	REQUIRE(wait_for([&] { return !os_process_is_alive((long)pid1); }, 3000));
	const uint64_t count_at_kill = u_vpp_read_write_count(shm.map);
	REQUIRE(wait_for(
	    [&] {
		    target_viewer_host_get_status(host, &hs);
		    return hs.restarts == 1 && hs.state == TARGET_VHOST_SUP_RUNNING && hs.pid != pid1 && hs.pid > 0;
	    },
	    15000));
	const int64_t pid2 = hs.pid;
	REQUIRE(wait_for(
	    [&] {
		    return u_vpp_read_producer(shm.map, &ps) && ps.pid == pid2 && ps.generation == 2 &&
		           u_vpp_read_write_count(shm.map) > count_at_kill + 5;
	    },
	    5000));
	// The same reader cursor keeps working across the restart (the count continued).
	auto s2 = drain_for(shm.map, r, 300);
	CHECK(s2.size() >= 5);
	REQUIRE(u_vpp_read_owner(shm.map, &os));
	CHECK(os.restarts == 1);
	CHECK(os.generation == 2);

	// 5. Orderly stop: the host stops its provider and exits 0; the page says STOPPED.
	target_viewer_host_destroy(&host);
	CHECK(host == nullptr);
	CHECK(wait_for([&] { return !os_process_is_alive((long)pid2); }, 3000));
	REQUIRE(u_vpp_read_owner(shm.map, &os));
	CHECK(os.state == U_VPP_STATE_STOPPED);
	REQUIRE(u_vpp_read_producer(shm.map, &ps));
	CHECK(ps.provider_status == XRT_TRACKING_PROVIDER_STATUS_STOPPED);

	os_shm_close(&shm);
}

TEST_CASE("tracking host: an unknown provider exits no_provider (terminal)", "[tracking_host]")
{
	char name[64];
	std::snprintf(name, sizeof(name), "/dxr-vpth2-%ld", os_process_current_pid());
	os_shm shm;
	REQUIRE(os_shm_create(name, U_VPP_PAGE_BYTES, &shm) == 0);
	REQUIRE(u_vpp_init(shm.map, shm.size, os_process_current_pid(), os_monotonic_get_ns()));
	target_viewer_host_config cfg = {};
	cfg.host_exe = TRACKING_HOST_EXE;
	cfg.page_name = name;
	cfg.page_mem = shm.map;
	cfg.provider_id = "no-such-provider";
	cfg.trust_enforce = -1;
	target_viewer_host *host = target_viewer_host_create(&cfg);
	REQUIRE(host != nullptr);
	target_viewer_host_demand(host);
	target_viewer_host_status hs;
	REQUIRE(wait_for(
	    [&] {
		    target_viewer_host_get_status(host, &hs);
		    return hs.state == TARGET_VHOST_SUP_FAILED;
	    },
	    10000));
	CHECK(hs.last_exit == TARGET_VHOST_EXIT_NO_PROVIDER);
	CHECK(hs.restarts == 0);
	target_viewer_host_destroy(&host);
	os_shm_close(&shm);
}

TEST_CASE("tracking host: a missing page is refused", "[tracking_host]")
{
	char id[64];
	bool ran = false;
	char dummy[U_VPP_PAGE_BYTES] = {};
	CHECK_FALSE(
	    target_viewer_host_probe(TRACKING_HOST_EXE, "/dxr-vpth-none", dummy, -1, 10000, id, sizeof(id), &ran));
	CHECK(ran);
}

namespace {

uint32_t
external_ok(struct xrt_plugin_instance *, const struct xrt_display_descriptor *)
{
	return XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK;
}

} // namespace

TEST_CASE("tracking host: an in-process runtime reads the page instead of loading a provider",
          "[tracking_host][reader]")
{
	// The service side: page + supervised host (nothing spawned yet).
	char name[64];
	std::snprintf(name, sizeof(name), "/dxr-vpth3-%ld", os_process_current_pid());
	os_shm shm;
	REQUIRE(os_shm_create(name, U_VPP_PAGE_BYTES, &shm) == 0);
	REQUIRE(u_vpp_init(shm.map, shm.size, os_process_current_pid(), os_monotonic_get_ns()));
	target_vhost_sup_config sup;
	target_vhost_sup_config_defaults(&sup);
	sup.backoff_initial_ns = 3000 * kMs; // long enough to watch the stale window
	target_viewer_host_config cfg = {};
	cfg.host_exe = TRACKING_HOST_EXE;
	cfg.page_name = name;
	cfg.page_mem = shm.map;
	cfg.provider_id = "sim-tracking";
	cfg.trust_enforce = -1;
	cfg.sup = &sup;
	target_viewer_host *host = target_viewer_host_create(&cfg);
	REQUIRE(host != nullptr);

	// The in-process runtime side: the glue an app's runtime runs at system
	// creation, for a display that does not track and is EXTERNAL_OK.
	setenv("DXR_VIEWER_POSE_PAGE", name, 1);
	xrt_plugin_iface iface = {};
	iface.struct_size = (uint32_t)sizeof(iface);
	iface.get_viewer_policy = external_ok;
	xrt_plugin_display_info pdi = {};
	pdi.struct_size = sizeof(pdi);
	pdi.display_width_m = 0.6f;
	pdi.display_height_m = 0.34f;
	pdi.nominal_viewer_z_m = 0.6f;
	auto *info = new xrt_system_compositor_info();
	target_viewer_tracking_apply(info, nullptr, &iface, &pdi);
	target_viewer_decision d;
	REQUIRE(target_viewer_tracking_get_decision(&d));
	CHECK(d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(std::string(d.provider_id) == "sim-tracking");
	CHECK(info->supported_eye_tracking_modes == (TARGET_VIEWER_MODE_MANAGED_BIT | TARGET_VIEWER_MODE_MANUAL_BIT));
	target_viewer_hosting_info hi;
	target_viewer_tracking_get_hosting(&hi);
	CHECK(std::string(hi.hosting) == "page");

	// The first viewer request posts demand; the service's supervisor spawns the host.
	xrt_viewer_tracking *vt = target_viewer_tracking_get_iface();
	xrt_eye_positions e = {};
	REQUIRE(wait_for(
	    [&] {
		    e = {};
		    return vt->get_eyes_at(vt, XRT_VIEWER_SCREEN_PRIMARY, os_monotonic_get_ns(), &e) && e.is_tracking;
	    },
	    10000));
	CHECK(e.count == 2);
	CHECK(e.eyes[1].x - e.eyes[0].x == Catch::Approx(0.063f).margin(0.002f));
	// The owner block follows the producer's hello within one supervisor tick.
	CHECK(wait_for(
	    [&] {
		    target_viewer_tracking_get_hosting(&hi);
		    return std::string(hi.host_state) == "running";
	    },
	    2000));
	CHECK(hi.host_pid > 0);
	CHECK(target_viewer_tracking_provider_state(&d) == TARGET_VIEWER_PROVIDER_SERVING);

	// The host dies: the reader goes "last known, not tracking" — never zeros.
	const long pid = (long)hi.host_pid;
	REQUIRE(kill((pid_t)pid, SIGKILL) == 0);
	REQUIRE(wait_for(
	    [&] {
		    e = {};
		    return vt->get_eyes_at(vt, XRT_VIEWER_SCREEN_PRIMARY, os_monotonic_get_ns(), &e) && !e.is_tracking;
	    },
	    2500));
	CHECK(e.valid);
	CHECK(e.eyes[1].x - e.eyes[0].x == Catch::Approx(0.063f).margin(0.002f));
	CHECK(target_viewer_tracking_provider_state(&d) == TARGET_VIEWER_PROVIDER_PAUSED);

	// ... and tracking again once the supervisor restarted it.
	REQUIRE(wait_for(
	    [&] {
		    e = {};
		    return vt->get_eyes_at(vt, XRT_VIEWER_SCREEN_PRIMARY, os_monotonic_get_ns(), &e) && e.is_tracking;
	    },
	    15000));
	target_viewer_tracking_get_hosting(&hi);
	CHECK(hi.restarts == 1);

	target_viewer_tracking_shutdown();
	target_viewer_host_destroy(&host);
	os_shm_close(&shm);
	unsetenv("DXR_VIEWER_POSE_PAGE");
	delete info;
}
