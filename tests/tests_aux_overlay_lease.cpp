// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1277 P2(a) — the weave satellite's overlay arbitration policy.
 *
 * There is one panel, one full-panel `TYPE_APPLICATION_OVERLAY` worth owning,
 * and (ADR-036 D3) one satellite process per client. Who gets it is a policy
 * decision that needs a device to *observe* but not to be *right*: it is a
 * comparison over four booleans and a slot number. The sibling precedent is
 * tests_aux_route_policy.cpp — the same argument, for the same reason.
 *
 * The cases below are the acceptance tests of #1376 restated as facts about the
 * decision function, so a device run can distinguish "the policy is wrong" from
 * "the plumbing is wrong".
 */

#include "catch_amalgamated.hpp"

#include "util/u_overlay_lease.h"

#include <vector>

namespace {

//! A weaving client, unscaled, unfocused, not the incumbent.
u_overlay_lease_candidate
client(int32_t slot)
{
	u_overlay_lease_candidate c = {};
	c.slot = slot;
	c.wants_overlay = true;
	return c;
}

int
pick(const std::vector<u_overlay_lease_candidate> &v, u_overlay_lease_reason *reason = nullptr)
{
	return u_overlay_lease_select(v.data(), v.size(), reason);
}

} // namespace

TEST_CASE("overlay lease: nobody weaving, nobody holds the panel")
{
	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_ONLY_CANDIDATE;
	CHECK(u_overlay_lease_select(nullptr, 0, &why) == -1);
	CHECK(why == U_OVERLAY_LEASE_REASON_NONE);

	auto idle = client(0);
	idle.wants_overlay = false;
	CHECK(pick({idle}) == -1);
}

TEST_CASE("overlay lease: a lone client takes it even unscaled")
{
	// The shipped single-client case: a fullscreen browser is NOT scaled and
	// still presents through the satellite. If this ever returns -1 the lease
	// has silently disabled P0.
	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_NONE;
	CHECK(pick({client(0)}, &why) == 0);
	CHECK(why == U_OVERLAY_LEASE_REASON_ONLY_CANDIDATE);
}

TEST_CASE("overlay lease: the scaled window wins over the unscaled one")
{
	// #1376 acceptance test 1, the whole point of the lease: browser
	// fullscreen (unscaled, correct in its own surface) + a demo dragged into
	// the OEM mini-window (scaled, correct only on the overlay). Both correct
	// at the same time.
	auto fullscreen = client(0);
	auto mini = client(1);
	mini.container_scaled = true;

	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_NONE;
	CHECK(pick({fullscreen, mini}, &why) == 1);
	CHECK(why == U_OVERLAY_LEASE_REASON_SCALED);

	// Order of the array must not matter.
	CHECK(pick({mini, fullscreen}) == 0);

	// Even if the unscaled one is focused and holds it today.
	fullscreen.focused = true;
	fullscreen.incumbent = true;
	CHECK(pick({fullscreen, mini}) == 1);
}

TEST_CASE("overlay lease: two scaled windows are broken by focus")
{
	// #1376 acceptance test 2. The loser weaves in its own surface — i.e. the
	// double image it shows today. A stated degradation, not a silent one.
	auto a = client(0);
	auto b = client(1);
	a.container_scaled = b.container_scaled = true;
	b.focused = true;

	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_NONE;
	CHECK(pick({a, b}, &why) == 1);
	CHECK(why == U_OVERLAY_LEASE_REASON_FOCUS);

	// Focus moving moves the lease.
	a.focused = true;
	b.focused = false;
	CHECK(pick({a, b}) == 0);
}

TEST_CASE("overlay lease: no focus feed means the incumbent keeps it")
{
	// The a11y window watcher is OFF by default and dies on an `am force-stop`,
	// so "nobody is focused" is a legitimate steady state, not an error. The
	// policy must not thrash: a hand-off costs a synchronous overlay clear.
	auto a = client(0);
	auto b = client(1);
	a.container_scaled = b.container_scaled = true;
	b.incumbent = true;

	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_NONE;
	CHECK(pick({a, b}, &why) == 1);
	CHECK(why == U_OVERLAY_LEASE_REASON_INCUMBENT);

	// Repeated evaluation is stable — this is what "no thrash" means.
	for (int i = 0; i < 20; i++) {
		CHECK(pick({a, b}) == 1);
	}
}

TEST_CASE("overlay lease: with nothing to distinguish them, the lowest slot wins")
{
	// Only so that every process computes the same answer. The main-process
	// service (slot -1) sorts below :dxr0 and would win a genuine tie, which is
	// deliberate: it is the one candidate that is never a satellite.
	auto a = client(2);
	auto b = client(1);
	a.container_scaled = b.container_scaled = true;

	u_overlay_lease_reason why = U_OVERLAY_LEASE_REASON_NONE;
	CHECK(pick({a, b}, &why) == 1);
	CHECK(why == U_OVERLAY_LEASE_REASON_LOWEST_SLOT);

	auto main_proc = client(U_OVERLAY_LEASE_SLOT_MAIN);
	main_proc.container_scaled = true;
	CHECK(pick({a, b, main_proc}) == 2);
}

TEST_CASE("overlay lease: an idle client never takes the panel from a weaving one")
{
	auto weaving = client(1);
	weaving.container_scaled = true;
	auto stopped = client(0);
	stopped.wants_overlay = false;
	stopped.container_scaled = true;
	stopped.focused = true;
	stopped.incumbent = true;

	// This is the `91f071770` shape: a client that stopped submitting must not
	// keep the overlay just because it was focused when it stopped.
	CHECK(pick({stopped, weaving}) == 1);
}

TEST_CASE("overlay lease: default mechanism grants, which is today's behaviour")
{
	u_overlay_lease_set_backend(nullptr);
	const auto self = client(0);

	CHECK(u_overlay_lease_held(0) == false);
	CHECK(u_overlay_lease_acquire(0, &self) == true);
	CHECK(u_overlay_lease_held(0) == true);

	// Idempotent both ways.
	CHECK(u_overlay_lease_acquire(0, &self) == true);
	u_overlay_lease_release(0);
	u_overlay_lease_release(0);
	CHECK(u_overlay_lease_held(0) == false);

	// The main-process service is a tracked slot; out-of-range ones are not.
	const auto main_self = client(U_OVERLAY_LEASE_SLOT_MAIN);
	CHECK(u_overlay_lease_acquire(U_OVERLAY_LEASE_SLOT_MAIN, &main_self) == true);
	u_overlay_lease_release(U_OVERLAY_LEASE_SLOT_MAIN);
	CHECK(u_overlay_lease_acquire(U_OVERLAY_LEASE_MAX_SLOTS, &self) == false);
	CHECK(u_overlay_lease_acquire(-2, &self) == false);
}

TEST_CASE("overlay lease: a backend can refuse, and release is synchronous")
{
	static int acquires = 0;
	static int releases = 0;
	static bool grant = false;
	acquires = releases = 0;

	u_overlay_lease_backend backend = {};
	backend.acquire = [](void *, int32_t, const u_overlay_lease_candidate *) {
		acquires++;
		return grant;
	};
	backend.release = [](void *, int32_t) { releases++; };
	u_overlay_lease_set_backend(&backend);

	const auto self = client(1);
	CHECK(u_overlay_lease_acquire(1, &self) == false);
	CHECK(u_overlay_lease_held(1) == false);
	CHECK(acquires == 1);

	// A refused acquire must not have called release — there was nothing held.
	u_overlay_lease_release(1);
	CHECK(releases == 0);

	grant = true;
	CHECK(u_overlay_lease_acquire(1, &self) == true);
	CHECK(u_overlay_lease_held(1) == true);
	u_overlay_lease_release(1);
	CHECK(releases == 1);
	CHECK(u_overlay_lease_held(1) == false);

	u_overlay_lease_set_backend(nullptr);
}
