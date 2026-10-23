// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The viewer pose page (ADR-054 N8, tracking-provider-discovery.md
 *         §4a): layout, validation, the per-slot seqlock under a two-thread
 *         torn-read stress, wrap / lapped samples, status blocks, stale
 *         detection inputs; the named shm it lives in; the host supervisor's
 *         pure state machine (backoff, crash loop, terminal exits).
 */

#include "catch_amalgamated.hpp"

#include "util/u_eye_history.h"
#include "util/u_viewer_pose_page.h"

#include "target_viewer_host.h"

#include "xrt/xrt_config_os.h"

#if !defined(XRT_OS_ANDROID)
#include "os/os_process.h"
#include "os/os_shm.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int64_t kMs = 1000ll * 1000ll;

struct Page
{
	alignas(64) uint8_t mem[U_VPP_PAGE_BYTES];
};

xrt_tracking_viewer_sample
sample(uint64_t k, int64_t capture_ns)
{
	xrt_tracking_viewer_sample s = {};
	s.struct_size = sizeof(s);
	s.flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_TRACKING;
	s.capture_ns = capture_ns;
	// Every float encodes k, so a torn copy (half old, half new) is detectable.
	const float v = (float)k;
	for (int i = 0; i < 3; i++) {
		s.left_eye[i] = v;
		s.right_eye[i] = v;
		s.head_position[i] = v;
	}
	for (int i = 0; i < 4; i++) {
		s.head_orientation[i] = v;
	}
	s.confidence = v;
	s.screen_hint = (uint32_t)k;
	std::snprintf(s.camera.camera_key, sizeof(s.camera.camera_key), "cam-%llu", (unsigned long long)k);
	return s;
}

bool
consistent(const xrt_tracking_viewer_sample &s)
{
	const float v = s.left_eye[0];
	for (int i = 0; i < 3; i++) {
		if (s.left_eye[i] != v || s.right_eye[i] != v || s.head_position[i] != v) {
			return false;
		}
	}
	for (int i = 0; i < 4; i++) {
		if (s.head_orientation[i] != v) {
			return false;
		}
	}
	char key[XRT_TRACKING_CAMERA_KEY_MAX];
	std::snprintf(key, sizeof(key), "cam-%llu", (unsigned long long)v);
	return s.confidence == v && s.screen_hint == (uint32_t)v && std::strcmp(key, s.camera.camera_key) == 0 &&
	       s.capture_ns == (int64_t)v * kMs;
}

} // namespace

TEST_CASE("viewer pose page: init, validate, write, read in order", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1234, 99));
	REQUIRE(u_vpp_attach_check(p.mem, sizeof(p.mem)));

	u_vpp_geometry g;
	u_vpp_read_geometry(p.mem, &g);
	CHECK(g.magic == U_VPP_MAGIC);
	CHECK(g.version == U_VPP_VERSION);
	CHECK(g.ring_size == U_VPP_RING_SIZE);
	CHECK(g.owner_pid == 1234);
	CHECK(g.owner_created_ns == 99);
	CHECK(g.sample_size == sizeof(xrt_tracking_viewer_sample));

	u_vpp_producer prod = {};
	u_vpp_producer_begin(&prod, p.mem);
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[U_VPP_RING_SIZE];
	CHECK(u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE) == 0);

	for (uint64_t k = 1; k <= 5; k++) {
		const auto s = sample(k, (int64_t)k * kMs);
		u_vpp_write_sample(&prod, &s);
	}
	CHECK(u_vpp_read_write_count(p.mem) == 5);
	const uint32_t n = u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE);
	REQUIRE(n == 5);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(consistent(out[i]));
		CHECK(out[i].left_eye[0] == (float)(i + 1));
	}
	// Nothing new: nothing returned.
	CHECK(u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE) == 0);
	CHECK(r.dropped == 0);
}

TEST_CASE("viewer pose page: rejects a bad geometry", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	CHECK_FALSE(u_vpp_attach_check(p.mem, U_VPP_HEADER_BYTES)); // mapping too small
	Page q = p;
	uint32_t bad = U_VPP_VERSION + 1;
	std::memcpy(q.mem + 4, &bad, sizeof(bad));
	CHECK_FALSE(u_vpp_attach_check(q.mem, sizeof(q.mem)));
	q = p;
	bad = 0;
	std::memcpy(q.mem + 0, &bad, sizeof(bad)); // no magic (an init in progress)
	CHECK_FALSE(u_vpp_attach_check(q.mem, sizeof(q.mem)));
	q = p;
	bad = 8; // a slot that cannot hold a sample
	std::memcpy(q.mem + 16, &bad, sizeof(bad));
	CHECK_FALSE(u_vpp_attach_check(q.mem, sizeof(q.mem)));
	CHECK_FALSE(u_vpp_init(p.mem, 100, 1, 1));
}

TEST_CASE("viewer pose page: wrap — a slow reader drops lapped samples, keeps the newest ring", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	u_vpp_producer prod = {};
	u_vpp_producer_begin(&prod, p.mem);
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[U_VPP_RING_SIZE];
	(void)u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE); // start the cursor at 0

	const uint64_t total = 3 * U_VPP_RING_SIZE + 5;
	for (uint64_t k = 1; k <= total; k++) {
		const auto s = sample(k, (int64_t)k * kMs);
		u_vpp_write_sample(&prod, &s);
	}
	const uint32_t n = u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE);
	REQUIRE(n == U_VPP_RING_SIZE);
	CHECK(r.dropped == total - U_VPP_RING_SIZE);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(consistent(out[i]));
		CHECK(out[i].left_eye[0] == (float)(total - U_VPP_RING_SIZE + 1 + i));
	}

	// A reader that first attaches late starts at the oldest kept sample.
	u_vpp_reader late = {};
	CHECK(u_vpp_read_new(p.mem, &late, out, U_VPP_RING_SIZE) == U_VPP_RING_SIZE);
	CHECK(out[0].left_eye[0] == (float)(total - U_VPP_RING_SIZE + 1));

	// A capped drain keeps the newest.
	for (uint64_t k = total + 1; k <= total + 10; k++) {
		const auto s = sample(k, (int64_t)k * kMs);
		u_vpp_write_sample(&prod, &s);
	}
	CHECK(u_vpp_read_new(p.mem, &late, out, 4) == 4);
	CHECK(out[3].left_eye[0] == (float)(total + 10));
}

TEST_CASE("viewer pose page: a restarted producer continues the count", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	u_vpp_producer a = {};
	u_vpp_producer_begin(&a, p.mem);
	for (uint64_t k = 1; k <= 3; k++) {
		const auto s = sample(k, (int64_t)k * kMs);
		u_vpp_write_sample(&a, &s);
	}
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[U_VPP_RING_SIZE];
	CHECK(u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE) == 3);

	// The host crashed and came back: a new producer, same page.
	u_vpp_producer b = {};
	u_vpp_producer_begin(&b, p.mem);
	CHECK(b.next == 3);
	const auto s4 = sample(4, 4 * kMs);
	u_vpp_write_sample(&b, &s4);
	REQUIRE(u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE) == 1);
	CHECK(out[0].left_eye[0] == 4.0f);
	CHECK(r.dropped == 0);

	// A re-initialised page (a new service) resets the count: the reader restarts.
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 2, 2));
	u_vpp_producer c = {};
	u_vpp_producer_begin(&c, p.mem);
	const auto s1 = sample(1, 1 * kMs);
	u_vpp_write_sample(&c, &s1);
	REQUIRE(u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE) == 1);
	CHECK(out[0].left_eye[0] == 1.0f);
}

TEST_CASE("viewer pose page: a short (older-ABI) sample is zero-extended", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	u_vpp_producer prod = {};
	u_vpp_producer_begin(&prod, p.mem);
	auto s = sample(7, 7 * kMs);
	s.struct_size = (uint32_t)offsetof(xrt_tracking_viewer_sample, camera);
	u_vpp_write_sample(&prod, &s);
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[1];
	REQUIRE(u_vpp_read_new(p.mem, &r, out, 1) == 1);
	CHECK(out[0].left_eye[0] == 7.0f);
	CHECK(out[0].camera.camera_key[0] == '\0');
	CHECK(out[0].struct_size == sizeof(xrt_tracking_viewer_sample));
}

TEST_CASE("viewer pose page: status blocks, heartbeat and demand", "[vpp]")
{
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));

	u_vpp_owner_status o = {};
	REQUIRE(u_vpp_read_owner(p.mem, &o));
	CHECK(o.state == U_VPP_STATE_NONE);
	o.state = U_VPP_STATE_RUNNING;
	o.restarts = 2;
	o.generation = 3;
	o.host_pid = 4242;
	std::snprintf(o.provider_id, sizeof(o.provider_id), "sim-tracking");
	u_vpp_write_owner(p.mem, &o);
	u_vpp_owner_status o2;
	REQUIRE(u_vpp_read_owner(p.mem, &o2));
	CHECK(o2.state == U_VPP_STATE_RUNNING);
	CHECK(o2.restarts == 2);
	CHECK(o2.generation == 3);
	CHECK(o2.host_pid == 4242);
	CHECK(std::string(o2.provider_id) == "sim-tracking");
	CHECK(std::string(u_vpp_state_str(o2.state)) == "running");

	u_vpp_producer_status ps = {};
	ps.pid = 4242;
	ps.generation = 3;
	ps.provider_status = XRT_TRACKING_PROVIDER_STATUS_RUNNING;
	ps.tracks = XRT_TRACKING_TRACKS_EYE_PAIR;
	std::snprintf(ps.provider_id, sizeof(ps.provider_id), "sim-tracking");
	std::snprintf(ps.source, sizeof(ps.source), "scripted sweep");
	u_vpp_write_producer(p.mem, &ps);
	u_vpp_producer_status ps2;
	REQUIRE(u_vpp_read_producer(p.mem, &ps2));
	CHECK(ps2.pid == 4242);
	CHECK(std::string(ps2.source) == "scripted sweep");

	// An untrusted producer leaving a string unterminated cannot overrun a reader.
	std::memset(p.mem + 416, 'x', 128);
	REQUIRE(u_vpp_read_producer(p.mem, &ps2));
	CHECK(std::strlen(ps2.source) == U_VPP_TEXT_MAX - 1);

	CHECK(u_vpp_read_heartbeat(p.mem) == 0);
	u_vpp_heartbeat(p.mem, 5 * kMs);
	CHECK(u_vpp_read_heartbeat(p.mem) == 5 * kMs);
	CHECK(u_vpp_read_demand(p.mem) == 0);
	u_vpp_post_demand(p.mem, 6 * kMs);
	CHECK(u_vpp_read_demand(p.mem) == 6 * kMs);
}

TEST_CASE("viewer pose page: stale — no sample for over 500 ms reads as not tracking", "[vpp]")
{
	// The reader's rule end to end: drain the page into a u_eye_history and
	// sample it; a producer that stopped (or died) leaves the newest sample
	// older than the max age, which the ring answers as "last known, not tracking".
	Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	u_vpp_producer prod = {};
	u_vpp_producer_begin(&prod, p.mem);
	for (uint64_t k = 1; k <= 4; k++) {
		auto s = sample(k, (int64_t)k * 16 * kMs);
		u_vpp_write_sample(&prod, &s);
	}
	u_eye_history ring;
	u_eye_history_init(&ring);
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[U_VPP_RING_SIZE];
	const uint32_t n = u_vpp_read_new(p.mem, &r, out, U_VPP_RING_SIZE);
	for (uint32_t i = 0; i < n; i++) {
		xrt_eye_positions e = {};
		e.count = 2;
		e.eyes[0] = {out[i].left_eye[0], out[i].left_eye[1], out[i].left_eye[2]};
		e.eyes[1] = {out[i].right_eye[0], out[i].right_eye[1], out[i].right_eye[2]};
		e.valid = true;
		e.is_tracking = (out[i].flags & XRT_TRACKING_SAMPLE_TRACKING) != 0;
		REQUIRE(u_eye_history_push(&ring, &e, out[i].capture_ns));
	}
	xrt_eye_positions e = {};
	CHECK(u_eye_history_get(&ring, 64 * kMs, &e) == U_EYE_HISTORY_RESULT_EXACT);
	CHECK(e.is_tracking);
	CHECK(u_eye_history_get(&ring, 64 * kMs + 501 * kMs, &e) == U_EYE_HISTORY_RESULT_STALE);
	CHECK_FALSE(e.is_tracking);
	CHECK(e.eyes[0].x == 4.0f); // last known, never zeros
	u_eye_history_destroy(&ring);

	// The producer liveness input: a heartbeat older than the stale bound.
	u_vpp_heartbeat(p.mem, 100 * kMs);
	const int64_t now = 100 * kMs + U_VPP_HEARTBEAT_STALE_NS + 1;
	CHECK(now - u_vpp_read_heartbeat(p.mem) > U_VPP_HEARTBEAT_STALE_NS);
}

TEST_CASE("viewer pose page: torn-read stress, one writer thread, one reader thread", "[vpp][stress]")
{
	static Page p;
	REQUIRE(u_vpp_init(p.mem, sizeof(p.mem), 1, 1));
	std::atomic<bool> done{false};
	const uint64_t total = 200000;

	std::thread writer([&] {
		u_vpp_producer prod = {};
		u_vpp_producer_begin(&prod, p.mem);
		for (uint64_t k = 1; k <= total; k++) {
			const auto s = sample(k, (int64_t)k * kMs);
			u_vpp_write_sample(&prod, &s);
		}
		done.store(true);
	});

	uint64_t got = 0, bad = 0, out_of_order = 0;
	float last = 0.0f;
	u_vpp_reader r = {};
	std::vector<xrt_tracking_viewer_sample> out(U_VPP_RING_SIZE);
	for (;;) {
		const bool finished = done.load();
		const uint32_t n = u_vpp_read_new(p.mem, &r, out.data(), U_VPP_RING_SIZE);
		for (uint32_t i = 0; i < n; i++) {
			if (!consistent(out[i])) {
				bad++;
			}
			if (out[i].left_eye[0] <= last) {
				out_of_order++;
			}
			last = out[i].left_eye[0];
		}
		got += n;
		if (finished && n == 0) {
			break;
		}
	}
	writer.join();
	INFO("got " << got << " dropped " << r.dropped << " torn-retried " << r.torn);
	CHECK(bad == 0);          // never a half-old, half-new sample
	CHECK(out_of_order == 0); // oldest first, never repeated
	CHECK(got + r.dropped == total);
	CHECK(got > 0);
	CHECK(last == (float)total); // the newest always arrives
}

#if !defined(XRT_OS_ANDROID)
TEST_CASE("os_shm: create by name, open read-only and read-write, unlink on close", "[vpp][shm]")
{
	char name[64];
#ifdef XRT_OS_WINDOWS
	std::snprintf(name, sizeof(name), "Local\\dxr-vpp-test-%ld", os_process_current_pid());
#else
	std::snprintf(name, sizeof(name), "/dxr-vpp-t-%ld", os_process_current_pid());
#endif
	os_shm missing;
	CHECK(os_shm_open(name, U_VPP_PAGE_BYTES, false, &missing) == OS_SHM_ERR_NOT_FOUND);

	os_shm owner;
	REQUIRE(os_shm_create(name, U_VPP_PAGE_BYTES, &owner) == 0);
	REQUIRE(u_vpp_init(owner.map, owner.size, os_process_current_pid(), 1));

	os_shm ro, rw;
	REQUIRE(os_shm_open(name, U_VPP_PAGE_BYTES, false, &ro) == 0);
	REQUIRE(os_shm_open(name, U_VPP_PAGE_BYTES, true, &rw) == 0);
	CHECK(u_vpp_attach_check(ro.map, ro.size));
	u_vpp_post_demand(rw.map, 77);
	CHECK(u_vpp_read_demand(owner.map) == 77);

	u_vpp_producer prod = {};
	u_vpp_producer_begin(&prod, rw.map);
	const auto s = sample(1, kMs);
	u_vpp_write_sample(&prod, &s);
	u_vpp_reader r = {};
	xrt_tracking_viewer_sample out[1];
	REQUIRE(u_vpp_read_new(ro.map, &r, out, 1) == 1);
	CHECK(consistent(out[0]));

	os_shm_close(&ro);
	os_shm_close(&rw);
	os_shm_close(&owner);
#ifndef XRT_OS_WINDOWS
	CHECK(os_shm_open(name, U_VPP_PAGE_BYTES, false, &missing) == OS_SHM_ERR_NOT_FOUND);
#endif
	CHECK(os_shm_open(name, U_VPP_PAGE_BYTES * 2, false, &missing) != 0);
}

TEST_CASE("os_process_is_alive: self yes, nonsense no", "[vpp]")
{
	CHECK(os_process_is_alive(os_process_current_pid()));
	CHECK_FALSE(os_process_is_alive(0));
	CHECK_FALSE(os_process_is_alive(-5));
}
#endif


/*
 *
 * Supervisor (pure).
 *
 */

namespace {

constexpr uint64_t kSec = 1000ull * 1000ull * 1000ull;

target_vhost_sup
make_sup()
{
	target_vhost_sup s;
	target_vhost_sup_init(&s, nullptr);
	return s;
}

} // namespace

TEST_CASE("viewer host supervisor: lazy spawn, hello, orderly stop", "[vhost_sup]")
{
	auto s = make_sup();
	CHECK(target_vhost_sup_tick(&s, 0) == TARGET_VHOST_SUP_ACTION_NONE); // no demand, no process
	target_vhost_sup_set_demand(&s, true);
	REQUIRE(target_vhost_sup_tick(&s, 1) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&s, 1, true);
	CHECK(s.state == TARGET_VHOST_SUP_STARTING);
	CHECK(target_vhost_sup_tick(&s, 2) == TARGET_VHOST_SUP_ACTION_NONE);
	target_vhost_sup_hello(&s, 3);
	CHECK(s.state == TARGET_VHOST_SUP_RUNNING);
	CHECK(target_vhost_sup_page_state(s.state) == U_VPP_STATE_RUNNING);
	// Demand latches: a later "no demand" does not stop it.
	target_vhost_sup_set_demand(&s, false);
	CHECK(s.demand);
	CHECK_FALSE(target_vhost_sup_exited(&s, 4, TARGET_VHOST_EXIT_ORDERLY, true));
	CHECK(s.restarts == 0);
}

TEST_CASE("viewer host supervisor: backoff doubles to the cap, resets after a healthy run", "[vhost_sup]")
{
	auto s = make_sup();
	target_vhost_sup_set_demand(&s, true);
	uint64_t now = 0;
	uint64_t expect = 500 * 1000 * 1000ull;
	for (int i = 0; i < 3; i++) {
		REQUIRE(target_vhost_sup_tick(&s, now) == TARGET_VHOST_SUP_ACTION_SPAWN);
		target_vhost_sup_spawned(&s, now, true);
		target_vhost_sup_hello(&s, now);
		now += 2 * kSec;
		CHECK(target_vhost_sup_exited(&s, now, 139, false)); // a crash
		CHECK(s.state == TARGET_VHOST_SUP_BACKOFF);
		CHECK(s.next_spawn_ns == now + expect);
		CHECK(target_vhost_sup_tick(&s, now + expect - 1) == TARGET_VHOST_SUP_ACTION_NONE);
		now += expect;
		expect *= 2;
		now += 30 * kSec; // spread the crashes beyond the crash-loop window
		now += 30 * kSec;
	}
	CHECK(s.restarts == 3);
	// A run longer than healthy_reset resets the backoff to the initial value.
	REQUIRE(target_vhost_sup_tick(&s, now) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&s, now, true);
	target_vhost_sup_hello(&s, now);
	now += 61 * kSec;
	CHECK(target_vhost_sup_exited(&s, now, 1, false));
	CHECK(s.next_spawn_ns == now + 500 * 1000 * 1000ull);

	// The cap.
	auto c = make_sup();
	target_vhost_sup_set_demand(&c, true);
	c.backoff_ns = 20 * kSec;
	REQUIRE(target_vhost_sup_tick(&c, 0) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&c, 0, true);
	CHECK(target_vhost_sup_exited(&c, 1, 1, false));
	CHECK(c.backoff_ns == 30 * kSec);
}

TEST_CASE("viewer host supervisor: crash loop and terminal exits FAIL, retried later", "[vhost_sup]")
{
	auto s = make_sup();
	target_vhost_sup_set_demand(&s, true);
	uint64_t now = 0;
	for (int i = 0; i < 5; i++) {
		if (s.state == TARGET_VHOST_SUP_BACKOFF) {
			now = s.next_spawn_ns;
		}
		REQUIRE(target_vhost_sup_tick(&s, now) == TARGET_VHOST_SUP_ACTION_SPAWN);
		target_vhost_sup_spawned(&s, now, true);
		now += kSec;
		(void)target_vhost_sup_exited(&s, now, 11, false);
	}
	CHECK(s.state == TARGET_VHOST_SUP_FAILED);
	CHECK(target_vhost_sup_page_state(s.state) == U_VPP_STATE_FAILED);
	CHECK(target_vhost_sup_tick(&s, now + 60 * kSec) == TARGET_VHOST_SUP_ACTION_NONE);
	CHECK(target_vhost_sup_tick(&s, now + 600 * kSec) == TARGET_VHOST_SUP_ACTION_SPAWN);

	// The provider is not usable in the host: no respawn (it would say the same).
	auto t = make_sup();
	target_vhost_sup_set_demand(&t, true);
	REQUIRE(target_vhost_sup_tick(&t, 0) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&t, 0, true);
	CHECK_FALSE(target_vhost_sup_exited(&t, 1, TARGET_VHOST_EXIT_NO_PROVIDER, false));
	CHECK(t.state == TARGET_VHOST_SUP_FAILED);
	CHECK(t.restarts == 0);

	// A host that never says hello is killed.
	auto h = make_sup();
	target_vhost_sup_set_demand(&h, true);
	REQUIRE(target_vhost_sup_tick(&h, 0) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&h, 0, true);
	CHECK(target_vhost_sup_tick(&h, 11 * kSec) == TARGET_VHOST_SUP_ACTION_KILL);

	// A failed spawn is abnormal (backoff), and STOPPED never respawns.
	auto f = make_sup();
	target_vhost_sup_set_demand(&f, true);
	REQUIRE(target_vhost_sup_tick(&f, 0) == TARGET_VHOST_SUP_ACTION_SPAWN);
	target_vhost_sup_spawned(&f, 0, false);
	CHECK(f.state == TARGET_VHOST_SUP_BACKOFF);
	target_vhost_sup_stop(&f);
	CHECK(target_vhost_sup_tick(&f, 100 * kSec) == TARGET_VHOST_SUP_ACTION_NONE);
}

TEST_CASE("viewer pose page name: env override and disable", "[vpp]")
{
	char name[128];
#ifdef XRT_OS_WINDOWS
	_putenv_s("DXR_VIEWER_POSE_PAGE", "none");
#else
	setenv("DXR_VIEWER_POSE_PAGE", "none", 1);
#endif
	CHECK_FALSE(target_viewer_page_name(name, sizeof(name)));
#ifdef XRT_OS_WINDOWS
	_putenv_s("DXR_VIEWER_POSE_PAGE", "Local\\dxr-x");
#else
	setenv("DXR_VIEWER_POSE_PAGE", "/dxr-x", 1);
#endif
	REQUIRE(target_viewer_page_name(name, sizeof(name)));
#ifdef XRT_OS_WINDOWS
	CHECK(std::string(name) == "Local\\dxr-x");
	_putenv_s("DXR_VIEWER_POSE_PAGE", "");
#else
	CHECK(std::string(name) == "/dxr-x");
	unsetenv("DXR_VIEWER_POSE_PAGE");
#endif
#if !defined(XRT_OS_ANDROID)
	REQUIRE(target_viewer_page_name(name, sizeof(name)));
	CHECK(std::strlen(name) <= 31); // macOS shm names
#endif
}
