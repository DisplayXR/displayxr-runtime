// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DXR_SCREEN_PLUGIN (per-screen plug-in pin) parser and the
 *         per-monitor winner rule of the DP registry (multi-screen M4).
 *
 * target_plugin_resolve_displays() needs loaded plug-ins and monitors; the
 * decision it takes per monitor is target_screen_pick(), pinned here together
 * with the pin parser and matcher.
 */

#include "catch_amalgamated.hpp"

#include "target_screen_pin.h"

#include <cstring>

TEST_CASE("DXR_SCREEN_PLUGIN parses match=id pairs", "[target][screen_pin]")
{
	target_screen_pins p;
	uint32_t bad = 99;

	CHECK(target_screen_pin_parse(nullptr, &p, &bad) == 0);
	CHECK(bad == 0);
	CHECK(target_screen_pin_parse("", &p, &bad) == 0);
	CHECK(bad == 0);

	REQUIRE(target_screen_pin_parse("HDMI-1=leia-sr", &p, &bad) == 1);
	CHECK(std::strcmp(p.pin[0].match, "HDMI-1") == 0);
	CHECK(std::strcmp(p.pin[0].plugin_id, "leia-sr") == 0);
	CHECK(bad == 0);

	REQUIRE(target_screen_pin_parse(" HDMI-1 = leia-sr , eDP-1=sim-display,", &p, &bad) == 2);
	CHECK(std::strcmp(p.pin[0].plugin_id, "leia-sr") == 0);
	CHECK(std::strcmp(p.pin[1].match, "eDP-1") == 0);
	CHECK(std::strcmp(p.pin[1].plugin_id, "sim-display") == 0);
	CHECK(bad == 0);

	// Malformed entries are skipped and counted; good ones survive.
	REQUIRE(target_screen_pin_parse("HDMI-1,=leia-sr,eDP-1=,DP-2=sim-display", &p, &bad) == 1);
	CHECK(std::strcmp(p.pin[0].match, "DP-2") == 0);
	CHECK(bad == 3);
}

TEST_CASE("a pin matches by output, connector or monitor id", "[target][screen_pin]")
{
	target_screen_pin pin;
	std::memset(&pin, 0, sizeof(pin));
	std::strcpy(pin.plugin_id, "leia-sr");

	std::strcpy(pin.match, "HDMI-1");
	CHECK(target_screen_pin_matches(&pin, 0x1234, "HDMI-1", "HDMI-A-1"));
	CHECK(target_screen_pin_matches(&pin, 0x1234, "hdmi-1", ""));
	CHECK_FALSE(target_screen_pin_matches(&pin, 0x1234, "HDMI-2", "HDMI-A-1"));
	CHECK_FALSE(target_screen_pin_matches(&pin, 0x1234, nullptr, nullptr));

	std::strcpy(pin.match, "HDMI-A-1");
	CHECK(target_screen_pin_matches(&pin, 0x1234, "HDMI-1", "HDMI-A-1"));

	std::strcpy(pin.match, "0x886e4475353b22b9");
	CHECK(target_screen_pin_matches(&pin, 0x886e4475353b22b9ULL, "", ""));
	std::strcpy(pin.match, "886E4475353B22B9");
	CHECK(target_screen_pin_matches(&pin, 0x886e4475353b22b9ULL, "", ""));
	CHECK_FALSE(target_screen_pin_matches(&pin, 0x03fdab50663276e4ULL, "", ""));
	std::strcpy(pin.match, "eDP-1"); // not hex: never an id match
	CHECK_FALSE(target_screen_pin_matches(&pin, 0xed1, "", ""));

	target_screen_pins pins;
	REQUIRE(target_screen_pin_parse("eDP-1=sim-display,HDMI-1=leia-sr", &pins, nullptr) == 2);
	CHECK(target_screen_pin_find(&pins, 1, "HDMI-1", "HDMI-A-1") == 1);
	CHECK(target_screen_pin_find(&pins, 2, "eDP-1", "eDP-1") == 0);
	CHECK(target_screen_pin_find(&pins, 3, "DP-3", "DP-3") == -1);
}

TEST_CASE("winner rule: pin > PreferredPlugin > active > confidence", "[target][screen_pin]")
{
	// The DS1 on HDMI-1 as ds1-linux sees it: sim claims it at FALLBACK (lower
	// ProbeOrder here), leia-sr claims it VERIFIED.
	const target_screen_candidate hdmi[] = {
	    {"sim-display", 10, true},
	    {"leia-sr", 100, false},
	};
	target_screen_pick_reason why;
	bool unclaimed = true;

	// No pin, sim preferred (mode B): sim wins — the #791 rule, unchanged.
	CHECK(target_screen_pick(hdmi, 2, nullptr, "sim-display", &why, &unclaimed) == 0);
	CHECK(why == TARGET_SCREEN_PICK_PREFERRED);
	CHECK_FALSE(unclaimed);

	// The pin outranks the preferred plug-in for THIS monitor.
	CHECK(target_screen_pick(hdmi, 2, "leia-sr", "sim-display", &why, &unclaimed) == 1);
	CHECK(why == TARGET_SCREEN_PICK_PIN);
	CHECK_FALSE(unclaimed);

	// ...and the active one.
	CHECK(target_screen_pick(hdmi, 2, "leia-sr", nullptr, &why, &unclaimed) == 1);
	CHECK(why == TARGET_SCREEN_PICK_PIN);

	// A pin naming a plug-in without a claim is ignored (and reported).
	CHECK(target_screen_pick(hdmi, 2, "other-vendor", "sim-display", &why, &unclaimed) == 0);
	CHECK(why == TARGET_SCREEN_PICK_PREFERRED);
	CHECK(unclaimed);

	// No pin, no preferred: the active plug-in (#1521).
	CHECK(target_screen_pick(hdmi, 2, nullptr, nullptr, &why, &unclaimed) == 0);
	CHECK(why == TARGET_SCREEN_PICK_ACTIVE);

	// Nobody active or preferred: confidence; a tie keeps the earlier source.
	const target_screen_candidate plain[] = {
	    {"a", 50, false},
	    {"b", 100, false},
	    {"c", 100, false},
	};
	CHECK(target_screen_pick(plain, 3, nullptr, nullptr, &why, &unclaimed) == 1);
	CHECK(why == TARGET_SCREEN_PICK_CONFIDENCE);

	// A preferred plug-in without a claim falls through.
	CHECK(target_screen_pick(plain, 3, nullptr, "zzz", &why, &unclaimed) == 1);
	CHECK(why == TARGET_SCREEN_PICK_CONFIDENCE);

	// No candidates at all.
	CHECK(target_screen_pick(nullptr, 0, "leia-sr", nullptr, &why, &unclaimed) == -1);
	CHECK(why == TARGET_SCREEN_PICK_NONE);
	CHECK(unclaimed);
}
