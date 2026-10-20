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
#include <string>
#include <vector>

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

	// The pin's plug-in id is matched case-insensitively (typed by hand).
	CHECK(target_screen_pick(hdmi, 2, "LEIA-SR", "sim-display", &why, &unclaimed) == 1);
	CHECK(why == TARGET_SCREEN_PICK_PIN);

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

TEST_CASE("claim-source shortcut: the active plug-in alone decides only without a competing pin",
          "[target][screen_pin]")
{
	// ds1-linux: eDP-1 laptop panel + the DS1 on HDMI-1; sim-display active and
	// claiming both at FALLBACK, as it claims every monitor.
	target_screen_monitor mons[] = {
	    {0x1111, "eDP-1", "eDP-1", true},
	    {0x886e4475353b22b9ull, "HDMI-1", "HDMI-A-1", true},
	};
	target_screen_pins none;
	REQUIRE(target_screen_pin_parse(nullptr, &none, nullptr) == 0);

	// Nothing outranks the active plug-in: the shortcut holds, whatever the
	// confidence of its claims (rule 3 is confidence-blind, #1521 — see the
	// "No pin, no preferred" case above: sim at FALLBACK beats leia VERIFIED).
	CHECK(target_screen_active_decides_every_monitor("sim-display", nullptr, &none, mons, 2));
	CHECK(target_screen_active_decides_every_monitor("sim-display", "sim-display", &none, mons, 2));

	// A different PreferredPlugin may outrank it (efa3f88d0's original guard).
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", "leia-sr", &none, mons, 2));

	// A monitor the active plug-in does not claim: another plug-in may.
	mons[1].active_claims = false;
	CHECK_FALSE(target_screen_active_decides_every_monitor("leia-sr", nullptr, &none, mons, 2));
	mons[1].active_claims = true;

	// THE BUG: the documented sim-session-with-a-woven-DS1 recipe,
	// XRT_PREFERRED_PLUGIN_ID=sim-display DXR_SCREEN_PLUGIN=HDMI-1=leia-sr.
	// The pin outranks sim on HDMI-1, so leia-sr must be loaded for its claim.
	target_screen_pins pins;
	REQUIRE(target_screen_pin_parse("HDMI-1=leia-sr", &pins, nullptr) == 1);
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", "sim-display", &pins, mons, 2));
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));
	// By connector and by monitor id too.
	REQUIRE(target_screen_pin_parse("hdmi-a-1=leia-sr", &pins, nullptr) == 1);
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));
	REQUIRE(target_screen_pin_parse("0x886e4475353b22b9=leia-sr", &pins, nullptr) == 1);
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));

	// A pin naming the active plug-in itself (any case) changes nothing.
	REQUIRE(target_screen_pin_parse("HDMI-1=SIM-DISPLAY", &pins, nullptr) == 1);
	CHECK(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));

	// A pin for a monitor that is not connected cannot change this resolve.
	REQUIRE(target_screen_pin_parse("DP-3=leia-sr", &pins, nullptr) == 1);
	CHECK(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));

	// No active plug-in, or no monitors: never short-circuit.
	CHECK_FALSE(target_screen_active_decides_every_monitor(nullptr, nullptr, &none, mons, 2));
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &none, mons, 0));
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &none, nullptr, 2));
}


/*
 *
 * Display dashboard phase 7: the per-screen display-processor preference.
 *
 */

TEST_CASE("per-screen preference: the resolver rule over a synthetic two-monitor registry", "[target][screen_pin]")
{
	// Two Leia panels; leia-sr is active and claims both VERIFIED, sim-display
	// claims every monitor at FALLBACK (sources in ProbeOrder: leia 50, sim 200).
	const target_screen_candidate laptop[] = {
	    {"leia-sr", 100, true},
	    {"sim-display", 10, false},
	};
	const target_screen_candidate ds1[] = {
	    {"leia-sr", 100, true},
	    {"sim-display", 10, false},
	};
	target_screen_pick_reason why;
	bool pin_unclaimed = true;
	bool pref_unclaimed = true;

	// Preference "sim on DS1": DS1 -> sim at its FALLBACK claim, over the
	// active plug-in's VERIFIED one (#1521 yields to the explicit preference)...
	CHECK(target_screen_pick_ex(ds1, 2, nullptr, "sim-display", nullptr, &why, &pin_unclaimed, &pref_unclaimed) ==
	      1);
	CHECK(why == TARGET_SCREEN_PICK_SCREEN_PREF);
	CHECK_FALSE(pin_unclaimed);
	CHECK_FALSE(pref_unclaimed);
	// ...while the laptop, with no preference, stays with the active plug-in.
	CHECK(target_screen_pick_ex(laptop, 2, nullptr, nullptr, nullptr, &why, &pin_unclaimed, &pref_unclaimed) == 0);
	CHECK(why == TARGET_SCREEN_PICK_ACTIVE);
	CHECK_FALSE(pref_unclaimed);

	// The preference id matches case-insensitively (written by hand / a UI).
	CHECK(target_screen_pick_ex(ds1, 2, nullptr, "Sim-Display", nullptr, &why, nullptr, nullptr) == 1);
	CHECK(why == TARGET_SCREEN_PICK_SCREEN_PREF);

	// It outranks the global PreferredPlugin for that screen.
	CHECK(target_screen_pick_ex(ds1, 2, nullptr, "sim-display", "leia-sr", &why, nullptr, nullptr) == 1);
	CHECK(why == TARGET_SCREEN_PICK_SCREEN_PREF);
	CHECK(target_screen_pick_ex(ds1, 2, nullptr, "leia-sr", "sim-display", &why, nullptr, nullptr) == 0);
	CHECK(why == TARGET_SCREEN_PICK_SCREEN_PREF);

	// A preference for a plug-in with no claim on the monitor (not loaded, or
	// it does not claim it) is ignored and reported; the normal rules decide.
	CHECK(target_screen_pick_ex(ds1, 2, nullptr, "other-vendor", nullptr, &why, &pin_unclaimed, &pref_unclaimed) ==
	      0);
	CHECK(why == TARGET_SCREEN_PICK_ACTIVE);
	CHECK(pref_unclaimed);
	CHECK_FALSE(pin_unclaimed);
	const target_screen_candidate only_sim[] = {
	    {"sim-display", 10, false},
	};
	CHECK(target_screen_pick_ex(only_sim, 1, nullptr, "leia-sr", nullptr, &why, nullptr, &pref_unclaimed) == 0);
	CHECK(why == TARGET_SCREEN_PICK_CONFIDENCE);
	CHECK(pref_unclaimed);

	// A DXR_SCREEN_PLUGIN pin (bring-up knob) still outranks it, and an
	// unclaimed preference is still reported under a pin.
	CHECK(target_screen_pick_ex(ds1, 2, "leia-sr", "sim-display", nullptr, &why, &pin_unclaimed, &pref_unclaimed) ==
	      0);
	CHECK(why == TARGET_SCREEN_PICK_PIN);
	CHECK_FALSE(pref_unclaimed);
	CHECK(target_screen_pick_ex(ds1, 2, "leia-sr", "zzz", nullptr, &why, nullptr, &pref_unclaimed) == 0);
	CHECK(pref_unclaimed);

	// No candidates: the preference is unclaimed.
	CHECK(target_screen_pick_ex(nullptr, 0, nullptr, "sim-display", nullptr, &why, nullptr, &pref_unclaimed) == -1);
	CHECK(why == TARGET_SCREEN_PICK_NONE);
	CHECK(pref_unclaimed);

	// target_screen_pick is the same rule without a preference.
	CHECK(target_screen_pick(ds1, 2, nullptr, nullptr, &why, nullptr) == 0);
	CHECK(why == TARGET_SCREEN_PICK_ACTIVE);
}

TEST_CASE("per-screen preference: the claim-source shortcut loads the others for it", "[target][screen_pin]")
{
	target_screen_monitor mons[] = {
	    {0x1111, "eDP-1", "eDP-1", true, nullptr},
	    {0x2222, "HDMI-1", "HDMI-A-1", true, nullptr},
	};
	target_screen_pins none;
	REQUIRE(target_screen_pin_parse(nullptr, &none, nullptr) == 0);
	CHECK(target_screen_active_decides_every_monitor("leia-sr", nullptr, &none, mons, 2));
	// A preference for another plug-in on one monitor needs its claim.
	mons[1].screen_pref = "sim-display";
	CHECK_FALSE(target_screen_active_decides_every_monitor("leia-sr", nullptr, &none, mons, 2));
	// One naming the active plug-in itself (any case) changes nothing.
	mons[1].screen_pref = "LEIA-SR";
	CHECK(target_screen_active_decides_every_monitor("leia-sr", nullptr, &none, mons, 2));
	mons[1].screen_pref = "";
	CHECK(target_screen_active_decides_every_monitor("leia-sr", nullptr, &none, mons, 2));
}

TEST_CASE("per-screen candidates: every plug-in that claimed the monitor, in source order", "[target][screen_pin]")
{
	// Two monitors. leia-sr (ProbeOrder 50, active) claims only the laptop;
	// sim-display (ProbeOrder 200) claims both at FALLBACK. A third source
	// (vendor2) claims nothing here.
	const uint64_t laptop = 0x1111, ds1 = 0x2222;
	const uint64_t leia_mon[] = {laptop};
	const uint32_t leia_conf[] = {100};
	const uint64_t sim_mon[] = {laptop, ds1};
	const uint32_t sim_conf[] = {10, 10};
	const target_screen_source_claims sources[] = {
	    {"leia-sr", true, leia_mon, leia_conf, 1},
	    {"vendor2", false, nullptr, nullptr, 0},
	    {"sim-display", false, sim_mon, sim_conf, 2},
	};
	target_screen_candidate out[4];
	uint32_t src[4];
	uint32_t claim[4];

	// The laptop: two claimants, in source order, with their confidences.
	REQUIRE(target_screen_collect_candidates(sources, 3, laptop, out, src, claim, 4) == 2);
	CHECK(std::string(out[0].plugin_id) == "leia-sr");
	CHECK(out[0].confidence == 100);
	CHECK(out[0].is_active);
	CHECK(src[0] == 0);
	CHECK(claim[0] == 0);
	CHECK(std::string(out[1].plugin_id) == "sim-display");
	CHECK(out[1].confidence == 10);
	CHECK_FALSE(out[1].is_active);
	CHECK(src[1] == 2);
	CHECK(claim[1] == 0);

	// DS1: only sim-display claimed it (its second claim).
	REQUIRE(target_screen_collect_candidates(sources, 3, ds1, out, src, claim, 4) == 1);
	CHECK(std::string(out[0].plugin_id) == "sim-display");
	CHECK(src[0] == 2);
	CHECK(claim[0] == 1);

	// The list feeds the winner rule unchanged: a preference for sim on the
	// laptop wins, one for leia-sr on DS1 is unclaimed there.
	REQUIRE(target_screen_collect_candidates(sources, 3, laptop, out, nullptr, nullptr, 4) == 2);
	target_screen_pick_reason why;
	CHECK(target_screen_pick_ex(out, 2, nullptr, "sim-display", nullptr, &why, nullptr, nullptr) == 1);
	CHECK(why == TARGET_SCREEN_PICK_SCREEN_PREF);
	bool pref_unclaimed = false;
	REQUIRE(target_screen_collect_candidates(sources, 3, ds1, out, nullptr, nullptr, 4) == 1);
	CHECK(target_screen_pick_ex(out, 1, nullptr, "leia-sr", nullptr, &why, nullptr, &pref_unclaimed) == 0);
	CHECK(pref_unclaimed);

	// One claim per source per monitor, the cap, an unknown monitor, NULLs.
	const uint64_t dup_mon[] = {ds1, ds1};
	const uint32_t dup_conf[] = {50, 90};
	const target_screen_source_claims dup[] = {{"dup", false, dup_mon, dup_conf, 2}};
	REQUIRE(target_screen_collect_candidates(dup, 1, ds1, out, nullptr, claim, 4) == 1);
	CHECK(out[0].confidence == 50);
	CHECK(claim[0] == 0);
	CHECK(target_screen_collect_candidates(sources, 3, laptop, out, nullptr, nullptr, 1) == 1);
	CHECK(target_screen_collect_candidates(sources, 3, 0x9999, out, nullptr, nullptr, 4) == 0);
	CHECK(target_screen_collect_candidates(nullptr, 3, laptop, out, nullptr, nullptr, 4) == 0);
	CHECK(target_screen_collect_candidates(sources, 3, laptop, nullptr, nullptr, nullptr, 4) == 0);
}

TEST_CASE("screen key: PNP-PROD-SERIAL, qualified on a zero serial or a collision", "[target][screen_key]")
{
	// "AUO" packed as the EDID stores it (little-endian): 0x06AF -> bytes AF 06.
	const uint16_t auo = 0xAF06;
	char pnp[4];
	target_screen_pnp_code(auo, pnp);
	CHECK(std::string(pnp) == "AUO");
	target_screen_pnp_code(0, pnp);
	CHECK(std::string(pnp) == "???");

	char keys[4][TARGET_SCREEN_KEY_MAX];

	SECTION("distinct serials: plain keys, stable whatever the device names")
	{
		const target_screen_key_input in[] = {
		    {auo, 0xB194, 0x0000ABCD, "\\\\.\\DISPLAY1"},
		    {auo, 0xB194, 0x0000ABCE, "\\\\.\\DISPLAY5"},
		};
		target_screen_keys_build(in, 2, keys);
		CHECK(std::string(keys[0]) == "AUO-B194-0000ABCD");
		CHECK(std::string(keys[1]) == "AUO-B194-0000ABCE");
	}

	SECTION("a zero serial is qualified with the device name")
	{
		const target_screen_key_input in[] = {
		    {auo, 0x1234, 0, "\\\\.\\DISPLAY2"},
		};
		target_screen_keys_build(in, 1, keys);
		CHECK(std::string(keys[0]) == "AUO-1234-00000000@DISPLAY2");
	}

	SECTION("two identical panels (same EDID serial): both qualified, so both distinct")
	{
		const target_screen_key_input in[] = {
		    {auo, 0x1234, 0x01010101, "\\\\.\\DISPLAY1"},
		    {auo, 0x1234, 0x01010101, "\\\\.\\DISPLAY3"},
		    {auo, 0x5678, 0x01010101, "HDMI-1"},
		};
		target_screen_keys_build(in, 3, keys);
		CHECK(std::string(keys[0]) == "AUO-1234-01010101@DISPLAY1");
		CHECK(std::string(keys[1]) == "AUO-1234-01010101@DISPLAY3");
		CHECK(std::string(keys[2]) == "AUO-5678-01010101"); // not part of the collision
		CHECK(std::string(keys[0]) != std::string(keys[1]));
	}

	SECTION("no device name: the base key stands")
	{
		const target_screen_key_input in[] = {
		    {auo, 0x1234, 0, nullptr},
		};
		target_screen_keys_build(in, 1, keys);
		CHECK(std::string(keys[0]) == "AUO-1234-00000000");
	}

	SECTION("a long device name never overflows the key")
	{
		const std::string longname(200, 'D');
		const target_screen_key_input in[] = {
		    {auo, 0x1234, 0, longname.c_str()},
		};
		target_screen_keys_build(in, 1, keys);
		CHECK(std::strlen(keys[0]) == TARGET_SCREEN_KEY_MAX - 1);
		CHECK(std::string(keys[0]).rfind("AUO-1234-00000000@D", 0) == 0);
	}
}

TEST_CASE("screen key: a vendor serial keys a zero-EDID-serial screen across inputs (#2250)", "[target][screen_key]")
{
	// "SAM" packed as the EDID stores it: 0x4C2D -> bytes 2D 4C.
	const uint16_t sam = 0x2D4C;
	char pnp[4];
	target_screen_pnp_code(sam, pnp);
	REQUIRE(std::string(pnp) == "SAM");

	char keys[4][TARGET_SCREEN_KEY_MAX];
	char legacy[4][TARGET_SCREEN_KEY_MAX];

	SECTION("the same panel on HDMI and on DisplayPort: one key, two legacy keys")
	{
		const target_screen_key_input hdmi[] = {{sam, 0x785F, 0, "\\\\.\\DISPLAY5", "VN022513ET0212"}};
		const target_screen_key_input dp[] = {{sam, 0x7860, 0, "\\\\.\\DISPLAY6", "VN022513ET0212"}};
		target_screen_keys_build_ex(hdmi, 1, keys, legacy);
		CHECK(std::string(keys[0]) == "SAM-VN022513ET0212");
		CHECK(std::string(legacy[0]) == "SAM-785F-00000000@DISPLAY5");
		const std::string on_hdmi = keys[0];
		target_screen_keys_build_ex(dp, 1, keys, legacy);
		CHECK(std::string(keys[0]) == on_hdmi);
		CHECK(std::string(legacy[0]) == "SAM-7860-00000000@DISPLAY6");
	}

	SECTION("no claim serial: today's EDID form, current == legacy")
	{
		const target_screen_key_input in[] = {
		    {sam, 0x785F, 0, "\\\\.\\DISPLAY5", nullptr},
		    {sam, 0x7860, 0, "\\\\.\\DISPLAY6", ""},
		};
		target_screen_keys_build_ex(in, 2, keys, legacy);
		CHECK(std::string(keys[0]) == "SAM-785F-00000000@DISPLAY5");
		CHECK(std::string(keys[1]) == "SAM-7860-00000000@DISPLAY6");
		CHECK(std::string(keys[0]) == std::string(legacy[0]));
		CHECK(std::string(keys[1]) == std::string(legacy[1]));
	}

	SECTION("a non-zero EDID serial keeps the EDID form whatever the claim serial")
	{
		const target_screen_key_input in[] = {{sam, 0x785F, 0x0000ABCD, "\\\\.\\DISPLAY5", "VN022513ET0212"}};
		target_screen_keys_build_ex(in, 1, keys, legacy);
		CHECK(std::string(keys[0]) == "SAM-785F-0000ABCD");
		CHECK(std::string(legacy[0]) == "SAM-785F-0000ABCD");
	}

	SECTION("the legacy form is exactly target_screen_keys_build")
	{
		const target_screen_key_input in[] = {
		    {sam, 0x785F, 0, "\\\\.\\DISPLAY5", "VN1"},
		    {sam, 0x785F, 0x01010101, "\\\\.\\DISPLAY1", nullptr},
		    {sam, 0x785F, 0x01010101, "\\\\.\\DISPLAY3", "VN2"},
		};
		char plain[4][TARGET_SCREEN_KEY_MAX];
		target_screen_keys_build(in, 3, plain);
		target_screen_keys_build_ex(in, 3, keys, legacy);
		for (int i = 0; i < 3; i++) {
			CHECK(std::string(plain[i]) == std::string(legacy[i]));
		}
		CHECK(std::string(keys[0]) == "SAM-VN1");
		CHECK(std::string(keys[1]) == "SAM-785F-01010101@DISPLAY1");
		CHECK(std::string(keys[2]) == "SAM-785F-01010101@DISPLAY3");
	}

	SECTION("two screens reporting the same vendor serial are both qualified")
	{
		const target_screen_key_input in[] = {
		    {sam, 0x785F, 0, "\\\\.\\DISPLAY5", "VN1"},
		    {sam, 0x7860, 0, "\\\\.\\DISPLAY6", "VN1"},
		};
		target_screen_keys_build_ex(in, 2, keys, nullptr);
		CHECK(std::string(keys[0]) == "SAM-VN1@DISPLAY5");
		CHECK(std::string(keys[1]) == "SAM-VN1@DISPLAY6");
	}

	SECTION("the serial is reduced to key-safe characters; nothing left = EDID form")
	{
		const target_screen_key_input in[] = {
		    {sam, 0x785F, 0, "\\\\.\\DISPLAY5", " -VN 02=25;13@ET 0212- "},
		    {sam, 0x7860, 0, "\\\\.\\DISPLAY6", " =;@ "},
		};
		target_screen_keys_build_ex(in, 2, keys, legacy);
		CHECK(std::string(keys[0]) == "SAM-VN022513ET0212");
		CHECK(std::string(keys[1]) == "SAM-7860-00000000@DISPLAY6");

		char out[TARGET_SCREEN_KEY_SERIAL_MAX + 1];
		const std::string longserial(200, 'A');
		CHECK(target_screen_key_sanitize_serial(longserial.c_str(), out, sizeof(out)));
		CHECK(std::strlen(out) == TARGET_SCREEN_KEY_SERIAL_MAX);
		CHECK_FALSE(target_screen_key_sanitize_serial(nullptr, out, sizeof(out)));
		CHECK(std::string(out).empty());
	}
}

TEST_CASE("screen key: which claim's serial keys the screen (#2250)", "[target][screen_key]")
{
	SECTION("the highest confidence above FALLBACK with a serial; ties to the earlier claim")
	{
		const uint32_t conf[] = {10, 50, 100, 100};
		const char *const ser[] = {"SIM", "EDIDSER", "VER1", "VER2"};
		CHECK(target_screen_key_serial_pick(conf, ser, 4) == 2);
	}
	SECTION("a FALLBACK claim's serial never keys a screen")
	{
		const uint32_t conf[] = {10};
		const char *const ser[] = {"SIM"};
		CHECK(target_screen_key_serial_pick(conf, ser, 1) == -1);
	}
	SECTION("a claim without a (usable) serial is skipped")
	{
		const uint32_t conf[] = {100, 50, 20};
		const char *const ser[] = {"", " ", "NR1"};
		CHECK(target_screen_key_serial_pick(conf, ser, 3) == 2);
	}
	SECTION("nothing to pick")
	{
		CHECK(target_screen_key_serial_pick(nullptr, nullptr, 0) == -1);
		const uint32_t conf[] = {100};
		const char *const ser[] = {""};
		CHECK(target_screen_key_serial_pick(conf, ser, 1) == -1);
	}
}

namespace {
struct FakeStore
{
	std::vector<std::string> keys;
};
bool
fake_has(const char *key, void *ud)
{
	const auto *s = static_cast<const FakeStore *>(ud);
	for (const auto &k : s->keys) {
		if (k == key) {
			return true;
		}
	}
	return false;
}
} // namespace

TEST_CASE("screen key: a preference stored under the legacy key still matches (#2250)", "[target][screen_key]")
{
	const char *cur = "SAM-VN022513ET0212";
	const char *old_hdmi = "SAM-785F-00000000@DISPLAY5";

	FakeStore none;
	CHECK(target_screen_key_pref_match(cur, old_hdmi, fake_has, &none) == TARGET_SCREEN_KEY_MATCH_NONE);

	FakeStore legacy_only{{old_hdmi}};
	CHECK(target_screen_key_pref_match(cur, old_hdmi, fake_has, &legacy_only) == TARGET_SCREEN_KEY_MATCH_LEGACY);

	FakeStore both{{old_hdmi, cur}};
	CHECK(target_screen_key_pref_match(cur, old_hdmi, fake_has, &both) == TARGET_SCREEN_KEY_MATCH_CURRENT);

	// No vendor serial: current == legacy, so it is a plain current match.
	FakeStore edid{{old_hdmi}};
	CHECK(target_screen_key_pref_match(old_hdmi, old_hdmi, fake_has, &edid) == TARGET_SCREEN_KEY_MATCH_CURRENT);
	CHECK(target_screen_key_pref_match(cur, "", fake_has, &legacy_only) == TARGET_SCREEN_KEY_MATCH_NONE);
	CHECK(target_screen_key_pref_match(cur, old_hdmi, nullptr, &legacy_only) == TARGET_SCREEN_KEY_MATCH_NONE);
}
