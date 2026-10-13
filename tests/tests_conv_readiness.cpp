// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 readiness: the per-(provider, capability) state machine and
 *         the spec-3 legacy mapping (conv_readiness).
 *
 * What must hold (spec conversion-provider-discovery.md §4.2-4.3, roadmap
 * §2.5, §7.3):
 *  - the golden transition table, event by event;
 *  - a cold boot reads ACTIVATING from the very first query, before any
 *    describe ("properties before the first describe => ACTIVATING");
 *  - READY wins: splats downloading + SBS READY => READY;
 *  - a host crash is WARMING(restarting, n); 5 abnormal exits in 10 min is
 *    FAILED(CRASH_LOOP), retried 30 min later;
 *  - FAILED(VERSION) is never FAILED(MODEL);
 *  - every retry trigger: registry change, explicit warm, a create for an
 *    unserved task failed > 30 s ago, the network backoff (15 s doubling to
 *    10 min), 30 min after a crash loop; FAILED(RESOURCES) waits for a budget
 *    change; nothing latches.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_readiness.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t S = CONV_NS_PER_S;

conv_readiness_event
ev(uint32_t type, uint64_t now, uint32_t flags = 0, uint32_t reason = 0, bool retryable = false)
{
	conv_readiness_event e{};
	e.type = type;
	e.now_ns = now;
	e.flags = flags;
	e.reason = reason;
	e.retryable = retryable;
	return e;
}

//! A machine sitting in @p state (reason @p reason), entered at t = 1000 s.
conv_readiness
at(uint32_t state, uint32_t reason = 0)
{
	conv_readiness r;
	conv_readiness_init(&r);
	r.state = state;
	r.reason = reason;
	r.entered_ns = 1000 * S;
	if (state == XRT_CONV_STATE_FAILED) {
		r.failed_ns = 1000 * S;
	}
	return r;
}

uint32_t
fire(conv_readiness &r, const conv_readiness_event &e)
{
	return conv_readiness_apply(&r, &e);
}

struct golden
{
	const char *name;
	uint32_t from, from_reason;
	uint32_t event, flags, reason;
	bool retryable;
	uint32_t to, to_reason;
	uint32_t bits; //!< CONV_RD_* that must be set
};

} // namespace

TEST_CASE("conv_readiness: golden transition table")
{
	const uint64_t t = 1001 * S;
	// clang-format off
	const golden table[] = {
	    {"described",                     XRT_CONV_STATE_UNKNOWN, 0, CONV_EV_DESCRIBED, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED},
	    {"binary missing",                XRT_CONV_STATE_UNKNOWN, 0, CONV_EV_DESCRIBE_FAILED, 0, 0, false, XRT_CONV_STATE_ABSENT, CONV_REASON_BINARY_MISSING, CONV_RD_CHANGED},
	    {"ABI rejected",                  XRT_CONV_STATE_UNKNOWN, 0, CONV_EV_DESCRIBE_FAILED, 0, CONV_REASON_ABI_REJECTED, false, XRT_CONV_STATE_ABSENT, CONV_REASON_ABI_REJECTED, CONV_RD_CHANGED},
	    {"installed: warm, model present",XRT_CONV_STATE_INSTALLED, 0, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED},
	    {"evicted: warm, model present",  XRT_CONV_STATE_EVICTED, 0, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED},
	    {"installed: warm, blobs missing",XRT_CONV_STATE_INSTALLED, 0, CONV_EV_WARM_REQUESTED, CONV_EVF_MODEL_MISSING, 0, false, XRT_CONV_STATE_DOWNLOADING, 0, CONV_RD_CHANGED},
	    {"evicted: warm, blobs missing",  XRT_CONV_STATE_EVICTED, 0, CONV_EV_WARM_REQUESTED, CONV_EVF_MODEL_MISSING, 0, false, XRT_CONV_STATE_DOWNLOADING, 0, CONV_RD_CHANGED},
	    {"download forbidden: ABSENT(policy)", XRT_CONV_STATE_INSTALLED, 0, CONV_EV_WARM_REQUESTED, CONV_EVF_MODEL_MISSING | CONV_EVF_DOWNLOAD_FORBIDDEN, 0, false, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_RD_CHANGED},
	    {"exceeds memory: FAILED(RESOURCES)", XRT_CONV_STATE_INSTALLED, 0, CONV_EV_WARM_REQUESTED, CONV_EVF_EXCEEDS_MEMORY | CONV_EVF_MODEL_MISSING, 0, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_RESOURCES, CONV_RD_CHANGED},
	    {"create warms like a request",   XRT_CONV_STATE_INSTALLED, 0, CONV_EV_CREATE_REQUESTED, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED},
	    {"download started",              XRT_CONV_STATE_INSTALLED, 0, CONV_EV_DOWNLOAD_STARTED, 0, 0, false, XRT_CONV_STATE_DOWNLOADING, 0, CONV_RD_CHANGED},
	    {"download progress",             XRT_CONV_STATE_DOWNLOADING, 0, CONV_EV_DOWNLOAD_PROGRESS, 0, 0, false, XRT_CONV_STATE_DOWNLOADING, 0, CONV_RD_PROGRESS},
	    {"download done",                 XRT_CONV_STATE_DOWNLOADING, 0, CONV_EV_DOWNLOAD_DONE, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED},
	    {"download failed: network",      XRT_CONV_STATE_DOWNLOADING, 0, CONV_EV_DOWNLOAD_FAILED, 0, XRT_CONV_FAIL_NETWORK, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_NETWORK, CONV_RD_CHANGED},
	    {"download failed: bad hash",     XRT_CONV_STATE_DOWNLOADING, 0, CONV_EV_DOWNLOAD_FAILED, 0, XRT_CONV_FAIL_MODEL, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_MODEL, CONV_RD_CHANGED},
	    {"warm progress",                 XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_PROGRESS, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_PROGRESS},
	    {"provider ready",                XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_OK, 0, 0, false, XRT_CONV_STATE_READY, 0, CONV_RD_CHANGED},
	    {"warm failed: licence",          XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_FAILED, 0, XRT_CONV_FAIL_LICENCE, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LICENCE, CONV_RD_CHANGED},
	    {"warm failed: platform",         XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_FAILED, 0, XRT_CONV_FAIL_PLATFORM, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_PLATFORM, CONV_RD_CHANGED},
	    {"warm failed: network",          XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_FAILED, 0, XRT_CONV_FAIL_NETWORK, true, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_NETWORK, CONV_RD_CHANGED},
	    {"runtime too old: VERSION",      XRT_CONV_STATE_WARMING, 0, CONV_EV_WARM_FAILED, 0, XRT_CONV_FAIL_VERSION, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION, CONV_RD_CHANGED},
	    {"idle past keep-alive",          XRT_CONV_STATE_READY, 0, CONV_EV_EVICTED, 0, 0, false, XRT_CONV_STATE_EVICTED, 0, CONV_RD_CHANGED},
	    {"never evicted while bound",     XRT_CONV_STATE_READY, 0, CONV_EV_EVICTED, CONV_EVF_BOUND, 0, false, XRT_CONV_STATE_READY, 0, CONV_RD_IGNORED},
	    {"ready: host crash",             XRT_CONV_STATE_READY, 0, CONV_EV_HOST_CRASH, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED | CONV_RD_ACT_WARM},
	    {"warming: host crash",           XRT_CONV_STATE_WARMING, 0, CONV_EV_HOST_CRASH, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED | CONV_RD_ACT_WARM},
	    {"crash-loop cutoff",             XRT_CONV_STATE_READY, 0, CONV_EV_CRASH_LOOP, 0, 0, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_CRASH_LOOP, CONV_RD_CHANGED},
	    {"crash-loop cutoff from evicted",XRT_CONV_STATE_EVICTED, 0, CONV_EV_CRASH_LOOP, 0, 0, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_CRASH_LOOP, CONV_RD_CHANGED},
	    {"disable from ready",            XRT_CONV_STATE_READY, 0, CONV_EV_DISABLE, 0, 0, false, XRT_CONV_STATE_DISABLED, 0, CONV_RD_CHANGED},
	    {"disable from failed",           XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LOAD, CONV_EV_DISABLE, 0, 0, false, XRT_CONV_STATE_DISABLED, 0, CONV_RD_CHANGED},
	    {"enable",                        XRT_CONV_STATE_DISABLED, 0, CONV_EV_ENABLE, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED | CONV_RD_ACT_REDESCRIBE},
	    {"registry change retries FAILED",XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION, CONV_EV_REGISTRY_CHANGED, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED | CONV_RD_ACT_REDESCRIBE},
	    {"registry change re-probes ABSENT", XRT_CONV_STATE_ABSENT, CONV_REASON_BINARY_MISSING, CONV_EV_REGISTRY_CHANGED, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED | CONV_RD_ACT_REDESCRIBE},
	    {"registry change lifts ABSENT(policy)", XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_EV_REGISTRY_CHANGED, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED | CONV_RD_ACT_REDESCRIBE},
	    {"budget change retries RESOURCES", XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_RESOURCES, CONV_EV_BUDGET_CHANGED, 0, 0, false, XRT_CONV_STATE_INSTALLED, 0, CONV_RD_CHANGED | CONV_RD_ACT_WARM},
	    {"explicit warm retries FAILED",  XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_MODEL, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_WARMING, 0, CONV_RD_CHANGED},
	    {"explicit warm: not RESOURCES",  XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_RESOURCES, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_RESOURCES, CONV_RD_IGNORED},
	    {"warm on READY is a no-op",      XRT_CONV_STATE_READY, 0, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_READY, 0, CONV_RD_IGNORED},
	    {"warm before describe ignored",  XRT_CONV_STATE_UNKNOWN, 0, CONV_EV_WARM_REQUESTED, 0, 0, false, XRT_CONV_STATE_UNKNOWN, 0, CONV_RD_IGNORED},
	    {"warm failed outside WARMING",   XRT_CONV_STATE_READY, 0, CONV_EV_WARM_FAILED, 0, XRT_CONV_FAIL_LOAD, false, XRT_CONV_STATE_READY, 0, CONV_RD_IGNORED},
	    {"declared absent while warming", XRT_CONV_STATE_WARMING, 0, CONV_EV_DECLARED_ABSENT, 0, 0, false, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_RD_CHANGED},
	    {"module-declared absent is model", XRT_CONV_STATE_WARMING, 0, CONV_EV_DECLARED_ABSENT, 0, XRT_CONV_FAIL_MODEL, false, XRT_CONV_STATE_ABSENT, XRT_CONV_FAIL_MODEL, CONV_RD_CHANGED},
	    {"kill switch over absent(model)", XRT_CONV_STATE_ABSENT, XRT_CONV_FAIL_MODEL, CONV_EV_DECLARED_ABSENT, 0, CONV_REASON_POLICY, false, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_RD_CHANGED},
	    {"declared absent ends a retry loop", XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_MODEL, CONV_EV_DECLARED_ABSENT, 0, 0, false, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_RD_CHANGED},
	    {"declared absent at describe",   XRT_CONV_STATE_INSTALLED, 0, CONV_EV_DECLARED_ABSENT, 0, 0, false, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, CONV_RD_CHANGED},
	    {"declared absent: DISABLED wins",XRT_CONV_STATE_DISABLED, 0, CONV_EV_DECLARED_ABSENT, 0, 0, false, XRT_CONV_STATE_DISABLED, 0, CONV_RD_IGNORED},
	    {"declared absent: UNLICENSED kept", XRT_CONV_STATE_UNLICENSED, 0, CONV_EV_DECLARED_ABSENT, 0, 0, false, XRT_CONV_STATE_UNLICENSED, 0, CONV_RD_IGNORED},
	};
	// clang-format on

	for (const golden &g : table) {
		CAPTURE(g.name);
		conv_readiness r = at(g.from, g.from_reason);
		const uint64_t gen0 = r.generation;
		uint32_t bits = fire(r, ev(g.event, t, g.flags, g.reason, g.retryable));
		CHECK(conv_state_name(r.state) == std::string(conv_state_name(g.to)));
		CHECK(r.reason == g.to_reason);
		CHECK((bits & g.bits) == g.bits);
		if ((g.bits & CONV_RD_CHANGED) != 0) {
			CHECK(r.generation > gen0);
		} else {
			CHECK(r.generation == gen0);
			CHECK((bits & CONV_RD_CHANGED) == 0);
		}
		CHECK(conv_state_in_range(r.state));
	}
}

TEST_CASE("conv_readiness: state classes")
{
	for (uint32_t s = 0; s <= XRT_CONV_STATE_EVICTED; s++) {
		CAPTURE(conv_state_name(s));
		// Exactly one of: READY, waited-for, terminal, UNKNOWN.
		int classes = (s == XRT_CONV_STATE_READY) + conv_state_is_waited(s) + conv_state_is_terminal(s) +
		              (s == XRT_CONV_STATE_UNKNOWN);
		CHECK(classes == 1);
	}
	CHECK_FALSE(conv_state_is_terminal(XRT_CONV_STATE_UNKNOWN)); // "not yet asked" is never terminal
	CHECK_FALSE(conv_state_in_range(XRT_CONV_STATE_EVICTED + 1));
	CHECK(std::string(conv_reason_name(XRT_CONV_FAIL_VERSION)) != conv_reason_name(XRT_CONV_FAIL_MODEL));
}

TEST_CASE("conv_readiness: cold boot reads ACTIVATING from the first query")
{
	// Properties before the first describe: no task row exists yet, the
	// registered provider is neither described nor terminal.
	CHECK(conv_legacy_v3_state(nullptr, 0, false) == CONV_V3_STATE_ACTIVATING);

	conv_readiness r;
	conv_readiness_init(&r);
	uint32_t s = r.state;
	CHECK(conv_legacy_v3_state(&s, 1, false) == CONV_V3_STATE_ACTIVATING);
	// Even "settled" cannot make a not-yet-described task UNAVAILABLE.
	CHECK(conv_legacy_v3_state(&s, 1, true) == CONV_V3_STATE_ACTIVATING);

	uint64_t t = 1 * S;
	fire(r, ev(CONV_EV_DESCRIBED, t)); // live, or from the describe cache
	s = r.state;
	CHECK(s == XRT_CONV_STATE_INSTALLED);
	CHECK(conv_legacy_v3_state(&s, 1, true) == CONV_V3_STATE_ACTIVATING);

	fire(r, ev(CONV_EV_WARM_REQUESTED, t + 1)); // prewarm on interest
	s = r.state;
	CHECK(s == XRT_CONV_STATE_WARMING);
	CHECK(conv_legacy_v3_state(&s, 1, true) == CONV_V3_STATE_ACTIVATING);

	fire(r, ev(CONV_EV_WARM_OK, t + 2 * S));
	s = r.state;
	CHECK(conv_legacy_v3_state(&s, 1, true) == CONV_V3_STATE_READY);

	SECTION("discovery settled check")
	{
		bool described[2] = {true, false};
		bool terminal[2] = {false, false};
		CHECK_FALSE(conv_discovery_settled(described, terminal, 2));
		terminal[1] = true;
		CHECK(conv_discovery_settled(described, terminal, 2));
		CHECK(conv_discovery_settled(nullptr, nullptr, 0));
	}
}

TEST_CASE("conv_readiness: spec-3 mapping")
{
	SECTION("splats downloading + SBS READY -> READY")
	{
		uint32_t s[] = {XRT_CONV_STATE_DOWNLOADING, XRT_CONV_STATE_READY};
		CHECK(conv_legacy_v3_state(s, 2, true) == CONV_V3_STATE_READY);
		CHECK(conv_legacy_v3_state(s, 2, false) == CONV_V3_STATE_READY);
	}
	SECTION("any non-terminal backing task -> ACTIVATING, never UNAVAILABLE")
	{
		const uint32_t waited[] = {XRT_CONV_STATE_INSTALLED, XRT_CONV_STATE_DOWNLOADING, XRT_CONV_STATE_WARMING,
		                           XRT_CONV_STATE_EVICTED};
		for (uint32_t w : waited) {
			uint32_t s[] = {XRT_CONV_STATE_FAILED, w};
			CHECK(conv_legacy_v3_state(s, 2, true) == CONV_V3_STATE_ACTIVATING);
		}
	}
	SECTION("every backing task terminal and discovery settled -> UNAVAILABLE")
	{
		uint32_t s[] = {XRT_CONV_STATE_FAILED, XRT_CONV_STATE_ABSENT, XRT_CONV_STATE_DISABLED};
		CHECK(conv_legacy_v3_state(s, 3, true) == CONV_V3_STATE_UNAVAILABLE);
		CHECK(conv_legacy_v3_state(s, 3, false) == CONV_V3_STATE_ACTIVATING);
		CHECK(conv_legacy_v3_state(nullptr, 0, true) == CONV_V3_STATE_UNAVAILABLE);
	}
	SECTION("v3 mode -> backing task")
	{
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_DEPTH, false) == XRT_CONV_TASK_DEPTH_VIDEO);
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_DEPTH, true) == XRT_CONV_TASK_DEPTH_MONO);
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_SBS, false) == XRT_CONV_TASK_VIEWS_MONO);
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_NVIEW, false) == XRT_CONV_TASK_VIEWS_MONO);
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_GAUSSIANS, true) == XRT_CONV_TASK_SPLATS_MONO);
		CHECK(conv_legacy_mode_task(CONV_V3_MODE_SBS | CONV_V3_MODE_DEPTH, false) == XRT_CONV_TASK_NONE);
	}
}

TEST_CASE("conv_readiness: host closed for update -> WARMING(updating), never a crash")
{
	// #2056 (roadmap §3.6): exit 20 is no crash: no record, no backoff, no cutoff.
	conv_readiness r = at(XRT_CONV_STATE_READY);
	uint64_t t = 3000 * S;
	for (int i = 0; i < 10; i++) {
		CAPTURE(i);
		uint32_t bits = fire(r, ev(CONV_EV_HOST_UPDATING, t));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
		CHECK(r.reason == CONV_REASON_UPDATING);
		CHECK_FALSE(r.restarting);
		CHECK((bits & CONV_RD_CHANGED) != 0);
		CHECK((bits & CONV_RD_ACT_WARM) != 0);
		CHECK(r.crash_count == 0u);
		CHECK(conv_readiness_next_deadline(&r) == 0u); // no timer of any kind
		fire(r, ev(CONV_EV_WARM_OK, t + S));
		CHECK(r.state == XRT_CONV_STATE_READY);
		CHECK(r.reason == 0u);
		t += 10 * S;
	}
	CHECK(std::string(conv_reason_name(CONV_REASON_UPDATING)) == "updating");

	SECTION("a crash while updating is the crash path")
	{
		fire(r, ev(CONV_EV_HOST_UPDATING, t));
		fire(r, ev(CONV_EV_HOST_CRASH, t + S));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
		CHECK(r.reason == 0u);
		CHECK(r.restarting);
		CHECK(r.restart_attempt == 1u);
	}
	SECTION("cold, terminal and disabled rows are untouched")
	{
		for (uint32_t s0 : {(uint32_t)XRT_CONV_STATE_INSTALLED, (uint32_t)XRT_CONV_STATE_EVICTED,
		                    (uint32_t)XRT_CONV_STATE_FAILED, (uint32_t)XRT_CONV_STATE_DISABLED}) {
			CAPTURE(s0);
			conv_readiness c = at(s0);
			CHECK((fire(c, ev(CONV_EV_HOST_UPDATING, t)) & CONV_RD_IGNORED) != 0);
			CHECK(c.state == s0);
		}
	}
}

TEST_CASE("conv_readiness: host crash -> WARMING(restarting, n); crash loop cutoff")
{
	conv_readiness r = at(XRT_CONV_STATE_READY);
	uint64_t t = 2000 * S;
	for (uint32_t n = 1; n <= 4; n++) {
		CAPTURE(n);
		uint64_t gen0 = r.generation;
		uint32_t bits = fire(r, ev(CONV_EV_HOST_CRASH, t));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
		CHECK(r.restarting);
		CHECK(r.restart_attempt == n);
		CHECK((bits & CONV_RD_CHANGED) != 0);
		CHECK(r.generation == gen0 + 1); // one bump per crash, never two
		t += 60 * S;
	}
	// Recovery clears the attempt counter but not the crash history.
	fire(r, ev(CONV_EV_WARM_OK, t));
	CHECK(r.state == XRT_CONV_STATE_READY);
	CHECK(r.restart_attempt == 0);
	CHECK_FALSE(r.restarting);

	// The fifth abnormal exit inside 10 min is the cutoff.
	fire(r, ev(CONV_EV_HOST_CRASH, t));
	CHECK(r.state == XRT_CONV_STATE_FAILED);
	CHECK(r.reason == XRT_CONV_FAIL_CRASH_LOOP);
	CHECK(conv_readiness_next_deadline(&r) == t + CONV_CRASH_LOOP_RETRY_NS);

	SECTION("retried 30 min after the cutoff, not before")
	{
		CHECK((conv_readiness_tick(&r, t + CONV_CRASH_LOOP_RETRY_NS - 1) & CONV_RD_IGNORED) != 0);
		CHECK(r.state == XRT_CONV_STATE_FAILED);
		uint32_t bits = conv_readiness_tick(&r, t + CONV_CRASH_LOOP_RETRY_NS);
		CHECK(r.state == XRT_CONV_STATE_INSTALLED);
		CHECK((bits & CONV_RD_ACT_WARM) != 0);
		// The history is cleared: one more crash is a restart, not a new cutoff.
		fire(r, ev(CONV_EV_WARM_REQUESTED, t + CONV_CRASH_LOOP_RETRY_NS));
		fire(r, ev(CONV_EV_HOST_CRASH, t + CONV_CRASH_LOOP_RETRY_NS + S));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
		CHECK(r.restart_attempt == 1);
	}
	SECTION("a create does not retry a crash loop early")
	{
		CHECK((fire(r, ev(CONV_EV_CREATE_REQUESTED, t + 600 * S)) & CONV_RD_IGNORED) != 0);
		CHECK(r.state == XRT_CONV_STATE_FAILED);
	}
	SECTION("an explicit warm does")
	{
		fire(r, ev(CONV_EV_WARM_REQUESTED, t + S));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
	}
}

TEST_CASE("conv_readiness: crashes spread over more than 10 min never trip the cutoff")
{
	conv_readiness r = at(XRT_CONV_STATE_READY);
	uint64_t t = 1000 * S;
	for (int i = 0; i < 12; i++) {
		fire(r, ev(CONV_EV_HOST_CRASH, t));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
		fire(r, ev(CONV_EV_WARM_OK, t + S));
		t += 151 * S; // 5 exits span 604 s > 600 s
	}
	SECTION("5 in exactly 10 min do")
	{
		conv_readiness q = at(XRT_CONV_STATE_READY);
		for (int i = 0; i < 4; i++) {
			fire(q, ev(CONV_EV_HOST_CRASH, 1000 * S + (uint64_t)i * 150 * S));
		}
		fire(q, ev(CONV_EV_HOST_CRASH, 1000 * S + 600 * S));
		CHECK(q.reason == XRT_CONV_FAIL_CRASH_LOOP);
	}
	SECTION("crashes while cold are counted but do not warm")
	{
		conv_readiness q = at(XRT_CONV_STATE_INSTALLED);
		CHECK((fire(q, ev(CONV_EV_HOST_CRASH, 1000 * S)) & CONV_RD_IGNORED) != 0);
		CHECK(q.state == XRT_CONV_STATE_INSTALLED);
		CHECK(q.crash_count == 1);
	}
}

TEST_CASE("conv_readiness: FAILED(VERSION) is distinct from FAILED(MODEL)")
{
	conv_readiness v = at(XRT_CONV_STATE_WARMING);
	fire(v, ev(CONV_EV_WARM_FAILED, 1001 * S, 0, XRT_CONV_FAIL_VERSION));
	conv_readiness m = at(XRT_CONV_STATE_DOWNLOADING);
	fire(m, ev(CONV_EV_DOWNLOAD_FAILED, 1001 * S, 0, XRT_CONV_FAIL_MODEL));
	CHECK(v.state == XRT_CONV_STATE_FAILED);
	CHECK(m.state == XRT_CONV_STATE_FAILED);
	CHECK(v.reason == XRT_CONV_FAIL_VERSION);
	CHECK(m.reason == XRT_CONV_FAIL_MODEL);
	CHECK(std::string(conv_reason_name(v.reason)) == "version");
	CHECK(std::string(conv_reason_name(m.reason)) == "model");
	// Neither is on a timer unless the caller says it is retryable.
	CHECK(conv_readiness_next_deadline(&v) == 0);
	CHECK(conv_readiness_next_deadline(&m) == 0);
}

TEST_CASE("conv_readiness: retry triggers")
{
	SECTION("network backoff: 15 s, doubling, capped at 10 min")
	{
		conv_readiness r = at(XRT_CONV_STATE_WARMING);
		uint64_t t = 1000 * S;
		// NETWORK is retryable even when the caller did not say so.
		fire(r, ev(CONV_EV_WARM_FAILED, t, 0, XRT_CONV_FAIL_NETWORK, false));
		CHECK(r.retryable);
		const uint64_t expect[] = {15, 30, 60, 120, 240, 480, 600, 600};
		for (uint64_t step : expect) {
			CAPTURE(step);
			CHECK(conv_readiness_next_deadline(&r) == t + step * S);
			CHECK((conv_readiness_tick(&r, t + step * S - 1) & CONV_RD_IGNORED) != 0);
			t += step * S;
			uint32_t bits = conv_readiness_tick(&r, t);
			CHECK(r.state == XRT_CONV_STATE_INSTALLED);
			CHECK((bits & CONV_RD_ACT_WARM) != 0);
			fire(r, ev(CONV_EV_WARM_REQUESTED, t));
			fire(r, ev(CONV_EV_WARM_FAILED, t, 0, XRT_CONV_FAIL_NETWORK, true));
		}
		// A success resets the backoff.
		t += 600 * S;
		conv_readiness_tick(&r, t);
		fire(r, ev(CONV_EV_WARM_REQUESTED, t));
		fire(r, ev(CONV_EV_WARM_OK, t));
		CHECK(r.state == XRT_CONV_STATE_READY);
		fire(r, ev(CONV_EV_HOST_CRASH, t));
		fire(r, ev(CONV_EV_WARM_FAILED, t, 0, XRT_CONV_FAIL_NETWORK, true));
		CHECK(conv_readiness_next_deadline(&r) == t + 15 * S);
	}
	SECTION("a non-retryable failure has no timer")
	{
		conv_readiness r = at(XRT_CONV_STATE_WARMING);
		fire(r, ev(CONV_EV_WARM_FAILED, 1000 * S, 0, XRT_CONV_FAIL_LICENCE, false));
		CHECK(conv_readiness_next_deadline(&r) == 0);
		CHECK((conv_readiness_tick(&r, 100000 * S) & CONV_RD_IGNORED) != 0);
	}
	SECTION("create for an unserved task: only when the failure is older than 30 s")
	{
		conv_readiness r = at(XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LOAD); // failed at 1000 s
		CHECK((fire(r, ev(CONV_EV_CREATE_REQUESTED, 1000 * S + 30 * S)) & CONV_RD_IGNORED) != 0);
		CHECK(r.state == XRT_CONV_STATE_FAILED);
		uint32_t bits = fire(r, ev(CONV_EV_CREATE_REQUESTED, 1000 * S + 31 * S));
		CHECK((bits & CONV_RD_CHANGED) != 0);
		CHECK(r.state == XRT_CONV_STATE_WARMING);
	}
	SECTION("create retry honours the warm flags (blobs missing -> DOWNLOADING)")
	{
		conv_readiness r = at(XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_NETWORK);
		fire(r, ev(CONV_EV_CREATE_REQUESTED, 1100 * S, CONV_EVF_MODEL_MISSING));
		CHECK(r.state == XRT_CONV_STATE_DOWNLOADING);
	}
	SECTION("explicit warm retries at once, whatever the age")
	{
		conv_readiness r = at(XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION);
		fire(r, ev(CONV_EV_WARM_REQUESTED, 1000 * S + 1));
		CHECK(r.state == XRT_CONV_STATE_WARMING);
	}
	SECTION("registry change retries every FAILED reason")
	{
		const uint32_t reasons[] = {XRT_CONV_FAIL_LICENCE, XRT_CONV_FAIL_VERSION, XRT_CONV_FAIL_MODEL,
		                            XRT_CONV_FAIL_RESOURCES, XRT_CONV_FAIL_CRASH_LOOP};
		for (uint32_t why : reasons) {
			CAPTURE(conv_reason_name(why));
			conv_readiness r = at(XRT_CONV_STATE_FAILED, why);
			uint32_t bits = fire(r, ev(CONV_EV_REGISTRY_CHANGED, 1001 * S));
			CHECK(r.state == XRT_CONV_STATE_INSTALLED);
			CHECK((bits & CONV_RD_ACT_REDESCRIBE) != 0);
		}
	}
	SECTION("FAILED(RESOURCES) waits for a budget change: no timer, no create retry")
	{
		conv_readiness r = at(XRT_CONV_STATE_INSTALLED);
		fire(r, ev(CONV_EV_WARM_REQUESTED, 1000 * S, CONV_EVF_EXCEEDS_MEMORY));
		CHECK(r.reason == XRT_CONV_FAIL_RESOURCES);
		CHECK(conv_readiness_next_deadline(&r) == 0);
		CHECK((fire(r, ev(CONV_EV_CREATE_REQUESTED, 5000 * S)) & CONV_RD_IGNORED) != 0);
		CHECK((fire(r, ev(CONV_EV_BUDGET_CHANGED, 5001 * S)) & CONV_RD_ACT_WARM) != 0);
		CHECK(r.state == XRT_CONV_STATE_INSTALLED);
	}
	SECTION("no latch: a provider absent at the first look comes back on a registry change")
	{
		conv_readiness r;
		conv_readiness_init(&r);
		fire(r, ev(CONV_EV_DESCRIBE_FAILED, S, 0, CONV_REASON_BINARY_MISSING));
		CHECK(r.state == XRT_CONV_STATE_ABSENT);
		fire(r, ev(CONV_EV_REGISTRY_CHANGED, 2 * S));
		fire(r, ev(CONV_EV_DESCRIBED, 3 * S));
		fire(r, ev(CONV_EV_WARM_REQUESTED, 4 * S));
		fire(r, ev(CONV_EV_WARM_OK, 5 * S));
		CHECK(r.state == XRT_CONV_STATE_READY);
	}
}

TEST_CASE("conv_readiness: a kill switch / module-declared ABSENT is terminal, never a retry loop (#2029)")
{
	// The panel's DXR_LIFT=0 shape: warm -> the provider says ABSENT, not
	// retryable. Before #2029 this was FAILED(model) + a 15 s backoff, so the
	// row cycled failed -> installed -> warming and a spec-3 caller read
	// ACTIVATING forever.
	conv_readiness r = at(XRT_CONV_STATE_INSTALLED);
	fire(r, ev(CONV_EV_WARM_REQUESTED, 1000 * S));
	REQUIRE(r.state == XRT_CONV_STATE_WARMING);
	CHECK((fire(r, ev(CONV_EV_DECLARED_ABSENT, 1001 * S)) & CONV_RD_CHANGED) != 0);
	CHECK(r.state == XRT_CONV_STATE_ABSENT);
	CHECK(r.reason == CONV_REASON_POLICY);
	CHECK_FALSE(r.retryable);
	CHECK(conv_readiness_next_deadline(&r) == 0); // no backoff timer at all
	CHECK(conv_state_is_terminal(r.state));

	// Nothing but a registry change (or an explicit warm, which the core maps
	// to one) moves it: not time, not a create, not a re-describe.
	const uint64_t gen0 = r.generation;
	const uint32_t idle[] = {CONV_EV_TIMER, CONV_EV_CREATE_REQUESTED, CONV_EV_DESCRIBED, CONV_EV_DECLARED_ABSENT};
	for (uint32_t e : idle) {
		CAPTURE(e);
		fire(r, ev(e, 5000 * S));
		CHECK(r.state == XRT_CONV_STATE_ABSENT);
		CHECK(r.generation == gen0);
	}

	// Spec-3: the only backing task is terminal and discovery settled -> UNAVAILABLE.
	const uint32_t states[] = {r.state};
	CHECK(conv_legacy_v3_state(states, 1, true) == CONV_V3_STATE_UNAVAILABLE);

	// A registry change (the switch flipped back, a module installed) tries again.
	fire(r, ev(CONV_EV_REGISTRY_CHANGED, 5001 * S));
	CHECK(r.state == XRT_CONV_STATE_INSTALLED);
	const uint32_t again[] = {r.state};
	CHECK(conv_legacy_v3_state(again, 1, true) == CONV_V3_STATE_ACTIVATING);
}

TEST_CASE("conv_readiness: a disabled row ignores everything but enable")
{
	conv_readiness r = at(XRT_CONV_STATE_DISABLED);
	const uint32_t events[] = {CONV_EV_DESCRIBED,        CONV_EV_DESCRIBE_FAILED, CONV_EV_WARM_REQUESTED,
	                           CONV_EV_CREATE_REQUESTED, CONV_EV_HOST_CRASH,      CONV_EV_CRASH_LOOP,
	                           CONV_EV_REGISTRY_CHANGED, CONV_EV_TIMER,           CONV_EV_DISABLE,
	                           CONV_EV_DECLARED_ABSENT};
	for (uint32_t e : events) {
		CAPTURE(e);
		uint64_t gen0 = r.generation;
		fire(r, ev(e, 1001 * S));
		CHECK(r.state == XRT_CONV_STATE_DISABLED);
		CHECK(r.generation == gen0);
	}
	fire(r, ev(CONV_EV_ENABLE, 1002 * S));
	CHECK(r.state == XRT_CONV_STATE_INSTALLED);
}
