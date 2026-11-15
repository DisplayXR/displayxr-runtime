// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief ADR-034 Amendment 8: a single tool declared
 *        XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER lands on the user's dominant
 *        hand — right by default — and never inherits ANY_HAND's "left first".
 *        Also pins the host-iface slot the policy is offered through.
 */

#include "target_input_handedness.h"

#include "xrt/xrt_input_plugin.h"

#include "catch_amalgamated.hpp"

#include <cstddef>

TEST_CASE("a dominant-hand tool resolves by handedness", "[input][handedness]")
{
	CHECK(t_input_resolve_dominant_hand(XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER, XRT_INPUT_HANDEDNESS_RIGHT) ==
	      XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
	CHECK(t_input_resolve_dominant_hand(XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER, XRT_INPUT_HANDEDNESS_LEFT) ==
	      XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);
	// An out-of-range policy value is the default: right.
	CHECK(t_input_resolve_dominant_hand(XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER, (enum xrt_input_handedness)7) ==
	      XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
}

TEST_CASE("every other device type is untouched", "[input][handedness]")
{
	const enum xrt_device_type others[] = {
	    XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER,
	    XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER,  XRT_DEVICE_TYPE_GAMEPAD,
	    XRT_DEVICE_TYPE_NAVIGATION,           XRT_DEVICE_TYPE_GENERIC_TRACKER,
	};
	for (enum xrt_device_type t : others) {
		CHECK(t_input_resolve_dominant_hand(t, XRT_INPUT_HANDEDNESS_LEFT) == t);
		CHECK(t_input_resolve_dominant_hand(t, XRT_INPUT_HANDEDNESS_RIGHT) == t);
	}
}

TEST_CASE("the handedness host slot is carved from reserved[], size unchanged", "[input][handedness]")
{
	// ADR-020: the slot occupies the former reserved[8]; the struct size is
	// what every provider built against an older header allocated.
	STATIC_REQUIRE(offsetof(struct xrt_input_plugin_host_iface, get_handedness) ==
	               offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) + 8 * sizeof(void *));
	STATIC_REQUIRE(sizeof(struct xrt_input_plugin_host_iface) == 2 * sizeof(uint32_t) + 14 * sizeof(void *));
	STATIC_REQUIRE(XRT_INPUT_HANDEDNESS_RIGHT == 0);
	STATIC_REQUIRE(XRT_INPUT_HANDEDNESS_LEFT == 1);
	// Append-only enum values (xrt_defines.h / xrt_tracking.h).
	STATIC_REQUIRE(XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER == XRT_DEVICE_TYPE_NAVIGATION + 1);
}
