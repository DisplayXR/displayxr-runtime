// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the 3D-panel selection rules in os_display_desktop_select.c
 *         (#1301, #1831) to the configurations measured on the DS1 box.
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_desktop.h"

#include <cstring>

namespace {

// The measured box: laptop eDP-1 2880x1800 next to a 3840x2160 3D panel on
// HDMI-1, GNOME 50 with the X screen scaled by the ceiling of the largest
// monitor scale.
constexpr uint32_t PANEL_W = 3840, PANEL_H = 2160;

os_display_desktop_info
mon(const char *name,
    int32_t left,
    uint32_t x11_w,
    uint32_t x11_h,
    uint32_t native_w,
    uint32_t native_h,
    bool primary,
    double scale = 0.0,
    uint32_t mm_w = 0,
    uint32_t mm_h = 0)
{
	os_display_desktop_info m = {};
	std::strncpy(m.device_name, name, sizeof(m.device_name) - 1);
	m.left = left;
	m.width = x11_w;
	m.height = x11_h;
	m.width_in_caller_dpi = x11_w;
	m.height_in_caller_dpi = x11_h;
	m.native_width = native_w;
	m.native_height = native_h;
	m.native_source = native_w > 0 ? OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR : OS_DISPLAY_NATIVE_SOURCE_NONE;
	m.scale = scale;
	m.is_primary = primary;
	m.physical_width_mm = mm_w;
	m.physical_height_mm = mm_h;
	return m;
}

os_display_panel_hint
panel_hint(uint32_t w = PANEL_W, uint32_t h = PANEL_H, float wm = 0.0f, float hm = 0.0f)
{
	os_display_panel_hint h_ = {};
	h_.pixel_width = w;
	h_.pixel_height = h;
	h_.width_m = wm;
	h_.height_m = hm;
	return h_;
}

} // namespace

TEST_CASE("panel select: #1831 repro — panel at 150 % is found by its connector, not missed by size")
{
	// laptop 166 %, panel 150 %: X screen at 2x. The panel reads 5120x2880 in
	// X11 and the laptop 3456x2160 — neither is the panel's 3840x2160, which
	// is what sent the old resolver to the primary fallback.
	const os_display_desktop_info mons[] = {
	    mon("eDP-1", 0, 3456, 2160, 2880, 1800, true, 1.6667),
	    mon("HDMI-1", 3456, 5120, 2880, PANEL_W, PANEL_H, false, 1.5),
	};
	const auto hint = panel_hint();
	os_display_panel_match match = {};
	const int32_t pick = os_display_desktop_select_by_size(mons, 2, &hint, &match);

	REQUIRE(pick == 1);
	CHECK(match.rule == OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE);
	CHECK(match.candidate_count == 1);
	CHECK(match.monitor_count == 2);
	// Identified, but NOT 1:1: an X11 window there is resampled.
	CHECK(os_display_desktop_info_is_1to1(&mons[1]) == 0);
}

TEST_CASE("panel select: panel at 200 % is found by its connector AND is 1:1")
{
	const os_display_desktop_info mons[] = {
	    mon("eDP-1", 0, 3456, 2160, 2880, 1800, true, 1.6667),
	    mon("HDMI-1", 3456, PANEL_W, PANEL_H, PANEL_W, PANEL_H, false, 2.0),
	};
	const auto hint = panel_hint();
	os_display_panel_match match = {};
	REQUIRE(os_display_desktop_select_by_size(mons, 2, &hint, &match) == 1);
	CHECK(match.rule == OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE);
	CHECK(os_display_desktop_info_is_1to1(&mons[1]) == 1);
}

TEST_CASE("panel select: a scaled monitor whose X11 rect happens to equal the panel size does not win")
{
	// A 4K laptop at 200 % reads 3840x2160 in X11 — exactly the panel size —
	// while the real panel sits at 150 % and reads 5120x2880. Size matching
	// alone would pick the laptop. The laptop's connector says 3840x2160 too,
	// so both are candidates by hardware mode; physical size breaks the tie.
	const os_display_desktop_info mons[] = {
	    mon("eDP-1", 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, true, 2.0, 344, 194),
	    mon("HDMI-1", 3840, 5120, 2880, PANEL_W, PANEL_H, false, 1.5, 340, 190),
	};
	const auto hint = panel_hint(PANEL_W, PANEL_H, 0.340f, 0.190f);
	os_display_panel_match match = {};
	REQUIRE(os_display_desktop_select_by_size(mons, 2, &hint, &match) == 1);
	CHECK(match.rule == OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE);
	CHECK(match.candidate_count == 2);
}

TEST_CASE("panel select: a monitor whose known device mode differs is never a size match")
{
	// 2880x1800 laptop whose X11 rect happens to read the panel size (some
	// scale combination), and no monitor runs the panel mode. The known
	// device mode says it is not the panel: fall through, do not guess.
	const os_display_desktop_info mons[] = {
	    mon("eDP-1", 0, PANEL_W, PANEL_H, 2880, 1800, true, 1.5),
	};
	const auto hint = panel_hint();
	os_display_panel_match match = {};
	CHECK(os_display_desktop_select_by_size(mons, 1, &hint, &match) == -1);
}

TEST_CASE("panel select: no device modes known — the old size rule, unchanged")
{
	// Windows / macOS / a Linux box with no compositor answer and no DRM:
	// native is 0 everywhere and the X11-size rule is the only evidence.
	const os_display_desktop_info mons[] = {
	    mon("DISPLAY1", 0, 2880, 1800, 0, 0, true),
	    mon("DISPLAY2", 2880, PANEL_W, PANEL_H, 0, 0, false),
	};
	const auto hint = panel_hint();
	os_display_panel_match match = {};
	REQUIRE(os_display_desktop_select_by_size(mons, 2, &hint, &match) == 1);
	CHECK(match.rule == OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH);
	CHECK(os_display_desktop_info_is_1to1(&mons[1]) == -1);
}

TEST_CASE("panel select: same-size tie without physical size prefers the non-primary monitor")
{
	const os_display_desktop_info mons[] = {
	    mon("DP-1", 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, true),
	    mon("HDMI-1", 3840, PANEL_W, PANEL_H, PANEL_W, PANEL_H, false),
	};
	const auto hint = panel_hint();
	os_display_panel_match match = {};
	REQUIRE(os_display_desktop_select_by_size(mons, 2, &hint, &match) == 1);
	CHECK(match.candidate_count == 2);
}

TEST_CASE("panel select: nothing matches — caller falls back")
{
	// sim_display's default 1920x1080 declaration on a box with no such
	// monitor: no rule fires, so the primary fallback (and today's
	// behaviour) applies.
	const os_display_desktop_info mons[] = {
	    mon("eDP-1", 0, 3456, 2160, 2880, 1800, true, 1.6667),
	    mon("HDMI-1", 3456, 5120, 2880, PANEL_W, PANEL_H, false, 1.5),
	};
	const auto hint = panel_hint(1920, 1080);
	os_display_panel_match match = {};
	CHECK(os_display_desktop_select_by_size(mons, 2, &hint, &match) == -1);
	CHECK(match.monitor_count == 2);
}

TEST_CASE("panel select: unknown panel size never selects")
{
	const os_display_desktop_info mons[] = {
	    mon("HDMI-1", 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, true),
	};
	const auto hint = panel_hint(0, 0);
	CHECK(os_display_desktop_select_by_size(mons, 1, &hint, nullptr) == -1);
	CHECK(os_display_desktop_select_by_size(mons, 1, nullptr, nullptr) == -1);
	CHECK(os_display_desktop_select_by_size(nullptr, 0, &hint, nullptr) == -1);
}

TEST_CASE("panel 1:1: the X screen at 2x over a panel at 100 % is resampled too")
{
	// Laptop 166 % forces the X screen to 2x; the panel at 100 % then reads
	// 7680x4320 — integer, but still not device pixels.
	const os_display_desktop_info m = mon("HDMI-1", 0, 7680, 4320, PANEL_W, PANEL_H, false, 1.0);
	CHECK(os_display_desktop_info_is_1to1(&m) == 0);
}

TEST_CASE("panel 1:1: rule names")
{
	CHECK(std::strcmp(os_display_desktop_rule_str(OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE), "connector match") == 0);
	CHECK(std::strcmp(os_display_desktop_rule_str(OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH), "size match") == 0);
}

/*
 * The claim rule (displayxr-leia-plugin#430): the field box was a Windows
 * laptop (primary 2560x1600 at 0,0) with a Lenovo 3D monitor on \\.\DISPLAY5 at
 * (-3840, 0), SR-VERIFIED by the plug-in's probe_displays, while its
 * get_display_info reported no origin — so the panel rect went to the laptop.
 */
namespace {

os_display_panel_claim
claim(int32_t left, uint32_t w, uint32_t h, bool verified, bool primary = false, int32_t top = 0)
{
	os_display_panel_claim c = {};
	c.left = left;
	c.top = top;
	c.width = w;
	c.height = h;
	c.verified = verified;
	c.is_primary = primary;
	return c;
}

} // namespace

TEST_CASE("claim select: #430 repro — no origin, one VERIFIED claim on a non-main monitor wins")
{
	const os_display_panel_claim claims[] = {claim(-3840, 3840, 2160, true)};
	const os_display_panel_hint h = panel_hint();
	bool overrides = true;
	CHECK(os_display_desktop_select_claim(claims, 1, &h, &overrides) == 0);
	CHECK_FALSE(overrides); // nothing was reported, so nothing is overridden
}

TEST_CASE("claim select: no origin, an EDID-level claim also wins (it beats the primary fallback)")
{
	const os_display_panel_claim claims[] = {claim(2560, 3840, 2160, false)};
	const os_display_panel_hint h = panel_hint();
	CHECK(os_display_desktop_select_claim(claims, 1, &h, nullptr) == 0);
}

TEST_CASE("claim select: no claims never fires")
{
	const os_display_panel_hint h = panel_hint();
	bool overrides = true;
	CHECK(os_display_desktop_select_claim(nullptr, 0, &h, &overrides) == -1);
	CHECK_FALSE(overrides);
}

TEST_CASE("claim select: an origin INSIDE a claimed monitor leaves the origin rule in charge")
{
	const os_display_panel_claim claims[] = {claim(-3840, 3840, 2160, true)};
	os_display_panel_hint h = panel_hint();
	h.screen_left = -3840;
	CHECK(os_display_desktop_select_claim(claims, 1, &h, nullptr) == -1);
	h.screen_left = -1; // last column of the panel
	h.screen_top = 2159;
	CHECK(os_display_desktop_select_claim(claims, 1, &h, nullptr) == -1);
}

TEST_CASE("claim select: an origin on no claimed monitor yields to a VERIFIED claim only")
{
	os_display_panel_hint h = panel_hint();
	h.screen_left = 2560; // a monitor the plug-in does not claim (stale / wrong DPI space)

	const os_display_panel_claim verified[] = {claim(-3840, 3840, 2160, true)};
	bool overrides = false;
	CHECK(os_display_desktop_select_claim(verified, 1, &h, &overrides) == 0);
	CHECK(overrides);

	// An EDID-table match is no stronger than the origin the same plug-in
	// computed: the origin rule keeps it.
	const os_display_panel_claim edid_only[] = {claim(-3840, 3840, 2160, false)};
	overrides = true;
	CHECK(os_display_desktop_select_claim(edid_only, 1, &h, &overrides) == -1);
	CHECK_FALSE(overrides);
}

TEST_CASE("claim select: several claims — verified, then the panel's size, then non-primary, then order")
{
	const os_display_panel_hint h = panel_hint();

	const os_display_panel_claim by_verified[] = {claim(0, 3840, 2160, false), claim(3840, 3840, 2160, true)};
	CHECK(os_display_desktop_select_claim(by_verified, 2, &h, nullptr) == 1);

	const os_display_panel_claim by_size[] = {claim(0, 2560, 1440, false), claim(2560, 3840, 2160, false)};
	CHECK(os_display_desktop_select_claim(by_size, 2, &h, nullptr) == 1);

	const os_display_panel_claim by_primary[] = {claim(0, 3840, 2160, false, true),
	                                             claim(3840, 3840, 2160, false, false)};
	CHECK(os_display_desktop_select_claim(by_primary, 2, &h, nullptr) == 1);

	const os_display_panel_claim by_order[] = {claim(0, 3840, 2160, true), claim(3840, 3840, 2160, true)};
	CHECK(os_display_desktop_select_claim(by_order, 2, &h, nullptr) == 0);
}

TEST_CASE("claim select: an overriding origin only ever lands on a VERIFIED claim")
{
	os_display_panel_hint h = panel_hint();
	h.screen_left = 9999;
	// The EDID-level claim is the panel's size; the verified one is not. A
	// contradicted origin is replaced by verified evidence, never by an
	// EDID-table match.
	const os_display_panel_claim claims[] = {claim(0, 3840, 2160, false), claim(3840, 2560, 1440, true)};
	bool overrides = false;
	CHECK(os_display_desktop_select_claim(claims, 2, &h, &overrides) == 1);
	CHECK(overrides);
}

TEST_CASE("claim rule: name")
{
	CHECK(std::strcmp(os_display_desktop_rule_str(OS_DISPLAY_DESKTOP_RULE_CLAIM), "plug-in claim") == 0);
}
