// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins where a plug-in without `probe_displays` gets its synthesized
 *         display claim (multi-screen M0, #69): the monitor its panel
 *         matches, not the primary monitor
 *         (`target_plugin_backcompat_claim_index`).
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_plugin.h"
#include "os/os_display_desktop.h"
#include "os/os_display_edid.h"
#include "target_plugin_loader.h"

#include <cstring>

namespace {

xrt_display_descriptor
desc(uint64_t id, int32_t left, uint32_t w, uint32_t h, bool primary)
{
	xrt_display_descriptor d = {};
	d.struct_size = sizeof(d);
	d.monitor_id = id;
	d.screen_left = left;
	d.pixel_width = w;
	d.pixel_height = h;
	d.flags = primary ? 1u : 0u;
	return d;
}

os_display_edid_monitor
mon(uint32_t native_w, uint32_t native_h, uint32_t mm_w, uint32_t mm_h, bool origin_unknown = false)
{
	os_display_edid_monitor m = {};
	m.native_width = native_w;
	m.native_height = native_h;
	m.physical_width_mm = mm_w;
	m.physical_height_mm = mm_h;
	m.origin_unknown = origin_unknown;
	return m;
}

//! The DS1 as the Leia plug-in reports it: 3840x2160, 0.344 x 0.193 m, no origin.
os_display_panel_hint
ds1_panel(int32_t left = 0, int32_t top = 0)
{
	os_display_panel_hint h = {};
	h.screen_left = left;
	h.screen_top = top;
	h.pixel_width = 3840;
	h.pixel_height = 2160;
	h.width_m = 0.344f;
	h.height_m = 0.193f;
	return h;
}

/*
 * ds1-linux: eDP-1 primary at (0,0), 3456x2160 X11 rect over a 2880x1800
 * device mode; the DS1 on HDMI at (3456,0), 3840x2160.
 */
struct Box
{
	xrt_display_descriptor d[2] = {
	    desc(0xA, 0, 3456, 2160, true),
	    desc(0xB, 3456, 3840, 2160, false),
	};
	os_display_edid_monitor m[2] = {
	    mon(2880, 1800, 300, 190),
	    mon(3840, 2160, 344, 193),
	};
	const os_display_edid_monitor *side[2] = {&m[0], &m[1]};
};

} // namespace

TEST_CASE("backcompat claim: no panel known -> the primary monitor")
{
	Box b;
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, nullptr) == 0);

	// No primary flagged at all -> descriptor 0.
	b.d[0].flags = 0;
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, nullptr, nullptr) == 0);

	// Primary second -> the second.
	b.d[1].flags = 1;
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, nullptr, nullptr) == 1);
}

#ifndef _WIN32
TEST_CASE("backcompat claim: the panel's monitor wins over the primary (connector mode)")
{
	Box b;
	const os_display_panel_hint p = ds1_panel();
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &p) == 1);
}

TEST_CASE("backcompat claim: pixel size alone places it when no device mode is known")
{
	Box b;
	const os_display_panel_hint p = ds1_panel();
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, nullptr, &p) == 1);
}

TEST_CASE("backcompat claim: a panel origin inside a known monitor is trusted first")
{
	Box b;
	// Both monitors 3840x2160, so a size match would be ambiguous; the
	// plug-in's own origin settles it.
	b.d[0] = desc(0xA, 0, 3840, 2160, true);
	b.m[0] = mon(3840, 2160, 344, 193);
	b.d[1].screen_left = 3840;
	const os_display_panel_hint p = ds1_panel(100, 50);
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &p) == 0);

	const os_display_panel_hint q = ds1_panel(4000, 10);
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &q) == 1);
}

TEST_CASE("backcompat claim: an origin is never matched against a DRM-only record")
{
	Box b;
	// No X server: both records sit at (0,0) with an unknown origin. A
	// plug-in origin of (10,10) must not pick monitor 0 by containment; the
	// connector-mode rule still finds the DS1.
	b.d[1].screen_left = 0;
	b.m[0].origin_unknown = true;
	b.m[1].origin_unknown = true;
	const os_display_panel_hint p = ds1_panel(10, 10);
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &p) == 1);
}

TEST_CASE("backcompat claim: a panel that matches nothing falls back to the primary")
{
	Box b;
	os_display_panel_hint p = ds1_panel();
	p.pixel_width = 1920;
	p.pixel_height = 1080;
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &p) == 0);

	// A panel with no pixel size is "not known".
	p.pixel_width = 0;
	p.pixel_height = 0;
	CHECK(target_plugin_backcompat_claim_index(b.d, 2, b.side, &p) == 0);
}
#endif
