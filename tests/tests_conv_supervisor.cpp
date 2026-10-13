// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2032): the conversion-host supervisor FSM
 *         (conv_supervisor), driven with a scripted clock.
 *
 * Every row of roadmap conversion-providers.md §3.4 (+ §3.5 "Crash ->
 * WARMING", §3.6 respawn rules):
 *  - lazy spawn on demand; the maintenance mutex blocks every spawn;
 *  - backoff 0.5 s doubling to the 30 s cap, reset after 10 min healthy;
 *  - crash-loop cutoff: 5 abnormal exits in 10 min -> FAILED(crash_loop),
 *    4 + 1 outside the window is not; retried 30 min later;
 *  - the exit-code table: 0 orderly (requested) vs unrequested, 10-14 terminal
 *    with their reasons, 20 closed for update (no backoff, no accounting,
 *    never on a timer), 30 / crash = abnormal;
 *  - watchdog breach, heartbeat silence and hello timeout kill with exit 30;
 *  - idle exit after 10 min without demand; orderly shutdown 3 s then
 *    terminate (not a crash); a broken channel defers to the real exit code;
 *  - service shutdown never respawns.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_supervisor.h"
#include "conversion/conv_host_wire.h"

#include "xrt/xrt_conversion_plugin.h"

#include <string>
#include <vector>

namespace {

constexpr uint64_t MS = 1000000ull;
constexpr uint64_t SEC = 1000ull * MS;
constexpr uint64_t MIN = 60ull * SEC;

//! A supervisor plus a scripted clock and the process it "runs".
struct rig
{
	conv_supervisor s{};
	uint64_t now = 1000 * SEC; // never 0, so "since" stamps are meaningful

	explicit rig(const conv_supervisor_config *cfg = nullptr)
	{
		conv_supervisor_init(&s, cfg);
	}

	conv_supervisor_action
	tick(uint64_t *next = nullptr)
	{
		return conv_supervisor_tick(&s, now, next);
	}

	void
	advance(uint64_t dt)
	{
		now += dt;
	}

	//! Demand -> SPAWN -> spawned -> accepted hello: RUNNING.
	void
	start()
	{
		conv_supervisor_set_demand(&s, now, true);
		REQUIRE(tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&s, now, true);
		REQUIRE(s.state == CONV_SUP_STARTING);
		advance(50 * MS);
		conv_supervisor_hello(&s, now, true);
		REQUIRE(s.state == CONV_SUP_RUNNING);
	}

	//! From BACKOFF: wait out the delay, respawn, hello.
	uint64_t
	respawn()
	{
		REQUIRE(s.state == CONV_SUP_BACKOFF);
		uint64_t delay = s.next_spawn_ns - now;
		uint64_t next = 0;
		REQUIRE(tick(&next) == CONV_SUP_ACTION_NONE);
		REQUIRE(next == s.next_spawn_ns);
		now = s.next_spawn_ns - 1;
		REQUIRE(tick() == CONV_SUP_ACTION_NONE);
		now += 1;
		REQUIRE(tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&s, now, true);
		conv_supervisor_hello(&s, now, true);
		REQUIRE(s.state == CONV_SUP_RUNNING);
		return delay;
	}

	conv_supervisor_status
	status() const
	{
		conv_supervisor_status st;
		conv_supervisor_get_status(&s, &st);
		return st;
	}
};

} // namespace

TEST_CASE("supervisor: defaults are the §3.4 numbers", "[conv][supervisor]")
{
	conv_supervisor_config c;
	conv_supervisor_config_defaults(&c);
	CHECK(c.backoff_initial_ns == 500 * MS);
	CHECK(c.backoff_max_ns == 30 * SEC);
	CHECK(c.healthy_reset_ns == 10 * MIN);
	CHECK(c.crash_loop_count == 5);
	CHECK(c.crash_loop_window_ns == 10 * MIN);
	CHECK(c.crash_loop_retry_ns == 30 * MIN);
	CHECK(c.idle_exit_ns == 10 * MIN);
	CHECK(c.shutdown_grace_ns == 3 * SEC);
	CHECK(conv_supervisor_kill_code(nullptr) == CONV_HOST_EXIT_WATCHDOG);
}

TEST_CASE("supervisor: lazy spawn, and the maintenance mutex blocks it", "[conv][supervisor]")
{
	rig r;
	uint64_t next = 0;
	CHECK(r.tick(&next) == CONV_SUP_ACTION_NONE);
	CHECK(next == UINT64_MAX);
	CHECK(r.s.state == CONV_SUP_IDLE);
	CHECK_FALSE(conv_supervisor_host_alive(&r.s));

	conv_supervisor_set_maintenance(&r.s, true);
	conv_supervisor_set_demand(&r.s, r.now, true);
	CHECK(r.tick() == CONV_SUP_ACTION_NONE);
	conv_supervisor_set_maintenance(&r.s, false);
	CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	CHECK(r.s.state == CONV_SUP_STARTING);
	CHECK(conv_supervisor_host_alive(&r.s));
	conv_supervisor_spawned(&r.s, r.now, true);
	conv_supervisor_hello(&r.s, r.now, true);
	CHECK(r.s.state == CONV_SUP_RUNNING);
	auto st = r.status();
	CHECK(st.conv_state == XRT_CONV_STATE_UNKNOWN); // the provider's own state applies
	CHECK(st.spawns == 1);
	CHECK(st.restarts == 0);
}

TEST_CASE("supervisor: idle exit is orderly", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_set_demand(&r.s, r.now, false);
	uint64_t next = 0;
	r.advance(10 * MIN - 1);
	conv_supervisor_heartbeat(&r.s, r.now);
	CHECK(r.tick(&next) == CONV_SUP_ACTION_NONE);
	CHECK(next == r.now + 1);
	r.advance(1);
	CHECK(r.tick() == CONV_SUP_ACTION_SHUTDOWN);
	CHECK(r.s.state == CONV_SUP_STOPPING);
	r.advance(200 * MS);
	conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_ORDERLY);
	CHECK(r.s.state == CONV_SUP_IDLE);
	CHECK(r.status().restarts == 0);
	CHECK(r.status().conv_state == XRT_CONV_STATE_UNKNOWN);

	// Demand returns: a fresh lazy spawn, no backoff.
	conv_supervisor_set_demand(&r.s, r.now, true);
	CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
}

TEST_CASE("supervisor: the host's own idle exit is orderly; an exit 0 with demand is not", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_set_demand(&r.s, r.now, false);
	conv_supervisor_idle_exited(&r.s, r.now);
	CHECK(r.s.state == CONV_SUP_IDLE);
	CHECK(r.status().restarts == 0);
	CHECK(r.status().last_exit == CONV_HOST_EXIT_ORDERLY);
	CHECK(r.tick() == CONV_SUP_ACTION_NONE); // no demand: no respawn
	conv_supervisor_set_demand(&r.s, r.now, true);
	CHECK(r.tick() == CONV_SUP_ACTION_SPAWN); // lazily, no backoff
}

TEST_CASE("supervisor: demand resets the idle timer", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_set_demand(&r.s, r.now, false);
	r.advance(9 * MIN);
	conv_supervisor_heartbeat(&r.s, r.now);
	conv_supervisor_set_demand(&r.s, r.now, true);
	r.advance(1 * MIN);
	conv_supervisor_heartbeat(&r.s, r.now);
	conv_supervisor_set_demand(&r.s, r.now, false);
	r.advance(9 * MIN);
	conv_supervisor_heartbeat(&r.s, r.now);
	CHECK(r.tick() == CONV_SUP_ACTION_NONE);
	CHECK(r.s.state == CONV_SUP_RUNNING);
}

TEST_CASE("supervisor: orderly shutdown overruns 3 s -> terminate, not a crash", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_set_demand(&r.s, r.now, false);
	r.advance(10 * MIN);
	conv_supervisor_heartbeat(&r.s, r.now);
	REQUIRE(r.tick() == CONV_SUP_ACTION_SHUTDOWN);
	r.advance(3 * SEC - 1);
	CHECK(r.tick() == CONV_SUP_ACTION_NONE);
	r.advance(1);
	CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
	CHECK(r.s.state == CONV_SUP_KILLING);
	conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
	CHECK(r.s.state == CONV_SUP_IDLE);
	CHECK(r.status().restarts == 0);
}

TEST_CASE("supervisor: crash -> WARMING(restarting), backoff doubles to the cap", "[conv][supervisor]")
{
	conv_supervisor_config c;
	conv_supervisor_config_defaults(&c);
	c.crash_loop_count = CONV_SUP_MAX_CRASH_HISTORY; // isolate the backoff from the cutoff
	rig r(&c);
	r.start();

	const uint64_t want[] = {500 * MS, 1 * SEC, 2 * SEC, 4 * SEC, 8 * SEC, 16 * SEC, 30 * SEC, 30 * SEC};
	for (uint64_t w : want) {
		r.advance(1 * SEC);
		conv_supervisor_exited(&r.s, r.now, -1073741819); // an access violation
		auto st = r.status();
		REQUIRE(r.s.state == CONV_SUP_BACKOFF);
		CHECK(st.conv_state == XRT_CONV_STATE_WARMING);
		CHECK(std::string(st.detail) == "restarting");
		CHECK(r.respawn() == w);
		// RUNNING again: back to the provider's own state.
		CHECK(r.status().conv_state == XRT_CONV_STATE_UNKNOWN);
	}
	CHECK(r.status().restarts == 8);
}

TEST_CASE("supervisor: 10 min healthy resets the backoff", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_exited(&r.s, r.now, 139);
	CHECK(r.respawn() == 500 * MS);
	r.advance(1 * SEC);
	conv_supervisor_exited(&r.s, r.now, 139);
	CHECK(r.respawn() == 1 * SEC);

	// Healthy for 10 min: the next crash starts from 0.5 s again.
	for (int i = 0; i < 600; i++) {
		r.advance(1 * SEC);
		conv_supervisor_heartbeat(&r.s, r.now);
		r.tick();
	}
	CHECK(r.status().backoff_ns == 500 * MS);
	conv_supervisor_exited(&r.s, r.now, 139);
	CHECK(r.respawn() == 500 * MS);
}

TEST_CASE("supervisor: crash-loop cutoff", "[conv][supervisor]")
{
	SECTION("5 abnormal exits in 10 min -> FAILED(crash_loop), no respawn")
	{
		rig r;
		r.start();
		for (int i = 0; i < 4; i++) {
			r.advance(10 * SEC);
			conv_supervisor_exited(&r.s, r.now, 3);
			REQUIRE(r.s.state == CONV_SUP_BACKOFF);
			r.respawn();
		}
		r.advance(10 * SEC);
		conv_supervisor_exited(&r.s, r.now, 3);
		auto st = r.status();
		CHECK(r.s.state == CONV_SUP_FAILED);
		CHECK(st.conv_state == XRT_CONV_STATE_FAILED);
		CHECK(st.failure_reason == XRT_CONV_FAIL_CRASH_LOOP);
		CHECK(std::string(st.detail) == "crash_loop");
		CHECK(st.restarts == 5);
		r.advance(29 * MIN);
		uint64_t next = 0;
		CHECK(r.tick(&next) == CONV_SUP_ACTION_NONE);
		CHECK(next == r.now + 1 * MIN);

		// Retried 30 min after the cutoff (as conv_readiness does).
		r.advance(1 * MIN);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&r.s, r.now, true);
		conv_supervisor_hello(&r.s, r.now, true);
		CHECK(r.status().conv_state == XRT_CONV_STATE_UNKNOWN);
		// The history was cleared: one crash is a plain restart.
		conv_supervisor_exited(&r.s, r.now, 3);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.s.next_spawn_ns - r.now == 500 * MS);
	}
	SECTION("4 in the window + 1 outside it is not a loop")
	{
		rig r;
		r.start();
		for (int i = 0; i < 4; i++) {
			r.advance(10 * SEC);
			conv_supervisor_exited(&r.s, r.now, 3);
			r.respawn();
		}
		// The first of those four is now > 10 min old.
		r.advance(10 * MIN);
		conv_supervisor_heartbeat(&r.s, r.now);
		conv_supervisor_exited(&r.s, r.now, 3);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
	}
	SECTION("a configuration change clears the cutoff at once")
	{
		rig r;
		r.start();
		for (int i = 0; i < 5; i++) {
			r.advance(1 * SEC);
			conv_supervisor_exited(&r.s, r.now, 3);
			if (i < 4) {
				r.respawn();
			}
		}
		REQUIRE(r.s.state == CONV_SUP_FAILED);
		conv_supervisor_config_changed(&r.s, r.now);
		CHECK(r.s.state == CONV_SUP_IDLE);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	}
}

TEST_CASE("supervisor: exit 20 = closed for update", "[conv][supervisor]")
{
	rig r;
	r.start();
	conv_supervisor_set_maintenance(&r.s, true); // the installer's RM session
	conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_UPDATE);
	auto st = r.status();
	CHECK(r.s.state == CONV_SUP_UPDATING);
	CHECK(st.conv_state == XRT_CONV_STATE_WARMING);
	CHECK(std::string(st.detail) == "updating");
	CHECK(st.restarts == 0); // no accounting

	// Never on a timer.
	uint64_t next = 0;
	CHECK(r.tick(&next) == CONV_SUP_ACTION_NONE);
	CHECK(next == UINT64_MAX);
	r.advance(1 * 60 * MIN);
	CHECK(r.tick() == CONV_SUP_ACTION_NONE);

	// Mutex free + demand: respawn.
	conv_supervisor_set_maintenance(&r.s, false);
	CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	conv_supervisor_spawned(&r.s, r.now, true);
	CHECK(r.status().conv_state == XRT_CONV_STATE_WARMING); // still "updating" until hello
	conv_supervisor_hello(&r.s, r.now, true);
	CHECK(r.status().conv_state == XRT_CONV_STATE_UNKNOWN);

	SECTION("repeated updates never reach the cutoff")
	{
		for (int i = 0; i < 10; i++) {
			conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_UPDATE);
			REQUIRE(r.s.state == CONV_SUP_UPDATING);
			REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
			conv_supervisor_spawned(&r.s, r.now, true);
			conv_supervisor_hello(&r.s, r.now, true);
		}
		CHECK(r.status().restarts == 0);
	}
	SECTION("no demand: stays down until demand returns")
	{
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_UPDATE);
		conv_supervisor_set_demand(&r.s, r.now, false);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		conv_supervisor_set_demand(&r.s, r.now, true);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	}
	SECTION("a configuration change while updating still waits for the mutex")
	{
		conv_supervisor_set_maintenance(&r.s, true);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_UPDATE);
		conv_supervisor_config_changed(&r.s, r.now);
		CHECK(r.status().conv_state == XRT_CONV_STATE_WARMING);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		conv_supervisor_set_maintenance(&r.s, false);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	}
}

TEST_CASE("supervisor: terminal exit codes", "[conv][supervisor]")
{
	struct row
	{
		int32_t code;
		uint32_t reason;
		uint32_t retryable;
	};
	const row rows[] = {
	    {CONV_HOST_EXIT_ABI_REJECT, XRT_CONV_FAIL_VERSION, 0},
	    {CONV_HOST_EXIT_NEGOTIATE_FAILED, XRT_CONV_FAIL_LOAD, 0},
	    {CONV_HOST_EXIT_LICENCE, XRT_CONV_FAIL_LICENCE, 1},
	    {CONV_HOST_EXIT_NO_DEVICE, XRT_CONV_FAIL_PLATFORM, 1},
	    {CONV_HOST_EXIT_ELEVATED, XRT_CONV_FAIL_PLATFORM, 0},
	};
	for (const row &w : rows) {
		CAPTURE(w.code);
		rig r;
		r.start();
		conv_supervisor_exited(&r.s, r.now, w.code);
		auto st = r.status();
		CHECK(r.s.state == CONV_SUP_FAILED);
		CHECK(st.conv_state == XRT_CONV_STATE_FAILED);
		CHECK(st.failure_reason == w.reason);
		CHECK(st.retryable == w.retryable);
		CHECK(st.restarts == 0);
		CHECK(st.last_exit == w.code);
		// No respawn on any timer.
		r.advance(2 * 60 * MIN);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		// An explicit retry (or a config change) re-arms the lazy spawn.
		conv_supervisor_retry(&r.s, r.now);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	}
	// A terminal code during STARTING (before hello) is the same.
	rig r;
	conv_supervisor_set_demand(&r.s, r.now, true);
	REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
	conv_supervisor_spawned(&r.s, r.now, true);
	conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_ELEVATED);
	CHECK(r.status().failure_reason == XRT_CONV_FAIL_PLATFORM);
}

TEST_CASE("supervisor: abnormal exits", "[conv][supervisor]")
{
	SECTION("exit 0 nobody asked for")
	{
		rig r;
		r.start();
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_ORDERLY);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.status().restarts == 1);
	}
	SECTION("a failed spawn")
	{
		rig r;
		conv_supervisor_set_demand(&r.s, r.now, true);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&r.s, r.now, false);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.status().last_exit == CONV_SUP_EXIT_SPAWN_FAILED);
		CHECK(r.status().restarts == 1);
	}
	SECTION("backoff expires with no demand left: idle, no spawn")
	{
		rig r;
		r.start();
		conv_supervisor_exited(&r.s, r.now, 139);
		conv_supervisor_set_demand(&r.s, r.now, false);
		r.advance(1 * SEC);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		CHECK(r.s.state == CONV_SUP_IDLE);
		CHECK(r.status().conv_state == XRT_CONV_STATE_UNKNOWN);
	}
	SECTION("the maintenance mutex holds a crash respawn too")
	{
		rig r;
		r.start();
		conv_supervisor_exited(&r.s, r.now, 139); // RmForceShutdown looks like this
		conv_supervisor_set_maintenance(&r.s, true);
		r.advance(1 * SEC);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		conv_supervisor_set_maintenance(&r.s, false);
		CHECK(r.tick() == CONV_SUP_ACTION_SPAWN);
	}
}

TEST_CASE("supervisor: watchdog kills with exit 30", "[conv][supervisor]")
{
	SECTION("call watchdog breach")
	{
		rig r;
		r.start();
		conv_supervisor_watchdog_breach(&r.s, r.now);
		CHECK(r.s.state == CONV_SUP_KILLING);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		CHECK(conv_supervisor_kill_code(&r.s) == CONV_HOST_EXIT_WATCHDOG);
		// No exit seen within kill_grace: TERMINATE again.
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		r.advance(5 * SEC);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.status().restarts == 1);
		CHECK(r.status().conv_state == XRT_CONV_STATE_WARMING);
	}
	SECTION("heartbeat silence")
	{
		rig r;
		r.start();
		r.advance(9 * SEC);
		conv_supervisor_heartbeat(&r.s, r.now);
		uint64_t next = 0;
		CHECK(r.tick(&next) == CONV_SUP_ACTION_NONE);
		CHECK(next == r.now + 10 * SEC);
		r.advance(10 * SEC);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		// Whatever code the killed host reports, our kill is abnormal.
		conv_supervisor_exited(&r.s, r.now, 0);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.status().restarts == 1);
	}
	SECTION("hello timeout")
	{
		rig r;
		conv_supervisor_set_demand(&r.s, r.now, true);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&r.s, r.now, true);
		r.advance(10 * SEC - 1);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		r.advance(1);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
	}
}

TEST_CASE("supervisor: hello rejected", "[conv][supervisor]")
{
	SECTION("the host exits 10 by itself")
	{
		rig r;
		conv_supervisor_set_demand(&r.s, r.now, true);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&r.s, r.now, true);
		conv_supervisor_hello(&r.s, r.now, false);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_ABI_REJECT);
		CHECK(r.s.state == CONV_SUP_FAILED);
		CHECK(r.status().failure_reason == XRT_CONV_FAIL_VERSION);
	}
	SECTION("the host lingers: killed after the grace, still a version reject")
	{
		rig r;
		conv_supervisor_set_demand(&r.s, r.now, true);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_spawned(&r.s, r.now, true);
		conv_supervisor_hello(&r.s, r.now, false);
		r.advance(5 * SEC);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
		CHECK(r.s.state == CONV_SUP_FAILED);
		CHECK(r.status().failure_reason == XRT_CONV_FAIL_VERSION);
		CHECK(r.status().restarts == 0);
	}
}

TEST_CASE("supervisor: a broken channel defers to the real exit code", "[conv][supervisor]")
{
	SECTION("the pipe closes because the host exited 20 (Restart Manager)")
	{
		rig r;
		r.start();
		conv_supervisor_channel_broken(&r.s, r.now);
		CHECK(r.s.state == CONV_SUP_KILLING);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE); // harmless if already gone
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_UPDATE);
		CHECK(r.s.state == CONV_SUP_UPDATING);
		CHECK(r.status().restarts == 0);
	}
	SECTION("the pipe closes on a crash")
	{
		rig r;
		r.start();
		conv_supervisor_channel_broken(&r.s, r.now);
		conv_supervisor_exited(&r.s, r.now, 139);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
		CHECK(r.status().restarts == 1);
	}
	SECTION("a protocol violation by a live host: killed, abnormal")
	{
		rig r;
		r.start();
		conv_supervisor_channel_broken(&r.s, r.now);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
		CHECK(r.s.state == CONV_SUP_BACKOFF);
	}
	SECTION("during an orderly stop it is expected")
	{
		rig r;
		r.start();
		conv_supervisor_service_shutdown(&r.s, r.now);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SHUTDOWN);
		conv_supervisor_channel_broken(&r.s, r.now);
		CHECK(r.s.state == CONV_SUP_STOPPING);
	}
}

TEST_CASE("supervisor: service shutdown never respawns", "[conv][supervisor]")
{
	SECTION("from RUNNING")
	{
		rig r;
		r.start();
		conv_supervisor_service_shutdown(&r.s, r.now);
		CHECK(r.tick() == CONV_SUP_ACTION_SHUTDOWN);
		conv_supervisor_exited(&r.s, r.now, 0);
		CHECK(r.s.state == CONV_SUP_STOPPED);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE); // demand is still true
	}
	SECTION("from STARTING: terminate, not a crash")
	{
		rig r;
		conv_supervisor_set_demand(&r.s, r.now, true);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SPAWN);
		conv_supervisor_service_shutdown(&r.s, r.now);
		CHECK(r.tick() == CONV_SUP_ACTION_TERMINATE);
		conv_supervisor_exited(&r.s, r.now, CONV_HOST_EXIT_WATCHDOG);
		CHECK(r.s.state == CONV_SUP_STOPPED);
		CHECK(r.status().restarts == 0);
	}
	SECTION("from BACKOFF")
	{
		rig r;
		r.start();
		conv_supervisor_exited(&r.s, r.now, 139);
		conv_supervisor_service_shutdown(&r.s, r.now);
		CHECK(r.s.state == CONV_SUP_STOPPED);
		r.advance(1 * MIN);
		CHECK(r.tick() == CONV_SUP_ACTION_NONE);
	}
	SECTION("a crash while stopping")
	{
		rig r;
		r.start();
		conv_supervisor_service_shutdown(&r.s, r.now);
		REQUIRE(r.tick() == CONV_SUP_ACTION_SHUTDOWN);
		conv_supervisor_exited(&r.s, r.now, 139);
		CHECK(r.s.state == CONV_SUP_STOPPED);
	}
}

TEST_CASE("supervisor: stale events are ignored", "[conv][supervisor]")
{
	rig r;
	conv_supervisor_exited(&r.s, r.now, 139); // nothing running
	CHECK(r.s.state == CONV_SUP_IDLE);
	conv_supervisor_hello(&r.s, r.now, true);
	CHECK(r.s.state == CONV_SUP_IDLE);
	conv_supervisor_watchdog_breach(&r.s, r.now);
	conv_supervisor_channel_broken(&r.s, r.now);
	CHECK(r.s.state == CONV_SUP_IDLE);
	CHECK(r.status().restarts == 0);
	CHECK(std::string(conv_supervisor_state_str(CONV_SUP_UPDATING)) == "updating");
}
