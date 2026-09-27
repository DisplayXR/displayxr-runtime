// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1744 — the desktop-Linux XR_DXR_weave present-owner routing rule.
 * @author David Fattal
 *
 * xrt_instance_create (hybrid target) sends a present-owner to the service
 * compositor by capability. v2.21.7 keyed that on XR_DXR_weave alone and so
 * also caught window-bound apps that enable weave only for the in-process
 * drag phase-snap — they became HOSTED service sessions whose own window
 * never mapped. The socket stat and the instance creation around the rule
 * need a service; the DECISION is pure, and that is what has to be right.
 */

#include "catch_amalgamated.hpp"

#include "util/u_sandbox.h"

static enum u_sandbox_present_owner_route
route(bool forced, bool weave, bool window_bound, bool socket)
{
	return u_sandbox_linux_present_owner_route(forced, weave, window_bound, socket);
}

TEST_CASE("present-owner route: weave and no window binding is a present-owner")
{
	// The browser / weave_present / weave_probe shape.
	CHECK(route(false, true, false, true) == U_SANDBOX_PRESENT_OWNER_SERVICE);
	CHECK(route(false, true, false, false) == U_SANDBOX_PRESENT_OWNER_NO_SERVICE);
}

TEST_CASE("present-owner route: a window-bound weave app is not a present-owner")
{
	// The Linux demos and cube_handle_vk_linux: xlib/wayland binding + weave
	// for xrWeaveSnapWindowRectDXR. The v2.21.7 regression was SERVICE here.
	CHECK(route(false, true, true, true) == U_SANDBOX_PRESENT_OWNER_WINDOW_BOUND);
	CHECK(route(false, true, true, false) == U_SANDBOX_PRESENT_OWNER_WINDOW_BOUND);
}

TEST_CASE("present-owner route: no weave leaves the generic rules in charge")
{
	for (bool window_bound : {false, true}) {
		for (bool socket : {false, true}) {
			CHECK(route(false, false, window_bound, socket) == U_SANDBOX_PRESENT_OWNER_NOT_APPLICABLE);
		}
	}
}

TEST_CASE("present-owner route: XRT_FORCE_MODE is authoritative either way")
{
	for (bool weave : {false, true}) {
		for (bool window_bound : {false, true}) {
			for (bool socket : {false, true}) {
				CHECK(route(true, weave, window_bound, socket) ==
				      U_SANDBOX_PRESENT_OWNER_NOT_APPLICABLE);
			}
		}
	}
}
