// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A runtime hardware-2D hold restores the session's own choice, not 3D.
 *
 * Pins @ref u_display_mode_hold, the rule the vk_native compositor's desktop-Linux
 * refuse-rather-than-resample degrade (#1595) runs on. The rule matters more than
 * it looks: under the Leia srSDK on Linux (LeiaSR #266) the degrade's 2D request
 * takes the lens preference away from the weaver for good, so the degrade's
 * release is the ONLY thing that ever asks for the lens back, and it must ask for
 * what the app wanted rather than force 3D over an app that chose 2D.
 *
 * The harness plays the display processor: it records every hardware state the
 * compositor would forward, in order.
 */

#include "catch_amalgamated.hpp"

#include "util/u_display_mode_hold.h"

#include <vector>

namespace {

//! The compositor's two entry points, as vk_native wires them.
struct Harness
{
	u_display_mode_hold hold{};
	std::vector<bool> sent; //!< hardware 3D states forwarded to the DP, in order

	Harness()
	{
		u_display_mode_hold_init(&hold, true);
	}

	//! comp_vk_native_compositor_request_display_mode
	void
	session_request(bool want_3d)
	{
		if (u_display_mode_hold_request(&hold, want_3d)) {
			sent.push_back(want_3d);
		}
	}

	//! vk_linux_update_surface_not_1to1, on a transition
	void
	degrade(bool not_1to1)
	{
		sent.push_back(u_display_mode_hold_set(&hold, not_1to1));
	}
};

} // namespace

TEST_CASE("degrade restores the app's 2D, not 3D", "[display_mode_hold]")
{
	Harness h;
	h.session_request(true);  // xrBeginSession
	h.session_request(false); // app picks 2D
	h.degrade(true);          // surface stops being 1:1
	h.degrade(false);         // ... and is 1:1 again

	REQUIRE(h.sent == std::vector<bool>{true, false, false, false});
	CHECK_FALSE(h.hold.wanted_3d);
}

TEST_CASE("degrade restores the app's 3D", "[display_mode_hold]")
{
	Harness h;
	h.session_request(true);
	h.degrade(true);
	h.degrade(false);

	// Without the explicit 3D on release the lens would stay off for the rest
	// of the SR context under LeiaSR #266: nothing else asks for it back.
	REQUIRE(h.sent == std::vector<bool>{true, false, true});
}

TEST_CASE("requests during a degrade are recorded, not forwarded, and win at release", "[display_mode_hold]")
{
	Harness h;
	h.session_request(true);
	h.degrade(true);

	h.session_request(false); // V key while degraded
	h.session_request(true);  // and back
	h.session_request(false); // last word: 2D
	REQUIRE(h.sent == std::vector<bool>{true, false});

	h.degrade(false);
	REQUIRE(h.sent == std::vector<bool>{true, false, false});

	// The next request goes straight through again.
	h.session_request(true);
	REQUIRE(h.sent == std::vector<bool>{true, false, false, true});
}

TEST_CASE("a session that never asked still restores 3D", "[display_mode_hold]")
{
	Harness h; // no request yet: sessions begin 3D
	h.degrade(true);
	h.degrade(false);
	REQUIRE(h.sent == std::vector<bool>{false, true});
}
