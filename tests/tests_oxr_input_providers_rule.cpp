// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_input_providers event discipline (ADR-034 Amendment 5, #2181):
 *        the setter queues XrEventDataInputProvidersChangedDXR only for a
 *        change xrSyncActions cannot see — never for a no-op, never twice.
 */

#include "catch_amalgamated.hpp"

#include "oxr_input_providers_rule.h"

#include <cstdio>
#include <cstring>

namespace {

pref_view
view(std::initializer_list<const char *> ids)
{
	pref_view v;
	std::memset(&v, 0, sizeof(v));
	for (const char *id : ids) {
		std::snprintf(v.id[v.n++], sizeof(v.id[0]), "%s", id);
	}
	return v;
}

} // namespace

TEST_CASE("input providers event: a no-op set is silent", "[oxr_input_providers_rule]")
{
	pref_view a = view({"hands", "ctl"});
	a.current[0] = 0x3;
	a.requested[1] = 0x1;
	pref_view b = a;
	CHECK_FALSE(oxr_input_providers_setter_should_notify(&a, &b));
}

TEST_CASE("input providers event: a holder move is left to xrSyncActions (one event, not two)",
          "[oxr_input_providers_rule]")
{
	pref_view a = view({"hands", "ctl"});
	a.current[0] = 0x3;
	pref_view b = a;
	b.current[0] = 0x2; // controllers left hands ...
	b.current[1] = 0x1; // ... for ctl
	b.requested[1] = 0x1;
	bool pref_changed = false;
	CHECK(diff_pref_view(&a, &b, &pref_changed) == 0x1);
	CHECK(pref_changed);
	CHECK_FALSE(oxr_input_providers_setter_should_notify(&a, &b));
}

TEST_CASE("input providers event: a request that moves no holder is announced by the setter",
          "[oxr_input_providers_rule]")
{
	// e.g. a request for a provider whose hardware is absent: the walk falls
	// through, the roles do not move, xrSyncActions sees nothing.
	pref_view a = view({"hands", "ctl"});
	a.current[0] = 0x3;
	pref_view b = a;
	b.requested[1] = 0x1;
	CHECK(oxr_input_providers_setter_should_notify(&a, &b));

	SECTION("a pin flip with no holder move, likewise")
	{
		pref_view c = a;
		c.pinned[1] = true;
		CHECK(oxr_input_providers_setter_should_notify(&a, &c));
	}
}

TEST_CASE("input providers event: a provider-set change counts as a holder move", "[oxr_input_providers_rule]")
{
	pref_view a = view({"hands", "ctl"});
	pref_view b = view({"hands", "pad"});
	bool pref_changed = false;
	CHECK(diff_pref_view(&a, &b, &pref_changed) == XRT_INPUT_ROLE_FAMILY_ALL_BITS);
	CHECK_FALSE(oxr_input_providers_setter_should_notify(&a, &b));
}
