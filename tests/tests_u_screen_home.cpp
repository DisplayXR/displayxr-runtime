// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2305: a live session's home screen across display hotplugs
 *         (u_screen_home) — the SYSTEM_DEFAULT flag the D3D11 / D3D12 / GL /
 *         Vulkan segment managers read as "the screen the session's own DP
 *         weaves" never moves off the panel that DP was made for.
 */

#include "catch_amalgamated.hpp"

#include "util/u_screen_home.h"

#include <cstdio>
#include <cstring>

namespace {

void
add(xrt_screen_list &l, uint64_t id, const char *plugin, int32_t left, bool is_default)
{
	xrt_screen &s = l.screens[l.count++];
	std::memset(&s, 0, sizeof(s));
	s.id = id;
	s.desktop_left = left;
	s.desktop_width = 1920;
	s.desktop_height = 1080;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	s.flags = is_default ? XRT_SCREEN_FLAG_SYSTEM_DEFAULT : 0u;
}

uint32_t
defaults(const xrt_screen_list &l, uint64_t *out_id)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < l.count; i++) {
		if ((l.screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0) {
			n++;
			*out_id = l.screens[i].id;
		}
	}
	return n;
}

constexpr uint64_t LAPTOP = 0x1001;
constexpr uint64_t PANEL = 0x2002;
constexpr uint64_t PANEL2 = 0x2003;
constexpr uint64_t VIRTUAL = 0x3003; // a casting tool's "extend" monitor: no EDID, nobody's claim

} // namespace

TEST_CASE("screen home: recorded from the first list's system default")
{
	xrt_screen_list l{};
	add(l, LAPTOP, "sim-display", 0, false);
	add(l, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &l);
	CHECK(h.valid);
	CHECK(h.screen_id == PANEL);
	CHECK(std::strcmp(h.plugin_id, "vendor") == 0);

	xrt_screen_list none{};
	add(none, LAPTOP, "sim-display", 0, false);
	u_screen_home_record(&h, &none);
	CHECK_FALSE(h.valid);
	CHECK(u_screen_home_anchor(&h, &l) == U_SCREEN_HOME_UNRECORDED);
}

TEST_CASE("screen home: a virtual monitor appearing and disappearing never takes the session DP's screen")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// Appears: listed, unclaimed (sim-display's fallback), and — as the
	// worst case — flagged system default by the screen builder.
	xrt_screen_list with_virtual{};
	add(with_virtual, VIRTUAL, "sim-display", 5760, true);
	add(with_virtual, LAPTOP, "sim-display", 0, false);
	add(with_virtual, PANEL, "vendor", 1920, false);
	CHECK(u_screen_home_anchor(&h, &with_virtual) == U_SCREEN_HOME_REANCHORED);
	uint64_t def = 0;
	CHECK(defaults(with_virtual, &def) == 1);
	CHECK(def == PANEL);

	// Disappears: back to the first list, untouched.
	xrt_screen_list back = first;
	CHECK(u_screen_home_anchor(&h, &back) == U_SCREEN_HOME_IN_PLACE);
	CHECK(std::memcmp(&back, &first, sizeof(back)) == 0);
}

TEST_CASE("screen home: a second claimed panel plugged in does not move the session DP")
{
	xrt_screen_list first{};
	add(first, PANEL, "vendor", 0, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	xrt_screen_list two{};
	add(two, PANEL2, "vendor", 3840, true);
	add(two, PANEL, "vendor", 0, false);
	CHECK(u_screen_home_anchor(&h, &two) == U_SCREEN_HOME_REANCHORED);
	uint64_t def = 0;
	REQUIRE(defaults(two, &def) == 1);
	CHECK(def == PANEL);
}

TEST_CASE("screen home: the panel unplugged — no default at all, so no segment DP for it; replugged — restored")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// Unplugged; a virtual monitor now sits where the panel was.
	xrt_screen_list gone{};
	add(gone, LAPTOP, "sim-display", 0, false);
	add(gone, VIRTUAL, "sim-display", 1920, true);
	CHECK(u_screen_home_anchor(&h, &gone) == U_SCREEN_HOME_GONE);
	uint64_t def = 0;
	CHECK(defaults(gone, &def) == 0);

	// Replugged (same screen id, same plug-in): home again.
	xrt_screen_list replug{};
	add(replug, LAPTOP, "sim-display", 0, true);
	add(replug, PANEL, "vendor", 1920, false);
	CHECK(u_screen_home_anchor(&h, &replug) == U_SCREEN_HOME_REANCHORED);
	REQUIRE(defaults(replug, &def) == 1);
	CHECK(def == PANEL);
}

TEST_CASE("screen home: the panel re-claimed by another plug-in is not the session DP's any more")
{
	xrt_screen_list first{};
	add(first, PANEL, "vendor", 0, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// The vendor service stopped: sim-display's fallback claim wins the monitor.
	xrt_screen_list reclaimed{};
	add(reclaimed, PANEL, "sim-display", 0, true);
	CHECK(u_screen_home_anchor(&h, &reclaimed) == U_SCREEN_HOME_RECLAIMED);
	uint64_t def = 0;
	CHECK(defaults(reclaimed, &def) == 0);
	CHECK(std::strcmp(u_screen_home_outcome_str(U_SCREEN_HOME_RECLAIMED), "re-claimed") == 0);
}
