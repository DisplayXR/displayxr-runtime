// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  aux_os Hyprland parsing (os_hyprland_parse.c): the plain-text
 *         `monitors` reply the panel lookup reads on Omarchy, the event filter
 *         the geometry bridge uses, and session detection.
 */

#include "catch_amalgamated.hpp"

#include "os/os_hyprland.h"

#include <cstring>

namespace {

// Same text as tests/data/hyprland/monitors.txt (kept inline so this test
// needs no fixture path).
const char *MONITORS_TEXT = "Monitor DP-1 (ID 0):\n"
                            "\t2560x1600@60.00000 at 0x0\n"
                            "\tdescription: Leia Inc. 3D panel (fixture)\n"
                            "\tscale: 1.00\n"
                            "\ttransform: 0\n"
                            "\tdisabled: false\n"
                            "\n"
                            "Monitor eDP-1 (ID 1):\r\n"
                            "\t2880x1800@120.00000 at 2560x0\r\n"
                            "\tscale: 1.50\r\n"
                            "\ttransform: 0\r\n"
                            "\tdisabled: false\r\n"
                            "\n"
                            "Monitor DP-2 (ID 2):\n"
                            "\t2560x1600@59.95100 at -1280x0\n"
                            "\tscale: 1.25\n"
                            "\ttransform: 1\n"
                            "\tdisabled: true\n";

} // namespace

TEST_CASE("os_hyprland: monitors text")
{
	os_hyprland_monitor m[8];
	const uint32_t n = os_hyprland_parse_monitors_text(MONITORS_TEXT, m, 8);
	REQUIRE(n == 3);

	CHECK(std::strcmp(m[0].name, "DP-1") == 0);
	CHECK(m[0].mode_w == 2560);
	CHECK(m[0].mode_h == 1600);
	CHECK(m[0].refresh_mhz == 60000);
	CHECK(m[0].scale == 1.0);
	CHECK(m[0].transform == 0);
	CHECK_FALSE(m[0].disabled);

	CHECK(std::strcmp(m[1].name, "eDP-1") == 0); // CRLF tolerated
	CHECK(m[1].x == 2560);
	CHECK(m[1].scale == 1.5);
	CHECK(m[1].refresh_mhz == 120000);

	CHECK(std::strcmp(m[2].name, "DP-2") == 0);
	CHECK(m[2].x == -1280);
	CHECK(m[2].transform == 1);
	CHECK(m[2].refresh_mhz == 59951);
	CHECK(m[2].disabled);
}

TEST_CASE("os_hyprland: monitors text, limits and junk")
{
	os_hyprland_monitor m[1];
	CHECK(os_hyprland_parse_monitors_text(MONITORS_TEXT, m, 1) == 1);
	CHECK(os_hyprland_parse_monitors_text("", m, 1) == 0);
	CHECK(os_hyprland_parse_monitors_text(nullptr, m, 1) == 0);
	CHECK(os_hyprland_parse_monitors_text("unknown request\n", m, 1) == 0);
	// A block without a mode line is dropped rather than half-filled.
	CHECK(os_hyprland_parse_monitors_text("Monitor X-1 (ID 0):\n\tscale: 2.00\n", m, 1) == 0);
}

TEST_CASE("os_hyprland: which events re-query geometry")
{
	CHECK(os_hyprland_event_moves_windows("openwindow>>5f3a1c0a2b10,1,cube,DisplayXR cube"));
	CHECK(os_hyprland_event_moves_windows("closewindow>>5f3a1c0a2b10"));
	CHECK(os_hyprland_event_moves_windows("movewindowv2>>5f3a1c0a2b10,2,2"));
	CHECK(os_hyprland_event_moves_windows("changefloatingmode>>5f3a1c0a2b10,1"));
	CHECK(os_hyprland_event_moves_windows("workspacev2>>2,2"));
	CHECK(os_hyprland_event_moves_windows("openlayer>>waybar")); // reserved area moves tiled windows
	CHECK(os_hyprland_event_moves_windows("some-future-event>>x")); // unknown = re-query

	CHECK_FALSE(os_hyprland_event_moves_windows("windowtitlev2>>5f3a1c0a2b10,new title"));
	CHECK_FALSE(os_hyprland_event_moves_windows("activelayout>>kbd,English (US)"));
	CHECK_FALSE(os_hyprland_event_moves_windows("submap>>resize"));
	CHECK_FALSE(os_hyprland_event_moves_windows("createworkspacev2>>4,4"));
	CHECK_FALSE(os_hyprland_event_moves_windows(""));
	// Prefix of a quiet name is not that name.
	CHECK(os_hyprland_event_moves_windows("windowtitlez>>x"));
}

TEST_CASE("os_hyprland: session detection")
{
	CHECK(os_hyprland_session_detected("Hyprland", nullptr));
	CHECK(os_hyprland_session_detected("hyprland:uwsm", nullptr));
	CHECK(os_hyprland_session_detected(nullptr, "abc123_1732500000_123"));
	CHECK_FALSE(os_hyprland_session_detected("GNOME", nullptr));
	CHECK_FALSE(os_hyprland_session_detected("KDE", ""));
	CHECK_FALSE(os_hyprland_session_detected("Hyprlandx", nullptr));
	CHECK_FALSE(os_hyprland_session_detected(nullptr, nullptr));
}

#if defined(__linux__)
#include "os/os_display_connector_linux.h"

// A Steam Deck (or any portrait-native panel) under Hyprland: the `monitors`
// mode line is the scanout mode (800x1280), and the transform is the
// wl_output one. The panel lookup hands that transform to the same mapping the
// Mutter path uses (#2844), so the quarter turn must survive the parse.
TEST_CASE("os_hyprland: a quarter-turned panel reaches the rotation mapping")
{
	const char *deck = "Monitor eDP-1 (ID 0):\n"
	                   "\t800x1280@90.00000 at 0x0\n"
	                   "\tscale: 1.00\n"
	                   "\ttransform: 3\n"
	                   "\tdisabled: false\n";
	os_hyprland_monitor m[1];
	REQUIRE(os_hyprland_parse_monitors_text(deck, m, 1) == 1);
	CHECK(m[0].mode_w == 800);
	CHECK(m[0].mode_h == 1280);
	CHECK(m[0].transform == 3);
	CHECK(os_display_rotation_from_mutter(m[0].transform) == OS_DISPLAY_ROTATION_270);
	CHECK(os_display_rotation_is_quarter(os_display_rotation_from_mutter(m[0].transform)));

	// Flipped-and-turned (5 = flipped 90) is still a quarter turn.
	CHECK(os_display_rotation_is_quarter(os_display_rotation_from_mutter(5)));

	// A transform Hyprland never sends is unknown, not "no rotation".
	const char *odd = "Monitor DP-1 (ID 0):\n"
	                  "\t2560x1600@60.00000 at 0x0\n"
	                  "\ttransform: 9\n";
	REQUIRE(os_hyprland_parse_monitors_text(odd, m, 1) == 1);
	CHECK(os_display_rotation_from_mutter(m[0].transform) == OS_DISPLAY_ROTATION_UNKNOWN);
}
#endif
